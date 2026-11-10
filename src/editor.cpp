// Screenshot editor: boxes, ellipses, arrows, lines, text, numbered markers, highlighter, pixelate
// and blur on top of a capture, crop, a store-page frame around it, then copy / save. Shapes stay
// editable (undo / redo) until the window closes.
// Shape coordinates are image pixels; the window shows the image scaled down to fit.
#include "app.h"
#include "theme.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out

#include <algorithm>
#include <cmath>
#include <sstream>
#pragma comment(lib, "comdlg32.lib")
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

// Same order as kTools (the toolbar shows them in this order).
enum class Tool { Select, Rect, Ellipse, Arrow, Line, Text, Number, Highlight, Pixelate, Blur, Crop };
struct ToolDef {
    Tool tool;
    UINT key;
    const wchar_t* tip;
};
const ToolDef kTools[] = {
    {Tool::Select, 'V', L"Select: click a shape to move, resize, recolor or delete it  (V)"},
    {Tool::Rect, 'R', L"Rectangle  (R)"},      {Tool::Ellipse, 'E', L"Ellipse  (E)"},
    {Tool::Arrow, 'A', L"Arrow  (A)"},         {Tool::Line, 'L', L"Line  (L)"},
    {Tool::Text, 'T', L"Text  (T)"},           {Tool::Number, 'N', L"Numbered marker  (N)"},
    {Tool::Highlight, 'H', L"Highlighter  (H)"}, {Tool::Pixelate, 'P', L"Pixelate, hides details  (P)"},
    {Tool::Blur, 'G', L"Blur: softens what's under it; the size buttons set how much  (G)"},
    {Tool::Crop, 'C', L"Crop: drag a box (Shift keeps the shape), Enter or double-click crops, Esc cancels  (C)"},
};
const COLORREF kColors[] = {RGB(232, 44, 56),  RGB(255, 140, 0),  RGB(255, 214, 0),   RGB(46, 204, 64),
                            RGB(0, 120, 215),  RGB(156, 39, 176), RGB(255, 255, 255), RGB(0, 0, 0)};
constexpr int kColorCount = static_cast<int>(std::size(kColors));
constexpr int kToolCount = static_cast<int>(std::size(kTools));
const float kLineW[] = {2, 4, 7};    // at 100 % display scale, in image pixels
const float kFontPx[] = {16, 24, 36};

struct Shape {
    Tool tool = Tool::Rect;
    COLORREF color = 0;
    int size = 1;
    G::PointF a, b;
    std::wstring text;
    int number = 0;
    float scale = 1;  // text and markers: stretched by their corner handles
};

enum class Item { Tool, Color, Size, Undo, Redo, Frame, Copy, Save, Alpha, NoBg, Upload, Done };
struct ItemRect {
    Item kind;
    int index;
    RECT r;
};

// Store-page frame: the picture on a background with padding, round corners and a shadow.
enum class FrameBg { White, Grey, Dark, Gradient, Clear, Custom };
constexpr int kFrameBgCount = 6;
const wchar_t* const kFrameBgKeys[kFrameBgCount] = {L"white", L"grey", L"dark", L"gradient", L"transparent", L"custom"};
const wchar_t* const kFrameBgNames[kFrameBgCount] = {L"white", L"light grey", L"dark", L"soft gradient",
                                                     L"transparent", L"your color"};
constexpr int kFrameShapeCount = 4;
const wchar_t* const kFrameShapeKeys[kFrameShapeCount] = {L"original", L"16:9", L"1:1", L"4:3"};
const wchar_t* const kFrameShapeNames[kFrameShapeCount] = {L"Original", L"16:9", L"1:1", L"4:3"};
const float kFrameRatio[kFrameShapeCount] = {0, 16.f / 9, 1, 4.f / 3};

struct FrameOpts {
    bool on = false;
    int padding = 8;    // % of the picture's longer side
    int corners = 6;    // corner radius, in 0.5 % steps of the shorter side
    bool shadow = true;
    int softness = 50;  // 0..100
    FrameBg bg = FrameBg::Gradient;
    COLORREF custom = RGB(46, 52, 64);
    int shape = 0;  // kFrameShapeKeys
    bool operator==(const FrameOpts&) const = default;
};

// The frame's controls (panel rows), in Tab order.
enum PanelRow { kRowOn, kRowPadding, kRowCorners, kRowShadow, kRowSoftness, kRowBg, kRowShape, kRowCount };

struct FrameGeom {
    int cw, ch;  // whole picture (canvas)
    int ox, oy;  // where the screenshot sits in it
    int radius;  // its round corners
};

// Undo step: the shape list and the picture under it (a crop changes the picture).
struct Snapshot {
    std::vector<Shape> shapes;
    std::shared_ptr<const Image> img;
};

struct Editor {
    HWND hwnd = nullptr;
    std::shared_ptr<CaptureResult> src;
    std::wstring path;  // file it was saved to ("" = not saved yet)
    float sc = 1;       // display scale of the captured monitor: sizes follow it
    std::shared_ptr<const Image> img;  // the picture being edited (cropped or not), shapes go on top
    Dib base, committed, view, back;
    Dib framed;  // committed inside the frame, for the screen (frame on)
    std::vector<Shape> shapes;
    std::vector<Snapshot> undo, redo;  // before / after each change
    Tool tool = Tool::Rect;
    // crop
    Tool prevTool = Tool::Rect;  // back to it after cropping
    bool cropBox = false;        // a crop box is shown
    int cropDrag = 0;            // 1 drawing a new box, 2 moving it, 3 dragging a handle
    int cropHandle = -1;
    G::RectF crop, cropStart;    // image pixels
    G::PointF cropFrom;
    // frame
    FrameOpts fr;
    bool panel = false;   // frame panel open
    int pfocus = -1;      // focused panel row (keyboard), -1 = none
    int pdrag = -1;       // slider row being dragged
    RECT panelRect{};
    RECT prow[kRowCount]{};  // each row's control area
    FrameOpts layerFor;      // what `layer` was built for
    int layerW = 0, layerH = 0;
    std::vector<uint32_t> layer, layerDisp;  // background + shadow, premultiplied; and over a checkerboard
    std::vector<uint8_t> cornerCov;          // coverage of a round corner, radius x radius
    int color = 0, size = 1;
    bool drawing = false, typing = false;
    Shape live;
    int sel = -1;        // selected shape (handles shown), -1 = none
    int editText = -1;   // text shape being retyped (left out of `committed` meanwhile)
    bool dragging = false;  // moving or resizing the selected shape (left out of `committed` meanwhile)
    int handle = -1;     // which handle is dragged, -1 = the whole shape
    G::PointF dragFrom;
    Shape dragStart;
    std::vector<Shape> before;  // shape list when the drag started (for undo)
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
    int ImgW() const { return base.w; }
    int ImgH() const { return base.h; }
};

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
    static G::FontFamily* f = new G::FontFamily(L"Segoe UI");  // never freed: no GDI+ call after GdiplusShutdown
    return *f;
}

float LineSpacing(float em) {
    G::FontFamily& f = TextFamily();
    return em * f.GetLineSpacing(G::FontStyleBold) / f.GetEmHeight(G::FontStyleBold);
}

void DrawText(G::Graphics& g, const Shape& s, float sc) {
    if (s.text.empty()) return;
    const float em = kFontPx[s.size] * sc * s.scale;
    G::GraphicsPath path;
    path.AddString(s.text.c_str(), -1, &TextFamily(), G::FontStyleBold, em, s.a, TypoFormat());
    G::Pen outline(IsLight(s.color) ? G::Color(220, 0, 0, 0) : G::Color(235, 255, 255, 255), em * 0.16f);
    outline.SetLineJoin(G::LineJoinRound);
    g.DrawPath(&outline, &path);
    G::SolidBrush br(GC(s.color));
    g.FillPath(&br, &path);
}

void DrawNumber(G::Graphics& g, const Shape& s, float sc) {
    const float d = kFontPx[s.size] * sc * s.scale * 1.45f;
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
    case Tool::Select:
        break;
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
    case Tool::Crop:
        break;
    case Tool::Pixelate:
    case Tool::Blur:
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

// One box-blur pass along a line of n values `stride` apart (edges repeat). tmp: n values of scratch.
void BoxLine(uint8_t* p, int n, size_t stride, int k, std::vector<uint8_t>& tmp) {
    for (int i = 0; i < n; ++i) tmp[i] = p[i * stride];
    const int win = 2 * k + 1;
    int sum = 0;
    for (int j = -k; j <= k; ++j) sum += tmp[std::clamp(j, 0, n - 1)];
    for (int i = 0; i < n; ++i) {
        p[i * stride] = static_cast<uint8_t>((sum + win / 2) / win);
        sum += tmp[std::min(i + k + 1, n - 1)] - tmp[std::max(i - k, 0)];
    }
}

// Three box passes each way: close to a gaussian blur with sigma about k.
void BlurPlane(uint8_t* p, int w, int h, int k) {
    if (k < 1 || w < 1 || h < 1) return;
    std::vector<uint8_t> tmp(static_cast<size_t>(std::max(w, h)));
    for (int pass = 0; pass < 3; ++pass) {
        for (int y = 0; y < h; ++y) BoxLine(p + static_cast<size_t>(y) * w, w, 1, k, tmp);
        for (int x = 0; x < w; ++x) BoxLine(p + x, h, static_cast<size_t>(w), k, tmp);
    }
}

// Blur = what's under the box, smoothly blurred (pixels around it count too, so the edge stays clean).
void ApplyBlur(Dib& d, RECT r, int k) {
    RECT all{0, 0, d.w, d.h};
    if (!IntersectRect(&r, &r, &all)) return;
    const int m = 3 * k;
    RECT o{std::max(0L, r.left - m), std::max(0L, r.top - m), std::min<LONG>(d.w, r.right + m),
           std::min<LONG>(d.h, r.bottom + m)};
    const int w = o.right - o.left, h = o.bottom - o.top;
    std::vector<uint8_t> ch[3];
    for (int c = 0; c < 3; ++c) ch[c].resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            const uint32_t v = d.bits[static_cast<size_t>(y + o.top) * d.w + x + o.left];
            const size_t i = static_cast<size_t>(y) * w + x;
            ch[0][i] = static_cast<uint8_t>(v);
            ch[1][i] = static_cast<uint8_t>(v >> 8);
            ch[2][i] = static_cast<uint8_t>(v >> 16);
        }
    for (auto& c : ch) BlurPlane(c.data(), w, h, k);
    for (int y = r.top; y < r.bottom; ++y)
        for (int x = r.left; x < r.right; ++x) {
            const size_t i = static_cast<size_t>(y - o.top) * w + (x - o.left);
            d.bits[static_cast<size_t>(y) * d.w + x] = ch[0][i] | (ch[1][i] << 8) | (ch[2][i] << 16);
        }
}

int BlurRadius(int size, float sc) {
    const int k[] = {3, 7, 14};
    return std::max(1, static_cast<int>(std::lround(k[std::clamp(size, 0, 2)] * sc)));
}

RECT ToRect(const G::RectF& r) {
    return {static_cast<LONG>(std::floor(r.X)), static_cast<LONG>(std::floor(r.Y)),
            static_cast<LONG>(std::ceil(r.X + r.Width)), static_cast<LONG>(std::ceil(r.Y + r.Height))};
}

// Draws one finished shape into the full-size image.
void Bake(Editor& e, Dib& d, const Shape& s) {
    if (s.tool == Tool::Highlight || s.tool == Tool::Pixelate || s.tool == Tool::Blur) {
        GdiFlush();
        const RECT r = ToRect(Norm(s.a, s.b));
        if (s.tool == Tool::Highlight) ApplyHighlight(d, r, s.color);
        else if (s.tool == Tool::Blur) ApplyBlur(d, r, BlurRadius(s.size, e.sc));
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

// ---- store-page frame ------------------------------------------------------------------------

FrameGeom Geom(const FrameOpts& f, int w, int h) {
    if (!f.on) return {w, h, 0, 0, 0};
    const int pad = static_cast<int>(std::lround(std::max(w, h) * f.padding / 100.0));
    int cw = w + 2 * pad, ch = h + 2 * pad;
    if (const float r = kFrameRatio[std::clamp(f.shape, 0, kFrameShapeCount - 1)]; r > 0) {
        if (cw < ch * r) cw = static_cast<int>(std::ceil(ch * r - 0.01f));
        else ch = static_cast<int>(std::ceil(cw / r - 0.01f));
    }
    const int radius = static_cast<int>(std::lround(std::min(w, h) * f.corners / 200.0));
    return {cw, ch, (cw - w) / 2, (ch - h) / 2, std::min({radius, w / 2, h / 2})};
}

FrameGeom Geom(const Editor& e) { return Geom(e.fr, e.ImgW(), e.ImgH()); }

uint32_t Premul(COLORREF c) {
    return 0xFF000000u | (static_cast<uint32_t>(GetRValue(c)) << 16) | (GetGValue(c) << 8) | GetBValue(c);
}

COLORREF FrameBgColor(const FrameOpts& f) {
    switch (f.bg) {
    case FrameBg::White: return RGB(255, 255, 255);
    case FrameBg::Grey: return RGB(238, 239, 242);
    case FrameBg::Dark: return RGB(30, 32, 38);
    case FrameBg::Custom: return f.custom;
    case FrameBg::Gradient:
    case FrameBg::Clear: break;
    }
    return RGB(255, 255, 255);
}

// The soft gradient: lavender top-left to light sky blue bottom-right.
uint32_t GradientAt(float t) {
    const float a[3] = {244, 236, 255}, b[3] = {205, 230, 250};  // R G B
    uint32_t v = 0xFF000000u;
    for (int i = 0; i < 3; ++i)
        v |= static_cast<uint32_t>(std::lround(a[i] + (b[i] - a[i]) * t)) << (16 - 8 * i);
    return v;
}

// Coverage (0..255) of a round corner of radius r, pixel (i, j) counted from the corner, 4 x 4 samples.
std::vector<uint8_t> CornerCoverage(int r) {
    std::vector<uint8_t> c(static_cast<size_t>(r) * r);
    const float rr = static_cast<float>(r) * r;
    for (int j = 0; j < r; ++j)
        for (int i = 0; i < r; ++i) {
            int in = 0;
            for (int sy = 0; sy < 4; ++sy)
                for (int sx = 0; sx < 4; ++sx) {
                    const float dx = r - (i + (sx + 0.5f) / 4), dy = r - (j + (sy + 0.5f) / 4);
                    in += dx * dx + dy * dy <= rr;
                }
            c[static_cast<size_t>(j) * r + i] = static_cast<uint8_t>((in * 255 + 8) / 16);
        }
    return c;
}

// Coverage of the rounded picture at canvas-relative pixel (x, y) of a w x h box with corner table.
inline int RoundCov(const std::vector<uint8_t>& cov, int r, int x, int y, int w, int h) {
    if (r <= 0) return 255;
    const int i = x < r ? x : x >= w - r ? w - 1 - x : -1;
    const int j = y < r ? y : y >= h - r ? h - 1 - y : -1;
    if (i < 0 || j < 0) return 255;
    return cov[static_cast<size_t>(j) * r + i];
}

// Background and shadow for the current picture size and options (only rebuilt when they change).
void BuildLayer(Editor& e) {
    const FrameGeom g = Geom(e);
    if (e.layerFor == e.fr && e.layerW == e.ImgW() && e.layerH == e.ImgH() &&
        e.layer.size() == static_cast<size_t>(g.cw) * g.ch)
        return;
    e.layerFor = e.fr;
    e.layerW = e.ImgW();
    e.layerH = e.ImgH();
    const int cw = g.cw, ch = g.ch, w = e.ImgW(), h = e.ImgH();
    e.layer.assign(static_cast<size_t>(cw) * ch, 0);
    if (e.fr.bg == FrameBg::Gradient) {
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x)
                e.layer[static_cast<size_t>(y) * cw + x] =
                    GradientAt((static_cast<float>(x) / cw + static_cast<float>(y) / ch) / 2);
    } else if (e.fr.bg != FrameBg::Clear) {
        std::fill(e.layer.begin(), e.layer.end(), Premul(FrameBgColor(e.fr)));
    }
    e.cornerCov = CornerCoverage(g.radius);

    if (e.fr.shadow) {
        // A blurred copy of the rounded picture, a little lower, worked out at a smaller size (it's soft).
        const int big = std::max(w, h);
        const float sigma = 1.5f + e.fr.softness / 100.f * 0.035f * big;
        const float dy = std::max(2.f, 0.25f * sigma + 0.002f * big);
        const int s = std::clamp(static_cast<int>(sigma / 4), 1, 16);
        const int lw = cw / s + 2, lh = ch / s + 2;
        std::vector<uint8_t> m(static_cast<size_t>(lw) * lh, 0);
        for (int y = 0; y < lh; ++y)
            for (int x = 0; x < lw; ++x) {
                int in = 0;
                for (int sy = 0; sy < 2; ++sy)
                    for (int sx = 0; sx < 2; ++sx) {
                        const int px = static_cast<int>((x + (sx + 0.5f) / 2) * s) - g.ox;
                        const int py = static_cast<int>((y + (sy + 0.5f) / 2) * s - dy) - g.oy;
                        if (px >= 0 && py >= 0 && px < w && py < h) in += RoundCov(e.cornerCov, g.radius, px, py, w, h);
                    }
                m[static_cast<size_t>(y) * lw + x] = static_cast<uint8_t>(in / 4);
            }
        BlurPlane(m.data(), lw, lh, std::max(1, static_cast<int>(std::lround(sigma / s))));
        const bool darkBg = e.fr.bg == FrameBg::Dark || (e.fr.bg == FrameBg::Custom && !IsLight(e.fr.custom));
        const float opacity = darkBg ? 0.6f : e.fr.bg == FrameBg::Clear ? 0.4f : 0.3f;
        for (int y = 0; y < ch; ++y) {
            const float fy = std::clamp((y + 0.5f) / s - 0.5f, 0.f, lh - 1.001f);
            const int y0 = static_cast<int>(fy);
            const float ty = fy - y0;
            for (int x = 0; x < cw; ++x) {
                const float fx = std::clamp((x + 0.5f) / s - 0.5f, 0.f, lw - 1.001f);
                const int x0 = static_cast<int>(fx);
                const float tx = fx - x0;
                const uint8_t* r0 = &m[static_cast<size_t>(y0) * lw + x0];
                const float v = (r0[0] * (1 - tx) + r0[1] * tx) * (1 - ty) + (r0[lw] * (1 - tx) + r0[lw + 1] * tx) * ty;
                const uint32_t sa = static_cast<uint32_t>(std::lround(v * opacity));
                if (!sa) continue;
                uint32_t& p = e.layer[static_cast<size_t>(y) * cw + x];
                const uint32_t k = 255 - sa;
                auto chn = [&](int sh) { return (((p >> sh) & 255) * k + 127) / 255; };
                p = ((sa + chn(24)) << 24) | (chn(16) << 16) | (chn(8) << 8) | chn(0);
            }
        }
    }

    // For the screen: a see-through background shows a checkerboard.
    e.layerDisp = e.layer;
    if (e.fr.bg == FrameBg::Clear) {
        const int sq = std::max(6, std::max(cw, ch) / 120);
        for (int y = 0; y < ch; ++y)
            for (int x = 0; x < cw; ++x) {
                uint32_t& p = e.layerDisp[static_cast<size_t>(y) * cw + x];
                const uint32_t bg = ((x / sq + y / sq) & 1) ? 0xE4E4E4u : 0xFFFFFFu, k = 255 - (p >> 24);
                auto chn = [&](int sh) { return ((p >> sh) & 255) + (((bg >> sh) & 255) * k + 127) / 255; };
                p = 0xFF000000u | (chn(16) << 16) | (chn(8) << 8) | chn(0);
            }
    }
}

// The framed picture (premultiplied BGRA) from `committed`: layer outside, picture with round corners inside.
void Compose(Editor& e, uint32_t* out, bool display) {
    BuildLayer(e);
    const FrameGeom g = Geom(e);
    const std::vector<uint32_t>& L = display ? e.layerDisp : e.layer;
    memcpy(out, L.data(), L.size() * 4);
    const int w = e.ImgW(), h = e.ImgH();
    GdiFlush();
    for (int y = 0; y < h; ++y) {
        const uint32_t* s = e.committed.bits + static_cast<size_t>(y) * w;
        uint32_t* d = out + static_cast<size_t>(y + g.oy) * g.cw + g.ox;
        const bool cornerRow = y < g.radius || y >= h - g.radius;
        for (int x = 0; x < w; ++x) {
            const int c = cornerRow ? RoundCov(e.cornerCov, g.radius, x, y, w, h) : 255;
            if (c == 255) {
                d[x] = 0xFF000000u | (s[x] & 0xFFFFFFu);
            } else if (c > 0) {
                const uint32_t k = 255 - c, p = d[x], v = s[x] | 0xFF000000u;
                auto chn = [&](int sh) { return (((v >> sh) & 255) * c + ((p >> sh) & 255) * k + 127) / 255; };
                d[x] = (chn(24) << 24) | (chn(16) << 16) | (chn(8) << 8) | chn(0);
            }
        }
    }
}

const Dib& Disp(const Editor& e) { return e.fr.on && e.framed.dc ? e.framed : e.committed; }
int CanvasW(const Editor& e) { return Disp(e).w; }
int CanvasH(const Editor& e) { return Disp(e).h; }
// Top-left of the whole picture (frame included) on screen; `origin` is the screenshot's.
POINT CanvasOrigin(const Editor& e) {
    const FrameGeom g = Geom(e);
    if (!e.fr.on) return e.origin;
    return {e.origin.x - std::lround(g.ox * e.zoom), e.origin.y - std::lround(g.oy * e.zoom)};
}

void UpdateView(Editor& e) {
    const Dib& src = Disp(e);
    const int vw = std::max(1, static_cast<int>(std::lround(src.w * e.zoom)));
    const int vh = std::max(1, static_cast<int>(std::lround(src.h * e.zoom)));
    if (e.zoom >= 0.999f) {
        e.view.Free();
        return;
    }
    if (e.view.w != vw || e.view.h != vh) e.view.Create(vw, vh);
    SetStretchBltMode(e.view.dc, HALFTONE);
    SetBrushOrgEx(e.view.dc, 0, 0, nullptr);
    StretchBlt(e.view.dc, 0, 0, vw, vh, src.dc, 0, 0, src.w, src.h, SRCCOPY);
}

// `committed` changed: the framed copy and the zoomed view follow.
void Refresh(Editor& e) {
    GdiFlush();
    if (e.fr.on) {
        const FrameGeom g = Geom(e);
        if (e.framed.w != g.cw || e.framed.h != g.ch) e.framed.Create(g.cw, g.ch);
        if (e.framed.dc) {
            Compose(e, e.framed.bits, true);
            GdiFlush();
        }
    } else {
        e.framed.Free();
    }
    UpdateView(e);
}

// Bakes every shape into `committed`, except `skip` (the one being moved or retyped, drawn live).
void Rebuild(Editor& e, int skip = -1) {
    GdiFlush();
    memcpy(e.committed.bits, e.base.bits, static_cast<size_t>(e.base.w) * e.base.h * 4);
    for (int i = 0; i < static_cast<int>(e.shapes.size()); ++i)
        if (i != skip) Bake(e, e.committed, e.shapes[i]);
    Refresh(e);
}

void PushUndo(Editor& e, std::vector<Shape> state) {
    e.undo.push_back({std::move(state), e.img});
    e.redo.clear();
    e.dirty = true;
}

// Adds a new shape on top and selects it.
void Commit(Editor& e, const Shape& s) {
    PushUndo(e, e.shapes);
    e.shapes.push_back(s);
    e.sel = static_cast<int>(e.shapes.size()) - 1;
    Bake(e, e.committed, s);
    Refresh(e);
}

// ---- selecting, moving and resizing shapes ----------------------------------------------------

bool IsBox(Tool t) {
    return t == Tool::Rect || t == Tool::Ellipse || t == Tool::Highlight || t == Tool::Pixelate || t == Tool::Blur;
}
bool IsLine(Tool t) { return t == Tool::Line || t == Tool::Arrow; }

// Text and marker outline at scale 1, relative to the shape's anchor point `a`.
G::RectF RelBox(const Shape& s, float sc) {
    if (s.tool == Tool::Number) {
        const float d = kFontPx[s.size] * sc * 1.45f;
        return G::RectF(-d / 2, -d / 2, d, d);
    }
    if (s.text.empty()) return G::RectF(0, 0, 0, 0);
    G::GraphicsPath path;
    path.AddString(s.text.c_str(), -1, &TextFamily(), G::FontStyleBold, kFontPx[s.size] * sc, G::PointF(0, 0),
                   TypoFormat());
    G::RectF r;
    path.GetBounds(&r);
    const float pad = kFontPx[s.size] * sc * 0.08f;  // the outline drawn around the letters
    r.Inflate(pad, pad);
    return r;
}

G::RectF Bounds(const Shape& s, float sc) {
    if (IsBox(s.tool) || IsLine(s.tool)) return Norm(s.a, s.b);
    const G::RectF r = RelBox(s, sc);
    return G::RectF(s.a.X + r.X * s.scale, s.a.Y + r.Y * s.scale, r.Width * s.scale, r.Height * s.scale);
}

// Handle ids: 0 top-left, 1 top, 2 top-right, 3 right, 4 bottom-right, 5 bottom, 6 bottom-left, 7 left;
// lines use 0 = start, 1 = end.
G::PointF BoxPoint(const G::RectF& r, int id) {
    const float x[] = {r.X, r.X + r.Width / 2, r.GetRight(), r.GetRight(), r.GetRight(), r.X + r.Width / 2, r.X, r.X};
    const float y[] = {r.Y, r.Y, r.Y, r.Y + r.Height / 2, r.GetBottom(), r.GetBottom(), r.GetBottom(), r.Y + r.Height / 2};
    return G::PointF(x[id], y[id]);
}

std::vector<std::pair<int, G::PointF>> Handles(const Shape& s, float sc) {
    std::vector<std::pair<int, G::PointF>> h;
    if (IsLine(s.tool)) return {{0, s.a}, {1, s.b}};
    const G::RectF r = Bounds(s, sc);
    for (int id = 0; id < 8; ++id)
        if (IsBox(s.tool) || id % 2 == 0) h.push_back({id, BoxPoint(r, id)});  // text, markers: corners only
    return h;
}

float SegmentDistance(G::PointF p, G::PointF a, G::PointF b) {
    const float dx = b.X - a.X, dy = b.Y - a.Y, len2 = dx * dx + dy * dy;
    const float t = len2 > 0 ? std::clamp(((p.X - a.X) * dx + (p.Y - a.Y) * dy) / len2, 0.f, 1.f) : 0.f;
    return std::hypot(p.X - (a.X + t * dx), p.Y - (a.Y + t * dy));
}

// tol = how far off the line still counts, in image pixels.
bool HitShape(const Shape& s, float sc, G::PointF p, float tol) {
    const G::RectF r = Bounds(s, sc);
    const float w = kLineW[s.size] * sc / 2 + tol;
    switch (s.tool) {
    case Tool::Rect: {
        G::RectF outer = r, inner = r;
        outer.Inflate(w, w);
        inner.Inflate(-w, -w);
        return outer.Contains(p) && !(inner.Width > 0 && inner.Height > 0 && inner.Contains(p));
    }
    case Tool::Ellipse: {
        const float rx = r.Width / 2, ry = r.Height / 2;
        if (rx < 1 || ry < 1) return SegmentDistance(p, s.a, s.b) <= w;
        const float nx = (p.X - r.X - rx) / rx, ny = (p.Y - r.Y - ry) / ry;
        return std::fabs(std::sqrt(nx * nx + ny * ny) - 1) * std::min(rx, ry) <= w;
    }
    case Tool::Line:
    case Tool::Arrow:
        return SegmentDistance(p, s.a, s.b) <= w + (s.tool == Tool::Arrow ? kLineW[s.size] * sc : 0);
    default: {
        G::RectF rr = r;
        rr.Inflate(tol / 2, tol / 2);
        return rr.Contains(p);
    }
    }
}

float HitTol(const Editor& e) { return e.Px(6) / e.zoom; }

int ShapeAt(const Editor& e, G::PointF p) {
    for (int i = static_cast<int>(e.shapes.size()) - 1; i >= 0; --i)
        if (HitShape(e.shapes[i], e.sc, p, HitTol(e))) return i;
    return -1;
}

int HandleAt(const Editor& e, G::PointF p) {
    if (e.sel < 0 || e.typing) return -1;
    const float r = e.Px(7) / e.zoom;
    for (const auto& [id, h] : Handles(e.shapes[e.sel], e.sc))
        if (std::fabs(p.X - h.X) <= r && std::fabs(p.Y - h.Y) <= r) return id;
    return -1;
}

// The shape under the mouse that a press would grab: any shape with the Select tool, otherwise only
// the selected one (so drawing next to a shape still starts a new one).
int GrabAt(const Editor& e, G::PointF p) {
    if (e.typing) return -1;
    if (e.tool == Tool::Select) return ShapeAt(e, p);
    return e.sel >= 0 && HitShape(e.shapes[e.sel], e.sc, p, HitTol(e)) ? e.sel : -1;
}

G::PointF Constrain(Tool t, G::PointF a, G::PointF b);

void StartDrag(Editor& e, int index, int handle, G::PointF p) {
    e.sel = index;
    e.dragging = true;
    e.handle = handle;
    e.dragFrom = p;
    e.dragStart = e.shapes[index];
    e.before = e.shapes;
    Rebuild(e, index);
    SetCapture(e.hwnd);
}

void DragTo(Editor& e, G::PointF p, bool shift) {
    Shape s = e.dragStart;
    const Shape& o = e.dragStart;
    if (e.handle < 0) {  // move
        float dx = p.X - e.dragFrom.X, dy = p.Y - e.dragFrom.Y;
        if (shift) (std::fabs(dx) > std::fabs(dy) ? dy : dx) = 0;  // straight across or up/down
        s.a = G::PointF(o.a.X + dx, o.a.Y + dy);
        s.b = G::PointF(o.b.X + dx, o.b.Y + dy);
    } else if (IsLine(s.tool)) {
        G::PointF& end = e.handle == 0 ? s.a : s.b;
        end = shift ? Constrain(s.tool, e.handle == 0 ? s.b : s.a, p) : p;
    } else if (IsBox(s.tool)) {
        const G::RectF r = Norm(o.a, o.b);
        float l = r.X, t = r.Y, rt = r.GetRight(), b = r.GetBottom();
        const int h = e.handle;
        if (h == 0 || h == 6 || h == 7) l = p.X;
        if (h == 2 || h == 3 || h == 4) rt = p.X;
        if (h == 0 || h == 1 || h == 2) t = p.Y;
        if (h == 4 || h == 5 || h == 6) b = p.Y;
        s.a = G::PointF(l, t);
        s.b = G::PointF(rt, b);
    } else {  // text, marker: stretch from the opposite corner, keeping the shape
        const G::RectF rel = RelBox(o, e.sc);
        const G::PointF relCorner = BoxPoint(rel, (e.handle + 4) % 8);  // at scale 1, from the anchor
        const G::PointF fixed(o.a.X + relCorner.X * o.scale, o.a.Y + relCorner.Y * o.scale);
        const G::PointF grab = BoxPoint(Bounds(o, e.sc), e.handle);
        const float d0 = std::hypot(grab.X - fixed.X, grab.Y - fixed.Y);
        const float d1 = std::hypot(p.X - fixed.X, p.Y - fixed.Y);
        if (d0 > 0.5f) s.scale = std::clamp(o.scale * d1 / d0, 0.25f, 12.f);
        s.a = G::PointF(fixed.X - relCorner.X * s.scale, fixed.Y - relCorner.Y * s.scale);
    }
    e.shapes[e.sel] = s;
}

void EndDrag(Editor& e, bool cancel) {
    if (!e.dragging) return;
    e.dragging = false;
    if (GetCapture() == e.hwnd) ReleaseCapture();
    const Shape& now = e.shapes[e.sel];
    const bool moved = now.a.X != e.dragStart.a.X || now.a.Y != e.dragStart.a.Y || now.b.X != e.dragStart.b.X ||
                       now.b.Y != e.dragStart.b.Y || now.scale != e.dragStart.scale;
    if (cancel) e.shapes = e.before;
    else if (moved) PushUndo(e, std::move(e.before));
    e.before.clear();
    Rebuild(e);
}

// Changes the selected shape (color, size, position) as one undo step.
template <class F>
void ChangeSelected(Editor& e, F change) {
    if (e.sel < 0 || e.typing || e.dragging) return;
    std::vector<Shape> old = e.shapes;
    change(e.shapes[e.sel]);
    PushUndo(e, std::move(old));
    Rebuild(e);
}

void DeleteSelected(Editor& e) {
    if (e.sel < 0 || e.typing || e.dragging) return;
    PushUndo(e, e.shapes);
    e.shapes.erase(e.shapes.begin() + e.sel);
    e.sel = -1;
    Rebuild(e);
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

// What Copy, Save, Done and Upload give: the picture, in its frame when the frame is on.
Image OutputImage(Editor& e) {
    if (!e.fr.on) return CurrentImage(e);
    const FrameGeom g = Geom(e);
    Image img;
    img.w = g.cw;
    img.h = g.ch;
    img.px.resize(static_cast<size_t>(g.cw) * g.ch);
    Compose(e, img.px.data(), false);
    if (e.fr.bg == FrameBg::Clear) {  // straight alpha for the PNG
        img.alpha = true;
        for (uint32_t& p : img.px) {
            const uint32_t a = p >> 24;
            if (a == 0) p = 0x00FFFFFFu;
            else if (a < 255) {
                auto chn = [&](int sh) { return std::min<uint32_t>(255, (((p >> sh) & 255) * 255 + a / 2) / a); };
                p = (a << 24) | (chn(16) << 16) | (chn(8) << 8) | chn(0);
            }
        }
    }
    return img;
}

// ---- layout --------------------------------------------------------------------------------

constexpr int kToolbarH = 44, kStatusH = 24, kPanelW = 252;

int ToolbarWidth(const Editor& e) {
    const int btn = e.Px(34), sw = e.Px(26), sep = e.Px(14);
    return e.Px(8) + kToolCount * btn + sep + kColorCount * sw + sep + 3 * e.Px(28) + sep + 2 * btn + sep + btn + sep +
           5 * btn + e.Px(6) + e.Px(78) + e.Px(8);
}

// ---- zoom and pan ---------------------------------------------------------------------------

constexpr float kMinZoom = 0.02f, kMaxZoom = 16.f;

float FitZoom(const Editor& e) {
    const int aw = e.imageArea.right - e.imageArea.left - 2 * e.Px(10);
    const int ah = e.imageArea.bottom - e.imageArea.top - 2 * e.Px(10);
    const float z = std::min({1.f, static_cast<float>(aw) / CanvasW(e), static_cast<float>(ah) / CanvasH(e)});
    return std::max(kMinZoom, z);
}

// An image smaller than the view is centred; a bigger one can't be dragged past its edges.
void ClampOrigin(Editor& e) {
    const int m = e.Px(10);
    auto axis = [m](LONG& o, LONG lo, LONG hi, int v) {
        if (v <= hi - lo - 2 * m) o = lo + (hi - lo - v) / 2;
        else o = std::clamp<LONG>(o, hi - m - v, lo + m);
    };
    // Works on the whole picture (frame included), then back to the screenshot's corner.
    const POINT c0 = CanvasOrigin(e);
    POINT c = c0;
    axis(c.x, e.imageArea.left, e.imageArea.right, static_cast<int>(std::lround(CanvasW(e) * e.zoom)));
    axis(c.y, e.imageArea.top, e.imageArea.bottom, static_cast<int>(std::lround(CanvasH(e) * e.zoom)));
    e.origin.x += c.x - c0.x;
    e.origin.y += c.y - c0.y;
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
    for (int i = 0; i < kToolCount; ++i) add(Item::Tool, i, btn);
    x += sep;
    for (int i = 0; i < kColorCount; ++i) add(Item::Color, i, sw);
    x += sep;
    for (int i = 0; i < 3; ++i) add(Item::Size, i, e.Px(28));
    x += sep;
    add(Item::Undo, 0, btn);
    add(Item::Redo, 0, btn);
    x += sep;
    add(Item::Frame, 0, btn);
    x += sep;
    add(Item::Copy, 0, btn);
    add(Item::Save, 0, btn);
    add(Item::Alpha, 0, btn);
    add(Item::NoBg, 0, btn);
    add(Item::Upload, 0, btn);
    x += e.Px(6);
    add(Item::Done, 0, e.Px(78));

    const LONG areaBottom = std::max<LONG>(e.Px(kToolbarH) + 1, rc.bottom - e.Px(kStatusH));
    e.imageArea = {0, e.Px(kToolbarH), rc.right, areaBottom};
    if (e.panel) {  // the frame panel takes the right side
        e.panelRect = {std::max<LONG>(e.Px(120), rc.right - e.Px(kPanelW)), e.Px(kToolbarH), rc.right, areaBottom};
        e.imageArea.right = e.panelRect.left;
        const int l = e.panelRect.left + e.Px(16), r = e.panelRect.right - e.Px(16);
        int y = e.panelRect.top + e.Px(12);
        const int heights[kRowCount] = {36, 52, 52, 36, 52, 62, 62};
        for (int i = 0; i < kRowCount; ++i) {
            e.prow[i] = {l, y, r, y + e.Px(heights[i])};
            y += e.Px(heights[i]) + e.Px(i == kRowOn ? 10 : 6);
        }
    } else {
        e.panelRect = {};
    }
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
    const G::Color fg = GC(Pal().icon);
    G::Pen pen(fg, static_cast<float>(e.Px(2)) * 0.8f);
    pen.SetLineJoin(G::LineJoinRound);
    pen.SetStartCap(G::LineCapRound);
    pen.SetEndCap(G::LineCapRound);
    switch (t) {
    case Tool::Select: {  // mouse pointer
        const float px[] = {-0.55f, -0.55f, -0.12f, 0.18f, 0.42f, 0.12f, 0.62f};
        const float py[] = {-1.0f, 0.62f, 0.24f, 0.98f, 0.86f, 0.12f, 0.12f};
        G::PointF pts[7];
        for (int i = 0; i < 7; ++i) pts[i] = G::PointF(cx + px[i] * s, cy + py[i] * s);
        G::SolidBrush b(fg);
        g.FillPolygon(&b, pts, 7);
        break;
    }
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
        SetTextColor(dc, Pal().icon);
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
        G::SolidBrush tb(GC(Pal().numText));
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
    case Tool::Blur: {  // a soft dot: rings fading out
        for (int i = 5; i >= 1; --i) {
            const float rr = s * (0.35f + 0.13f * i);
            G::SolidBrush b(GC(Pal().icon, static_cast<BYTE>(255 - i * 42)));
            g.FillEllipse(&b, cx - rr, cy - rr, 2 * rr, 2 * rr);
        }
        break;
    }
    case Tool::Crop: {  // two crossing corner brackets
        const G::PointF a[3] = {G::PointF(cx - s * 0.55f, cy - s), G::PointF(cx - s * 0.55f, cy + s * 0.55f),
                                G::PointF(cx + s, cy + s * 0.55f)};
        const G::PointF b2[3] = {G::PointF(cx - s, cy - s * 0.55f), G::PointF(cx + s * 0.55f, cy - s * 0.55f),
                                 G::PointF(cx + s * 0.55f, cy + s)};
        g.DrawLines(&pen, a, 3);
        g.DrawLines(&pen, b2, 3);
        break;
    }
    }
}

// The frame button: a rounded picture inside a larger frame.
void FrameIcon(Editor& e, G::Graphics& g, const RECT& r) {
    const float cx = (r.left + r.right) / 2.f, cy = (r.top + r.bottom) / 2.f, s = static_cast<float>(e.Px(8));
    G::Pen pen(GC(Pal().icon), static_cast<float>(e.Px(2)) * 0.7f);
    g.DrawRectangle(&pen, cx - s * 1.1f, cy - s * 0.85f, 2.2f * s, 1.7f * s);
    const RECT in{static_cast<LONG>(cx - s * 0.6f), static_cast<LONG>(cy - s * 0.4f), static_cast<LONG>(cx + s * 0.6f),
                  static_cast<LONG>(cy + s * 0.45f)};
    G::GraphicsPath p;
    const float x = static_cast<float>(in.left), y = static_cast<float>(in.top), d = s * 0.5f;
    const float w = static_cast<float>(in.right - in.left), h = static_cast<float>(in.bottom - in.top);
    p.AddArc(x, y, d, d, 180, 90);
    p.AddArc(x + w - d, y, d, d, 270, 90);
    p.AddArc(x + w - d, y + h - d, d, d, 0, 90);
    p.AddArc(x, y + h - d, d, d, 90, 90);
    p.CloseFigure();
    G::SolidBrush b(GC(Pal().icon));
    g.FillPath(&b, &p);
}

void Glyph(Editor& e, HDC dc, const wchar_t* glyph, const RECT& r, bool enabled) {
    HGDIOBJ old = SelectObject(dc, e.icons);
    SetTextColor(dc, enabled ? Pal().icon : Pal().iconOff);
    RECT rr = r;
    DrawTextW(dc, glyph, -1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}

const wchar_t* Tip(const Editor& e, int i) {
    if (i < 0 || i >= static_cast<int>(e.items.size())) return nullptr;
    const ItemRect& it = e.items[i];
    switch (it.kind) {
    case Item::Tool: return kTools[it.index].tip;
    case Item::Color: return L"Color  (1 to 8); also changes the selected shape";
    case Item::Size: return L"Size: line width, text and markers  ([ and ], or the mouse wheel); also changes the selected shape";
    case Item::Undo: return L"Undo  (Ctrl+Z)";
    case Item::Redo: return L"Redo  (Ctrl+Y)";
    case Item::Frame:
        return L"Store-page frame: padding, round corners, shadow and a background around the picture  (F)";
    case Item::Copy: return L"Copy to the clipboard  (Ctrl+C)";
    case Item::Save: return L"Save to the screenshot file  (Ctrl+S)";
    case Item::Alpha: return L"Make alpha: turn details into a white-on-black square for Substance  (M)";
    case Item::NoBg: return L"Remove background: save as a transparent PNG and copy it  (B)";
    case Item::Upload: {
        static std::wstring tip;
        tip = L"Copy, save, upload to " + Upload_ServiceName() + L", copy the link and close  (U)";
        return tip.c_str();
    }
    case Item::Done: return L"Copy, save and close  (Enter)";
    }
    return nullptr;
}

void RoundPath(G::GraphicsPath& p, const G::RectF& r, float rad) {
    rad = std::min({rad, r.Width / 2, r.Height / 2});
    const float d = std::max(0.01f, rad * 2);
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.GetRight() - d, r.Y, d, d, 270, 90);
    p.AddArc(r.GetRight() - d, r.GetBottom() - d, d, d, 0, 90);
    p.AddArc(r.X, r.GetBottom() - d, d, d, 90, 90);
    p.CloseFigure();
}

G::RectF RectF(const RECT& r) {
    return G::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                    static_cast<float>(r.bottom - r.top));
}

// The crop box in whole image pixels, inside the picture.
RECT CropPixels(const Editor& e) {
    RECT r{static_cast<LONG>(std::lround(e.crop.X)), static_cast<LONG>(std::lround(e.crop.Y)),
           static_cast<LONG>(std::lround(e.crop.GetRight())), static_cast<LONG>(std::lround(e.crop.GetBottom()))};
    RECT all{0, 0, e.ImgW(), e.ImgH()};
    if (!IntersectRect(&r, &r, &all)) r = {};
    return r;
}

// Crop: outside the box dimmed, thirds, handles and the size in pixels.
void PaintCrop(Editor& e, HDC b) {
    G::Graphics g(b);
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);
    g.SetTextRenderingHint(G::TextRenderingHintAntiAlias);
    const G::RectF area = RectF(e.imageArea);
    g.SetClip(area);
    const RECT px = CropPixels(e);
    const G::RectF s(e.origin.x + px.left * e.zoom, e.origin.y + px.top * e.zoom, (px.right - px.left) * e.zoom,
                     (px.bottom - px.top) * e.zoom);
    G::Region dim(area);
    dim.Exclude(s);
    G::SolidBrush shade(G::Color(150, 0, 0, 0));
    g.FillRegion(&shade, &dim);
    G::Pen third(G::Color(80, 255, 255, 255), 1);
    for (int i = 1; i <= 2; ++i) {
        g.DrawLine(&third, s.X + s.Width * i / 3, s.Y, s.X + s.Width * i / 3, s.GetBottom());
        g.DrawLine(&third, s.X, s.Y + s.Height * i / 3, s.GetRight(), s.Y + s.Height * i / 3);
    }
    G::Pen dark(G::Color(160, 0, 0, 0), 3), light(G::Color(255, 255, 255, 255), 1);
    g.DrawRectangle(&dark, s);
    g.DrawRectangle(&light, s);
    if (e.cropBox && e.cropDrag != 1) {
        const float hs = static_cast<float>(e.Px(4));
        G::SolidBrush fill(G::Color(255, 255, 255, 255));
        G::Pen ring(GC(kAccent), 1.5f);
        for (int id = 0; id < 8; ++id) {
            const G::PointF h = BoxPoint(s, id);
            g.FillRectangle(&fill, h.X - hs, h.Y - hs, 2 * hs, 2 * hs);
            g.DrawRectangle(&ring, h.X - hs, h.Y - hs, 2 * hs, 2 * hs);
        }
    }
    wchar_t t[48];
    swprintf_s(t, L"%ld × %ld", px.right - px.left, px.bottom - px.top);
    G::Font font(b, e.ui);
    G::RectF tb;
    g.MeasureString(t, -1, &font, G::PointF(0, 0), &tb);
    const float pw = tb.Width + e.Px(12), ph = tb.Height + e.Px(4);
    float lx = s.X, ly = s.Y - ph - e.Px(6);
    if (ly < area.Y + e.Px(4)) ly = s.Y + e.Px(8), lx = s.X + e.Px(8);
    G::GraphicsPath pill;
    RoundPath(pill, G::RectF(lx, ly, pw, ph), ph / 2);
    G::SolidBrush pb(G::Color(200, 20, 20, 24)), tw(G::Color(255, 255, 255, 255));
    g.FillPath(&pb, &pill);
    g.DrawString(t, -1, &font, G::PointF(lx + e.Px(6), ly + e.Px(2)), &tw);
}

// ---- frame panel ---------------------------------------------------------------------------

struct SliderDef {
    int lo, hi, step;
    const wchar_t* name;
    const wchar_t* unit;
};
SliderDef SliderFor(int row) {
    if (row == kRowPadding) return {0, 25, 1, L"Padding", L" %"};
    if (row == kRowCorners) return {0, 20, 1, L"Corner roundness", L""};
    return {0, 100, 5, L"Shadow softness", L""};
}
int* SliderValue(Editor& e, int row) {
    if (row == kRowPadding) return &e.fr.padding;
    if (row == kRowCorners) return &e.fr.corners;
    if (row == kRowSoftness) return &e.fr.softness;
    return nullptr;
}
bool IsSlider(int row) { return row == kRowPadding || row == kRowCorners || row == kRowSoftness; }

// Slider track ends (x) and its line (y).
void Track(const Editor& e, int row, int& x0, int& x1, int& y) {
    const RECT& r = e.prow[row];
    x0 = r.left + e.Px(8);
    x1 = r.right - e.Px(8);
    y = r.top + e.Px(36);
}
RECT SwitchRectFor(const Editor& e, int row) {
    const RECT& r = e.prow[row];
    const int w = e.Px(38), h = e.Px(20), cy = (r.top + r.bottom) / 2;
    return {r.right - w, cy - h / 2, r.right, cy - h / 2 + h};
}
RECT SwatchRect(const Editor& e, int i) {
    const RECT& r = e.prow[kRowBg];
    const int sz = e.Px(28), gap = (r.right - r.left - kFrameBgCount * sz) / (kFrameBgCount - 1);
    const int x = r.left + i * (sz + gap), y = r.top + e.Px(26);
    return {x, y, x + sz, y + sz};
}
RECT SegmentRect(const Editor& e, int i) {
    const RECT& r = e.prow[kRowShape];
    const int w = r.right - r.left, y = r.top + e.Px(26);
    return {r.left + w * i / kFrameShapeCount, y, r.left + w * (i + 1) / kFrameShapeCount, y + e.Px(28)};
}

void PanelText(Editor& e, HDC dc, const wchar_t* t, RECT r, COLORREF c, UINT fmt, HFONT f = nullptr) {
    HGDIOBJ old = SelectObject(dc, f ? f : e.ui);
    SetTextColor(dc, c);
    DrawTextW(dc, t, -1, &r, fmt | DT_NOPREFIX);
    SelectObject(dc, old);
}

void PaintSwitch(Editor& e, G::Graphics& g, const RECT& pr, bool on) {
    const Palette& p = Pal();
    const G::RectF r = RectF(pr);
    G::GraphicsPath path;
    RoundPath(path, r, r.Height / 2);
    G::SolidBrush fill(GC(on ? p.accent : p.nav));
    g.FillPath(&fill, &path);
    if (!on) {
        G::Pen line(GC(p.switchLine), std::max(1.f, e.Px(10) / 10.f));
        g.DrawPath(&line, &path);
    }
    const float k = on ? r.Height - e.Px(8) : r.Height - e.Px(9), pad = (r.Height - k) / 2;
    const float kx = on ? r.GetRight() - pad - k : r.X + pad;
    G::SolidBrush knob(GC(on ? p.onAccent : p.switchKnob));
    g.FillEllipse(&knob, kx, r.Y + pad, k, k);
}

void PaintPanel(Editor& e, HDC b) {
    const Palette& p = Pal();
    Fill(b, e.panelRect, p.bar);
    Fill(b, {e.panelRect.left, e.panelRect.top, e.panelRect.left + 1, e.panelRect.bottom}, p.barLine);
    SetBkMode(b, TRANSPARENT);
    G::Graphics g(b);
    g.SetSmoothingMode(G::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(G::PixelOffsetModeHalf);
    const bool on = e.fr.on;
    const COLORREF text = p.text, dim = p.grey;
    for (int row = 0; row < kRowCount; ++row) {
        const RECT& r = e.prow[row];
        const bool active = row == kRowOn || (on && (row != kRowSoftness || e.fr.shadow));
        const COLORREF tc = active ? text : dim;
        RECT label{r.left, r.top, r.right, r.top + e.Px(22)};
        if (row == kRowOn || row == kRowShadow) {
            RECT lr{r.left, r.top, r.right - e.Px(44), r.bottom};
            PanelText(e, b, row == kRowOn ? L"Store-page frame" : L"Shadow", lr, tc, DT_LEFT | DT_VCENTER | DT_SINGLELINE,
                      row == kRowOn ? e.bold : nullptr);
            PaintSwitch(e, g, SwitchRectFor(e, row), row == kRowOn ? on : e.fr.shadow);
        } else if (IsSlider(row)) {
            const SliderDef d = SliderFor(row);
            const int v = *SliderValue(e, row);
            PanelText(e, b, d.name, label, tc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            const std::wstring val = std::to_wstring(v) + d.unit;
            PanelText(e, b, val.c_str(), label, dim, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
            int x0, x1, y;
            Track(e, row, x0, x1, y);
            const float t = static_cast<float>(v - d.lo) / (d.hi - d.lo), kx = x0 + t * (x1 - x0);
            const float th = static_cast<float>(e.Px(4));
            G::GraphicsPath full, part;
            RoundPath(full, G::RectF(static_cast<float>(x0), y - th / 2, static_cast<float>(x1 - x0), th), th / 2);
            G::SolidBrush tr(GC(p.switchLine));
            g.FillPath(&tr, &full);
            if (kx > x0 + 1) {
                RoundPath(part, G::RectF(static_cast<float>(x0), y - th / 2, kx - x0, th), th / 2);
                G::SolidBrush ac(GC(active ? p.accent : p.switchLine));
                g.FillPath(&ac, &part);
            }
            const float kr = static_cast<float>(e.Px(8));
            G::SolidBrush knob(GC(active ? p.accent : p.switchLine));
            G::Pen ring(GC(p.bar), static_cast<float>(e.Px(2)));
            g.FillEllipse(&knob, kx - kr, y - kr, 2 * kr, 2 * kr);
            g.DrawEllipse(&ring, kx - kr + 1, y - kr + 1, 2 * kr - 2, 2 * kr - 2);
        } else if (row == kRowBg) {
            const std::wstring lab = std::wstring(L"Background: ") + kFrameBgNames[static_cast<int>(e.fr.bg)];
            PanelText(e, b, lab.c_str(), label, tc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            for (int i = 0; i < kFrameBgCount; ++i) {
                const RECT sr = SwatchRect(e, i);
                const G::RectF f = RectF(sr);
                G::GraphicsPath sp;
                RoundPath(sp, f, static_cast<float>(e.Px(6)));
                const FrameBg kind = static_cast<FrameBg>(i);
                g.SetClip(&sp);
                if (kind == FrameBg::Gradient) {
                    G::LinearGradientBrush lg(f, G::Color(255, 244, 236, 255), G::Color(255, 205, 230, 250), 45.f);
                    g.FillRectangle(&lg, f);
                } else if (kind == FrameBg::Clear) {
                    const float q = f.Width / 4;
                    for (int yy = 0; yy < 4; ++yy)
                        for (int xx = 0; xx < 4; ++xx) {
                            G::SolidBrush cb(((xx + yy) & 1) ? G::Color(255, 205, 205, 205) : G::Color(255, 255, 255, 255));
                            g.FillRectangle(&cb, f.X + xx * q, f.Y + yy * q, q, q);
                        }
                } else {
                    FrameOpts tmp = e.fr;
                    tmp.bg = kind;
                    G::SolidBrush sb(GC(FrameBgColor(tmp)));
                    g.FillRectangle(&sb, f);
                    if (kind == FrameBg::Custom) {  // a "+": click picks the color
                        G::Pen plus(IsLight(e.fr.custom) ? G::Color(200, 0, 0, 0) : G::Color(230, 255, 255, 255),
                                    static_cast<float>(e.Px(2)) * 0.8f);
                        const float cx = f.X + f.Width / 2, cy = f.Y + f.Height / 2, a = f.Width * 0.22f;
                        g.DrawLine(&plus, cx - a, cy, cx + a, cy);
                        g.DrawLine(&plus, cx, cy - a, cx, cy + a);
                    }
                }
                g.ResetClip();
                const bool sel = static_cast<int>(e.fr.bg) == i;
                G::Pen ring(GC(sel ? p.accent : p.ring), sel ? static_cast<float>(e.Px(2)) : 1.f);
                G::GraphicsPath rp;
                if (sel) {
                    G::RectF o = f;
                    o.Inflate(static_cast<float>(e.Px(2)), static_cast<float>(e.Px(2)));
                    RoundPath(rp, o, static_cast<float>(e.Px(8)));
                } else {
                    RoundPath(rp, f, static_cast<float>(e.Px(6)));
                }
                g.DrawPath(&ring, &rp);
            }
        } else if (row == kRowShape) {
            PanelText(e, b, L"Shape", label, tc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            RECT all = SegmentRect(e, 0);
            all.right = SegmentRect(e, kFrameShapeCount - 1).right;
            G::GraphicsPath bgp;
            RoundPath(bgp, RectF(all), static_cast<float>(e.Px(6)));
            G::SolidBrush nb(GC(p.nav));
            g.FillPath(&nb, &bgp);
            G::Pen nl(GC(p.switchLine), 1);
            g.DrawPath(&nl, &bgp);
            for (int i = 0; i < kFrameShapeCount; ++i) {
                RECT sr = SegmentRect(e, i);
                const bool sel = e.fr.shape == i;
                if (sel) {
                    G::RectF f = RectF(sr);
                    f.Inflate(-static_cast<float>(e.Px(2)), -static_cast<float>(e.Px(2)));
                    G::GraphicsPath sp;
                    RoundPath(sp, f, static_cast<float>(e.Px(5)));
                    G::SolidBrush ab(GC(active ? p.accent : p.switchLine));
                    g.FillPath(&ab, &sp);
                }
                PanelText(e, b, kFrameShapeNames[i], sr, sel ? p.onAccent : tc, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            }
        }
        if (row == e.pfocus) {  // keyboard focus
            G::RectF f = RectF(r);
            f.Inflate(static_cast<float>(e.Px(7)), static_cast<float>(e.Px(3)));
            G::GraphicsPath fp;
            RoundPath(fp, f, static_cast<float>(e.Px(6)));
            G::Pen fpen(GC(p.accent), 1.5f);
            g.DrawPath(&fpen, &fp);
        }
    }
    RECT hint{e.prow[0].left, e.prow[kRowCount - 1].bottom + e.Px(14), e.prow[0].right, e.panelRect.bottom - e.Px(8)};
    PanelText(e, b,
              L"Tab or Up/Down moves between settings, Left/Right changes them, Space switches. F closes this panel.\n"
              L"Copy, Save, Done and Upload give the framed picture.",
              hint, p.grey, DT_LEFT | DT_WORDBREAK);
}

void Paint(Editor& e, HDC hdc) {
    HDC b = e.back.dc;
    RECT rc{0, 0, e.back.w, e.back.h};
    Fill(b, rc, Pal().canvas);

    // image (in its frame when that's on) + the shape being drawn
    const Dib& src = Disp(e);
    const POINT co = CanvasOrigin(e);
    const int vw = static_cast<int>(std::lround(src.w * e.zoom)), vh = static_cast<int>(std::lround(src.h * e.zoom));
    HRGN areaClip = CreateRectRgnIndirect(&e.imageArea);
    SelectClipRgn(b, areaClip);
    if (e.view.dc) {
        BitBlt(b, co.x, co.y, vw, vh, e.view.dc, 0, 0, SRCCOPY);
    } else {  // 100 % and up: only the visible part, hard pixels
        const RECT img{co.x, co.y, co.x + vw, co.y + vh};
        RECT vis;
        if (IntersectRect(&vis, &img, &e.imageArea)) {
            const int x0 = std::max(0, static_cast<int>(std::floor((vis.left - co.x) / e.zoom)));
            const int y0 = std::max(0, static_cast<int>(std::floor((vis.top - co.y) / e.zoom)));
            const int x1 = std::min(src.w, static_cast<int>(std::ceil((vis.right - co.x) / e.zoom)));
            const int y1 = std::min(src.h, static_cast<int>(std::ceil((vis.bottom - co.y) / e.zoom)));
            const int dx0 = co.x + static_cast<int>(std::lround(x0 * e.zoom));
            const int dy0 = co.y + static_cast<int>(std::lround(y0 * e.zoom));
            const int dx1 = co.x + static_cast<int>(std::lround(x1 * e.zoom));
            const int dy1 = co.y + static_cast<int>(std::lround(y1 * e.zoom));
            SetStretchBltMode(b, COLORONCOLOR);
            StretchBlt(b, dx0, dy0, dx1 - dx0, dy1 - dy0, src.dc, x0, y0, x1 - x0, y1 - y0, SRCCOPY);
        }
    }
    SelectClipRgn(b, nullptr);
    DeleteObject(areaClip);
    {
        G::Graphics g(b);
        g.SetSmoothingMode(G::SmoothingModeAntiAlias);
        g.SetTextRenderingHint(G::TextRenderingHintAntiAlias);
        g.SetClip(G::Rect(e.imageArea.left, e.imageArea.top, e.imageArea.right - e.imageArea.left,
                          e.imageArea.bottom - e.imageArea.top));
        g.TranslateTransform(static_cast<float>(e.origin.x), static_cast<float>(e.origin.y));
        g.ScaleTransform(e.zoom, e.zoom);
        if (e.drawing || e.typing) DrawShape(g, e.live, e.sc, true);
        if (e.dragging) DrawShape(g, e.shapes[e.sel], e.sc, true);
        if (e.sel >= 0 && !e.typing) {  // selection: dashed frame and handles, same size at any zoom
            const Shape& s = e.shapes[e.sel];
            const float px = 1 / e.zoom;
            if (!IsLine(s.tool)) {
                G::RectF r = Bounds(s, e.sc);
                const float pad = IsBox(s.tool) ? kLineW[s.size] * e.sc / 2 + 3 * px : 0;
                r.Inflate(pad, pad);
                G::Pen dark(G::Color(160, 0, 0, 0), 3 * px), light(G::Color(255, 255, 255, 255), px);
                light.SetDashStyle(G::DashStyleDash);
                g.DrawRectangle(&dark, r);
                g.DrawRectangle(&light, r);
            }
            const float hs = e.Px(4) * px;
            G::SolidBrush fill(G::Color(255, 255, 255, 255));
            G::Pen ring(GC(kAccent), 1.5f * px);
            for (const auto& [id, h] : Handles(s, e.sc)) {
                g.FillRectangle(&fill, h.X - hs, h.Y - hs, 2 * hs, 2 * hs);
                g.DrawRectangle(&ring, h.X - hs, h.Y - hs, 2 * hs, 2 * hs);
            }
        }
        if (e.typing) {  // caret and a light frame around the text
            const float em = kFontPx[e.live.size] * e.sc * e.live.scale, ls = LineSpacing(em);
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
    if (e.tool == Tool::Crop && (e.cropBox || e.cropDrag == 1)) PaintCrop(e, b);

    // toolbar
    Fill(b, {0, 0, rc.right, e.Px(kToolbarH)}, Pal().bar);
    Fill(b, {0, e.Px(kToolbarH) - 1, rc.right, e.Px(kToolbarH)}, Pal().barLine);
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
                                  (it.kind == Item::Size && e.size == it.index) ||
                                  (it.kind == Item::Frame && (e.fr.on || e.panel));
            RECT inner = r;
            InflateRect(&inner, -e.Px(2), 0);
            if (it.kind == Item::Done) {
                RoundFill(g, inner, i == e.hover ? GC(RGB(245, 70, 80)) : GC(kAccent), static_cast<float>(e.Px(4)));
            } else if (selected) {
                RoundFill(g, inner, GC(Pal().toolSel), static_cast<float>(e.Px(4)));
            } else if (i == e.hover) {
                RoundFill(g, inner, GC(Pal().toolHover), static_cast<float>(e.Px(4)));
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
                G::Pen ring(GC(Pal().ring), 1);
                g.DrawEllipse(&ring, cx - d / 2, cy - d / 2, d, d);
                break;
            }
            case Item::Size: {
                const float d = static_cast<float>(e.Px(it.index == 0 ? 4 : it.index == 1 ? 8 : 13));
                G::SolidBrush br(GC(Pal().icon));
                g.FillEllipse(&br, cx - d / 2, cy - d / 2, d, d);
                break;
            }
            case Item::Undo:
                Glyph(e, b, L"\xE7A7", r, !e.undo.empty());
                break;
            case Item::Redo:
                Glyph(e, b, L"\xE7A6", r, !e.redo.empty());
                break;
            case Item::Frame:
                FrameIcon(e, g, r);
                break;
            case Item::Copy:
                Glyph(e, b, L"\xE8C8", r, true);
                break;
            case Item::Save:
                Glyph(e, b, L"\xE74E", r, true);
                break;
            case Item::NoBg:
                Glyph(e, b, L"\xE8C6", r, true);  // scissors: cut the subject out
                break;
            case Item::Upload:
                Glyph(e, b, L"\xE898", r, true);
                break;
            case Item::Alpha: {
                HGDIOBJ old = SelectObject(b, e.bold);
                SetTextColor(b, Pal().icon);
                RECT rr = r;
                DrawTextW(b, L"\x03B1", 1, &rr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
                SelectObject(b, old);
                break;
            }
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

    if (e.panel) PaintPanel(e, b);

    // status bar
    RECT sb{0, rc.bottom - e.Px(kStatusH), rc.right, rc.bottom};
    Fill(b, sb, Pal().bar);
    Fill(b, {0, sb.top, rc.right, sb.top + 1}, Pal().barLine);
    HGDIOBJ old = SelectObject(b, e.ui);
    wchar_t right[96];
    if (e.fr.on)
        swprintf_s(right, L"%d × %d  framed %d × %d   %d%%", e.ImgW(), e.ImgH(), CanvasW(e), CanvasH(e),
                   static_cast<int>(std::lround(e.zoom * 100)));
    else swprintf_s(right, L"%d × %d   %d%%", e.ImgW(), e.ImgH(), static_cast<int>(std::lround(e.zoom * 100)));
    RECT rr = sb;
    rr.right -= e.Px(10);
    SetTextColor(b, Pal().status);
    DrawTextW(b, right, -1, &rr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    std::wstring left;
    if (const wchar_t* t = Tip(e, e.hover)) left = t;
    else if (!e.status.empty()) left = e.status;
    else if (e.typing) left = L"Type your text. Enter finishes, Shift+Enter starts a new line, Esc cancels.";
    else if (e.panel && e.pfocus >= 0)
        left = L"Frame: Tab or Up/Down = next setting, Left/Right = change it (Shift = faster), Space = switch or pick a "
               L"color.  Esc leaves the panel, F closes it.";
    else if (e.tool == Tool::Crop)
        left = e.cropBox ? L"Drag the box or its handles (Shift keeps the shape).  Arrows move it, Ctrl+arrows resize it."
                           L"  Enter or double-click crops, Esc cancels."
                         : L"Drag a box over the part to keep (Shift keeps the picture's shape), or press an arrow key "
                           L"for a box you move with the keyboard.  Esc = back to the last tool.";
    else if (e.sel >= 0)
        left = L"Selected: drag to move, drag a handle to resize, pick a color or size to change it, Delete removes it."
               L"  Double-click text to edit it.  Esc = unselect.";
    else if (e.tool == Tool::Select)
        left = L"Click a shape to select it.  Drag empty space to move around.  Ctrl+wheel = zoom.";
    else left = L"Drag to draw.  V = select shapes, C = crop, F = frame.  Space+drag = move around.  Ctrl+wheel = zoom (Ctrl+0 fit).  Enter = copy, save, close.";
    RECT lr = sb;
    lr.left += e.Px(10);
    lr.right -= e.Px(150);
    SetTextColor(b, e.status.empty() || e.hover >= 0 ? Pal().hint : Pal().statusOk);
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
    if (e.editText >= 0) {  // retyped an existing text: replace it (or remove it when emptied)
        const int i = std::exchange(e.editText, -1);
        const Shape& old = e.shapes[i];
        if (e.live.text != old.text || e.live.color != old.color || e.live.size != old.size) {
            PushUndo(e, e.shapes);
            if (e.live.text.empty()) {
                e.shapes.erase(e.shapes.begin() + i);
                e.sel = -1;
            } else {
                e.shapes[i] = e.live;
                e.sel = i;
            }
        } else {
            e.sel = i;
        }
        Rebuild(e);
    } else if (!e.live.text.empty()) {
        Commit(e, e.live);
    }
    Redraw(e);
}

void CancelText(Editor& e) {
    if (!e.typing) return;
    e.typing = false;
    if (e.editText >= 0) {
        e.sel = std::exchange(e.editText, -1);
        Rebuild(e);
    }
    Redraw(e);
}

void EditText(Editor& e, int i) {
    FinishText(e);
    EndDrag(e, false);
    if (i < 0 || i >= static_cast<int>(e.shapes.size()) || e.shapes[i].tool != Tool::Text) return;
    e.editText = i;
    e.live = e.shapes[i];
    e.typing = true;
    e.sel = -1;
    Rebuild(e, i);
    Redraw(e);
}

// A new picture under the shapes (crop, or undoing one): new buffers, fit to the window again.
void SetImage(Editor& e, std::shared_ptr<const Image> img) {
    e.img = std::move(img);
    e.base.Create(e.img->w, e.img->h);
    e.committed.Create(e.img->w, e.img->h);
    memcpy(e.base.bits, e.img->px.data(), e.img->px.size() * 4);
    e.view.Free();
    e.fit = true;
}


// Back to an undo / redo step (the picture too, when a crop changed it).
void Restore(Editor& e, Snapshot s) {
    e.cropBox = false;
    e.cropDrag = 0;
    e.shapes = std::move(s.shapes);
    const bool newImage = s.img != e.img;
    if (newImage) SetImage(e, std::move(s.img));
    e.sel = -1;
    e.dirty = true;
    Rebuild(e);
    if (newImage) Layout(e);
    Redraw(e);
}

void Undo(Editor& e) {
    if (e.typing) {
        CancelText(e);
        return;
    }
    EndDrag(e, true);
    if (e.undo.empty()) return;
    e.redo.push_back({std::move(e.shapes), e.img});
    Snapshot s = std::move(e.undo.back());
    e.undo.pop_back();
    Restore(e, std::move(s));
}

void Redo(Editor& e) {
    if (e.typing || e.redo.empty()) return;
    EndDrag(e, true);
    e.undo.push_back({std::move(e.shapes), e.img});
    Snapshot s = std::move(e.redo.back());
    e.redo.pop_back();
    Restore(e, std::move(s));
}

// ---- crop ------------------------------------------------------------------------------------

void SetTool(Editor& e, Tool t) {
    FinishText(e);
    if (t == Tool::Crop && e.tool != Tool::Crop) {
        e.prevTool = e.tool;
        e.sel = -1;
    }
    if (t != Tool::Crop) {
        e.cropBox = false;
        e.cropDrag = 0;
    }
    e.tool = t;
}

// Cuts the picture to the crop box; shapes move with it. One undo step.
void ApplyCrop(Editor& e) {
    const RECT r = CropPixels(e);
    const int w = r.right - r.left, h = r.bottom - r.top;
    e.cropBox = false;
    e.cropDrag = 0;
    if (w < 1 || h < 1 || (w == e.ImgW() && h == e.ImgH())) {
        SetTool(e, e.prevTool);
        Redraw(e);
        return;
    }
    PushUndo(e, e.shapes);
    auto img = std::make_shared<Image>();
    img->w = w;
    img->h = h;
    img->px.resize(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y)
        memcpy(&img->px[static_cast<size_t>(y) * w], &e.img->px[static_cast<size_t>(y + r.top) * e.img->w + r.left],
               static_cast<size_t>(w) * 4);
    const float dx = static_cast<float>(r.left), dy = static_cast<float>(r.top);
    for (Shape& s : e.shapes) {
        s.a = G::PointF(s.a.X - dx, s.a.Y - dy);
        s.b = G::PointF(s.b.X - dx, s.b.Y - dy);
    }
    SetImage(e, std::move(img));
    Rebuild(e);
    Layout(e);
    SetTool(e, e.prevTool);
    Log(L"Editor: cropped to %d x %d at %ld, %ld", w, h, r.left, r.top);
    ShowStatus(e, L"Cropped to " + std::to_wstring(w) + L" × " + std::to_wstring(h) + L"  (Ctrl+Z brings the rest back)");
}

int CropHandleAt(const Editor& e, G::PointF p) {
    if (!e.cropBox) return -1;
    const float r = e.Px(7) / e.zoom;
    for (int id = 0; id < 8; ++id) {
        const G::PointF h = BoxPoint(e.crop, id);
        if (std::fabs(p.X - h.X) <= r && std::fabs(p.Y - h.Y) <= r) return id;
    }
    return -1;
}

bool InCrop(const Editor& e, G::PointF p) { return e.cropBox && e.crop.Contains(p); }

void CropDown(Editor& e, G::PointF p) {
    const float W = static_cast<float>(e.ImgW()), H = static_cast<float>(e.ImgH());
    e.cropStart = e.crop;
    e.cropFrom = G::PointF(std::clamp(p.X, 0.f, W), std::clamp(p.Y, 0.f, H));
    if (const int h = CropHandleAt(e, p); h >= 0) {
        e.cropDrag = 3;
        e.cropHandle = h;
    } else if (InCrop(e, p)) {
        e.cropDrag = 2;
    } else {
        e.cropDrag = 1;
        e.crop = G::RectF(e.cropFrom.X, e.cropFrom.Y, 0, 0);
    }
    SetCapture(e.hwnd);
}

// Moves `to` toward `from` as little as needed for the box from `from` to `to` to keep ratio q and stay in the picture.
G::PointF KeepRatio(const Editor& e, G::PointF from, G::PointF to, float q) {
    const float W = static_cast<float>(e.ImgW()), H = static_cast<float>(e.ImgH());
    float dx = to.X - from.X, dy = to.Y - from.Y;
    if (std::fabs(dx) / q > std::fabs(dy)) dy = std::copysign(std::fabs(dx) / q, dy == 0 ? 1.f : dy);
    else dx = std::copysign(std::fabs(dy) * q, dx == 0 ? 1.f : dx);
    float t = 1;
    if (dx > 0) t = std::min(t, (W - from.X) / dx);
    if (dx < 0) t = std::min(t, -from.X / dx);
    if (dy > 0) t = std::min(t, (H - from.Y) / dy);
    if (dy < 0) t = std::min(t, -from.Y / dy);
    t = std::max(0.f, t);
    return G::PointF(from.X + dx * t, from.Y + dy * t);
}

void CropMove(Editor& e, G::PointF p, bool shift) {
    const float W = static_cast<float>(e.ImgW()), H = static_cast<float>(e.ImgH());
    p = G::PointF(std::clamp(p.X, 0.f, W), std::clamp(p.Y, 0.f, H));
    const G::RectF& o = e.cropStart;
    if (e.cropDrag == 1) {
        const G::PointF b = shift ? KeepRatio(e, e.cropFrom, p, W / H) : p;
        e.crop = Norm(e.cropFrom, b);
    } else if (e.cropDrag == 2) {
        e.crop = o;
        e.crop.X = std::clamp(o.X + p.X - e.cropFrom.X, 0.f, W - o.Width);
        e.crop.Y = std::clamp(o.Y + p.Y - e.cropFrom.Y, 0.f, H - o.Height);
    } else if (e.cropDrag == 3) {
        const int h = e.cropHandle;
        if (shift && h % 2 == 0 && o.Width > 0 && o.Height > 0) {  // corner, keeping the box's shape
            const G::PointF fixed = BoxPoint(o, (h + 4) % 8);
            e.crop = Norm(fixed, KeepRatio(e, fixed, p, o.Width / o.Height));
        } else {
            float l = o.X, t = o.Y, rt = o.GetRight(), b = o.GetBottom();
            if (h == 0 || h == 6 || h == 7) l = p.X;
            if (h == 2 || h == 3 || h == 4) rt = p.X;
            if (h == 0 || h == 1 || h == 2) t = p.Y;
            if (h == 4 || h == 5 || h == 6) b = p.Y;
            e.crop = Norm(G::PointF(l, t), G::PointF(rt, b));
        }
    }
}

void CropUp(Editor& e) {
    if (!e.cropDrag) return;
    e.cropDrag = 0;
    if (GetCapture() == e.hwnd) ReleaseCapture();
    const RECT r = CropPixels(e);
    e.cropBox = r.right - r.left >= 2 && r.bottom - r.top >= 2;
}

// Keyboard: arrows move the box, Ctrl+arrows change its size (a box of the whole picture to start with).
void CropKey(Editor& e, UINT vk, bool ctrl, bool shift) {
    const float W = static_cast<float>(e.ImgW()), H = static_cast<float>(e.ImgH());
    if (!e.cropBox) {
        e.crop = G::RectF(0, 0, W, H);
        e.cropBox = true;
    }
    const float step = shift ? 10.f : 1.f;
    const float dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0;
    const float dy = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
    G::RectF& c = e.crop;
    if (ctrl) {
        c.Width = std::clamp(c.Width + dx, 2.f, W - c.X);
        c.Height = std::clamp(c.Height + dy, 2.f, H - c.Y);
    } else {
        c.X = std::clamp(c.X + dx, 0.f, W - c.Width);
        c.Y = std::clamp(c.Y + dy, 0.f, H - c.Height);
    }
}

// ---- frame -----------------------------------------------------------------------------------

void LoadFrame(FrameOpts& f) {
    const std::wstring ini = SettingsPath();
    auto num = [&](const wchar_t* k, int def, int lo, int hi) {
        return std::clamp(static_cast<int>(GetPrivateProfileIntW(L"Editor", k, def, ini.c_str())), lo, hi);
    };
    auto str = [&](const wchar_t* k, const wchar_t* def) {
        wchar_t buf[64];
        GetPrivateProfileStringW(L"Editor", k, def, buf, 64, ini.c_str());
        return std::wstring(buf);
    };
    f.on = num(L"Frame", 0, 0, 1) != 0;
    f.padding = num(L"FramePadding", f.padding, 0, 25);
    f.corners = num(L"FrameCorners", f.corners, 0, 20);
    f.shadow = num(L"FrameShadow", 1, 0, 1) != 0;
    f.softness = num(L"FrameSoftness", f.softness, 0, 100);
    const std::wstring bg = str(L"FrameBackground", L"gradient");
    for (int i = 0; i < kFrameBgCount; ++i)
        if (!_wcsicmp(bg.c_str(), kFrameBgKeys[i])) f.bg = static_cast<FrameBg>(i);
    const std::wstring col = str(L"FrameColor", L"2e3440");
    if (col.size() == 6) {
        const unsigned long v = wcstoul(col.c_str(), nullptr, 16);
        f.custom = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
    }
    const std::wstring shape = str(L"FrameShape", L"original");
    for (int i = 0; i < kFrameShapeCount; ++i)
        if (!_wcsicmp(shape.c_str(), kFrameShapeKeys[i])) f.shape = i;
}

void SaveFrame(const FrameOpts& f) {
    SaveSettingValue(L"Editor", L"Frame", f.on ? L"1" : L"0");
    SaveSettingValue(L"Editor", L"FramePadding", std::to_wstring(f.padding));
    SaveSettingValue(L"Editor", L"FrameCorners", std::to_wstring(f.corners));
    SaveSettingValue(L"Editor", L"FrameShadow", f.shadow ? L"1" : L"0");
    SaveSettingValue(L"Editor", L"FrameSoftness", std::to_wstring(f.softness));
    SaveSettingValue(L"Editor", L"FrameBackground", kFrameBgKeys[static_cast<int>(f.bg)]);
    wchar_t hex[8];
    swprintf_s(hex, L"%02x%02x%02x", GetRValue(f.custom), GetGValue(f.custom), GetBValue(f.custom));
    SaveSettingValue(L"Editor", L"FrameColor", hex);
    SaveSettingValue(L"Editor", L"FrameShape", kFrameShapeKeys[f.shape]);
}

// The frame options changed: new framed picture and fit. save = remember them now (not while dragging).
void FrameChanged(Editor& e, bool save) {
    Refresh(e);
    Layout(e);
    if (save) SaveFrame(e.fr);
    Redraw(e);
}

void ShowPanel(Editor& e, bool show) {
    if (show == e.panel) return;
    e.panel = show;
    e.pdrag = -1;
    e.pfocus = show ? kRowOn : -1;
    if (show && !e.fr.on) {  // opening it means "frame this"
        e.fr.on = true;
        FrameChanged(e, true);
        return;
    }
    Layout(e);
    Redraw(e);
}

void PickCustomColor(Editor& e) {
    static COLORREF custom[16] = {};
    CHOOSECOLORW cc{sizeof(cc)};
    cc.hwndOwner = e.hwnd;
    cc.rgbResult = e.fr.custom;
    cc.lpCustColors = custom;
    cc.Flags = CC_FULLOPEN | CC_RGBINIT;
    if (ChooseColorW(&cc)) e.fr.custom = cc.rgbResult;
    e.fr.bg = FrameBg::Custom;
    FrameChanged(e, true);
}

// One step of a panel control from the keyboard: dir -1 / +1 (Left / Right), 0 = Space.
void PanelStep(Editor& e, int row, int dir, bool big) {
    if (row == kRowOn) {
        e.fr.on = dir == 0 ? !e.fr.on : dir > 0;
    } else if (row == kRowShadow) {
        e.fr.shadow = dir == 0 ? !e.fr.shadow : dir > 0;
    } else if (IsSlider(row)) {
        if (dir == 0) return;
        const SliderDef d = SliderFor(row);
        int& v = *SliderValue(e, row);
        v = std::clamp(v + dir * d.step * (big ? 5 : 1), d.lo, d.hi);
    } else if (row == kRowBg) {
        if (dir == 0) {
            PickCustomColor(e);
            return;
        }
        e.fr.bg = static_cast<FrameBg>((static_cast<int>(e.fr.bg) + dir + kFrameBgCount) % kFrameBgCount);
    } else if (row == kRowShape) {
        if (dir == 0) return;
        e.fr.shape = (e.fr.shape + dir + kFrameShapeCount) % kFrameShapeCount;
    }
    if (row != kRowOn && !e.fr.on) e.fr.on = true;  // changing a setting shows it
    FrameChanged(e, true);
}

void SliderTo(Editor& e, int row, int x) {
    int x0, x1, y;
    Track(e, row, x0, x1, y);
    const SliderDef d = SliderFor(row);
    const float t = std::clamp(static_cast<float>(x - x0) / std::max(1, x1 - x0), 0.f, 1.f);
    int v = d.lo + static_cast<int>(std::lround(t * (d.hi - d.lo) / d.step)) * d.step;
    v = std::clamp(v, d.lo, d.hi);
    int& cur = *SliderValue(e, row);
    if (v == cur && e.fr.on) return;
    cur = v;
    e.fr.on = true;
    FrameChanged(e, false);
}

// A click in the panel. Returns true when it was there.
bool PanelDown(Editor& e, int x, int y) {
    const POINT pt{x, y};
    if (!e.panel || !PtInRect(&e.panelRect, pt)) return false;
    for (int row = 0; row < kRowCount; ++row) {
        RECT r = e.prow[row];
        InflateRect(&r, e.Px(6), e.Px(3));
        if (!PtInRect(&r, pt)) continue;
        e.pfocus = row;
        if (row == kRowOn || row == kRowShadow) {
            PanelStep(e, row, 0, false);
        } else if (IsSlider(row)) {
            e.pdrag = row;
            SetCapture(e.hwnd);
            SliderTo(e, row, x);
        } else if (row == kRowBg) {
            for (int i = 0; i < kFrameBgCount; ++i) {
                const RECT sr = SwatchRect(e, i);
                if (!PtInRect(&sr, pt)) continue;
                if (static_cast<FrameBg>(i) == FrameBg::Custom) {
                    PickCustomColor(e);
                } else {
                    e.fr.bg = static_cast<FrameBg>(i);
                    e.fr.on = true;
                    FrameChanged(e, true);
                }
            }
        } else if (row == kRowShape) {
            for (int i = 0; i < kFrameShapeCount; ++i) {
                const RECT sr = SegmentRect(e, i);
                if (!PtInRect(&sr, pt)) continue;
                e.fr.shape = i;
                e.fr.on = true;
                FrameChanged(e, true);
            }
        }
        break;
    }
    Redraw(e);
    return true;
}

void PanelUp(Editor& e) {
    if (e.pdrag < 0) return;
    e.pdrag = -1;
    if (GetCapture() == e.hwnd) ReleaseCapture();
    SaveFrame(e.fr);
}

// Keys while a panel control has focus. Returns true when used.
bool PanelKey(Editor& e, UINT vk, bool shift) {
    if (!e.panel) return false;
    if (vk == VK_TAB) {
        if (e.pfocus < 0) e.pfocus = shift ? kRowCount - 1 : 0;
        else e.pfocus += shift ? -1 : 1;
        if (e.pfocus >= kRowCount) e.pfocus = -1;  // Tab past the end leaves the panel
        Redraw(e);
        return true;
    }
    if (e.pfocus < 0) return false;
    switch (vk) {
    case VK_UP:
    case VK_DOWN:
        e.pfocus = std::clamp(e.pfocus + (vk == VK_UP ? -1 : 1), 0, kRowCount - 1);
        break;
    case VK_LEFT:
    case VK_RIGHT:
        PanelStep(e, e.pfocus, vk == VK_LEFT ? -1 : 1, shift);
        break;
    case VK_SPACE:
        PanelStep(e, e.pfocus, 0, false);
        break;
    case VK_ESCAPE:
        e.pfocus = -1;
        break;
    default:
        return false;
    }
    Redraw(e);
    return true;
}

void SetColor(Editor& e, int index) {
    e.color = index;
    if (e.typing) e.live.color = kColors[index];
    else ChangeSelected(e, [&](Shape& s) { s.color = kColors[index]; });
}

void SetSize(Editor& e, int index) {
    e.size = std::clamp(index, 0, 2);
    if (e.typing) e.live.size = e.size;
    else ChangeSelected(e, [&](Shape& s) { s.size = e.size; });
}

void CopyNow(Editor& e) {
    FinishText(e);
    Output_CopyImage(OutputImage(e));
    e.dirty = false;
    ShowStatus(e, L"Copied to the clipboard");
}

void SaveNow(Editor& e) {
    FinishText(e);
    const std::wstring p = Output_SaveImage(OutputImage(e), *e.src, e.path);
    if (p.empty()) {
        ShowStatus(e, L"Could not save the file");
        return;
    }
    e.path = p;
    e.dirty = false;
    ShowStatus(e, L"Saved  " + p);
}

void Done(Editor& e, bool upload = false) {
    FinishText(e);
    auto r = std::make_shared<CaptureResult>(*e.src);
    r->img = OutputImage(e);
    r->autoUpload = r->autoUpload || upload;
    Output_EditDone(std::move(r), e.path);
    e.dirty = false;
    DestroyWindow(e.hwnd);
}

void MakeAlpha(Editor& e) {
    FinishText(e);
    Alpha_Open(CurrentImage(e), *e.src);
}

void RemoveBackground(Editor& e) {
    FinishText(e);
    ShowStatus(e, L"Removing the background…");
    Rmbg_Image(CurrentImage(e), *e.src, e.hwnd);  // answers with WM_APP_NOBG
}

void Close(Editor& e) {
    if (e.dirty && (!e.shapes.empty() || !e.undo.empty())) {
        const int a = MessageBoxW(e.hwnd, L"Close without saving or copying your edits?", L"Canivete Editor",
                                  MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2);
        if (a != IDYES) return;
    }
    DestroyWindow(e.hwnd);
}

void Click(Editor& e, const ItemRect& it) {
    switch (it.kind) {
    case Item::Tool:
        SetTool(e, kTools[it.index].tool);
        break;
    case Item::Color:
        SetColor(e, it.index);
        break;
    case Item::Size:
        SetSize(e, it.index);
        break;
    case Item::Undo: Undo(e); break;
    case Item::Redo: Redo(e); break;
    case Item::Frame: ShowPanel(e, !e.panel); return;
    case Item::Copy: CopyNow(e); break;
    case Item::Save: SaveNow(e); break;
    case Item::Alpha: MakeAlpha(e); break;
    case Item::NoBg: RemoveBackground(e); break;
    case Item::Upload: Done(e, true); return;
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
    if (PanelDown(e, x, y)) return;
    const POINT cp{x, y};
    if (!PtInRect(&e.imageArea, cp)) return;
    if (e.space) {
        StartPan(e, x, y);
        return;
    }
    const G::PointF p = ToImage(e, x, y);
    e.pfocus = -1;  // the panel lets go of the keyboard
    if (e.tool == Tool::Crop) {
        CropDown(e, p);
        Redraw(e);
        return;
    }
    if (!e.typing) {
        // Grab the selected shape's handle, or a shape to move.
        if (const int h = HandleAt(e, p); h >= 0) {
            StartDrag(e, e.sel, h, p);
            Redraw(e);
            return;
        }
        if (const int i = GrabAt(e, p); i >= 0) {
            StartDrag(e, i, -1, p);
            Redraw(e);
            return;
        }
        if (e.tool == Tool::Text) {  // clicking an existing text with the Text tool edits it
            const int i = ShapeAt(e, p);
            if (i >= 0 && e.shapes[i].tool == Tool::Text) {
                EditText(e, i);
                return;
            }
        }
    }
    if (e.tool == Tool::Select) {  // empty space: unselect, and drag to move around
        FinishText(e);
        e.sel = -1;
        StartPan(e, x, y);
        Redraw(e);
        return;
    }
    const bool wasTyping = e.typing;
    FinishText(e);
    e.sel = -1;
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
    if (e.pdrag >= 0) {
        SliderTo(e, e.pdrag, x);
        return;
    }
    if (e.cropDrag) {
        CropMove(e, ToImage(e, x, y), (keys & MK_SHIFT) != 0);
        Redraw(e);
        return;
    }
    if (e.dragging) {
        DragTo(e, ToImage(e, x, y), (keys & MK_SHIFT) != 0);
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
    if (e.pdrag >= 0) {
        PanelUp(e);
        Redraw(e);
        return;
    }
    if (e.cropDrag) {
        CropUp(e);
        Redraw(e);
        return;
    }
    if (e.dragging) {
        EndDrag(e, false);
        Redraw(e);
        return;
    }
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
        CancelText(e);
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
    if (!ctrl && PanelKey(e, vk, shift)) return;
    const bool arrow = vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN;
    if (e.tool == Tool::Crop && !e.cropDrag) {
        if (arrow) {
            CropKey(e, vk, ctrl, shift);
            Redraw(e);
            return;
        }
        if (vk == VK_RETURN && e.cropBox) {
            ApplyCrop(e);
            Redraw(e);
            return;
        }
        if (vk == VK_ESCAPE) {
            if (e.cropBox) e.cropBox = false;
            else SetTool(e, e.prevTool);
            Redraw(e);
            return;
        }
    }
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
        if (e.cropDrag) {  // put the box back as it was
            e.cropDrag = 0;
            e.crop = e.cropStart;
            if (GetCapture() == e.hwnd) ReleaseCapture();
        } else if (e.drawing) {
            e.drawing = false;
            ReleaseCapture();
        } else if (e.dragging) {
            EndDrag(e, true);
        } else if (e.sel >= 0) {
            e.sel = -1;
        } else {
            Close(e);
            return;
        }
        Redraw(e);
        return;
    }
    if (vk == VK_RETURN) {
        Done(e);
        return;
    }
    if ((vk == VK_DELETE || vk == VK_BACK) && e.sel >= 0) {
        DeleteSelected(e);
    } else if (e.sel >= 0 && (vk == VK_LEFT || vk == VK_RIGHT || vk == VK_UP || vk == VK_DOWN)) {
        const float step = (shift ? 10.f : 1.f) * e.sc;  // nudge
        const float dx = vk == VK_LEFT ? -step : vk == VK_RIGHT ? step : 0;
        const float dy = vk == VK_UP ? -step : vk == VK_DOWN ? step : 0;
        ChangeSelected(e, [&](Shape& s) {
            s.a = G::PointF(s.a.X + dx, s.a.Y + dy);
            s.b = G::PointF(s.b.X + dx, s.b.Y + dy);
        });
    } else if (vk == 'M') {
        MakeAlpha(e);
        return;
    } else if (vk == 'B') {
        RemoveBackground(e);
        return;
    } else if (vk == 'U') {
        Done(e, true);
        return;
    } else if (vk == 'F') {
        ShowPanel(e, !e.panel);
        return;
    } else if (vk >= '1' && vk < '1' + kColorCount) {
        SetColor(e, static_cast<int>(vk - '1'));
    } else if (vk == VK_OEM_4) {
        SetSize(e, e.size - 1);
    } else if (vk == VK_OEM_6) {
        SetSize(e, e.size + 1);
    } else {
        for (const ToolDef& t : kTools)
            if (t.key == vk) SetTool(e, t.tool);
    }
    Redraw(e);
}

// ---- test hook -------------------------------------------------------------------------------

constexpr ULONG_PTR kTestCommand = 0x45444954;  // "EDIT"

// UI tests (test mode only) send WM_COPYDATA with lines like these, run through the same code as the
// mouse and keys:
//   crop L T R B          blur L T R B [size]       tool <key letter>
//   frame on|off          panel on|off              focus <row>
//   padding N  corners N  shadow 0|1  softness N    bg white|grey|dark|gradient|transparent|RRGGBB
//   shape original|16:9|1:1|4:3                     undo  redo
//   saveas <path.png>     info (writes sizes, zoom and where the picture is to the log)
void TestCommands(Editor& e, const std::wstring& text) {
    std::wistringstream lines(text);
    std::wstring line;
    while (std::getline(lines, line)) {
        std::wistringstream in(line);
        std::wstring cmd, arg;
        in >> cmd;
        if (cmd.empty()) continue;
        if (cmd == L"crop") {
            float l = 0, t = 0, r = 0, b = 0;
            in >> l >> t >> r >> b;
            SetTool(e, Tool::Crop);
            e.crop = G::RectF(l, t, r - l, b - t);
            e.cropBox = true;
            ApplyCrop(e);
        } else if (cmd == L"blur") {
            float l = 0, t = 0, r = 0, b = 0;
            int size = e.size;
            in >> l >> t >> r >> b >> size;
            Shape s;
            s.tool = Tool::Blur;
            s.size = std::clamp(size, 0, 2);
            s.a = G::PointF(l, t);
            s.b = G::PointF(r, b);
            Commit(e, s);
        } else if (cmd == L"tool") {
            in >> arg;
            for (const ToolDef& t : kTools)
                if (!arg.empty() && t.key == static_cast<UINT>(towupper(arg[0]))) SetTool(e, t.tool);
        } else if (cmd == L"undo") {
            Undo(e);
        } else if (cmd == L"redo") {
            Redo(e);
        } else if (cmd == L"panel") {
            in >> arg;
            ShowPanel(e, arg == L"on");
        } else if (cmd == L"focus") {
            int row = -1;
            in >> row;
            e.pfocus = std::clamp(row, -1, kRowCount - 1);
        } else if (cmd == L"frame" || cmd == L"padding" || cmd == L"corners" || cmd == L"shadow" ||
                   cmd == L"softness" || cmd == L"bg" || cmd == L"shape") {
            in >> arg;
            if (cmd == L"frame") e.fr.on = arg == L"on";
            else if (cmd == L"padding") e.fr.padding = std::clamp(_wtoi(arg.c_str()), 0, 25);
            else if (cmd == L"corners") e.fr.corners = std::clamp(_wtoi(arg.c_str()), 0, 20);
            else if (cmd == L"shadow") e.fr.shadow = arg == L"1";
            else if (cmd == L"softness") e.fr.softness = std::clamp(_wtoi(arg.c_str()), 0, 100);
            else if (cmd == L"bg") {
                bool named = false;
                for (int i = 0; i < kFrameBgCount; ++i)
                    if (arg == kFrameBgKeys[i]) e.fr.bg = static_cast<FrameBg>(i), named = true;
                if (!named && arg.size() == 6) {
                    const unsigned long v = wcstoul(arg.c_str(), nullptr, 16);
                    e.fr.custom = RGB((v >> 16) & 255, (v >> 8) & 255, v & 255);
                    e.fr.bg = FrameBg::Custom;
                }
            } else {
                for (int i = 0; i < kFrameShapeCount; ++i)
                    if (arg == kFrameShapeKeys[i]) e.fr.shape = i;
            }
            FrameChanged(e, true);
        } else if (cmd == L"saveas") {
            std::getline(in >> std::ws, arg);
            const Image img = OutputImage(e);
            DeleteFileW(arg.c_str());
            const bool ok = WriteFileBytes(arg, EncodePng(img));
            Log(L"Editor test: saved %d x %d%s -> %s (%s)", img.w, img.h, img.alpha ? L" with alpha" : L"", arg.c_str(),
                ok ? L"ok" : L"failed");
        } else if (cmd == L"info") {
            const FrameGeom g = Geom(e);
            Log(L"Editor test: image %d x %d, canvas %d x %d, shapes %zu, undo %zu, zoom %.4f, origin %ld %ld, area %ld %ld "
                L"%ld %ld, tool %d, cropBox %d",
                e.ImgW(), e.ImgH(), g.cw, g.ch, e.shapes.size(), e.undo.size(), e.zoom, e.origin.x, e.origin.y,
                e.imageArea.left, e.imageArea.top, e.imageArea.right, e.imageArea.bottom, static_cast<int>(e.tool),
                e.cropBox ? 1 : 0);
        } else {
            Log(L"Editor test: unknown command %s", cmd.c_str());
        }
    }
    Redraw(e);
    UpdateWindow(e.hwnd);
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
    case WM_APP_NOBG: {  // Remove background finished (rmbg.cpp)
        std::unique_ptr<std::wstring> text(reinterpret_cast<std::wstring*>(lp));
        ShowStatus(e, *text);
        SetTimer(h, kTimerStatus, 6000, nullptr);
        return 0;
    }
    case WM_COPYDATA: {  // test copies only: commands from a UI test script (see TestCommands)
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (!TestMode() || !cd || cd->dwData != kTestCommand || !cd->lpData) return FALSE;
        TestCommands(e, std::wstring(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t)));
        return TRUE;
    }
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
        RECT r{0, 0, ToolbarWidth(e), e.Px(kToolbarH + kStatusH + (e.panel ? 520 : 120))};
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
            const G::PointF ip = ToImage(e, p.x, p.y);
            const bool crop = e.tool == Tool::Crop;
            const int hd = img && !e.space && !crop ? (e.dragging ? e.handle : HandleAt(e, ip)) : -1;
            const bool grab = img && !e.space && !crop && (e.dragging || GrabAt(e, ip) >= 0);
            const wchar_t* cur = IDC_CROSS;
            const wchar_t* bySide[] = {IDC_SIZENWSE, IDC_SIZENS, IDC_SIZENESW, IDC_SIZEWE};
            if (e.panning || (img && e.space)) cur = IDC_SIZEALL;
            else if (e.pdrag >= 0 || !img) cur = IDC_ARROW;
            else if (crop) {
                const int ch = e.cropDrag == 3 ? e.cropHandle : e.cropDrag ? -1 : CropHandleAt(e, ip);
                if (ch >= 0) cur = bySide[ch % 4];
                else if (e.cropDrag == 2 || (!e.cropDrag && InCrop(e, ip))) cur = IDC_SIZEALL;
            }
            else if (hd >= 0 && IsLine(e.shapes[e.sel].tool)) cur = IDC_CROSS;
            else if (hd >= 0) cur = bySide[hd % 4];
            else if (grab) cur = IDC_SIZEALL;
            else if (e.tool == Tool::Select) cur = IDC_ARROW;
            else if (e.tool == Tool::Text) cur = IDC_IBEAM;
            SetCursor(LoadCursorW(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_LBUTTONDOWN:
        OnLButtonDown(e, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_LBUTTONDBLCLK: {  // double-click a text to edit it; otherwise a normal click (quick markers)
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        const POINT cp{x, y};
        if (e.tool == Tool::Crop && PtInRect(&e.imageArea, cp) && !e.space && InCrop(e, ToImage(e, x, y))) {
            CropUp(e);
            ApplyCrop(e);  // double-click in the box crops
            return 0;
        }
        if (PtInRect(&e.imageArea, cp) && HitItem(e, x, y) < 0 && !e.space) {
            if (e.typing && e.editText < 0 && e.live.text.empty()) CancelText(e);
            const int i = e.typing ? -1 : ShapeAt(e, ToImage(e, x, y));
            if (i >= 0 && e.shapes[i].tool == Tool::Text) {
                EditText(e, i);
                return 0;
            }
        }
        OnLButtonDown(e, x, y);
        return 0;
    }
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
        if (e.pdrag >= 0) PanelUp(e);
        if (e.cropDrag) {
            CropUp(e);
            Redraw(e);
        }
        if (e.dragging) {
            EndDrag(e, false);
            Redraw(e);
        }
        return 0;
    case WM_KILLFOCUS:
        e.space = false;
        EndPan(e);
        return 0;
    case WM_RBUTTONUP:  // right-click cancels the shape being drawn or the move
        if (e.cropDrag) {
            e.cropDrag = 0;
            e.crop = e.cropStart;
            if (GetCapture() == h) ReleaseCapture();
            Redraw(e);
        } else if (e.drawing) {
            e.drawing = false;
            ReleaseCapture();
            Redraw(e);
        } else if (e.dragging) {
            EndDrag(e, true);
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
        if (e.panel) {  // over the frame panel: nothing to size there
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(h, &p);
            if (PtInRect(&e.panelRect, p)) return 0;
        }
        SetSize(e, e.size + (GET_WHEEL_DELTA_WPARAM(wp) > 0 ? 1 : -1));
        Redraw(e);
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_SPACE && e.panel && e.pfocus >= 0 && !e.typing) {  // Space works the focused frame control
            if (!(lp & (1 << 30))) OnKey(e, VK_SPACE);
            return 0;
        }
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
    Gdiplus_Ensure();
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = EditorProc;
        wc.style = CS_DBLCLKS;
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
    e->img = std::make_shared<const Image>(e->src->img);
    const int w = e->img->w, h = e->img->h;
    if (!e->base.Create(w, h) || !e->committed.Create(w, h)) {
        Log(L"Editor: out of memory for %d x %d", w, h);
        delete e;
        return;
    }
    memcpy(e->base.bits, e->img->px.data(), static_cast<size_t>(w) * h * 4);
    memcpy(e->committed.bits, e->base.bits, static_cast<size_t>(w) * h * 4);
    LoadFrame(e->fr);
    Refresh(*e);

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
    const int cw = CanvasW(*e), ch = CanvasH(*e);  // frame included
    const float z = std::min({1.f, static_cast<float>(maxW - chromeW) / cw, static_cast<float>(maxH - chromeH) / ch});
    int ww = std::max(static_cast<int>(cw * z) + chromeW, ToolbarWidth(*e) + fw);
    int wh = std::max(static_cast<int>(ch * z) + chromeH, e->Px(kToolbarH + kStatusH + 200) + fh);
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
    Theme_Window(hw);
    e->dpi = GetDpiForWindow(hw);
    MakeFonts(*e);
    Layout(*e);
    ShowWindow(hw, SW_SHOWNORMAL);
    SetForegroundWindow(hw);
    Log(L"Editor opened %d x %d", w, h);
}
