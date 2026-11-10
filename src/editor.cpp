// Screenshot editor: boxes, ellipses, arrows, lines, text, numbered markers, highlighter and pixelate
// on top of a capture, then copy / save. Shapes stay editable (undo / redo) until the window closes.
// Shape coordinates are image pixels; the window shows the image scaled down to fit.
#include "app.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out

#include <algorithm>
#include <cmath>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

namespace G = Gdiplus;

namespace {

constexpr wchar_t kEditorClass[] = L"CaniveteEditor";
constexpr COLORREF kAccent = RGB(232, 44, 56);
constexpr UINT_PTR kTimerStatus = 1;

enum class Tool { Rect, Ellipse, Arrow, Line, Text, Number, Highlight, Pixelate };
struct ToolDef {
    Tool tool;
    UINT key;
    const wchar_t* tip;
};
const ToolDef kTools[] = {
    {Tool::Rect, 'R', L"Rectangle  (R)"},      {Tool::Ellipse, 'E', L"Ellipse  (E)"},
    {Tool::Arrow, 'A', L"Arrow  (A)"},         {Tool::Line, 'L', L"Line  (L)"},
    {Tool::Text, 'T', L"Text  (T)"},           {Tool::Number, 'N', L"Numbered marker  (N)"},
    {Tool::Highlight, 'H', L"Highlighter  (H)"}, {Tool::Pixelate, 'P', L"Pixelate, hides details  (P)"},
};
const COLORREF kColors[] = {RGB(232, 44, 56),  RGB(255, 140, 0),  RGB(255, 214, 0),   RGB(46, 204, 64),
                            RGB(0, 120, 215),  RGB(156, 39, 176), RGB(255, 255, 255), RGB(0, 0, 0)};
constexpr int kColorCount = static_cast<int>(std::size(kColors));
const float kLineW[] = {2, 4, 7};    // at 100 % display scale, in image pixels
const float kFontPx[] = {16, 24, 36};

struct Shape {
    Tool tool = Tool::Rect;
    COLORREF color = 0;
    int size = 1;
    G::PointF a, b;
    std::wstring text;
    int number = 0;
};

enum class Item { Tool, Color, Size, Undo, Redo, Copy, Save, Done };
struct ItemRect {
    Item kind;
    int index;
    RECT r;
};

struct Editor {
    HWND hwnd = nullptr;
    std::shared_ptr<CaptureResult> src;
    std::wstring path;  // file it was saved to ("" = not saved yet)
    float sc = 1;       // display scale of the captured monitor: sizes follow it
    Dib base, committed, view, back;
    std::vector<Shape> shapes, redo;
    Tool tool = Tool::Rect;
    int color = 0, size = 1;
    bool drawing = false, typing = false;
    Shape live;
    float zoom = 1;
    bool fit = true;  // zoom follows the window size until the user zooms by hand
    POINT origin{};   // image top-left, client coords
    bool space = false, panning = false;  // Space held = hand tool; dragging the view
    POINT panFrom{}, panOrigin{};
    RECT imageArea{};
    UINT dpi = 96;
    std::vector<ItemRect> items;
    int hover = -1;
    bool dirty = false;
    std::wstring status;  // short-lived message in the status bar
    HFONT ui = nullptr, icons = nullptr, bold = nullptr;

    int Px(int v) const { return MulDiv(v, static_cast<int>(dpi), 96); }
    int ImgW() const { return src->img.w; }
    int ImgH() const { return src->img.h; }
};

ULONG_PTR g_gdipToken = 0;
int g_open = 0;  // editor windows; a standalone --edit run quits when the last one closes

Editor* Get(HWND h) { return reinterpret_cast<Editor*>(GetWindowLongPtrW(h, GWLP_USERDATA)); }

G::Color GC(COLORREF c, BYTE a = 255) { return G::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

bool IsLight(COLORREF c) { return 0.2126 * GetRValue(c) + 0.7152 * GetGValue(c) + 0.0722 * GetBValue(c) > 150; }

G::RectF Norm(G::PointF a, G::PointF b) {
    return G::RectF(std::min(a.X, b.X), std::min(a.Y, b.Y), std::fabs(b.X - a.X), std::fabs(b.Y - a.Y));
}

// ---- drawing shapes ------------------------------------------------------------------------

void ArrowPath(G::Graphics& g, G::PointF a, G::PointF b, float w, const G::Color& c) {
    const float dx = b.X - a.X, dy = b.Y - a.Y, len = std::hypot(dx, dy);
    if (len < 1) return;
    const float ux = dx / len, uy = dy / len;
    const float head = std::min(len, 3.5f * w + 8), half = head * 0.45f;
    const G::PointF base(b.X - ux * head, b.Y - uy * head);
    G::Pen pen(c, w);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapFlat);
    g.DrawLine(&pen, a, G::PointF(b.X - ux * head * 0.8f, b.Y - uy * head * 0.8f));
    const G::PointF tri[3] = {b, G::PointF(base.X - uy * half, base.Y + ux * half),
                              G::PointF(base.X + uy * half, base.Y - ux * half)};
    G::SolidBrush br(c);
    g.FillPolygon(&br, tri, 3);
}

G::StringFormat* TypoFormat() {
    static G::StringFormat* f = [] {
        auto* s = G::StringFormat::GenericTypographic()->Clone();
        s->SetFormatFlags(s->GetFormatFlags() | G::StringFormatFlagsMeasureTrailingSpaces);
        return s;
    }();
    return f;
}

G::FontFamily& TextFamily() {
    static G::FontFamily f(L"Segoe UI");
    return f;
}

float LineSpacing(float em) {
    G::FontFamily& f = TextFamily();
    return em * f.GetLineSpacing(G::FontStyleBold) / f.GetEmHeight(G::FontStyleBold);
}

void DrawText(G::Graphics& g, const Shape& s, float sc) {
    if (s.text.empty()) return;
    const float em = kFontPx[s.size] * sc;
    G::GraphicsPath path;
    path.AddString(s.text.c_str(), -1, &TextFamily(), G::FontStyleBold, em, s.a, TypoFormat());
    G::Pen outline(IsLight(s.color) ? G::Color(220, 0, 0, 0) : G::Color(235, 255, 255, 255), em * 0.16f);
    outline.SetLineJoin(G::LineJoinRound);
    g.DrawPath(&outline, &path);
    G::SolidBrush br(GC(s.color));
    g.FillPath(&br, &path);
}

void DrawNumber(G::Graphics& g, const Shape& s, float sc) {
    const float d = kFontPx[s.size] * sc * 1.45f;
    const G::RectF r(s.a.X - d / 2, s.a.Y - d / 2, d, d);
    G::SolidBrush fill(GC(s.color));
    g.FillEllipse(&fill, r);
    G::Pen ring(IsLight(s.color) ? G::Color(200, 0, 0, 0) : G::Color(230, 255, 255, 255), std::max(1.5f, d * 0.06f));
    g.DrawEllipse(&ring, r);
    const std::wstring t = std::to_wstring(s.number);
    G::Font font(&TextFamily(), d * (t.size() > 1 ? 0.48f : 0.58f), G::FontStyleBold, G::UnitPixel);
    G::StringFormat f;
    f.SetAlignment(G::StringAlignmentCenter);
    f.SetLineAlignment(G::StringAlignmentCenter);
    G::SolidBrush txt(IsLight(s.color) ? G::Color(255, 0, 0, 0) : G::Color(255, 255, 255, 255));
    g.DrawString(t.c_str(), -1, &font, G::RectF(r.X, r.Y + d * 0.03f, d, d), &f, &txt);
}

// preview = still being drawn: highlighter and pixelate show a stand-in (they work on pixels).
void DrawShape(G::Graphics& g, const Shape& s, float sc, bool preview) {
    const float w = kLineW[s.size] * sc;
    G::Pen pen(GC(s.color), w);
    pen.SetLineJoin(G::LineJoinRound);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    const G::RectF r = Norm(s.a, s.b);
    switch (s.tool) {
    case Tool::Rect:
        g.DrawRectangle(&pen, r);
        break;
    case Tool::Ellipse:
        g.DrawEllipse(&pen, r);
        break;
    case Tool::Line:
        g.DrawLine(&pen, s.a, s.b);
        break;
    case Tool::Arrow:
        ArrowPath(g, s.a, s.b, w, GC(s.color));
        break;
    case Tool::Text:
        DrawText(g, s, sc);
        break;
    case Tool::Number:
        DrawNumber(g, s, sc);
        break;
    case Tool::Highlight:
        if (preview) {
            G::SolidBrush br(GC(s.color, 110));
            g.FillRectangle(&br, r);
        }
        break;
    case Tool::Pixelate:
        if (preview) {
            G::Pen dash(G::Color(255, 255, 255, 255), 2 * sc);
            dash.SetDashStyle(G::DashStyleDash);
            G::SolidBrush br(G::Color(60, 128, 128, 128));
            g.FillRectangle(&br, r);
            g.DrawRectangle(&dash, r);
        }
        break;
    }
}

// Highlighter = multiply with the color, so dark text under it stays readable.
void ApplyHighlight(Dib& d, RECT r, COLORREF c) {
    RECT all{0, 0, d.w, d.h};
    if (!IntersectRect(&r, &r, &all)) return;
    const uint32_t cr = GetRValue(c), cg = GetGValue(c), cb = GetBValue(c);
    for (int y = r.top; y < r.bottom; ++y) {
        uint32_t* p = d.bits + static_cast<size_t>(y) * d.w;
        for (int x = r.left; x < r.right; ++x) {
            // Multiply (keeps dark text readable on light backgrounds), plus some of the plain color on
            // dark pixels so it still shows on dark backgrounds.
            const uint32_t v = p[x], r0 = (v >> 16) & 255, g0 = (v >> 8) & 255, b0 = v & 255;
            const uint32_t dark = 255 - (r0 * 54 + g0 * 183 + b0 * 19) / 256;  // 0 white .. 255 black
            const uint32_t w = dark * 115 / 255;                                // up to 45 % color
            auto mix = [&](uint32_t s, uint32_t col) { return ((s * col / 255) * (255 - w) + col * w) / 255; };
            p[x] = (mix(r0, cr) << 16) | (mix(g0, cg) << 8) | mix(b0, cb);
        }
    }
}

void ApplyPixelate(Dib& d, RECT r, int block) {
    RECT all{0, 0, d.w, d.h};
    if (!IntersectRect(&r, &r, &all)) return;
    for (int by = r.top; by < r.bottom; by += block)
        for (int bx = r.left; bx < r.right; bx += block) {
            const int ex = std::min<int>(bx + block, r.right), ey = std::min<int>(by + block, r.bottom);
            uint64_t sr = 0, sg = 0, sb = 0, n = 0;
            for (int y = by; y < ey; ++y)
                for (int x = bx; x < ex; ++x) {
                    const uint32_t v = d.bits[static_cast<size_t>(y) * d.w + x];
                    sr += (v >> 16) & 255;
                    sg += (v >> 8) & 255;
                    sb += v & 255;
                    ++n;
                }
            const uint32_t avg = static_cast<uint32_t>(((sr / n) << 16) | ((sg / n) << 8) | (sb / n));
            for (int y = by; y < ey; ++y)
                for (int x = bx; x < ex; ++x) d.bits[static_cast<size_t>(y) * d.w + x] = avg;
        }
}

RECT ToRect(const G::RectF& r) {
    return {static_cast<LONG>(std::floor(r.X)), static_cast<LONG>(std::floor(r.Y)),
            static_cast<LONG>(std::ceil(r.X + r.Width)), static_cast<LONG>(std::ceil(r.Y + r.Height))};
}

// Draws one finished shape into the full-size image.
void Bake(Editor& e, Dib& d, const Shape& s) {
    if (s.tool == Tool::Highlight || s.tool == Tool::Pixelate) {
        GdiFlush();
        const RECT r = ToRect(Norm(s.a, s.b));
        if (s.tool == Tool::Highlight) ApplyHighlight(d, r, s.color);
        else ApplyPixelate(d, r, std::max(6, static_cast<int>(std::lround(12 * e.sc))));
        return;
    }
    G::Graphics g(d.dc);
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
    g.SetTextRenderingHint(G::TextRenderingHintAntiAlias);
    DrawShape(g, s, e.sc, false);
}

// ---- image buffers -------------------------------------------------------------------------

void UpdateView(Editor& e) {
    const int vw = std::max(1, static_cast<int>(std::lround(e.ImgW() * e.zoom)));
    const int vh = std::max(1, static_cast<int>(std::lround(e.ImgH() * e.zoom)));
    if (e.zoom >= 0.999f) {
        e.view.Free();
        return;
    }
    if (e.view.w != vw || e.view.h != vh) e.view.Create(vw, vh);
    SetStretchBltMode(e.view.dc, HALFTONE);
    SetBrushOrgEx(e.view.dc, 0, 0, nullptr);
    StretchBlt(e.view.dc, 0, 0, vw, vh, e.committed.dc, 0, 0, e.ImgW(), e.ImgH(), SRCCOPY);
}

void Rebuild(Editor& e) {
    GdiFlush();
    memcpy(e.committed.bits, e.base.bits, static_cast<size_t>(e.base.w) * e.base.h * 4);
    for (const Shape& s : e.shapes) Bake(e, e.committed, s);
    GdiFlush();
    UpdateView(e);
}

void Commit(Editor& e, const Shape& s) {
    e.shapes.push_back(s);
    e.redo.clear();
    Bake(e, e.committed, s);
    GdiFlush();
    UpdateView(e);
    e.dirty = true;
}

int NextNumber(const Editor& e) {
    int n = 1;
    for (const Shape& s : e.shapes)
        if (s.tool == Tool::Number) n = std::max(n, s.number + 1);
    return n;
}

Image CurrentImage(const Editor& e) {
    GdiFlush();
    Image img;
    img.w = e.ImgW();
    img.h = e.ImgH();
    img.px.assign(e.committed.bits, e.committed.bits + static_cast<size_t>(img.w) * img.h);
    return img;
}

// ---- layout --------------------------------------------------------------------------------

constexpr int kToolbarH = 44, kStatusH = 24;

int ToolbarWidth(const Editor& e) {
    const int btn = e.Px(34), sw = e.Px(26), sep = e.Px(14);
    return e.Px(8) + 8 * btn + sep + kColorCount * sw + sep + 3 * e.Px(28) + sep + 2 * btn + sep + 2 * btn +
           e.Px(6) + e.Px(78) + e.Px(8);
}

// ---- zoom and pan ---------------------------------------------------------------------------

constexpr float kMinZoom = 0.02f, kMaxZoom = 16.f;

float FitZoom(const Editor& e) {
    const int aw = e.imageArea.right - e.imageArea.left - 2 * e.Px(10);
    const int ah = e.imageArea.bottom - e.imageArea.top - 2 * e.Px(10);
    const float z = std::min({1.f, static_cast<float>(aw) / e.ImgW(), static_cast<float>(ah) / e.ImgH()});
    return std::max(kMinZoom, z);
}

// An image smaller than the view is centred; a bigger one can't be dragged past its edges.
void ClampOrigin(Editor& e) {
    const int m = e.Px(10);
    auto axis = [m](LONG& o, LONG lo, LONG hi, int v) {
        if (v <= hi - lo - 2 * m) o = lo + (hi - lo - v) / 2;
        else o = std::clamp<LONG>(o, hi - m - v, lo + m);
    };
    axis(e.origin.x, e.imageArea.left, e.imageArea.right, static_cast<int>(std::lround(e.ImgW() * e.zoom)));
    axis(e.origin.y, e.imageArea.top, e.imageArea.bottom, static_cast<int>(std::lround(e.ImgH() * e.zoom)));
}

// Zoom keeping the image point under `anchor` (client coords) where it is.
void SetZoom(Editor& e, float zoom, POINT anchor) {
    zoom = std::clamp(zoom, kMinZoom, kMaxZoom);
    const float ix = (anchor.x - e.origin.x) / e.zoom, iy = (anchor.y - e.origin.y) / e.zoom;
    const bool changed = std::fabs(zoom - e.zoom) > 1e-4f || (zoom < 0.999f && !e.view.dc);
    e.fit = false;
    e.zoom = zoom;
    e.origin = {anchor.x - std::lround(ix * zoom), anchor.y - std::lround(iy * zoom)};
    ClampOrigin(e);
    if (changed) UpdateView(e);
}

POINT AreaCenter(const Editor& e) {
    return {(e.imageArea.left + e.imageArea.right) / 2, (e.imageArea.top + e.imageArea.bottom) / 2};
}

// Steps of 25 %, stopping once at 100 % on the way through.
void ZoomStep(Editor& e, int steps, POINT anchor) {
    float z = e.zoom * std::pow(1.25f, static_cast<float>(steps));
    if ((e.zoom < 0.999f && z > 1.001f) || (e.zoom > 1.001f && z < 0.999f)) z = 1;
    SetZoom(e, z, anchor);
}

void Layout(Editor& e) {
    RECT rc;
    GetClientRect(e.hwnd, &rc);
    e.items.clear();
    const int btn = e.Px(34), sw = e.Px(26), sep = e.Px(14), top = e.Px(5), h = e.Px(34);
    int x = e.Px(8);
    auto add = [&](Item k, int i, int w) {
        e.items.push_back({k, i, {x, top, x + w, top + h}});
        x += w;
    };
    for (int i = 0; i < 8; ++i) add(Item::Tool, i, btn);
    x += sep;
    for (int i = 0; i < kColorCount; ++i) add(Item::Color, i, sw);
    x += sep;
    for (int i = 0; i < 3; ++i) add(Item::Size, i, e.Px(28));
    x += sep;
    add(Item::Undo, 0, btn);
    add(Item::Redo, 0, btn);
    x += sep;
    add(Item::Copy, 0, btn);
    add(Item::Save, 0, btn);
    x += e.Px(6);
    add(Item::Done, 0, e.Px(78));

    e.imageArea = {0, e.Px(kToolbarH), rc.right, std::max<LONG>(e.Px(kToolbarH) + 1, rc.bottom - e.Px(kStatusH))};
    if (e.fit) {
        const float zoom = FitZoom(e);
        const bool changed = std::fabs(zoom - e.zoom) > 1e-4f || !e.view.dc;
        e.zoom = zoom;
        ClampOrigin(e);
        if (changed) UpdateView(e);
    } else {
        ClampOrigin(e);
    }
    if (e.back.w != rc.right || e.back.h != rc.bottom) e.back.Create(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
}

void MakeFonts(Editor& e) {
    for (HFONT* f : {&e.ui, &e.icons, &e.bold})
        if (*f) DeleteObject(*f);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, e.dpi);
    e.ui = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_BOLD;
    lf.lfHeight = -e.Px(16);
    e.bold = CreateFontIndirectW(&lf);
    e.icons = CreateFontW(-e.Px(16), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0,
                          L"Segoe MDL2 Assets");
}

// ---- painting ------------------------------------------------------------------------------

void Fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void RoundFill(G::Graphics& g, const RECT& r, const G::Color& c, float rad) {
    G::GraphicsPath p;
    const float x = static_cast<float>(r.left), y = static_cast<float>(r.top);
    const float w = static_cast<float>(r.right - r.left), h = static_cast<float>(r.bottom - r.top), d = rad * 2;
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
    G::SolidBrush b(c);
    g.FillPath(&b, &p);
}

void ToolIcon(Editor& e, G::Graphics& g, HDC dc, Tool t, const RECT& r) {
    const float cx = (r.left + r.right) / 2.f, cy = (r.top + r.bottom) / 2.f, s = static_cast<float>(e.Px(8));
    const G::Color fg(255, 225, 225, 225);
    G::Pen pen(fg, static_cast<float>(e.Px(2)) * 0.8f);
    pen.SetLineJoin(G::LineJoinRound);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    switch (t) {
    case Tool::Rect:
        g.DrawRectangle(&pen, cx - s, cy - s * 0.75f, 2 * s, 1.5f * s);
        break;
    case Tool::Ellipse:
        g.DrawEllipse(&pen, cx - s, cy - s * 0.75f, 2 * s, 1.5f * s);
        break;
    case Tool::Arrow:
        ArrowPath(g, G::PointF(cx - s, cy + s), G::PointF(cx + s, cy - s), static_cast<float>(e.Px(2)) * 0.8f, fg);
        break;
    case Tool::Line:
        g.DrawLine(&pen, cx - s, cy + s, cx + s, cy - s);
        break;
    case Tool::Text: {
        HGDIOBJ old = SelectObject(dc, e.bold);
        SetTextColor(dc, RGB(225, 225, 225));
        RECT rr = r;
        DrawTextW(dc, L"T", 1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        SelectObject(dc, old);
        break;
    }
    case Tool::Number: {
        G::SolidBrush b(fg);
        g.FillEllipse(&b, cx - s, cy - s, 2 * s, 2 * s);
        G::Font f(&TextFamily(), s * 1.3f, G::FontStyleBold, G::UnitPixel);
        G::StringFormat sf;
        sf.SetAlignment(G::StringAlignmentCenter);
        sf.SetLineAlignment(G::StringAlignmentCenter);
        G::SolidBrush tb(G::Color(255, 40, 40, 40));
        g.DrawString(L"1", -1, &f, G::RectF(cx - s, cy - s + s * 0.08f, 2 * s, 2 * s), &sf, &tb);
        break;
    }
    case Tool::Highlight: {
        G::SolidBrush b(G::Color(200, 255, 214, 0));
        g.FillRectangle(&b, cx - s, cy - s * 0.45f, 2 * s, 0.9f * s);
        g.DrawLine(&pen, cx - s, cy + s * 0.85f, cx + s, cy + s * 0.85f);
        break;
    }
    case Tool::Pixelate: {
        const float q = 2 * s / 3;
        for (int j = 0; j < 3; ++j)
            for (int i = 0; i < 3; ++i) {
                const BYTE v = ((i + j) & 1) ? 120 : 220;
                G::SolidBrush b(G::Color(255, v, v, v));
                g.FillRectangle(&b, cx - s + i * q, cy - s + j * q, q, q);
            }
        break;
    }
    }
}

void Glyph(Editor& e, HDC dc, const wchar_t* glyph, const RECT& r, bool enabled) {
    HGDIOBJ old = SelectObject(dc, e.icons);
    SetTextColor(dc, enabled ? RGB(225, 225, 225) : RGB(100, 100, 100));
    RECT rr = r;
    DrawTextW(dc, glyph, -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

const wchar_t* Tip(const Editor& e, int i) {
    if (i < 0 || i >= static_cast<int>(e.items.size())) return nullptr;
    const ItemRect& it = e.items[i];
    switch (it.kind) {
    case Item::Tool: return kTools[it.index].tip;
    case Item::Color: return L"Color  (1 to 8)";
    case Item::Size: return L"Size: line width, text and markers  ([ and ], or the mouse wheel)";
    case Item::Undo: return L"Undo  (Ctrl+Z)";
    case Item::Redo: return L"Redo  (Ctrl+Y)";
    case Item::Copy: return L"Copy to the clipboard  (Ctrl+C)";
    case Item::Save: return L"Save to the screenshot file  (Ctrl+S)";
    case Item::Done: return L"Copy, save and close  (Enter)";
    }
    return nullptr;
}

void Paint(Editor& e, HDC hdc) {
    HDC b = e.back.dc;
    RECT rc{0, 0, e.back.w, e.back.h};
    Fill(b, rc, RGB(38, 38, 38));

    // image + the shape being drawn
    const int vw = static_cast<int>(std::lround(e.ImgW() * e.zoom)), vh = static_cast<int>(std::lround(e.ImgH() * e.zoom));
    if (e.view.dc) {
        BitBlt(b, e.origin.x, e.origin.y, vw, vh, e.view.dc, 0, 0, SRCCOPY);
    } else {  // 100 % and up: only the visible part, hard pixels
        const RECT img{e.origin.x, e.origin.y, e.origin.x + vw, e.origin.y + vh};
        RECT vis;
        if (IntersectRect(&vis, &img, &e.imageArea)) {
            const int x0 = std::max(0, static_cast<int>(std::floor((vis.left - e.origin.x) / e.zoom)));
            const int y0 = std::max(0, static_cast<int>(std::floor((vis.top - e.origin.y) / e.zoom)));
            const int x1 = std::min(e.ImgW(), static_cast<int>(std::ceil((vis.right - e.origin.x) / e.zoom)));
            const int y1 = std::min(e.ImgH(), static_cast<int>(std::ceil((vis.bottom - e.origin.y) / e.zoom)));
            const int dx0 = e.origin.x + static_cast<int>(std::lround(x0 * e.zoom));
            const int dy0 = e.origin.y + static_cast<int>(std::lround(y0 * e.zoom));
            const int dx1 = e.origin.x + static_cast<int>(std::lround(x1 * e.zoom));
            const int dy1 = e.origin.y + static_cast<int>(std::lround(y1 * e.zoom));
            SetStretchBltMode(b, COLORONCOLOR);
            StretchBlt(b, dx0, dy0, dx1 - dx0, dy1 - dy0, e.committed.dc, x0, y0, x1 - x0, y1 - y0, SRCCOPY);
        }
    }
    {
        G::Graphics g(b);
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(G::TextRenderingHintAntiAlias);
        g.SetClip(G::Rect(e.imageArea.left, e.imageArea.top, e.imageArea.right - e.imageArea.left,
                          e.imageArea.bottom - e.imageArea.top));
        g.TranslateTransform(static_cast<float>(e.origin.x), static_cast<float>(e.origin.y));
        g.ScaleTransform(e.zoom, e.zoom);
        if (e.drawing || e.typing) DrawShape(g, e.live, e.sc, true);
        if (e.typing) {  // caret and a light frame around the text
            const float em = kFontPx[e.live.size] * e.sc, ls = LineSpacing(em);
            const size_t nl = e.live.text.find_last_of(L'\n');
            const std::wstring last = nl == std::wstring::npos ? e.live.text : e.live.text.substr(nl + 1);
            const int lines = 1 + static_cast<int>(std::count(e.live.text.begin(), e.live.text.end(), L'\n'));
            G::Font font(&TextFamily(), em, G::FontStyleBold, G::UnitPixel);
            G::RectF box;
            g.MeasureString(last.c_str(), -1, &font, G::PointF(0, 0), TypoFormat(), &box);
            const float x = e.live.a.X + box.Width + em * 0.08f, y = e.live.a.Y + (lines - 1) * ls;
            G::Pen caret(G::Color(255, 255, 255, 255), std::max(1.5f, em * 0.07f));
            G::Pen caretDark(G::Color(255, 0, 0, 0), std::max(1.5f, em * 0.07f) + 2 / e.zoom);
            g.DrawLine(&caretDark, x, y + em * 0.1f, x, y + em * 1.25f);
            g.DrawLine(&caret, x, y + em * 0.1f, x, y + em * 1.25f);
        }
    }

    // toolbar
    Fill(b, {0, 0, rc.right, e.Px(kToolbarH)}, RGB(28, 28, 28));
    Fill(b, {0, e.Px(kToolbarH) - 1, rc.right, e.Px(kToolbarH)}, RGB(60, 60, 60));
    SetBkMode(b, TRANSPARENT);
    {
        G::Graphics g(b);
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(G::TextRenderingHintAntiAlias);
        for (int i = 0; i < static_cast<int>(e.items.size()); ++i) {
            const ItemRect& it = e.items[i];
            RECT r = it.r;
            const bool selected = (it.kind == Item::Tool && static_cast<int>(e.tool) == it.index) ||
                                  (it.kind == Item::Color && e.color == it.index) ||
                                  (it.kind == Item::Size && e.size == it.index);
            RECT inner = r;
            InflateRect(&inner, -e.Px(2), 0);
            if (it.kind == Item::Done) {
                RoundFill(g, inner, i == e.hover ? GC(RGB(245, 70, 80)) : GC(kAccent), static_cast<float>(e.Px(4)));
            } else if (selected) {
                RoundFill(g, inner, G::Color(255, 72, 72, 72), static_cast<float>(e.Px(4)));
            } else if (i == e.hover) {
                RoundFill(g, inner, G::Color(255, 54, 54, 54), static_cast<float>(e.Px(4)));
            }
            const float cx = (r.left + r.right) / 2.f, cy = (r.top + r.bottom) / 2.f;
            switch (it.kind) {
            case Item::Tool:
                ToolIcon(e, g, b, kTools[it.index].tool, r);
                break;
            case Item::Color: {
                const float d = static_cast<float>(e.Px(16));
                G::SolidBrush br(GC(kColors[it.index]));
                g.FillEllipse(&br, cx - d / 2, cy - d / 2, d, d);
                G::Pen ring(G::Color(255, 110, 110, 110), 1);
                g.DrawEllipse(&ring, cx - d / 2, cy - d / 2, d, d);
                break;
            }
            case Item::Size: {
                const float d = static_cast<float>(e.Px(it.index == 0 ? 4 : it.index == 1 ? 8 : 13));
                G::SolidBrush br(G::Color(255, 225, 225, 225));
                g.FillEllipse(&br, cx - d / 2, cy - d / 2, d, d);
                break;
            }
            case Item::Undo:
                Glyph(e, b, L"\xE7A7", r, !e.shapes.empty());
                break;
            case Item::Redo:
                Glyph(e, b, L"\xE7A6", r, !e.redo.empty());
                break;
            case Item::Copy:
                Glyph(e, b, L"\xE8C8", r, true);
                break;
            case Item::Save:
                Glyph(e, b, L"\xE74E", r, true);
                break;
            case Item::Done: {
                HGDIOBJ old = SelectObject(b, e.ui);
                SetTextColor(b, RGB(255, 255, 255));
                RECT rr = r;
                DrawTextW(b, L"Done", -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(b, old);
                break;
            }
            }
        }
    }

    // status bar
    RECT sb{0, rc.bottom - e.Px(kStatusH), rc.right, rc.bottom};
    Fill(b, sb, RGB(28, 28, 28));
    HGDIOBJ old = SelectObject(b, e.ui);
    wchar_t right[64];
    swprintf_s(right, L"%d × %d   %d%%", e.ImgW(), e.ImgH(), static_cast<int>(std::lround(e.zoom * 100)));
    RECT rr = sb;
    rr.right -= e.Px(10);
    SetTextColor(b, RGB(150, 150, 150));
    DrawTextW(b, right, -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    std::wstring left;
    if (const wchar_t* t = Tip(e, e.hover)) left = t;
    else if (!e.status.empty()) left = e.status;
    else if (e.typing) left = L"Type your text. Enter finishes, Shift+Enter starts a new line, Esc cancels.";
    else left = L"Drag to draw.  Space+drag = move.  Ctrl+wheel = zoom (Ctrl+0 fit).  Enter = copy, save, close.  Esc = close.";
    RECT lr = sb;
    lr.left += e.Px(10);
    lr.right -= e.Px(150);
    SetTextColor(b, e.status.empty() || e.hover >= 0 ? RGB(190, 190, 190) : RGB(120, 220, 130));
    DrawTextW(b, left.c_str(), -1, &lr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(b, old);

    BitBlt(hdc, 0, 0, rc.right, rc.bottom, b, 0, 0, SRCCOPY);
}

void Redraw(Editor& e) { InvalidateRect(e.hwnd, nullptr, FALSE); }

void ShowStatus(Editor& e, const std::wstring& msg) {
    e.status = msg;
    SetTimer(e.hwnd, kTimerStatus, 2500, nullptr);
    Redraw(e);
}

// ---- actions -------------------------------------------------------------------------------

void FinishText(Editor& e) {
    if (!e.typing) return;
    e.typing = false;
    while (!e.live.text.empty() && (e.live.text.back() == L'\n' || e.live.text.back() == L' ')) e.live.text.pop_back();
    if (!e.live.text.empty()) Commit(e, e.live);
    Redraw(e);
}

void Undo(Editor& e) {
    if (e.typing) {
        e.typing = false;
        Redraw(e);
        return;
    }
    if (e.shapes.empty()) return;
    e.redo.push_back(e.shapes.back());
    e.shapes.pop_back();
    e.dirty = true;
    Rebuild(e);
    Redraw(e);
}

void Redo(Editor& e) {
    if (e.redo.empty()) return;
    const Shape s = e.redo.back();
    e.redo.pop_back();
    e.shapes.push_back(s);
    Bake(e, e.committed, s);
    GdiFlush();
    UpdateView(e);
    e.dirty = true;
    Redraw(e);
}

void CopyNow(Editor& e) {
    FinishText(e);
    Output_CopyImage(CurrentImage(e));
    e.dirty = false;
    ShowStatus(e, L"Copied to the clipboard");
}

void SaveNow(Editor& e) {
    FinishText(e);
    const std::wstring p = Output_SaveImage(CurrentImage(e), *e.src, e.path);
    if (p.empty()) {
        ShowStatus(e, L"Could not save the file");
        return;
    }
    e.path = p;
    e.dirty = false;
    ShowStatus(e, L"Saved  " + p);
}

void Done(Editor& e) {
    FinishText(e);
    auto r = std::make_shared<CaptureResult>(*e.src);
    r->img = CurrentImage(e);
    Output_EditDone(std::move(r), e.path);
    e.dirty = false;
    DestroyWindow(e.hwnd);
}

void Close(Editor& e) {
    if (e.dirty && !e.shapes.empty()) {
        const int a = MessageBoxW(e.hwnd, L"Close without saving or copying your edits?", L"Canivete Editor",
                                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (a != IDYES) return;
    }
    DestroyWindow(e.hwnd);
}

void Click(Editor& e, const ItemRect& it) {
    switch (it.kind) {
    case Item::Tool:
        FinishText(e);
        e.tool = kTools[it.index].tool;
        break;
    case Item::Color:
        e.color = it.index;
        if (e.typing) e.live.color = kColors[e.color];
        break;
    case Item::Size:
        e.size = it.index;
        if (e.typing) e.live.size = e.size;
        break;
    case Item::Undo: Undo(e); break;
    case Item::Redo: Redo(e); break;
    case Item::Copy: CopyNow(e); break;
    case Item::Save: SaveNow(e); break;
    case Item::Done: Done(e); return;
    }
    Redraw(e);
}

G::PointF ToImage(const Editor& e, int x, int y) {
    return G::PointF((x - e.origin.x) / e.zoom, (y - e.origin.y) / e.zoom);
}

bool InImage(const Editor& e, G::PointF p) { return p.X >= 0 && p.Y >= 0 && p.X < e.ImgW() && p.Y < e.ImgH(); }

G::PointF Constrain(Tool t, G::PointF a, G::PointF b) {
    const float dx = b.X - a.X, dy = b.Y - a.Y;
    if (t == Tool::Line || t == Tool::Arrow) {
        const double step = 3.14159265358979 / 4;
        const double ang = std::round(std::atan2(dy, dx) / step) * step, len = std::hypot(dx, dy);
        return G::PointF(a.X + static_cast<float>(len * std::cos(ang)), a.Y + static_cast<float>(len * std::sin(ang)));
    }
    const float m = std::max(std::fabs(dx), std::fabs(dy));
    return G::PointF(a.X + std::copysign(m, dx), a.Y + std::copysign(m, dy));
}

int HitItem(const Editor& e, int x, int y) {
    for (int i = 0; i < static_cast<int>(e.items.size()); ++i) {
        const POINT p{x, y};
        if (PtInRect(&e.items[i].r, p)) return i;
    }
    return -1;
}

void StartPan(Editor& e, int x, int y) {
    e.panning = true;
    e.panFrom = {x, y};
    e.panOrigin = e.origin;
    SetCapture(e.hwnd);
}

void EndPan(Editor& e) {
    if (!e.panning) return;
    e.panning = false;
    if (GetCapture() == e.hwnd) ReleaseCapture();
}

void OnLButtonDown(Editor& e, int x, int y) {
    if (const int i = HitItem(e, x, y); i >= 0) {
        Click(e, e.items[i]);
        return;
    }
    const POINT cp{x, y};
    if (!PtInRect(&e.imageArea, cp)) return;
    if (e.space) {
        StartPan(e, x, y);
        return;
    }
    const G::PointF p = ToImage(e, x, y);
    const bool wasTyping = e.typing;
    FinishText(e);
    Shape s;
    s.tool = e.tool;
    s.color = kColors[e.color];
    s.size = e.size;
    s.a = s.b = p;
    if (e.tool == Tool::Text) {
        if (wasTyping || !InImage(e, p)) return;  // a click outside the text just ends it
        s.a.Y -= kFontPx[e.size] * e.sc * 0.65f;  // the click marks the middle of the first line
        e.live = s;
        e.typing = true;
        Redraw(e);
        return;
    }
    if (e.tool == Tool::Number) {
        if (!InImage(e, p)) return;
        s.number = NextNumber(e);
        Commit(e, s);
        Redraw(e);
        return;
    }
    e.live = s;
    e.drawing = true;
    SetCapture(e.hwnd);
}

void OnMouseMove(Editor& e, int x, int y, WPARAM keys) {
    const int h = HitItem(e, x, y);
    if (h != e.hover) {
        e.hover = h;
        Redraw(e);
        TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, e.hwnd, 0};
        TrackMouseEvent(&t);
    }
    if (e.panning) {
        e.origin = {e.panOrigin.x + x - e.panFrom.x, e.panOrigin.y + y - e.panFrom.y};
        ClampOrigin(e);
        Redraw(e);
        return;
    }
    if (!e.drawing) return;
    G::PointF p = ToImage(e, x, y);
    if (keys & MK_SHIFT) p = Constrain(e.live.tool, e.live.a, p);
    e.live.b = p;
    Redraw(e);
}

void OnLButtonUp(Editor& e) {
    if (!e.drawing) return;
    e.drawing = false;
    ReleaseCapture();
    const float dx = std::fabs(e.live.b.X - e.live.a.X) * e.zoom, dy = std::fabs(e.live.b.Y - e.live.a.Y) * e.zoom;
    const bool line = e.live.tool == Tool::Line || e.live.tool == Tool::Arrow;
    if ((line && std::hypot(dx, dy) >= 4) || (!line && dx >= 3 && dy >= 3)) Commit(e, e.live);
    Redraw(e);
}

void OnChar(Editor& e, wchar_t c) {
    if (!e.typing) return;
    if (c == L'\r') {
        if (GetKeyState(VK_SHIFT) < 0) e.live.text += L'\n';
        else FinishText(e);
    } else if (c == L'\b') {
        if (!e.live.text.empty()) e.live.text.pop_back();
    } else if (c == 0x1B) {
        e.typing = false;
    } else if (c == 0x16) {  // Ctrl+V: paste text
        if (OpenClipboard(e.hwnd)) {
            if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
                if (const auto* t = static_cast<const wchar_t*>(GlobalLock(h))) {
                    for (const wchar_t* p = t; *p; ++p)
                        if (*p != L'\r') e.live.text += *p;
                    GlobalUnlock(h);
                }
            }
            CloseClipboard();
        }
    } else if (c >= 32) {
        e.live.text += c;
    }
    Redraw(e);
}

void OnKey(Editor& e, UINT vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    if (e.typing) return;  // keys arrive as WM_CHAR
    if (ctrl) {
        if (vk == 'Z' && !shift) Undo(e);
        else if (vk == 'Y' || (vk == 'Z' && shift)) Redo(e);
        else if (vk == 'C') CopyNow(e);
        else if (vk == 'S') SaveNow(e);
        else if (vk == '0' || vk == VK_NUMPAD0) {  // fit to window
            e.fit = true;
            Layout(e);
            Redraw(e);
        } else if (vk == '1' || vk == VK_NUMPAD1) {  // 100 %
            SetZoom(e, 1, AreaCenter(e));
            Redraw(e);
        } else if (vk == VK_OEM_PLUS || vk == VK_ADD || vk == VK_OEM_MINUS || vk == VK_SUBTRACT) {
            ZoomStep(e, vk == VK_OEM_PLUS || vk == VK_ADD ? 1 : -1, AreaCenter(e));
            Redraw(e);
        }
        return;
    }
    if (vk == VK_ESCAPE) {
        if (e.drawing) {
            e.drawing = false;
            ReleaseCapture();
            Redraw(e);
        } else {
            Close(e);
        }
        return;
    }
    if (vk == VK_RETURN) {
        Done(e);
        return;
    }
    if (vk >= '1' && vk < '1' + kColorCount) {
        e.color = static_cast<int>(vk - '1');
    } else if (vk == VK_OEM_4) {
        e.size = std::max(0, e.size - 1);
    } else if (vk == VK_OEM_6) {
        e.size = std::min(2, e.size + 1);
    } else {
        for (const ToolDef& t : kTools)
            if (t.key == vk) e.tool = t.tool;
    }
    Redraw(e);
}

LRESULT CALLBACK EditorProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Editor* ep = Get(h);
    if (msg == WM_NCCREATE) {
        ep = static_cast<Editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        ep->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(ep));
    }
    if (!ep) return DefWindowProcW(h, msg, wp, lp);
    Editor& e = *ep;
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (e.back.dc) Paint(e, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        Layout(e);
        Redraw(e);
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        RECT r{0, 0, ToolbarWidth(e), e.Px(kToolbarH + kStatusH + 120)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, e.dpi);
        mm->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_DPICHANGED: {
        e.dpi = HIWORD(wp);
        MakeFonts(e);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(e);
        Redraw(e);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            const bool img = PtInRect(&e.imageArea, p) && HitItem(e, p.x, p.y) < 0;
            const wchar_t* cur = e.panning || (img && e.space) ? IDC_SIZEALL
                                 : !img                        ? IDC_ARROW
                                 : e.tool == Tool::Text        ? IDC_IBEAM
                                                               : IDC_CROSS;
            SetCursor(LoadCursorW(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        OnLButtonDown(e, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSEMOVE:
        OnMouseMove(e, GET_X_LPARAM(lp), GET_Y_LPARAM(lp), wp);
        return 0;
    case WM_MOUSELEAVE:
        if (e.hover >= 0) {
            e.hover = -1;
            Redraw(e);
        }
        return 0;
    case WM_LBUTTONUP:
        if (e.panning) EndPan(e);
        else OnLButtonUp(e);
        return 0;
    case WM_MBUTTONDOWN:  // middle-drag pans too
        if (!e.drawing) StartPan(e, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MBUTTONUP:
        EndPan(e);
        return 0;
    case WM_CAPTURECHANGED:
        e.panning = false;
        return 0;
    case WM_KILLFOCUS:
        e.space = false;
        EndPan(e);
        return 0;
    case WM_RBUTTONUP:  // right-click cancels the shape being drawn
        if (e.drawing) {
            e.drawing = false;
            ReleaseCapture();
            Redraw(e);
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {  // Ctrl+wheel zooms at the mouse
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            if (!PtInRect(&e.imageArea, p)) p = AreaCenter(e);
            ZoomStep(e, GET_WHEEL_DELTA_WPARAM(wp) / WHEEL_DELTA, p);
            Redraw(e);
            return 0;
        }
        e.size = std::clamp(e.size + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1), 0, 2);
        if (e.typing) e.live.size = e.size;
        Redraw(e);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && !e.typing) {  // hold Space to drag the view
            if (!e.space && !e.drawing) {
                e.space = true;
                SendMessageW(h, WM_SETCURSOR, reinterpret_cast<WPARAM>(h), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            }
            return 0;
        }
        OnKey(e, static_cast<UINT>(wp));
        return 0;
    case WM_KEYUP:
        if (wp == VK_SPACE && e.space) {
            e.space = false;
            EndPan(e);
            SendMessageW(h, WM_SETCURSOR, reinterpret_cast<WPARAM>(h), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
            return 0;
        }
        break;
    case WM_CHAR:
        OnChar(e, static_cast<wchar_t>(wp));
        return 0;
    case WM_TIMER:
        if (wp == kTimerStatus) {
            KillTimer(h, kTimerStatus);
            e.status.clear();
            Redraw(e);
        }
        return 0;
    case WM_CLOSE:
        Close(e);
        return 0;
    case WM_NCDESTROY: {
        for (HFONT f : {e.ui, e.icons, e.bold})
            if (f) DeleteObject(f);
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        delete ep;
        if (--g_open == 0 && !g_mainWnd) PostQuitMessage(0);
        return DefWindowProcW(h, msg, wp, lp);
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Editor_Open(std::shared_ptr<CaptureResult> r, const std::wstring& path) {
    if (!r || r->img.w <= 0 || r->img.h <= 0) return;
    if (!g_gdipToken) {
        G::GdiplusStartupInput in;
        G::GdiplusStartup(&g_gdipToken, &in, nullptr);
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = EditorProc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = kEditorClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
        registered = RegisterClassExW(&wc) != 0;
    }

    auto* e = new Editor;
    e->src = std::move(r);
    e->path = path;
    const int w = e->ImgW(), h = e->ImgH();
    if (!e->base.Create(w, h) || !e->committed.Create(w, h)) {
        Log(L"Editor: out of memory for %d x %d", w, h);
        delete e;
        return;
    }
    memcpy(e->base.bits, e->src->img.px.data(), static_cast<size_t>(w) * h * 4);
    memcpy(e->committed.bits, e->base.bits, static_cast<size_t>(w) * h * 4);

    // Open on the monitor the capture came from, as large as the image (up to 90 % of the screen).
    HMONITOR mon = MonitorFromRect(&e->src->screenRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    e->dpi = dx;
    e->sc = dx / 96.f;
    MakeFonts(*e);
    const RECT wa = mi.rcWork;
    const int maxW = (wa.right - wa.left) * 9 / 10, maxH = (wa.bottom - wa.top) * 9 / 10;
    RECT frame{0, 0, 0, 0};
    AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0, dx);
    const int fw = frame.right - frame.left, fh = frame.bottom - frame.top;
    const int chromeH = e->Px(kToolbarH + kStatusH) + 2 * e->Px(10) + fh, chromeW = 2 * e->Px(10) + fw;
    const float z = std::min({1.f, static_cast<float>(maxW - chromeW) / w, static_cast<float>(maxH - chromeH) / h});
    int ww = std::max(static_cast<int>(w * z) + chromeW, ToolbarWidth(*e) + fw);
    int wh = std::max(static_cast<int>(h * z) + chromeH, e->Px(kToolbarH + kStatusH + 200) + fh);
    ww = std::min<int>(ww, wa.right - wa.left);
    wh = std::min<int>(wh, wa.bottom - wa.top);
    const int x = wa.left + (wa.right - wa.left - ww) / 2, y = wa.top + (wa.bottom - wa.top - wh) / 2;

    HWND hw = CreateWindowExW(0, kEditorClass, L"Canivete Editor", WS_OVERLAPPEDWINDOW, x, y, ww, wh, nullptr, nullptr,
                              g_hinst, e);
    if (!hw) {
        Log(L"Editor window failed (%lu)", GetLastError());
        delete e;
        return;
    }
    ++g_open;
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hw, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    e->dpi = GetDpiForWindow(hw);
    MakeFonts(*e);
    Layout(*e);
    ShowWindow(hw, SW_SHOWNORMAL);
    SetForegroundWindow(hw);
    Log(L"Editor opened %d x %d", w, h);
}
