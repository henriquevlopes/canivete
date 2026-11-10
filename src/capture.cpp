// Screenshots: grab the screen with GDI, then (for region mode) show a frozen full-desktop overlay
// where the user drags a rectangle or clicks a highlighted window / monitor.
// All coordinates are physical pixels (the app is Per-Monitor-V2 DPI aware).
#include "app.h"

#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

namespace {

constexpr wchar_t kOverlayClass[] = L"CaniveteOverlay";
constexpr int kMag = 17;  // magnifier shows kMag x kMag source pixels (odd: one centre pixel)
constexpr COLORREF kAccent = RGB(232, 44, 56);

struct State {
    HWND hwnd = nullptr;
    HWND prevForeground = nullptr;
    bool active = false;
    bool picking = false;      // color picker: no dimming, no selection, click picks the pixel
    RECT vr{};                 // virtual desktop, screen coords
    Dib frozen, dim, back, mag;
    std::vector<RECT> windows;   // top-level windows, z-order (topmost first), screen coords
    std::vector<RECT> monitors;  // screen coords
    POINT cur{};                 // cursor, screen coords
    POINT anchor{};
    bool down = false, dragging = false;
    RECT sel{};                  // current selection / highlight, screen coords
    RECT prevDecor{};            // area painted last time, client coords
    std::wstring proc, title;
    HFONT font = nullptr;
    int fontPx = 0;
} S;

struct Layout {
    RECT sel{}, label{}, mag{};  // client coords
    std::wstring labelText;
    int cell = 8;
} L;

RECT ToClient(RECT r) {
    OffsetRect(&r, -S.vr.left, -S.vr.top);
    return r;
}

double ScaleAt(POINT p) {
    UINT dx = 96, dy = 96;
    HMONITOR m = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    if (FAILED(GetDpiForMonitor(m, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    return dx / 96.0;
}

RECT MonitorAt(POINT p) {
    for (const RECT& r : S.monitors)
        if (PtInRect(&r, p)) return r;
    return S.vr;
}

RECT MonitorUnderCursor() {
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcMonitor;
}

RECT WindowBounds(HWND h) {
    RECT r{};
    // Extended frame bounds = the visible window, without the invisible resize borders.
    if (FAILED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof(r)))) GetWindowRect(h, &r);
    return r;
}

void WindowInfo(HWND h, std::wstring& proc, std::wstring& title) {
    proc.clear();
    title.clear();
    if (!h) return;
    wchar_t buf[512];
    if (GetWindowTextW(h, buf, 512) > 0) title = buf;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        DWORD n = 512;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
            std::wstring full = buf;
            size_t slash = full.find_last_of(L'\\');
            proc = full.substr(slash == std::wstring::npos ? 0 : slash + 1);
            size_t dot = proc.find_last_of(L'.');
            if (dot != std::wstring::npos) proc.resize(dot);
        }
        CloseHandle(p);
    }
}

bool IsShellBackground(HWND h) {
    if (!h) return true;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    return !wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") ||
           !wcscmp(cls, L"Shell_SecondaryTrayWnd");
}

BOOL CALLBACK EnumWindowsProc(HWND h, LPARAM) {
    if (!IsWindowVisible(h) || IsIconic(h)) return TRUE;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;
    DWORD cloaked = 0;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    if (cloaked) return TRUE;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"CEF-OSC-WIDGET")) return TRUE;
    const LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    const bool taskbar = !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd");
    if (!taskbar && (ex & (WS_EX_TOOLWINDOW | WS_EX_TRANSPARENT))) return TRUE;
    RECT r = WindowBounds(h);
    if (!IntersectRect(&r, &r, &S.vr)) return TRUE;
    if (r.right - r.left < 8 || r.bottom - r.top < 8) return TRUE;
    S.windows.push_back(r);
    return TRUE;
}

BOOL CALLBACK EnumMonitorsProc(HMONITOR, HDC, LPRECT r, LPARAM) {
    S.monitors.push_back(*r);
    return TRUE;
}

void DrawCursor(HDC dc, int ox, int oy) {
    CURSORINFO ci{sizeof(ci)};
    if (!GetCursorInfo(&ci) || !(ci.flags & CURSOR_SHOWING)) return;
    ICONINFO ii{};
    if (!GetIconInfo(ci.hCursor, &ii)) return;
    DrawIconEx(dc, ci.ptScreenPos.x - static_cast<int>(ii.xHotspot) - ox,
               ci.ptScreenPos.y - static_cast<int>(ii.yHotspot) - oy, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
}

bool GrabInto(Dib& d, const RECT& r, bool withCursor) {
    const int w = r.right - r.left, h = r.bottom - r.top;
    if (w <= 0 || h <= 0 || !d.Create(w, h)) return false;
    HDC screen = GetDC(nullptr);
    const BOOL ok = BitBlt(d.dc, 0, 0, w, h, screen, r.left, r.top, SRCCOPY | CAPTUREBLT);
    ReleaseDC(nullptr, screen);
    if (withCursor) DrawCursor(d.dc, r.left, r.top);
    GdiFlush();
    return ok != FALSE;
}

void FillRectColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// Frame drawn just outside r, t pixels thick.
void FrameOutside(HDC dc, const RECT& r, COLORREF c, int t) {
    FillRectColor(dc, {r.left - t, r.top - t, r.right + t, r.top}, c);
    FillRectColor(dc, {r.left - t, r.bottom, r.right + t, r.bottom + t}, c);
    FillRectColor(dc, {r.left - t, r.top, r.left, r.bottom}, c);
    FillRectColor(dc, {r.right, r.top, r.right + t, r.bottom}, c);
}

void EnsureFont(double scale) {
    const int px = std::max(11, static_cast<int>(std::lround(13 * scale)));
    if (S.font && S.fontPx == px) return;
    if (S.font) DeleteObject(S.font);
    S.font = CreateFontW(-px, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                         L"Segoe UI");
    S.fontPx = px;
    SelectObject(S.back.dc, S.font);
}

// Works out where the selection label and magnifier go. Returns the area they cover (client).
RECT ComputeLayout() {
    const double s = ScaleAt(S.cur);
    EnsureFont(s);
    L.sel = ToClient(S.sel);
    const int pad = static_cast<int>(std::lround(5 * s));

    wchar_t buf[64];
    swprintf_s(buf, L"%d × %d", S.sel.right - S.sel.left, S.sel.bottom - S.sel.top);
    L.labelText = buf;
    SIZE ts{};
    GetTextExtentPoint32W(S.back.dc, buf, static_cast<int>(wcslen(buf)), &ts);
    const int lw = ts.cx + 2 * pad, lh = ts.cy + pad;
    const int infoH = S.picking ? 2 * lh : lh;
    int lx = L.sel.left, ly = L.sel.top - lh - 3;
    const RECT selMon = ToClient(MonitorAt({S.sel.left, S.sel.top}));
    if (ly < selMon.top) {
        lx = L.sel.left + 3;
        ly = L.sel.top + 3;
    }
    L.label = {lx, ly, lx + lw, ly + lh};
    if (S.picking) L.sel = L.label = {};

    L.cell = std::max(4, static_cast<int>(std::lround((S.picking ? 12 : 8) * s)));
    const int M = kMag * L.cell;
    const int off = static_cast<int>(std::lround(24 * s));
    const RECT mon = ToClient(MonitorAt(S.cur));
    const POINT c{S.cur.x - S.vr.left, S.cur.y - S.vr.top};
    int mx = c.x + off, my = c.y + off;
    if (mx + M > mon.right) mx = c.x - off - M;
    if (my + M + infoH > mon.bottom) my = c.y - off - M - infoH;
    L.mag = {mx, my, mx + M, my + M + infoH};
    if (S.picking) {
        RECT m = L.mag;
        InflateRect(&m, 3, 3);
        return m;
    }

    RECT d = L.sel;
    InflateRect(&d, 4, 4);
    UnionRect(&d, &d, &L.label);
    if (g_settings.showMagnifier) {
        RECT m = L.mag;
        InflateRect(&m, 3, 3);
        UnionRect(&d, &d, &m);
    }
    return d;
}

void DrawMagnifier(HDC dc) {
    const int half = kMag / 2;
    const int cx = S.cur.x - S.vr.left, cy = S.cur.y - S.vr.top;
    for (int j = 0; j < kMag; ++j)
        for (int i = 0; i < kMag; ++i) {
            const int sx = cx - half + i, sy = cy - half + j;
            const bool in = sx >= 0 && sy >= 0 && sx < S.frozen.w && sy < S.frozen.h;
            S.mag.bits[j * kMag + i] = in ? S.frozen.bits[sy * S.frozen.w + sx] : 0x202020;
        }
    const int cell = L.cell, M = kMag * cell;
    const RECT r{L.mag.left, L.mag.top, L.mag.left + M, L.mag.top + M};
    SetStretchBltMode(dc, COLORONCOLOR);
    StretchBlt(dc, r.left, r.top, M, M, S.mag.dc, 0, 0, kMag, kMag, SRCCOPY);
    if (cell >= 6) {
        SetDCPenColor(dc, RGB(64, 64, 64));
        HGDIOBJ oldPen = SelectObject(dc, GetStockObject(DC_PEN));
        for (int i = 1; i < kMag; ++i) {
            MoveToEx(dc, r.left + i * cell, r.top, nullptr);
            LineTo(dc, r.left + i * cell, r.bottom);
            MoveToEx(dc, r.left, r.top + i * cell, nullptr);
            LineTo(dc, r.right, r.top + i * cell);
        }
        SelectObject(dc, oldPen);
    }
    const RECT centre{r.left + half * cell, r.top + half * cell, r.left + (half + 1) * cell,
                      r.top + (half + 1) * cell};
    FrameOutside(dc, centre, RGB(0, 0, 0), 1);
    RECT inner = centre;
    InflateRect(&inner, 1, 1);
    FrameOutside(dc, inner, RGB(255, 255, 255), 1);
    FrameOutside(dc, r, kAccent, 2);

    const uint32_t px = S.mag.bits[half * kMag + half];
    RECT info{r.left - 2, r.bottom, r.right + 2, L.mag.bottom + 2};
    FillRectColor(dc, info, RGB(24, 24, 24));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(240, 240, 240));
    if (S.picking) {  // swatch + value in the copy format, then hex and position
        const COLORREF c = RGB((px >> 16) & 255, (px >> 8) & 255, px & 255);
        const int lh = (info.bottom - info.top) / 2, gap = lh / 4;
        RECT sw{info.left + 2 + gap, info.top + gap, info.left + 2 + lh - gap, info.top + lh - gap};
        FillRectColor(dc, sw, c);
        FrameOutside(dc, sw, RGB(140, 140, 140), 1);
        RECT t1{sw.right + gap, info.top, info.right - gap, info.top + lh};
        DrawTextW(dc, FormatColor(c, g_settings.colorFormat).c_str(), -1, &t1,
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        wchar_t b2[64];
        swprintf_s(b2, L"#%02X%02X%02X    %d, %d", (px >> 16) & 255, (px >> 8) & 255, px & 255, S.cur.x, S.cur.y);
        RECT t2{info.left, info.top + lh, info.right, info.bottom};
        SetTextColor(dc, RGB(170, 170, 170));
        DrawTextW(dc, b2, -1, &t2, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        return;
    }
    wchar_t buf[64];
    swprintf_s(buf, L"%d, %d   #%02X%02X%02X", S.cur.x, S.cur.y, (px >> 16) & 255, (px >> 8) & 255, px & 255);
    DrawTextW(dc, buf, -1, &info, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

void Paint(HDC hdc, const RECT& rc) {
    const int x = rc.left, y = rc.top, w = rc.right - rc.left, h = rc.bottom - rc.top;
    HDC b = S.back.dc;
    BitBlt(b, x, y, w, h, S.picking ? S.frozen.dc : S.dim.dc, x, y, SRCCOPY);
    if (!IsRectEmpty(&L.sel)) {
        RECT is;
        if (IntersectRect(&is, &L.sel, &rc))
            BitBlt(b, is.left, is.top, is.right - is.left, is.bottom - is.top, S.frozen.dc, is.left, is.top, SRCCOPY);
        FrameOutside(b, L.sel, kAccent, 2);
        FillRectColor(b, L.label, RGB(24, 24, 24));
        SetBkMode(b, TRANSPARENT);
        SetTextColor(b, RGB(255, 255, 255));
        RECT lr = L.label;
        DrawTextW(b, L.labelText.c_str(), -1, &lr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    if (g_settings.showMagnifier || S.picking) DrawMagnifier(b);
    BitBlt(hdc, x, y, w, h, b, x, y, SRCCOPY);
}

void UpdateHover() {
    for (const RECT& r : S.windows)
        if (PtInRect(&r, S.cur)) {
            S.sel = r;
            return;
        }
    S.sel = MonitorAt(S.cur);
}

void Redraw() {
    RECT d = ComputeLayout();
    RECT u;
    UnionRect(&u, &d, &S.prevDecor);
    S.prevDecor = d;
    InvalidateRect(S.hwnd, &u, FALSE);
}

void Close() {
    if (!S.active) return;
    S.active = false;
    S.picking = false;
    if (GetCapture() == S.hwnd) ReleaseCapture();
    ShowWindow(S.hwnd, SW_HIDE);
    S.frozen.Free();
    S.dim.Free();
    S.back.Free();
    S.mag.Free();
    if (S.font) DeleteObject(S.font);
    S.font = nullptr;
    S.fontPx = 0;
    S.windows.clear();
    S.monitors.clear();
    if (S.prevForeground && IsWindow(S.prevForeground)) SetForegroundWindow(S.prevForeground);
}

void Finish() {
    RECT r;
    if (!IntersectRect(&r, &S.sel, &S.vr) || r.right - r.left < 1 || r.bottom - r.top < 1) {
        Close();
        return;
    }
    auto res = std::make_unique<CaptureResult>();
    res->screenRect = r;
    res->processName = S.proc;
    res->windowTitle = S.title;
    Image& img = res->img;
    img.w = r.right - r.left;
    img.h = r.bottom - r.top;
    img.px.resize(static_cast<size_t>(img.w) * img.h);
    const int ox = r.left - S.vr.left, oy = r.top - S.vr.top;
    for (int y = 0; y < img.h; ++y)
        memcpy(&img.px[static_cast<size_t>(y) * img.w], &S.frozen.bits[static_cast<size_t>(oy + y) * S.frozen.w + ox],
               img.w * sizeof(uint32_t));
    Log(L"Region capture %dx%d at %d,%d", img.w, img.h, r.left, r.top);
    Close();
    Output_Process(std::move(res));
}

void FinishPick() {
    const int x = S.cur.x - S.vr.left, y = S.cur.y - S.vr.top;
    if (x < 0 || y < 0 || x >= S.frozen.w || y >= S.frozen.h) {
        Close();
        return;
    }
    const uint32_t px = S.frozen.bits[static_cast<size_t>(y) * S.frozen.w + x];
    const POINT at = S.cur;
    Close();
    Color_Picked(RGB((px >> 16) & 255, (px >> 8) & 255, px & 255), at);
}

void OnMove(POINT p) {
    S.cur = p;
    if (S.picking) {
        Redraw();
        return;
    }
    if (S.down && !S.dragging && (std::abs(p.x - S.anchor.x) > 3 || std::abs(p.y - S.anchor.y) > 3))
        S.dragging = true;
    if (S.dragging)
        S.sel = {std::min(S.anchor.x, p.x), std::min(S.anchor.y, p.y), std::max(S.anchor.x, p.x) + 1,
                 std::max(S.anchor.y, p.y) + 1};
    else
        UpdateHover();
    Redraw();
}

POINT ScreenPt(LPARAM lp) { return {GET_X_LPARAM(lp) + S.vr.left, GET_Y_LPARAM(lp) + S.vr.top}; }

LRESULT CALLBACK OverlayProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (S.active && S.back.dc) Paint(dc, ps.rcPaint);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_CROSS));
        return TRUE;
    case WM_MOUSEMOVE:
        if (S.active) OnMove(ScreenPt(lp));
        return 0;
    case WM_LBUTTONDOWN:
        if (!S.active) return 0;
        S.down = true;
        S.dragging = false;
        S.anchor = ScreenPt(lp);
        SetCapture(h);
        return 0;
    case WM_LBUTTONUP:
        if (!S.active || !S.down) return 0;
        S.down = false;
        ReleaseCapture();
        if (S.picking) FinishPick();
        else Finish();
        return 0;
    case WM_RBUTTONUP:
        if (S.dragging && !S.picking) {  // right-click while dragging = drop that selection, keep picking
            S.dragging = S.down = false;
            ReleaseCapture();
            UpdateHover();
            Redraw();
        } else {
            Close();
        }
        return 0;
    case WM_KEYDOWN: {
        if (!S.active) return 0;
        if (wp == VK_ESCAPE) {
            Close();
            return 0;
        }
        if (wp == VK_RETURN || wp == VK_SPACE) {
            if (S.picking) FinishPick();
            else Finish();
            return 0;
        }
        const int step = (GetKeyState(VK_SHIFT) < 0) ? 10 : 1;
        POINT p = S.cur;
        if (wp == VK_LEFT) p.x -= step;
        else if (wp == VK_RIGHT) p.x += step;
        else if (wp == VK_UP) p.y -= step;
        else if (wp == VK_DOWN) p.y += step;
        else return 0;
        SetCursorPos(p.x, p.y);  // generates WM_MOUSEMOVE
        return 0;
    }
    case WM_DPICHANGED:
        return 0;  // we work in physical pixels; never let Windows resize the overlay
    case WM_MOUSEACTIVATE:
        return MA_ACTIVATE;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// Windows only lets the app the user is working in change the foreground window. A hotkey gives us
// that right, but a launch from the command line or tray may not, and then Esc would not reach
// the overlay. Borrow the foreground thread's input state for a moment to take focus anyway.
void ForceForeground(HWND h) {
    if (SetForegroundWindow(h) && GetForegroundWindow() == h) {
        SetFocus(h);
        return;
    }
    const DWORD me = GetCurrentThreadId();
    const DWORD fgThread = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, TRUE);
    BringWindowToTop(h);
    SetForegroundWindow(h);
    SetFocus(h);
    if (fgThread && fgThread != me) AttachThreadInput(me, fgThread, FALSE);
    if (GetForegroundWindow() != h) Log(L"Overlay could not take focus");
}

void StartRegion(HWND fg, bool picking) {
    S.vr = VirtualScreenRect();
    S.prevForeground = fg;
    S.picking = picking;
    WindowInfo(fg, S.proc, S.title);
    S.windows.clear();
    S.monitors.clear();
    if (!picking) EnumWindows(EnumWindowsProc, 0);
    EnumDisplayMonitors(nullptr, nullptr, EnumMonitorsProc, 0);

    if (!GrabInto(S.frozen, S.vr, g_settings.includeCursor && !picking)) {
        Log(L"Screen grab failed (%d x %d)", S.vr.right - S.vr.left, S.vr.bottom - S.vr.top);
        S.frozen.Free();
        return;
    }
    const int w = S.frozen.w, h = S.frozen.h;
    if ((!picking && !S.dim.Create(w, h)) || !S.back.Create(w, h) || !S.mag.Create(kMag, kMag)) {
        Log(L"Out of memory for overlay %d x %d", w, h);
        S.frozen.Free();
        S.dim.Free();
        S.back.Free();
        return;
    }
    if (!picking) {
        const size_t n = static_cast<size_t>(w) * h;
        const uint32_t* src = S.frozen.bits;
        uint32_t* dst = S.dim.bits;
        for (size_t i = 0; i < n; ++i) dst[i] = (src[i] >> 1) & 0x7F7F7F;  // 50 % darker
    }

    GetCursorPos(&S.cur);
    S.down = S.dragging = false;
    S.sel = {};
    if (!picking) UpdateHover();
    S.active = true;
    ComputeLayout();
    S.prevDecor = {0, 0, w, h};
    SetWindowPos(S.hwnd, HWND_TOPMOST, S.vr.left, S.vr.top, w, h, SWP_SHOWWINDOW);
    RedrawWindow(S.hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW);
    ForceForeground(S.hwnd);
    RECT wr;
    GetWindowRect(S.hwnd, &wr);
    if (!EqualRect(&wr, &S.vr))
        Log(L"Overlay rect mismatch: got %d,%d,%d,%d want %d,%d,%d,%d", wr.left, wr.top, wr.right, wr.bottom,
            S.vr.left, S.vr.top, S.vr.right, S.vr.bottom);
}

void GrabDirect(RECT r, HWND fg) {
    const RECT vr = VirtualScreenRect();
    if (!IntersectRect(&r, &r, &vr)) return;
    auto res = std::make_unique<CaptureResult>();
    if (!GrabScreen(r, res->img, g_settings.includeCursor)) {
        Log(L"Screen grab failed");
        return;
    }
    res->screenRect = r;
    WindowInfo(fg, res->processName, res->windowTitle);
    Log(L"Capture %dx%d at %d,%d", res->img.w, res->img.h, r.left, r.top);
    Output_Process(std::move(res));
}

}  // namespace

RECT VirtualScreenRect() {
    const int x = GetSystemMetrics(SM_XVIRTUALSCREEN), y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    return {x, y, x + GetSystemMetrics(SM_CXVIRTUALSCREEN), y + GetSystemMetrics(SM_CYVIRTUALSCREEN)};
}

bool GrabScreen(const RECT& r, Image& out, bool withCursor) {
    Dib d;
    if (!GrabInto(d, r, withCursor)) return false;
    out.w = d.w;
    out.h = d.h;
    out.px.assign(d.bits, d.bits + static_cast<size_t>(d.w) * d.h);
    return true;
}

void Capture_Init() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = OverlayProc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = kOverlayClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_CROSS);
    RegisterClassExW(&wc);
    S.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kOverlayClass, L"Canivete capture", WS_POPUP, 0, 0,
                             1, 1, nullptr, nullptr, g_hinst, nullptr);
    BOOL off = TRUE;  // no fade-in animation: the overlay must appear instantly
    DwmSetWindowAttribute(S.hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off));
}

bool Capture_IsActive() { return S.active; }

namespace {

// Top-level window under the mouse; nullptr over the desktop, taskbar or Canivete's own windows.
HWND WindowUnderCursor() {
    POINT p;
    GetCursorPos(&p);
    HWND h = GetAncestor(WindowFromPoint(p), GA_ROOT);
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (!h || IsShellBackground(h) || pid == GetCurrentProcessId()) return nullptr;
    return h;
}

}  // namespace

void Capture_Start(CaptureMode mode) {
    if (S.active) return;
    HWND fg = GetForegroundWindow();
    switch (mode) {
    case CaptureMode::Region:
        StartRegion(fg, false);
        break;
    case CaptureMode::Color:
        StartRegion(fg, true);
        break;
    case CaptureMode::Window:
        if (HWND w = WindowUnderCursor()) GrabDirect(WindowBounds(w), w);
        else GrabDirect(MonitorUnderCursor(), nullptr);
        break;
    case CaptureMode::Full:
        GrabDirect(g_settings.fullScreen == L"monitor" ? MonitorUnderCursor() : VirtualScreenRect(), fg);
        break;
    }
}
