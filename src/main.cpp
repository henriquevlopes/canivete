// Canivete - a small tray toolbox. Entry point: tray icon, global hotkeys, menu, settings reload.
#include "app.h"
#include "theme.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <climits>
#include <thread>

HINSTANCE g_hinst = nullptr;
HWND g_mainWnd = nullptr;

namespace {

enum : UINT { ID_REGION = 100, ID_WINDOW, ID_FULL, ID_COLOR, ID_GIF, ID_OPEN_FOLDER, ID_SETTINGS, ID_EXIT, ID_RESIZE, ID_RENAME, ID_VIDEO, ID_MAINT, ID_UPDATE, ID_TEXT };
constexpr UINT ID_RECENT0 = 200;  // .. 209: recent colors in the tray menu
constexpr UINT kCmdAlreadyRunning = 99;
constexpr UINT kCmdSettings = 98;
constexpr UINT kCmdMaint = 97;
constexpr UINT kCmdUpdate = 96;      // --check-update
constexpr UINT kCmdMaintCheck = 95;  // --maint-check: the daily background check, now
constexpr UINT kCmdClickNotice = 94; // --click-notice (test copy): as if the last notice was clicked
constexpr UINT kCmdDriveConnect = 93;  // --drive-connect: Google Drive sign-in (as the Settings button)
constexpr UINT kCmdS3Test = 92;        // --s3-test: upload a small test file to S3
constexpr UINT_PTR kTimerDeferred = 10;
constexpr UINT_PTR kTimerResize = 11;
constexpr UINT_PTR kTimerRename = 12;
constexpr UINT_PTR kTimerAction = 13;
constexpr UINT_PTR kTimerChecks = 14;  // one-shot: the next daily check (maintenance, Canivete update) is due
constexpr ULONG_PTR kCopyResize = 0x52535A31;  // WM_COPYDATA tags: file list for the resizer
constexpr ULONG_PTR kCopyRename = 0x524E4D31;  // ... and for the renamer
constexpr ULONG_PTR kCopyAction = 0x41435431;  // ... and for a quick action (first entry = action name)
constexpr ULONG_PTR kCopyUpload = 0x55504C31;  // ... and files to upload (--upload)
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

UINT g_taskbarCreated = 0;
NOTIFYICONDATAW g_nid{};
HICON g_icon = nullptr;
CaptureMode g_deferredMode = CaptureMode::Region;
FILETIME g_iniTime{};
bool g_hotkeysSuspended = false;
std::vector<std::wstring> g_resizeQueue;  // files from Explorer, gathered for ~300 ms (one process per file)
std::vector<std::wstring> g_renameQueue;
std::vector<std::pair<std::wstring, std::wstring>> g_actionQueue;  // (action, file) from the Canivete submenu

// Queues a file list from the command line or another copy and starts the gather timer.
void QueueList(ULONG_PTR tag, const std::vector<std::wstring>& entries) {
    if (tag == kCopyUpload) {
        for (const std::wstring& e : entries)
            if (!e.empty()) Upload_File(e);
        return;
    }
    if (tag == kCopyAction) {
        if (entries.empty()) return;
        for (size_t i = 1; i < entries.size(); ++i)
            if (!entries[i].empty()) g_actionQueue.emplace_back(entries[0], entries[i]);
        SetTimer(g_mainWnd, kTimerAction, 300, nullptr);
        return;
    }
    std::vector<std::wstring>& queue = tag == kCopyRename ? g_renameQueue : g_resizeQueue;
    for (const std::wstring& e : entries)
        if (!e.empty()) queue.push_back(e);
    SetTimer(g_mainWnd, tag == kCopyRename ? kTimerRename : kTimerResize, 300, nullptr);
}

// Windows reserves some Win+key combos (Win+C, Win+Shift+C ...) so RegisterHotKey refuses them.
// Those are caught with a low-level keyboard hook instead, like PowerToys does. The hook runs on its
// own small thread: every key press on the PC passes through it, so it must never wait for the UI
// thread (a busy UI thread would make typing lag everywhere).
struct HookKey {
    int id;
    Hotkey hk;
    bool down;
};
std::vector<HookKey> g_hookKeys;  // set before the hook thread starts; only that thread uses it afterwards
std::thread g_hookThread;
DWORD g_hookThreadId = 0;

UINT CurrentMods() {
    UINT m = 0;
    if (GetAsyncKeyState(VK_CONTROL) < 0) m |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_MENU) < 0) m |= MOD_ALT;
    if (GetAsyncKeyState(VK_SHIFT) < 0) m |= MOD_SHIFT;
    if (GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0) m |= MOD_WIN;
    return m;
}

LRESULT CALLBACK WinKeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if (!(k->flags & LLKHF_INJECTED)) {
            const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
            for (HookKey& e : g_hookKeys) {
                if (e.hk.vk != k->vkCode) continue;
                if (down && (e.down || CurrentMods() == (e.hk.mods & ~MOD_NOREPEAT))) {
                    if (!e.down) {
                        e.down = true;
                        // A dummy key between Win down and up stops the Start menu from opening.
                        INPUT in[2]{};
                        in[0].type = in[1].type = INPUT_KEYBOARD;
                        in[0].ki.wVk = in[1].ki.wVk = 0xFF;
                        in[1].ki.dwFlags = KEYEVENTF_KEYUP;
                        SendInput(2, in, sizeof(INPUT));
                        PostMessageW(g_mainWnd, WM_HOTKEY, e.id, 0);
                    }
                    return 1;
                }
                if (!down && e.down) {
                    e.down = false;
                    return 1;
                }
            }
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void StopKeyHook() {
    if (!g_hookThread.joinable()) return;
    PostThreadMessageW(g_hookThreadId, WM_QUIT, 0, 0);
    g_hookThread.join();
    g_hookThreadId = 0;
}

// Restarts the hook thread for these keys (none: no hook, no thread).
void StartKeyHook(std::vector<HookKey> keys) {
    StopKeyHook();
    g_hookKeys = std::move(keys);
    if (g_hookKeys.empty()) return;
    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_hookThread = std::thread([ready] {
        MSG m;
        PeekMessageW(&m, nullptr, WM_USER, WM_USER, PM_NOREMOVE);  // the thread's message queue, for WM_QUIT
        g_hookThreadId = GetCurrentThreadId();
        // Tiny work per key, but Windows drops a hook that answers late: run ahead of busy threads.
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HHOOK hook = SetWindowsHookExW(WH_KEYBOARD_LL, WinKeyHook, g_hinst, 0);
        SetEvent(ready);
        if (!hook) return;
        while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        }
        UnhookWindowsHookEx(hook);
    });
    WaitForSingleObject(ready, 5000);
    CloseHandle(ready);
}

// The tray icon: a pocket knife with no background. Its blade is light on a dark taskbar and dark on a
// light one (made by tools/make_icons.py).
bool TaskbarIsLight() {
    DWORD v = 0, n = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                        L"SystemUsesLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS &&
           v != 0;
}

void LoadTrayIcon() {
    HICON ic = nullptr;
    LoadIconMetric(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(TaskbarIsLight() ? IDI_TRAY_LIGHT : IDI_TRAY_DARK),
                   LIM_SMALL, &ic);
    if (!ic) return;
    if (g_icon) DestroyIcon(g_icon);
    g_icon = ic;
}

void TrayAdd() {
    g_nid = {};
    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_mainWnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP | NIF_SHOWTIP;
    g_nid.uCallbackMessage = WM_APP_TRAY;
    g_nid.hIcon = g_icon;
    wcscpy_s(g_nid.szTip, kAppName);
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    g_nid.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &g_nid);
}

std::function<void()> g_noticeClick;  // what clicking the notice on screen does

void TrayBalloon(const wchar_t* title, const std::wstring& text) {
    g_noticeClick = nullptr;
    if (TestMode()) {
        Log(L"Notice: %s - %s", title, text.c_str());
        return;
    }
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

// Registers the three hotkeys. Returns what went wrong, one line per hotkey ("" = all fine).
std::wstring RegisterHotkeys() {
    struct Entry {
        CaptureMode mode;
        const std::wstring* text;
        const wchar_t* name;
        Feature feature;
    } const list[] = {
        {CaptureMode::Region, &g_settings.hkRegion, L"Region screenshot", kFeatShots},
        {CaptureMode::Full, &g_settings.hkFull, L"Full screenshot", kFeatShots},
        {CaptureMode::Color, &g_settings.hkColor, L"Color picker", kFeatColor},
        {CaptureMode::Gif, &g_settings.hkGif, L"Record GIF", kFeatGif},
        {CaptureMode::Video, &g_settings.hkVideo, L"Record video", kFeatVideo},
        {CaptureMode::Text, &g_settings.hkText, L"Extract text", kFeatShots},
    };
    std::wstring failed;
    for (const Entry& e : list) UnregisterHotKey(g_mainWnd, static_cast<int>(e.mode));
    std::vector<HookKey> hookKeys;
    if (g_hotkeysSuspended || TestMode()) {
        StartKeyHook({});
        return failed;
    }
    for (const Entry& e : list) {
        if (!FeatureOn(e.feature)) continue;
        const int id = static_cast<int>(e.mode);
        Hotkey hk;
        if (!ParseHotkey(*e.text, hk)) {
            if (!e.text->empty() && _wcsicmp(e.text->c_str(), L"none") != 0)
                failed += std::wstring(e.name) + L": can't read \"" + *e.text + L"\"\n";
            continue;
        }
        if (RegisterHotKey(g_mainWnd, id, hk.mods, hk.vk)) continue;
        if (hk.mods & MOD_WIN) hookKeys.push_back({id, hk, false});  // reserved by Windows: use the hook
        else failed += std::wstring(e.name) + L": " + *e.text + L" is used by another app\n";
    }
    StartKeyHook(std::move(hookKeys));
    if (!failed.empty()) Log(L"Hotkey problems: %s", failed.c_str());
    return failed;
}

void RegisterHotkeysOrWarn() {
    const std::wstring failed = RegisterHotkeys();
    if (!failed.empty()) TrayBalloon(L"Some Canivete hotkeys don't work", failed);
}

FILETIME IniTime() {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(SettingsPath().c_str(), GetFileExInfoStandard, &fa)) return fa.ftLastWriteTime;
    return {};
}

ULONGLONG g_startTick = 0;

// One timer for the daily checks, set for the moment the next one is due (no hourly polling). The
// checks themselves say when they are due next (after a failure or a skip they wait a few hours).
void ScheduleChecks() {
    long long s = std::min(Maint_SecondsToNext(), Update_SecondsToNext());
    if (s == LLONG_MAX) {
        KillTimer(g_mainWnd, kTimerChecks);
        return;
    }
    const long long up = static_cast<long long>((GetTickCount64() - g_startTick) / 1000);
    s = std::max(s, 15 * 60 - up);           // nothing heavy in the first 15 minutes after start
    s = std::clamp(s, 60LL, 12 * 3600LL);    // and a look twice a day at most when nothing is due
    SetTimer(g_mainWnd, kTimerChecks, static_cast<UINT>(s * 1000), nullptr);
}

void ReloadIfChanged() {
    const FILETIME t = IniTime();
    if (CompareFileTime(&t, &g_iniTime) == 0) return;
    g_iniTime = t;
    LoadSettings();
    RegisterHotkeysOrWarn();
    if (!TestMode()) Shell_Register();
    ScheduleChecks();
    Log(L"Settings reloaded");
}

}  // namespace

void Tray_Notify(const wchar_t* title, const std::wstring& text) { TrayBalloon(title, text); }

void Tray_NotifyAction(const wchar_t* title, const std::wstring& text, std::function<void()> onClick) {
    TrayBalloon(title, text);
    g_noticeClick = std::move(onClick);
}

void Tray_SetTip(const std::wstring& tip) {
    if (TestMode()) return;  // no tray icon
    g_nid.uFlags = NIF_TIP | NIF_SHOWTIP;
    wcsncpy_s(g_nid.szTip, tip.empty() ? kAppName : tip.c_str(), _TRUNCATE);
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

std::wstring Settings_Applied() {
    LoadSettings();
    g_iniTime = IniTime();  // so the file watcher doesn't reload it a second time
    Log(L"Settings saved");
    if (!TestMode()) Shell_Register();  // Explorer entries follow the feature switches
    ScheduleChecks();
    return RegisterHotkeys();
}

void Hotkeys_Suspend(bool suspend) {
    if (g_hotkeysSuspended == suspend) return;
    g_hotkeysSuspended = suspend;
    RegisterHotkeys();
}

bool AutostartEnabled() {
    return RegGetValueW(HKEY_CURRENT_USER, kRunKey, kAppName, RRF_RT_REG_SZ, nullptr, nullptr, nullptr) ==
           ERROR_SUCCESS;
}

void SetAutostart(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunKey, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        const std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, kAppName, 0, REG_SZ, reinterpret_cast<const BYTE*>(v.c_str()),
                       static_cast<DWORD>((v.size() + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, kAppName);
    }
    RegCloseKey(k);
}

namespace {

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    auto item = [&](UINT id, const wchar_t* text, const std::wstring& hk) {
        std::wstring s = text;
        if (!hk.empty() && _wcsicmp(hk.c_str(), L"none") != 0) s += L"\t" + hk;
        AppendMenuW(m, MF_STRING, id, s.c_str());
    };
    // Separator before a group, only when something came before it.
    auto group = [&] {
        if (GetMenuItemCount(m) > 0) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    };
    if (FeatureOn(kFeatShots)) {
        item(ID_REGION, L"Region screenshot", g_settings.hkRegion);
        item(ID_FULL, L"Full screenshot", g_settings.hkFull);
        item(ID_TEXT, L"Extract text", g_settings.hkText);
    }
    if (Recorder_Active()) {
        item(ID_GIF, L"Stop recording", L"");
    } else {
        if (FeatureOn(kFeatGif)) item(ID_GIF, L"Record GIF", g_settings.hkGif);
        if (FeatureOn(kFeatVideo)) item(ID_VIDEO, L"Record video", g_settings.hkVideo);
    }
    if (FeatureOn(kFeatColor)) {
        group();
        item(ID_COLOR, L"Pick a color", g_settings.hkColor);
    }
    // Recent colors: a swatch and the value in the copy format; clicking copies it again.
    std::vector<HBITMAP> swatches;
    const std::vector<COLORREF> recent = FeatureOn(kFeatColor) ? RecentColors() : std::vector<COLORREF>{};
    if (!recent.empty()) {
        HMENU sub = CreatePopupMenu();
        const int sz = GetSystemMetricsForDpi(SM_CYMENUCHECK, GetDpiForWindow(g_mainWnd));
        HDC screen = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(screen);
        for (size_t i = 0; i < recent.size(); ++i) {
            const COLORREF c = recent[i];
            HBITMAP b = CreateCompatibleBitmap(screen, sz, sz);
            HGDIOBJ old = SelectObject(mem, b);
            RECT r{0, 0, sz, sz};
            SetDCBrushColor(mem, RGB(128, 128, 128));
            FillRect(mem, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            InflateRect(&r, -1, -1);
            SetDCBrushColor(mem, c);
            FillRect(mem, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
            SelectObject(mem, old);
            swatches.push_back(b);
            wchar_t hex[16];
            swprintf_s(hex, L"#%02X%02X%02X", GetRValue(c), GetGValue(c), GetBValue(c));
            std::wstring text = std::wstring(hex) + L"\t" + FormatColor(c, g_settings.colorFormat);
            MENUITEMINFOW mii{sizeof(mii)};
            mii.fMask = MIIM_ID | MIIM_STRING | MIIM_BITMAP;
            mii.wID = ID_RECENT0 + static_cast<UINT>(i);
            mii.dwTypeData = text.data();
            mii.hbmpItem = b;
            InsertMenuItemW(sub, static_cast<UINT>(i), TRUE, &mii);
        }
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
        AppendMenuW(m, MF_POPUP, reinterpret_cast<UINT_PTR>(sub), L"Recent colors");
    }
    if (FeatureOn(kFeatImages) || FeatureOn(kFeatRename)) group();
    if (FeatureOn(kFeatImages)) AppendMenuW(m, MF_STRING, ID_RESIZE, L"Resize images…");
    if (FeatureOn(kFeatRename)) AppendMenuW(m, MF_STRING, ID_RENAME, L"Rename files…");
    if (FeatureOn(kFeatMaint)) {
        group();
        AppendMenuW(m, MF_STRING, ID_MAINT, L"Computer maintenance…");
    }
    if (Update_Info().available) {
        group();
        AppendMenuW(m, MF_STRING, ID_UPDATE, (L"Update Canivete to " + Update_Info().latest + L"…").c_str());
    }
    group();
    AppendMenuW(m, MF_STRING, ID_OPEN_FOLDER, L"Open screenshots folder");
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_mainWnd);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON, p.x, p.y, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    for (HBITMAP b : swatches) DeleteObject(b);
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {  // Explorer restarted: put the icon back
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WM_SETTINGCHANGE:  // the taskbar went light or dark, or the screen scale changed
    case WM_DPICHANGED:
        if (msg == WM_DPICHANGED || (lp && lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet") == 0)) {
            LoadTrayIcon();
            if (!TestMode()) {
                g_nid.uFlags = NIF_ICON;
                g_nid.hIcon = g_icon;
                Shell_NotifyIconW(NIM_MODIFY, &g_nid);
            }
        }
        break;
    case WM_HOTKEY:
        if (wp >= 1 && wp <= kHotkeyCount) Capture_Start(static_cast<CaptureMode>(wp));
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:  // right-click (or Shift+F10 / the menu key): the menu
            ShowMenu();
            break;
        case NIN_SELECT:  // left-click (or Enter / Space): Settings
        case NIN_KEYSELECT:
            SettingsUI_Show();
            break;
        case NIN_BALLOONUSERCLICK:
            if (auto f = std::move(g_noticeClick)) {
                g_noticeClick = nullptr;
                f();
            }
            break;
        case NIN_BALLOONTIMEOUT:
            g_noticeClick = nullptr;
            break;
        }
        return 0;
    case WM_APP_MAINT:
        Maint_OnBackground(lp);
        ScheduleChecks();
        return 0;
    case WM_APP_TEXT: {
        std::unique_ptr<std::wstring> t(reinterpret_cast<std::wstring*>(lp));
        if (t) Tray_Notify(L"Extract text", *t);
        return 0;
    }
    case WM_APP_UPDATE:
        Update_OnMessage(wp, lp);
        ScheduleChecks();
        return 0;
    case WM_POWERBROADCAST:
        // Back from sleep: a check that came due meanwhile runs once the network is back.
        if (wp == PBT_APMRESUMEAUTOMATIC) SetTimer(h, kTimerChecks, 2 * 60 * 1000, nullptr);
        return TRUE;
    case WM_APP_UPLOAD:
        Upload_OnMessage(wp, lp);
        return 0;
    case WM_APP_CAPTURE_DONE:
        Output_OnDone(lp);
        return 0;
    case WM_APP_COMMAND:
        if (wp >= 1 && wp <= kHotkeyCount) Capture_Start(static_cast<CaptureMode>(wp));
        else if (wp == kCmdSettings)
            SettingsUI_Show();
        else if (wp == kCmdMaint)
            Maint_Show();
        else if (wp == kCmdUpdate)
            Update_Check(true);
        else if (wp == kCmdMaintCheck)
            Maint_Background(true);
        else if (wp == kCmdDriveConnect)
            Upload_DriveConnect();
        else if (wp == kCmdS3Test)
            Upload_S3Test();
        else if (wp == kCmdClickNotice && TestMode())
            SendMessageW(h, WM_APP_TRAY, 0, NIN_BALLOONUSERCLICK);
        else if (wp == kCmdAlreadyRunning)
            TrayBalloon(L"Canivete is already running", L"Right-click the red knife icon in the tray.");
        return 0;
    case WM_COPYDATA: {  // --resize / --rename / --do from another copy: ' '-separated entries
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cd->dwData != kCopyResize && cd->dwData != kCopyRename && cd->dwData != kCopyAction &&
            cd->dwData != kCopyUpload)
            return FALSE;
        std::vector<std::wstring> entries;
        const wchar_t* p = static_cast<const wchar_t*>(cd->lpData);
        const size_t n = cd->cbData / sizeof(wchar_t);
        for (size_t i = 0; i < n;) {
            const size_t len = wcsnlen(p + i, n - i);
            entries.emplace_back(p + i, len);
            i += len + 1;
        }
        QueueList(cd->dwData, entries);
        return TRUE;
    }
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_RESIZE:
            Resizer_Open({});
            break;
        case ID_RENAME:
            Renamer_Open({});
            break;
        case ID_MAINT:
            Maint_Show();
            break;
        case ID_UPDATE:
            Update_Install();
            break;
        case ID_REGION:
        case ID_WINDOW:
        case ID_FULL:
        case ID_COLOR:
        case ID_GIF:
        case ID_VIDEO:
        case ID_TEXT:
            if ((LOWORD(wp) == ID_GIF || LOWORD(wp) == ID_VIDEO) && Recorder_Active()) {
                Recorder_Stop();
                break;
            }
            // Wait for the menu to fade out so it isn't in the screenshot.
            g_deferredMode = LOWORD(wp) == ID_TEXT ? CaptureMode::Text : LOWORD(wp) == ID_VIDEO ? CaptureMode::Video
                                                    : static_cast<CaptureMode>(LOWORD(wp) - ID_REGION + 1);
            SetTimer(h, kTimerDeferred, 250, nullptr);
            break;
        case ID_OPEN_FOLDER: {
            const std::wstring dir = ScreenshotsDir();
            SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        case ID_SETTINGS:
            SettingsUI_Show();
            break;
        case ID_EXIT:
            DestroyWindow(h);
            break;
        default:
            if (LOWORD(wp) >= ID_RECENT0 && LOWORD(wp) < ID_RECENT0 + 10) {
                const std::vector<COLORREF> recent = RecentColors();
                const size_t i = LOWORD(wp) - ID_RECENT0;
                POINT p;
                GetCursorPos(&p);
                if (i < recent.size()) Color_Picked(recent[i], p);
            }
        }
        return 0;
    case WM_TIMER:
        if (wp == kTimerChecks) {
            KillTimer(h, kTimerChecks);
            Maint_Background();
            Update_Background();
            ScheduleChecks();
        } else if (wp == kTimerDeferred) {
            KillTimer(h, kTimerDeferred);
            Capture_Start(g_deferredMode);
        } else if (wp == kTimerResize) {
            KillTimer(h, kTimerResize);
            std::vector<std::wstring> files = std::move(g_resizeQueue);
            g_resizeQueue.clear();
            Resizer_Open(files);
        } else if (wp == kTimerRename) {
            KillTimer(h, kTimerRename);
            std::vector<std::wstring> files = std::move(g_renameQueue);
            g_renameQueue.clear();
            Renamer_Open(files);
        } else if (wp == kTimerAction) {
            KillTimer(h, kTimerAction);
            std::vector<std::pair<std::wstring, std::vector<std::wstring>>> groups;  // per action, in arrival order
            for (auto& [action, file] : g_actionQueue) {
                auto it = std::find_if(groups.begin(), groups.end(), [&](const auto& g) { return g.first == action; });
                if (it == groups.end()) it = groups.insert(groups.end(), {action, {}});
                it->second.push_back(std::move(file));
            }
            g_actionQueue.clear();
            for (const auto& [action, files] : groups) {
                try {
                    Resizer_Quick(action, files);
                } catch (const std::bad_alloc&) {  // an image too big for memory: say so instead of closing
                    Log(L"Quick action %s: out of memory", action.c_str());
                    Tray_Notify(L"Not enough memory", L"The image is too big for this action.");
                }
            }
        }
        return 0;
    case WM_DESTROY:
        if (HWND w = SettingsUI_Window()) DestroyWindow(w);
        if (HWND w = Resizer_Window()) DestroyWindow(w);
        if (HWND w = Renamer_Window()) DestroyWindow(w);
        if (HWND w = Maint_Window()) DestroyWindow(w);
        if (HWND w = Alpha_Window()) DestroyWindow(w);
        Recorder_Shutdown();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        for (int id = 1; id <= kHotkeyCount; ++id) UnregisterHotKey(h, id);
        StopKeyHook();
        KillTimer(h, kTimerChecks);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Headless self-test: grab every monitor and save a PNG. Used by the build checks.
int RunGrabTest(const wchar_t* outPath) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    Image img;
    const ULONGLONG t0 = GetTickCount64();
    if (!GrabScreen(VirtualScreenRect(), img, false)) return 2;
    const ULONGLONG t1 = GetTickCount64();
    const size_t n = wcslen(outPath);
    const bool jpg = n > 4 && (!_wcsicmp(outPath + n - 4, L".jpg") || !_wcsicmp(outPath + n - 5, L".jpeg"));
    const std::vector<BYTE> data = jpg ? EncodeJpeg(img, 90) : EncodePng(img);
    const ULONGLONG t2 = GetTickCount64();
    DeleteFileW(outPath);
    if (data.empty() || !WriteFileBytes(outPath, data)) return 3;
    Log(L"--grab test: %dx%d grab %llu ms, %s %llu ms, %zu KB", img.w, img.h, t1 - t0, jpg ? L"jpg" : L"png", t2 - t1,
        data.size() / 1024);
    return 0;
}

// Opens an image file in the editor, in this process, and exits when the editor closes.
int RunEditor(const wchar_t* file) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadSettings();
    Output_Init();
    auto r = std::make_shared<CaptureResult>();
    if (!LoadImageFile(file, r->img)) return 2;
    POINT p;
    GetCursorPos(&p);
    r->screenRect = {p.x, p.y, p.x + 1, p.y + 1};
    Editor_Open(r, file);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}

// Opens an image file in the Make alpha window, in this process, and exits when it closes.
int RunAlpha(const wchar_t* file) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    LoadSettings();
    Output_Init();
    CaptureResult meta;
    if (!LoadImageFile(file, meta.img)) return 2;
    Alpha_Open(meta.img, meta);
    if (!Alpha_Window()) return 3;
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    CoUninitialize();
    return 0;
}

// Hands a --resize / --rename file list to the running copy. Explorer starts one copy per selected file, all
// at once, so the first one may still be starting: wait for its window for a few seconds.
void SendFileList(ULONG_PTR tag, const std::vector<std::wstring>& files) {
    std::wstring blob;
    for (const std::wstring& f : files) {
        blob += f;
        blob.push_back(L'\0');
    }
    for (int i = 0; i < 60; ++i) {
        if (HWND w = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            COPYDATASTRUCT cd{tag, static_cast<DWORD>(blob.size() * sizeof(wchar_t)), blob.data()};
            DWORD_PTR res = 0;
            if (SendMessageTimeoutW(w, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&cd), SMTO_ABORTIFHUNG, 5000, &res) &&
                res)
                return;
        }
        Sleep(50);
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    // DLLs only from System32 and Canivete's own folder, never from the current folder (Explorer
    // starts Canivete in the folder of the clicked file) or PATH.
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_SYSTEM32 | LOAD_LIBRARY_SEARCH_APPLICATION_DIR);
    g_hinst = hinst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    UINT cmd = 0;
    std::vector<std::wstring> list;  // --resize / --rename files, or --do's action then files
    ULONG_PTR listTag = 0;
    RECT gifRect{};  // --gif-test / --mp4-test area and length
    int gifMs = 0;
    bool testVideo = false;
    if (argc >= 2) {
        if (!_wcsicmp(argv[1], L"--resize") || !_wcsicmp(argv[1], L"--rename")) {
            listTag = !_wcsicmp(argv[1], L"--resize") ? kCopyResize : kCopyRename;
        } else if (!_wcsicmp(argv[1], L"--do") && argc >= 3) {
            listTag = kCopyAction;
            list.push_back(argv[2]);
        } else if (!_wcsicmp(argv[1], L"--upload") && argc >= 3) {
            listTag = kCopyUpload;
        }
        if (listTag) {
            for (int i = listTag == kCopyAction ? 3 : 2; i < argc; ++i) {
                wchar_t full[MAX_PATH * 4];
                const DWORD n = GetFullPathNameW(argv[i], MAX_PATH * 4, full, nullptr);
                list.push_back(n && n < MAX_PATH * 4 ? full : argv[i]);
            }
        }
        if (!_wcsicmp(argv[1], L"--maint-admin")) {
            const int r = Maint_AdminMain(argc, argv);
            LocalFree(argv);
            return r;
        }
        if (!_wcsicmp(argv[1], L"--uninstall")) {  // run by the uninstaller: undo what Canivete added to Windows
            LocalFree(argv);
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
            Shell_Unregister();
            SetAutostart(false);
            Upload_Uninstall();  // the Google sign-in (revoked at Google too) and the saved secrets
            Log(L"--uninstall: Explorer entries, shortcuts, start with Windows and saved sign-ins removed");
            CoUninitialize();
            return 0;
        }
        if (!_wcsicmp(argv[1], L"--upload-selftest")) {  // signing and secrets against known answers
            LocalFree(argv);
            return Upload_SelfTest();
        }
        if (!_wcsicmp(argv[1], L"--set-secret") && argc >= 4 && TestMode()) {  // test copy: name value
            Upload_SaveSecret(argv[2], argv[3]);
            LocalFree(argv);
            return 0;
        }
        if (!_wcsicmp(argv[1], L"--update-test")) {  // download and check the latest release, install nothing
            LocalFree(argv);
            return Update_SelfTest();
        }
        if (!_wcsicmp(argv[1], L"--verify-update")) {  // version sha256 signature: the signature check alone
            const int r = Update_VerifyTest(argc, argv);
            LocalFree(argv);
            return r;
        }
        if (!_wcsicmp(argv[1], L"--clean-walk-test") && argc >= 3 && TestMode()) {  // test copies: cleanup on a folder
            const int r = Maint_WalkTest(argv[2]);
            LocalFree(argv);
            return r;
        }
        if (!_wcsicmp(argv[1], L"--grab") && argc >= 3) return RunGrabTest(argv[2]);
        if (!_wcsicmp(argv[1], L"--edit") && argc >= 3) return RunEditor(argv[2]);
        if (!_wcsicmp(argv[1], L"--alpha") && argc >= 3) return RunAlpha(argv[2]);
        if (!_wcsicmp(argv[1], L"--nobg-bench") && argc >= 4) return Rmbg_Bench(argv[2], argv[3]);  // in out
        if (!_wcsicmp(argv[1], L"--alpha-bench") && argc >= 4) {  // in out [x y]... [box x y w h]
            std::vector<POINT> picks;
            RECT box{};
            for (int i = 4; i + 1 < argc; i += 2) {
                if (!_wcsicmp(argv[i], L"box") && i + 4 < argc) {
                    box = {_wtoi(argv[i + 1]), _wtoi(argv[i + 2]), _wtoi(argv[i + 1]) + _wtoi(argv[i + 3]),
                           _wtoi(argv[i + 2]) + _wtoi(argv[i + 4])};
                    break;
                }
                picks.push_back({_wtoi(argv[i]), _wtoi(argv[i + 1])});
            }
            CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            LoadSettings();
            return Alpha_Bench(argv[2], argv[3], picks, box);
        }
        if (!_wcsicmp(argv[1], L"--mp4-shrink") && argc >= 7) {  // in.mp4 out.mp4 percent quality kbps (self-test)
            const VideoPreset p{L"test", L"", L"", _wtoi(argv[4]), _wtoi(argv[5]), _wtoi(argv[6])};
            return Recorder_BenchShrink(argv[2], argv[3], p);
        }
        if (!_wcsicmp(argv[1], L"--region")) cmd = static_cast<UINT>(CaptureMode::Region);
        else if (!_wcsicmp(argv[1], L"--window")) cmd = static_cast<UINT>(CaptureMode::Window);
        else if (!_wcsicmp(argv[1], L"--full")) cmd = static_cast<UINT>(CaptureMode::Full);
        else if (!_wcsicmp(argv[1], L"--color")) cmd = static_cast<UINT>(CaptureMode::Color);
        else if (!_wcsicmp(argv[1], L"--text")) cmd = static_cast<UINT>(CaptureMode::Text);
        else if (!_wcsicmp(argv[1], L"--gif")) cmd = static_cast<UINT>(CaptureMode::Gif);
        else if (!_wcsicmp(argv[1], L"--mp4")) cmd = static_cast<UINT>(CaptureMode::Video);
        else if ((!_wcsicmp(argv[1], L"--mp4-test") || !_wcsicmp(argv[1], L"--mp4-bench")) && argc >= 7) {
            gifRect = {_wtoi(argv[2]), _wtoi(argv[3]), _wtoi(argv[2]) + _wtoi(argv[4]), _wtoi(argv[3]) + _wtoi(argv[5])};
            gifMs = _wtoi(argv[6]);
            testVideo = true;
            if (!_wcsicmp(argv[1], L"--mp4-bench")) {  // x y w h ms out.mp4
                if (argc < 8) return 1;
                const std::wstring out = argv[7];
                LocalFree(argv);
                LoadSettings();
                return Recorder_BenchVideo(gifRect, gifMs, out);
            }
            if (!TestMode()) return 1;
        }
        else if ((!_wcsicmp(argv[1], L"--gif-test") || !_wcsicmp(argv[1], L"--gif-bench")) && argc >= 7) {
            gifRect = {_wtoi(argv[2]), _wtoi(argv[3]), _wtoi(argv[2]) + _wtoi(argv[4]), _wtoi(argv[3]) + _wtoi(argv[5])};
            gifMs = _wtoi(argv[6]);
            if (!_wcsicmp(argv[1], L"--gif-bench")) {
                if (argc < 8) return 1;
                const std::wstring out = argv[7];
                LocalFree(argv);
                LoadSettings();
                return Recorder_Bench(gifRect, gifMs, out);
            }
            if (!TestMode()) return 1;  // the UI test only runs in a private test copy
        }
        else if (!_wcsicmp(argv[1], L"--settings")) cmd = kCmdSettings;
        else if (!_wcsicmp(argv[1], L"--maintenance")) cmd = kCmdMaint;
        else if (!_wcsicmp(argv[1], L"--check-update")) cmd = kCmdUpdate;
        else if (!_wcsicmp(argv[1], L"--maint-check")) cmd = kCmdMaintCheck;
        else if (!_wcsicmp(argv[1], L"--click-notice")) cmd = kCmdClickNotice;
        else if (!_wcsicmp(argv[1], L"--drive-connect")) cmd = kCmdDriveConnect;
        else if (!_wcsicmp(argv[1], L"--s3-test")) cmd = kCmdS3Test;
    }
    LocalFree(argv);

    // One copy only. A second launch passes its command to the running one.
    // Test copies with different test folders (parallel test runs) are not the same copy.
    const std::wstring mutexName =
        TestMode() ? L"Local\\Canivete.Test." + std::to_wstring(std::hash<std::wstring>{}(AppDataDir()))
                   : std::wstring(L"Local\\Canivete.Running");
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName.c_str());
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (listTag) {
            if (list.empty()) list.push_back(L"");  // still opens the window
            SendFileList(listTag, list);
            return 0;
        }
        if (HWND w = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(w, WM_APP_COMMAND, cmd ? cmd : kCmdAlreadyRunning, 0);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS};
    InitCommonControlsEx(&icc);
    LoadSettings();
    g_iniTime = IniTime();

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainProc;
    wc.hInstance = hinst;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);
    // A hidden top-level window (not message-only) so it also gets broadcasts like TaskbarCreated.
    g_mainWnd = CreateWindowExW(WS_EX_TOOLWINDOW, kMainClass, kAppName, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                                hinst, nullptr);
    Theme_Init(g_mainWnd);
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    LoadTrayIcon();

    if (!TestMode()) TrayAdd();
    Capture_Init();
    Output_Init();
    RegisterHotkeysOrWarn();
    if (!TestMode()) Shell_Register();
    Log(L"Canivete %s started", CANIVETE_VERSION);
    if (cmd) PostMessageW(g_mainWnd, WM_APP_COMMAND, cmd, 0);
    g_startTick = GetTickCount64();
    ScheduleChecks();
    if (gifMs > 0) Recorder_Test(gifRect, gifMs, testVideo);
    if (listTag) {
        if (list.empty()) list.push_back(L"");  // still opens the window
        QueueList(listTag, list);
    }

    // Message loop that also wakes when the settings folder changes (no polling).
    HANDLE watch = FindFirstChangeNotificationW(AppDataDir().c_str(), FALSE,
                                                FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_FILE_NAME);
    const DWORD nHandles = (watch != INVALID_HANDLE_VALUE) ? 1 : 0;
    MSG msg{};
    bool running = true;
    while (running) {
        const DWORD r = MsgWaitForMultipleObjectsEx(nHandles, &watch, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (nHandles && r == WAIT_OBJECT_0) {
            FindNextChangeNotification(watch);
            ReloadIfChanged();
            continue;
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                running = false;
                break;
            }
            HWND dlg = SettingsUI_Window();
            if (dlg && SettingsUI_Key(msg)) continue;
            if (dlg && IsDialogMessageW(dlg, &msg)) continue;  // Tab, Enter, Esc in the Settings window
            HWND rs = Resizer_Window();
            if (rs && IsDialogMessageW(rs, &msg)) continue;
            HWND rn = Renamer_Window();
            if (rn && IsDialogMessageW(rn, &msg)) continue;
            HWND mt = Maint_Window();
            if (mt && IsDialogMessageW(mt, &msg)) continue;
            HWND gs = Recorder_Window();
            if (gs && IsDialogMessageW(gs, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (nHandles) FindCloseChangeNotification(watch);
    if (g_icon) DestroyIcon(g_icon);
    Gdiplus_Shutdown();
    CoUninitialize();
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
