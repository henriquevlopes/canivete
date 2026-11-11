// Canivete's own notices: a small flyout in the bottom-right corner, instead of Windows' tray balloons.
// Notice_Show is a message that goes away by itself; Notice_Progress stays (with a progress bar) until
// Notice_Hide. All three can be called from any thread.
#include "app.h"
#include "fluent.h"
#include "theme.h"

#include <shellscalingapi.h>

#include <algorithm>

namespace {

constexpr wchar_t kClass[] = L"CaniveteNotice";
constexpr UINT WM_N_SHOW = WM_APP + 1, WM_N_HIDE = WM_APP + 2;
constexpr UINT_PTR kTimerHold = 1, kTimerAnim = 2;

struct Req {
    std::wstring title, text;
    std::function<void()> click;
    int pos = -2;  // -2 = a plain message, -1 = bar that slides, 0..1000 = bar
};

struct State {
    HWND hwnd = nullptr;
    std::wstring title, text;
    std::function<void()> click;
    int pos = -2;
    UINT dpi = 96;
} N;

int Px(int v) { return MulDiv(v, static_cast<int>(N.dpi), 96); }

void Place() {
    POINT pt{};
    GetCursorPos(&pt);
    HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) || !dx) dx = 96;
    N.dpi = dx;
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    const int w = Px(360), h = Px(N.pos == -2 ? 76 : 92), m = Px(16);
    SetWindowPos(N.hwnd, HWND_TOPMOST, mi.rcWork.right - w - m, mi.rcWork.bottom - h - m, w, h,
                 SWP_NOACTIVATE | SWP_SHOWWINDOW);
}

void Paint(HDC dc) {
    RECT rc;
    GetClientRect(N.hwnd, &rc);
    const Palette& p = Pal();
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    HBRUSH bg = CreateSolidBrush(p.card);
    FillRect(mem, &rc, bg);
    DeleteObject(bg);
    SetBkMode(mem, TRANSPARENT);
    const int pad = Px(16);
    RECT tr{pad, Px(12), rc.right - pad, Px(34)};
    HGDIOBJ of = SelectObject(mem, Fluent_Font(kTypeBodyStrong, N.dpi));
    SetTextColor(mem, p.text);
    DrawTextW(mem, N.title.c_str(), -1, &tr, DT_LEFT | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    const bool bar = N.pos != -2;
    RECT xr{pad, Px(36), rc.right - pad, bar ? rc.bottom - Px(26) : rc.bottom - Px(10)};
    SelectObject(mem, Fluent_Font(kTypeBody, N.dpi));
    SetTextColor(mem, p.grey);
    DrawTextW(mem, N.text.c_str(), -1, &xr, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(mem, of);
    if (bar) {
        RECT br{pad, rc.bottom - Px(22), rc.right - pad, rc.bottom - Px(14)};
        Fluent_DrawProgress(mem, br, N.dpi, N.pos, GetTickCount64(), p.accent, p.card);
    }
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_N_SHOW: {
        std::unique_ptr<Req> r(reinterpret_cast<Req*>(lp));
        const bool wasProgress = N.pos != -2 && IsWindowVisible(h);
        // A plain message must not wipe out a running progress bar's window (it shows after the bar ends).
        if (r->pos == -2 && wasProgress) return 0;
        N.title = r->title;
        N.text = r->text;
        N.click = std::move(r->click);
        const bool sizeChanged = (r->pos == -2) != (N.pos == -2) || !IsWindowVisible(h);
        N.pos = r->pos;
        if (sizeChanged) Place();
        InvalidateRect(h, nullptr, FALSE);
        KillTimer(h, kTimerHold);
        KillTimer(h, kTimerAnim);
        if (N.pos == -2) SetTimer(h, kTimerHold, N.click ? 8000 : 5000, nullptr);
        else if (N.pos == -1) SetTimer(h, kTimerAnim, 30, nullptr);
        return 0;
    }
    case WM_N_HIDE:
        KillTimer(h, kTimerHold);
        KillTimer(h, kTimerAnim);
        N.pos = -2;
        N.click = nullptr;
        ShowWindow(h, SW_HIDE);
        return 0;
    case WM_TIMER:
        if (wp == kTimerHold) {
            KillTimer(h, kTimerHold);
            N.click = nullptr;
            ShowWindow(h, SW_HIDE);
        } else {
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {
        auto f = std::move(N.click);
        N.click = nullptr;
        if (N.pos == -2) {
            KillTimer(h, kTimerHold);
            ShowWindow(h, SW_HIDE);
        }
        if (f) f();
        return 0;
    }
    case WM_RBUTTONUP:
        if (N.pos == -2) {
            KillTimer(h, kTimerHold);
            N.click = nullptr;
            ShowWindow(h, SW_HIDE);
        }
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, N.click ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        Paint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    }
    return DefWindowProcW(h, m, wp, lp);
}

void Post(Req* r) {
    if (!N.hwnd || !PostMessageW(N.hwnd, WM_N_SHOW, 0, reinterpret_cast<LPARAM>(r))) delete r;
}

}  // namespace

void Notice_Init() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = kClass;
    RegisterClassExW(&wc);
    N.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, kClass, L"Canivete", WS_POPUP, 0, 0, 1,
                             1, nullptr, nullptr, g_hinst, nullptr);
    if (N.hwnd) {
        Fluent_StylePopup(N.hwnd);
        if (!TestMode()) SetWindowDisplayAffinity(N.hwnd, WDA_EXCLUDEFROMCAPTURE);
    }
}

void Notice_Show(const wchar_t* title, const std::wstring& text, std::function<void()> onClick) {
    if (TestMode()) {
        Log(L"Notice: %s - %s", title, text.c_str());
        return;
    }
    auto* r = new Req;
    r->title = title;
    r->text = text;
    r->click = std::move(onClick);
    Post(r);
}

void Notice_Progress(const wchar_t* title, const std::wstring& text, int pos) {
    if (TestMode()) return;
    auto* r = new Req;
    r->title = title;
    r->text = text;
    r->pos = std::clamp(pos, -1, 1000);
    Post(r);
}

void Notice_Hide() {
    if (N.hwnd) PostMessageW(N.hwnd, WM_N_HIDE, 0, 0);
}
