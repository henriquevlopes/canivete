// Explorer integration: the "Canivete" right-click submenu (under "Show more options") and Send to
// shortcuts. Explorer starts one copy per selected item with MultiSelectModel=Player; each copy
// hands its path to the running one (see main.cpp), which gathers them into one window.
#include "app.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

using Microsoft::WRL::ComPtr;

namespace {

struct SendTo {
    const wchar_t* name;
    const wchar_t* arg;
    const wchar_t* tip;
};
const SendTo kSendTo[] = {
    {L"Canivete - Resize images.lnk", L"--resize", L"Resize images with Canivete"},
    {L"Canivete - Rename.lnk", L"--rename", L"Rename files and folders with Canivete"},
};

// The "Canivete" submenu on every file and folder (like XnShell's): key names sort the entries; a
// null action makes a submenu of the entries below it. Top-level entries other than Rename show
// for pictures only (AppliesTo); their children go with them. Explorer shows at most 16 entries
// of such a menu (submenu entries included, separators not), so keep the list at 16.
const wchar_t* const kMenus[] = {
    L"Software\\Classes\\*\\shell\\Canivete",
    L"Software\\Classes\\Directory\\shell\\Canivete",
};
// Earlier menu entries, removed when the menu is written.
const wchar_t* const kOldKeys[] = {
    L"Software\\Classes\\SystemFileAssociations\\image\\shell\\Canivete",
    L"Software\\Classes\\SystemFileAssociations\\image\\shell\\CaniveteResize",
    L"Software\\Classes\\*\\shell\\CaniveteRename",
    L"Software\\Classes\\Directory\\shell\\CaniveteRename",
};
constexpr wchar_t kMenuVersion[] = L"|3";  // bump when the entries change, so they are written again
struct MenuEntry {
    const wchar_t* path;    // under the menu's shell key
    const wchar_t* text;
    const wchar_t* action;  // --do <action>; "rename" runs --rename
    bool separatorBefore;
};
const MenuEntry kMenu[] = {
    {L"1resize", L"Resize", nullptr, false},
    {L"1resize\\shell\\1small", L"Small  (854 × 480)", L"small", false},
    {L"1resize\\shell\\2medium", L"Medium  (1366 × 768)", L"medium", false},
    {L"1resize\\shell\\3large", L"Large  (1920 × 1080)", L"large", false},
    {L"1resize\\shell\\5custom", L"Choose a size…", L"custom", true},
    {L"2convert", L"Convert to", nullptr, false},
    {L"2convert\\shell\\1jpg", L"JPG", L"jpg", false},
    {L"2convert\\shell\\2png", L"PNG", L"png", false},
    {L"2convert\\shell\\3tif", L"TIFF", L"tif", false},
    {L"3rotate", L"Rotate", nullptr, false},
    {L"3rotate\\shell\\1right", L"Turn right  (90° clockwise)", L"right", false},
    {L"3rotate\\shell\\2left", L"Turn left  (90° counter-clockwise)", L"left", false},
    {L"3rotate\\shell\\4fliph", L"Flip left to right", L"fliph", true},
    {L"3rotate\\shell\\5flipv", L"Flip top to bottom", L"flipv", false},
    {L"4copy", L"Copy to clipboard", L"copy", true},
    {L"5rename", L"Rename…", L"rename", true},
};

void Put(HKEY key, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                   static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

// Writes one copy of the submenu.
void WriteMenu(const wchar_t* path, const std::wstring& exe, const std::wstring& marker) {
    RegDeleteTreeW(HKEY_CURRENT_USER, path);
    HKEY root;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, path, 0, nullptr, 0, KEY_WRITE, nullptr, &root, nullptr) != ERROR_SUCCESS)
        return;
    const std::wstring quoted = L"\"" + exe + L"\"";
    Put(root, L"MUIVerb", L"Canivete");
    Put(root, L"Icon", quoted + L",0");
    Put(root, L"SubCommands", L"");
    Put(root, L"MultiSelectModel", L"Player");  // shown for any number of items
    for (const MenuEntry& e : kMenu) {
        HKEY k;
        if (RegCreateKeyExW(root, (std::wstring(L"shell\\") + e.path).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                            nullptr) != ERROR_SUCCESS)
            continue;
        const bool rename = e.action && wcscmp(e.action, L"rename") == 0;
        Put(k, L"MUIVerb", e.text);
        Put(k, L"MultiSelectModel", L"Player");
        if (!wcschr(e.path, L'\\') && !rename) Put(k, L"AppliesTo", L"System.Kind:=picture");
        if (e.separatorBefore) {
            const DWORD flags = 0x20;  // ECF_SEPARATORBEFORE
            RegSetValueExW(k, L"CommandFlags", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&flags), sizeof(flags));
        }
        if (!e.action) {
            Put(k, L"SubCommands", L"");
        } else {
            HKEY c;
            if (RegCreateKeyExW(k, L"command", 0, nullptr, 0, KEY_WRITE, nullptr, &c, nullptr) == ERROR_SUCCESS) {
                const std::wstring arg = rename ? std::wstring(L"--rename") : std::wstring(L"--do ") + e.action;
                Put(c, nullptr, quoted + L" " + arg + L" \"%1\"");
                RegCloseKey(c);
            }
        }
        RegCloseKey(k);
    }
    Put(root, L"CaniveteMenu", marker);  // last, so a half-written menu is written again next time
    RegCloseKey(root);
}

// Writes the submenus again when the exe moved or the entries changed. Returns true when written.
bool RegisterMenu(const std::wstring& exe) {
    const std::wstring marker = exe + kMenuVersion;
    bool current = true;
    for (const wchar_t* path : kMenus) {
        wchar_t cur[MAX_PATH * 2] = {};
        DWORD size = sizeof(cur);
        if (RegGetValueW(HKEY_CURRENT_USER, path, L"CaniveteMenu", RRF_RT_REG_SZ, nullptr, cur, &size) !=
                ERROR_SUCCESS ||
            marker != cur)
            current = false;
    }
    if (current) return false;
    for (const wchar_t* old : kOldKeys) RegDeleteTreeW(HKEY_CURRENT_USER, old);
    for (const wchar_t* path : kMenus) WriteMenu(path, exe, marker);
    Log(L"Explorer Canivete submenu registered");
    return true;
}

}  // namespace

void Shell_Register() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const bool changed = RegisterMenu(exe);

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
