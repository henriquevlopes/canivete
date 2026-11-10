// The save window's timeline for GIF and video recordings: a preview of the frame at the handle
// being moved, a strip of small pictures, and a start and an end handle to cut off the beginning
// and the end. Drag a handle with the mouse, or use the keys: Left / Right one frame (Shift: one
// second), Home / End, Space switches between the start and the end handle. The parent gets
// WM_COMMAND (id, kTrimChanged) once the user lets go (keys: a moment after the last press).
#include "app.h"
#include "theme.h"

#include <windowsx.h>

#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>

#include <cmath>
#include <cstdio>

namespace {

constexpr wchar_t kClass[] = L"CaniveteTrim";
constexpr UINT_PTR kTimerNotify = 1;
constexpr UINT kNotifyDelay = 600;  // keys: tell the parent this long after the last press

struct Trim {
    HWND hwnd = nullptr;
    const std::vector<TrimThumb>* thumbs = nullptr;
    uint32_t duration = 1, from = 0, to = 1;
    uint32_t sentFrom = 0, sentTo = 1;  // what the parent last heard
    int fps = 0;
    UINT dpi = 96;
    int active = 0;     // handle the keys move: 0 = start, 1 = end
    bool dragging = false;
    HFONT font = nullptr;
};


int Px(const Trim& t, int v) { return MulDiv(v, static_cast<int>(t.dpi), 96); }

uint32_t StepMs(const Trim& t) { return t.fps > 0 ? static_cast<uint32_t>(std::lround(1000.0 / t.fps)) : 50; }
uint32_t MinLen(const Trim& t) { return std::max<uint32_t>(100, StepMs(t)); }

std::wstring Clock(uint32_t ms) {  // 0:04.8
    wchar_t s[32];
    swprintf_s(s, L"%u:%02u.%u", ms / 60000, ms / 1000 % 60, ms / 100 % 10);
    return s;
}

// Layout in client pixels.
struct Layout {
    RECT preview, strip, label;
    int x0, x1;  // the strip's time axis
};
Layout GetLayout(const Trim& t, const RECT& c) {
    Layout l;
    const int stripH = Px(t, 34), labelH = Px(t, 22), pad = Px(t, 8);
    l.label = {c.left, c.bottom - labelH, c.right, c.bottom};
    l.strip = {c.left + pad, l.label.top - Px(t, 6) - stripH, c.right - pad, l.label.top - Px(t, 6)};
    l.preview = {c.left, c.top, c.right, l.strip.top - Px(t, 10)};
    l.x0 = l.strip.left;
    l.x1 = l.strip.right;
    return l;
}

int XOf(const Layout& l, const Trim& t, uint32_t ms) {
    return l.x0 + static_cast<int>(std::lround(static_cast<double>(ms) * (l.x1 - l.x0) / t.duration));
}
uint32_t MsOf(const Layout& l, const Trim& t, int x) {
    const double f = std::clamp(static_cast<double>(x - l.x0) / std::max(1, l.x1 - l.x0), 0.0, 1.0);
    return static_cast<uint32_t>(std::lround(f * t.duration));
}

const TrimThumb* ThumbAt(const Trim& t, uint32_t ms) {
    if (!t.thumbs || t.thumbs->empty()) return nullptr;
    const auto& v = *t.thumbs;
    // the picture taken nearest to ms
    auto it = std::lower_bound(v.begin(), v.end(), ms, [](const TrimThumb& th, uint32_t m) { return th.ms < m; });
    if (it == v.end()) return &v.back();
    if (it != v.begin() && ms - (it - 1)->ms < it->ms - ms) --it;
    return &*it;
}

void DrawThumb(Gdiplus::Graphics& g, const TrimThumb& th, const RECT& box, bool fit) {
    if (th.img.w <= 0 || th.img.h <= 0) return;
    Gdiplus::Bitmap bmp(th.img.w, th.img.h, th.img.w * 4, PixelFormat32bppRGB,
                        reinterpret_cast<BYTE*>(const_cast<uint32_t*>(th.img.px.data())));
    const double bw = box.right - box.left, bh = box.bottom - box.top;
    double s = fit ? std::min(bw / th.img.w, bh / th.img.h) : std::max(bw / th.img.w, bh / th.img.h);
    const double w = th.img.w * s, h = th.img.h * s;
    const Gdiplus::RectF dst(static_cast<float>(box.left + (bw - w) / 2), static_cast<float>(box.top + (bh - h) / 2),
                             static_cast<float>(w), static_cast<float>(h));
    g.DrawImage(&bmp, dst, 0, 0, static_cast<float>(th.img.w), static_cast<float>(th.img.h), Gdiplus::UnitPixel);
}

Gdiplus::Color C(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

void RoundRect(Gdiplus::GraphicsPath& p, float x, float y, float w, float h, float r) {
    const float d = std::min({2 * r, w, h});
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
}

void Paint(Trim& t, HDC dc) {
    RECT c;
    GetClientRect(t.hwnd, &c);
    const Palette& pal = Pal();
    const Layout l = GetLayout(t, c);
    {
        Gdiplus::Graphics g(dc);
        g.Clear(C(pal.window));
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBilinear);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);

        // Preview: the frame at the handle being moved.
        Gdiplus::SolidBrush canvas(C(pal.canvas));
        g.FillRectangle(&canvas, static_cast<INT>(l.preview.left), static_cast<INT>(l.preview.top),
                        static_cast<INT>(l.preview.right - l.preview.left),
                        static_cast<INT>(l.preview.bottom - l.preview.top));
        if (const TrimThumb* th = ThumbAt(t, t.active ? t.to : t.from)) {
            RECT in = l.preview;
            InflateRect(&in, -Px(t, 4), -Px(t, 4));
            DrawThumb(g, *th, in, true);
        }

        // The strip: small pictures along the time line.
        const int sw = l.strip.right - l.strip.left, sh = l.strip.bottom - l.strip.top;
        Gdiplus::GraphicsPath stripPath;
        RoundRect(stripPath, static_cast<float>(l.strip.left), static_cast<float>(l.strip.top), static_cast<float>(sw),
                  static_cast<float>(sh), static_cast<float>(Px(t, 4)));
        g.FillPath(&canvas, &stripPath);
        if (t.thumbs && !t.thumbs->empty()) {
            const TrimThumb& first = t.thumbs->front();
            const double aspect = first.img.h > 0 ? static_cast<double>(first.img.w) / first.img.h : 1.6;
            const int cellW = std::max(Px(t, 16), static_cast<int>(sh * aspect));
            const int cells = std::max(1, (sw + cellW - 1) / cellW);
            Gdiplus::Region old;
            g.GetClip(&old);
            g.SetClip(&stripPath);
            for (int i = 0; i < cells; ++i) {
                const RECT cell = {l.strip.left + i * cellW, l.strip.top, l.strip.left + (i + 1) * cellW, l.strip.bottom};
                const uint32_t ms = MsOf(l, t, (cell.left + std::min(cell.right, l.strip.right)) / 2);
                if (const TrimThumb* th = ThumbAt(t, ms)) DrawThumb(g, *th, cell, false);
            }
            // Cut-off parts: dimmed.
            Gdiplus::SolidBrush dim(Gdiplus::Color(pal.dark ? 170 : 150, 0, 0, 0));
            const int a = XOf(l, t, t.from), b = XOf(l, t, t.to);
            if (a > l.strip.left) g.FillRectangle(&dim, l.strip.left, l.strip.top, a - l.strip.left, sh);
            if (b < l.strip.right) g.FillRectangle(&dim, b, l.strip.top, l.strip.right - b, sh);
            g.SetClip(&old);
        }
        // The kept part: an accent frame, with a handle at each end.
        const float a = static_cast<float>(XOf(l, t, t.from)), b = static_cast<float>(XOf(l, t, t.to));
        const float hw = static_cast<float>(Px(t, 10)), top = static_cast<float>(l.strip.top - Px(t, 3)),
                    hh = static_cast<float>(sh + Px(t, 6));
        Gdiplus::Pen frame(C(pal.accent), static_cast<float>(Px(t, 2)));
        g.DrawRectangle(&frame, a, static_cast<float>(l.strip.top) + 1, b - a, static_cast<float>(sh) - 2);
        const bool focused = GetFocus() == t.hwnd;
        for (int i = 0; i < 2; ++i) {
            const float x = i == 0 ? a - hw : b;
            Gdiplus::GraphicsPath hp;
            RoundRect(hp, x, top, hw, hh, static_cast<float>(Px(t, 3)));
            Gdiplus::SolidBrush fill(C(pal.accent));
            g.FillPath(&fill, &hp);
            if (focused && i == t.active) {  // the handle the keys move
                Gdiplus::Pen ring(C(pal.text), static_cast<float>(std::max(1, Px(t, 2))));
                g.DrawPath(&ring, &hp);
            }
            Gdiplus::Pen grip(C(pal.onAccent), static_cast<float>(std::max(1, Px(t, 1))));
            const float gx = x + hw / 2, gy = top + hh / 2;
            for (float dx : {-2.0f, 2.0f})
                g.DrawLine(&grip, gx + dx * t.dpi / 96, gy - hh / 5, gx + dx * t.dpi / 96, gy + hh / 5);
        }
    }

    // The times, in the window's font.
    HGDIOBJ oldFont = SelectObject(dc, t.font ? t.font : GetStockObject(DEFAULT_GUI_FONT));
    SetBkMode(dc, TRANSPARENT);
    wchar_t s[160];
    const uint32_t len = t.to - t.from;
    if (t.from == 0 && t.to == t.duration)
        swprintf_s(s, L"%s long. Drag the handles to cut off the start or the end.", Clock(t.duration).c_str());
    else
        swprintf_s(s, L"%s – %s  ·  %.1f s of %.1f s", Clock(t.from).c_str(), Clock(t.to).c_str(), len / 1000.0,
                   t.duration / 1000.0);
    SetTextColor(dc, pal.grey);
    RECT lr = l.label;
    lr.left += Px(t, 8);
    lr.right -= Px(t, 8);
    DrawTextW(dc, s, -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, oldFont);
}

void Notify(Trim& t) {
    KillTimer(t.hwnd, kTimerNotify);
    if (t.from == t.sentFrom && t.to == t.sentTo) return;
    t.sentFrom = t.from;
    t.sentTo = t.to;
    HWND parent = GetParent(t.hwnd);
    SendMessageW(parent, WM_COMMAND, MAKEWPARAM(GetDlgCtrlID(t.hwnd), kTrimChanged), reinterpret_cast<LPARAM>(t.hwnd));
}

// Moves the active handle to ms (snapped to a frame), keeping the two at least MinLen apart.
void MoveHandle(Trim& t, int64_t ms) {
    ms = std::clamp<int64_t>(ms, 0, t.duration);
    uint32_t v = Trim_SnapMs(static_cast<uint32_t>(ms), t.fps);
    if (ms == t.duration) v = t.duration;
    const uint32_t gap = std::min(MinLen(t), t.duration);
    if (t.active == 0) t.from = std::min(v, t.to >= gap ? t.to - gap : 0);
    else t.to = std::clamp(v, std::min(t.duration, t.from + gap), t.duration);
    InvalidateRect(t.hwnd, nullptr, FALSE);
}

LRESULT CALLBACK Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    auto* t = reinterpret_cast<Trim*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (m) {
    case WM_NCCREATE: {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
        t = static_cast<Trim*>(cs->lpCreateParams);
        t->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(t));
        break;
    }
    case WM_NCDESTROY:
        delete t;
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        break;
    case WM_SETFONT:
        if (t) t->font = reinterpret_cast<HFONT>(wp);
        return 0;
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        RECT c;
        GetClientRect(h, &c);
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, std::max(1L, c.right), std::max(1L, c.bottom));
        HGDIOBJ old = SelectObject(mem, bmp);
        if (t) Paint(*t, mem);
        BitBlt(dc, 0, 0, c.right, c.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
        InvalidateRect(h, nullptr, FALSE);
        if (m == WM_KILLFOCUS && t) Notify(*t);
        return 0;
    case WM_LBUTTONDOWN: {
        if (!t) break;
        SetFocus(h);
        RECT c;
        GetClientRect(h, &c);
        const Layout l = GetLayout(*t, c);
        const int x = GET_X_LPARAM(lp);
        const int a = XOf(l, *t, t->from), b = XOf(l, *t, t->to), hw = Px(*t, 10);
        // The nearer handle (a handle's grip sits just outside the kept part).
        const int da = std::abs(x - (a - hw / 2)), db = std::abs(x - (b + hw / 2));
        t->active = da <= db ? 0 : 1;
        t->dragging = true;
        SetCapture(h);
        const bool onHandle = (t->active == 0 && x >= a - hw && x <= a) || (t->active == 1 && x >= b && x <= b + hw);
        if (!onHandle) MoveHandle(*t, MsOf(l, *t, x));
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_MOUSEMOVE:
        if (t && t->dragging) {
            RECT c;
            GetClientRect(h, &c);
            const Layout l = GetLayout(*t, c);
            const int hw = Px(*t, 10);
            const int x = GET_X_LPARAM(lp) + (t->active == 0 ? hw / 2 : -hw / 2);
            MoveHandle(*t, MsOf(l, *t, x));
            UpdateWindow(h);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_CAPTURECHANGED:
        if (t && t->dragging) {
            t->dragging = false;
            if (m == WM_LBUTTONUP) ReleaseCapture();
            Notify(*t);
        }
        return 0;
    case WM_SETCURSOR:
        if (t && LOWORD(lp) == HTCLIENT) {
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(h, &pt);
            RECT c;
            GetClientRect(h, &c);
            const Layout l = GetLayout(*t, c);
            const bool onStrip = pt.y >= l.strip.top - Px(*t, 4) && pt.y <= l.strip.bottom + Px(*t, 4);
            SetCursor(LoadCursorW(nullptr, onStrip ? IDC_SIZEWE : IDC_ARROW));
            return TRUE;
        }
        break;
    case WM_KEYDOWN: {
        if (!t) break;
        const bool shift = GetKeyState(VK_SHIFT) < 0;
        const int64_t step = shift ? 1000 : StepMs(*t);
        const int64_t cur = t->active ? t->to : t->from;
        switch (wp) {
        case VK_LEFT: case VK_DOWN: MoveHandle(*t, cur - step); break;
        case VK_RIGHT: case VK_UP: MoveHandle(*t, cur + step); break;
        case VK_HOME: MoveHandle(*t, 0); break;
        case VK_END: MoveHandle(*t, t->duration); break;
        case VK_SPACE:
            t->active ^= 1;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        default: return DefWindowProcW(h, m, wp, lp);
        }
        SetTimer(h, kTimerNotify, kNotifyDelay, nullptr);
        return 0;
    }
    case WM_CHAR:
        return 0;
    case WM_TIMER:
        if (t && wp == kTimerNotify) Notify(*t);
        return 0;
    case WM_ENABLE:
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

}  // namespace

uint32_t Trim_SnapMs(uint32_t ms, int fps) {
    if (fps <= 0) return ms / 10 * 10;
    const long long k = std::llround(ms * static_cast<double>(fps) / 1000.0);
    return static_cast<uint32_t>(std::llround(k * 1000.0 / fps));
}

HWND Trim_Create(HWND parent, int id, int x, int y, int w, int h, const std::vector<TrimThumb>* thumbs,
                 uint32_t durationMs, int fps, UINT dpi) {
    Gdiplus_Ensure();
    static bool registered = false;
    if (!registered) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = Proc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        registered = RegisterClassW(&wc) != 0;
    }
    auto* t = new Trim;
    t->thumbs = thumbs;
    t->duration = std::max<uint32_t>(1, durationMs);
    t->from = t->sentFrom = 0;
    t->to = t->sentTo = t->duration;
    t->fps = fps;
    t->dpi = dpi;
    t->active = 0;
    HWND wnd = CreateWindowExW(0, kClass, L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, x, y, w, h, parent,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hinst, t);
    return wnd;
}

void Trim_Get(HWND trim, uint32_t& fromMs, uint32_t& toMs) {
    const auto* t = reinterpret_cast<const Trim*>(GetWindowLongPtrW(trim, GWLP_USERDATA));
    fromMs = t ? t->from : 0;
    toMs = t ? t->to : 0;
}

void Trim_Set(HWND trim, uint32_t fromMs, uint32_t toMs) {
    auto* t = reinterpret_cast<Trim*>(GetWindowLongPtrW(trim, GWLP_USERDATA));
    if (!t) return;
    t->to = std::min(toMs, t->duration);
    t->from = std::min(fromMs, t->to > MinLen(*t) ? t->to - MinLen(*t) : 0);
    t->sentFrom = t->from;
    t->sentTo = t->to;
    InvalidateRect(trim, nullptr, FALSE);
}
