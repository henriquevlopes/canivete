// Light / dark theme for all windows (see theme.h), with the Windows 11 colors and shapes of fluent.h.
//
// Standard controls go dark through the themes Windows' own Explorer uses (DarkMode_Explorer,
// DarkMode_CFD, DarkMode_ItemsView) plus WM_CTLCOLOR* colors. Push buttons, check boxes, radio buttons
// and command links are painted here the Windows 11 way in both modes (NM_CUSTOMDRAW); High Contrast
// keeps Windows' own drawing and colors.
// Popup menus follow through uxtheme's SetPreferredAppMode / FlushMenuThemes (exports 135 / 136,
// Windows 10 1903 and later; older Windows just keeps light menus).
#include "app.h"
#include "theme.h"
#include "fluent.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <vssym32.h>
#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <map>
#include <mutex>

namespace {

constexpr wchar_t kProp[] = L"Canivete.Themed";

bool g_ready = false, g_dark = false;
DWORD g_uiThread = 0;
Palette g_pal{};
std::map<COLORREF, HBRUSH> g_brushes;

// Undocumented uxtheme exports, by number.
enum AppMode { kAppDefault = 0, kAllowDark = 1, kForceDark = 2, kForceLight = 3 };
using AllowDarkForWindowFn = BOOL(WINAPI*)(HWND, BOOL);
using SetPreferredAppModeFn = int(WINAPI*)(int);
using FlushMenuThemesFn = void(WINAPI*)();
AllowDarkForWindowFn pAllowDarkForWindow = nullptr;
SetPreferredAppModeFn pSetPreferredAppMode = nullptr;
FlushMenuThemesFn pFlushMenuThemes = nullptr;

void LoadApi() {
    static bool done = false;
    if (done) return;
    done = true;
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    OSVERSIONINFOW v{sizeof(v)};
    if (auto f = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")))
        f(&v);
    if (v.dwMajorVersion < 10 || v.dwBuildNumber < 18362) return;  // 135 meant something else before 1903
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    pAllowDarkForWindow = reinterpret_cast<AllowDarkForWindowFn>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    pSetPreferredAppMode = reinterpret_cast<SetPreferredAppModeFn>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    pFlushMenuThemes = reinterpret_cast<FlushMenuThemesFn>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
}

bool SystemDark() {
    DWORD v = 1, size = sizeof(v);
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
                     L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &v, &size) != ERROR_SUCCESS)
        return false;
    return v == 0;
}

bool HighContrast() {
    HIGHCONTRASTW hc{sizeof(hc)};
    return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(hc), &hc, 0) && (hc.dwFlags & HCF_HIGHCONTRASTON);
}

bool WantDark() {
    if (HighContrast()) return false;  // high contrast: Windows' own colors
    if (g_settings.theme == L"dark") return true;
    if (g_settings.theme == L"light") return false;
    return SystemDark();
}

COLORREF Blend(COLORREF a, COLORREF b, float t) {  // a..b
    auto ch = [t](int x, int y) { return static_cast<BYTE>(x + (y - x) * t + 0.5f); };
    return RGB(ch(GetRValue(a), GetRValue(b)), ch(GetGValue(a), GetGValue(b)), ch(GetBValue(a), GetBValue(b)));
}

// Windows' own colors, for High Contrast.
void MakeContrastPalette() {
    Palette& p = g_pal;
    p = Palette{};
    p.dark = false;
    p.highContrast = true;
    const COLORREF win = GetSysColor(COLOR_WINDOW), txt = GetSysColor(COLOR_WINDOWTEXT),
                   btn = GetSysColor(COLOR_BTNFACE), hl = GetSysColor(COLOR_HIGHLIGHT),
                   hlTxt = GetSysColor(COLOR_HIGHLIGHTTEXT), grey = GetSysColor(COLOR_GRAYTEXT);
    p.window = p.base = p.layer = p.card = p.nav = p.footer = p.edit = p.ctlFill = p.ctlDisabled = p.ctlFocus = win;
    p.text = p.text2 = p.text3 = p.grey = p.hint = p.status = p.icon = p.btnText = txt;
    p.textDis = p.iconOff = grey;
    p.red = p.critical = p.caution = p.success = p.statusOk = txt;
    p.navSel = p.subtleHover = p.subtlePressed = p.ctlHover = p.ctlPressed = p.toolSel = p.toolHover = hl;
    p.line = p.divider = p.cardStroke = p.ctlStroke = p.ctlStrokeBottom = p.ctlStrong = p.barLine = p.ring = p.frame =
        txt;
    p.accent = p.accentHover = p.accentPressed = p.accentText = hl;
    p.accentDis = grey;
    p.onAccent = p.onAccentDis = hlTxt;
    p.switchLine = p.switchKnob = txt;
    p.focusBg = hl;
    p.focusOuter = txt;
    p.focusInner = win;
    p.canvas = p.bar = p.hist = win;
    p.histBar = txt;
    p.btn = btn;
    p.btnHover = p.btnOn = p.btnOnHover = hl;
    p.numText = win;
}

void MakePalette(bool dark) {
    if (HighContrast()) {
        MakeContrastPalette();
        return;
    }
    Palette& p = g_pal;
    p.dark = dark;
    p.highContrast = false;
    const FluentAccent a = Fluent_ReadAccent();
    // Windows 11 tokens (solid colors over the window color).
    if (dark) {
        p.base = RGB(0x20, 0x20, 0x20);
        p.layer = RGB(0x27, 0x27, 0x27);
        p.card = RGB(0x2B, 0x2B, 0x2B);
        p.cardStroke = RGB(0x1D, 0x1D, 0x1D);
        p.ctlFill = RGB(0x32, 0x32, 0x32);  // between Windows' 6 % white over the window (2D) and over a card (37)
        p.ctlHover = RGB(0x39, 0x39, 0x39);
        p.ctlPressed = RGB(0x2B, 0x2B, 0x2B);
        p.ctlDisabled = RGB(0x2E, 0x2E, 0x2E);
        p.ctlStroke = RGB(0x3A, 0x3A, 0x3A);
        p.ctlStrokeBottom = RGB(0x3A, 0x3A, 0x3A);
        p.ctlStrong = RGB(0x9A, 0x9A, 0x9A);
        p.subtleHover = RGB(0x2D, 0x2D, 0x2D);
        p.subtlePressed = RGB(0x29, 0x29, 0x29);
        p.divider = RGB(0x33, 0x33, 0x33);
        p.text = RGB(0xFF, 0xFF, 0xFF);
        p.text2 = RGB(0xCF, 0xCF, 0xCF);
        p.text3 = RGB(0x9E, 0x9E, 0x9E);
        p.textDis = RGB(0x71, 0x71, 0x71);
        p.critical = RGB(0xFF, 0x99, 0xA4);
        p.success = RGB(0x6C, 0xCB, 0x5F);
        p.caution = RGB(0xFC, 0xE1, 0x00);
        p.focusOuter = RGB(0xFF, 0xFF, 0xFF);
        p.focusInner = RGB(0x00, 0x00, 0x00);
        p.ctlFocus = RGB(0x1F, 0x1F, 0x1F);
        p.accent = a.light2;
        p.accentText = a.light3;
        p.onAccent = RGB(0, 0, 0);
        p.accentDis = RGB(0x43, 0x43, 0x43);
        p.onAccentDis = RGB(0xA7, 0xA7, 0xA7);
    } else {
        p.base = RGB(0xF3, 0xF3, 0xF3);
        p.layer = RGB(0xF9, 0xF9, 0xF9);
        p.card = RGB(0xFB, 0xFB, 0xFB);
        p.cardStroke = RGB(0xE5, 0xE5, 0xE5);
        p.ctlFill = RGB(0xFB, 0xFB, 0xFB);
        p.ctlHover = RGB(0xF6, 0xF6, 0xF6);
        p.ctlPressed = RGB(0xF5, 0xF5, 0xF5);
        p.ctlDisabled = RGB(0xF5, 0xF5, 0xF5);
        p.ctlStroke = RGB(0xE5, 0xE5, 0xE5);
        p.ctlStrokeBottom = RGB(0xCC, 0xCC, 0xCC);
        p.ctlStrong = RGB(0x8B, 0x8B, 0x8B);
        p.subtleHover = RGB(0xEA, 0xEA, 0xEA);
        p.subtlePressed = RGB(0xED, 0xED, 0xED);
        p.divider = RGB(0xE5, 0xE5, 0xE5);
        p.text = RGB(0x1A, 0x1A, 0x1A);
        p.text2 = RGB(0x5C, 0x5C, 0x5C);
        p.text3 = RGB(0x86, 0x86, 0x86);
        p.textDis = RGB(0x9C, 0x9C, 0x9C);
        p.critical = RGB(0xC4, 0x2B, 0x1C);
        p.success = RGB(0x0F, 0x7B, 0x0F);
        p.caution = RGB(0x9D, 0x5D, 0x00);
        p.focusOuter = RGB(0x1A, 0x1A, 0x1A);
        p.focusInner = RGB(0xFF, 0xFF, 0xFF);
        p.ctlFocus = RGB(0xFF, 0xFF, 0xFF);
        p.accent = a.dark1;
        p.accentText = a.dark2;
        p.onAccent = RGB(0xFF, 0xFF, 0xFF);
        p.accentDis = RGB(0xC5, 0xC5, 0xC5);
        p.onAccentDis = RGB(0xFF, 0xFF, 0xFF);
    }
    // Hover / pressed accent: 90 % / 80 % of the accent over the window color.
    p.accentHover = Blend(p.base, p.accent, 0.9f);
    p.accentPressed = Blend(p.base, p.accent, 0.8f);

    // The older names used by every window, mapped onto the tokens.
    p.window = p.base;
    p.grey = p.text2;
    p.red = p.critical;
    p.nav = p.base;
    p.navSel = p.subtleHover;
    p.footer = p.base;
    p.line = p.divider;
    p.switchLine = p.ctlStrong;
    p.switchKnob = p.text2;
    p.edit = p.ctlFill;
    p.bar = p.layer;
    p.barLine = p.divider;
    p.icon = p.text;
    p.hint = dark ? p.text2 : p.text;
    p.status = dark ? p.text3 : p.text2;
    p.statusOk = p.success;
    p.btn = p.ctlFill;
    p.btnHover = p.ctlHover;
    p.btnText = p.text;
    if (dark) {
        p.focusBg = RGB(92, 40, 46);
        p.canvas = RGB(0x1C, 0x1C, 0x1C);
        p.iconOff = p.textDis;
        p.toolSel = RGB(0x3A, 0x3A, 0x3A);
        p.toolHover = RGB(0x32, 0x32, 0x32);
        p.ring = RGB(110, 110, 110);
        p.btnOn = RGB(0x45, 0x45, 0x45);
        p.btnOnHover = RGB(0x50, 0x50, 0x50);
        p.hist = RGB(22, 22, 22);
        p.histBar = RGB(105, 105, 105);
        p.frame = RGB(80, 80, 80);
        p.numText = RGB(40, 40, 40);
    } else {
        p.focusBg = RGB(255, 236, 236);
        p.canvas = RGB(0xE4, 0xE4, 0xE4);
        p.iconOff = RGB(175, 175, 175);
        p.toolSel = RGB(0xE0, 0xE0, 0xE0);
        p.toolHover = p.subtleHover;
        p.ring = RGB(150, 150, 150);
        p.btnOn = RGB(0xD6, 0xD6, 0xD6);
        p.btnOnHover = RGB(0xCC, 0xCC, 0xCC);
        p.hist = RGB(236, 236, 236);
        p.histBar = RGB(165, 165, 165);
        p.frame = RGB(170, 170, 170);
        p.numText = RGB(240, 240, 240);
    }
}

// ---- painting buttons, check boxes, radio buttons, switches and command links ---------------

constexpr wchar_t kSwitchProp[] = L"Canivete.Switch";
constexpr wchar_t kAccentProp[] = L"Canivete.Accent";
constexpr wchar_t kFramedProp[] = L"Canivete.Framed";

int Scale(HWND h, int v) { return MulDiv(v, static_cast<int>(GetDpiForWindow(h)), 96); }

std::wstring TextOf(HWND h) {
    std::wstring s(GetWindowTextLengthW(h), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    return s;
}

bool ShowFocus(HWND b) { return GetFocus() == b && !(SendMessageW(b, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS); }

void DrawCommandLink(HWND b, HDC dc, const RECT& rc, UINT state, bool enabled) {
    const Palette& p = g_pal;
    const UINT dpi = GetDpiForWindow(b);
    const bool hot = (state & BST_HOT) != 0, pushed = (state & BST_PUSHED) != 0;
    // A Windows 11 card button: control fill, hover / pressed fills, rounded.
    const COLORREF fill = !enabled ? p.ctlDisabled : pushed ? p.ctlPressed : hot ? p.ctlHover : p.ctlFill;
    Fluent_RoundRect(dc, rc, static_cast<float>(Fluent_Px(dpi, 6)), fill, p.ctlStroke,
                     std::max(1.0f, static_cast<float>(Fluent_Px(dpi, 1))));
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(b, WM_GETFONT, 0, 0));
    LOGFONTW lf{};
    GetObjectW(font ? font : GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    const int px = std::max(12, MulDiv(std::abs(lf.lfHeight ? lf.lfHeight : 14), 96, static_cast<int>(dpi)));
    HFONT bold = Fluent_FontPx(px, true, dpi);
    SetBkMode(dc, TRANSPARENT);
    const int pad = Scale(b, 12);
    RECT ar{rc.left + pad, rc.top + Scale(b, 9), rc.left + pad + Scale(b, 16), rc.top + Scale(b, 9) + Scale(b, 18)};
    Fluent_DrawGlyph(dc, ar, 0xE72A, 14, dpi, enabled ? p.accentText : p.textDis);  // Forward arrow
    HGDIOBJ old = SelectObject(dc, bold);
    const int left = ar.right + Scale(b, 10);
    SetTextColor(dc, enabled ? p.text : p.textDis);
    RECT tr{left, rc.top + Scale(b, 8), rc.right - pad, rc.bottom};
    const std::wstring title = TextOf(b);
    DrawTextW(dc, title.c_str(), -1, &tr, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    const DWORD n = static_cast<DWORD>(SendMessageW(b, BCM_GETNOTELENGTH, 0, 0));
    if (n) {
        std::wstring note(n + 1, L'\0');
        DWORD size = n + 1;
        SendMessageW(b, BCM_GETNOTE, reinterpret_cast<WPARAM>(&size), reinterpret_cast<LPARAM>(note.data()));
        note.resize(wcslen(note.c_str()));
        SelectObject(dc, font);
        SetTextColor(dc, enabled ? p.text2 : p.textDis);
        RECT nr{left, tr.top + tm.tmHeight + Scale(b, 2), rc.right - pad, rc.bottom - Scale(b, 4)};
        DrawTextW(dc, note.c_str(), -1, &nr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
    if (ShowFocus(b)) Fluent_DrawFocus(dc, rc, dpi, 6);
}

void DrawPush(HWND b, HDC dc, const RECT& rc, UINT state, bool enabled) {
    const UINT dpi = GetDpiForWindow(b);
    const bool accent = GetPropW(b, kAccentProp) != nullptr;
    const bool focus = ShowFocus(b);
    RECT br = rc;
    if (focus) InflateRect(&br, -Scale(b, 3), -Scale(b, 3));  // room for the ring inside the control
    const std::wstring text = TextOf(b);
    const LRESULT ui = SendMessageW(b, WM_QUERYUISTATE, 0, 0);
    Fluent_DrawButton(dc, br, dpi, text.c_str(), reinterpret_cast<HFONT>(SendMessageW(b, WM_GETFONT, 0, 0)), accent,
                      (state & BST_HOT) != 0, (state & BST_PUSHED) != 0, enabled, false,
                      (ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    if (focus) Fluent_DrawFocus(dc, rc, dpi, 7);
}

// The check box or radio glyph (20 x 20 at 96 DPI), Windows 11 style.
void DrawCheckGlyph(HDC dc, const RECT& g, UINT dpi, bool radio, bool checked, bool mixed, bool hot, bool pushed,
                    bool enabled) {
    const Palette& p = g_pal;
    const float one = std::max(1.0f, static_cast<float>(Fluent_Px(dpi, 1)));
    const bool on = checked || mixed;
    const COLORREF onFill = !enabled ? p.accentDis : pushed ? p.accentPressed : hot ? p.accentHover : p.accent;
    const COLORREF offFill = !enabled ? p.ctlDisabled : pushed ? p.subtlePressed : hot ? p.subtleHover : p.ctlFill;
    if (radio) {
        const float d = static_cast<float>(g.right - g.left);
        if (on) {
            Fluent_RoundRect(dc, g, d / 2, onFill);
            const int dot = Fluent_Px(dpi, pushed ? 6 : hot ? 10 : 8);
            RECT dr{(g.left + g.right - dot) / 2, (g.top + g.bottom - dot) / 2, 0, 0};
            dr.right = dr.left + dot;
            dr.bottom = dr.top + dot;
            Fluent_RoundRect(dc, dr, dot / 2.0f, enabled ? p.onAccent : p.onAccentDis);
        } else {
            Fluent_RoundRect(dc, g, d / 2, offFill, enabled ? p.ctlStrong : p.textDis, one);
        }
        return;
    }
    const float rad = static_cast<float>(Fluent_Px(dpi, 4));
    if (on) {
        Fluent_RoundRect(dc, g, rad, onFill);
        Fluent_DrawGlyph(dc, g, mixed ? 0xE738 : 0xE73E, 12, dpi, enabled ? p.onAccent : p.onAccentDis);
    } else {
        Fluent_RoundRect(dc, g, rad, offFill, enabled ? p.ctlStrong : p.textDis, one);
    }
}

// NM_CUSTOMDRAW from a button. Returns -1 to let the button draw itself.
LRESULT DrawButton(const NMCUSTOMDRAW& cd) {
    HWND b = cd.hdr.hwndFrom;
    const DWORD style = static_cast<DWORD>(GetWindowLongW(b, GWL_STYLE));
    const DWORD type = style & BS_TYPEMASK;
    const bool check = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE;
    const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    const bool link = type == BS_COMMANDLINK || type == BS_DEFCOMMANDLINK;
    const bool push = (type == BS_PUSHBUTTON || type == BS_DEFPUSHBUTTON) && !(style & (BS_ICON | BS_BITMAP)) &&
                      !SendMessageW(b, BM_GETIMAGE, IMAGE_BITMAP, 0) && !SendMessageW(b, BM_GETIMAGE, IMAGE_ICON, 0);
    if (!check && !radio && !link && !push) return -1;
    if (cd.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;  // CDDS_PREERASE comes first: skipping it skips the paint
    HDC dc = cd.hdc;
    const RECT rc = cd.rc;
    // The background the parent paints under this control (its WM_CTLCOLORSTATIC / BTN brush).
    HBRUSH bg = reinterpret_cast<HBRUSH>(SendMessageW(GetParent(b), push ? WM_CTLCOLORBTN : WM_CTLCOLORSTATIC,
                                                      reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(b)));
    FillRect(dc, &rc, bg ? bg : Theme_Brush(g_pal.window));
    const UINT state = static_cast<UINT>(SendMessageW(b, BM_GETSTATE, 0, 0));
    const bool enabled = IsWindowEnabled(b) != FALSE;
    if (link) {
        DrawCommandLink(b, dc, rc, state, enabled);
        return CDRF_SKIPDEFAULT;
    }
    if (push) {
        DrawPush(b, dc, rc, state, enabled);
        return CDRF_SKIPDEFAULT;
    }
    const UINT dpi = GetDpiForWindow(b);
    const bool hot = (state & BST_HOT) != 0, pushed = (state & BST_PUSHED) != 0;
    const bool checked = (state & BST_CHECKED) != 0, mixed = (state & BST_INDETERMINATE) != 0;
    const LRESULT ui = SendMessageW(b, WM_QUERYUISTATE, 0, 0);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(b, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    const std::wstring text = TextOf(b);
    const UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | ((ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    RECT focusRc = rc;
    if (check && GetPropW(b, kSwitchProp)) {
        // Switch: the text on the left, "On" / "Off" and the switch on the right.
        const SIZE sw = Fluent_SwitchSize(dpi);
        const int edge = Scale(b, 4);
        RECT trk{rc.right - edge - sw.cx, (rc.top + rc.bottom - sw.cy) / 2, rc.right - edge, 0};
        trk.bottom = trk.top + sw.cy;
        Fluent_DrawSwitch(dc, trk, dpi, checked, hot, enabled);
        SetTextColor(dc, enabled ? g_pal.text : g_pal.textDis);
        RECT orc{trk.left - Scale(b, 12) - Scale(b, 40), rc.top, trk.left - Scale(b, 12), rc.bottom};
        DrawTextW(dc, checked ? L"On" : L"Off", -1, &orc, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
        if (!text.empty()) {
            RECT tr{rc.left + edge, rc.top, orc.left - Scale(b, 8), rc.bottom};
            DrawTextW(dc, text.c_str(), -1, &tr, fmt);
        }
    } else {
        const int gsz = Scale(b, 20);
        RECT gr{rc.left + Scale(b, 1), (rc.top + rc.bottom - gsz) / 2, rc.left + Scale(b, 1) + gsz, 0};
        gr.bottom = gr.top + gsz;
        DrawCheckGlyph(dc, gr, dpi, radio, checked, mixed, hot, pushed, enabled);
        SetTextColor(dc, enabled ? g_pal.text : g_pal.textDis);
        RECT tr = rc;
        tr.left = gr.right + Scale(b, 8);
        DrawTextW(dc, text.c_str(), -1, &tr, fmt);
        RECT calc = tr;
        DrawTextW(dc, text.c_str(), -1, &calc, fmt | DT_CALCRECT);
        focusRc.right = std::min<LONG>(rc.right, calc.right + Scale(b, 4));
    }
    SelectObject(dc, old);
    if (ShowFocus(b)) Fluent_DrawFocus(dc, focusRc, dpi, 4);
    return CDRF_SKIPDEFAULT;
}

// ---- drop-down lists -----------------------------------------------------------------------

constexpr wchar_t kHotProp[] = L"Canivete.Hot";

// A drop-down list (CBS_DROPDOWNLIST) drawn as a Windows 11 combo box: a rounded control with the
// selected text and a chevron. The list that opens stays Windows' own.
LRESULT CALLBACK ComboSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (m) {
    case WM_PAINT:
    case WM_PRINTCLIENT: {
        if (g_pal.highContrast) break;
        PAINTSTRUCT ps{};
        HDC out = m == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        HDC dc = CreateCompatibleDC(out);
        HBITMAP bmp = CreateCompatibleBitmap(out, rc.right, rc.bottom);
        HGDIOBJ oldBmp = SelectObject(dc, bmp);
        SetDCBrushColor(dc, Theme_BackOf(h));
        FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        const UINT dpi = GetDpiForWindow(h);
        const bool enabled = IsWindowEnabled(h) != FALSE;
        const bool dropped = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
        const bool focus = GetFocus() == h && !(SendMessageW(h, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS);
        RECT br = rc;
        if (focus) InflateRect(&br, -Scale(h, 3), -Scale(h, 3));
        Fluent_DrawButton(dc, br, dpi, nullptr, nullptr, false, GetPropW(h, kHotProp) != nullptr, dropped, enabled,
                          false);
        const LRESULT sel = SendMessageW(h, CB_GETCURSEL, 0, 0);
        std::wstring text;
        if (sel >= 0) {
            const LRESULT n = SendMessageW(h, CB_GETLBTEXTLEN, sel, 0);
            if (n > 0) {
                text.resize(static_cast<size_t>(n) + 1);
                SendMessageW(h, CB_GETLBTEXT, sel, reinterpret_cast<LPARAM>(text.data()));
                text.resize(wcslen(text.c_str()));
            }
        }
        HGDIOBJ oldFont = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, enabled ? g_pal.text : g_pal.textDis);
        RECT tr{rc.left + Scale(h, 11), rc.top, rc.right - Scale(h, 34), rc.bottom};
        DrawTextW(dc, text.c_str(), -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
        SelectObject(dc, oldFont);
        RECT cr{rc.right - Scale(h, 34), rc.top, rc.right - Scale(h, 6), rc.bottom};
        Fluent_DrawGlyph(dc, cr, 0xE70D, 12, dpi, enabled ? g_pal.text2 : g_pal.textDis);
        if (focus) Fluent_DrawFocus(dc, rc, dpi, 7);
        BitBlt(out, 0, 0, rc.right, rc.bottom, dc, 0, 0, SRCCOPY);
        SelectObject(dc, oldBmp);
        DeleteObject(bmp);
        DeleteDC(dc);
        if (m == WM_PAINT) EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        if (!g_pal.highContrast) return 1;
        break;
    case WM_MOUSEMOVE:
        if (!GetPropW(h, kHotProp)) {
            SetPropW(h, kHotProp, reinterpret_cast<HANDLE>(1));
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
            InvalidateRect(h, nullptr, FALSE);
        }
        break;
    case WM_MOUSELEAVE:
        RemovePropW(h, kHotProp);
        InvalidateRect(h, nullptr, FALSE);
        break;
    case WM_SETFOCUS:
    case WM_KILLFOCUS:
    case WM_ENABLE:
    case WM_UPDATEUISTATE: {
        const LRESULT r = DefSubclassProc(h, m, wp, lp);
        InvalidateRect(h, nullptr, FALSE);
        return r;
    }
    case WM_NCDESTROY:
        RemovePropW(h, kHotProp);
        RemoveWindowSubclass(h, ComboSubclass, id);
        break;
    }
    return DefSubclassProc(h, m, wp, lp);
}

// ---- subclasses ----------------------------------------------------------------------------

bool OwnerDrawList(HWND h) { return (GetWindowLongW(h, GWL_STYLE) & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) != 0; }

// On each themed top-level window: text box and drop-down list colors, dark check boxes.
LRESULT CALLBACK TopSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (m) {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (!g_pal.highContrast && !(m == WM_CTLCOLORLISTBOX && OwnerDrawList(reinterpret_cast<HWND>(lp)))) {
            HDC dc = reinterpret_cast<HDC>(wp);
            HWND c = reinterpret_cast<HWND>(lp);
            // A text box in a Windows 11 frame gets a lighter (darker in dark mode) fill while typing.
            const COLORREF bg = m == WM_CTLCOLOREDIT && GetPropW(c, kFramedProp) && GetFocus() == c ? g_pal.ctlFocus
                                                                                                    : g_pal.edit;
            SetTextColor(dc, g_pal.text);
            SetBkColor(dc, bg);
            return reinterpret_cast<LRESULT>(Theme_Brush(bg));
        }
        break;
    case WM_NOTIFY: {
        const auto* n = reinterpret_cast<const NMHDR*>(lp);
        if (!g_pal.highContrast && n->code == NM_CUSTOMDRAW) {
            wchar_t cls[32];
            if (GetClassNameW(n->hwndFrom, cls, 32) && !_wcsicmp(cls, WC_BUTTONW)) {
                const LRESULT r = DrawButton(*reinterpret_cast<const NMCUSTOMDRAW*>(lp));
                if (r >= 0) return r;
            }
        }
        break;
    }
    case WM_SETTINGCHANGE:
        if ((lp && !lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet")) || wp == SPI_SETHIGHCONTRAST)
            Theme_Update();
        break;
    case WM_DWMCOLORIZATIONCOLORCHANGED:  // the accent color changed
        Theme_Update();
        break;
    case WM_NCDESTROY:
        RemoveWindowSubclass(h, TopSubclass, id);
        RemovePropW(h, kProp);
        break;
    }
    return DefSubclassProc(h, m, wp, lp);
}

// On list views: header text color (the dark header theme keeps black text otherwise).
LRESULT CALLBACK ListSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (m == WM_NOTIFY && g_dark) {
        const auto* n = reinterpret_cast<const NMHDR*>(lp);
        if (n->code == NM_CUSTOMDRAW && n->hwndFrom == ListView_GetHeader(h)) {
            const auto* cd = reinterpret_cast<const NMCUSTOMDRAW*>(lp);
            if (cd->dwDrawStage == CDDS_PREPAINT) return CDRF_NOTIFYITEMDRAW;
            if (cd->dwDrawStage == CDDS_ITEMPREPAINT) {
                SetTextColor(cd->hdc, g_pal.text);
                return CDRF_DODEFAULT;
            }
        }
    } else if (m == WM_NCDESTROY) {
        RemoveWindowSubclass(h, ListSubclass, id);
    }
    return DefSubclassProc(h, m, wp, lp);
}

LRESULT CALLBACK MainSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if ((m == WM_SETTINGCHANGE &&
         ((lp && !lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet")) || wp == SPI_SETHIGHCONTRAST)) ||
        m == WM_DWMCOLORIZATIONCOLORCHANGED)
        Theme_Update();
    else if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, MainSubclass, id);
    return DefSubclassProc(h, m, wp, lp);
}

void SetTheme(HWND c, const wchar_t* dark, const wchar_t* light = nullptr) { SetWindowTheme(c, g_dark ? dark : light, nullptr); }

// On text labels: Windows draws a disabled label engraved (a white shadow). Draw it in the disabled
// text color instead.
LRESULT CALLBACK StaticSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if ((m == WM_PAINT || m == WM_PRINTCLIENT) && !g_pal.highContrast && !IsWindowEnabled(h)) {
        const DWORD st = static_cast<DWORD>(GetWindowLongW(h, GWL_STYLE));
        const DWORD type = st & SS_TYPEMASK;
        if (type == SS_LEFT || type == SS_CENTER || type == SS_RIGHT || type == SS_LEFTNOWORDWRAP) {
            PAINTSTRUCT ps{};
            HDC dc = m == WM_PAINT ? BeginPaint(h, &ps) : reinterpret_cast<HDC>(wp);
            RECT rc;
            GetClientRect(h, &rc);
            HBRUSH br = reinterpret_cast<HBRUSH>(
                SendMessageW(GetParent(h), WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(h)));
            FillRect(dc, &rc, br ? br : Theme_Brush(g_pal.window));
            HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(h, WM_GETFONT, 0, 0)));
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, g_pal.textDis);
            UINT fmt = DT_EXPANDTABS | (type == SS_CENTER ? DT_CENTER : type == SS_RIGHT ? DT_RIGHT : DT_LEFT);
            if (type != SS_LEFTNOWORDWRAP) fmt |= DT_WORDBREAK;
            if (st & SS_NOPREFIX) fmt |= DT_NOPREFIX;
            if (st & SS_CENTERIMAGE) fmt = (fmt & ~DT_WORDBREAK) | DT_VCENTER | DT_SINGLELINE;
            if ((st & SS_ELLIPSISMASK) == SS_ENDELLIPSIS) fmt |= DT_END_ELLIPSIS;
            const std::wstring text = TextOf(h);
            DrawTextW(dc, text.c_str(), -1, &rc, fmt);
            SelectObject(dc, old);
            if (m == WM_PAINT) EndPaint(h, &ps);
            return 0;
        }
    }
    if (m == WM_NCDESTROY) RemoveWindowSubclass(h, StaticSubclass, id);
    return DefSubclassProc(h, m, wp, lp);
}

void ThemeControl(HWND c) {
    wchar_t cls[64] = {};
    GetClassNameW(c, cls, 64);
    if (pAllowDarkForWindow) pAllowDarkForWindow(c, g_dark);
    const DWORD style = static_cast<DWORD>(GetWindowLongW(c, GWL_STYLE));
    if (!_wcsicmp(cls, WC_BUTTONW)) {
        SetTheme(c, L"DarkMode_Explorer");
        // The window's default button is the accent one (kept when the focus moves the default).
        if ((style & BS_TYPEMASK) == BS_DEFPUSHBUTTON) SetPropW(c, kAccentProp, reinterpret_cast<HANDLE>(1));
    } else if (!_wcsicmp(cls, WC_STATICW)) {
        SetWindowSubclass(c, StaticSubclass, 1, 0);
    } else if (!_wcsicmp(cls, WC_EDITW)) {
        SetTheme(c, (style & ES_MULTILINE) ? L"DarkMode_Explorer" : L"DarkMode_CFD");
    } else if (!_wcsicmp(cls, WC_COMBOBOXW)) {
        SetTheme(c, L"DarkMode_CFD");
        if ((style & 3) == CBS_DROPDOWNLIST) SetWindowSubclass(c, ComboSubclass, 1, 0);
        COMBOBOXINFO ci{sizeof(ci)};
        if (GetComboBoxInfo(c, &ci) && ci.hwndList) SetTheme(ci.hwndList, L"DarkMode_Explorer");
    } else if (!_wcsicmp(cls, WC_LISTBOXW) || !_wcsicmp(cls, UPDOWN_CLASSW) || !_wcsicmp(cls, WC_SCROLLBARW) ||
               !_wcsicmp(cls, TOOLTIPS_CLASSW)) {
        SetTheme(c, L"DarkMode_Explorer");
    } else if (!_wcsicmp(cls, WC_LISTVIEWW)) {
        SetTheme(c, L"DarkMode_Explorer", L"Explorer");
        if (HWND hd = ListView_GetHeader(c)) {
            if (pAllowDarkForWindow) pAllowDarkForWindow(hd, g_dark);
            SetTheme(hd, L"DarkMode_ItemsView");
        }
        if (HWND tt = ListView_GetToolTips(c)) SetTheme(tt, L"DarkMode_Explorer");
        ListView_SetBkColor(c, g_pal.card);
        ListView_SetTextBkColor(c, g_pal.card);
        ListView_SetTextColor(c, g_pal.text);
        SetWindowSubclass(c, ListSubclass, 1, 0);
    }
}

BOOL CALLBACK ChildProc(HWND c, LPARAM) {
    ThemeControl(c);
    return TRUE;
}

void ThemeTop(HWND top) {
    SetPropW(top, kProp, reinterpret_cast<HANDLE>(1));
    if (pAllowDarkForWindow) pAllowDarkForWindow(top, g_dark);
    const BOOL dark = g_dark;
    DwmSetWindowAttribute(top, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    Fluent_StyleWindow(top);
    SetWindowSubclass(top, TopSubclass, 1, 0);
    EnumChildWindows(top, ChildProc, 0);
}

BOOL CALLBACK RepaintProc(HWND h, LPARAM) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId()) return TRUE;
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (!_wcsicmp(cls, TOOLTIPS_CLASSW)) {
        ThemeControl(h);
    } else if (GetPropW(h, kProp)) {
        ThemeTop(h);
        // Title bar and every control again (SWP_FRAMECHANGED makes Windows redraw the caption).
        SetWindowPos(h, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    if (IsWindowVisible(h)) RedrawWindow(h, nullptr, nullptr, RDW_ERASE | RDW_INVALIDATE | RDW_FRAME | RDW_ALLCHILDREN);
    return TRUE;
}

void Evaluate(bool repaint) {
    LoadApi();
    const bool dark = WantDark();
    const Palette before = g_pal;
    MakePalette(dark);  // always: the accent color or High Contrast colors may have changed
    if (g_ready && dark == g_dark && before.accent == g_pal.accent && before.highContrast == g_pal.highContrast &&
        before.text == g_pal.text && before.window == g_pal.window)
        return;
    g_ready = true;
    g_dark = dark;
    if (pSetPreferredAppMode) pSetPreferredAppMode(dark ? kForceDark : kForceLight);
    if (pFlushMenuThemes) pFlushMenuThemes();
    if (repaint) EnumWindows(RepaintProc, 0);
}

}  // namespace

const Palette& Pal() {
    if (!g_ready) Evaluate(false);
    return g_pal;
}

bool Theme_Dark() { return Pal().dark; }

HBRUSH Theme_Brush(COLORREF c) {
    HBRUSH& b = g_brushes[c];
    if (!b) b = CreateSolidBrush(c);
    return b;
}

namespace {
std::mutex g_gdipMx;
ULONG_PTR g_gdipToken = 0;
}  // namespace

void Gdiplus_Ensure() {
    std::lock_guard<std::mutex> lock(g_gdipMx);
    if (g_gdipToken) return;
    Gdiplus::GdiplusStartupInput in;
    if (Gdiplus::GdiplusStartup(&g_gdipToken, &in, nullptr) != Gdiplus::Ok) g_gdipToken = 0;
}

void Gdiplus_Shutdown() {
    std::lock_guard<std::mutex> lock(g_gdipMx);
    if (g_gdipToken) Gdiplus::GdiplusShutdown(g_gdipToken);
    g_gdipToken = 0;
}

void Theme_Init(HWND mainWnd) {
    g_uiThread = GetCurrentThreadId();
    Evaluate(false);
    if (pAllowDarkForWindow) pAllowDarkForWindow(mainWnd, TRUE);  // its menus (tray) may go dark
    SetWindowSubclass(mainWnd, MainSubclass, 1, 0);
}

void Theme_Window(HWND top) {
    Evaluate(true);
    ThemeTop(top);
}

void Theme_Update() { Evaluate(true); }

void Theme_SettingsLoaded() {
    if (g_ready && g_uiThread && GetCurrentThreadId() == g_uiThread) Evaluate(true);
}

void Theme_FramedEdit(HWND edit) {
    if (edit) SetPropW(edit, kFramedProp, reinterpret_cast<HANDLE>(1));
}

void Theme_AsSwitch(HWND checkbox) {
    if (checkbox) SetPropW(checkbox, kSwitchProp, reinterpret_cast<HANDLE>(1));
}

COLORREF Theme_BackOf(HWND ctl) {
    HWND parent = GetParent(ctl);
    if (!parent) return Pal().window;
    HDC dc = GetDC(ctl);
    HBRUSH br = reinterpret_cast<HBRUSH>(
        SendMessageW(parent, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc), reinterpret_cast<LPARAM>(ctl)));
    ReleaseDC(ctl, dc);
    LOGBRUSH lb{};
    if (br && GetObjectW(br, sizeof(lb), &lb) == sizeof(lb) && lb.lbStyle == BS_SOLID) return lb.lbColor;
    return Pal().window;
}
