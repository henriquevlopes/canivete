// Canivete - a small tray toolbox. Entry point: tray icon, global hotkeys, menu, settings reload.
#include "app.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>

HINSTANCE g_hinst = nullptr;
HWND g_mainWnd = nullptr;

namespace {

enum : UINT { ID_REGION = 100, ID_WINDOW, ID_FULL, ID_COLOR, ID_OPEN_FOLDER, ID_SETTINGS, ID_EXIT, ID_RESIZE, ID_RENAME };
constexpr UINT ID_RECENT0 = 200;  // .. 209: recent colors in the tray menu
constexpr UINT kCmdAlreadyRunning = 99;
constexpr UINT kCmdSettings = 98;
constexpr UINT_PTR kTimerDeferred = 10;
constexpr UINT_PTR kTimerResize = 11;
constexpr UINT_PTR kTimerRename = 12;
constexpr UINT_PTR kTimerAction = 13;
constexpr ULONG_PTR kCopyResize = 0x52535A31;  // WM_COPYDATA tags: file list for the resizer
constexpr ULONG_PTR kCopyRename = 0x524E4D31;  // ... and for the renamer
constexpr ULONG_PTR kCopyAction = 0x41435431;  // ... and for a quick action (first entry = action name)
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
// Those are caught with a low-level keyboard hook instead, like PowerToys does.
struct HookKey {
    int id;
    Hotkey hk;
    bool down;
};
std::vector<HookKey> g_hookKeys;
HHOOK g_kbHook = nullptr;

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

void UpdateKeyHook() {
    if (!g_hookKeys.empty() && !g_kbHook) {
        g_kbHook = SetWindowsHookExW(WH_KEYBOARD_LL, WinKeyHook, g_hinst, 0);
    } else if (g_hookKeys.empty() && g_kbHook) {
        UnhookWindowsHookEx(g_kbHook);
        g_kbHook = nullptr;
    }
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

void TrayBalloon(const wchar_t* title, const std::wstring& text) {
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
    } const list[] = {
        {CaptureMode::Region, &g_settings.hkRegion, L"Region screenshot"},
        {CaptureMode::Window, &g_settings.hkWindow, L"Window screenshot"},
        {CaptureMode::Full, &g_settings.hkFull, L"Full screenshot"},
        {CaptureMode::Color, &g_settings.hkColor, L"Color picker"},
    };
    std::wstring failed;
    for (const Entry& e : list) UnregisterHotKey(g_mainWnd, static_cast<int>(e.mode));
    g_hookKeys.clear();
    if (g_hotkeysSuspended || TestMode()) {
        UpdateKeyHook();
        return failed;
    }
    for (const Entry& e : list) {
        const int id = static_cast<int>(e.mode);
        Hotkey hk;
        if (!ParseHotkey(*e.text, hk)) {
            if (!e.text->empty() && _wcsicmp(e.text->c_str(), L"none") != 0)
                failed += std::wstring(e.name) + L": can't read \"" + *e.text + L"\"\n";
            continue;
        }
        if (RegisterHotKey(g_mainWnd, id, hk.mods, hk.vk)) continue;
        if (hk.mods & MOD_WIN) g_hookKeys.push_back({id, hk, false});  // reserved by Windows: use the hook
        else failed += std::wstring(e.name) + L": " + *e.text + L" is used by another app\n";
    }
    UpdateKeyHook();
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

void ReloadIfChanged() {
    const FILETIME t = IniTime();
    if (CompareFileTime(&t, &g_iniTime) == 0) return;
    g_iniTime = t;
    LoadSettings();
    RegisterHotkeysOrWarn();
    Log(L"Settings reloaded");
}

}  // namespace

void Tray_Notify(const wchar_t* title, const std::wstring& text) { TrayBalloon(title, text); }

std::wstring Settings_Applied() {
    LoadSettings();
    g_iniTime = IniTime();  // so the file watcher doesn't reload it a second time
    Log(L"Settings saved");
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
    item(ID_REGION, L"Region screenshot", g_settings.hkRegion);
    item(ID_WINDOW, L"Window screenshot", g_settings.hkWindow);
    item(ID_FULL, L"Full screenshot", g_settings.hkFull);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    item(ID_COLOR, L"Pick a color", g_settings.hkColor);
    // Recent colors: a swatch and the value in the copy format; clicking copies it again.
    std::vector<HBITMAP> swatches;
    const std::vector<COLORREF> recent = RecentColors();
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
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_RESIZE, L"Resize images…");
    AppendMenuW(m, MF_STRING, ID_RENAME, L"Rename files…");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
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
    case WM_HOTKEY:
        if (wp >= 1 && wp <= kHotkeyCount) Capture_Start(static_cast<CaptureMode>(wp));
        return 0;
    case WM_APP_TRAY:
        switch (LOWORD(lp)) {
        case WM_CONTEXTMENU:
        case NIN_SELECT:
        case NIN_KEYSELECT:
            ShowMenu();
        }
        return 0;
    case WM_APP_CAPTURE_DONE:
        Output_OnDone(lp);
        return 0;
    case WM_APP_COMMAND:
        if (wp >= 1 && wp <= kHotkeyCount) Capture_Start(static_cast<CaptureMode>(wp));
        else if (wp == kCmdSettings)
            SettingsUI_Show();
        else if (wp == kCmdAlreadyRunning)
            TrayBalloon(L"Canivete is already running", L"Right-click the red knife icon in the tray.");
        return 0;
    case WM_COPYDATA: {  // --resize / --rename / --do from another copy: ' '-separated entries
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cd->dwData != kCopyResize && cd->dwData != kCopyRename && cd->dwData != kCopyAction) return FALSE;
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
        case ID_REGION:
        case ID_WINDOW:
        case ID_FULL:
        case ID_COLOR:
            // Wait for the menu to fade out so it isn't in the screenshot.
            g_deferredMode = static_cast<CaptureMode>(LOWORD(wp) - ID_REGION + 1);
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
        if (wp == kTimerDeferred) {
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
            for (const auto& [action, files] : groups) Resizer_Quick(action, files);
        }
        return 0;
    case WM_DESTROY:
        if (HWND w = SettingsUI_Window()) DestroyWindow(w);
        if (HWND w = Resizer_Window()) DestroyWindow(w);
        if (HWND w = Renamer_Window()) DestroyWindow(w);
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        for (int id = 1; id <= kHotkeyCount; ++id) UnregisterHotKey(h, id);
        g_hookKeys.clear();
        UpdateKeyHook();
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
    g_hinst = hinst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    UINT cmd = 0;
    std::vector<std::wstring> list;  // --resize / --rename files, or --do's action then files
    ULONG_PTR listTag = 0;
    if (argc >= 2) {
        if (!_wcsicmp(argv[1], L"--resize") || !_wcsicmp(argv[1], L"--rename")) {
            listTag = !_wcsicmp(argv[1], L"--resize") ? kCopyResize : kCopyRename;
        } else if (!_wcsicmp(argv[1], L"--do") && argc >= 3) {
            listTag = kCopyAction;
            list.push_back(argv[2]);
        }
        if (listTag) {
            for (int i = listTag == kCopyAction ? 3 : 2; i < argc; ++i) {
                wchar_t full[MAX_PATH * 4];
                const DWORD n = GetFullPathNameW(argv[i], MAX_PATH * 4, full, nullptr);
                list.push_back(n && n < MAX_PATH * 4 ? full : argv[i]);
            }
        }
        if (!_wcsicmp(argv[1], L"--grab") && argc >= 3) return RunGrabTest(argv[2]);
        if (!_wcsicmp(argv[1], L"--edit") && argc >= 3) return RunEditor(argv[2]);
        if (!_wcsicmp(argv[1], L"--region")) cmd = static_cast<UINT>(CaptureMode::Region);
        else if (!_wcsicmp(argv[1], L"--window")) cmd = static_cast<UINT>(CaptureMode::Window);
        else if (!_wcsicmp(argv[1], L"--full")) cmd = static_cast<UINT>(CaptureMode::Full);
        else if (!_wcsicmp(argv[1], L"--color")) cmd = static_cast<UINT>(CaptureMode::Color);
        else if (!_wcsicmp(argv[1], L"--settings")) cmd = kCmdSettings;
    }
    LocalFree(argv);

    // One copy only. A second launch passes its command to the running one.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, TestMode() ? L"Local\\Canivete.Test" : L"Local\\Canivete.Running");
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
    g_taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    LoadIconMetric(hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &g_icon);

    if (!TestMode()) TrayAdd();
    Capture_Init();
    Output_Init();
    RegisterHotkeysOrWarn();
    if (!TestMode()) Shell_Register();
    Log(L"Canivete %s started", CANIVETE_VERSION);
    if (cmd) PostMessageW(g_mainWnd, WM_APP_COMMAND, cmd, 0);
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
            if (dlg && IsDialogMessageW(dlg, &msg)) continue;  // Tab, Enter, Esc in the Settings window
            HWND rs = Resizer_Window();
            if (rs && IsDialogMessageW(rs, &msg)) continue;
            HWND rn = Renamer_Window();
            if (rn && IsDialogMessageW(rn, &msg)) continue;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
    if (nHandles) FindCloseChangeNotification(watch);
    if (g_icon) DestroyIcon(g_icon);
    CoUninitialize();
    CloseHandle(mutex);
    return static_cast<int>(msg.wParam);
}
