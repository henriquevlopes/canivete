// Canivete's own updates: asks GitHub for the latest release of the public canivete repo, and when it
// is newer, downloads its setup, checks the file's SHA-256 against the one written in the release
// notes ("SHA256: <hex>"), runs it silently and exits. The installer replaces the files and starts
// Canivete again. Checked once a day (General: "Tell me when a new version is out") and on request.
//
// The notes also carry "SIG: <base64>": an ECDSA P-256 signature of "canivete-update|<version>|<sha>"
// made with an offline key (tools/sign_update.ps1). Only a release whose signature checks out against
// the public key below is offered, so someone who gets hold of the GitHub account still can't ship a
// version of their own.
#include "app.h"

#include <bcrypt.h>
#include <netlistmgr.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <ctime>
#include <memory>
#include <string>
#include <thread>

namespace {

constexpr wchar_t kHost[] = L"api.github.com";
constexpr wchar_t kLatest[] = L"/repos/henriquevlopes/canivete/releases/latest";
constexpr wchar_t kDownloadPrefix[] = L"https://github.com/henriquevlopes/canivete/releases/download/";
constexpr ULONGLONG kMaxSetup = 64ull << 20;  // a bigger "setup" is not ours

// BCRYPT_ECCPUBLIC_BLOB (ECDSA P-256) of the update signing key, made by tools/make_update_key.ps1.
// The private half stays on the release PC (%APPDATA%\Canivete-release\update_key.bin, DPAPI).
constexpr BYTE kUpdatePublicKey[72] = {
    0x45, 0x43, 0x53, 0x31, 0x20, 0x00, 0x00, 0x00, 0xEB, 0x9C, 0x66, 0x5E,
    0x47, 0xBA, 0x3F, 0x54, 0xFA, 0xCA, 0xF9, 0x00, 0x24, 0x8A, 0xE3, 0xB4,
    0x57, 0xEE, 0x02, 0xD7, 0xB5, 0xD4, 0xDA, 0x92, 0x80, 0x47, 0xED, 0x05,
    0x79, 0xA1, 0xDC, 0x76, 0x1F, 0x52, 0xCE, 0x2B, 0x6D, 0x64, 0xF4, 0x5B,
    0x0B, 0x20, 0x10, 0x24, 0x24, 0x34, 0x49, 0x2C, 0xCC, 0xF6, 0x4A, 0x51,
    0x11, 0xE5, 0x43, 0xD6, 0x9D, 0xC4, 0x99, 0x5C, 0xD4, 0x61, 0x0E, 0xBC,
};

UpdateInfo g_info;
bool g_checking = false, g_installing = false, g_manual = false;
int g_progress = -2;       // while updating: -1 = sliding bar, 0..1000 = download done, -2 = not updating
std::wstring g_stage;      // what it is doing, for the Maintenance window
std::wstring g_installError;  // why the last update didn't work
long long g_retryAt = 0;  // offline at the last try: not before this (time())

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
            unsigned cp = 0;
            bool hex = true;
            for (size_t h = 1; h <= 4; ++h) {
                const char d = j[p + h];
                hex = hex && isxdigit(static_cast<unsigned char>(d));
                cp = cp * 16 + static_cast<unsigned>(isdigit(static_cast<unsigned char>(d)) ? d - '0' : (tolower(d) - 'a' + 10));
            }
            if (hex && cp < 0x80) out += static_cast<char>(cp);
            p += 4;
        } else out += c;
    }
    if (end) *end = p;
    return out;
}

// GET over HTTPS. url: a full https:// URL. Into a string, or into a file when path is given (a new
// file: an existing one is never written through, and more than kMaxSetup bytes are refused).
bool HttpGet(const std::wstring& url, bool api, std::string* body, const std::wstring& path, std::wstring& err,
             const std::function<void(int)>& progress = nullptr) {
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
                    f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
                ok = path.empty() || f != INVALID_HANDLE_VALUE;
                DWORD len = 0, lsz = sizeof(len);
                if (!WinHttpQueryHeaders(r, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &len, &lsz,
                                         nullptr))
                    len = 0;
                char buf[65536];
                ULONGLONG total = 0;
                int lastPos = -2;
                for (DWORD got = 0; ok && WinHttpReadData(r, buf, sizeof(buf), &got) && got;) {
                    if (body) body->append(buf, got);
                    DWORD wrote = 0;
                    if (f != INVALID_HANDLE_VALUE && (!WriteFile(f, buf, got, &wrote, nullptr) || wrote != got)) ok = false;
                    if (body && body->size() > (8u << 20)) ok = false;
                    if ((total += got) > kMaxSetup) ok = false;
                    if (progress) {
                        const int pos = len ? static_cast<int>(std::min<ULONGLONG>(total * 1000 / len, 1000)) : -1;
                        if (pos != lastPos) progress(lastPos = pos);
                    }
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

// SHA-256 (hex) of what an open file holds, read from its start.
std::wstring Sha256(HANDLE f) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(f, zero, nullptr, FILE_BEGIN)) return {};
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
    return hex;
}

// Checks "SIG:" (base64 of r||s) for this version and SHA-256 against kUpdatePublicKey.
bool SignatureOk(const std::wstring& version, const std::wstring& shaHex, const std::string& sigB64) {
    BYTE sig[64];
    DWORD n = sizeof(sig);
    if (sigB64.empty() || shaHex.size() != 64 ||
        !CryptStringToBinaryA(sigB64.c_str(), static_cast<DWORD>(sigB64.size()), CRYPT_STRING_BASE64, sig, &n, nullptr,
                              nullptr) ||
        n != sizeof(sig))
        return false;
    std::string msg = "canivete-update|";
    for (wchar_t c : version) {
        if (c > 0x7E || c < 0x20) return false;
        msg += static_cast<char>(c);
    }
    msg += '|';
    for (wchar_t c : shaHex) {
        if (!iswxdigit(c)) return false;
        msg += static_cast<char>(towlower(c));
    }
    BYTE digest[32];
    if (!BCRYPT_SUCCESS(BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0,
                                   reinterpret_cast<PUCHAR>(msg.data()), static_cast<ULONG>(msg.size()), digest, 32)))
        return false;
    BCRYPT_KEY_HANDLE key = nullptr;
    if (!BCRYPT_SUCCESS(BCryptImportKeyPair(BCRYPT_ECDSA_P256_ALG_HANDLE, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                            const_cast<PUCHAR>(kUpdatePublicKey), sizeof(kUpdatePublicKey), 0)))
        return false;
    const bool ok = BCRYPT_SUCCESS(BCryptVerifySignature(key, nullptr, digest, 32, sig, sizeof(sig), 0));
    BCryptDestroyKey(key);
    return ok;
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
    {   // what changed: after the "Canivete x.y.z" title line, before the download / fingerprint lines
        std::string n = notes;
        n.erase(std::remove(n.begin(), n.end(), '\r'), n.end());
        const size_t first = n.find("\n\n");
        n = first == std::string::npos ? "" : n.substr(first + 2);
        for (const char* stop : {"Download Canivete-Setup", "SHA256:", "SIG:"}) {
            const size_t s = n.find(stop);
            if (s != std::string::npos) n.erase(s);
        }
        while (!n.empty() && (n.back() == '\n' || n.back() == ' ')) n.pop_back();
        std::string crlf;
        for (char ch : n) {
            if (ch == '\n') crlf += '\r';
            crlf += ch;
        }
        u.notes = Widen(crlf);
    }
    const size_t p = notes.find("SHA256:");
    if (p != std::string::npos) {
        size_t i = p + 7;
        while (i < notes.size() && notes[i] == ' ') ++i;
        std::string hex;
        while (i < notes.size() && isxdigit(static_cast<unsigned char>(notes[i]))) hex += static_cast<char>(tolower(notes[i++]));
        if (hex.size() == 64) u.sha = Widen(hex);
    }
    std::string sig;
    const size_t q = notes.find("SIG:");
    if (q != std::string::npos) {
        size_t i = q + 4;
        while (i < notes.size() && notes[i] == ' ') ++i;
        while (i < notes.size() && (isalnum(static_cast<unsigned char>(notes[i])) || notes[i] == '+' || notes[i] == '/' ||
                                    notes[i] == '='))
            sig += notes[i++];
    }
    if (u.latest.empty()) u.error = L"The release has no version number.";
    const bool newer = u.error.empty() && Compare(u.latest, CANIVETE_VERSION) > 0;
    const bool signedOk = newer && SignatureOk(u.latest, u.sha, sig);
    if (newer && !signedOk) {
        u.error = L"Version " + u.latest + L" isn't signed with Canivete's key, so it isn't offered.";
        Log(L"Update %s: missing or wrong signature", u.latest.c_str());
    }
    u.available = newer && signedOk && !u.url.empty() && u.url.rfind(kDownloadPrefix, 0) == 0 && !u.sha.empty();
    return u;
}

// A new folder of our own in %TEMP% (random name), so nothing can be planted where the setup lands.
std::wstring NewTempFolder() {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    for (int i = 0; i < 8; ++i) {
        BYTE rnd[8];
        if (!BCRYPT_SUCCESS(BCryptGenRandom(nullptr, rnd, sizeof(rnd), BCRYPT_USE_SYSTEM_PREFERRED_RNG))) return {};
        std::wstring dir = std::wstring(tmp) + L"Canivete-update-";
        for (BYTE b : rnd) {
            wchar_t x[3];
            swprintf_s(x, L"%02x", b);
            dir += x;
        }
        if (CreateDirectoryW(dir.c_str(), nullptr)) return dir;
    }
    return {};
}

struct InstallResult {
    std::wstring path, err;
    HANDLE lock = INVALID_HANDLE_VALUE;  // the checked setup, open so it can't be changed until it runs
    InstallResult() = default;
    InstallResult(const InstallResult&) = delete;
    InstallResult& operator=(const InstallResult&) = delete;
    ~InstallResult() { Release(); }
    void Release() {
        if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
        lock = INVALID_HANDLE_VALUE;
    }
    void Remove() {  // the setup and its folder
        Release();
        DeleteFileW(path.c_str());
        RemoveDirectoryW(path.substr(0, path.rfind(L'\\')).c_str());
    }
};

// Downloads and checks the setup into r (r.path empty = failed, r.err says why). The file is checked
// through a handle that lets nobody else write, rename or delete it, and that handle stays open until
// the setup has been started.
void Download(const UpdateInfo& u, InstallResult& r, const std::function<void(int)>& progress = nullptr) {
    const std::wstring dir = NewTempFolder();
    if (dir.empty()) {
        r.err = L"Couldn't make a folder for the download.";
        return;
    }
    const std::wstring path = dir + L"\\Canivete-Setup-" + u.latest + L".exe";
    if (!HttpGet(u.url, false, nullptr, path, r.err, progress)) {
        DeleteFileW(path.c_str());
        RemoveDirectoryW(dir.c_str());
        return;
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (f == INVALID_HANDLE_VALUE || _wcsicmp(Sha256(f).c_str(), u.sha.c_str()) != 0) {
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        DeleteFileW(path.c_str());
        RemoveDirectoryW(dir.c_str());
        r.err = L"The downloaded file doesn't match its published fingerprint, so it wasn't run.";
        return;
    }
    r.lock = f;
    r.path = path;
}

}  // namespace

const UpdateInfo& Update_Info() { return g_info; }
bool Update_Busy() { return g_checking || g_installing; }
int Update_Progress() { return g_installing || g_progress != -2 ? g_progress : -2; }
const std::wstring& Update_Stage() { return g_stage; }
const std::wstring& Update_InstallError() { return g_installError; }

void Update_Check(bool manual) {
    if (g_checking) return;
    g_checking = true;
    g_manual = manual;
    WriteState(L"LastAttempt", std::to_wstring(std::time(nullptr)));
    HWND w = g_mainWnd;
    if (manual) SettingsUI_UpdateChanged();  // feedback: "Checking…" with a moving bar in Settings
    std::thread([w, manual] {
        const ULONGLONG t0 = GetTickCount64();
        auto* u = new UpdateInfo(Fetch());
        if (manual && GetTickCount64() - t0 < 800) Sleep(static_cast<DWORD>(800 - (GetTickCount64() - t0)));  // long enough to be seen
        if (!PostMessageW(w, WM_APP_UPDATE, 0, reinterpret_cast<LPARAM>(u))) delete u;
    }).detach();
}

// Due 20 hours after the last check that reached GitHub, and 6 hours after any try (so a PC that is
// offline doesn't try every hour).
long long Update_SecondsToNext() {
    if (!g_settings.checkUpdates) return LLONG_MAX;
    const long long now = std::time(nullptr);
    const long long due = std::max({_wtoi64(ReadState(L"LastCheck").c_str()) + 20 * 3600,
                                    _wtoi64(ReadState(L"LastAttempt").c_str()) + 6 * 3600, g_retryAt});
    return std::max(0LL, due - now);
}

bool Net_Online() {
    Microsoft::WRL::ComPtr<INetworkListManager> nlm;
    VARIANT_BOOL on = VARIANT_TRUE;
    if (SUCCEEDED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm))))
        nlm->get_IsConnectedToInternet(&on);
    return on != VARIANT_FALSE;
}

void Update_Background() {
    if (g_checking || Update_SecondsToNext() > 0) return;
    if (!Net_Online()) {  // no request at all; look again in an hour
        g_retryAt = std::time(nullptr) + 3600;
        return;
    }
    Update_Check(false);
}

void Update_Install() {
    if (g_installing || !g_info.available) return;
    g_installing = true;
    g_progress = -1;
    g_stage = L"Downloading version " + g_info.latest + L"…";
    g_installError.clear();
    HWND w = g_mainWnd;
    const UpdateInfo u = g_info;
    std::thread([w, u] {
        auto* r = new InstallResult;
        Download(u, *r, [w](int pos) { PostMessageW(w, WM_APP_UPDATE, 2, pos + 1); });  // lp: 0 = sliding, else pos+1
        if (!PostMessageW(w, WM_APP_UPDATE, 1, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
    SettingsUI_UpdateChanged();
}

void Update_OnMessage(WPARAM wp, LPARAM lp) {
    if (wp == 2) {  // download progress
        const int pos = static_cast<int>(lp) - 1;
        if (g_installing) {
            g_progress = pos;
            g_stage = L"Downloading version " + g_info.latest + L"…" + (pos >= 0 ? L"  " + std::to_wstring(pos / 10) + L"%" : L"");
            SettingsUI_UpdateChanged();
            UpdateDlg_Progress(pos, g_stage);
            Notice_Progress(L"Updating Canivete", g_stage, pos);
        }
        return;
    }
    if (wp == 0) {  // check finished
        std::unique_ptr<UpdateInfo> u(reinterpret_cast<UpdateInfo*>(lp));
        g_checking = false;
        g_info = *u;
        if (u->checked) WriteState(L"LastCheck", std::to_wstring(std::time(nullptr)));
        Log(L"Update check: latest %s, this %s%s%s", u->latest.c_str(), CANIVETE_VERSION,
            u->error.empty() ? L"" : L", ", u->error.c_str());
        // No pop-ups: what was found shows in the Maintenance window (Overview).
        Tray_UpdateBadge(u->available);  // a green dot on the tray icon; clicking "Update" in its menu updates at once
        if (u->available && g_manual) UpdateDlg_Show();
        SettingsUI_UpdateChanged();
        return;
    }
    std::unique_ptr<InstallResult> r(reinterpret_cast<InstallResult*>(lp));
    g_installing = false;
    g_progress = -2;
    if (r->path.empty()) {
        Log(L"Update failed: %s", r->err.c_str());
        g_installError = r->err;
        UpdateDlg_Failed(r->err);
        Notice_Show(L"Canivete update failed", r->err, nullptr);
        SettingsUI_UpdateChanged();
        return;
    }
    if (TestMode()) {
        Log(L"Update downloaded and checked (test copy, not installing): %s", r->path.c_str());
        r->Remove();
        SettingsUI_UpdateChanged();
        return;
    }
    g_progress = -1;
    g_stage = L"Installing version " + g_info.latest + L"… Canivete restarts by itself.";
    UpdateDlg_Progress(-1, g_stage);
    Notice_Progress(L"Updating Canivete", g_stage, -1);
    SettingsUI_UpdateChanged();
    Log(L"Running %s", r->path.c_str());
    const HINSTANCE h = ShellExecuteW(nullptr, L"open", r->path.c_str(),
                                      L"/VERYSILENT /SUPPRESSMSGBOXES /NORESTART", nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(h) <= 32) {
        g_progress = -2;
        g_installError = L"Couldn't start the installer.";
        UpdateDlg_Failed(g_installError);
        Notice_Show(L"Canivete update failed", g_installError, nullptr);
        SettingsUI_UpdateChanged();
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
    InstallResult r;
    Download(u, r);
    Log(L"--update-test: download %s %s", r.path.empty() ? L"FAILED" : L"ok, fingerprint matches", r.err.c_str());
    if (r.path.empty()) return 2;
    r.Remove();
    return 0;
}

// --verify-update <version> <sha256> <base64 signature>: the signature check alone, for
// tools/test_update_sig.ps1. 0 = valid, 1 = not.
int Update_VerifyTest(int argc, wchar_t** argv) {
    if (argc < 5) return 2;
    std::string sig;
    for (const wchar_t* p = argv[4]; *p; ++p) sig += static_cast<char>(*p & 0x7F);
    const bool ok = SignatureOk(argv[2], argv[3], sig);
    Log(L"--verify-update %s %s: %s", argv[2], argv[3], ok ? L"valid" : L"INVALID");
    return ok ? 0 : 1;
}

int Update_Compare(const std::wstring& a, const std::wstring& b) { return Compare(a, b); }
