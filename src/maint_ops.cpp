// What the Maintenance window checks and does: Windows Update through its own COM API (wuapi), app
// updates through winget, cleanup of a fixed list of safe folders, the StartupApproved switches Task
// Manager uses, and Windows' own repair and optimize tools. Jobs that need administrator rights run in
// a second, elevated copy of Canivete (--maint-admin, see Maint_AdminMain), which reports back to the
// window with WM_COPYDATA.
#include "maint.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <oleauto.h>
#include <wuapi.h>
#include <srrestoreptapi.h>
#include <setupapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <future>
#include <set>

#include <initguid.h>
#include <devguid.h>
#include <devpkey.h>

using Microsoft::WRL::ComPtr;

namespace maint {
namespace {

struct Bstr {
    BSTR b = nullptr;
    Bstr() = default;
    explicit Bstr(const wchar_t* s) : b(SysAllocString(s)) {}
    ~Bstr() { SysFreeString(b); }
    Bstr(const Bstr&) = delete;
    Bstr& operator=(const Bstr&) = delete;
    BSTR* out() {
        SysFreeString(b);
        b = nullptr;
        return &b;
    }
    std::wstring str() const { return b ? std::wstring(b, SysStringLen(b)) : std::wstring(); }
};

struct ComInit {
    HRESULT hr;
    explicit ComInit(DWORD model = COINIT_MULTITHREADED) : hr(CoInitializeEx(nullptr, model)) {}
    ~ComInit() {
        if (SUCCEEDED(hr)) CoUninitialize();
    }
};

std::wstring Fmt(const wchar_t* f, ...) {
    wchar_t buf[1024];
    va_list a;
    va_start(a, f);
    _vsnwprintf_s(buf, _TRUNCATE, f, a);
    va_end(a);
    return buf;
}

std::wstring Expand(const wchar_t* s) {
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = ExpandEnvironmentStringsW(s, buf, static_cast<DWORD>(std::size(buf)));
    return n && n <= std::size(buf) ? buf : s;
}

std::wstring SysExe(const wchar_t* name) { return Expand(L"%SystemRoot%\\System32\\") + name; }

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

bool StartsWithI(const std::wstring& s, const wchar_t* p) { return _wcsnicmp(s.c_str(), p, wcslen(p)) == 0; }

bool KeyExists(HKEY root, const wchar_t* path) {
    HKEY k;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS) return false;
    RegCloseKey(k);
    return true;
}

HANDLE g_cancel = nullptr;

// What a job tells the window's progress bar. Percentages and steps are sent at most about ten times
// a second (the elevated copy sends each one as a message).
class Report {
public:
    explicit Report(const Sink& s) : sink_(s) {}
    const Sink& Out() const { return sink_; }
    // Item i of n (i = 0: a step before the items, like the restore point).
    void Step(int i, int n, const std::wstring& key, const std::wstring& what, bool force = true) {
        std::wstring s = std::to_wstring(i) + L"|" + std::to_wstring(n) + L"|" + key + L"|" + what;
        if (s == step_ || (!force && !Due(stepT_))) return;
        step_ = s;
        sink_(L"step", s);
    }
    // f = 0..1 of the whole job; below 0: no number known.
    void Pct(double f) {
        const int p = f < 0 ? -1 : static_cast<int>(std::clamp(f, 0.0, 1.0) * 1000 + 0.5);
        if (p == pct_ || (p >= 0 && pct_ >= 0 && p < 1000 && !Due(pctT_))) return;
        pct_ = p;
        sink_(L"pct", std::to_wstring(p));
    }
    void Freed(ULONGLONG b, bool force = false) {
        if (b == freed_ || (!force && !Due(freedT_))) return;
        freed_ = b;
        sink_(L"freed", std::to_wstring(b));
    }
    void End(const std::wstring& key, const wchar_t* how) { sink_(how, key); }  // "ok", "fail", "skip"

private:
    static bool Due(ULONGLONG& t) {
        const ULONGLONG now = GetTickCount64();
        if (now - t < 100) return false;
        t = now;
        return true;
    }
    const Sink& sink_;
    std::wstring step_;
    int pct_ = -2;
    ULONGLONG freed_ = ~0ULL, stepT_ = 0, pctT_ = 0, freedT_ = 0;
};

std::wstring PctText(double f) { return std::to_wstring(static_cast<int>(std::clamp(f, 0.0, 1.0) * 100)) + L"%"; }

// The last "45%" / "45.5 %" in a line of tool output, as 0..1; -1 if none.
double PercentIn(const std::wstring& l) {
    size_t p = l.rfind(L'%');
    if (p == std::wstring::npos) return -1;
    size_t e = p;
    while (e > 0 && l[e - 1] == L' ') --e;
    size_t s = e;
    while (s > 0 && (iswdigit(l[s - 1]) || l[s - 1] == L'.' || l[s - 1] == L',')) --s;
    if (s == e) return -1;
    std::wstring n = l.substr(s, e - s);
    std::replace(n.begin(), n.end(), L',', L'.');
    const double v = _wtof(n.c_str());
    return v >= 0 && v <= 100 ? v / 100 : -1;
}

// "12.5 MB / 25.1 MB" in winget's download bar, as 0..1; -1 if none.
double SizeFraction(const std::wstring& l) {
    const size_t m = l.find(L" / ");
    if (m == std::wstring::npos) return -1;
    auto mult = [](wchar_t u) {
        switch (towupper(u)) {
        case L'K': return 1e3;
        case L'M': return 1e6;
        case L'G': return 1e9;
        default: return 1.0;
        }
    };
    // Left: "<number> <unit>" ending just before " / ".
    size_t e = m;
    while (e > 0 && iswalpha(l[e - 1])) --e;
    const wchar_t lu = e < m ? l[e] : L'B';
    while (e > 0 && l[e - 1] == L' ') --e;
    size_t s = e;
    while (s > 0 && (iswdigit(l[s - 1]) || l[s - 1] == L'.')) --s;
    if (s == e) return -1;
    const double a = _wtof(l.substr(s, e - s).c_str()) * mult(lu);
    // Right: "<number> <unit>".
    size_t r = m + 3;
    while (r < l.size() && l[r] == L' ') ++r;
    size_t re = r;
    while (re < l.size() && (iswdigit(l[re]) || l[re] == L'.')) ++re;
    if (re == r) return -1;
    const double b0 = _wtof(l.substr(r, re - r).c_str());
    while (re < l.size() && l[re] == L' ') ++re;
    const double b = b0 * (re < l.size() ? mult(l[re]) : 1.0);
    return b > 0 ? std::clamp(a / b, 0.0, 1.0) : -1;
}

// Windows Update's asynchronous download and install call these; only "completed" matters (the
// progress is read with GetProgress while waiting).
class WuCallback final : public IDownloadProgressChangedCallback,
                         public IDownloadCompletedCallback,
                         public IInstallationProgressChangedCallback,
                         public IInstallationCompletedCallback {
public:
    WuCallback() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}
    HANDLE Done() const { return done_; }
    IUnknown* Progress() { return static_cast<IDownloadProgressChangedCallback*>(this); }
    IUnknown* InstProgress() { return static_cast<IInstallationProgressChangedCallback*>(this); }
    IUnknown* Completed() { return static_cast<IDownloadCompletedCallback*>(this); }
    IUnknown* InstCompleted() { return static_cast<IInstallationCompletedCallback*>(this); }

    STDMETHODIMP QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDownloadCompletedCallback))
            *out = static_cast<IDownloadCompletedCallback*>(this);
        else if (iid == __uuidof(IDownloadProgressChangedCallback))
            *out = static_cast<IDownloadProgressChangedCallback*>(this);
        else if (iid == __uuidof(IInstallationProgressChangedCallback))
            *out = static_cast<IInstallationProgressChangedCallback*>(this);
        else if (iid == __uuidof(IInstallationCompletedCallback))
            *out = static_cast<IInstallationCompletedCallback*>(this);
        else {
            *out = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG r = InterlockedDecrement(&ref_);
        if (!r) delete this;
        return static_cast<ULONG>(r);
    }
    STDMETHODIMP Invoke(IDownloadJob*, IDownloadProgressChangedCallbackArgs*) override { return S_OK; }
    STDMETHODIMP Invoke(IDownloadJob*, IDownloadCompletedCallbackArgs*) override {
        SetEvent(done_);
        return S_OK;
    }
    STDMETHODIMP Invoke(IInstallationJob*, IInstallationProgressChangedCallbackArgs*) override { return S_OK; }
    STDMETHODIMP Invoke(IInstallationJob*, IInstallationCompletedCallbackArgs*) override {
        SetEvent(done_);
        return S_OK;
    }

private:
    ~WuCallback() {
        if (done_) CloseHandle(done_);
    }
    LONG ref_ = 1;
    HANDLE done_;
};

// ---- running console tools ---------------------------------------------------------------

// Splits a tool's output into lines. Tools write the console code page (winget, DISM, defrag) or
// UTF-16 (sfc) when their output goes to a pipe; a lone '\r' ends a progress line that the next
// line replaces.
class LineReader {
public:
    using OnLine = std::function<void(const std::wstring& line, bool progress)>;
    explicit LineReader(OnLine on) : on_(std::move(on)) {}

    void Feed(const char* p, size_t n) {
        bytes_.append(p, n);
        if (wide_ < 0 && bytes_.size() >= 16) Detect();
        if (wide_ >= 0) Split(false);
    }
    void Finish() {
        if (wide_ < 0) Detect();
        Split(true);
    }
    const std::wstring& All() const { return all_; }

private:
    void Detect() {
        size_t zeros = 0, n = std::min<size_t>(bytes_.size(), 256);
        for (size_t i = 1; i < n; i += 2) zeros += bytes_[i] == 0;
        wide_ = n >= 4 && zeros * 4 >= n ? 1 : 0;  // about half the odd bytes are 0: UTF-16
    }
    static std::wstring Decode(const std::string& b) {
        if (b.empty()) return {};
        for (UINT cp : {static_cast<UINT>(CP_UTF8), static_cast<UINT>(CP_OEMCP)}) {
            const DWORD flags = cp == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
            const int n = MultiByteToWideChar(cp, flags, b.data(), static_cast<int>(b.size()), nullptr, 0);
            if (n <= 0) continue;
            std::wstring w(n, L'\0');
            MultiByteToWideChar(cp, flags, b.data(), static_cast<int>(b.size()), w.data(), n);
            return w;
        }
        return {};
    }
    void Emit(std::wstring line, bool progress) {
        // Spinner and bar characters some tools draw.
        while (!line.empty() && (line.back() == L'\r' || line.back() == L'\n')) line.pop_back();
        all_ += line;
        all_ += L'\n';
        if (on_) on_(line, progress);
    }
    void Split(bool end) {
        if (wide_ == 1) {
            const size_t nw = bytes_.size() / 2;
            text_.append(reinterpret_cast<const wchar_t*>(bytes_.data()), nw);
            bytes_.erase(0, nw * 2);
            size_t start = 0;
            for (size_t i = 0; i < text_.size(); ++i) {
                const wchar_t c = text_[i];
                if (c != L'\n' && c != L'\r') continue;
                if (c == L'\r' && i + 1 == text_.size() && !end) break;  // wait: maybe "\r\n"
                const bool progress = c == L'\r' && !(i + 1 < text_.size() && text_[i + 1] == L'\n');
                Emit(text_.substr(start, i - start), progress);
                if (c == L'\r' && !progress) ++i;
                start = i + 1;
            }
            text_.erase(0, start);
            if (end && !text_.empty()) Emit(text_, false), text_.clear();
            return;
        }
        size_t start = 0;
        for (size_t i = 0; i < bytes_.size(); ++i) {
            const char c = bytes_[i];
            if (c != '\n' && c != '\r') continue;
            if (c == '\r' && i + 1 == bytes_.size() && !end) break;
            const bool progress = c == '\r' && !(i + 1 < bytes_.size() && bytes_[i + 1] == '\n');
            Emit(Decode(bytes_.substr(start, i - start)), progress);
            if (c == '\r' && !progress) ++i;
            start = i + 1;
        }
        bytes_.erase(0, start);
        if (end && !bytes_.empty()) Emit(Decode(bytes_), false), bytes_.clear();
    }

    OnLine on_;
    std::string bytes_;
    std::wstring text_, all_;
    int wide_ = -1;
};

struct ProcResult {
    bool started = false;
    DWORD error = 0;     // why it didn't start
    DWORD code = 0;      // exit code
    bool timedOut = false;
    std::wstring out;    // all output, one line per '\n'
};

// Runs a console program hidden, with its output in a pipe. Waits for the program itself, not for
// the pipe to close: installers it starts may keep a copy of the pipe open long after.
ProcResult Run(const std::wstring& exe, const std::wstring& args, DWORD timeoutMs,
               const LineReader::OnLine& onLine = {}) {
    ProcResult r;
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) {
        r.error = GetLastError();
        return r;
    }
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    HANDLE nul = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    STARTUPINFOW si{sizeof(si)};
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdInput = nul;
    si.hStdOutput = si.hStdError = wr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = QuoteArg(exe) + L" " + args;
    r.started = CreateProcessW(exe.c_str(), cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                               nullptr, nullptr, &si, &pi) != 0;
    if (!r.started) r.error = GetLastError();
    CloseHandle(wr);
    if (nul != INVALID_HANDLE_VALUE) CloseHandle(nul);
    if (!r.started) {
        CloseHandle(rd);
        return r;
    }
    LineReader lr(onLine);
    const ULONGLONG t0 = GetTickCount64();
    char buf[8192];
    for (;;) {
        DWORD avail = 0;
        bool any = false;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            DWORD got = 0;
            if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || !got) break;
            lr.Feed(buf, got);
            any = true;
        }
        if (WaitForSingleObject(pi.hProcess, any ? 0 : 60) == WAIT_OBJECT_0) {
            while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
                DWORD got = 0;
                if (!ReadFile(rd, buf, std::min<DWORD>(avail, sizeof(buf)), &got, nullptr) || !got) break;
                lr.Feed(buf, got);
            }
            break;
        }
        if (timeoutMs && GetTickCount64() - t0 > timeoutMs) {
            TerminateProcess(pi.hProcess, 1);
            r.timedOut = true;
            WaitForSingleObject(pi.hProcess, 5000);
            break;
        }
    }
    lr.Finish();
    GetExitCodeProcess(pi.hProcess, &r.code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    r.out = lr.All();
    return r;
}

std::vector<std::wstring> Lines(const std::wstring& s) {
    std::vector<std::wstring> v;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == L'\n') {
            v.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    return v;
}

// The last line of a tool's output that reads like a message (no progress bars).
std::wstring LastMessage(const std::wstring& out) {
    const std::vector<std::wstring> lines = Lines(out);
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) {
        const std::wstring t = Trim(*it);
        if (t.size() < 4) continue;
        if (t.find(L'█') != std::wstring::npos || t.find(L'▒') != std::wstring::npos) continue;
        int letters = 0;
        for (wchar_t c : t) letters += iswalpha(c) != 0;
        if (letters >= 4) return t.size() > 160 ? t.substr(0, 160) + L"…" : t;
    }
    return {};
}

// ---- Windows Update ----------------------------------------------------------------------

std::wstring WuError(HRESULT hr) {
    switch (static_cast<unsigned>(hr)) {
    case 0x80240016: return L"Windows Update is busy installing something else. Try again later.";
    case 0x8024001E: return L"Windows Update is shutting down. Try again in a minute.";
    case 0x80072EE2: case 0x80072EFD: case 0x80072EE7: case 0x8024402C: case 0x80244022:
        return L"Couldn't reach Windows Update. Check the internet connection.";
    case 0x80070422: return L"The Windows Update service is turned off.";
    case 0x80070005: return L"Windows Update needs administrator rights for this.";
    }
    return Fmt(L"Windows Update error 0x%08X.", static_cast<unsigned>(hr));
}

HRESULT WuSearch(ComPtr<IUpdateSession>& session, ComPtr<IUpdateCollection>& found) {
    HRESULT hr = CoCreateInstance(CLSID_UpdateSession, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&session));
    if (FAILED(hr)) return hr;
    Bstr app(L"Canivete");
    session->put_ClientApplicationID(app.b);
    ComPtr<IUpdateSearcher> searcher;
    if (FAILED(hr = session->CreateUpdateSearcher(&searcher))) return hr;
    ComPtr<ISearchResult> result;
    Bstr q(L"IsInstalled=0 and IsHidden=0");
    if (FAILED(hr = searcher->Search(q.b, &result))) return hr;
    return result->get_Updates(&found);
}

std::wstring UpdateId(IUpdate* u) {
    ComPtr<IUpdateIdentity> id;
    Bstr s;
    if (SUCCEEDED(u->get_Identity(&id))) id->get_UpdateID(s.out());
    return s.str();
}

void ScanWindowsUpdate(UpdateScan& out) {
    ComInit com;
    ComPtr<IUpdateSession> session;
    ComPtr<IUpdateCollection> coll;
    const HRESULT hr = WuSearch(session, coll);
    if (FAILED(hr)) {
        out.wuError = WuError(hr);
        return;
    }
    LONG n = 0;
    coll->get_Count(&n);
    std::vector<UpdateItem> drivers;
    for (LONG i = 0; i < n; ++i) {
        ComPtr<IUpdate> u;
        if (FAILED(coll->get_Item(i, &u))) continue;
        UpdateItem it;
        Bstr title;
        u->get_Title(title.out());
        it.name = title.str();
        it.key = L"wu:" + UpdateId(u.Get());
        UpdateType type = utSoftware;
        u->get_Type(&type);
        VARIANT_BOOL browse = VARIANT_FALSE;
        ComPtr<IUpdate3> u3;
        if (SUCCEEDED(u.As(&u3))) u3->get_BrowseOnly(&browse);
        it.optional = browse == VARIANT_TRUE;
        DECIMAL d{};
        ULONG64 size = 0;
        if (SUCCEEDED(u->get_MaxDownloadSize(&d)) && SUCCEEDED(VarUI8FromDec(&d, &size))) it.size = size;
        if (it.name.find(L"Security Intelligence") != std::wstring::npos) it.size = 0;  // the full size, not the delta
        if (type == utDriver) {
            it.kind = L"Driver";
            ComPtr<IWindowsDriverUpdate> du;
            DATE dt = 0;
            SYSTEMTIME st;
            if (SUCCEEDED(u.As(&du)) && SUCCEEDED(du->get_DriverVerDate(&dt)) && dt > 0 && VariantTimeToSystemTime(dt, &st))
                it.version = Fmt(L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
            drivers.push_back(std::move(it));
        } else {
            it.kind = L"Windows";
            ComPtr<IStringCollection> kbs;
            LONG k = 0;
            if (SUCCEEDED(u->get_KBArticleIDs(&kbs)) && SUCCEEDED(kbs->get_Count(&k)) && k > 0) {
                Bstr s;
                kbs->get_Item(0, s.out());
                it.version = L"KB" + s.str();
            }
            out.items.push_back(std::move(it));
        }
    }
    for (auto& d : drivers) out.items.push_back(std::move(d));
}

// keyList: the Windows Update keys, items 1..n of the job's `items`. `part(f)`: f = 0..1 of this
// group of updates (downloads are the first half, installing the second).
int InstallWindowsUpdates(const std::vector<std::wstring>& keyList, int items, Report& rep,
                          const std::function<void(double)>& part) {
    const Sink& sink = rep.Out();
    const std::set<std::wstring> keys(keyList.begin(), keyList.end());
    auto itemNo = [&](const std::wstring& k) {
        return static_cast<int>(std::find(keyList.begin(), keyList.end(), k) - keyList.begin()) + 1;
    };
    auto failAll = [&](const std::set<std::wstring>& ks, const std::wstring& why) {
        for (const auto& k : ks) {
            sink(k, why);
            rep.End(k, L"fail");
        }
        return 1;
    };
    rep.Step(1, items, L"", L"Checking with Windows Update");
    rep.Pct(-1);
    ComPtr<IUpdateSession> session;
    ComPtr<IUpdateCollection> found;
    HRESULT hr = WuSearch(session, found);
    if (FAILED(hr)) return failAll(keys, WuError(hr));
    ComPtr<IUpdateCollection> want;
    if (FAILED(hr = CoCreateInstance(CLSID_UpdateCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&want))))
        return failAll(keys, WuError(hr));
    std::vector<std::wstring> order;  // keys in collection order
    std::set<std::wstring> missing = keys;
    LONG count = 0;
    found->get_Count(&count);
    for (LONG i = 0; i < count; ++i) {
        ComPtr<IUpdate> u;
        if (FAILED(found->get_Item(i, &u))) continue;
        const std::wstring key = L"wu:" + UpdateId(u.Get());
        if (!keys.count(key)) continue;
        missing.erase(key);
        VARIANT_BOOL eula = VARIANT_TRUE;
        if (SUCCEEDED(u->get_EulaAccepted(&eula)) && eula != VARIANT_TRUE) u->AcceptEula();
        LONG idx = 0;
        if (SUCCEEDED(want->Add(u.Get(), &idx))) {
            order.push_back(key);
            sink(key, L"Waiting to download…");
        }
    }
    for (const auto& k : missing) {
        sink(k, L"Not offered any more (already installed?)");
        rep.End(k, L"skip");
    }
    if (order.empty()) return 0;
    const double n = static_cast<double>(order.size());

    // Download, with the progress read while it runs; Cancel stops it (nothing is installed then).
    ComPtr<IUpdateDownloader> dl;
    ComPtr<IDownloadResult> dres;
    bool stopped = false;
    if (SUCCEEDED(hr = session->CreateUpdateDownloader(&dl)) && SUCCEEDED(hr = dl->put_Updates(want.Get()))) {
        auto* cb = new WuCallback;
        VARIANT state;
        VariantInit(&state);
        ComPtr<IDownloadJob> job;
        if (SUCCEEDED(dl->BeginDownload(cb->Progress(), cb->Completed(), state, &job)) && job) {
            for (;;) {
                WaitForSingleObject(cb->Done(), 200);
                VARIANT_BOOL completed = VARIANT_FALSE;
                job->get_IsCompleted(&completed);
                ComPtr<IDownloadProgress> p;
                LONG all = 0, idx = 0, cur = 0;
                if (SUCCEEDED(job->GetProgress(&p)) && p) {
                    p->get_PercentComplete(&all);
                    p->get_CurrentUpdateIndex(&idx);
                    p->get_CurrentUpdatePercentComplete(&cur);
                    if (idx >= 0 && idx < static_cast<LONG>(order.size())) {
                        const std::wstring& k = order[static_cast<size_t>(idx)];
                        sink(k, L"Downloading " + PctText(cur / 100.0));
                        rep.Step(itemNo(k), items, k, L"Downloading " + PctText(cur / 100.0), false);
                    }
                    part(0.5 * all / 100.0);
                }
                if (completed == VARIANT_TRUE) break;
                if (!stopped && CancelRequested()) {
                    job->RequestAbort();
                    stopped = true;
                    sink(L"status", L"Stopping the download…");
                }
            }
            hr = dl->EndDownload(job.Get(), &dres);
        } else {
            hr = dl->Download(&dres);  // no asynchronous download: wait for it without progress
        }
        cb->Release();
    }
    ComPtr<IUpdateCollection> ready;
    CoCreateInstance(CLSID_UpdateCollection, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&ready));
    std::vector<std::wstring> readyKeys;
    int failed = 0;
    for (LONG i = 0; i < static_cast<LONG>(order.size()); ++i) {
        const std::wstring& k = order[static_cast<size_t>(i)];
        ComPtr<IUpdate> u;
        VARIANT_BOOL got = VARIANT_FALSE;
        const bool have = SUCCEEDED(want->get_Item(i, &u)) && SUCCEEDED(u->get_IsDownloaded(&got)) && got == VARIANT_TRUE;
        if (stopped) {
            sink(k, have ? L"Downloaded, not installed (stopped)" : L"Stopped");
            rep.End(k, L"skip");
        } else if (have && ready) {
            LONG idx = 0;
            ready->Add(u.Get(), &idx);
            readyKeys.push_back(k);
            sink(k, L"Waiting to install…");
        } else {
            ++failed;
            sink(k, FAILED(hr) ? L"Download failed: " + WuError(hr) : L"Download failed");
            rep.End(k, L"fail");
        }
    }
    if (readyKeys.empty()) return failed ? 1 : 0;

    // Install. Not stopped halfway: Cancel then only skips what comes after.
    ComPtr<IUpdateInstaller> inst;
    ComPtr<IInstallationResult> ires;
    if (SUCCEEDED(hr = session->CreateUpdateInstaller(&inst))) {
        ComPtr<IUpdateInstaller2> i2;
        if (SUCCEEDED(inst.As(&i2))) i2->put_ForceQuiet(VARIANT_TRUE);
        inst->put_AllowSourcePrompts(VARIANT_FALSE);
        if (SUCCEEDED(hr = inst->put_Updates(ready.Get()))) {
            auto* cb = new WuCallback;
            VARIANT state;
            VariantInit(&state);
            ComPtr<IInstallationJob> job;
            if (SUCCEEDED(inst->BeginInstall(cb->InstProgress(), cb->InstCompleted(), state, &job)) && job) {
                for (;;) {
                    WaitForSingleObject(cb->Done(), 200);
                    VARIANT_BOOL completed = VARIANT_FALSE;
                    job->get_IsCompleted(&completed);
                    ComPtr<IInstallationProgress> p;
                    LONG all = 0, idx = 0, cur = 0;
                    if (SUCCEEDED(job->GetProgress(&p)) && p) {
                        p->get_PercentComplete(&all);
                        p->get_CurrentUpdateIndex(&idx);
                        p->get_CurrentUpdatePercentComplete(&cur);
                        if (idx >= 0 && idx < static_cast<LONG>(readyKeys.size())) {
                            const std::wstring& k = readyKeys[static_cast<size_t>(idx)];
                            sink(k, L"Installing " + PctText(cur / 100.0));
                            rep.Step(itemNo(k), items, k, L"Installing " + PctText(cur / 100.0), false);
                        }
                        const double got = (n - static_cast<double>(readyKeys.size())) + static_cast<double>(readyKeys.size()) * all / 100.0;
                        part(0.5 + 0.5 * got / n);
                    }
                    if (completed == VARIANT_TRUE) break;
                }
                hr = inst->EndInstall(job.Get(), &ires);
            } else {
                hr = inst->Install(&ires);
            }
            cb->Release();
        }
    }
    if (FAILED(hr) || !ires) {
        for (const auto& k : readyKeys) {
            sink(k, L"Install failed: " + WuError(hr));
            rep.End(k, L"fail");
        }
        return 1;
    }
    for (LONG i = 0; i < static_cast<LONG>(readyKeys.size()); ++i) {
        ComPtr<IUpdateInstallationResult> ur;
        OperationResultCode rc = orcFailed;
        VARIANT_BOOL reboot = VARIANT_FALSE;
        HRESULT uhr = S_OK;
        if (SUCCEEDED(ires->GetUpdateResult(i, &ur))) {
            ur->get_ResultCode(&rc);
            ur->get_RebootRequired(&reboot);
            ur->get_HResult(&uhr);
        }
        if (rc == orcSucceeded || rc == orcSucceededWithErrors) {
            sink(readyKeys[i], reboot == VARIANT_TRUE ? L"Installed: restart to finish" : L"Installed");
            rep.End(readyKeys[i], L"ok");
        } else {
            ++failed;
            sink(readyKeys[i], FAILED(uhr) ? L"Failed: " + WuError(uhr) : L"Failed");
            rep.End(readyKeys[i], L"fail");
        }
    }
    VARIANT_BOOL reboot = VARIANT_FALSE;
    if (SUCCEEDED(ires->get_RebootRequired(&reboot)) && reboot == VARIANT_TRUE) sink(L"reboot", L"");
    return failed ? 1 : 0;
}

// ---- winget ------------------------------------------------------------------------------

std::wstring WingetExe() { return Expand(L"%LOCALAPPDATA%\\Microsoft\\WindowsApps\\winget.exe"); }

bool IsDashLine(const std::wstring& l) {
    const std::wstring t = Trim(l);
    return t.size() >= 20 && t.find_first_not_of(L'-') == std::wstring::npos;
}

// The first table of `winget upgrade` (the one after it lists apps that need an explicit --id, like
// pinned ones, is left out). Columns are found from the header line above the dashes, in any language.
bool ParseWinget(const std::wstring& out, std::vector<UpdateItem>& items) {
    std::vector<std::wstring> lines = Lines(out);
    for (auto& l : lines) {
        const size_t cr = l.rfind(L'\r');  // spinner frames: keep what was drawn last
        if (cr != std::wstring::npos) l.erase(0, cr + 1);
    }
    size_t dash = 0;
    while (dash < lines.size() && !IsDashLine(lines[dash])) ++dash;
    if (dash == lines.size() || dash == 0) return false;
    const std::wstring& header = lines[dash - 1];
    std::vector<size_t> starts;
    for (size_t i = 0; i < header.size(); ++i)
        if (!iswspace(header[i]) && (i == 0 || iswspace(header[i - 1]))) starts.push_back(i);
    if (starts.size() != 5) {  // a header name of several words: columns are 2+ spaces apart
        starts.clear();
        for (size_t i = 0; i < header.size(); ++i)
            if (!iswspace(header[i]) && (i == 0 || (i >= 2 && iswspace(header[i - 1]) && iswspace(header[i - 2]))))
                starts.push_back(i);
    }
    if (starts.size() < 5) return true;
    for (size_t li = dash + 1; li < lines.size(); ++li) {
        const std::wstring& l = lines[li];
        if (l.size() <= starts[4]) break;
        std::wstring col[5];
        for (int c = 0; c < 5; ++c) {
            const size_t a = starts[c], b = c < 4 ? starts[c + 1] : l.size();
            col[c] = a < l.size() ? Trim(l.substr(a, b - a)) : L"";
        }
        bool srcOk = !col[4].empty();
        for (wchar_t ch : col[4]) srcOk = srcOk && (iswalnum(ch) || ch == L'.' || ch == L'-' || ch == L'_');
        if (!srcOk || !ValidWingetId(col[1])) break;
        UpdateItem it;
        it.key = L"app:" + col[1];
        it.name = col[0];
        it.kind = L"App";
        it.version = col[2] + L"  →  " + col[3];
        items.push_back(std::move(it));
    }
    return true;
}

void ScanApps(UpdateScan& out) {
    const std::wstring exe = WingetExe();
    if (GetFileAttributesW(exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        out.appError = L"winget (Windows' app installer) isn't installed, so apps weren't checked.";
        return;
    }
    // winget often answers nothing when another copy is busy: try three times.
    for (int t = 0; t < 3; ++t) {
        const ProcResult r = Run(exe, L"upgrade --accept-source-agreements --disable-interactivity", 300000);
        if (!r.started) {
            out.appError = Fmt(L"Couldn't start winget (error %lu).", r.error);
            return;
        }
        std::vector<UpdateItem> items;
        if (ParseWinget(r.out, items) || r.code == 0) {
            for (auto& i : items) out.items.push_back(std::move(i));
            return;
        }
        Sleep(3000 * (t + 1));
    }
    out.appError = L"winget didn't answer (tried 3 times). Try Check again in a minute.";
}

std::wstring WingetFailure(DWORD code) {
    switch (code) {
    case 0x8A15002B: return L"No newer version applies to this PC";
    case 0x8A150056: return L"The app is running: close it and try again";
    case 0x8A150101: case 0x8A150102: return L"The app is in use: close it and try again";
    case 0x8A15010D: return L"Another installation is running: try again later";
    case 0x8A150019: return L"Needs administrator rights";
    }
    return Fmt(L"Failed (code 0x%08X)", code);
}

// Item i of `items`. part(f): f = 0..1 of this app (download 0..0.8, install the rest), read from
// winget's progress bars ("12.5 MB / 25.1 MB", "45%").
int UpgradeApp(const std::wstring& key, int i, int items, Report& rep, const std::function<void(double)>& part) {
    const Sink& sink = rep.Out();
    const std::wstring id = key.substr(4);
    if (!ValidWingetId(id)) {
        rep.End(key, L"fail");
        return 1;
    }
    sink(key, L"Updating…");
    rep.Step(i, items, key, L"Updating");
    part(0);
    bool installing = false;
    const ProcResult r = Run(WingetExe(),
                             L"upgrade --id " + QuoteArg(id) +
                                 L" --exact --silent --accept-package-agreements --accept-source-agreements "
                                 L"--disable-interactivity",
                             3600000, [&](const std::wstring& line, bool) {
                                 double f = SizeFraction(line);
                                 if (f < 0) f = PercentIn(line);
                                 if (f < 0) return;
                                 if (!installing) {
                                     sink(key, L"Downloading " + PctText(f));
                                     rep.Step(i, items, key, L"Downloading " + PctText(f), false);
                                     part(0.8 * f);
                                     if (f >= 0.999) {
                                         installing = true;
                                         sink(key, L"Installing…");
                                         rep.Step(i, items, key, L"Installing");
                                     }
                                 } else {
                                     rep.Step(i, items, key, L"Installing " + PctText(f), false);
                                     part(0.8 + 0.2 * f);
                                 }
                             });
    part(1);
    if (!r.started) {
        sink(key, Fmt(L"Couldn't start winget (error %lu)", r.error));
        rep.End(key, L"fail");
        return 1;
    }
    if (r.code == 0) {
        sink(key, L"Updated");
        rep.End(key, L"ok");
        return 0;
    }
    if (r.code == 0x8A150077 || r.code == 0x8A15002B) {  // nothing to do (already the newest)
        sink(key, L"Already up to date");
        rep.End(key, L"ok");
        return 0;
    }
    std::wstring why = WingetFailure(r.code);
    const std::wstring msg = LastMessage(r.out);
    if (!msg.empty() && why.rfind(L"Failed (code", 0) == 0) why += L": " + msg;
    sink(key, why);
    rep.End(key, L"fail");
    return 1;
}

// ---- graphics drivers --------------------------------------------------------------------

std::wstring NvidiaVersion(const std::wstring& win) {  // 32.0.15.6094 -> 560.94
    std::vector<std::wstring> p;
    size_t s = 0;
    for (size_t i = 0; i <= win.size(); ++i)
        if (i == win.size() || win[i] == L'.') {
            p.push_back(win.substr(s, i - s));
            s = i + 1;
        }
    if (p.size() != 4) return {};
    std::wstring d = p[2] + (p[3].size() < 4 ? std::wstring(4 - p[3].size(), L'0') : L"") + p[3];
    if (d.size() < 5) return {};
    d = d.substr(d.size() - 5);
    return std::to_wstring(_wtoi(d.substr(0, 3).c_str())) + L"." + d.substr(3);
}

// ---- cleanup -----------------------------------------------------------------------------

struct Target {
    std::wstring dir;
    const wchar_t* pattern;  // file name pattern
    int days;                // only files older than this (0 = all)
    bool recursive;
};

struct CleanDef {
    const wchar_t* key;
    const wchar_t* name;
    const wchar_t* note;
    bool admin;
    bool byDefault;
};
const CleanDef kClean[] = {
    {L"usertemp", L"Your temporary files", L"Older than a week, in your Temp folder", false, true},
    {L"wintemp", L"Windows temporary files", L"Older than a week, in Windows\\Temp", true, true},
    {L"wudownload", L"Windows Update downloads", L"Update files Windows downloaded more than a week ago", true, true},
    {L"delivery", L"Delivery Optimization cache", L"Update pieces Windows keeps to share with other PCs", true, true},
    {L"crash", L"Crash reports and memory dumps", L"Older than a week (recent ones are kept for troubleshooting)",
     true, true},
    {L"shader", L"Graphics shader caches",
     L"DirectX and graphics driver caches. Apps rebuild them, so their next start is slower", false, false},
    {L"recycle", L"Recycle Bin", L"Files you deleted. They can't be brought back afterwards", false, false},
    {L"winsxs", L"Old Windows update files", L"Windows' own cleanup of replaced system files. Takes several minutes",
     true, false},
};

std::vector<Target> Targets(const std::wstring& key) {
    if (key == L"usertemp") {
        wchar_t t[MAX_PATH + 1];
        const DWORD n = GetTempPathW(MAX_PATH + 1, t);
        std::wstring d(t, n);
        while (!d.empty() && d.back() == L'\\') d.pop_back();
        return {{d, L"*", 7, true}};
    }
    if (key == L"wintemp") return {{Expand(L"%SystemRoot%\\Temp"), L"*", 7, true}};
    if (key == L"wudownload") return {{Expand(L"%SystemRoot%\\SoftwareDistribution\\Download"), L"*", 7, true}};
    if (key == L"delivery")
        return {{Expand(L"%SystemRoot%\\ServiceProfiles\\NetworkService\\AppData\\Local\\Microsoft\\Windows\\"
                        L"DeliveryOptimization\\Cache"),
                 L"*", 0, true}};
    if (key == L"crash")
        return {{Expand(L"%LOCALAPPDATA%\\CrashDumps"), L"*", 7, true},
                {Expand(L"%LOCALAPPDATA%\\Microsoft\\Windows\\WER"), L"*", 7, true},
                {Expand(L"%ProgramData%\\Microsoft\\Windows\\WER\\ReportArchive"), L"*", 7, true},
                {Expand(L"%ProgramData%\\Microsoft\\Windows\\WER\\ReportQueue"), L"*", 7, true},
                {Expand(L"%SystemRoot%\\Minidump"), L"*", 7, true},
                {Expand(L"%SystemRoot%\\LiveKernelReports"), L"*.dmp", 7, true},
                {Expand(L"%SystemRoot%"), L"MEMORY.DMP", 7, false}};
    if (key == L"shader")
        return {{Expand(L"%LOCALAPPDATA%\\D3DSCache"), L"*", 0, true},
                {Expand(L"%LOCALAPPDATA%\\NVIDIA\\DXCache"), L"*", 0, true},
                {Expand(L"%LOCALAPPDATA%\\NVIDIA\\GLCache"), L"*", 0, true},
                {Expand(L"%LOCALAPPDATA%\\AMD\\DxCache"), L"*", 0, true},
                {Expand(L"%LOCALAPPDATA%\\AMD\\GLCache"), L"*", 0, true}};
    return {};
}

struct WalkStats {
    ULONGLONG bytes = 0;
    ULONGLONG files = 0;
    ULONGLONG kept = 0;  // in use or not allowed
    bool denied = false; // the folder itself could not be read
    const std::function<void(ULONGLONG bytes)>* tick = nullptr;  // now and then while deleting
};

ULONGLONG FileTime64(const FILETIME& f) { return (static_cast<ULONGLONG>(f.dwHighDateTime) << 32) | f.dwLowDateTime; }

// Measures (del = false) or deletes the matching files. Never follows links or junctions, never
// deletes the folder it starts from, skips files in use.
void Walk(const std::wstring& dir, const Target& t, ULONGLONG cutoff, bool del, bool top, WalkStats& s) {
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) {
        const DWORD e = GetLastError();
        if (top && e == ERROR_ACCESS_DENIED) s.denied = true;
        return;
    }
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        // Claude Code's working folders in Temp stay.
        if (top && t.days && _wcsnicmp(name.c_str(), L"claude", 6) == 0) continue;
        const std::wstring path = dir + L"\\" + name;
        const ULONGLONG age = std::max(FileTime64(fd.ftLastWriteTime), FileTime64(fd.ftCreationTime));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (!t.recursive) continue;
            Walk(path, t, cutoff, del, false, s);
            if (del && wcscmp(t.pattern, L"*") == 0 && (!t.days || age < cutoff)) RemoveDirectoryW(path.c_str());
            continue;
        }
        if (!PathMatchSpecW(name.c_str(), t.pattern)) continue;
        if (t.days && age >= cutoff) continue;
        const ULONGLONG size = (static_cast<ULONGLONG>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        if (!del) {
            s.bytes += size;
            ++s.files;
            continue;
        }
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_READONLY)
            SetFileAttributesW(path.c_str(), fd.dwFileAttributes & ~FILE_ATTRIBUTE_READONLY);
        if (DeleteFileW(path.c_str())) {
            s.bytes += size;
            if (++s.files % 64 == 0 && s.tick) (*s.tick)(s.bytes);
        } else {
            ++s.kept;
        }
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

WalkStats WalkAll(const std::wstring& key, bool del, const std::function<void(ULONGLONG)>* tick = nullptr) {
    WalkStats s;
    s.tick = tick;
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    for (const Target& t : Targets(key)) {
        if (GetFileAttributesW(t.dir.c_str()) == INVALID_FILE_ATTRIBUTES) {
            if (GetLastError() == ERROR_ACCESS_DENIED) s.denied = true;
            continue;
        }
        const ULONGLONG cutoff = FileTime64(now) - static_cast<ULONGLONG>(t.days) * 24ULL * 3600ULL * 10000000ULL;
        Walk(t.dir, t, cutoff, del, true, s);
    }
    return s;
}

// freed: bytes freed by the whole job so far (this place's are added). part(f): f = 0..1 of this place.
int Clean(const std::wstring& key, Report& rep, ULONGLONG& freed, const std::function<void(double)>& part) {
    const Sink& sink = rep.Out();
    const bool admin = IsElevated();
    if (key == L"recycle") {
        SHQUERYRBINFO rb{sizeof(rb)};
        SHQueryRecycleBinW(nullptr, &rb);
        sink(key, L"Emptying…");
        const HRESULT hr = SHEmptyRecycleBinW(nullptr, nullptr, SHERB_NOCONFIRMATION | SHERB_NOPROGRESSUI | SHERB_NOSOUND);
        // E_UNEXPECTED: it was already empty.
        const bool ok = SUCCEEDED(hr) || hr == E_UNEXPECTED;
        if (ok) freed += static_cast<ULONGLONG>(rb.i64Size);
        sink(key, ok ? L"Freed " + SizeText(rb.i64Size) : Fmt(L"Failed (0x%08X)", hr));
        rep.End(key, ok ? L"ok" : L"fail");
        return ok ? 0 : 1;
    }
    if (key == L"winsxs") {
        if (!admin) {
            sink(key, L"Needs administrator rights");
            rep.End(key, L"fail");
            return 1;
        }
        sink(key, L"Cleaning (several minutes)…");
        const ProcResult r = Run(SysExe(L"Dism.exe"), L"/Online /Cleanup-Image /StartComponentCleanup /NoRestart", 0,
                                 [&](const std::wstring& l, bool progress) {
                                     const std::wstring t = Trim(l);
                                     const double f = PercentIn(t);
                                     if (f >= 0) part(f);
                                     if (progress && !t.empty()) sink(key, f >= 0 ? L"Cleaning… " + PctText(f) : L"Cleaning… " + t);
                                 });
        const bool ok = r.started && r.code == 0;
        sink(key, ok ? L"Done" : Fmt(L"Failed (DISM code %lu)", r.code));
        rep.End(key, ok ? L"ok" : L"fail");
        return ok ? 0 : 1;
    }
    sink(key, L"Cleaning…");
    const ULONGLONG base = freed;
    const std::function<void(ULONGLONG)> tick = [&](ULONGLONG b) { rep.Freed(base + b); };
    const WalkStats before = key == L"delivery" ? WalkAll(key, false) : WalkStats{};
    WalkStats s;
    if (key == L"delivery") {
        // The supported way: the service lets go of the pieces it is still sharing.
        if (admin)
            Run(SysExe(L"WindowsPowerShell\\v1.0\\powershell.exe"),
                L"-NoProfile -NonInteractive -Command \"Delete-DeliveryOptimizationCache -Force\"", 600000);
        const WalkStats after = WalkAll(key, false);
        s.bytes = before.bytes > after.bytes ? before.bytes - after.bytes : 0;
        s.denied = before.denied && after.denied;
    } else {
        s = WalkAll(key, true, &tick);
    }
    freed = base + s.bytes;
    std::wstring t = L"Freed " + SizeText(s.bytes);
    if (s.kept) t += Fmt(L" (%llu files in use were kept)", s.kept);
    if (s.denied && !admin) t += L" (only what it was allowed to)";
    sink(key, t);
    rep.End(key, L"ok");
    return 0;
}

// ---- startup apps ------------------------------------------------------------------------

constexpr wchar_t kApproved[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\";

// value name (lower case) -> on, from a StartupApproved key. Odd first byte = turned off.
std::vector<std::pair<std::wstring, bool>> Approved(HKEY root, const wchar_t* sub) {
    std::vector<std::pair<std::wstring, bool>> v;
    HKEY k;
    if (RegOpenKeyExW(root, (std::wstring(kApproved) + sub).c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &k) != ERROR_SUCCESS)
        return v;
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD nl = static_cast<DWORD>(std::size(name)), type = 0, size = 64;
        BYTE data[64];
        const LSTATUS r = RegEnumValueW(k, i, name, &nl, nullptr, &type, data, &size);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || type != REG_BINARY || size < 1) continue;
        v.emplace_back(name, !(data[0] & 1));
    }
    RegCloseKey(k);
    return v;
}

bool LookupOn(const std::vector<std::pair<std::wstring, bool>>& v, const std::wstring& name) {
    for (const auto& [n, on] : v)
        if (!_wcsicmp(n.c_str(), name.c_str())) return on;
    return true;
}

bool UnderWindows(const std::wstring& cmd) {
    std::wstring c = Expand(cmd.c_str());
    if (!c.empty() && c[0] == L'"') c.erase(0, 1);
    const std::wstring win = Expand(L"%SystemRoot%\\");
    return _wcsnicmp(c.c_str(), win.c_str(), win.size()) == 0;
}

bool IsClaude(const std::wstring& s) {
    std::wstring l = s;
    for (auto& c : l) c = static_cast<wchar_t>(towlower(c));
    return l.find(L"claude") != std::wstring::npos || l.find(L"anthropic") != std::wstring::npos ||
           l.find(L"canivete") != std::wstring::npos;  // Canivete itself: Settings has its own switch
}

void RunKey(std::vector<StartupItem>& out, HKEY root, const wchar_t* path, REGSAM view, const wchar_t* sub, bool machine) {
    HKEY k;
    if (RegOpenKeyExW(root, path, 0, KEY_READ | view, &k) != ERROR_SUCCESS) return;
    const auto approved = Approved(root, sub);
    for (DWORD i = 0;; ++i) {
        wchar_t name[512];
        DWORD nl = static_cast<DWORD>(std::size(name)), type = 0, size = 0;
        LSTATUS r = RegEnumValueW(k, i, name, &nl, nullptr, &type, nullptr, &size);
        if (r == ERROR_NO_MORE_ITEMS) break;
        if (r != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ) || !nl) continue;
        std::wstring data(size / sizeof(wchar_t) + 1, L'\0');
        nl = static_cast<DWORD>(std::size(name));
        DWORD sz = static_cast<DWORD>(data.size() * sizeof(wchar_t));
        if (RegEnumValueW(k, i, name, &nl, nullptr, &type, reinterpret_cast<BYTE*>(data.data()), &sz) != ERROR_SUCCESS)
            continue;
        data.resize(wcsnlen(data.c_str(), data.size()));
        if (UnderWindows(data) || IsClaude(std::wstring(name) + L" " + data)) continue;  // Windows' own, and Claude's
        StartupItem it;
        it.key = std::wstring(machine ? L"M|" : L"U|") + sub + L"|" + name;
        it.name = name;
        it.command = Expand(data.c_str());
        it.where = machine ? L"All users" : L"You";
        it.on = LookupOn(approved, name);
        it.admin = machine;
        out.push_back(std::move(it));
    }
    RegCloseKey(k);
}

void StartupFolder(std::vector<StartupItem>& out, REFKNOWNFOLDERID id, bool machine) {
    PWSTR dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &dir))) return;
    const std::wstring d = dir;
    CoTaskMemFree(dir);
    const auto approved = Approved(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, L"StartupFolder");
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileW((d + L"\\*").c_str(), &fd);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        if (!_wcsicmp(name.c_str(), L"desktop.ini")) continue;
        std::wstring target = d + L"\\" + name;
        if (!_wcsicmp(PathFindExtensionW(name.c_str()), L".lnk")) {
            ComPtr<IShellLinkW> sl;
            ComPtr<IPersistFile> pf;
            wchar_t path[MAX_PATH * 2] = L"", args[1024] = L"";
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) &&
                SUCCEEDED(sl.As(&pf)) && SUCCEEDED(pf->Load((d + L"\\" + name).c_str(), STGM_READ))) {
                sl->GetPath(path, static_cast<int>(std::size(path)), nullptr, SLGP_RAWPATH);
                sl->GetArguments(args, static_cast<int>(std::size(args)));
                target = Expand(path);
                if (*args) target = L"\"" + target + L"\" " + args;
            }
        }
        if (IsClaude(name + L" " + target)) continue;
        StartupItem it;
        it.key = std::wstring(machine ? L"M|" : L"U|") + L"StartupFolder|" + name;
        it.name = name;
        if (!_wcsicmp(PathFindExtensionW(it.name.c_str()), L".lnk")) it.name.resize(it.name.size() - 4);
        it.command = target;
        it.where = machine ? L"All users (Startup folder)" : L"You (Startup folder)";
        it.on = LookupOn(approved, name);
        it.admin = machine;
        out.push_back(std::move(it));
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

int SetStartup(const std::wstring& arg, const Sink& sink) {
    // "<0|1>|<U|M>|<Run|Run32|StartupFolder>|<name>"
    if (arg.size() < 7 || arg[1] != L'|' || arg[3] != L'|') return 1;
    const bool on = arg[0] == L'1';
    const bool machine = arg[2] == L'M';
    const size_t bar = arg.find(L'|', 4);
    if (bar == std::wstring::npos) return 1;
    const std::wstring sub = arg.substr(4, bar - 4), name = arg.substr(bar + 1);
    const std::wstring key = arg.substr(2);
    if (sub != L"Run" && sub != L"Run32" && sub != L"StartupFolder") return 1;
    BYTE data[12]{};
    data[0] = on ? 2 : 3;
    if (!on) {  // turned off: the time, as Task Manager writes it
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        memcpy(data + 4, &ft, sizeof(ft));
    }
    HKEY k;
    LSTATUS r = RegCreateKeyExW(machine ? HKEY_LOCAL_MACHINE : HKEY_CURRENT_USER, (std::wstring(kApproved) + sub).c_str(), 0,
                                nullptr, 0, KEY_SET_VALUE | KEY_WOW64_64KEY, nullptr, &k, nullptr);
    if (r == ERROR_SUCCESS) {
        r = RegSetValueExW(k, name.c_str(), 0, REG_BINARY, data, sizeof(data));
        RegCloseKey(k);
    }
    sink(key, r == ERROR_SUCCESS ? (on ? L"Turned on" : L"Turned off")
                                 : r == ERROR_ACCESS_DENIED ? L"Needs administrator rights" : Fmt(L"Failed (%ld)", r));
    sink(r == ERROR_SUCCESS ? L"ok" : L"fail", key);
    return r == ERROR_SUCCESS ? 0 : 1;
}

// ---- restore point, repair, optimize -----------------------------------------------------

int MakeRestorePoint(const Sink& sink, const wchar_t* what) {
    using SetFn = BOOL(WINAPI*)(PRESTOREPOINTINFOW, PSTATEMGRSTATUS);
    HMODULE dll = LoadLibraryExW(L"srclient.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    auto set = dll ? reinterpret_cast<SetFn>(GetProcAddress(dll, "SRSetRestorePointW")) : nullptr;
    if (!set) {
        sink(L"status", L"System Restore isn't available on this PC.");
        return 1;
    }
    sink(L"status", L"Making a restore point…");
    // Windows makes at most one a day unless this is 0 (minutes); set it for this one only.
    constexpr wchar_t kSr[] = L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\SystemRestore";
    HKEY k = nullptr;
    DWORD old = 0, oldSize = sizeof(old);
    bool hadOld = false;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kSr, 0, KEY_QUERY_VALUE | KEY_SET_VALUE | KEY_WOW64_64KEY, &k) == ERROR_SUCCESS) {
        hadOld = RegQueryValueExW(k, L"SystemRestorePointCreationFrequency", nullptr, nullptr, reinterpret_cast<BYTE*>(&old),
                                  &oldSize) == ERROR_SUCCESS;
        const DWORD zero = 0;
        RegSetValueExW(k, L"SystemRestorePointCreationFrequency", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&zero),
                       sizeof(zero));
    }
    RESTOREPOINTINFOW ri{};
    ri.dwEventType = BEGIN_SYSTEM_CHANGE;
    ri.dwRestorePtType = MODIFY_SETTINGS;
    wcsncpy_s(ri.szDescription, what, _TRUNCATE);
    STATEMGRSTATUS st{};
    const BOOL ok = set(&ri, &st);
    if (ok) {
        ri.dwEventType = END_SYSTEM_CHANGE;
        ri.llSequenceNumber = st.llSequenceNumber;
        STATEMGRSTATUS st2{};
        set(&ri, &st2);
    }
    if (k) {
        if (hadOld)
            RegSetValueExW(k, L"SystemRestorePointCreationFrequency", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&old),
                           sizeof(old));
        else
            RegDeleteValueW(k, L"SystemRestorePointCreationFrequency");
        RegCloseKey(k);
    }
    if (ok) {
        sink(L"status", L"Restore point made.");
        sink(L"log", L"Restore point made: " + std::wstring(what));
        return 0;
    }
    std::wstring why;
    switch (st.nStatus) {
    case ERROR_SERVICE_DISABLED: why = L"System Protection is off for the system drive, so Windows can't make one."; break;
    case ERROR_ACCESS_DENIED: why = L"Needs administrator rights."; break;
    case ERROR_DISK_FULL: why = L"Not enough free disk space."; break;
    default: why = Fmt(L"Windows said error %lu.", st.nStatus);
    }
    sink(L"status", L"No restore point: " + why);
    sink(L"log", L"No restore point: " + why);
    return 1;
}

// part(f): the tool's own percentage, when its output shows one.
int RunTool(const std::wstring& title, const std::wstring& exe, const std::wstring& args, const Sink& sink,
            const std::function<void(double)>& part = {}) {
    sink(L"log", L"");
    sink(L"log", L"▶ " + title);
    const ProcResult r = Run(exe, args, 0, [&](const std::wstring& l, bool progress) {
        const std::wstring t = Trim(l);
        if (!t.empty()) sink(progress ? L"logp" : L"log", t);
        if (part) {
            const double f = PercentIn(t);
            if (f >= 0) part(f);
        }
    });
    if (!r.started) {
        sink(L"log", Fmt(L"Couldn't start it (error %lu).", r.error));
        return 1;
    }
    sink(L"log", Fmt(L"Finished (code %lu).", r.code));
    return static_cast<int>(r.code);
}

// ---- test copies: simulated checks and jobs (CANIVETE_MAINT_FAKE=1) ------------------------

ULONGLONG FakeSize(const std::wstring& key) {
    if (key == L"usertemp") return 412ULL << 20;
    if (key == L"wintemp") return 38ULL << 20;
    if (key == L"wudownload") return 1260ULL << 20;
    if (key == L"delivery") return 905ULL << 20;
    if (key == L"crash") return 2280ULL << 20;
    if (key == L"shader") return 640ULL << 20;
    if (key == L"recycle") return 150ULL << 20;
    return 0;
}

// Same messages as the real jobs, with pauses instead of work. An app whose id has "Fail" in it fails.
int FakeTask(const std::wstring& task, const std::vector<std::wstring>& args, const Sink& sink) {
    Report rep(sink);
    int failed = 0;
    auto ticks = [&](int n, int ms, const std::function<void(double)>& at) {
        for (int k = 1; k <= n; ++k) {
            Sleep(static_cast<DWORD>(ms));
            at(static_cast<double>(k) / n);
        }
    };
    if (task == L"install") {
        std::vector<std::wstring> items;
        for (const auto& a : args)
            if (a != L"restore") items.push_back(a);
        const bool restore = items.size() != args.size();
        const int n = static_cast<int>(items.size());
        const double units = n + (restore ? 1 : 0);
        double done = 0;
        if (restore) {
            rep.Step(0, n, L"restore", L"Making a restore point");
            rep.Pct(-1);
            Sleep(1500);
            sink(L"status", L"Restore point made (test copy: not really).");
            rep.End(L"restore", L"ok");
            done = 1;
        }
        for (int i = 0; i < n; ++i) {
            const std::wstring& k = items[static_cast<size_t>(i)];
            if (CancelRequested()) {
                sink(k, L"Not started (stopped)");
                rep.End(k, L"skip");
                continue;
            }
            ticks(12, 150, [&](double f) {
                sink(k, L"Downloading " + PctText(f));
                rep.Step(i + 1, n, k, L"Downloading " + PctText(f), false);
                rep.Pct((done + 0.7 * f) / units);
            });
            ticks(6, 150, [&](double f) {
                sink(k, L"Installing " + PctText(f));
                rep.Step(i + 1, n, k, L"Installing " + PctText(f), false);
                rep.Pct((done + 0.7 + 0.3 * f) / units);
            });
            done += 1;
            if (k.find(L"Fail") != std::wstring::npos) {
                ++failed;
                sink(k, L"The app is running: close it and try again");
                rep.End(k, L"fail");
            } else {
                sink(k, k.rfind(L"wu:", 0) == 0 ? L"Installed (test copy: not really)" : L"Updated (test copy: not really)");
                rep.End(k, L"ok");
            }
        }
    } else if (task == L"clean") {
        const int n = static_cast<int>(args.size());
        ULONGLONG freed = 0;
        for (int i = 0; i < n; ++i) {
            const std::wstring& k = args[static_cast<size_t>(i)];
            if (CancelRequested()) {
                sink(k, L"Not cleaned (stopped)");
                rep.End(k, L"skip");
                continue;
            }
            rep.Step(i + 1, n, k, L"Cleaning");
            sink(k, L"Cleaning…");
            const ULONGLONG size = FakeSize(k), base = freed;
            ticks(8, 180, [&](double f) {
                rep.Freed(base + static_cast<ULONGLONG>(static_cast<double>(size) * f));
                rep.Pct((i + f) / n);
                if (k == L"winsxs") {
                    sink(k, L"Cleaning… " + PctText(f));
                    rep.Step(i + 1, n, k, L"Cleaning " + PctText(f), false);
                }
            });
            freed = base + size;
            rep.Freed(freed, true);
            sink(k, k == L"winsxs" ? L"Done (test copy: nothing deleted)" : L"Freed " + SizeText(size) + L" (test copy: nothing deleted)");
            rep.End(k, L"ok");
        }
    } else if (task == L"startup") {
        const int n = static_cast<int>(args.size());
        for (int i = 0; i < n; ++i) {
            const std::wstring& a = args[static_cast<size_t>(i)];
            const std::wstring k = a.size() > 2 ? a.substr(2) : a;
            rep.Step(i + 1, n, k, L"Changing");
            Sleep(300);
            rep.Pct((i + 1.0) / n);
            sink(k, a[0] == L'1' ? L"Turned on (test copy: not really)" : L"Turned off (test copy: not really)");
            rep.End(k, L"ok");
        }
    } else if (task == L"repair" || task == L"optimize" || task == L"scan" || task == L"restore") {
        const int steps = task == L"repair" ? 2 : 1;
        for (int s = 0; s < steps; ++s) {
            if (s && CancelRequested()) {
                sink(L"log", L"Stopped before the next step.");
                rep.End(L"sfc", L"skip");
                break;
            }
            const std::wstring key = task == L"repair" ? (s ? L"sfc" : L"dism") : task;
            rep.Step(steps > 1 ? s + 1 : 0, steps, key, task == L"repair" ? (s ? L"Checking system files" : L"Repairing the Windows image")
                                                       : task == L"optimize" ? L"Optimizing drives"
                                                       : task == L"scan" ? L"Quick virus scan" : L"Making a restore point");
            sink(L"log", L"▶ " + task + L" (test copy: simulated)");
            const bool known = task != L"scan" && task != L"restore";
            if (!known) rep.Pct(-1);
            ticks(10, 250, [&](double f) {
                sink(L"logp", L"Progress " + PctText(f));
                if (known) rep.Pct((s + f) / steps);
            });
            sink(L"log", L"Finished (code 0).");
            rep.End(key, L"ok");
        }
    } else {
        return 2;
    }
    sink(L"status", failed ? L"Finished, with problems (see the list)." : L"Finished.");
    return failed ? 1 : 0;
}

}  // namespace

// ---- public ------------------------------------------------------------------------------

std::wstring CancelEventName(DWORD windowPid) { return L"Local\\Canivete.MaintCancel." + std::to_wstring(windowPid); }
void SetCancelEvent(HANDLE event) { g_cancel = event; }
bool CancelRequested() { return g_cancel && WaitForSingleObject(g_cancel, 0) == WAIT_OBJECT_0; }

bool FakeMode() {
    static const bool fake = [] {
        wchar_t v[8] = L"";
        GetEnvironmentVariableW(L"CANIVETE_MAINT_FAKE", v, 8);
        return TestMode() && v[0] == L'1';
    }();
    return fake;
}

bool IsElevated() {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &t)) return false;
    TOKEN_ELEVATION e{};
    DWORD n = 0;
    const bool ok = GetTokenInformation(t, TokenElevation, &e, sizeof(e), &n) && e.TokenIsElevated;
    CloseHandle(t);
    return ok;
}

bool ValidWingetId(const std::wstring& id) {
    if (id.empty() || id.size() > 128) return false;
    for (wchar_t c : id)
        if (!(iswalnum(c) || c == L'.' || c == L'-' || c == L'_' || c == L'+')) return false;
    return true;
}

std::wstring SizeText(ULONGLONG bytes) {
    wchar_t buf[64];
    if (FAILED(StrFormatByteSizeEx(bytes, SFBS_FLAGS_ROUND_TO_NEAREST_DISPLAYED_DIGIT, buf, 64))) return L"";
    return buf;
}

std::wstring QuoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring q = L"\"";
    size_t bs = 0;
    for (wchar_t c : a) {
        if (c == L'\\') {
            ++bs;
            continue;
        }
        if (c == L'"') q.append(bs * 2 + 1, L'\\');
        else q.append(bs, L'\\');
        bs = 0;
        q.push_back(c);
    }
    q.append(bs * 2, L'\\');
    q.push_back(L'"');
    return q;
}

std::vector<GpuInfo> ScanGpus() {
    std::vector<GpuInfo> v;
    HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
    if (set == INVALID_HANDLE_VALUE) return v;
    SP_DEVINFO_DATA d{sizeof(d)};
    for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &d); ++i) {
        wchar_t inst[512];
        if (!SetupDiGetDeviceInstanceIdW(set, &d, inst, 512, nullptr) || _wcsnicmp(inst, L"PCI\\", 4)) continue;
        auto prop = [&](const DEVPROPKEY& key) -> std::wstring {
            DEVPROPTYPE t = 0;
            wchar_t buf[512];
            if (SetupDiGetDevicePropertyW(set, &d, &key, &t, reinterpret_cast<PBYTE>(buf), sizeof(buf), nullptr, 0) &&
                t == DEVPROP_TYPE_STRING)
                return buf;
            return {};
        };
        GpuInfo g;
        g.name = prop(DEVPKEY_Device_FriendlyName);
        if (g.name.empty()) g.name = prop(DEVPKEY_Device_DeviceDesc);
        g.version = prop(DEVPKEY_Device_DriverVersion);
        FILETIME ft{};
        DEVPROPTYPE t = 0;
        SYSTEMTIME st;
        if (SetupDiGetDevicePropertyW(set, &d, &DEVPKEY_Device_DriverDate, &t, reinterpret_cast<PBYTE>(&ft), sizeof(ft),
                                      nullptr, 0) &&
            t == DEVPROP_TYPE_FILETIME && FileTimeToSystemTime(&ft, &st))
            g.date = Fmt(L"%04d-%02d-%02d", st.wYear, st.wMonth, st.wDay);
        std::wstring lower = g.name;
        for (auto& c : lower) c = static_cast<wchar_t>(towlower(c));
        auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
        if (lower.find(L"nvidia") != std::wstring::npos) {
            const std::wstring nv = NvidiaVersion(g.version);
            if (!nv.empty()) g.version = nv;
            const std::wstring app = Expand(L"%ProgramFiles%\\NVIDIA Corporation\\NVIDIA App\\CEF\\NVIDIA App.exe");
            g.open = exists(app) ? app : L"https://www.nvidia.com/en-us/software/nvidia-app/";
            g.openLabel = L"Open NVIDIA app";
        } else if (lower.find(L"amd") != std::wstring::npos || lower.find(L"radeon") != std::wstring::npos) {
            const std::wstring app = Expand(L"%ProgramFiles%\\AMD\\CNext\\CNext\\RadeonSoftware.exe");
            g.open = exists(app) ? app : L"https://www.amd.com/en/support/download/drivers.html";
            g.openLabel = L"Open AMD Software";
        } else if (lower.find(L"intel") != std::wstring::npos) {
            g.open = L"https://www.intel.com/content/www/us/en/support/detect.html";
            g.openLabel = L"Intel driver page";
        }
        v.push_back(std::move(g));
    }
    SetupDiDestroyDeviceInfoList(set);
    // The card with the maker's app first (NVIDIA / AMD before the built-in Intel graphics).
    std::stable_sort(v.begin(), v.end(), [](const GpuInfo& a, const GpuInfo& b) {
        auto rank = [](const GpuInfo& g) { return g.openLabel.empty() ? 2 : g.openLabel.rfind(L"Intel", 0) == 0 ? 1 : 0; };
        return rank(a) < rank(b);
    });
    return v;
}

UpdateScan ScanUpdates(const Sink& progress) {
    UpdateScan wu, apps;
    progress(L"status", L"Checking Windows Update and apps… (Windows Update can take a few minutes)");
    if (FakeMode()) {
        Sleep(800);
        UpdateScan f;
        auto add = [&](const wchar_t* key, const wchar_t* name, const wchar_t* kind, const wchar_t* ver, ULONGLONG size) {
            UpdateItem it;
            it.key = key;
            it.name = name;
            it.kind = kind;
            it.version = ver;
            it.size = size;
            f.items.push_back(std::move(it));
        };
        add(L"wu:fake-cu", L"2026-11 Cumulative Update for Windows 11 (test)", L"Windows", L"KB5099001", 812ULL << 20);
        add(L"wu:fake-defender", L"Security Intelligence Update for Microsoft Defender (test)", L"Windows", L"KB2267602", 0);
        add(L"wu:fake-driver", L"Realtek - Audio - 6.0.9800.1 (test)", L"Driver", L"2026-09-30", 64ULL << 20);
        add(L"app:Fake.Browser", L"Fake Browser (test)", L"App", L"140.0  →  141.0", 0);
        add(L"app:Fake.Zip", L"Fake Zip (test)", L"App", L"24.08  →  25.01", 0);
        add(L"app:Fake.FailingEditor", L"Fake Editor that fails (test)", L"App", L"8.7  →  8.8", 0);
        f.gpus = ScanGpus();
        return f;
    }
    auto a = std::async(std::launch::async, [&] { ScanApps(apps); });
    ScanWindowsUpdate(wu);
    a.wait();
    UpdateScan out;
    out.wuError = wu.wuError;
    out.appError = apps.appError;
    out.items = std::move(wu.items);
    for (auto& i : apps.items) out.items.push_back(std::move(i));
    out.gpus = ScanGpus();
    return out;
}

std::vector<CleanItem> ScanCleanup() {
    std::vector<CleanItem> v;
    for (const CleanDef& d : kClean) {
        CleanItem it{d.key, d.name, d.note, 0, true, d.admin, d.byDefault};
        if (FakeMode()) {
            it.size = FakeSize(it.key);
            it.measured = it.key != L"winsxs";
        } else if (it.key == L"recycle") {
            SHQUERYRBINFO rb{sizeof(rb)};
            if (SUCCEEDED(SHQueryRecycleBinW(nullptr, &rb))) it.size = rb.i64Size;
        } else if (it.key == L"winsxs") {
            it.measured = false;
        } else {
            const WalkStats s = WalkAll(it.key, false);
            it.size = s.bytes;
            if (s.denied && !s.bytes) it.measured = false;
        }
        v.push_back(std::move(it));
    }
    return v;
}

std::vector<StartupItem> ScanStartup() {
    ComInit com(COINIT_APARTMENTTHREADED);
    std::vector<StartupItem> v;
    constexpr wchar_t kRun[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
    RunKey(v, HKEY_CURRENT_USER, kRun, 0, L"Run", false);
    StartupFolder(v, FOLDERID_Startup, false);
    RunKey(v, HKEY_LOCAL_MACHINE, kRun, KEY_WOW64_64KEY, L"Run", true);
    RunKey(v, HKEY_LOCAL_MACHINE, kRun, KEY_WOW64_32KEY, L"Run32", true);
    StartupFolder(v, FOLDERID_CommonStartup, true);
    std::stable_sort(v.begin(), v.end(),
                     [](const StartupItem& a, const StartupItem& b) { return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0; });
    return v;
}

bool RebootPending() {
    if (KeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\WindowsUpdate\\Auto Update\\RebootRequired") ||
        KeyExists(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Component Based Servicing\\RebootPending"))
        return true;
    ComInit com;
    ComPtr<ISystemInformation> si;
    VARIANT_BOOL r = VARIANT_FALSE;
    return SUCCEEDED(CoCreateInstance(CLSID_SystemInformation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&si))) &&
           SUCCEEDED(si->get_RebootRequired(&r)) && r == VARIANT_TRUE;
}

Health ReadHealth() {
    Health h;
    h.uptimeMs = GetTickCount64();
    h.rebootPending = RebootPending();
    GlobalMemoryStatusEx(&h.mem);
    wchar_t drives[256];
    const DWORD n = GetLogicalDriveStringsW(255, drives);
    for (const wchar_t* d = drives; n && n < 255 && *d; d += wcslen(d) + 1) {
        if (GetDriveTypeW(d) != DRIVE_FIXED) continue;
        ULARGE_INTEGER freeU{}, total{}, freeAll{};
        if (!GetDiskFreeSpaceExW(d, &freeU, &total, &freeAll)) continue;
        h.drives.push_back({std::wstring(d, 2), freeU.QuadPart, total.QuadPart});
    }
    return h;
}

int RunTask(const std::wstring& task, const std::vector<std::wstring>& args, const Sink& sink) {
    if (FakeMode()) return FakeTask(task, args, sink);
    ComInit com;
    Report rep(sink);
    int failed = 0;
    if (task == L"install") {
        // Items in order: Windows updates (one batch), then apps one by one. The restore point is a
        // step before them.
        std::vector<std::wstring> wu, apps;
        for (const auto& a : args)
            if (a.rfind(L"wu:", 0) == 0) wu.push_back(a);
            else if (a.rfind(L"app:", 0) == 0) apps.push_back(a);
        const bool restore = std::find(args.begin(), args.end(), L"restore") != args.end();
        const int items = static_cast<int>(wu.size() + apps.size());
        const double units = items + (restore ? 1 : 0);
        double done = 0;
        if (restore) {
            rep.Step(0, items, L"restore", L"Making a restore point");
            rep.Pct(-1);
            rep.End(L"restore", MakeRestorePoint(sink, L"Canivete: before installing updates") == 0 ? L"ok" : L"fail");
            done = 1;
        }
        auto skip = [&](const std::wstring& k) {
            sink(k, L"Not started (stopped)");
            rep.End(k, L"skip");
        };
        if (!wu.empty()) {
            if (CancelRequested()) {
                for (const auto& k : wu) skip(k);
            } else {
                const double base = done, n = static_cast<double>(wu.size());
                failed += InstallWindowsUpdates(wu, items, rep, [&](double f) { rep.Pct((base + n * f) / units); }) != 0;
            }
            done += static_cast<double>(wu.size());
        }
        for (size_t i = 0; i < apps.size(); ++i) {
            if (CancelRequested()) {
                skip(apps[i]);
                continue;
            }
            const double base = done;
            failed += UpgradeApp(apps[i], static_cast<int>(wu.size() + i + 1), items, rep,
                                 [&](double f) { rep.Pct((base + f) / units); }) != 0;
            done += 1;
        }
        if (RebootPending()) sink(L"reboot", L"");
    } else if (task == L"clean") {
        std::vector<std::wstring> keys;
        for (const auto& a : args)
            for (const CleanDef& d : kClean)
                if (a == d.key) keys.push_back(a);
        const int n = static_cast<int>(keys.size());
        ULONGLONG freed = 0;
        for (int i = 0; i < n; ++i) {
            const std::wstring& k = keys[static_cast<size_t>(i)];
            if (CancelRequested()) {
                sink(k, L"Not cleaned (stopped)");
                rep.End(k, L"skip");
                continue;
            }
            rep.Step(i + 1, n, k, L"Cleaning");
            rep.Pct(static_cast<double>(i) / n);
            failed += Clean(k, rep, freed, [&](double f) { rep.Pct((i + f) / n); }) != 0;
            rep.Freed(freed, true);
        }
        rep.Pct(1);
    } else if (task == L"startup") {
        const int n = static_cast<int>(args.size());
        for (int i = 0; i < n; ++i) {
            rep.Step(i + 1, n, args[static_cast<size_t>(i)].size() > 2 ? args[static_cast<size_t>(i)].substr(2) : L"", L"Changing");
            failed += SetStartup(args[static_cast<size_t>(i)], sink) != 0;
            rep.Pct((i + 1.0) / n);
        }
    } else if (task == L"restore") {
        rep.Step(0, 1, L"restore", L"Making a restore point");
        rep.Pct(-1);
        failed += MakeRestorePoint(sink, L"Canivete: restore point") != 0;
    } else if (task == L"repair") {
        sink(L"status", L"Checking Windows' system files (this takes a while)…");
        rep.Step(1, 2, L"dism", L"Repairing the Windows image");
        rep.Pct(0);
        failed += RunTool(L"Repairing the Windows image (DISM /RestoreHealth)", SysExe(L"Dism.exe"),
                          L"/Online /Cleanup-Image /RestoreHealth /NoRestart", sink, [&](double f) { rep.Pct(f / 2); }) != 0;
        if (CancelRequested()) {
            sink(L"log", L"Stopped before sfc.");
            rep.End(L"sfc", L"skip");
        } else {
            rep.Step(2, 2, L"sfc", L"Checking system files");
            rep.Pct(0.5);
            failed += RunTool(L"Checking system files (sfc /scannow)", SysExe(L"sfc.exe"), L"/scannow", sink,
                              [&](double f) { rep.Pct(0.5 + f / 2); }) != 0;
        }
        if (RebootPending()) sink(L"reboot", L"");
    } else if (task == L"optimize") {
        sink(L"status", L"Optimizing drives…");
        rep.Step(0, 1, L"optimize", L"Optimizing drives");
        rep.Pct(-1);
        failed += RunTool(L"Optimizing drives (trim for SSDs, defragment for hard disks)", SysExe(L"defrag.exe"),
                          L"/C /O /U /V", sink) != 0;
    } else if (task == L"scan") {
        sink(L"status", L"Quick virus scan (a few minutes)…");
        rep.Step(0, 1, L"scan", L"Quick virus scan");
        rep.Pct(-1);
        failed += RunTool(L"Microsoft Defender quick scan", Expand(L"%ProgramFiles%\\Windows Defender\\MpCmdRun.exe"),
                          L"-Scan -ScanType 1", sink) != 0;
    } else {
        return 2;
    }
    sink(L"status", failed ? L"Finished, with problems (see the list)." : L"Finished.");
    return failed ? 1 : 0;
}

}  // namespace maint

// --maint-admin <window> <task> [args…]: the elevated copy. Reports to the Maintenance window.
int Maint_AdminMain(int argc, wchar_t** argv) {
    if (argc < 4) return 2;
    HWND w = reinterpret_cast<HWND>(static_cast<UINT_PTR>(_wcstoui64(argv[2], nullptr, 10)));
    wchar_t cls[64] = L"";
    // Only for a job the Maintenance window asked for.
    if (!IsWindow(w) || !GetClassNameW(w, cls, 64) || wcscmp(cls, maint::kWindowClass)) return 3;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY, RPC_C_IMP_LEVEL_IMPERSONATE,
                         nullptr, EOAC_DYNAMIC_CLOAKING, nullptr);
    std::vector<std::wstring> args(argv + 4, argv + argc);
    const maint::Sink sink = [w](const std::wstring& key, const std::wstring& text) {
        Log(L"Maintenance: %s %s", key.c_str(), text.c_str());
        std::wstring msg = key + L"\t" + text;
        COPYDATASTRUCT cd{maint::kCopyProgress, static_cast<DWORD>((msg.size() + 1) * sizeof(wchar_t)), msg.data()};
        DWORD_PTR res = 0;
        SendMessageTimeoutW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 3000, &res);
    };
    // Windows gave this copy the foreground when the prompt closed, and it has no window: hand it back
    // to the Maintenance window so it doesn't drop behind other apps.
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    AllowSetForegroundWindow(pid);
    sink(L"hello", L"");
    HANDLE cancel = OpenEventW(SYNCHRONIZE, FALSE, maint::CancelEventName(pid).c_str());
    maint::SetCancelEvent(cancel);
    const int code = maint::RunTask(argv[3], args, sink);
    maint::SetCancelEvent(nullptr);
    if (cancel) CloseHandle(cancel);
    CoUninitialize();
    return code;
}
