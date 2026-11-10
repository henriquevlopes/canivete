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
    Feature feature;
};
const SendTo kSendTo[] = {
    {L"Canivete - Resize images.lnk", L"--resize", L"Resize images with Canivete", kFeatImages},
    {L"Canivete - Rename.lnk", L"--rename", L"Rename files and folders with Canivete", kFeatRename},
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
constexpr wchar_t kMenuVersion[] = L"|4";  // bump when the entries change, so they are written again
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
    // "Flip top to bottom" (flipv) left the menu for Remove background (16-entry limit); --do flipv still works.
    {L"4nobg", L"Remove background", L"nobg", true},
    {L"5copy", L"Copy to clipboard", L"copy", false},
    {L"6rename", L"Rename…", L"rename", true},
};

void Put(HKEY key, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                   static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
}

bool IsRename(const MenuEntry& e) { return wcsncmp(e.path, L"6rename", 7) == 0; }
bool Wanted(const MenuEntry& e) { return IsRename(e) ? FeatureOn(kFeatRename) : FeatureOn(kFeatImages); }

// Writes one copy of the submenu, with the entries of the features that are on.
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
    bool first = true;
    for (const MenuEntry& e : kMenu) {
        if (!Wanted(e)) continue;
        const bool top = !wcschr(e.path, L'\\');
        HKEY k;
        if (RegCreateKeyExW(root, (std::wstring(L"shell\\") + e.path).c_str(), 0, nullptr, 0, KEY_WRITE, nullptr, &k,
                            nullptr) != ERROR_SUCCESS)
            continue;
        const bool rename = e.action && wcscmp(e.action, L"rename") == 0;
        Put(k, L"MUIVerb", e.text);
        Put(k, L"MultiSelectModel", L"Player");
        if (!wcschr(e.path, L'\\') && !rename) Put(k, L"AppliesTo", L"System.Kind:=picture");
        if (e.separatorBefore && !(top && first)) {
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
        if (top) first = false;
    }
    Put(root, L"CaniveteMenu", marker);  // last, so a half-written menu is written again next time
    RegCloseKey(root);
}

// Writes the submenus again when the exe moved or the entries changed. Returns true when written.
bool RegisterMenu(const std::wstring& exe) {
    const bool images = FeatureOn(kFeatImages), rename = FeatureOn(kFeatRename);
    const std::wstring marker = exe + kMenuVersion + (images ? L"i" : L"") + (rename ? L"r" : L"");
    bool changed = false;
    for (const wchar_t* path : kMenus) {
        // Folders only get Rename (the picture entries never show on a folder).
        const bool folders = path == kMenus[1];
        if (folders ? !rename : !(images || rename)) {
            if (RegDeleteTreeW(HKEY_CURRENT_USER, path) == ERROR_SUCCESS) changed = true;
            continue;
        }
        wchar_t cur[MAX_PATH * 2] = {};
        DWORD size = sizeof(cur);
        if (RegGetValueW(HKEY_CURRENT_USER, path, L"CaniveteMenu", RRF_RT_REG_SZ, nullptr, cur, &size) ==
                ERROR_SUCCESS &&
            marker == cur)
            continue;
        if (!changed)
            for (const wchar_t* old : kOldKeys) RegDeleteTreeW(HKEY_CURRENT_USER, old);
        WriteMenu(path, exe, marker);
        changed = true;
    }
    if (changed) Log(L"Explorer Canivete submenu written (pictures %d, rename %d)", images, rename);
    return changed;
}

}  // namespace

void Shell_Register() {
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const bool changed = RegisterMenu(exe);

    // Start menu: Computer Maintenance.
    PWSTR dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &dir))) {
        const std::wstring lnk = std::wstring(dir) + L"\\Computer Maintenance.lnk";
        CoTaskMemFree(dir);
        dir = nullptr;
        if (!FeatureOn(kFeatMaint)) {
            DeleteFileW(lnk.c_str());
        } else if (changed || GetFileAttributesW(lnk.c_str()) == INVALID_FILE_ATTRIBUTES) {
            ComPtr<IShellLinkW> sl;
            ComPtr<IPersistFile> pf;
            if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl))) &&
                SUCCEEDED(sl.As(&pf))) {
                sl->SetPath(exe);
                sl->SetArguments(L"--maintenance");
                sl->SetIconLocation(exe, 0);
                sl->SetDescription(L"Updates, cleanup and startup apps (Canivete)");
                pf->Save(lnk.c_str(), TRUE);
            }
        }
    }

    if (FAILED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &dir))) return;
    const std::wstring sendTo = dir;
    CoTaskMemFree(dir);
    for (const SendTo& s : kSendTo) {
        const std::wstring lnk = sendTo + L"\\" + s.name;
        if (!FeatureOn(s.feature)) {
            DeleteFileW(lnk.c_str());
            continue;
        }
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

void Shell_Unregister() {
    for (const wchar_t* path : kMenus) RegDeleteTreeW(HKEY_CURRENT_USER, path);
    for (const wchar_t* old : kOldKeys) RegDeleteTreeW(HKEY_CURRENT_USER, old);
    PWSTR dir = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &dir))) {
        DeleteFileW((std::wstring(dir) + L"\\Computer Maintenance.lnk").c_str());
        CoTaskMemFree(dir);
        dir = nullptr;
    }
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_SendTo, 0, nullptr, &dir))) {
        for (const SendTo& s : kSendTo) DeleteFileW((std::wstring(dir) + L"\\" + s.name).c_str());
        CoTaskMemFree(dir);
    }
}
