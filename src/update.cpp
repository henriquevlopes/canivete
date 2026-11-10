// Canivete's own updates: asks GitHub for the latest release of the public releases repo, and when it
// is newer, downloads its setup, checks the file's SHA-256 against the one written in the release
// notes ("SHA256: <hex>"), runs it silently and exits. The installer replaces the files and starts
// Canivete again. Checked once a day (General: "Tell me when a new version is out") and on request.
#include "app.h"

#include <bcrypt.h>
#include <shellapi.h>
#include <winhttp.h>

#include <ctime>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kHost[] = L"api.github.com";
constexpr wchar_t kLatest[] = L"/repos/henriquevlopes/canivete-releases/releases/latest";
constexpr wchar_t kDownloadPrefix[] = L"https://github.com/henriquevlopes/canivete-releases/releases/download/";

UpdateInfo g_info;
bool g_checking = false, g_installing = false, g_manual = false;

std::wstring StatePath() { return AppDataDir() + L"\\state.ini"; }
std::wstring ReadState(const wchar_t* key) {
    wchar_t buf[256] = L"";
    GetPrivateProfileStringW(L"Update", key, L"", buf, 256, StatePath().c_str());
    return buf;
}
void WriteState(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"Update", key, v.c_str(), StatePath().c_str());
}

// "0.2.10" > "0.2.9"; a leading "v" is ignored.
int Compare(std::wstring a, std::wstring b) {
    for (auto* s : {&a, &b})
        if (!s->empty() && (s->front() == L'v' || s->front() == L'V')) s->erase(0, 1);
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        long x = 0, y = 0;
        while (i < a.size() && iswdigit(a[i])) x = x * 10 + (a[i++] - L'0');
        while (j < b.size() && iswdigit(b[j])) y = y * 10 + (b[j++] - L'0');
        if (x != y) return x < y ? -1 : 1;
        while (i < a.size() && !iswdigit(a[i])) ++i;
        while (j < b.size() && !iswdigit(b[j])) ++j;
    }
    return 0;
}

std::wstring Widen(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

// The JSON string value after "key": (first match from pos). Handles the usual escapes.
std::string JsonString(const std::string& j, const std::string& key, size_t from = 0, size_t* end = nullptr) {
    const std::string k = "\"" + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return {};
    p = j.find(':', p + k.size());
    if (p == std::string::npos) return {};
    p = j.find('"', p);
    if (p == std::string::npos) return {};
    std::string out;
    for (++p; p < j.size() && j[p] != '"'; ++p) {
        if (j[p] != '\\' || p + 1 >= j.size()) {
            out += j[p];
            continue;
        }
        const char c = j[++p];
        if (c == 'n') out += '\n';
        else if (c == 'r') out += '\r';
        else if (c == 't') out += '\t';
        else if (c == 'u' && p + 4 < j.size()) {
            const unsigned cp = static_cast<unsigned>(std::stoul(j.substr(p + 1, 4), nullptr, 16));
            if (cp < 0x80) out += static_cast<char>(cp);
            p += 4;
        } else out += c;
    }
    if (end) *end = p;
    return out;
}

// GET over HTTPS. url: a full https:// URL. Into a string, or into a file when path is given.
bool HttpGet(const std::wstring& url, bool api, std::string* body, const std::wstring& path, std::wstring& err) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], urlPath[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = urlPath;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc) || uc.nScheme != INTERNET_SCHEME_HTTPS) {
        err = L"Bad address.";
        return false;
    }
    const std::wstring agent = std::wstring(L"Canivete/") + CANIVETE_VERSION;
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", urlPath, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    bool ok = false;
    if (r) {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        WinHttpSetTimeouts(r, 10000, 10000, 15000, 30000);
        const wchar_t* hdr = api ? L"Accept: application/vnd.github+json\r\nX-GitHub-Api-Version: 2022-11-28\r\n"
                                 : L"Accept: application/octet-stream\r\n";
        if (WinHttpSendRequest(r, hdr, static_cast<DWORD>(-1L), nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            if (status == 200) {
                HANDLE f = INVALID_HANDLE_VALUE;
                if (!path.empty())
                    f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
                ok = path.empty() || f != INVALID_HANDLE_VALUE;
                char buf[65536];
                for (DWORD got = 0; ok && WinHttpReadData(r, buf, sizeof(buf), &got) && got;) {
                    if (body) body->append(buf, got);
                    DWORD wrote = 0;
                    if (f != INVALID_HANDLE_VALUE && (!WriteFile(f, buf, got, &wrote, nullptr) || wrote != got)) ok = false;
                    if (body && body->size() > (8u << 20)) ok = false;
                }
                if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
                if (!ok) err = L"The download didn't finish.";
            } else {
                err = status == 404 ? L"No release found." : L"GitHub answered " + std::to_wstring(status) + L".";
            }
        } else {
            err = L"Couldn't reach GitHub (no internet?).";
        }
    } else {
        err = L"Couldn't reach GitHub.";
    }
    for (HINTERNET h : {r, c, s})
        if (h) WinHttpCloseHandle(h);
    return ok;
}

std::wstring Sha256(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::wstring hex;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0))) {
        std::unique_ptr<BYTE[]> buf(new BYTE[1 << 16]);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.get(), 1 << 16, &got, nullptr) && got)
            ok = BCRYPT_SUCCESS(BCryptHashData(h, buf.get(), got, 0));
        BYTE d[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(h, d, 32, 0)))
            for (BYTE b : d) {
                wchar_t x[3];
                swprintf_s(x, L"%02x", b);
                hex += x;
            }
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

UpdateInfo Fetch() {
    UpdateInfo u;
    std::string body;
    if (!HttpGet(std::wstring(L"https://") + kHost + kLatest, true, &body, L"", u.error)) return u;
    u.checked = true;
    u.latest = Widen(JsonString(body, "tag_name"));
    if (!u.latest.empty() && (u.latest[0] == L'v' || u.latest[0] == L'V')) u.latest.erase(0, 1);
    // The setup asset, and the SHA-256 line in the notes.
    for (size_t pos = 0;;) {
        size_t end = 0;
        const std::string url = JsonString(body, "browser_download_url", pos, &end);
        if (url.empty()) break;
        pos = end;
        if (url.size() > 4 && _stricmp(url.c_str() + url.size() - 4, ".exe") == 0) {
            u.url = Widen(url);
            break;
        }
    }
    const std::string notes = JsonString(body, "body");
    const size_t p = notes.find("SHA256:");
    if (p != std::string::npos) {
        size_t i = p + 7;
        while (i < notes.size() && notes[i] == ' ') ++i;
        std::string hex;
        while (i < notes.size() && isxdigit(static_cast<unsigned char>(notes[i]))) hex += static_cast<char>(tolower(notes[i++]));
        if (hex.size() == 64) u.sha = Widen(hex);
    }
    if (u.latest.empty()) u.error = L"The release has no version number.";
    u.available = u.error.empty() && Compare(u.latest, CANIVETE_VERSION) > 0 && !u.url.empty() &&
                  u.url.rfind(kDownloadPrefix, 0) == 0 && !u.sha.empty();
    return u;
}

// Downloads and checks the setup; empty = failed (err says why).
std::wstring Download(const UpdateInfo& u, std::wstring& err) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    const std::wstring path = std::wstring(tmp) + L"Canivete-Setup-" + u.latest + L".exe";
    if (!HttpGet(u.url, false, nullptr, path, err)) return {};
    if (_wcsicmp(Sha256(path).c_str(), u.sha.c_str()) != 0) {
        DeleteFileW(path.c_str());
        err = L"The downloaded file doesn't match its published fingerprint, so it wasn't run.";
        return {};
    }
    return path;
}

struct InstallResult {
    std::wstring path, err;
};

}  // namespace

const UpdateInfo& Update_Info() { return g_info; }
bool Update_Busy() { return g_checking || g_installing; }

void Update_Check(bool manual) {
    if (g_checking) return;
    g_checking = true;
    g_manual = manual;
    HWND w = g_mainWnd;
    std::thread([w] {
        auto* u = new UpdateInfo(Fetch());
        if (!PostMessageW(w, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(u))) delete u;
    }).detach();
}

void Update_Background() {
    if (!g_settings.checkUpdates) return;
    const long long last = _wtoi64(ReadState(L"LastCheck").c_str());
    if (std::time(nullptr) - last < 20 * 3600) return;
    Update_Check(false);
}

void Update_Install() {
    if (g_installing || !g_info.available) return;
    const std::wstring q = L"Update Canivete to version " + g_info.latest +
                           L" now?\n\nIt closes, installs the new version and starts again (a few seconds).";
    if (MessageBoxW(nullptr, q.c_str(), L"Canivete update", MB_OKCANCEL | MB_ICONQUESTION | MB_SETFOREGROUND) != IDOK)
        return;
    g_installing = true;
    HWND w = g_mainWnd;
    const UpdateInfo u = g_info;
    std::thread([w, u] {
        auto* r = new InstallResult;
        r->path = Download(u, r->err);
        if (!PostMessageW(w, WM_APP_UPDATE, 1, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
    Tray_Notify(L"Updating Canivete", L"Downloading version " + u.latest + L"…");
}

void Update_OnMessage(WPARAM wp, LPARAM lp) {
    if (wp == 0) {  // check finished
        std::unique_ptr<UpdateInfo> u(reinterpret_cast<UpdateInfo*>(lp));
        g_checking = false;
        g_info = *u;
        if (u->checked) WriteState(L"LastCheck", std::to_wstring(std::time(nullptr)));
        Log(L"Update check: latest %s, this %s%s%s", u->latest.c_str(), CANIVETE_VERSION,
            u->error.empty() ? L"" : L", ", u->error.c_str());
        if (u->available && (g_manual || ReadState(L"Notified") != u->latest)) {
            WriteState(L"Notified", u->latest);
            Tray_NotifyAction(L"A new Canivete is out", L"Version " + u->latest + L" is ready. Click here to update.",
                              [] { Update_Install(); });
        } else if (g_manual) {
            Tray_Notify(L"Canivete updates", u->error.empty() ? std::wstring(L"Canivete is up to date (version ") +
                                                                    CANIVETE_VERSION + L")."
                                                              : L"Couldn't check: " + u->error);
        }
        Maint_Refresh();
        return;
    }
    std::unique_ptr<InstallResult> r(reinterpret_cast<InstallResult*>(lp));
    g_installing = false;
    if (r->path.empty()) {
        Log(L"Update failed: %s", r->err.c_str());
        Tray_Notify(L"Canivete update failed", r->err);
        return;
    }
    if (TestMode()) {
        Log(L"Update downloaded and checked (test copy, not installing): %s", r->path.c_str());
        return;
    }
    Log(L"Running %s", r->path.c_str());
    const HINSTANCE h = ShellExecuteW(nullptr, L"open", r->path.c_str(),
                                      L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        Tray_Notify(L"Canivete update failed", L"Couldn't start the installer.");
        return;
    }
    DestroyWindow(g_mainWnd);  // the installer starts the new version when it's done
}

// --update-test: checks, downloads and verifies the latest release without installing. 0 = all fine.
int Update_SelfTest() {
    const UpdateInfo u = Fetch();
    Log(L"--update-test: latest %s, this %s, url %s, sha %s, available %d, error %s", u.latest.c_str(), CANIVETE_VERSION,
        u.url.c_str(), u.sha.c_str(), u.available, u.error.c_str());
    if (!u.checked || u.url.empty() || u.sha.empty()) return 1;
    UpdateInfo any = u;
    std::wstring err;
    const std::wstring path = Download(any, err);
    Log(L"--update-test: download %s %s", path.empty() ? L"FAILED" : L"ok, fingerprint matches", err.c_str());
    if (path.empty()) return 2;
    DeleteFileW(path.c_str());
    return 0;
}

int Update_Compare(const std::wstring& a, const std::wstring& b) { return Compare(a, b); }
