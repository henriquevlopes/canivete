// Uploads: sends a capture to Google Drive or Amazon S3 and copies its link.
//
// Google Drive: OAuth 2.0 for installed apps. The browser signs in, Google sends the code back to a
// one-off listener on 127.0.0.1 (loopback redirect), PKCE proves this copy asked for it. Scope
// drive.file: Canivete only sees the files it made. Resumable upload in 8 MB pieces (a broken
// connection resumes where it stopped), then optionally "anyone with the link can view".
// Amazon S3: PutObject signed with AWS Signature V4, done here with WinHTTP and BCrypt (no SDK).
// Links are public addresses (bucket or CloudFront) or presigned GET links that last 7 days.
//
// Secrets (Google client secret and sign-in, AWS secret key) are encrypted with DPAPI for this
// Windows user and kept in secrets.ini, never in settings.ini.
//
// Test mode: CANIVETE_UPLOAD_MOCK=http://127.0.0.1:port sends every request to a local fake server
// (tools/upload_mock.py) instead of Google and AWS, and no browser is opened.
#include "app.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <bcrypt.h>
#include <objbase.h>
#include <shellapi.h>
#include <wincrypt.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <mutex>
#include <thread>

namespace {

constexpr wchar_t kAuthUrl[] = L"https://accounts.google.com/o/oauth2/v2/auth";
constexpr wchar_t kTokenUrl[] = L"https://oauth2.googleapis.com/token";
constexpr wchar_t kRevokeUrl[] = L"https://oauth2.googleapis.com/revoke";
constexpr wchar_t kDriveApi[] = L"https://www.googleapis.com/drive/v3";
constexpr wchar_t kDriveUpload[] = L"https://www.googleapis.com/upload/drive/v3/files";
constexpr char kDriveScope[] = "https://www.googleapis.com/auth/drive.file";
constexpr uint64_t kChunk = 8ull << 20;  // Drive pieces: a multiple of 256 KB
constexpr int kPresignSeconds = 7 * 24 * 3600;  // the longest AWS allows

enum Msg : WPARAM { kStarted, kProgress, kDone, kConnected, kTested };

// ---- small helpers ---------------------------------------------------------------------------

std::string Utf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

std::wstring TrimW(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

std::string Hex(const BYTE* p, size_t n) {
    static const char d[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

std::string Sha256(const std::string& data) {
    BYTE h[32]{};
    BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
               static_cast<ULONG>(data.size()), h, 32);
    return std::string(reinterpret_cast<char*>(h), 32);
}

std::string Hmac(const std::string& key, const std::string& data) {
    BYTE h[32]{};
    BCryptHash(BCRYPT_HMAC_SHA256_ALG_HANDLE, reinterpret_cast<PUCHAR>(const_cast<char*>(key.data())),
               static_cast<ULONG>(key.size()), reinterpret_cast<PUCHAR>(const_cast<char*>(data.data())),
               static_cast<ULONG>(data.size()), h, 32);
    return std::string(reinterpret_cast<char*>(h), 32);
}

std::string HexOf(const std::string& raw) { return Hex(reinterpret_cast<const BYTE*>(raw.data()), raw.size()); }

std::string Base64(const BYTE* p, DWORD n) {
    DWORD len = 0;
    CryptBinaryToStringA(p, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, nullptr, &len);
    std::string s(len, '\0');
    if (!CryptBinaryToStringA(p, n, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, s.data(), &len)) return {};
    s.resize(len);
    return s;
}

std::string Base64Url(const std::string& raw) {
    std::string s = Base64(reinterpret_cast<const BYTE*>(raw.data()), static_cast<DWORD>(raw.size()));
    for (char& c : s) c = c == '+' ? '-' : c == '/' ? '_' : c;
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}

std::string RandomBytes(size_t n) {
    std::string s(n, '\0');
    BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(s.data()), static_cast<ULONG>(n), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    return s;
}

// RFC 3986: everything but A-Z a-z 0-9 - _ . ~ is %XX (and / too unless keepSlash).
std::string UriEncode(const std::string& s, bool keepSlash) {
    static const char d[] = "0123456789ABCDEF";
    std::string o;
    for (unsigned char c : s) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' || (keepSlash && c == '/')) {
            o += static_cast<char>(c);
        } else {
            o += '%';
            o += d[c >> 4];
            o += d[c & 15];
        }
    }
    return o;
}

std::string UrlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') o += ' ';
        else if (s[i] == '%' && i + 2 < s.size() && isxdigit(static_cast<unsigned char>(s[i + 1])) &&
                 isxdigit(static_cast<unsigned char>(s[i + 2]))) {
            o += static_cast<char>(std::stoi(s.substr(i + 1, 2), nullptr, 16));
            i += 2;
        } else o += s[i];
    }
    return o;
}

std::string JsonEscape(const std::string& s) {
    std::string o;
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += static_cast<char>(c);
        } else if (c < 0x20) {
            char b[8];
            sprintf_s(b, "\\u%04x", c);
            o += b;
        } else o += static_cast<char>(c);
    }
    return o;
}

// The string after "key": in a JSON text (first one from `from`). Enough for Google's answers.
std::string JsonString(const std::string& j, const std::string& key, size_t from = 0) {
    const std::string k = "\"" + key + "\"";
    size_t p = j.find(k, from);
    if (p == std::string::npos) return {};
    p = j.find_first_not_of(" \t\r\n", p + k.size());
    if (p == std::string::npos || j[p] != ':') return {};
    p = j.find_first_not_of(" \t\r\n", p + 1);
    if (p == std::string::npos || j[p] != '"') return {};
    std::string out;
    for (++p; p < j.size() && j[p] != '"'; ++p) {
        if (j[p] != '\\' || p + 1 >= j.size()) {
            out += j[p];
            continue;
        }
        const char c = j[++p];
        if (c == 'n') out += '\n';
        else if (c == 't') out += '\t';
        else if (c == 'u' && p + 4 < j.size()) {
            unsigned cp = 0;
            bool hex = true;
            for (size_t h = p + 1; h <= p + 4; ++h) {
                const unsigned char d = static_cast<unsigned char>(j[h]);
                if (!isxdigit(d)) hex = false;
                cp = cp * 16 + (isdigit(d) ? d - '0' : (tolower(d) - 'a' + 10) & 15);
            }
            if (hex && cp < 0x80) out += static_cast<char>(cp);
            else out += '?';
            p += 4;
        } else out += c;
    }
    return out;
}

long long JsonNumber(const std::string& j, const std::string& key) {
    const std::string k = "\"" + key + "\"";
    size_t p = j.find(k);
    if (p == std::string::npos) return 0;
    p = j.find(':', p + k.size());
    return p == std::string::npos ? 0 : _atoi64(j.c_str() + p + 1);
}

std::string XmlTag(const std::string& x, const std::string& tag) {
    const size_t a = x.find("<" + tag + ">");
    if (a == std::string::npos) return {};
    const size_t s = a + tag.size() + 2, e = x.find("</" + tag + ">", s);
    return e == std::string::npos ? std::string() : x.substr(s, e - s);
}

std::wstring SizeText(uint64_t b) {
    wchar_t s[32];
    if (b < 1024 * 1024) swprintf_s(s, L"%.0f KB", b / 1024.0);
    else swprintf_s(s, L"%.1f MB", b / 1048576.0);
    return s;
}

std::wstring FileName(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

std::string MimeOf(const std::wstring& path) {
    std::wstring ext = path.substr(path.find_last_of(L'.') + 1);
    for (auto& c : ext) c = static_cast<wchar_t>(towlower(c));
    if (ext == L"png") return "image/png";
    if (ext == L"jpg" || ext == L"jpeg") return "image/jpeg";
    if (ext == L"gif") return "image/gif";
    if (ext == L"mp4") return "video/mp4";
    if (ext == L"webp") return "image/webp";
    if (ext == L"txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

// ---- test redirection ------------------------------------------------------------------------

const std::wstring& MockBase() {
    static const std::wstring base = [] {
        wchar_t b[256] = {};
        if (!TestMode() || !GetEnvironmentVariableW(L"CANIVETE_UPLOAD_MOCK", b, 256)) return std::wstring();
        std::wstring s = b;
        while (!s.empty() && s.back() == L'/') s.pop_back();
        return s;
    }();
    return base;
}

// A Google address, or the same path on the fake server in a test.
std::wstring Endpoint(const std::wstring& url) {
    if (MockBase().empty()) return url;
    const size_t p = url.find(L'/', url.find(L"://") + 3);
    return MockBase() + (p == std::wstring::npos ? L"" : url.substr(p));
}

// ---- secrets (DPAPI) -------------------------------------------------------------------------

std::mutex g_secretMx;

std::wstring SecretsPath() { return AppDataDir() + L"\\secrets.ini"; }

// A link without its query: a private S3 link's signature never reaches the log.
std::wstring LogLink(const std::wstring& link) { return link.substr(0, link.find(L'?')); }

DATA_BLOB Entropy() {
    static char e[] = "Canivete uploads";
    return {static_cast<DWORD>(sizeof(e) - 1), reinterpret_cast<BYTE*>(e)};
}

// ---- HTTP ------------------------------------------------------------------------------------

struct HttpReq {
    const wchar_t* method = L"GET";
    std::wstring url;
    std::wstring headers;  // "Name: value\r\n" lines
    std::string body;
    HANDLE file = INVALID_HANDLE_VALUE;  // or a piece of this file as the body
    uint64_t offset = 0, length = 0;
    std::function<void(uint64_t sent)> progress;
};

struct HttpResp {
    DWORD status = 0;  // 0 = no answer (error says why)
    std::string body;
    std::wstring location, range;
    std::wstring error;
};

std::wstring QueryHeader(HINTERNET r, DWORD what, const wchar_t* name = WINHTTP_HEADER_NAME_BY_INDEX) {
    DWORD len = 0;
    WinHttpQueryHeaders(r, what, name, nullptr, &len, nullptr);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER || !len) return {};
    std::wstring v(len / sizeof(wchar_t), L'\0');
    if (!WinHttpQueryHeaders(r, what, name, v.data(), &len, nullptr)) return {};
    v.resize(len / sizeof(wchar_t));
    return v;
}

bool Http(const HttpReq& q, HttpResp& a) {
    a = {};
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], path[4096];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = 4096;
    wchar_t extra[4096];
    uc.lpszExtraInfo = extra;
    uc.dwExtraInfoLength = 4096;
    const bool crack = WinHttpCrackUrl(q.url.c_str(), 0, 0, &uc);
    const bool secure = crack && uc.nScheme == INTERNET_SCHEME_HTTPS;
    if (!crack || (!secure && MockBase().empty())) {
        a.error = L"Bad address: " + q.url;
        return false;
    }
    const std::wstring object = std::wstring(path) + extra;
    const std::wstring agent = std::wstring(L"Canivete/") + CANIVETE_VERSION;
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, q.method, object.c_str(), nullptr, nullptr, nullptr,
                                         secure ? WINHTTP_FLAG_SECURE : 0)
                    : nullptr;
    bool ok = false;
    if (r) {
        // Drive answers 308 for "piece received": that must come back to us, not be followed.
        DWORD noRedirect = WINHTTP_DISABLE_REDIRECTS;
        WinHttpSetOption(r, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirect, sizeof(noRedirect));
        WinHttpSetTimeouts(r, 15000, 15000, 60000, 60000);
        const bool fromFile = q.file != INVALID_HANDLE_VALUE;
        const uint64_t total = fromFile ? q.length : q.body.size();
        BOOL sent = FALSE;
        if (!fromFile) {
            sent = WinHttpSendRequest(r, q.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : q.headers.c_str(),
                                      static_cast<DWORD>(-1L), const_cast<char*>(q.body.data()),
                                      static_cast<DWORD>(q.body.size()), static_cast<DWORD>(q.body.size()), 0);
        } else if (total < 0xFFFFFFFFull) {
            sent = WinHttpSendRequest(r, q.headers.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : q.headers.c_str(),
                                      static_cast<DWORD>(-1L), WINHTTP_NO_REQUEST_DATA, 0, static_cast<DWORD>(total), 0);
            LARGE_INTEGER pos;
            pos.QuadPart = static_cast<LONGLONG>(q.offset);
            sent = sent && SetFilePointerEx(q.file, pos, nullptr, FILE_BEGIN);
            std::unique_ptr<char[]> buf(new char[1 << 18]);
            for (uint64_t done = 0; sent && done < total;) {
                const DWORD want = static_cast<DWORD>(std::min<uint64_t>(1 << 18, total - done));
                DWORD got = 0, wrote = 0;
                if (!ReadFile(q.file, buf.get(), want, &got, nullptr) || got != want) {
                    a.error = L"Couldn't read the file.";
                    sent = FALSE;
                    break;
                }
                sent = WinHttpWriteData(r, buf.get(), got, &wrote) && wrote == got;
                done += got;
                if (sent && q.progress) q.progress(done);
            }
        }
        if (sent && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            a.status = status;
            a.location = QueryHeader(r, WINHTTP_QUERY_LOCATION);
            a.range = QueryHeader(r, WINHTTP_QUERY_CUSTOM, L"Range");
            char buf[16384];
            for (DWORD got = 0; WinHttpReadData(r, buf, sizeof(buf), &got) && got;)
                if (a.body.size() < (1u << 20)) a.body.append(buf, got);
            ok = true;
        } else if (a.error.empty()) {
            const DWORD e = GetLastError();
            a.error = e == ERROR_WINHTTP_TIMEOUT ? L"The connection timed out."
                      : e == ERROR_WINHTTP_NAME_NOT_RESOLVED || e == ERROR_WINHTTP_CANNOT_CONNECT
                          ? std::wstring(L"Couldn't reach ") + host + L" (no internet?)."
                          : std::wstring(L"The connection to ") + host + L" failed (error " + std::to_wstring(e) + L").";
        }
    } else {
        a.error = std::wstring(L"Couldn't connect to ") + host + L".";
    }
    for (HINTERNET h : {r, c, s})
        if (h) WinHttpCloseHandle(h);
    return ok;
}

std::string Form(std::initializer_list<std::pair<const char*, std::string>> kv) {
    std::string s;
    for (const auto& [k, v] : kv) {
        if (!s.empty()) s += '&';
        s += k;
        s += '=';
        s += UriEncode(v, false);
    }
    return s;
}

// ---- Google ----------------------------------------------------------------------------------

struct Token {
    std::string access;
    long long expires = 0;  // GetTickCount64() ms
} g_token;
std::mutex g_tokenMx;

bool g_driveReconnect = false;  // the last failure needs a new sign-in

// Google's error text for a failed token request, in plain words.
std::wstring TokenError(const HttpResp& a) {
    const std::string e = JsonString(a.body, "error");
    if (e == "invalid_grant")
        return L"Google signed Canivete out (the sign-in expired or was removed). Connect again in Settings → "
               L"Uploads. If this happens every week, the Google Cloud app is still in Testing: publish it.";
    if (e == "invalid_client" || e == "unauthorized_client")
        return L"Google doesn't accept the Client ID or Client secret. Check them in Settings → Uploads.";
    if (!a.status) return a.error;
    const std::string d = JsonString(a.body, "error_description");
    return L"Google answered " + std::to_wstring(a.status) + (e.empty() ? L"" : L" (" + Wide(e) + L")") +
           (d.empty() ? L"" : L": " + Wide(d));
}

bool DriveAccessToken(const Settings& s, std::string& token, std::wstring& err) {
    std::lock_guard<std::mutex> lock(g_tokenMx);
    if (!g_token.access.empty() && static_cast<long long>(GetTickCount64()) < g_token.expires) {
        token = g_token.access;
        return true;
    }
    const std::wstring refresh = Upload_LoadSecret(L"DriveRefreshToken");
    if (refresh.empty()) {
        err = L"Google Drive isn't connected yet: Settings → Uploads → Connect.";
        g_driveReconnect = true;
        return false;
    }
    HttpReq q;
    q.method = L"POST";
    q.url = Endpoint(kTokenUrl);
    q.headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    q.body = Form({{"client_id", Utf8(s.driveClientId)},
                   {"client_secret", Utf8(Upload_LoadSecret(L"DriveClientSecret"))},
                   {"refresh_token", Utf8(refresh)},
                   {"grant_type", "refresh_token"}});
    HttpResp a;
    if (!Http(q, a) || a.status != 200 || JsonString(a.body, "access_token").empty()) {
        err = TokenError(a);
        g_driveReconnect = JsonString(a.body, "error") == "invalid_grant";
        return false;
    }
    g_token.access = JsonString(a.body, "access_token");
    const long long secs = std::max(60LL, JsonNumber(a.body, "expires_in"));
    g_token.expires = static_cast<long long>(GetTickCount64()) + (secs - 60) * 1000;
    token = g_token.access;
    return true;
}

std::wstring DriveError(const HttpResp& a, const wchar_t* what) {
    if (!a.status) return a.error;
    const std::string msg = JsonString(a.body, "message");
    if (a.status == 401) return L"Google Drive refused the sign-in. Connect again in Settings → Uploads.";
    if (a.status == 403 && a.body.find("storageQuotaExceeded") != std::string::npos)
        return L"Your Google Drive is full.";
    if (a.status == 403 && (a.body.find("accessNotConfigured") != std::string::npos ||
                            a.body.find("SERVICE_DISABLED") != std::string::npos))
        return L"The Google Drive API is turned off in your Google Cloud project: APIs & Services → Library → "
               L"Google Drive API → Enable.";
    return std::wstring(what) + L": Google Drive answered " + std::to_wstring(a.status) +
           (msg.empty() ? L"." : L" (" + Wide(msg) + L").");
}

std::wstring StatePath() { return AppDataDir() + L"\\state.ini"; }

// The Drive folder uploads go into (made the first time). "" = My Drive itself.
bool DriveFolder(const Settings& s, const std::string& token, bool fresh, std::string& id, std::wstring& err) {
    id.clear();
    const std::wstring name = TrimW(s.driveFolder);
    if (name.empty()) return true;
    wchar_t cached[256] = L"", cachedName[512] = L"";
    GetPrivateProfileStringW(L"Upload", L"DriveFolderId", L"", cached, 256, StatePath().c_str());
    GetPrivateProfileStringW(L"Upload", L"DriveFolderName", L"", cachedName, 512, StatePath().c_str());
    if (!fresh && *cached && name == cachedName) {
        id = Utf8(cached);
        return true;
    }
    const std::string auth = "Authorization: Bearer " + token + "\r\n";
    // drive.file only lists folders Canivete made, so a folder of the same name made by hand is not
    // found and a new one is made next to it.
    HttpReq q;
    const std::string search = "name='" + JsonEscape(Utf8(name)) +
                               "' and mimeType='application/vnd.google-apps.folder' and trashed=false";
    q.url = Endpoint(kDriveApi) + L"/files?spaces=drive&fields=files(id)&q=" + Wide(UriEncode(search, false));
    q.headers = Wide(auth);
    HttpResp a;
    if (!Http(q, a) || a.status != 200) {
        err = DriveError(a, L"Finding the folder");
        return false;
    }
    id = JsonString(a.body, "id");
    if (id.empty()) {
        HttpReq c;
        c.method = L"POST";
        c.url = Endpoint(kDriveApi) + L"/files?fields=id";
        c.headers = Wide(auth) + L"Content-Type: application/json; charset=UTF-8\r\n";
        c.body = "{\"name\":\"" + JsonEscape(Utf8(name)) + "\",\"mimeType\":\"application/vnd.google-apps.folder\"}";
        if (!Http(c, a) || (a.status != 200 && a.status != 201) || JsonString(a.body, "id").empty()) {
            err = DriveError(a, L"Making the folder");
            return false;
        }
        id = JsonString(a.body, "id");
        Log(L"Drive: made folder \"%s\"", name.c_str());
    }
    WritePrivateProfileStringW(L"Upload", L"DriveFolderId", Wide(id).c_str(), StatePath().c_str());
    WritePrivateProfileStringW(L"Upload", L"DriveFolderName", name.c_str(), StatePath().c_str());
    return true;
}

struct Job {
    std::wstring path;
    bool deleteAfter = false;
    Settings s;  // a copy made on the main thread
};

struct Result {
    std::wstring path, name, link, error, service, note;
    uint64_t bytes = 0;
    bool ok = false, deleteAfter = false, reconnect = false;
};

using Progress = std::function<void(uint64_t sent)>;

bool DriveUpload(const Job& j, HANDLE f, uint64_t size, const Progress& progress, Result& res) {
    std::string token;
    if (!DriveAccessToken(j.s, token, res.error)) {
        res.reconnect = g_driveReconnect;
        return false;
    }
    std::string folder;
    if (!DriveFolder(j.s, token, false, folder, res.error)) return false;
    const std::string auth = "Authorization: Bearer " + token + "\r\n";
    const std::string mime = MimeOf(j.path);

    // 1. Start a resumable session: Google answers with the address to send the file to.
    HttpResp a;
    for (int attempt = 0; attempt < 2; ++attempt) {
        HttpReq q;
        q.method = L"POST";
        q.url = Endpoint(kDriveUpload) + L"?uploadType=resumable&fields=id,webViewLink";
        q.headers = Wide(auth) + L"Content-Type: application/json; charset=UTF-8\r\nX-Upload-Content-Type: " +
                    Wide(mime) + L"\r\nX-Upload-Content-Length: " + std::to_wstring(size) + L"\r\n";
        q.body = "{\"name\":\"" + JsonEscape(Utf8(res.name)) + "\"" +
                 (folder.empty() ? std::string() : ",\"parents\":[\"" + folder + "\"]") + "}";
        if (!Http(q, a)) break;
        // The folder was deleted since: find or make it again, once.
        if (a.status == 404 && !folder.empty() && attempt == 0) {
            if (!DriveFolder(j.s, token, true, folder, res.error)) return false;
            continue;
        }
        break;
    }
    if (a.status != 200 || a.location.empty()) {
        res.error = DriveError(a, L"Starting the upload");
        res.reconnect = a.status == 401;
        return false;
    }
    const std::wstring session = a.location;

    // 2. Send it in pieces. After a broken connection, ask Google how much it has and go on from there.
    uint64_t offset = 0;
    int failures = 0;
    std::string body;
    bool finished = false;
    while (!finished) {
        HttpReq q;
        q.method = L"PUT";
        q.url = session;
        const uint64_t n = std::min(kChunk, size - offset);
        if (failures == 0 || offset >= size) {
            q.file = f;
            q.offset = offset;
            q.length = n;
            q.headers = L"Content-Range: bytes " + std::to_wstring(offset) + L"-" + std::to_wstring(offset + n - 1) +
                        L"/" + std::to_wstring(size) + L"\r\n";
            const uint64_t base = offset;
            q.progress = [&progress, base](uint64_t sent) { progress(base + sent); };
        } else {
            q.headers = L"Content-Range: bytes */" + std::to_wstring(size) + L"\r\n";  // "how much do you have?"
        }
        HttpResp r;
        const bool answered = Http(q, r);
        if (answered && (r.status == 200 || r.status == 201)) {
            body = r.body;
            finished = true;
        } else if (answered && r.status == 308) {
            // "Range: bytes=0-N": the first N+1 bytes are there.
            const size_t dash = r.range.find(L'-');
            offset = dash == std::wstring::npos ? 0 : _wtoi64(r.range.c_str() + dash + 1) + 1;
            failures = 0;
        } else if (answered && r.status == 404) {
            res.error = L"Google Drive dropped the upload (it took too long). Try again.";
            return false;
        } else if ((!answered || r.status >= 500 || r.status == 429) && ++failures <= 5) {
            Log(L"Drive upload: piece at %llu failed (%s), retrying", offset,
                answered ? std::to_wstring(r.status).c_str() : r.error.c_str());
            Sleep(1000u << std::min(failures, 4));
        } else {
            res.error = DriveError(r, L"Sending the file");
            return false;
        }
    }
    const std::string id = JsonString(body, "id");
    std::string link = JsonString(body, "webViewLink");
    if (id.empty()) {
        res.error = L"Google Drive didn't say where the file went.";
        return false;
    }
    if (link.empty()) link = "https://drive.google.com/file/d/" + id + "/view";

    // 3. Anyone with the link can view it.
    if (j.s.driveShare) {
        HttpReq q;
        q.method = L"POST";
        q.url = Endpoint(kDriveApi) + L"/files/" + Wide(id) + L"/permissions?fields=id";
        q.headers = Wide(auth) + L"Content-Type: application/json; charset=UTF-8\r\n";
        q.body = "{\"role\":\"reader\",\"type\":\"anyone\"}";
        HttpResp p;
        if (!Http(q, p) || p.status != 200) {
            Log(L"Drive: sharing failed: %s", DriveError(p, L"Sharing").c_str());
            res.note = L"Not shared: only you can open the link.";
        }
    }
    res.link = Wide(link);
    return true;
}

// ---- Amazon S3 -------------------------------------------------------------------------------

struct S3Place {
    bool secure = true;
    std::string host;  // what goes in the Host header (and the signature)
    std::string uri;   // canonical (already encoded) path
    std::string key;
};

std::string AmzDate(const SYSTEMTIME& t) {
    char b[32];
    sprintf_s(b, "%04d%02d%02dT%02d%02d%02dZ", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return b;
}

S3Place S3Where(const Settings& s, const std::string& key) {
    S3Place p;
    p.key = key;
    const std::string bucket = Utf8(TrimW(s.s3Bucket)), region = Utf8(TrimW(s.s3Region));
    if (!MockBase().empty()) {
        const std::wstring& m = MockBase();
        p.secure = false;
        p.host = Utf8(m.substr(m.find(L"://") + 3));
        p.uri = "/" + bucket + "/" + UriEncode(key, true);
    } else if (bucket.find('.') == std::string::npos) {
        p.host = bucket + ".s3." + region + ".amazonaws.com";
        p.uri = "/" + UriEncode(key, true);
    } else {  // dots in the name break the certificate of the bucket's own host name
        p.host = "s3." + region + ".amazonaws.com";
        p.uri = "/" + bucket + "/" + UriEncode(key, true);
    }
    return p;
}

std::string SigningKey(const std::string& secret, const std::string& date, const std::string& region,
                       const std::string& service) {
    return Hmac(Hmac(Hmac(Hmac("AWS4" + secret, date), region), service), "aws4_request");
}

// headers: lowercase name -> value. Returns the canonical request.
std::string CanonicalRequest(const std::string& method, const std::string& uri, const std::string& query,
                             const std::map<std::string, std::string>& headers, const std::string& payload,
                             std::string& signedHeaders) {
    std::string canon = method + "\n" + uri + "\n" + query + "\n";
    signedHeaders.clear();
    for (const auto& [k, v] : headers) {
        canon += k + ":" + v + "\n";
        signedHeaders += (signedHeaders.empty() ? "" : ";") + k;
    }
    return canon + "\n" + signedHeaders + "\n" + payload;
}

std::string Signature(const std::string& secret, const std::string& amzDate, const std::string& region,
                      const std::string& canonical) {
    const std::string date = amzDate.substr(0, 8);
    const std::string scope = date + "/" + region + "/s3/aws4_request";
    const std::string toSign = "AWS4-HMAC-SHA256\n" + amzDate + "\n" + scope + "\n" + HexOf(Sha256(canonical));
    return HexOf(Hmac(SigningKey(secret, date, region, "s3"), toSign));
}

// A GET link anyone can open for 7 days, signed with the same key.
std::string S3Presign(const S3Place& p, const std::string& keyId, const std::string& secret,
                      const std::string& region, const std::string& amzDate, int seconds) {
    const std::string scope = amzDate.substr(0, 8) + "/" + region + "/s3/aws4_request";
    const std::string query = "X-Amz-Algorithm=AWS4-HMAC-SHA256&X-Amz-Credential=" + UriEncode(keyId + "/" + scope, false) +
                              "&X-Amz-Date=" + amzDate + "&X-Amz-Expires=" + std::to_string(seconds) +
                              "&X-Amz-SignedHeaders=host";
    std::string signedHeaders;
    const std::string canon = CanonicalRequest("GET", p.uri, query, {{"host", p.host}}, "UNSIGNED-PAYLOAD", signedHeaders);
    return query + "&X-Amz-Signature=" + Signature(secret, amzDate, region, canon);
}

std::wstring S3Error(const HttpResp& a, const Settings& s) {
    if (!a.status) return a.error;
    const std::string code = XmlTag(a.body, "Code");
    if (code == "AccessDenied")
        return L"AWS refused: this access key isn't allowed to write there (AccessDenied). Check the user's policy "
               L"allows s3:PutObject on " + TrimW(s.s3Bucket) + L"/" + TrimW(s.s3Prefix) + L"*.";
    if (code == "SignatureDoesNotMatch") return L"AWS refused the secret access key (it doesn't match the key ID).";
    if (code == "InvalidAccessKeyId") return L"AWS doesn't know this access key ID.";
    if (code == "NoSuchBucket") return L"There is no bucket called " + TrimW(s.s3Bucket) + L".";
    if (code == "AuthorizationHeaderMalformed" || code == "PermanentRedirect" || a.status == 301) {
        const std::string region = XmlTag(a.body, "Region");
        return L"The bucket is in another region" + (region.empty() ? std::wstring() : L" (" + Wide(region) + L")") +
               L": fix Region in Settings → Uploads.";
    }
    const std::string msg = XmlTag(a.body, "Message");
    return L"AWS answered " + std::to_wstring(a.status) + (code.empty() ? L"" : L" (" + Wide(code) + L")") +
           (msg.empty() ? L"." : L": " + Wide(msg));
}

std::string S3Key(const Settings& s, const std::wstring& name) {
    std::string prefix = Utf8(TrimW(s.s3Prefix));
    for (char& c : prefix)
        if (c == '\\') c = '/';
    while (!prefix.empty() && prefix.front() == '/') prefix.erase(0, 1);
    if (!prefix.empty() && prefix.back() != '/') prefix += '/';
    std::string file = Utf8(name);
    for (char& c : file)
        if (c == ' ') c = '-';  // cleaner links
    return prefix + file;
}

bool S3Upload(const Job& j, HANDLE f, uint64_t size, const Progress& progress, Result& res,
              const std::string* text = nullptr) {
    const Settings& s = j.s;
    const std::string keyId = Utf8(TrimW(s.s3KeyId)), region = Utf8(TrimW(s.s3Region));
    const std::string secret = Utf8(Upload_LoadSecret(L"S3SecretKey"));
    if (TrimW(s.s3Bucket).empty() || region.empty() || keyId.empty() || secret.empty()) {
        res.error = L"Amazon S3 isn't set up yet: Settings → Uploads needs the bucket, region and access key.";
        return false;
    }
    if (size > (5ull << 30)) {
        res.error = L"Amazon S3 takes up to 5 GB in one upload; this file is bigger.";
        return false;
    }
    const S3Place p = S3Where(s, S3Key(s, res.name));
    SYSTEMTIME t;
    GetSystemTime(&t);
    const std::string amzDate = AmzDate(t), mime = MimeOf(j.path);
    const std::map<std::string, std::string> headers = {
        {"content-type", mime}, {"host", p.host}, {"x-amz-content-sha256", "UNSIGNED-PAYLOAD"}, {"x-amz-date", amzDate}};
    std::string signedHeaders;
    const std::string canon = CanonicalRequest("PUT", p.uri, "", headers, "UNSIGNED-PAYLOAD", signedHeaders);
    const std::string auth = "AWS4-HMAC-SHA256 Credential=" + keyId + "/" + amzDate.substr(0, 8) + "/" + region +
                             "/s3/aws4_request, SignedHeaders=" + signedHeaders +
                             ", Signature=" + Signature(secret, amzDate, region, canon);
    HttpReq q;
    q.method = L"PUT";
    q.url = Wide((p.secure ? "https://" : "http://") + p.host + p.uri);
    q.headers = Wide("Content-Type: " + mime + "\r\nx-amz-content-sha256: UNSIGNED-PAYLOAD\r\nx-amz-date: " + amzDate +
                     "\r\nAuthorization: " + auth + "\r\n");
    if (text) {
        q.body = *text;
    } else {
        q.file = f;
        q.length = size;
        q.progress = progress;
    }
    HttpResp a;
    if (!Http(q, a) || a.status != 200) {
        res.error = S3Error(a, s);
        return false;
    }
    const std::string base = Utf8(TrimW(s.s3LinkBase));
    if (s.s3Private) {
        res.link = Wide((p.secure ? "https://" : "http://") + p.host + p.uri + "?" +
                        S3Presign(p, keyId, secret, region, amzDate, kPresignSeconds));
    } else if (!base.empty()) {
        std::string b = base;
        while (!b.empty() && b.back() == '/') b.pop_back();
        if (b.find("://") == std::string::npos) b = "https://" + b;
        res.link = Wide(b + "/" + UriEncode(p.key, true));
    } else {
        res.link = Wide((p.secure ? "https://" : "http://") + p.host + p.uri);
    }
    return true;
}

// ---- the upload queue ------------------------------------------------------------------------

std::mutex g_queueMx;
std::deque<Job> g_queue;
bool g_working = false;
int g_lastPercent = -1;

std::wstring ServiceName(const std::wstring& key) { return key == L"s3" ? L"Amazon S3" : L"Google Drive"; }

void Post(WPARAM what, LPARAM lp) {
    if (!PostMessageW(g_mainWnd, WM_APP_UPLOAD, what, lp)) {
        if (what == kDone || what == kStarted || what == kTested) delete reinterpret_cast<Result*>(lp);
        if (what == kConnected) delete reinterpret_cast<std::wstring*>(lp);
    }
}

Result RunJob(const Job& j) {
    Result res;
    res.path = j.path;
    res.name = FileName(j.path);
    res.deleteAfter = j.deleteAfter;
    res.service = ServiceName(j.s.uploadService);
    HANDLE f = CreateFileW(j.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    LARGE_INTEGER sz{};
    if (f == INVALID_HANDLE_VALUE || !GetFileSizeEx(f, &sz)) {
        res.error = L"Couldn't open " + res.name + L".";
        if (f != INVALID_HANDLE_VALUE) CloseHandle(f);
        return res;
    }
    res.bytes = static_cast<uint64_t>(sz.QuadPart);
    if (!res.bytes) {
        res.error = res.name + L" is empty.";
        CloseHandle(f);
        return res;
    }
    auto* started = new Result(res);
    Post(kStarted, reinterpret_cast<LPARAM>(started));
    const uint64_t total = res.bytes;
    int last = -1;
    const Progress progress = [&last, total](uint64_t sent) {
        const int pct = static_cast<int>(std::min<uint64_t>(100, sent * 100 / total));
        if (pct != last) {
            last = pct;
            Post(kProgress, pct);
        }
    };
    const ULONGLONG t0 = GetTickCount64();
    res.ok = j.s.uploadService == L"s3" ? S3Upload(j, f, total, progress, res) : DriveUpload(j, f, total, progress, res);
    CloseHandle(f);
    Log(L"Upload %s to %s: %s in %llu ms%s%s", res.name.c_str(), res.service.c_str(), SizeText(res.bytes).c_str(),
        GetTickCount64() - t0, res.ok ? L" -> " : L", failed: ", res.ok ? LogLink(res.link).c_str() : res.error.c_str());
    return res;
}

void Worker() {
    for (;;) {
        Job j;
        {
            std::lock_guard<std::mutex> lock(g_queueMx);
            if (g_queue.empty()) {
                g_working = false;
                return;
            }
            j = std::move(g_queue.front());
            g_queue.pop_front();
        }
        Post(kDone, reinterpret_cast<LPARAM>(new Result(RunJob(j))));
    }
}

void SetClipboardText(const std::wstring& text) {
    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(g_mainWnd)); ++i) Sleep(15);
    if (!open) return;
    EmptyClipboard();
    const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        memcpy(GlobalLock(g), text.c_str(), bytes);
        GlobalUnlock(g);
        if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
    }
    CloseClipboard();
}

void OpenUrl(const std::wstring& url) {
    if (TestMode()) {
        Log(L"Would open %s", LogLink(url).c_str());
        return;
    }
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// ---- Google sign-in (loopback + PKCE) ----------------------------------------------------------

std::atomic<bool> g_connecting{false};

// Waits for the browser to come back to http://127.0.0.1:port/?code=...&state=...
// Returns the query string ("" on timeout); answers the browser with a short page.
std::string WaitForRedirect(SOCKET ls, const std::string& state, std::wstring& err) {
    const ULONGLONG until = GetTickCount64() + 5 * 60 * 1000;
    while (GetTickCount64() < until) {
        fd_set set;
        FD_ZERO(&set);
        FD_SET(ls, &set);
        timeval tv{1, 0};
        if (select(0, &set, nullptr, nullptr, &tv) <= 0) continue;
        SOCKET c = accept(ls, nullptr, nullptr);
        if (c == INVALID_SOCKET) continue;
        DWORD timeout = 5000;
        setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&timeout), sizeof(timeout));
        std::string req;
        char buf[4096];
        while (req.find("\r\n\r\n") == std::string::npos && req.size() < 65536) {
            const int n = recv(c, buf, sizeof(buf), 0);
            if (n <= 0) break;
            req.append(buf, n);
        }
        // "GET /?code=...&state=... HTTP/1.1"
        std::string query;
        if (req.rfind("GET /", 0) == 0) {
            const size_t q = req.find('?'), sp = req.find(' ', 4);
            if (q != std::string::npos && sp != std::string::npos && q < sp) query = req.substr(q + 1, sp - q - 1);
        }
        auto param = [&](const char* name) {
            const std::string k = std::string(name) + "=";
            for (size_t p = 0; p < query.size();) {
                size_t e = query.find('&', p);
                if (e == std::string::npos) e = query.size();
                if (query.compare(p, k.size(), k) == 0) return UrlDecode(query.substr(p + k.size(), e - p - k.size()));
                p = e + 1;
            }
            return std::string();
        };
        const bool ours = param("state") == state && (!param("code").empty() || !param("error").empty());
        std::string page;
        if (!ours) {
            page = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
        } else {
            const bool good = !param("code").empty();
            const std::string html =
                std::string("<!doctype html><meta charset=utf-8><title>Canivete</title><body style=\"font:16px "
                            "'Segoe UI',sans-serif;margin:60px\"><h2>") +
                (good ? "Canivete is connected to Google Drive." : "Canivete was not connected.") +
                "</h2><p>You can close this tab.</p>";
            page = "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: " +
                   std::to_string(html.size()) + "\r\nConnection: close\r\n\r\n" + html;
        }
        send(c, page.data(), static_cast<int>(page.size()), 0);
        shutdown(c, SD_SEND);
        closesocket(c);
        if (!ours) continue;
        if (!param("error").empty()) {
            err = param("error") == "access_denied" ? L"You didn't allow access in the browser."
                                                    : L"Google said: " + Wide(param("error"));
            return {};
        }
        return param("code");
    }
    err = L"No answer from the browser within 5 minutes.";
    return {};
}

std::wstring Connect(const std::wstring& clientId, const std::wstring& clientSecret) {
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa)) return L"Couldn't start networking.";
    SOCKET ls = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int alen = sizeof(addr);
    if (ls == INVALID_SOCKET || bind(ls, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) ||
        listen(ls, 4) || getsockname(ls, reinterpret_cast<sockaddr*>(&addr), &alen)) {
        if (ls != INVALID_SOCKET) closesocket(ls);
        WSACleanup();
        return L"Couldn't open a port on this PC for Google's answer.";
    }
    const std::string redirect = "http://127.0.0.1:" + std::to_string(ntohs(addr.sin_port));
    const std::string verifier = Base64Url(RandomBytes(48));  // 64 characters
    const std::string challenge = Base64Url(Sha256(verifier));
    const std::string state = Base64Url(RandomBytes(16));
    const std::wstring url = Endpoint(kAuthUrl) + L"?" +
                             Wide(Form({{"client_id", Utf8(clientId)},
                                        {"redirect_uri", redirect},
                                        {"response_type", "code"},
                                        {"scope", kDriveScope},
                                        {"code_challenge", challenge},
                                        {"code_challenge_method", "S256"},
                                        {"state", state},
                                        {"access_type", "offline"},
                                        {"prompt", "consent"}}));
    if (TestMode()) Log(L"Drive sign-in address: %s", url.c_str());
    else {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        CoUninitialize();
    }
    std::wstring err;
    const std::string code = WaitForRedirect(ls, state, err);
    closesocket(ls);
    WSACleanup();
    if (code.empty()) return err;

    HttpReq q;
    q.method = L"POST";
    q.url = Endpoint(kTokenUrl);
    q.headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
    q.body = Form({{"code", code},
                   {"client_id", Utf8(clientId)},
                   {"client_secret", Utf8(clientSecret)},
                   {"redirect_uri", redirect},
                   {"grant_type", "authorization_code"},
                   {"code_verifier", verifier}});
    HttpResp a;
    if (!Http(q, a) || a.status != 200) return TokenError(a);
    const std::string refresh = JsonString(a.body, "refresh_token"), access = JsonString(a.body, "access_token");
    if (refresh.empty() || access.empty())
        return L"Google gave no lasting sign-in. Remove Canivete at myaccount.google.com/permissions and connect again.";
    Upload_SaveSecret(L"DriveRefreshToken", Wide(refresh));
    {
        std::lock_guard<std::mutex> lock(g_tokenMx);
        g_token.access = access;
        g_token.expires = static_cast<long long>(GetTickCount64()) +
                          (std::max(60LL, JsonNumber(a.body, "expires_in")) - 60) * 1000;
    }
    g_driveReconnect = false;
    // Which account it is, to show in Settings.
    HttpReq u;
    u.url = Endpoint(kDriveApi) + L"/about?fields=user(emailAddress)";
    u.headers = Wide("Authorization: Bearer " + access + "\r\n");
    HttpResp ua;
    std::wstring email;
    if (Http(u, ua) && ua.status == 200) email = Wide(JsonString(ua.body, "emailAddress"));
    if (ua.status == 403) Log(L"Drive about: %s", DriveError(ua, L"Account").c_str());
    Upload_SaveSecret(L"DriveAccount", email.empty() ? L"(account)" : email);
    WritePrivateProfileStringW(L"Upload", L"DriveFolderId", nullptr, StatePath().c_str());  // new account, new folder
    return {};
}

}  // namespace

// ---- public ----------------------------------------------------------------------------------

std::wstring Upload_LoadSecret(const wchar_t* name) {
    std::lock_guard<std::mutex> lock(g_secretMx);
    wchar_t b64[8192] = L"";
    GetPrivateProfileStringW(L"Secrets", name, L"", b64, 8192, SecretsPath().c_str());
    if (!*b64) return {};
    DWORD len = 0;
    if (!CryptStringToBinaryW(b64, 0, CRYPT_STRING_BASE64, nullptr, &len, nullptr, nullptr)) return {};
    std::string raw(len, '\0');
    CryptStringToBinaryW(b64, 0, CRYPT_STRING_BASE64, reinterpret_cast<BYTE*>(raw.data()), &len, nullptr, nullptr);
    DATA_BLOB in{len, reinterpret_cast<BYTE*>(raw.data())}, out{}, ent = Entropy();
    if (!CryptUnprotectData(&in, nullptr, &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        Log(L"Upload: couldn't decrypt %s (another Windows user or PC?)", name);
        return {};
    }
    const std::wstring v = Wide(std::string(reinterpret_cast<char*>(out.pbData), out.cbData));
    SecureZeroMemory(out.pbData, out.cbData);
    LocalFree(out.pbData);
    return v;
}

void Upload_SaveSecret(const wchar_t* name, const std::wstring& value) {
    std::lock_guard<std::mutex> lock(g_secretMx);
    if (value.empty()) {
        WritePrivateProfileStringW(L"Secrets", name, nullptr, SecretsPath().c_str());
        return;
    }
    std::string plain = Utf8(value);
    DATA_BLOB in{static_cast<DWORD>(plain.size()), reinterpret_cast<BYTE*>(plain.data())}, out{}, ent = Entropy();
    if (!CryptProtectData(&in, L"Canivete", &ent, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &out)) {
        Log(L"Upload: couldn't encrypt %s (%lu)", name, GetLastError());
        return;
    }
    const std::string b64 = Base64(out.pbData, out.cbData);
    LocalFree(out.pbData);
    SecureZeroMemory(plain.data(), plain.size());
    WritePrivateProfileStringW(L"Secrets", name, Wide(b64).c_str(), SecretsPath().c_str());
}

bool Upload_Ready() {
    if (!FeatureOn(kFeatUpload)) return false;
    const Settings& s = g_settings;
    if (s.uploadService == L"s3")
        return !TrimW(s.s3Bucket).empty() && !TrimW(s.s3Region).empty() && !TrimW(s.s3KeyId).empty();
    return !TrimW(s.driveClientId).empty() && !Upload_DriveAccount().empty();
}

std::wstring Upload_ServiceName() { return ServiceName(g_settings.uploadService); }

std::wstring Upload_DriveAccount() { return Upload_LoadSecret(L"DriveAccount"); }

bool Upload_Auto(UploadKind k) {
    const Settings& s = g_settings;
    return Upload_Ready() && (k == UploadKind::Screenshot ? s.uploadShots : k == UploadKind::Gif ? s.uploadGif : s.uploadVideo);
}

void Upload_File(const std::wstring& path, bool deleteAfter) {
    if (!Upload_Ready()) {
        Tray_NotifyAction(L"Uploads aren't set up",
                          FeatureOn(kFeatUpload) ? L"Click here to set up " + Upload_ServiceName() + L" in Settings."
                                                 : std::wstring(L"Uploads are turned off. Click here for Settings."),
                          [] { SettingsUI_Show(kSettingsTabUploads); });
        if (deleteAfter) DeleteFileW(path.c_str());
        return;
    }
    std::lock_guard<std::mutex> lock(g_queueMx);
    g_queue.push_back({path, deleteAfter, g_settings});
    if (!g_working) {
        g_working = true;
        std::thread(Worker).detach();
    }
}

void Upload_Image(const Image& img, const CaptureResult& /*meta*/) {
    // Not saved to a file: a PNG in the temp folder, removed after the upload.
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"Canivete";
    CreateDirectoryW(dir.c_str(), nullptr);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf_s(stamp, L"Canivete %04u-%02u-%02u %02u.%02u.%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    std::wstring path = dir + L"\\" + stamp + L".png";
    for (int n = 2; GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; ++n)
        path = dir + L"\\" + stamp + L" (" + std::to_wstring(n) + L").png";
    if (!WriteFileBytes(path, EncodePng(img))) {
        Tray_Notify(L"Upload failed", L"Couldn't make the picture to upload.");
        return;
    }
    Upload_File(path, true);
}

void Upload_DriveConnect() {
    if (g_connecting.exchange(true)) return;
    const std::wstring id = TrimW(g_settings.driveClientId), secret = Upload_LoadSecret(L"DriveClientSecret");
    std::thread([id, secret] {
        const std::wstring err = Connect(id, secret);
        Log(L"Drive connect: %s", err.empty() ? L"ok" : err.c_str());
        g_connecting = false;
        Post(kConnected, reinterpret_cast<LPARAM>(new std::wstring(err)));
    }).detach();
}

bool Upload_DriveConnecting() { return g_connecting; }

// --uninstall: forget the Google sign-in (here and at Google) and the saved keys.
void Upload_Uninstall() {
    const std::wstring refresh = Upload_LoadSecret(L"DriveRefreshToken");
    if (!refresh.empty()) {
        HttpReq q;
        q.method = L"POST";
        q.url = Endpoint(kRevokeUrl);
        q.headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
        q.body = Form({{"token", Utf8(refresh)}});
        HttpResp a;
        Http(q, a);
        Log(L"Uninstall: Drive sign-in revoked: %lu", a.status);
    }
    DeleteFileW(SecretsPath().c_str());
}

void Upload_DriveDisconnect() {
    const std::wstring refresh = Upload_LoadSecret(L"DriveRefreshToken");
    Upload_SaveSecret(L"DriveRefreshToken", L"");
    Upload_SaveSecret(L"DriveAccount", L"");
    WritePrivateProfileStringW(L"Upload", L"DriveFolderId", nullptr, StatePath().c_str());
    {
        std::lock_guard<std::mutex> lock(g_tokenMx);
        g_token = {};
    }
    Log(L"Drive disconnected");
    if (refresh.empty()) return;
    std::thread([refresh] {  // also tell Google, so the sign-in stops working there too
        HttpReq q;
        q.method = L"POST";
        q.url = Endpoint(kRevokeUrl);
        q.headers = L"Content-Type: application/x-www-form-urlencoded\r\n";
        q.body = Form({{"token", Utf8(refresh)}});
        HttpResp a;
        Http(q, a);
        Log(L"Drive sign-in revoked: %lu", a.status);
    }).detach();
}

void Upload_S3Test() {
    Job j{L"canivete-connection-test.txt", false, g_settings};
    j.s.uploadService = L"s3";
    std::thread([j] {
        Result res;
        res.name = j.path;
        res.service = L"Amazon S3";
        const std::string text = "Canivete connection test. You can delete this file.\n";
        res.ok = S3Upload(j, INVALID_HANDLE_VALUE, text.size(), nullptr, res, &text);
        Log(L"S3 test: %s", res.ok ? LogLink(res.link).c_str() : res.error.c_str());
        Post(kTested, reinterpret_cast<LPARAM>(new Result(res)));
    }).detach();
}

void Upload_OnMessage(WPARAM wp, LPARAM lp) {
    switch (wp) {
    case kStarted: {
        std::unique_ptr<Result> r(reinterpret_cast<Result*>(lp));
        g_lastPercent = -1;
        Tray_Notify((L"Uploading to " + r->service).c_str(), r->name + L"  (" + SizeText(r->bytes) + L")…");
        Tray_SetTip(L"Canivete: uploading " + r->name);
        return;
    }
    case kProgress:
        if (static_cast<int>(lp) / 5 != g_lastPercent / 5) {
            g_lastPercent = static_cast<int>(lp);
            Tray_SetTip(L"Canivete: uploading, " + std::to_wstring(lp) + L" %");
        }
        return;
    case kDone: {
        std::unique_ptr<Result> r(reinterpret_cast<Result*>(lp));
        if (r->deleteAfter) DeleteFileW(r->path.c_str());
        {
            std::lock_guard<std::mutex> lock(g_queueMx);
            if (!g_working) Tray_SetTip(L"");
        }
        if (r->ok) {
            if (TestMode()) Log(L"Link (would be copied): %s", LogLink(r->link).c_str());
            else SetClipboardText(r->link);
            const std::wstring link = r->link;
            Tray_NotifyAction(L"Link copied", r->name + L" is on " + r->service + L". " +
                                                  (r->note.empty() ? L"Click here to open it." : r->note),
                              [link] { OpenUrl(link); });
        } else {
            Tray_NotifyAction(L"Upload failed", r->name + L": " + r->error,
                              [] { SettingsUI_Show(kSettingsTabUploads); });
        }
        return;
    }
    case kConnected: {
        std::unique_ptr<std::wstring> err(reinterpret_cast<std::wstring*>(lp));
        if (err->empty())
            Tray_Notify(L"Google Drive connected", L"Uploads go to " + Upload_DriveAccount() + L".");
        else
            Tray_Notify(L"Google Drive not connected", *err);
        SettingsUI_UploadStatus(*err);
        return;
    }
    case kTested: {
        std::unique_ptr<Result> r(reinterpret_cast<Result*>(lp));
        SettingsUI_UploadStatus(r->ok ? L"" : r->error);
        if (r->ok) Tray_NotifyAction(L"Amazon S3 works", L"A test file went up. Click here to open its link.",
                                     [link = r->link] { OpenUrl(link); });
        else Tray_Notify(L"Amazon S3 test failed", r->error);
        return;
    }
    }
}

// --upload-selftest: the signing code against AWS's published examples, plus the helpers. 0 = fine.
int Upload_SelfTest() {
    int bad = 0;
    auto check = [&](const char* what, const std::string& got, const std::string& want) {
        const bool ok = got == want;
        if (!ok) ++bad;
        Log(L"--upload-selftest %s: %s%s%s", Wide(what).c_str(), ok ? L"ok" : L"WRONG ", ok ? L"" : Wide(got).c_str(),
            ok ? L"" : (L" (want " + Wide(want) + L")").c_str());
    };
    const std::string secret = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY";
    // "GET Object" example from the S3 Signature V4 documentation.
    std::string sh;
    const std::string get = CanonicalRequest(
        "GET", "/test.txt", "",
        {{"host", "examplebucket.s3.amazonaws.com"},
         {"range", "bytes=0-9"},
         {"x-amz-content-sha256", "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
         {"x-amz-date", "20130524T000000Z"}},
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", sh);
    check("S3 GET signature", Signature(secret, "20130524T000000Z", "us-east-1", get),
          "f0e8bdb87c964420e857bd35b5d6ed310bd44f0170aba48dd91039c6036bdb41");
    // Presigned URL example from the same documentation (24 hours).
    S3Place p;
    p.host = "examplebucket.s3.amazonaws.com";
    p.uri = "/test.txt";
    const std::string q = S3Presign(p, "AKIAIOSFODNN7EXAMPLE", secret, "us-east-1", "20130524T000000Z", 86400);
    check("S3 presigned signature", q.substr(q.rfind('=') + 1),
          "aeeed9bbccd4d02ee5c0109b86d86835f995330da4c265957d157751f604d404");
    // PKCE example from RFC 7636, appendix B.
    check("PKCE S256", Base64Url(Sha256("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk")),
          "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");
    check("URI encoding", UriEncode("canivete/2026 shot (1)+é.png", true), "canivete/2026%20shot%20%281%29%2B%C3%A9.png");
    check("JSON", JsonString("{\"id\" : \"a\\\"b\", \"webViewLink\":\"x\"}", "id"), "a\"b");
    // DPAPI round trip.
    Upload_SaveSecret(L"SelfTest", L"sécret ✓");
    check("DPAPI", Utf8(Upload_LoadSecret(L"SelfTest")), Utf8(L"sécret ✓"));
    Upload_SaveSecret(L"SelfTest", L"");
    return bad;
}
