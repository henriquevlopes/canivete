// The "A new version is available" window: current and new version, what changed, and one Update button.
// Update downloads, installs and restarts Canivete by itself; the same window shows the progress bar.
#include "app.h"
#include "fluent.h"
#include "theme.h"

#include <commctrl.h>

#include <algorithm>

namespace {

constexpr wchar_t kClass[] = L"CaniveteUpdateDialog";
enum : int { IDC_HEAD = 10, IDC_VERS, IDC_LOG, IDC_STATE, IDC_BAR, IDC_GO, IDC_LATER };

struct State {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    bool working = false;
} D;

int Px(int v) { return MulDiv(v, static_cast<int>(D.dpi), 96); }

HWND Child(int id) { return GetDlgItem(D.hwnd, id); }

void Layout() {
    RECT rc;
    GetClientRect(D.hwnd, &rc);
    const int w = rc.right, h = rc.bottom, m = Px(20);
    auto put = [&](int id, int x, int y, int cw, int ch) { SetWindowPos(Child(id), nullptr, x, y, cw, ch, SWP_NOZORDER); };
    put(IDC_HEAD, m, Px(16), w - 2 * m, Px(24));
    put(IDC_VERS, m, Px(46), w - 2 * m, Px(22));
    put(IDC_LOG, m, Px(80), w - 2 * m, h - Px(80) - Px(120));
    put(IDC_STATE, m, h - Px(108), w - 2 * m, Px(22));
    put(IDC_BAR, m, h - Px(80), w - 2 * m, Px(8));
    const int bw = Px(120), bh = Px(32), by = h - Px(20) - bh;
    put(IDC_GO, w - m - bw, by, bw, bh);
    put(IDC_LATER, w - m - 2 * bw - Px(8), by, bw, bh);
}

void SetWorking(bool on) {
    D.working = on;
    ShowWindow(Child(IDC_BAR), on ? SW_SHOWNA : SW_HIDE);
    ShowWindow(Child(IDC_STATE), on ? SW_SHOWNA : SW_HIDE);
    EnableWindow(Child(IDC_GO), !on);
    EnableWindow(Child(IDC_LATER), !on);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_SIZE:
        Layout();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_GO && !D.working) {
            SetWorking(true);
            Update_Install();  // downloads, installs and restarts; progress comes back to this window
        } else if (LOWORD(wp) == IDC_LATER && !D.working) {
            DestroyWindow(h);
        }
        return 0;
    case WM_CLOSE:
        if (!D.working) DestroyWindow(h);  // (while installing it stays: the app is about to restart)
        return 0;
    case WM_DESTROY:
        D.hwnd = nullptr;
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

void UpdateDlg_Show() {
    if (TestMode()) return;
    if (D.hwnd) {
        ShowWindow(D.hwnd, SW_SHOWNORMAL);
        SetForegroundWindow(D.hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = Proc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClass;
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(1), LIM_SMALL, &wc.hIconSm);
        registered = RegisterClassExW(&wc) != 0;
    }
    const UpdateInfo& u = Update_Info();
    POINT pt{};
    GetCursorPos(&pt);
    D.dpi = GetDpiForSystem();
    const int w = Px(560), h = Px(500);
    D.hwnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"Canivete update", WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT,
                             CW_USEDEFAULT, w, h, nullptr, nullptr, g_hinst, nullptr);
    if (!D.hwnd) return;
    D.dpi = GetDpiForWindow(D.hwnd);
    auto make = [&](const wchar_t* cls, const wchar_t* text, DWORD style, int id, DWORD ex = 0) {
        return CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 10, 10, D.hwnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hinst, nullptr);
    };
    HWND head = make(WC_STATICW, L"A new version of Canivete is available", SS_LEFT | SS_NOPREFIX, IDC_HEAD);
    const std::wstring ver = L"Current version: " + std::wstring(CANIVETE_VERSION) + L"      New version: " + u.latest;
    HWND vers = make(WC_STATICW, ver.c_str(), SS_LEFT | SS_NOPREFIX, IDC_VERS);
    const std::wstring notes = u.notes.empty() ? L"No notes were published for this version." : u.notes;
    HWND log = make(WC_EDITW, notes.c_str(), ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, IDC_LOG, WS_EX_CLIENTEDGE);
    make(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, IDC_STATE);
    make(PROGRESS_CLASSW, L"", PBS_SMOOTH | PBS_MARQUEE, IDC_BAR);
    make(WC_BUTTONW, L"Update", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_GO);
    make(WC_BUTTONW, L"Later", BS_PUSHBUTTON | WS_TABSTOP, IDC_LATER);
    SendMessageW(Child(IDC_BAR), PBM_SETMARQUEE, TRUE, 30);
    SendMessageW(head, WM_SETFONT, reinterpret_cast<WPARAM>(Fluent_Font(kTypeBodyStrong, D.dpi)), TRUE);
    for (int id : {IDC_VERS, IDC_STATE, IDC_GO, IDC_LATER})
        SendMessageW(Child(id), WM_SETFONT, reinterpret_cast<WPARAM>(Fluent_Font(kTypeBody, D.dpi)), TRUE);
    SendMessageW(log, WM_SETFONT, reinterpret_cast<WPARAM>(Fluent_Font(kTypeBody, D.dpi)), TRUE);
    (void)vers;
    SetWorking(false);
    RECT rc{0, 0, Px(560), Px(500)};
    AdjustWindowRectExForDpi(&rc, WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOPMOST, D.dpi);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const int ww = rc.right - rc.left, wh = rc.bottom - rc.top;
    SetWindowPos(D.hwnd, HWND_TOPMOST, (mi.rcWork.left + mi.rcWork.right - ww) / 2, (mi.rcWork.top + mi.rcWork.bottom - wh) / 2, ww, wh,
                 SWP_NOACTIVATE);
    Layout();
    Theme_Window(D.hwnd);
    ShowWindow(D.hwnd, SW_SHOWNORMAL);
    SetForegroundWindow(D.hwnd);
}

// pos: -1 = sliding bar, 0..1000 = download done. text: the line above the bar.
void UpdateDlg_Progress(int pos, const std::wstring& text) {
    if (!D.hwnd) return;
    if (!D.working) SetWorking(true);
    SetWindowTextW(Child(IDC_STATE), text.c_str());
    HWND bar = Child(IDC_BAR);
    LONG st = GetWindowLongW(bar, GWL_STYLE);
    if (pos < 0) {
        SetWindowLongW(bar, GWL_STYLE, st | PBS_MARQUEE);
        SendMessageW(bar, PBM_SETMARQUEE, TRUE, 30);
    } else {
        SendMessageW(bar, PBM_SETMARQUEE, FALSE, 0);
        SetWindowLongW(bar, GWL_STYLE, st & ~PBS_MARQUEE);
        SendMessageW(bar, PBM_SETRANGE32, 0, 1000);
        SendMessageW(bar, PBM_SETPOS, pos, 0);
    }
}

// It didn't work: say why, and let the person try again or close.
void UpdateDlg_Failed(const std::wstring& why) {
    if (!D.hwnd) return;
    SetWorking(false);
    ShowWindow(Child(IDC_STATE), SW_SHOWNA);
    SetWindowTextW(Child(IDC_STATE), (L"The update didn't work: " + why).c_str());
    SetWindowTextW(Child(IDC_GO), L"Try again");
}
