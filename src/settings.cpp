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
    L"[Features]\r\n"
    L"; 0 turns a feature off: no hotkeys, no menu entries.\r\n"
    L"Screenshots=1\r\n"
    L"ColorPicker=1\r\n"
    L"Gif=1\r\n"
    L"Video=1\r\n"
    L"ImageTools=1\r\n"
    L"Renamer=1\r\n"
    L"Maintenance=1\r\n"
    L"\r\n"
    L"[Hotkeys]\r\n"
    L"RegionScreenshot=Ctrl+PrintScreen\r\n"
    L"WindowScreenshot=None\r\n"
    L"FullScreenshot=PrintScreen\r\n"
    L"RecordGif=Ctrl+Shift+PrintScreen\r\n"
    L"RecordVideo=Shift+PrintScreen\r\n"
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

// Named keys, both ways (text <-> virtual key). Letters, digits and F1-F24 are handled in code.
struct KeyName {
    UINT vk;
    const wchar_t* name;
};
const KeyName kKeys[] = {
    {VK_SNAPSHOT, L"PrintScreen"}, {VK_SPACE, L"Space"},   {VK_PAUSE, L"Pause"},
    {VK_INSERT, L"Insert"},        {VK_DELETE, L"Delete"}, {VK_HOME, L"Home"},
    {VK_END, L"End"},              {VK_PRIOR, L"PageUp"},  {VK_NEXT, L"PageDown"},
    {VK_SCROLL, L"ScrollLock"},    {VK_LEFT, L"Left"},     {VK_RIGHT, L"Right"},
    {VK_UP, L"Up"},                {VK_DOWN, L"Down"},     {VK_RETURN, L"Enter"},
    {VK_NUMPAD0, L"Num0"},         {VK_NUMPAD1, L"Num1"},  {VK_NUMPAD2, L"Num2"},
    {VK_NUMPAD3, L"Num3"},         {VK_NUMPAD4, L"Num4"},  {VK_NUMPAD5, L"Num5"},
    {VK_NUMPAD6, L"Num6"},         {VK_NUMPAD7, L"Num7"},  {VK_NUMPAD8, L"Num8"},
    {VK_NUMPAD9, L"Num9"},
};

const KeyName* FindKey(const std::wstring& lowerName) {
    for (const KeyName& k : kKeys)
        if (Lower(k.name) == lowerName) return &k;
    return nullptr;
}

void WriteStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(sec, key, v.c_str(), SettingsPath().c_str());
}

void WriteBool(const wchar_t* sec, const wchar_t* key, bool v) { WriteStr(sec, key, v ? L"1" : L"0"); }

}  // namespace

void SaveSettingValue(const wchar_t* section, const wchar_t* key, const std::wstring& value) {
    WriteStr(section, key, value);
}

// CANIVETE_TEST=<folder>: a private test copy (own settings and log in that folder, no tray icon,
// hotkeys or Explorer entries), so tests run on a hidden desktop next to the real one.
const std::wstring& TestDir() {
    static const std::wstring dir = [] {
        wchar_t buf[MAX_PATH * 2] = {};
        const DWORD n = GetEnvironmentVariableW(L"CANIVETE_TEST", buf, MAX_PATH * 2);
        return n && n < MAX_PATH * 2 ? std::wstring(buf) : std::wstring();
    }();
    return dir;
}

bool TestMode() { return !TestDir().empty(); }

std::wstring AppDataDir() {
    static std::wstring dir;
    if (dir.empty() && TestMode()) {
        dir = TestDir();
        CreateDirectoryW(dir.c_str(), nullptr);
    }
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
    s.hkWindow = ReadStr(L"Hotkeys", L"WindowScreenshot", L"None");
    s.hkFull = ReadStr(L"Hotkeys", L"FullScreenshot", L"PrintScreen");
    s.hkColor = ReadStr(L"Hotkeys", L"ColorPicker", L"Win+C");
    s.hkGif = ReadStr(L"Hotkeys", L"RecordGif", L"Ctrl+Shift+PrintScreen");
    s.hkVideo = ReadStr(L"Hotkeys", L"RecordVideo", L"Shift+PrintScreen");
    for (int f = 0; f < kFeatCount; ++f) s.features[f] = ReadBool(L"Features", kFeatureKeys[f], true);
    s.gifPreset = Lower(Trim(ReadStr(L"Gif", L"Preset", L"balanced")));
    s.videoPreset = Lower(Trim(ReadStr(L"Video", L"Preset", L"balanced")));
    s.colorFormat = Lower(Trim(ReadStr(L"ColorPicker", L"CopyFormat", L"gray")));
    {
        bool known = false;
        for (const ColorFormatDef& f : kColorFormats) known |= s.colorFormat == f.key;
        if (!known) s.colorFormat = L"gray";
    }
    s.folder = Trim(ReadStr(L"Screenshot", L"Folder", L""));
    s.fileName = Trim(ReadStr(L"Screenshot", L"FileName", L"%y-%mo-%d %h.%mi.%s"));
    if (s.fileName.empty()) s.fileName = L"%y-%mo-%d %h.%mi.%s";
    s.saveToFile = ReadBool(L"Screenshot", L"SaveToFile", true);
    s.copyToClipboard = ReadBool(L"Screenshot", L"CopyToClipboard", true);
    s.includeCursor = ReadBool(L"Screenshot", L"IncludeCursor", false);
    s.fullScreen = Lower(Trim(ReadStr(L"Screenshot", L"FullScreen", L"all")));
    s.showNotification = ReadBool(L"Screenshot", L"ShowNotification", true);
    s.showMagnifier = ReadBool(L"Screenshot", L"ShowMagnifier", true);
    s.openEditor = ReadBool(L"Screenshot", L"OpenEditor", false);
    s.videoSound = ReadBool(L"Video", L"Sound", true);
    s.videoMic = ReadBool(L"Video", L"Microphone", false);
    s.maintDaily = ReadBool(L"Maintenance", L"DailyCheck", true);
    s.checkUpdates = ReadBool(L"General", L"CheckUpdates", true);
    s.uploadService = Lower(Trim(ReadStr(L"Upload", L"Service", L"drive"))) == L"s3" ? L"s3" : L"drive";
    s.uploadShots = ReadBool(L"Upload", L"AutoScreenshots", false);
    s.uploadGif = ReadBool(L"Upload", L"AutoGif", false);
    s.uploadVideo = ReadBool(L"Upload", L"AutoVideo", false);
    s.driveClientId = Trim(ReadStr(L"Upload", L"DriveClientId", L""));
    s.driveFolder = Trim(ReadStr(L"Upload", L"DriveFolder", L"Canivete"));
    s.driveShare = ReadBool(L"Upload", L"DriveShare", true);
    s.s3Bucket = Trim(ReadStr(L"Upload", L"S3Bucket", L""));
    s.s3Region = Lower(Trim(ReadStr(L"Upload", L"S3Region", L"")));
    s.s3Prefix = Trim(ReadStr(L"Upload", L"S3Prefix", L"canivete/"));
    s.s3KeyId = Trim(ReadStr(L"Upload", L"S3AccessKeyId", L""));
    s.s3LinkBase = Trim(ReadStr(L"Upload", L"S3LinkBase", L""));
    s.s3Private = ReadBool(L"Upload", L"S3PrivateLinks", false);
    s.format = Lower(Trim(ReadStr(L"Screenshot", L"Format", L"png")));
    if (s.format == L"jpeg") s.format = L"jpg";
    if (s.format != L"jpg") s.format = L"png";
    s.jpegQuality = GetPrivateProfileIntW(L"Screenshot", L"JpegQuality", 90, path.c_str());
    s.jpegQuality = s.jpegQuality < 1 ? 1 : s.jpegQuality > 100 ? 100 : s.jpegQuality;
    g_settings = s;
}

void SaveSettings(const Settings& s) {
    LoadSettings();  // makes sure the file exists (with its comments) before writing keys into it
    for (int f = 0; f < kFeatCount; ++f) WriteBool(L"Features", kFeatureKeys[f], s.features[f]);
    WriteStr(L"Hotkeys", L"RegionScreenshot", s.hkRegion);
    WriteStr(L"Hotkeys", L"WindowScreenshot", s.hkWindow);
    WriteStr(L"Hotkeys", L"FullScreenshot", s.hkFull);
    WriteStr(L"Hotkeys", L"ColorPicker", s.hkColor);
    WriteStr(L"Hotkeys", L"RecordGif", s.hkGif);
    WriteStr(L"Hotkeys", L"RecordVideo", s.hkVideo);
    WriteStr(L"ColorPicker", L"CopyFormat", s.colorFormat);
    WriteStr(L"Screenshot", L"Folder", s.folder);
    WriteStr(L"Screenshot", L"FileName", s.fileName);
    WriteStr(L"Screenshot", L"Format", s.format);
    WriteStr(L"Screenshot", L"JpegQuality", std::to_wstring(s.jpegQuality));
    WriteBool(L"Screenshot", L"SaveToFile", s.saveToFile);
    WriteBool(L"Screenshot", L"CopyToClipboard", s.copyToClipboard);
    WriteBool(L"Screenshot", L"IncludeCursor", s.includeCursor);
    WriteStr(L"Screenshot", L"FullScreen", s.fullScreen);
    WriteBool(L"Screenshot", L"ShowNotification", s.showNotification);
    WriteBool(L"Screenshot", L"ShowMagnifier", s.showMagnifier);
    WriteBool(L"Screenshot", L"OpenEditor", s.openEditor);
    WriteBool(L"Video", L"Sound", s.videoSound);
    WriteBool(L"Video", L"Microphone", s.videoMic);
    WriteBool(L"Maintenance", L"DailyCheck", s.maintDaily);
    WriteBool(L"General", L"CheckUpdates", s.checkUpdates);
    WriteStr(L"Upload", L"Service", s.uploadService);
    WriteBool(L"Upload", L"AutoScreenshots", s.uploadShots);
    WriteBool(L"Upload", L"AutoGif", s.uploadGif);
    WriteBool(L"Upload", L"AutoVideo", s.uploadVideo);
    WriteStr(L"Upload", L"DriveClientId", s.driveClientId);
    WriteStr(L"Upload", L"DriveFolder", s.driveFolder);
    WriteBool(L"Upload", L"DriveShare", s.driveShare);
    WriteStr(L"Upload", L"S3Bucket", s.s3Bucket);
    WriteStr(L"Upload", L"S3Region", s.s3Region);
    WriteStr(L"Upload", L"S3Prefix", s.s3Prefix);
    WriteStr(L"Upload", L"S3AccessKeyId", s.s3KeyId);
    WriteStr(L"Upload", L"S3LinkBase", s.s3LinkBase);
    WriteBool(L"Upload", L"S3PrivateLinks", s.s3Private);
}

std::wstring HotkeyText(const Hotkey& hk) {
    if (!hk.vk) return L"";
    std::wstring t;
    if (hk.mods & MOD_CONTROL) t += L"Ctrl+";
    if (hk.mods & MOD_ALT) t += L"Alt+";
    if (hk.mods & MOD_SHIFT) t += L"Shift+";
    if (hk.mods & MOD_WIN) t += L"Win+";
    for (const KeyName& k : kKeys)
        if (k.vk == hk.vk) return t + k.name;
    if (hk.vk >= VK_F1 && hk.vk <= VK_F24) return t + L"F" + std::to_wstring(hk.vk - VK_F1 + 1);
    if ((hk.vk >= L'A' && hk.vk <= L'Z') || (hk.vk >= L'0' && hk.vk <= L'9')) return t + static_cast<wchar_t>(hk.vk);
    return L"";  // a key we have no name for
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
        else if (part == L"prtsc" || part == L"print") out.vk = VK_SNAPSHOT;
        else if (const KeyName* k = FindKey(part)) out.vk = k->vk;
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
