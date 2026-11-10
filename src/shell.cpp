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
    {L"SystemFileAssociations\\image\\shell\\CaniveteResize", L"Resize with Canivete", L"--resize"},
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

void Put(HKEY key, const wchar_t* name, const std::wstring& v) {
    RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                   static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
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
    bool changed = false;
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
