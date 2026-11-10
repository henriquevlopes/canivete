// Windows 11 (Fluent) look: fonts, window frame, accent color and shapes (see fluent.h).
#include "app.h"
#include "fluent.h"
#include "theme.h"

#include <dwmapi.h>
#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace {

// DWMWINDOWATTRIBUTE values newer than some SDKs.
constexpr DWORD kDwmCorner = 33, kDwmBorderColor = 34, kDwmCaptionColor = 35, kDwmTextColor = 36,
                kDwmBackdrop = 38;
constexpr DWORD kCornerRound = 2, kCornerRoundSmall = 3;
constexpr DWORD kBackdropNone = 1, kBackdropMica = 2;
constexpr COLORREF kDwmDefault = 0xFFFFFFFF;

DWORD Build() {
    static DWORD build = [] {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
        OSVERSIONINFOW v{sizeof(v)};
        if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
            f(&v);
        return v.dwMajorVersion >= 10 ? v.dwBuildNumber : 0UL;
    }();
    return build;
}

std::mutex g_fontMx;  // fonts may be asked for from the worker that draws recordings

int CALLBACK FaceFound(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
    *reinterpret_cast<bool*>(lp) = true;
    return 0;
}

bool HasFace(const std::wstring& face) {
    static std::map<std::wstring, bool> known;
    auto it = known.find(face);
    if (it != known.end()) return it->second;
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
    bool found = false;
    HDC dc = GetDC(nullptr);
    EnumFontFamiliesExW(dc, &lf, FaceFound, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    known[face] = found;
    return found;
}

// Segoe UI Variable picks its optical size by name; its semibold instance is a family of its own
// for GDI (names cut at 31 characters, as GDI lists them).
std::wstring FaceFor(int px, bool semibold, LONG& weight) {
    const wchar_t* size = px <= 12 ? L"Small" : px < 20 ? L"Text" : L"Display";
    std::wstring face = std::wstring(L"Segoe UI Variable ") + size;
    weight = semibold ? FW_SEMIBOLD : FW_NORMAL;
    if (semibold) {
        std::wstring sb = (face + L" Semibold").substr(0, LF_FACESIZE - 1);
        if (HasFace(sb)) return sb;
    }
    if (HasFace(face)) return face;
    return L"Segoe UI";
}

Gdiplus::Color Gp(COLORREF c, BYTE a = 255) { return Gdiplus::Color(a, GetRValue(c), GetGValue(c), GetBValue(c)); }

bool GdipReady() {
    Gdiplus_Ensure();
    return true;
}

struct Smooth {
    bool on = GdipReady();
    Gdiplus::Graphics gr;
    Gdiplus::Graphics* g = &gr;
    explicit Smooth(HDC dc) : gr(dc) {
        gr.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        gr.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    }
    Smooth(const Smooth&) = delete;
    Smooth& operator=(const Smooth&) = delete;
};

void AddRound(Gdiplus::GraphicsPath& p, Gdiplus::RectF r, float radius) {
    radius = std::min({radius, r.Width / 2, r.Height / 2});
    if (radius < 0.01f) {
        p.AddRectangle(r);
        return;
    }
    const float d = radius * 2;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
}

Gdiplus::RectF RF(const RECT& r) {
    return Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                          static_cast<float>(r.bottom - r.top));
}

void FillRound(Gdiplus::Graphics& g, Gdiplus::RectF r, float radius, COLORREF fill, COLORREF stroke, float sw) {
    if (stroke != CLR_INVALID && sw > 0) {
        r.X += sw / 2;
        r.Y += sw / 2;
        r.Width -= sw;
        r.Height -= sw;
        radius = std::max(0.0f, radius - sw / 2);
    }
    if (r.Width <= 0 || r.Height <= 0) return;
    Gdiplus::GraphicsPath p;
    AddRound(p, r, radius);
    if (fill != CLR_INVALID) {
        Gdiplus::SolidBrush b(Gp(fill));
        g.FillPath(&b, &p);
    }
    if (stroke != CLR_INVALID && sw > 0) {
        Gdiplus::Pen pen(Gp(stroke), sw);
        g.DrawPath(&pen, &p);
    }
}

COLORREF Mix(COLORREF a, COLORREF b, float t) {  // a..b
    auto ch = [&](int x, int y) { return static_cast<BYTE>(x + (y - x) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

}  // namespace

int Fluent_Px(UINT dpi, int v) { return MulDiv(v, static_cast<int>(dpi ? dpi : 96), 96); }

bool Fluent_IsWin11() { return Build() >= 22000; }

bool Fluent_HighContrast() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool Fluent_Animations() {
    BOOL on = TRUE;
    SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &on, 0);
    return on != FALSE;
}

HFONT Fluent_FontPx(int px, bool semibold, UINT dpi) {
    if (!dpi) dpi = 96;
    std::lock_guard<std::mutex> lock(g_fontMx);
    static std::map<std::tuple<int, bool, UINT>, HFONT> cache;
    HFONT& f = cache[{px, semibold, dpi}];
    if (!f) {
        LOGFONTW lf{};
        const std::wstring face = FaceFor(px, semibold, lf.lfWeight);
        wcsncpy_s(lf.lfFaceName, face.c_str(), _TRUNCATE);
        lf.lfHeight = -MulDiv(px, static_cast<int>(dpi), 96);
        lf.lfCharSet = DEFAULT_CHARSET;
        lf.lfQuality = CLEARTYPE_QUALITY;
        lf.lfOutPrecision = OUT_TT_PRECIS;
        f = CreateFontIndirectW(&lf);
    }
    return f;
}

HFONT Fluent_Font(FluentType type, UINT dpi) {
    switch (type) {
    case kTypeCaption: return Fluent_FontPx(12, false, dpi);
    case kTypeBody: return Fluent_FontPx(14, false, dpi);
    case kTypeBodyStrong: return Fluent_FontPx(14, true, dpi);
    case kTypeBodyLarge: return Fluent_FontPx(18, false, dpi);
    case kTypeSubtitle: return Fluent_FontPx(20, true, dpi);
    case kTypeTitle: return Fluent_FontPx(28, true, dpi);
    }
    return Fluent_FontPx(14, false, dpi);
}

const wchar_t* Fluent_IconFace() {
    static const bool fluent = [] {
        std::lock_guard<std::mutex> lock(g_fontMx);
        return HasFace(L"Segoe Fluent Icons");
    }();
    return fluent ? L"Segoe Fluent Icons" : L"Segoe MDL2 Assets";
}

HFONT Fluent_IconFont(int px, UINT dpi) {
    if (!dpi) dpi = 96;
    const wchar_t* face = Fluent_IconFace();
    std::lock_guard<std::mutex> lock(g_fontMx);
    static std::map<std::pair<int, UINT>, HFONT> cache;
    HFONT& f = cache[{px, dpi}];
    if (!f)
        f = CreateFontW(-MulDiv(px, static_cast<int>(dpi), 96), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                        OUT_TT_PRECIS, 0, CLEARTYPE_QUALITY, 0, face);
    return f;
}

void Fluent_StyleWindow(HWND top) {
    if (!top || !Fluent_IsWin11()) return;
    const Palette& p = Pal();
    if (p.highContrast) {
        const DWORD none = kBackdropNone;
        if (Build() >= 22621) DwmSetWindowAttribute(top, kDwmBackdrop, &none, sizeof(none));
        const COLORREF def = kDwmDefault;
        DwmSetWindowAttribute(top, kDwmCaptionColor, &def, sizeof(def));
        DwmSetWindowAttribute(top, kDwmTextColor, &def, sizeof(def));
        return;
    }
    const DWORD corner = kCornerRound;
    DwmSetWindowAttribute(top, kDwmCorner, &corner, sizeof(corner));
    if (Build() >= 22621) {
        // Mica in the title bar; the client area stays opaque in the window color.
        const DWORD mica = kBackdropMica;
        DwmSetWindowAttribute(top, kDwmBackdrop, &mica, sizeof(mica));
    } else {
        const COLORREF cap = p.base, txt = p.text;
        DwmSetWindowAttribute(top, kDwmCaptionColor, &cap, sizeof(cap));
        DwmSetWindowAttribute(top, kDwmTextColor, &txt, sizeof(txt));
    }
}

void Fluent_StylePopup(HWND popup, bool compact) {
    if (!popup || !Fluent_IsWin11()) return;
    const Palette& p = Pal();
    const DWORD corner = compact ? kCornerRoundSmall : kCornerRound;
    DwmSetWindowAttribute(popup, kDwmCorner, &corner, sizeof(corner));
    const COLORREF border = p.highContrast ? kDwmDefault : p.cardStroke;
    DwmSetWindowAttribute(popup, kDwmBorderColor, &border, sizeof(border));
}

FluentAccent Fluent_ReadAccent() {
    // Windows' default blue (#0078D4) when the registry has nothing.
    FluentAccent a{RGB(0x99, 0xEB, 0xFF), RGB(0x4C, 0xC2, 0xFF), RGB(0x00, 0x91, 0xF8), RGB(0x00, 0x78, 0xD4),
                   RGB(0x00, 0x5F, 0xB8), RGB(0x00, 0x3E, 0x92), RGB(0x00, 0x1A, 0x68)};
    BYTE pal[32];
    DWORD size = sizeof(pal);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Accent",
                     L"AccentPalette", RRF_RT_REG_BINARY, nullptr, pal, &size) == ERROR_SUCCESS &&
        size >= 28) {
        // 8 RGBA entries: light 3, light 2, light 1, the accent, dark 1, dark 2, dark 3, (unused).
        auto c = [&](int i) { return RGB(pal[i * 4], pal[i * 4 + 1], pal[i * 4 + 2]); };
        a = {c(0), c(1), c(2), c(3), c(4), c(5), c(6)};
    }
    return a;
}

// ---- drawing ---------------------------------------------------------------------------------

void Fluent_RoundRect(HDC dc, const RECT& r, float radius, COLORREF fill, COLORREF stroke, float strokeW) {
    Smooth s(dc);
    FillRound(*s.g, RF(r), radius, fill, stroke, stroke == CLR_INVALID ? 0.0f : strokeW);
}

void Fluent_DrawCard(HDC dc, const RECT& r, UINT dpi, int radius) {
    const Palette& p = Pal();
    Fluent_RoundRect(dc, r, static_cast<float>(Fluent_Px(dpi, radius)), p.card, p.cardStroke,
                     std::max(1.0f, dpi / 96.0f * 0.75f));
}

void Fluent_DrawTextBox(HDC dc, const RECT& r, UINT dpi, bool focused, bool enabled, COLORREF fill) {
    const Palette& p = Pal();
    const float rad = static_cast<float>(Fluent_Px(dpi, 4));
    const float one = std::max(1.0f, static_cast<float>(Fluent_Px(dpi, 1)));
    if (fill == CLR_INVALID) fill = !enabled ? p.ctlDisabled : focused ? p.ctlFocus : p.ctlFill;
    Smooth s(dc);
    FillRound(*s.g, RF(r), rad, fill, p.ctlStroke, one);
    if (!enabled) return;
    // The bottom line, clipped to the rounded shape.
    Gdiplus::GraphicsPath clip;
    AddRound(clip, RF(r), rad);
    s.g->SetClip(&clip);
    const float h = focused ? 2 * one : one;
    Gdiplus::SolidBrush b(Gp(focused ? p.accent : p.ctlStrong));  // Windows 11 text boxes: a strong bottom line
    s.g->FillRectangle(&b, static_cast<float>(r.left), static_cast<float>(r.bottom) - h,
                       static_cast<float>(r.right - r.left), h);
    s.g->ResetClip();
}

void Fluent_DrawNavItem(HDC dc, const RECT& item, UINT dpi, bool selected, bool hot, bool focus, COLORREF bg) {
    const Palette& p = Pal();
    SetDCBrushColor(dc, bg);
    FillRect(dc, &item, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    RECT b = item;
    InflateRect(&b, -Fluent_Px(dpi, 4), -Fluent_Px(dpi, 2));
    {
    Smooth s(dc);
    if (selected || hot) {
        const COLORREF fill = p.highContrast ? (selected ? p.accent : p.ctlFill)
                                             : selected && hot ? p.subtlePressed : p.subtleHover;
        FillRound(*s.g, RF(b), static_cast<float>(Fluent_Px(dpi, 4)), fill, CLR_INVALID, 0);
    }
    if (selected) {
        // The accent indicator: 3 x 16, rounded ends, on the left edge.
        const float w = static_cast<float>(Fluent_Px(dpi, 3)), h = static_cast<float>(Fluent_Px(dpi, 16));
        const float y = (b.top + b.bottom - h) / 2.0f;
        FillRound(*s.g, Gdiplus::RectF(static_cast<float>(b.left), y, w, h), w / 2, p.highContrast ? p.onAccent : p.accent,
                  CLR_INVALID, 0);
    }
    }
    if (focus) Fluent_DrawFocus(dc, b, dpi, 4);
}

SIZE Fluent_SwitchSize(UINT dpi) { return {Fluent_Px(dpi, 40), Fluent_Px(dpi, 20)}; }

void Fluent_DrawSwitch(HDC dc, const RECT& track, UINT dpi, bool on, bool hot, bool enabled) {
    const Palette& p = Pal();
    Smooth s(dc);
    const Gdiplus::RectF r = RF(track);
    const float h = r.Height;
    const float one = std::max(1.0f, dpi / 96.0f);
    if (on) {
        const COLORREF fill = !enabled ? p.accentDis : hot ? p.accentHover : p.accent;
        FillRound(*s.g, r, h / 2, fill, CLR_INVALID, 0);
    } else {
        const COLORREF fill = hot && enabled ? p.subtleHover : CLR_INVALID;
        FillRound(*s.g, r, h / 2, fill == CLR_INVALID ? p.ctlFill : fill, enabled ? p.ctlStrong : p.textDis, one);
    }
    // Knob: 12 px, 14 when hovered.
    const float k = static_cast<float>(Fluent_Px(dpi, hot && enabled ? 14 : 12));
    const float pad = (h - k) / 2;
    const float inset = static_cast<float>(Fluent_Px(dpi, 4)) - (k - Fluent_Px(dpi, 12)) / 2;
    const float kx = on ? r.X + r.Width - inset - k : r.X + inset;
    const COLORREF knob = on ? (enabled ? p.onAccent : p.onAccentDis) : (enabled ? p.text2 : p.textDis);
    Gdiplus::SolidBrush kb(Gp(knob));
    s.g->FillEllipse(&kb, kx, r.Y + pad, k, k);
}

void Fluent_DrawButton(HDC dc, const RECT& r, UINT dpi, const wchar_t* text, HFONT font, bool accent, bool hot,
                       bool pressed, bool enabled, bool focus, UINT textFlags) {
    const Palette& p = Pal();
    const float rad = static_cast<float>(Fluent_Px(dpi, 4));
    const float one = std::max(1.0f, static_cast<float>(Fluent_Px(dpi, 1)));
    COLORREF fill, stroke, bottom, fg;
    if (p.highContrast) {
        fill = hot ? p.accent : p.ctlFill;
        stroke = bottom = p.text;
        fg = hot ? p.onAccent : enabled ? p.text : p.textDis;
    } else if (accent && enabled) {
        fill = pressed ? p.accentPressed : hot ? p.accentHover : p.accent;
        stroke = fill;
        bottom = pressed ? fill : Mix(fill, p.dark ? RGB(0, 0, 0) : RGB(0, 0, 0), 0.18f);
        fg = p.onAccent;
    } else if (accent) {
        fill = stroke = bottom = p.accentDis;
        fg = p.onAccentDis;
    } else {
        fill = !enabled ? p.ctlDisabled : pressed ? p.ctlPressed : hot ? p.ctlHover : p.ctlFill;
        stroke = p.ctlStroke;
        bottom = pressed || !enabled ? p.ctlStroke : p.ctlStrokeBottom;
        fg = !enabled ? p.textDis : pressed ? p.text2 : p.text;
    }
    {
        Smooth s(dc);
        FillRound(*s.g, RF(r), rad, fill, stroke, one);
        if (bottom != stroke) {
            Gdiplus::GraphicsPath clip;
            AddRound(clip, RF(r), rad);
            s.g->SetClip(&clip);
            Gdiplus::SolidBrush b(Gp(bottom));
            s.g->FillRectangle(&b, static_cast<float>(r.left), static_cast<float>(r.bottom) - one,
                               static_cast<float>(r.right - r.left), one);
            s.g->ResetClip();
        }
    }
    if (text && *text) {
        HGDIOBJ old = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, fg);
        RECT tr = r;
        InflateRect(&tr, -Fluent_Px(dpi, 8), 0);
        DrawTextW(dc, text, -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | textFlags);
        SelectObject(dc, old);
    }
    if (focus) {
        RECT fr = r;
        InflateRect(&fr, Fluent_Px(dpi, 3), Fluent_Px(dpi, 3));
        Fluent_DrawFocus(dc, fr, dpi, 7);
    }
}

void Fluent_DrawProgress(HDC dc, const RECT& r, UINT dpi, int pos, ULONGLONG phase, COLORREF fill, COLORREF bg) {
    const Palette& p = Pal();
    if (fill == CLR_INVALID) fill = p.accent;
    if (bg != CLR_INVALID) {
        SetDCBrushColor(dc, bg);
        FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    Smooth s(dc);
    const float cy = (r.top + r.bottom) / 2.0f;
    const float barH = static_cast<float>(std::max(3, Fluent_Px(dpi, 3)));
    const float trackH = std::max(1.0f, static_cast<float>(Fluent_Px(dpi, 1)));
    const float x0 = static_cast<float>(r.left), w = static_cast<float>(r.right - r.left);
    Gdiplus::SolidBrush tb(Gp(p.ctlStrong));
    s.g->FillRectangle(&tb, x0 + barH / 2, cy - trackH / 2, std::max(0.0f, w - barH), trackH);
    const Gdiplus::RectF lane(x0, cy - barH / 2, w, barH);
    if (pos < 0) {
        // No number known: a segment slides along (one pass every 2 s).
        const float seg = w * 0.4f, span = w + seg;
        const float x = static_cast<float>(phase % 2000) / 2000.0f * span - seg;
        Gdiplus::GraphicsPath lp;
        AddRound(lp, lane, barH / 2);
        s.g->SetClip(&lp);
        FillRound(*s.g, Gdiplus::RectF(x0 + x, lane.Y, seg, barH), barH / 2, fill, CLR_INVALID, 0);
        s.g->ResetClip();
    } else if (pos > 0) {
        const float fw = std::max(w * std::min(pos, 1000) / 1000.0f, barH);
        FillRound(*s.g, Gdiplus::RectF(x0, lane.Y, fw, barH), barH / 2, fill, CLR_INVALID, 0);
    }
}

void Fluent_DrawFocus(HDC dc, const RECT& r, UINT dpi, int radius) {
    const Palette& p = Pal();
    Smooth s(dc);
    const float two = static_cast<float>(std::max(2, Fluent_Px(dpi, 2)));
    const float one = std::max(1.0f, static_cast<float>(Fluent_Px(dpi, 1)));
    const float rad = static_cast<float>(Fluent_Px(dpi, radius));
    FillRound(*s.g, RF(r), rad, CLR_INVALID, p.focusOuter, two);
    RECT in = r;
    InflateRect(&in, -static_cast<int>(two), -static_cast<int>(two));
    FillRound(*s.g, RF(in), std::max(0.0f, rad - two), CLR_INVALID, p.focusInner, one);
}

void Fluent_ComboHeight(HWND combo, int height) {
    RECT r;
    GetWindowRect(combo, &r);
    const int item = static_cast<int>(SendMessageW(combo, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
    const int edge = (r.bottom - r.top) - item;  // the frame around the selection field
    if (item > 0 && edge >= 0 && height - edge > 0)
        SendMessageW(combo, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), height - edge);
}

void Fluent_DrawGlyph(HDC dc, const RECT& r, wchar_t glyph, int px, UINT dpi, COLORREF color) {
    HGDIOBJ old = SelectObject(dc, Fluent_IconFont(px, dpi));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    RECT gr = r;
    DrawTextW(dc, &glyph, 1, &gr, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    SelectObject(dc, old);
}
