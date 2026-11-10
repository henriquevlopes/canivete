// Settings (INI file in %APPDATA%\Canivete), hotkey text parsing and a tiny log.
#include "app.h"

#include <shlobj.h>

#include <cstdarg>
#include <cwctype>

Settings g_settings;

namespace {

const wchar_t kDefaultIni[] =
    L"; Canivete settings. Changes apply as soon as you save this file.\r\n"
    L";\r\n"
    L"; Hotkeys: Ctrl, Alt, Shift, Win plus one key, e.g. Ctrl+Shift+S, PrintScreen, F9.\r\n"
    L"; Leave a hotkey empty to turn it off.\r\n"
    L"[Hotkeys]\r\n"
    L"RegionScreenshot=Ctrl+PrintScreen\r\n"
    L"WindowScreenshot=Alt+PrintScreen\r\n"
    L"FullScreenshot=PrintScreen\r\n"
    L"\r\n"
    L"[Screenshot]\r\n"
    L"; Folder: empty = your Windows Screenshots folder\r\n"
    L"Folder=\r\n"
    L"; File name tokens: %y year, %mo month, %d day, %h hour, %mi minute, %s second,\r\n"
    L"; %ms milliseconds, %pn app name, %t window title, %width, %height.\r\n"
    L"; A \\ makes subfolders, e.g.  %y-%mo\\%pn %h.%mi.%s\r\n"
    L"FileName=%y-%mo-%d %h.%mi.%s\r\n"
    L"SaveToFile=1\r\n"
    L"CopyToClipboard=1\r\n"
    L"IncludeCursor=0\r\n"
    L"; FullScreen: all = every monitor, monitor = the monitor under the mouse\r\n"
    L"FullScreen=all\r\n"
    L"ShowNotification=1\r\n";

std::wstring ReadStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(sec, key, def, buf, 1024, SettingsPath().c_str());
    return buf;
}

bool ReadBool(const wchar_t* sec, const wchar_t* key, bool def) {
    return GetPrivateProfileIntW(sec, key, def ? 1 : 0, SettingsPath().c_str()) != 0;
}

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

}  // namespace

std::wstring AppDataDir() {
    static std::wstring dir;
    if (dir.empty()) {
        PWSTR p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &p))) {
            dir = std::wstring(p) + L"\\Canivete";
            CoTaskMemFree(p);
        } else {
            dir = L".";
        }
        CreateDirectoryW(dir.c_str(), nullptr);
    }
    return dir;
}

std::wstring SettingsPath() { return AppDataDir() + L"\\settings.ini"; }

void LoadSettings() {
    const std::wstring path = SettingsPath();
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        // UTF-16 LE with BOM so the INI functions handle any characters in folder names.
        HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            DWORD n;
            const wchar_t bom = 0xFEFF;
            WriteFile(f, &bom, sizeof(bom), &n, nullptr);
            WriteFile(f, kDefaultIni, static_cast<DWORD>(wcslen(kDefaultIni) * sizeof(wchar_t)), &n, nullptr);
            CloseHandle(f);
        }
    }
    Settings s;
    s.hkRegion = ReadStr(L"Hotkeys", L"RegionScreenshot", L"Ctrl+PrintScreen");
    s.hkWindow = ReadStr(L"Hotkeys", L"WindowScreenshot", L"Alt+PrintScreen");
    s.hkFull = ReadStr(L"Hotkeys", L"FullScreenshot", L"PrintScreen");
    s.folder = Trim(ReadStr(L"Screenshot", L"Folder", L""));
    s.fileName = Trim(ReadStr(L"Screenshot", L"FileName", L"%y-%mo-%d %h.%mi.%s"));
    if (s.fileName.empty()) s.fileName = L"%y-%mo-%d %h.%mi.%s";
    s.saveToFile = ReadBool(L"Screenshot", L"SaveToFile", true);
    s.copyToClipboard = ReadBool(L"Screenshot", L"CopyToClipboard", true);
    s.includeCursor = ReadBool(L"Screenshot", L"IncludeCursor", false);
    s.fullScreen = Lower(Trim(ReadStr(L"Screenshot", L"FullScreen", L"all")));
    s.showNotification = ReadBool(L"Screenshot", L"ShowNotification", true);
    g_settings = s;
}

bool ParseHotkey(const std::wstring& text, Hotkey& out) {
    out = {};
    std::wstring t = Lower(Trim(text));
    if (t.empty() || t == L"none") return false;
    size_t start = 0;
    while (start <= t.size()) {
        size_t plus = t.find(L'+', start);
        std::wstring part = Trim(t.substr(start, plus == std::wstring::npos ? std::wstring::npos : plus - start));
        start = (plus == std::wstring::npos) ? t.size() + 1 : plus + 1;
        if (part.empty()) continue;
        if (part == L"ctrl" || part == L"control") out.mods |= MOD_CONTROL;
        else if (part == L"alt") out.mods |= MOD_ALT;
        else if (part == L"shift") out.mods |= MOD_SHIFT;
        else if (part == L"win" || part == L"windows") out.mods |= MOD_WIN;
        else if (part == L"printscreen" || part == L"prtsc" || part == L"print") out.vk = VK_SNAPSHOT;
        else if (part == L"space") out.vk = VK_SPACE;
        else if (part == L"pause") out.vk = VK_PAUSE;
        else if (part == L"insert") out.vk = VK_INSERT;
        else if (part == L"delete") out.vk = VK_DELETE;
        else if (part == L"home") out.vk = VK_HOME;
        else if (part == L"end") out.vk = VK_END;
        else if (part == L"pageup") out.vk = VK_PRIOR;
        else if (part == L"pagedown") out.vk = VK_NEXT;
        else if (part == L"scrolllock") out.vk = VK_SCROLL;
        else if (part.size() >= 2 && part[0] == L'f' && iswdigit(part[1])) {
            int n = _wtoi(part.c_str() + 1);
            if (n < 1 || n > 24) return false;
            out.vk = VK_F1 + n - 1;
        } else if (part.size() == 1 && (iswalpha(part[0]) || iswdigit(part[0]))) {
            out.vk = towupper(part[0]);
        } else {
            return false;
        }
    }
    if (!out.vk) return false;
    out.mods |= MOD_NOREPEAT;
    return true;
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vswprintf_s(msg, fmt, ap);
    va_end(ap);
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t line[1200];
    swprintf_s(line, L"%04d-%02d-%02d %02d:%02d:%02d.%03d  %s\r\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond, t.wMilliseconds, msg);
    OutputDebugStringW(line);

    const std::wstring path = AppDataDir() + L"\\canivete.log";
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 512 * 1024)
        DeleteFileW(path.c_str());
    HANDLE f = CreateFileW(path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char utf8[2400];
    int n = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
    DWORD written;
    if (n > 1) WriteFile(f, utf8, n - 1, &written, nullptr);
    CloseHandle(f);
}
