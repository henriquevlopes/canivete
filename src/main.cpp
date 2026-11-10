// Canivete - a small tray toolbox. Entry point: tray icon, global hotkeys, menu, settings reload.
#include "app.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

HINSTANCE g_hinst = nullptr;
HWND g_mainWnd = nullptr;

namespace {

enum : UINT { ID_REGION = 100, ID_WINDOW, ID_FULL, ID_OPEN_FOLDER, ID_SETTINGS, ID_AUTOSTART, ID_EXIT };
constexpr UINT kCmdAlreadyRunning = 99;
constexpr UINT_PTR kTimerDeferred = 10;
constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";

UINT g_taskbarCreated = 0;
NOTIFYICONDATAW g_nid{};
HICON g_icon = nullptr;
CaptureMode g_deferredMode = CaptureMode::Region;
FILETIME g_iniTime{};

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
    NOTIFYICONDATAW n = g_nid;
    n.uFlags = NIF_INFO;
    wcsncpy_s(n.szInfoTitle, title, _TRUNCATE);
    wcsncpy_s(n.szInfo, text.c_str(), _TRUNCATE);
    n.dwInfoFlags = NIIF_INFO | NIIF_NOSOUND;
    Shell_NotifyIconW(NIM_MODIFY, &n);
}

void RegisterHotkeys() {
    struct Entry {
        CaptureMode mode;
        const std::wstring* text;
        const wchar_t* name;
    } const list[] = {
        {CaptureMode::Region, &g_settings.hkRegion, L"Region screenshot"},
        {CaptureMode::Window, &g_settings.hkWindow, L"Window screenshot"},
        {CaptureMode::Full, &g_settings.hkFull, L"Full screenshot"},
    };
    std::wstring failed;
    for (const Entry& e : list) {
        const int id = static_cast<int>(e.mode);
        UnregisterHotKey(g_mainWnd, id);
        Hotkey hk;
        if (!ParseHotkey(*e.text, hk)) {
            if (!e.text->empty() && _wcsicmp(e.text->c_str(), L"none") != 0)
                failed += std::wstring(e.name) + L": can't read \"" + *e.text + L"\"\n";
            continue;
        }
        if (!RegisterHotKey(g_mainWnd, id, hk.mods, hk.vk))
            failed += std::wstring(e.name) + L": " + *e.text + L" is used by another app\n";
    }
    if (!failed.empty()) {
        Log(L"Hotkey problems: %s", failed.c_str());
        TrayBalloon(L"Some Canivete hotkeys don't work", failed);
    }
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
    RegisterHotkeys();
    Log(L"Settings reloaded");
}

void ShowMenu() {
    HMENU m = CreatePopupMenu();
    auto item = [&](UINT id, const wchar_t* text, const std::wstring& hk) {
        std::wstring s = text;
        if (!hk.empty()) s += L"\t" + hk;
        AppendMenuW(m, MF_STRING, id, s.c_str());
    };
    item(ID_REGION, L"Region screenshot", g_settings.hkRegion);
    item(ID_WINDOW, L"Window screenshot", g_settings.hkWindow);
    item(ID_FULL, L"Full screenshot", g_settings.hkFull);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_OPEN_FOLDER, L"Open screenshots folder");
    AppendMenuW(m, MF_STRING, ID_SETTINGS, L"Settings…");
    AppendMenuW(m, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), ID_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, ID_EXIT, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(g_mainWnd);
    TrackPopupMenuEx(m, TPM_RIGHTBUTTON, p.x, p.y, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
}

LRESULT CALLBACK MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (g_taskbarCreated && msg == g_taskbarCreated) {  // Explorer restarted: put the icon back
        TrayAdd();
        return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        if (wp >= 1 && wp <= 3) Capture_Start(static_cast<CaptureMode>(wp));
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
        if (wp >= 1 && wp <= 3) Capture_Start(static_cast<CaptureMode>(wp));
        else if (wp == kCmdAlreadyRunning)
            TrayBalloon(L"Canivete is already running", L"Right-click the red knife icon in the tray.");
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case ID_REGION:
        case ID_WINDOW:
        case ID_FULL:
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
            ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + SettingsPath() + L"\"").c_str(), nullptr,
                          SW_SHOWNORMAL);
            break;
        case ID_AUTOSTART:
            SetAutostart(!AutostartEnabled());
            break;
        case ID_EXIT:
            DestroyWindow(h);
            break;
        }
        return 0;
    case WM_TIMER:
        if (wp == kTimerDeferred) {
            KillTimer(h, kTimerDeferred);
            Capture_Start(g_deferredMode);
        }
        return 0;
    case WM_DESTROY:
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        for (int id = 1; id <= 3; ++id) UnregisterHotKey(h, id);
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
    const std::vector<BYTE> png = EncodePng(img);
    const ULONGLONG t2 = GetTickCount64();
    DeleteFileW(outPath);
    if (png.empty() || !WriteFileBytes(outPath, png)) return 3;
    Log(L"--grab test: %dx%d grab %llu ms, png %llu ms, %zu KB", img.w, img.h, t1 - t0, t2 - t1, png.size() / 1024);
    return 0;
}

}  // namespace

int WINAPI wWinMain(HINSTANCE hinst, HINSTANCE, PWSTR, int) {
    g_hinst = hinst;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    UINT cmd = 0;
    if (argc >= 2) {
        if (!_wcsicmp(argv[1], L"--grab") && argc >= 3) return RunGrabTest(argv[2]);
        if (!_wcsicmp(argv[1], L"--region")) cmd = static_cast<UINT>(CaptureMode::Region);
        else if (!_wcsicmp(argv[1], L"--window")) cmd = static_cast<UINT>(CaptureMode::Window);
        else if (!_wcsicmp(argv[1], L"--full")) cmd = static_cast<UINT>(CaptureMode::Full);
    }
    LocalFree(argv);

    // One copy only. A second launch passes its command to the running one.
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Canivete.Running");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        if (HWND w = FindWindowW(kMainClass, nullptr)) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w, &pid);
            AllowSetForegroundWindow(pid);
            PostMessageW(w, WM_APP_COMMAND, cmd ? cmd : kCmdAlreadyRunning, 0);
        }
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
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

    TrayAdd();
    Capture_Init();
    Output_Init();
    RegisterHotkeys();
    Log(L"Canivete %s started", CANIVETE_VERSION);
    if (cmd) PostMessageW(g_mainWnd, WM_APP_COMMAND, cmd, 0);

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
