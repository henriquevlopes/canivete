// Explorer integration: right-click entries (under "Show more options") and Send to shortcuts.
// Explorer starts one copy per selected item with MultiSelectModel=Player; each copy hands its
// path to the running one (see main.cpp), which gathers them into one window.
#include "app.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

struct Verb {
    const wchar_t* key;   // under HKCU\Software\Classes
    const wchar_t* text;  // menu text
    const wchar_t* arg;   // command-line switch
};
const Verb kVerbs[] = {
    {L"*\\shell\\CaniveteRename", L"Rename with Canivete", L"--rename"},
    {L"Directory\\shell\\CaniveteRename", L"Rename with Canivete", L"--rename"},
};

struct SendTo {
    const wchar_t* name;
    const wchar_t* arg;
    const wchar_t* tip;
};
const SendTo kSendTo[] = {
    {L"Canivete - Resize images.lnk", L"--resize", L"Resize images with Canivete"},
    {L"Canivete - Rename.lnk", L"--rename", L"Rename files and folders with Canivete"},
};

// The "Canivete" submenu on images (like XnShell's): key names sort the entries; a null action
// makes a submenu of the entries below it.
constexpr wchar_t kImageMenu[] = L"Software\\Classes\\SystemFileAssociations\\image\\shell\\Canivete";
constexpr wchar_t kMenuVersion[] = L"|1";  // bump when the entries change, so they are written again
struct MenuEntry {
    const wchar_t* path;    // under kImageMenu\shell
    const wchar_t* text;
    const wchar_t* action;  // --do <action>
    bool separatorBefore;
};
const MenuEntry kMenu[] = {
    {L"1resize", L"Resize", nullptr, false},
    {L"1resize\\shell\\1small", L"Small  (854 × 480)", L"small", false},
    {L"1resize\\shell\\2medium", L"Medium  (1366 × 768)", L"medium", false},
    {L"1resize\\shell\\3large", L"Large  (1920 × 1080)", L"large", false},
    {L"1resize\\shell\\4half", L"Half size  (50 %)", L"half", false},
    {L"1resize\\shell\\5custom", L"Choose a size…", L"custom", true},
    {L"2convert", L"Convert to", nullptr, false},
    {L"2convert\\shell\\1jpg", L"JPG", L"jpg", false},
    {L"2convert\\shell\\2png", L"PNG", L"png", false},
    {L"2convert\\shell\\3tif", L"TIFF", L"tif", false},
    {L"2convert\\shell\\4bmp", L"BMP", L"bmp", false},
    {L"3rotate", L"Rotate", nullptr, false},
    {L"3rotate\\shell\\1right", L"Turn right  (90° clockwise)", L"right", false},
    {L"3rotate\\shell\\2left", L"Turn left  (90° counter-clockwise)", L"left", false},
    {L"3rotate\\shell\\3half", L"Turn upside down  (180°)", L"180", false},
    {L"3rotate\\shell\\4fliph", L"Flip left to right", L"fliph", true},
    {L"3rotate\\shell\\5flipv", L"Flip top to bottom", L"flipv", false},
    {L"4copy", L"Copy to clipboard", L"copy", true},
};

void Put(HKEY key, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                   static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

// Writes the image submenu again when the exe moved or the entries changed. Returns true when written.
bool RegisterImageMenu(const std::wstring& exe) {
    const std::wstring marker = exe + kMenuVersion;
    wchar_t cur[MAX_PATH * 2] = {};
    DWORD size = sizeof(cur);
    if (RegGetValueW(HKEY_CURRENT_USER, kImageMenu, L"CaniveteMenu", RRF_RT_REG_SZ, nullptr, cur, &size) ==
            ERROR_SUCCESS &&
        marker == cur)
        return false;
    RegDeleteTreeW(HKEY_CURRENT_USER, kImageMenu);
    RegDeleteTreeW(HKEY_CURRENT_USER, L"Software\\Classes\\SystemFileAssociations\\image\\shell\\CaniveteResize");
    HKEY root;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kImageMenu, 0, nullptr, 0, KEY_WRITE, nullptr, &root, nullptr) != ERROR_SUCCESS)
        return false;
    const std::wstring icon = L"\"" + exe + L"\",0";
    Put(root, L"MUIVerb", L"Canivete");
    Put(root, L"Icon", icon);
    Put(root, L"SubCommands", L"");
    Put(root, L"MultiSelectModel", L"Player");  // shown for any number of images
    for (const MenuEntry& e : kMenu) {
        HKEY k;
        if (RegCreateKeyExW(root, (std::wstring(L"shell\\") + e.path).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                            nullptr) != ERROR_SUCCESS)
            continue;
        Put(k, L"MUIVerb", e.text);
        Put(k, L"MultiSelectModel", L"Player");
        if (e.separatorBefore) {
            const DWORD flags = 0x20;  // ECF_SEPARATORBEFORE
            RegSetValueExW(k, L"CommandFlags", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&flags), sizeof(flags));
        }
        if (!e.action) {
            Put(k, L"SubCommands", L"");
        } else {
            HKEY c;
            if (RegCreateKeyExW(k, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
                Put(c, nullptr, L"\"" + exe + L"\" --do " + e.action + L" \"%1\"");
                RegCloseKey(c);
            }
        }
        RegCloseKey(k);
    }
    Put(root, L"CaniveteMenu", marker);  // last, so a half-written menu is written again next time
    RegCloseKey(root);
    Log(L"Explorer image submenu registered");
    return true;
}

// True when the entry already points at this exe.
bool VerbCurrent(const std::wstring& key, const std::wstring& cmd) {
    wchar_t cur[MAX_PATH * 2] = {};
    DWORD size = sizeof(cur);
    return RegGetValueW(HKEY_CURRENT_USER, (key + L"\\command").c_str(), nullptr, RRF_RT_REG_SZ, nullptr, cur, &size) ==
               ERROR_SUCCESS &&
           cmd == cur;
}

}  // namespace

void Shell_Register() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    bool changed = RegisterImageMenu(exe);
    for (const Verb& v : kVerbs) {
        const std::wstring key = std::wstring(L"Software\\Classes\\") + v.key;
        const std::wstring cmd = L"\"" + std::wstring(exe) + L"\" " + v.arg + L" \"%1\"";
        if (VerbCurrent(key, cmd)) continue;
        HKEY k;
        if (RegCreateKeyExW(HKEY_CURRENT_USER, key.c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k, nullptr) !=
            ERROR_SUCCESS)
            continue;
        Put(k, nullptr, v.text);
        Put(k, L"Icon", L"\"" + std::wstring(exe) + L"\",0");
        Put(k, L"MultiSelectModel", L"Player");  // any number of items
        HKEY c;
        if (RegCreateKeyExW(k, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
            Put(c, nullptr, cmd);
            RegCloseKey(c);
        }
        RegCloseKey(k);
        changed = true;
        Log(L"Explorer menu entry registered: %s", cmd.c_str());
    }

    PWSTR dir = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &dir))) return;
    const std::wstring sendTo = dir;
    CoTaskMemFree(dir);
    for (const SendTo& s : kSendTo) {
        const std::wstring lnk = sendTo + L"\\" + s.name;
        if (!changed && GetFileAttributesW(lnk.c_str()) != INVALID_FILE_ATTRIBUTES) continue;
        ComPtr<IShellLinkW> sl;
        ComPtr<IPersistFile> pf;
        if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) ||
            FAILED(sl.As(&pf)))
            continue;
        sl->SetPath(exe);
        sl->SetArguments(s.arg);
        sl->SetIconLocation(exe, 0);
        sl->SetDescription(s.tip);
        pf->Save(lnk.c_str(), TRUE);
    }
}
