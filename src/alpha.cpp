// Make alpha: turns a picture into a square black-and-white alpha (detail white, background black), for
// Substance Painter stamps and the like. Drag around the part you want (lasso or rectangle; several areas add up),
// click the detail color, right-click the background, fine-tune with the Levels bar. The detail is cropped,
// centred on a black square with a small margin and scaled to a power-of-two size.
//
// Colors are compared mostly by hue: brightness counts less, so light and shadow across one painted surface
// stay "background" while a differently colored detail on it stands out.
#include "app.h"
#include "theme.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellscalingapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>

#pragma comment(lib, "msimg32.lib")

namespace {

// ---- engine --------------------------------------------------------------------------------

struct Feat {
    float l, a, b;
};

Feat ToFeat(uint32_t p) {
    const float b = (p & 255) / 255.f, g = ((p >> 8) & 255) / 255.f, r = ((p >> 16) & 255) / 255.f;
    const float s = r + g + b;
    const float conf = std::min(1.f, s / 0.45f);  // the hue of very dark pixels is mostly noise
    const float cr = s > 1e-4f ? r / s : 1 / 3.f, cg = s > 1e-4f ? g / s : 1 / 3.f;
    return {(0.299f * r + 0.587f * g + 0.114f * b) * 0.7f, (cr - 1 / 3.f) * 2.2f * conf, (cg - 1 / 3.f) * 2.2f * conf};
}

float Dist(const Feat& x, const Feat& y) {
    const float a = x.l - y.l, b = x.a - y.a, c = x.b - y.b;
    return std::sqrt(a * a + b * b + c * c);
}

struct Params {
    std::vector<uint8_t> area;     // per image pixel, 1 = inside the mask; empty = the whole picture
    RECT box{};                    // bounds of the area (image pixels); empty = the whole picture
    std::vector<uint32_t> detail;  // picked detail colors
    bool hasSurface = false;       // background picked by hand (else: the most common color)
    uint32_t surface = 0;
    // Levels on the detail score (0 = background, 1 = detail, 0.5 = the natural split): below `black` is
    // black, above `white` is white, `gamma` bends the part between (> 1 brighter).
    float black = 0.425f, white = 0.575f, gamma = 1.f;
    bool invert = false;
};

constexpr int kMaxPicks = 8, kHistBins = 64;

// How much each pixel of the area looks like detail. Slow part; levels and invert only redo Finish.
struct Score {
    RECT box{};
    int bw = 0, bh = 0;
    std::vector<float> s;      // bw x bh, 0..1
    std::vector<uint8_t> in;   // inside the mask
    float hist[kHistBins]{};   // of s inside the mask, 0..1 (log scale)
};

struct Mask {
    int side = 0;  // square, before scaling
    std::vector<float> v;
};

RECT ClampBox(const Image& img, RECT b) {
    if (IsRectEmpty(&b)) return {0, 0, img.w, img.h};
    b.left = std::clamp<LONG>(b.left, 0, img.w);
    b.right = std::clamp<LONG>(b.right, 0, img.w);
    b.top = std::clamp<LONG>(b.top, 0, img.h);
    b.bottom = std::clamp<LONG>(b.bottom, 0, img.h);
    if (b.right - b.left < 2 || b.bottom - b.top < 2) return {0, 0, img.w, img.h};
    return b;
}

uint32_t AverageAt(const Image& img, int cx, int cy, int r = 2) {
    uint64_t s[3] = {};
    int n = 0;
    for (int y = std::max(0, cy - r); y <= std::min(img.h - 1, cy + r); ++y)
        for (int x = std::max(0, cx - r); x <= std::min(img.w - 1, cx + r); ++x) {
            const uint32_t p = img.px[static_cast<size_t>(y) * img.w + x];
            s[0] += p & 255;
            s[1] += (p >> 8) & 255;
            s[2] += (p >> 16) & 255;
            ++n;
        }
    if (!n) return 0;
    return static_cast<uint32_t>(s[0] / n) | static_cast<uint32_t>(s[1] / n) << 8 | static_cast<uint32_t>(s[2] / n) << 16;
}

// The most common color inside the mask (32 levels per channel), averaged over the pixels in that bin.
// Colors close to a picked detail color are skipped: inside a tight mask the detail may cover the most area.
uint32_t MostCommon(const Image& img, const RECT& b, const std::vector<uint8_t>& area, const std::vector<Feat>& avoid) {
    std::vector<uint32_t> count(32768);
    auto key = [](uint32_t p) { return ((p >> 19) & 31) << 10 | ((p >> 11) & 31) << 5 | ((p >> 3) & 31); };
    auto inside = [&](size_t i) { return area.empty() || area[i]; };
    for (int y = b.top; y < b.bottom; ++y)
        for (int x = b.left; x < b.right; ++x) {
            const size_t i = static_cast<size_t>(y) * img.w + x;
            if (inside(i)) ++count[key(img.px[i])];
        }
    uint32_t best = 0, bestAny = 0;
    for (uint32_t k = 0; k < 32768; ++k) {
        if (!count[k]) continue;
        if (count[k] > count[bestAny]) bestAny = k;
        if (count[k] <= count[best]) continue;
        const uint32_t c = ((k >> 10) & 31) << 19 | ((k >> 5) & 31) << 11 | (k & 31) << 3 | 0x040404u;
        const Feat f = ToFeat(c);
        bool clear = true;
        for (const Feat& a : avoid)
            if (Dist(f, a) < 0.12f) clear = false;
        if (clear) best = k;
    }
    if (!count[best]) best = bestAny;
    uint64_t s[3] = {};
    uint64_t n = 0;
    for (int y = b.top; y < b.bottom; ++y)
        for (int x = b.left; x < b.right; ++x) {
            const size_t i = static_cast<size_t>(y) * img.w + x;
            const uint32_t p = img.px[i];
            if (!inside(i) || key(p) != best) continue;
            s[0] += p & 255;
            s[1] += (p >> 8) & 255;
            s[2] += (p >> 16) & 255;
            ++n;
        }
    if (!n) return 0;
    return static_cast<uint32_t>(s[0] / n) | static_cast<uint32_t>(s[1] / n) << 8 | static_cast<uint32_t>(s[2] / n) << 16;
}

// Fills a closed polygon (image pixels) into `area` (w x h), even-odd rule, pixel centres.
void FillPolygon(const std::vector<POINT>& poly, int w, int h, std::vector<uint8_t>& area) {
    const size_t n = poly.size();
    std::vector<float> xs;
    int y0 = h, y1 = -1;
    for (const POINT& p : poly) {
        y0 = std::min<int>(y0, p.y);
        y1 = std::max<int>(y1, p.y);
    }
    for (int y = std::max(0, y0); y <= std::min(h - 1, y1); ++y) {
        const float cy = y + 0.5f;
        xs.clear();
        for (size_t i = 0; i < n; ++i) {
            const POINT a = poly[i], b = poly[(i + 1) % n];
            const float ay = a.y + 0.5f, by = b.y + 0.5f;
            if ((ay <= cy) == (by <= cy)) continue;
            xs.push_back(a.x + 0.5f + (cy - ay) / (by - ay) * (b.x - a.x));
        }
        std::sort(xs.begin(), xs.end());
        for (size_t i = 0; i + 1 < xs.size(); i += 2) {
            const int xa = std::max(0, static_cast<int>(std::ceil(xs[i] - 0.5f)));
            const int xb = std::min(w - 1, static_cast<int>(std::floor(xs[i + 1] - 0.5f)));
            for (int x = xa; x <= xb; ++x) area[static_cast<size_t>(y) * w + x] = 1;
        }
    }
}

// Threshold that best splits the values into two groups.
float Otsu(const std::vector<float>& d, float mx) {
    double hist[256] = {};
    for (float v : d) ++hist[std::min(255, static_cast<int>(v / mx * 255.99f))];
    double total = static_cast<double>(d.size()), sumAll = 0;
    for (int i = 0; i < 256; ++i) sumAll += i * hist[i];
    double w0 = 0, sum0 = 0, best = -1;
    int bi = 0;
    for (int i = 0; i < 255; ++i) {
        w0 += hist[i];
        sum0 += i * hist[i];
        const double w1 = total - w0;
        if (w0 == 0 || w1 == 0) continue;
        const double m0 = sum0 / w0, m1 = (sumAll - sum0) / w1, v = w0 * w1 * (m0 - m1) * (m0 - m1);
        if (v > best) {
            best = v;
            bi = i;
        }
    }
    return (bi + 1) / 256.f * mx;
}

// Drops tiny specks (noise, dust) and, when `cleanSoft`, keeps soft edge pixels only next to real detail.
void DropSpecks(std::vector<float>& m, int w, int h, bool cleanSoft) {
    const size_t minSize = static_cast<size_t>(std::max(8, w * h / 5000));
    std::vector<uint8_t> state(m.size(), 0);  // 1 kept, 2 dropped
    std::vector<int> stack, comp;
    for (int i = 0; i < w * h; ++i) {
        if (m[i] < 0.5f || state[i]) continue;
        comp.clear();
        stack.push_back(i);
        state[i] = 1;
        while (!stack.empty()) {
            const int j = stack.back();
            stack.pop_back();
            comp.push_back(j);
            const int x = j % w, y = j / w;
            const int nb[4] = {x > 0 ? j - 1 : -1, x < w - 1 ? j + 1 : -1, y > 0 ? j - w : -1, y < h - 1 ? j + w : -1};
            for (int k : nb)
                if (k >= 0 && !state[k] && m[k] >= 0.5f) {
                    state[k] = 1;
                    stack.push_back(k);
                }
        }
        if (comp.size() < minSize)
            for (int j : comp) {
                state[j] = 2;
                m[j] = 0;
            }
    }
    if (!cleanSoft) return;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float& v = m[static_cast<size_t>(y) * w + x];
            if (v <= 0 || v >= 0.5f) continue;
            bool nextTo = false;
            for (int yy = std::max(0, y - 2); yy <= std::min(h - 1, y + 2) && !nextTo; ++yy)
                for (int xx = std::max(0, x - 2); xx <= std::min(w - 1, x + 2); ++xx)
                    if (state[static_cast<size_t>(yy) * w + xx] == 1) {
                        nextTo = true;
                        break;
                    }
            if (!nextTo) v = 0;
        }
}

Score MakeScore(const Image& img, const Params& p, uint32_t* surfaceUsed) {
    Score sc;
    const RECT b = ClampBox(img, p.box);
    const int bw = b.right - b.left, bh = b.bottom - b.top;
    sc.box = b;
    sc.bw = bw;
    sc.bh = bh;
    std::vector<Feat> fd;
    for (uint32_t c : p.detail) fd.push_back(ToFeat(c));
    const uint32_t surf = p.hasSurface ? p.surface : MostCommon(img, b, p.area, fd);
    if (surfaceUsed) *surfaceUsed = surf;
    const Feat fs = ToFeat(surf);
    sc.in.resize(static_cast<size_t>(bw) * bh);
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x)
            sc.in[static_cast<size_t>(y) * bw + x] = p.area.empty() || p.area[static_cast<size_t>(b.top + y) * img.w + b.left + x];

    std::vector<float>& s = sc.s;
    s.resize(sc.in.size());
    if (fd.empty()) {  // nothing picked: how far from the background, 0.5 = the best split (Otsu)
        std::vector<float> in;
        float mx = 1e-6f;
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                const size_t i = static_cast<size_t>(y) * bw + x;
                s[i] = Dist(ToFeat(img.px[static_cast<size_t>(b.top + y) * img.w + b.left + x]), fs);
                if (!sc.in[i]) continue;
                in.push_back(s[i]);
                mx = std::max(mx, s[i]);
            }
        if (in.empty()) in.push_back(0);
        const float t = std::max(0.04f, Otsu(in, mx));
        for (float& v : s) v = std::clamp(v / (2 * t), 0.f, 1.f);
    } else {  // closer to a picked detail color (1) than to the background (0)
        for (int y = 0; y < bh; ++y)
            for (int x = 0; x < bw; ++x) {
                const Feat f = ToFeat(img.px[static_cast<size_t>(b.top + y) * img.w + b.left + x]);
                const float ds = Dist(f, fs);
                float dd = 1e9f;
                for (const Feat& c : fd) dd = std::min(dd, Dist(f, c));
                s[static_cast<size_t>(y) * bw + x] = ds / (ds + dd + 1e-6f);
            }
    }
    uint32_t count[kHistBins] = {}, top = 1;
    for (size_t i = 0; i < s.size(); ++i)
        if (sc.in[i]) top = std::max(top, ++count[std::min(kHistBins - 1, static_cast<int>(s[i] * kHistBins))]);
    for (int i = 0; i < kHistBins; ++i) sc.hist[i] = std::log1p(static_cast<float>(count[i])) / std::log1p(static_cast<float>(top));
    return sc;
}

float Levels(float s, const Params& p) {
    float v = std::clamp((s - p.black) / std::max(1e-3f, p.white - p.black), 0.f, 1.f);
    if (p.gamma != 1.f) v = std::pow(v, 1 / p.gamma);
    return v;
}

Mask Finish(const Score& sc, const Params& p) {
    const int bw = sc.bw, bh = sc.bh;
    std::vector<float> m(sc.s.size());
    for (size_t i = 0; i < m.size(); ++i) m[i] = sc.in[i] ? Levels(sc.s[i], p) : 0.f;
    DropSpecks(m, bw, bh, p.white - p.black < 0.35f);  // a wide levels range wants its soft grays kept
    if (p.invert)  // only inside the mask: outside stays black
        for (size_t i = 0; i < m.size(); ++i)
            if (sc.in[i]) m[i] = 1 - m[i];

    // crop to the detail (plus its soft edge), centre it on a black square with a margin
    int x0 = bw, y0 = bh, x1 = -1, y1 = -1;
    for (int y = 0; y < bh; ++y)
        for (int x = 0; x < bw; ++x)
            if (m[static_cast<size_t>(y) * bw + x] >= 0.5f) {
                x0 = std::min(x0, x);
                x1 = std::max(x1, x);
                y0 = std::min(y0, y);
                y1 = std::max(y1, y);
            }
    Mask out;
    if (x1 < 0) {
        out.side = 64;
        out.v.assign(64 * 64, 0.f);
        return out;
    }
    x0 = std::max(0, x0 - 2);
    y0 = std::max(0, y0 - 2);
    x1 = std::min(bw - 1, x1 + 2);
    y1 = std::min(bh - 1, y1 + 2);
    const int cw = x1 - x0 + 1, ch = y1 - y0 + 1, side = std::max(cw, ch);
    const int margin = std::max(2, static_cast<int>(std::lround(side * 0.06)));
    out.side = side + 2 * margin;
    out.v.assign(static_cast<size_t>(out.side) * out.side, 0.f);
    const int ox = margin + (side - cw) / 2, oy = margin + (side - ch) / 2;
    for (int y = 0; y < ch; ++y)
        std::copy_n(&m[static_cast<size_t>(y0 + y) * bw + x0], cw, &out.v[static_cast<size_t>(oy + y) * out.side + ox]);
    return out;
}

Mask Compute(const Image& img, const Params& p, uint32_t* surfaceUsed) { return Finish(MakeScore(img, p, surfaceUsed), p); }

const int kSizes[] = {0, 512, 1024, 2048, 4096};  // 0 = automatic
constexpr int kSizeCount = static_cast<int>(std::size(kSizes));

int OutputSize(const Mask& m, int sizeIdx) {
    if (kSizes[sizeIdx]) return kSizes[sizeIdx];
    int n = 512;
    while (n < m.side && n < 4096) n *= 2;
    return n;
}

// Scales the square to n x n (cubic filter, widened when shrinking) as a gray BGRX image.
Image Scale(const Mask& m, int n) {
    auto cubic = [](float x) {
        x = std::fabs(x);
        if (x < 1) return 1.5f * x * x * x - 2.5f * x * x + 1;
        if (x < 2) return -0.5f * x * x * x + 2.5f * x * x - 4 * x + 2;
        return 0.f;
    };
    const int s = m.side;
    const float scale = static_cast<float>(n) / s, fs = scale < 1 ? 1 / scale : 1, support = 2 * fs;
    struct Taps {
        std::vector<int> idx;
        std::vector<float> w;
    };
    std::vector<Taps> taps(n);
    for (int o = 0; o < n; ++o) {
        const float c = (o + 0.5f) / scale - 0.5f;
        float sum = 0;
        for (int i = static_cast<int>(std::ceil(c - support)); i <= static_cast<int>(std::floor(c + support)); ++i) {
            const float w = cubic((i - c) / fs);
            if (w == 0) continue;
            taps[o].idx.push_back(std::clamp(i, 0, s - 1));
            taps[o].w.push_back(w);
            sum += w;
        }
        for (float& w : taps[o].w) w /= sum;
    }
    std::vector<float> tmp(static_cast<size_t>(n) * s);  // rows scaled: n wide, s tall
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < n; ++x) {
            float v = 0;
            for (size_t k = 0; k < taps[x].idx.size(); ++k) v += m.v[static_cast<size_t>(y) * s + taps[x].idx[k]] * taps[x].w[k];
            tmp[static_cast<size_t>(y) * n + x] = v;
        }
    Image out;
    out.w = out.h = n;
    out.px.resize(static_cast<size_t>(n) * n);
    for (int y = 0; y < n; ++y)
        for (int x = 0; x < n; ++x) {
            float v = 0;
            for (size_t k = 0; k < taps[y].idx.size(); ++k) v += tmp[static_cast<size_t>(taps[y].idx[k]) * n + x] * taps[y].w[k];
            const uint32_t g = static_cast<uint32_t>(std::lround(std::clamp(v, 0.f, 1.f) * 255));
            out.px[static_cast<size_t>(y) * n + x] = g * 0x010101u;
        }
    return out;
}

// ---- window --------------------------------------------------------------------------------

constexpr wchar_t kClass[] = L"CaniveteAlpha";
constexpr COLORREF kAccent = RGB(232, 44, 56);
constexpr int kHintH = 34, kBarH = 52, kGap = 12;
// Levels block under the result: histogram, gradient strip, handles; then its text line and the background swatch.
constexpr int kHistH = 44, kStripH = 6, kHandleH = 12, kUnderRes = 8 + kHistH + kStripH + kHandleH + 22 + 26 + 8;

enum BtnId { B_LASSO, B_RECT, B_INVERT, B_SIZE, B_RESET, B_COPY, B_SAVE, B_COUNT };
enum Tool { T_LASSO, T_RECT };

struct Btn {
    BtnId id;
    std::wstring label;
    RECT r{};
};

struct Win {
    HWND hwnd = nullptr;
    Image img;
    CaptureResult meta;
    Dib src, prev, back, shade;
    Params p;
    Score score;
    Mask mask;
    int tool = T_LASSO;
    RECT levR{};      // histogram + strip + handles
    int levDrag = -1; // handle being dragged: 0 black, 1 mid, 2 white
    uint32_t surfaceUsed = 0;
    std::vector<POINT> detailPts;  // image pixels, for the markers
    POINT surfacePt{-1, -1};
    int sizeIdx = 0;
    UINT dpi = 96;
    HFONT ui = nullptr, bold = nullptr;
    RECT srcArea{}, resArea{};
    float zoom = 1;
    POINT org{};
    bool down = false, dragging = false;
    POINT downPt{}, curPt{};
    std::vector<POINT> path;                // freehand mask being drawn (client coords)
    std::vector<std::vector<POINT>> areas;  // finished mask outlines (image pixels)
    std::vector<char> history;              // 'p' pick, 'a' area: what Ctrl+Z takes back
    Dib shaded;                             // the picture, darkened outside the mask
    std::vector<Btn> btns;
    int hover = -1;

    int Px(int v) const { return MulDiv(v, static_cast<int>(dpi), 96); }
};

Win* g_win = nullptr;

void MakeFonts(Win& w) {
    for (HFONT* f : {&w.ui, &w.bold})
        if (*f) DeleteObject(*f);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, w.dpi);
    w.ui = CreateFontIndirectW(&ncm.lfMessageFont);
    LOGFONTW lf = ncm.lfMessageFont;
    lf.lfWeight = FW_SEMIBOLD;
    w.bold = CreateFontIndirectW(&lf);
}

std::wstring SizeLabel(const Win& w) {
    const int n = OutputSize(w.mask, w.sizeIdx);
    return kSizes[w.sizeIdx] ? L"Size: " + std::to_wstring(n) : L"Size: auto (" + std::to_wstring(n) + L")";
}

void Refine(Win& w) {  // levels / invert changed: the score stays
    w.mask = Finish(w.score, w.p);
    const int s = w.mask.side;
    if (w.prev.w != s) w.prev.Create(s, s);
    if (w.prev.bits)
        for (size_t i = 0; i < w.mask.v.size(); ++i)
            w.prev.bits[i] = static_cast<uint32_t>(std::lround(w.mask.v[i] * 255)) * 0x010101u;
}

void Recompute(Win& w) {
    w.score = MakeScore(w.img, w.p, &w.surfaceUsed);
    Refine(w);
}

void RebuildArea(Win& w) {
    w.p.area.clear();
    w.p.box = {};
    if (w.areas.empty()) return;
    w.p.area.assign(static_cast<size_t>(w.img.w) * w.img.h, 0);
    for (const auto& poly : w.areas) FillPolygon(poly, w.img.w, w.img.h, w.p.area);
    RECT b{w.img.w, w.img.h, 0, 0};
    for (int y = 0; y < w.img.h; ++y)
        for (int x = 0; x < w.img.w; ++x)
            if (w.p.area[static_cast<size_t>(y) * w.img.w + x]) {
                b.left = std::min<LONG>(b.left, x);
                b.top = std::min<LONG>(b.top, y);
                b.right = std::max<LONG>(b.right, x + 1);
                b.bottom = std::max<LONG>(b.bottom, y + 1);
            }
    if (b.right <= b.left) {  // too thin to hold a pixel
        w.areas.pop_back();
        w.p.area.clear();
        RebuildArea(w);
        return;
    }
    w.p.box = b;
    if (w.shaded.w != w.img.w || w.shaded.h != w.img.h) w.shaded.Create(w.img.w, w.img.h);
    if (!w.shaded.bits) return;
    for (size_t i = 0; i < w.p.area.size(); ++i) {
        const uint32_t px = w.img.px[i];
        w.shaded.bits[i] = w.p.area[i] ? px : (px >> 2 & 0x3F3F3F);
    }
}

void Layout(Win& w) {
    RECT rc;
    GetClientRect(w.hwnd, &rc);
    const int m = w.Px(kGap), top = w.Px(kHintH), bot = rc.bottom - w.Px(kBarH);
    const int midH = std::max(1, bot - top - 2 * m - w.Px(kUnderRes)), res = std::max(1, std::min(midH, (static_cast<int>(rc.right) - 3 * m) / 2));
    w.resArea = {rc.right - m - res, top + m, rc.right - m, top + m + res};
    w.levR = {w.resArea.left, w.resArea.bottom + w.Px(8), w.resArea.right, w.resArea.bottom + w.Px(8 + kHistH + kStripH + kHandleH)};
    w.srcArea = {m, top + m, w.resArea.left - m, bot - m};
    const int aw = std::max<int>(1, w.srcArea.right - w.srcArea.left), ah = std::max<int>(1, w.srcArea.bottom - w.srcArea.top);
    w.zoom = std::min({4.f, static_cast<float>(aw) / w.img.w, static_cast<float>(ah) / w.img.h});
    w.org = {w.srcArea.left + (aw - static_cast<int>(w.img.w * w.zoom)) / 2,
             w.srcArea.top + (ah - static_cast<int>(w.img.h * w.zoom)) / 2};

    // buttons: tuning on the left, Copy and Save on the right
    w.btns.clear();
    HDC dc = GetDC(w.hwnd);
    HGDIOBJ old = SelectObject(dc, w.ui);
    const int bh = w.Px(32), by = rc.bottom - w.Px(kBarH) + (w.Px(kBarH) - bh) / 2;
    auto width = [&](const std::wstring& s) {
        SIZE sz{};
        GetTextExtentPoint32W(dc, s.c_str(), static_cast<int>(s.size()), &sz);
        return sz.cx + w.Px(24);
    };
    int x = m;
    auto add = [&](BtnId id, std::wstring label, int gapAfter) {
        const int bw = id == B_SIZE ? width(L"Size: auto (4096)") : width(label);
        w.btns.push_back({id, std::move(label), {x, by, x + bw, by + bh}});
        x += bw + w.Px(gapAfter);
    };
    add(B_LASSO, L"Lasso", 4);
    add(B_RECT, L"Rectangle", 14);
    add(B_INVERT, L"Invert", 14);
    add(B_SIZE, SizeLabel(w), 14);
    add(B_RESET, L"Start over", 14);
    const int saveW = width(L"Save") + w.Px(16), copyW = width(L"Copy");
    int rx = rc.right - m - saveW;
    w.btns.push_back({B_SAVE, L"Save", {rx, by, rx + saveW, by + bh}});
    rx -= w.Px(6) + copyW;
    w.btns.push_back({B_COPY, L"Copy", {rx, by, rx + copyW, by + bh}});
    SelectObject(dc, old);
    ReleaseDC(w.hwnd, dc);

    if (w.back.w != rc.right || w.back.h != rc.bottom) w.back.Create(std::max<LONG>(1, rc.right), std::max<LONG>(1, rc.bottom));
}

int MinWidth(Win& w) {
    if (w.btns.empty()) return w.Px(900);
    int sum = 0;
    for (const Btn& b : w.btns) sum += b.r.right - b.r.left;
    return sum + w.Px(4 + 14 + 14 + 14 + 14 + 6 + 2 * kGap + 30);
}

void Fill(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

POINT ToScreen(const Win& w, float ix, float iy) {
    return {w.org.x + static_cast<LONG>(std::lround(ix * w.zoom)), w.org.y + static_cast<LONG>(std::lround(iy * w.zoom))};
}

POINT ToImg(const Win& w, int x, int y) {
    return {static_cast<LONG>(std::floor((x - w.org.x) / w.zoom)), static_cast<LONG>(std::floor((y - w.org.y) / w.zoom))};
}

// ---- levels bar ----

int LevX(const Win& w, float v) { return w.levR.left + static_cast<int>(std::lround(v * (w.levR.right - w.levR.left))); }

float MidPos(const Params& p) { return p.black + (p.white - p.black) * std::pow(0.5f, p.gamma); }

void PaintLevels(Win& w, HDC b) {
    const RECT L = w.levR;
    const int W = L.right - L.left;
    if (W < 20) return;
    const RECT hr{L.left, L.top, L.right, L.top + w.Px(kHistH)};
    Fill(b, hr, Pal().hist);
    for (int i = 0; i < kHistBins; ++i) {
        const int x0 = L.left + i * W / kHistBins, x1 = L.left + (i + 1) * W / kHistBins;
        const int h = static_cast<int>(std::lround(w.score.hist[i] * (hr.bottom - hr.top - w.Px(4))));
        if (h > 0) Fill(b, {x0, hr.bottom - h, std::max(x0 + 1, x1 - 1), hr.bottom}, Pal().histBar);
    }
    // the curve: what each score turns into
    std::vector<POINT> curve;
    for (int x = 0; x <= W; x += 2)
        curve.push_back({L.left + x, hr.bottom - 1 - static_cast<LONG>(std::lround(Levels(static_cast<float>(x) / W, w.p) *
                                                                                   (hr.bottom - hr.top - 3)))});
    HPEN pen = CreatePen(PS_SOLID, std::max(1, w.Px(2)), kAccent);
    HGDIOBJ oldPen = SelectObject(b, pen);
    Polyline(b, curve.data(), static_cast<int>(curve.size()));
    SelectObject(b, oldPen);
    DeleteObject(pen);
    // black-to-white strip
    const int sy = hr.bottom, sb = sy + w.Px(kStripH);
    TRIVERTEX tv[2] = {{L.left, sy, 0, 0, 0, 0}, {L.right, sb, 0xFF00, 0xFF00, 0xFF00, 0}};
    GRADIENT_RECT gr{0, 1};
    GradientFill(b, tv, 2, &gr, 1, GRADIENT_FILL_RECT_H);
    // handles
    const float pos[3] = {w.p.black, MidPos(w.p), w.p.white};
    const COLORREF fill[3] = {RGB(0, 0, 0), RGB(128, 128, 128), RGB(255, 255, 255)};
    HPEN edge = CreatePen(PS_SOLID, 1, RGB(170, 170, 170));
    oldPen = SelectObject(b, edge);
    HGDIOBJ oldBrush = SelectObject(b, GetStockObject(DC_BRUSH));
    for (int i = 0; i < 3; ++i) {
        const int x = LevX(w, pos[i]), hw = w.Px(6);
        const POINT tri[3] = {{x, sb}, {x - hw, sb + w.Px(kHandleH) - 1}, {x + hw, sb + w.Px(kHandleH) - 1}};
        SetDCBrushColor(b, fill[i]);
        Polygon(b, tri, 3);
    }
    SelectObject(b, oldBrush);
    SelectObject(b, oldPen);
    DeleteObject(edge);
    wchar_t t[160];
    swprintf_s(t, L"Levels   black %.2f   mid %.2f   white %.2f      double-click resets", w.p.black, w.p.gamma, w.p.white);
    RECT tr{L.left, L.bottom + w.Px(2), L.right, L.bottom + w.Px(22)};
    SetTextColor(b, Pal().status);
    DrawTextW(b, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
}

bool InLevels(const Win& w, POINT p) {
    RECT r = w.levR;
    InflateRect(&r, w.Px(8), w.Px(2));
    return PtInRect(&r, p) != 0;
}

// Picks the handle under (or nearest to) x.
int LevHandleAt(const Win& w, int x) {
    const float pos[3] = {w.p.black, MidPos(w.p), w.p.white};
    int best = 0;
    for (int i = 1; i < 3; ++i)
        if (std::abs(LevX(w, pos[i]) - x) < std::abs(LevX(w, pos[best]) - x)) best = i;
    // black and white sitting on each other: drag the one that makes room
    if (best != 1 && LevX(w, w.p.white) - LevX(w, w.p.black) < w.Px(4)) best = x < LevX(w, w.p.black) ? 0 : 2;
    return best;
}

void SetLevel(Win& w, int which, int x) {
    const float v = std::clamp(static_cast<float>(x - w.levR.left) / std::max<LONG>(1, w.levR.right - w.levR.left), 0.f, 1.f);
    if (which == 0) w.p.black = std::min(v, w.p.white - 0.01f);
    else if (which == 2) w.p.white = std::max(v, w.p.black + 0.01f);
    else {
        const float t = std::clamp((v - w.p.black) / (w.p.white - w.p.black), 0.02f, 0.98f);
        w.p.gamma = std::log(t) / std::log(0.5f);
    }
}

// More / less detail: slide both ends (negative = more detail).
void ShiftLevels(Win& w, float d) {
    d = std::clamp(d, -w.p.black, 1 - w.p.white);
    w.p.black += d;
    w.p.white += d;
}

// Sharper (< 1) / softer (> 1) edges: narrow or widen the range around its middle.
void SpreadLevels(Win& w, float f) {
    const float c = (w.p.black + w.p.white) / 2;
    const float half = std::clamp((w.p.white - w.p.black) / 2 * f, 0.005f, 0.5f);
    w.p.black = std::clamp(c - half, 0.f, 0.99f);
    w.p.white = std::clamp(c + half, w.p.black + 0.01f, 1.f);
}

// A closed outline in client coords: dark under a dotted white line.
void Outline(HDC b, const std::vector<POINT>& pts, bool closed) {
    if (pts.size() < 2) return;
    std::vector<POINT> q(pts);
    if (closed) q.push_back(pts.front());
    HPEN dark = CreatePen(PS_SOLID, 3, RGB(0, 0, 0)), light = CreatePen(PS_DOT, 1, RGB(255, 255, 255));
    HGDIOBJ oldPen = SelectObject(b, dark);
    Polyline(b, q.data(), static_cast<int>(q.size()));
    SelectObject(b, light);
    SetBkMode(b, OPAQUE);
    SetBkColor(b, RGB(0, 0, 0));
    Polyline(b, q.data(), static_cast<int>(q.size()));
    SetBkMode(b, TRANSPARENT);
    SelectObject(b, oldPen);
    DeleteObject(dark);
    DeleteObject(light);
}

void Marker(Win& w, HDC b, POINT ip, bool detail) {
    const POINT c = ToScreen(w, ip.x + 0.5f, ip.y + 0.5f);
    const int r = w.Px(6);
    HGDIOBJ oldBrush = SelectObject(b, GetStockObject(NULL_BRUSH));
    HPEN outer = CreatePen(PS_SOLID, w.Px(4), RGB(0, 0, 0)), inner = CreatePen(PS_SOLID, w.Px(2), detail ? RGB(255, 255, 255) : kAccent);
    for (HPEN pen : {outer, inner}) {
        HGDIOBJ old = SelectObject(b, pen);
        if (detail) Ellipse(b, c.x - r, c.y - r, c.x + r, c.y + r);
        else Rectangle(b, c.x - r, c.y - r, c.x + r, c.y + r);
        SelectObject(b, old);
    }
    SelectObject(b, oldBrush);
    DeleteObject(outer);
    DeleteObject(inner);
}

void Paint(Win& w, HDC hdc) {
    HDC b = w.back.dc;
    RECT rc{0, 0, w.back.w, w.back.h};
    Fill(b, rc, Pal().canvas);
    SetBkMode(b, TRANSPARENT);

    // hint
    Fill(b, {0, 0, rc.right, w.Px(kHintH)}, Pal().bar);
    Fill(b, {0, w.Px(kHintH) - 1, rc.right, w.Px(kHintH)}, Pal().barLine);
    const wchar_t* drag = w.tool == T_RECT ? L"Drag a box around the part you want" : L"Drag around the part you want";
    std::wstring hint;
    if (w.p.detail.empty())
        hint = std::wstring(drag) + L" (more add up). Click the detail color, right-click the background. Ctrl+Z undoes.";
    else
        hint = std::to_wstring(w.p.detail.size()) +
               (w.p.detail.size() == 1 ? L" detail color picked." : L" detail colors picked.") +
               L"  Click to add more, right-click the background. " + drag + L".  Ctrl+Z undoes.";
    HGDIOBJ oldFont = SelectObject(b, w.ui);
    SetTextColor(b, Pal().hint);
    RECT hr{w.Px(kGap), 0, rc.right - w.Px(kGap), w.Px(kHintH)};
    DrawTextW(b, hint.c_str(), -1, &hr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);

    // source picture, box and picks
    const RECT imgR{w.org.x, w.org.y, w.org.x + static_cast<LONG>(std::lround(w.img.w * w.zoom)),
                    w.org.y + static_cast<LONG>(std::lround(w.img.h * w.zoom))};
    SetStretchBltMode(b, w.zoom < 1 ? HALFTONE : COLORONCOLOR);
    SetBrushOrgEx(b, 0, 0, nullptr);
    StretchBlt(b, imgR.left, imgR.top, imgR.right - imgR.left, imgR.bottom - imgR.top,
               !w.areas.empty() && w.shaded.dc ? w.shaded.dc : w.src.dc, 0, 0, w.img.w, w.img.h, SRCCOPY);
    for (const auto& poly : w.areas) {
        std::vector<POINT> sp;
        for (POINT p : poly) sp.push_back(ToScreen(w, p.x + 0.5f, p.y + 0.5f));
        Outline(b, sp, true);
    }
    if (w.dragging && w.tool == T_RECT) {
        const POINT a = w.downPt, c = w.curPt;
        Outline(b, {a, {c.x, a.y}, c, {a.x, c.y}}, true);
    } else if (w.dragging) {
        Outline(b, w.path, false);
    }
    for (POINT p : w.detailPts) Marker(w, b, p, true);
    if (w.surfacePt.x >= 0) Marker(w, b, w.surfacePt, false);

    // result
    Fill(b, w.resArea, RGB(0, 0, 0));
    if (w.prev.dc) {
        SetStretchBltMode(b, HALFTONE);
        SetBrushOrgEx(b, 0, 0, nullptr);
        StretchBlt(b, w.resArea.left, w.resArea.top, w.resArea.right - w.resArea.left, w.resArea.bottom - w.resArea.top,
                   w.prev.dc, 0, 0, w.prev.w, w.prev.h, SRCCOPY);
    }
    {
        RECT fr = w.resArea;
        InflateRect(&fr, 1, 1);
        HBRUSH br = CreateSolidBrush(Pal().frame);
        FrameRect(b, &fr, br);
        DeleteObject(br);
    }
    PaintLevels(w, b);
    // the background color in use, under the levels
    {
        const int sw = w.Px(14), y = w.levR.bottom + w.Px(30);
        const uint32_t s = w.surfaceUsed;
        Fill(b, {w.resArea.left, y, w.resArea.left + sw, y + sw}, RGB((s >> 16) & 255, (s >> 8) & 255, s & 255));
        RECT tr{w.resArea.left + sw + w.Px(8), y - w.Px(4), w.resArea.right, y + sw + w.Px(4)};
        SetTextColor(b, Pal().status);
        const wchar_t* t = w.p.hasSurface ? L"Background (picked)" : L"Background (most common color)";
        DrawTextW(b, t, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    }

    // buttons
    Fill(b, {0, rc.bottom - w.Px(kBarH), rc.right, rc.bottom}, Pal().bar);
    Fill(b, {0, rc.bottom - w.Px(kBarH), rc.right, rc.bottom - w.Px(kBarH) + 1}, Pal().barLine);
    SelectObject(b, GetStockObject(NULL_PEN));
    SelectObject(b, GetStockObject(DC_BRUSH));
    for (int i = 0; i < static_cast<int>(w.btns.size()); ++i) {
        const Btn& bt = w.btns[i];
        const bool hov = i == w.hover;
        const bool on = (bt.id == B_INVERT && w.p.invert) || (bt.id == B_LASSO && w.tool == T_LASSO) ||
                        (bt.id == B_RECT && w.tool == T_RECT);
        const Palette& p = Pal();
        COLORREF c = hov ? p.btnHover : p.btn;
        if (bt.id == B_SAVE) c = hov ? RGB(245, 70, 80) : kAccent;
        else if (on) c = hov ? p.btnOnHover : p.btnOn;
        SetDCBrushColor(b, c);
        RoundRect(b, bt.r.left, bt.r.top, bt.r.right + 1, bt.r.bottom + 1, w.Px(8), w.Px(8));
        SelectObject(b, bt.id == B_SAVE ? w.bold : w.ui);
        SetTextColor(b, bt.id == B_SAVE ? RGB(255, 255, 255) : p.btnText);
        RECT tr = bt.r;
        DrawTextW(b, bt.label.c_str(), -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(b, oldFont);
    BitBlt(hdc, 0, 0, rc.right, rc.bottom, b, 0, 0, SRCCOPY);
}

void Redraw(Win& w) { InvalidateRect(w.hwnd, nullptr, FALSE); }

void Shown(Win& w) {
    for (Btn& b : w.btns)
        if (b.id == B_SIZE) b.label = SizeLabel(w);
    Redraw(w);
}

void Changed(Win& w) {  // picks or mask changed
    Recompute(w);
    Shown(w);
}

void LevelsChanged(Win& w) {  // only levels / invert
    Refine(w);
    Shown(w);
}

void LoadChoices(Win& w) {
    wchar_t buf[16] = {};
    GetPrivateProfileStringW(L"Alpha", L"Size", L"auto", buf, 16, SettingsPath().c_str());
    for (int i = 1; i < kSizeCount; ++i)
        if (_wtoi(buf) == kSizes[i]) w.sizeIdx = i;
    GetPrivateProfileStringW(L"Alpha", L"Tool", L"lasso", buf, 16, SettingsPath().c_str());
    w.tool = _wcsicmp(buf, L"rectangle") ? T_LASSO : T_RECT;
}

void SetTool(Win& w, int t) {
    w.tool = t;
    SaveSettingValue(L"Alpha", L"Tool", t == T_RECT ? L"rectangle" : L"lasso");
    Redraw(w);
}

Image Final(const Win& w) { return Scale(w.mask, OutputSize(w.mask, w.sizeIdx)); }

void CopyNow(Win& w) {
    Output_CopyImage(Final(w));
    Tray_Notify(L"Alpha copied", L"Ready to paste.");
}

void SaveNow(Win& w) {
    const Image out = Final(w);
    const std::wstring path = Output_NewPath(w.meta, L" alpha.png");
    if (!WriteFileBytes(path, EncodePng(out))) {
        MessageBoxW(w.hwnd, (L"Could not save\n" + path).c_str(), L"Make alpha", MB_OK | MB_ICONWARNING);
        return;
    }
    Output_CopyImage(out);
    SaveSettingValue(L"Alpha", L"Size", kSizes[w.sizeIdx] ? std::to_wstring(kSizes[w.sizeIdx]) : L"auto");
    Log(L"Alpha saved %dx%d: %s", out.w, out.h, path.c_str());
    Tray_Notify(L"Alpha saved", L"Saved to your screenshots folder and copied.");
    DestroyWindow(w.hwnd);
}

void Press(Win& w, BtnId id) {
    switch (id) {
    case B_LASSO: SetTool(w, T_LASSO); return;
    case B_RECT: SetTool(w, T_RECT); return;
    case B_INVERT:
        w.p.invert = !w.p.invert;
        LevelsChanged(w);
        return;
    case B_SIZE:
        w.sizeIdx = (w.sizeIdx + 1) % kSizeCount;
        Shown(w);
        return;
    case B_RESET: {
        const int keep = w.sizeIdx;
        w.p = Params{};
        w.detailPts.clear();
        w.surfacePt = {-1, -1};
        w.areas.clear();
        w.history.clear();
        RebuildArea(w);
        w.sizeIdx = keep;
        break;
    }
    case B_COPY: CopyNow(w); return;
    case B_SAVE: SaveNow(w); return;
    default: return;
    }
    Changed(w);
}

int BtnAt(const Win& w, int x, int y) {
    const POINT p{x, y};
    for (int i = 0; i < static_cast<int>(w.btns.size()); ++i)
        if (PtInRect(&w.btns[i].r, p)) return i;
    return -1;
}

bool InPicture(const Win& w, POINT ip) { return ip.x >= 0 && ip.y >= 0 && ip.x < w.img.w && ip.y < w.img.h; }

void OnLeftUp(Win& w) {
    if (!w.down) return;
    w.down = false;
    ReleaseCapture();
    if (w.dragging) {
        w.dragging = false;
        auto toImg = [&](POINT c) {
            POINT q = ToImg(w, c.x, c.y);
            q.x = std::clamp<LONG>(q.x, 0, w.img.w - 1);
            q.y = std::clamp<LONG>(q.y, 0, w.img.h - 1);
            return q;
        };
        std::vector<POINT> poly;
        if (w.tool == T_RECT) {
            const POINT a = toImg(w.downPt), c = toImg(w.curPt);
            const LONG x0 = std::min(a.x, c.x), x1 = std::max(a.x, c.x), y0 = std::min(a.y, c.y), y1 = std::max(a.y, c.y) + 1;
            if (x1 > x0 && y1 - y0 > 1) poly = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y1}};
        } else {
            for (POINT c : w.path) {
                const POINT q = toImg(c);
                if (poly.empty() || q.x != poly.back().x || q.y != poly.back().y) poly.push_back(q);
            }
        }
        w.path.clear();
        if (poly.size() >= 3) {
            w.areas.push_back(std::move(poly));
            w.history.push_back('a');
            RebuildArea(w);
        }
        Changed(w);
        return;
    }
    const POINT ip = ToImg(w, w.downPt.x, w.downPt.y);
    if (!InPicture(w, ip)) return;
    if (static_cast<int>(w.p.detail.size()) >= kMaxPicks) {
        w.p.detail.erase(w.p.detail.begin());
        w.detailPts.erase(w.detailPts.begin());
        w.history.erase(std::find(w.history.begin(), w.history.end(), 'p'));
    }
    w.p.detail.push_back(AverageAt(w.img, ip.x, ip.y));
    w.detailPts.push_back(ip);
    w.history.push_back('p');
    Changed(w);
}

void OnKey(Win& w, UINT vk) {
    const bool ctrl = GetKeyState(VK_CONTROL) < 0;
    if (ctrl && vk == 'C') CopyNow(w);
    else if (ctrl && vk == 'S') SaveNow(w);
    else if (ctrl && vk == 'Z') {
        if (!w.history.empty()) {
            if (w.history.back() == 'p') {
                w.p.detail.pop_back();
                w.detailPts.pop_back();
            } else {
                w.areas.pop_back();
                RebuildArea(w);
            }
            w.history.pop_back();
            Changed(w);
        }
    } else if (vk == VK_RETURN) SaveNow(w);
    else if (vk == VK_ESCAPE) DestroyWindow(w.hwnd);
    else if (vk == VK_BACK || vk == VK_DELETE) Press(w, B_RESET);
    else if (vk == 'I') Press(w, B_INVERT);
    else if (vk == 'L') SetTool(w, T_LASSO);
    else if (vk == 'R') SetTool(w, T_RECT);
    else if (vk == VK_OEM_PLUS || vk == VK_ADD) {
        ShiftLevels(w, -0.03f);
        LevelsChanged(w);
    } else if (vk == VK_OEM_MINUS || vk == VK_SUBTRACT) {
        ShiftLevels(w, 0.03f);
        LevelsChanged(w);
    } else if (vk == VK_OEM_4) {
        SpreadLevels(w, 1 / 1.4f);
        LevelsChanged(w);
    } else if (vk == VK_OEM_6) {
        SpreadLevels(w, 1.4f);
        LevelsChanged(w);
    }
}

const wchar_t* Tip(BtnId id) {
    switch (id) {
    case B_LASSO: return L"Lasso: drag freely around the part you want  (L)";
    case B_RECT: return L"Rectangle: drag a box around the part you want  (R)";
    case B_INVERT: return L"Swap white and black  (I)";
    case B_SIZE: return L"Output size; auto = the smallest power of two that keeps every pixel";
    case B_RESET: return L"Forget the picks and the mask  (Backspace)";
    case B_COPY: return L"Copy the alpha  (Ctrl+C)";
    case B_SAVE: return L"Save to the screenshots folder and copy  (Enter)";
    default: return L"";
    }
}

HWND g_tip = nullptr;

void UpdateTip(Win& w) {
    if (!g_tip) return;
    TOOLINFOW ti{sizeof(ti)};
    ti.hwnd = w.hwnd;
    ti.uId = 1;
    ti.lpszText = const_cast<wchar_t*>(w.hover >= 0 ? Tip(w.btns[w.hover].id) : L"");
    if (w.hover >= 0) ti.rect = w.btns[w.hover].r;
    SendMessageW(g_tip, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
    SendMessageW(g_tip, TTM_UPDATETIPTEXTW, 0, reinterpret_cast<LPARAM>(&ti));
}

LRESULT CALLBACK Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    Win* wp_ = reinterpret_cast<Win*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        wp_ = static_cast<Win*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        wp_->hwnd = h;
        SetWindowLongPtrW(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(wp_));
    }
    if (!wp_) return DefWindowProcW(h, msg, wp, lp);
    Win& w = *wp_;
    switch (msg) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        if (w.back.dc) Paint(w, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_SIZE:
        Layout(w);
        Redraw(w);
        return 0;
    case WM_GETMINMAXINFO: {
        RECT r{0, 0, MinWidth(w), w.Px(kHintH + kBarH + kUnderRes + 200)};
        AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, w.dpi);
        reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_DPICHANGED: {
        w.dpi = HIWORD(wp);
        MakeFonts(w);
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(w);
        Redraw(w);
        return 0;
    }
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            POINT p;
            GetCursorPos(&p);
            ScreenToClient(h, &p);
            const wchar_t* cur = IDC_ARROW;
            if (BtnAt(w, p.x, p.y) >= 0) cur = IDC_HAND;
            else if (w.levDrag >= 0 || InLevels(w, p)) cur = IDC_SIZEWE;
            else if (w.down || InPicture(w, ToImg(w, p.x, p.y))) cur = IDC_CROSS;
            SetCursor(LoadCursorW(nullptr, cur));
            return TRUE;
        }
        break;
    case WM_LBUTTONDBLCLK:
        if (InLevels(w, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)})) {  // reset levels
            const Params d;
            w.p.black = d.black;
            w.p.white = d.white;
            w.p.gamma = d.gamma;
            LevelsChanged(w);
            return 0;
        }
        [[fallthrough]];  // elsewhere a double-click is two clicks
    case WM_LBUTTONDOWN: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        const int bi = BtnAt(w, x, y);
        if (bi >= 0) {
            Press(w, w.btns[bi].id);
            return 0;
        }
        if (InLevels(w, POINT{x, y})) {
            w.levDrag = LevHandleAt(w, x);
            SetCapture(h);
            SetLevel(w, w.levDrag, x);
            LevelsChanged(w);
            return 0;
        }
        if (!PtInRect(&w.srcArea, POINT{x, y})) return 0;
        w.down = true;
        w.dragging = false;
        w.downPt = w.curPt = {x, y};
        w.path.assign(1, POINT{x, y});
        SetCapture(h);
        return 0;
    }
    case WM_MOUSEMOVE: {
        const int x = GET_X_LPARAM(lp), y = GET_Y_LPARAM(lp);
        if (w.levDrag >= 0) {
            SetLevel(w, w.levDrag, x);
            LevelsChanged(w);
            return 0;
        }
        if (w.down) {
            w.curPt = {x, y};
            if (!w.dragging && (std::abs(x - w.downPt.x) > w.Px(4) || std::abs(y - w.downPt.y) > w.Px(4))) w.dragging = true;
            if (w.dragging) {
                w.path.push_back({x, y});
                Redraw(w);
            }
        }
        const int hv = BtnAt(w, x, y);
        if (hv != w.hover) {
            w.hover = hv;
            UpdateTip(w);
            Redraw(w);
        }
        TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
        TrackMouseEvent(&tme);
        return 0;
    }
    case WM_MOUSELEAVE:
        if (w.hover >= 0) {
            w.hover = -1;
            Redraw(w);
        }
        return 0;
    case WM_LBUTTONUP:
        if (w.levDrag >= 0) {
            w.levDrag = -1;
            ReleaseCapture();
            return 0;
        }
        OnLeftUp(w);
        return 0;
    case WM_CAPTURECHANGED:
        w.levDrag = -1;
        if (w.down) {
            w.down = w.dragging = false;
            Redraw(w);
        }
        return 0;
    case WM_RBUTTONUP: {  // right-click: this is the background
        if (w.down) return 0;
        const POINT ip = ToImg(w, GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (!PtInRect(&w.srcArea, POINT{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)}) || !InPicture(w, ip)) return 0;
        w.p.hasSurface = true;
        w.p.surface = AverageAt(w.img, ip.x, ip.y);
        w.surfacePt = ip;
        Changed(w);
        return 0;
    }
    case WM_MOUSEWHEEL:  // up = more detail
        ShiftLevels(w, GET_WHEEL_DELTA_WPARAM(wp) > 0 ? -0.03f : 0.03f);
        LevelsChanged(w);
        return 0;
    case WM_KEYDOWN:
        OnKey(w, static_cast<UINT>(wp));
        return 0;
    case WM_CLOSE:
        DestroyWindow(h);
        return 0;
    case WM_NCDESTROY: {
        for (HFONT f : {w.ui, w.bold})
            if (f) DeleteObject(f);
        if (g_tip) {
            DestroyWindow(g_tip);
            g_tip = nullptr;
        }
        SetWindowLongPtrW(h, GWLP_USERDATA, 0);
        if (g_win == wp_) g_win = nullptr;
        delete wp_;
        if (!g_mainWnd) PostQuitMessage(0);
        return DefWindowProcW(h, msg, wp, lp);
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

void Alpha_Open(const Image& img, const CaptureResult& meta) {
    if (img.w <= 0 || img.h <= 0) return;
    if (g_win) DestroyWindow(g_win->hwnd);  // one at a time
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = Proc;
        wc.hInstance = g_hinst;
        wc.lpszClassName = kClass;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
        registered = RegisterClassExW(&wc) != 0;
    }
    auto* w = new Win;
    w->img = img;
    w->meta = meta;
    w->meta.img = {};
    if (!w->src.Create(img.w, img.h)) {
        Log(L"Alpha: out of memory for %d x %d", img.w, img.h);
        delete w;
        return;
    }
    memcpy(w->src.bits, img.px.data(), static_cast<size_t>(img.w) * img.h * 4);
    LoadChoices(*w);
    Recompute(*w);

    POINT cur;
    GetCursorPos(&cur);
    HMONITOR mon = MonitorFromPoint(cur, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dx = 96, dy = 96;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy);
    w->dpi = dx;
    MakeFonts(*w);
    const RECT wa = mi.rcWork;
    const int ww = (wa.right - wa.left) * 8 / 10, wh = (wa.bottom - wa.top) * 7 / 10;
    HWND hw = CreateWindowExW(0, kClass, L"Make alpha", WS_OVERLAPPEDWINDOW, wa.left + (wa.right - wa.left - ww) / 2,
                              wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, nullptr, nullptr, g_hinst, w);
    if (!hw) {
        Log(L"Alpha window failed (%lu)", GetLastError());
        delete w;
        return;
    }
    g_win = w;
    Theme_Window(hw);
    w->dpi = GetDpiForWindow(hw);
    MakeFonts(*w);
    Layout(*w);
    g_tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0, 0,
                            hw, nullptr, g_hinst, nullptr);
    if (g_tip) {
        TOOLINFOW ti{sizeof(ti)};
        ti.uFlags = TTF_SUBCLASS;
        ti.hwnd = hw;
        ti.uId = 1;
        ti.lpszText = const_cast<wchar_t*>(L"");
        SendMessageW(g_tip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
        SetWindowTheme(g_tip, Theme_Dark() ? L"DarkMode_Explorer" : nullptr, nullptr);
    }
    ShowWindow(hw, SW_SHOWNORMAL);
    SetForegroundWindow(hw);
    Log(L"Alpha opened %d x %d", img.w, img.h);
}

HWND Alpha_Window() { return g_win ? g_win->hwnd : nullptr; }

int Alpha_Bench(const std::wstring& in, const std::wstring& out, const std::vector<POINT>& picks, const RECT& box) {
    Image img;
    if (!LoadImageFile(in, img)) return 2;
    Params p;
    if (!IsRectEmpty(&box)) {
        const RECT b = ClampBox(img, box);
        p.box = b;
        p.area.assign(static_cast<size_t>(img.w) * img.h, 0);
        for (int y = b.top; y < b.bottom; ++y) std::fill_n(&p.area[static_cast<size_t>(y) * img.w + b.left], b.right - b.left, uint8_t{1});
    }
    for (POINT pt : picks) p.detail.push_back(AverageAt(img, pt.x, pt.y));
    const ULONGLONG t0 = GetTickCount64();
    uint32_t surf = 0;
    const Mask m = Compute(img, p, &surf);
    const ULONGLONG t1 = GetTickCount64();
    const Image o = Scale(m, OutputSize(m, 0));
    const ULONGLONG t2 = GetTickCount64();
    if (!WriteFileBytes(out, EncodePng(o))) return 3;
    Log(L"--alpha-bench %dx%d: mask %llu ms, scale to %d %llu ms, background %06x", img.w, img.h, t1 - t0, o.w, t2 - t1,
        surf);
    return 0;
}
