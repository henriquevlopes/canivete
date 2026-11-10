// Light / dark theme for all windows (see theme.h).
//
// Standard controls go dark through the themes Windows' own Explorer uses (DarkMode_Explorer,
// DarkMode_CFD, DarkMode_ItemsView) plus WM_CTLCOLOR* colors. Check boxes, radio buttons and command
// links ignore text colors when themed, so in dark mode they are painted here (NM_CUSTOMDRAW).
// Popup menus follow through uxtheme's SetPreferredAppMode / FlushMenuThemes (exports 135 / 136,
// Windows 10 1903 and later; older Windows just keeps light menus).
#include "app.h"
#include "theme.h"

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

void MakePalette(bool dark) {
    Palette& p = g_pal;
    p.dark = dark;
    if (dark) {
        p.window = RGB(32, 32, 32);
        p.text = RGB(240, 240, 240);
        p.grey = RGB(165, 165, 165);
        p.red = RGB(255, 153, 164);
        p.nav = RGB(43, 43, 43);
        p.navSel = RGB(64, 64, 64);
        p.footer = RGB(43, 43, 43);
        p.line = RGB(64, 64, 64);
        p.accent = RGB(76, 194, 255);
        p.onAccent = RGB(0, 0, 0);
        p.switchLine = RGB(200, 200, 200);
        p.switchKnob = RGB(210, 210, 210);
        p.focusBg = RGB(92, 40, 46);
        p.edit = RGB(45, 45, 45);
        p.canvas = RGB(38, 38, 38);
        p.bar = RGB(28, 28, 28);
        p.barLine = RGB(60, 60, 60);
        p.icon = RGB(225, 225, 225);
        p.iconOff = RGB(100, 100, 100);
        p.toolSel = RGB(72, 72, 72);
        p.toolHover = RGB(54, 54, 54);
        p.ring = RGB(110, 110, 110);
        p.hint = RGB(200, 200, 200);
        p.status = RGB(150, 150, 150);
        p.statusOk = RGB(120, 220, 130);
        p.btn = RGB(50, 50, 50);
        p.btnHover = RGB(66, 66, 66);
        p.btnOn = RGB(84, 84, 84);
        p.btnOnHover = RGB(96, 96, 96);
        p.btnText = RGB(235, 235, 235);
        p.hist = RGB(22, 22, 22);
        p.histBar = RGB(105, 105, 105);
        p.frame = RGB(80, 80, 80);
        p.numText = RGB(40, 40, 40);
    } else {
        p.window = GetSysColor(COLOR_WINDOW);
        p.text = GetSysColor(COLOR_WINDOWTEXT);
        p.grey = RGB(105, 105, 105);
        p.red = RGB(196, 43, 28);
        p.nav = RGB(243, 243, 243);
        p.navSel = RGB(226, 226, 226);
        p.footer = GetSysColor(COLOR_BTNFACE);
        p.line = RGB(220, 220, 220);
        p.accent = RGB(0, 95, 184);
        p.onAccent = RGB(255, 255, 255);
        p.switchLine = RGB(110, 110, 110);
        p.switchKnob = RGB(90, 90, 90);
        p.focusBg = RGB(255, 236, 236);
        p.edit = GetSysColor(COLOR_WINDOW);
        p.canvas = RGB(222, 222, 222);
        p.bar = RGB(249, 249, 249);
        p.barLine = RGB(218, 218, 218);
        p.icon = RGB(40, 40, 40);
        p.iconOff = RGB(175, 175, 175);
        p.toolSel = RGB(214, 214, 214);
        p.toolHover = RGB(232, 232, 232);
        p.ring = RGB(150, 150, 150);
        p.hint = RGB(50, 50, 50);
        p.status = RGB(100, 100, 100);
        p.statusOk = RGB(16, 124, 16);
        p.btn = RGB(229, 229, 229);
        p.btnHover = RGB(214, 214, 214);
        p.btnOn = RGB(196, 196, 196);
        p.btnOnHover = RGB(184, 184, 184);
        p.btnText = RGB(30, 30, 30);
        p.hist = RGB(236, 236, 236);
        p.histBar = RGB(165, 165, 165);
        p.frame = RGB(170, 170, 170);
        p.numText = RGB(240, 240, 240);
    }
}

// ---- painting check boxes, radio buttons and command links in dark mode --------------------

int Scale(HWND h, int v) { return MulDiv(v, static_cast<int>(GetDpiForWindow(h)), 96); }

Gdiplus::Color GpColor(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

void RoundRectSmooth(HDC dc, const RECT& rc, float radius, COLORREF fill, bool outline, COLORREF line) {
    static ULONG_PTR token = 0;
    if (!token) {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&token, &in, nullptr);
    }
    Gdiplus::Graphics g(dc);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    Gdiplus::RectF r(static_cast<float>(rc.left) + 0.5f, static_cast<float>(rc.top) + 0.5f,
                     static_cast<float>(rc.right - rc.left) - 1, static_cast<float>(rc.bottom - rc.top) - 1);
    const float d = std::min({radius * 2, r.Width, r.Height});
    Gdiplus::GraphicsPath p;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
    Gdiplus::SolidBrush b(GpColor(fill));
    g.FillPath(&b, &p);
    if (outline) {
        Gdiplus::Pen pen(GpColor(line), 1);
        g.DrawPath(&pen, &p);
    }
}

std::wstring TextOf(HWND h) {
    std::wstring s(GetWindowTextLengthW(h), L'\0');
    GetWindowTextW(h, s.data(), static_cast<int>(s.size()) + 1);
    return s;
}

void DrawCommandLink(HWND b, HDC dc, const RECT& rc, UINT state, bool enabled) {
    const Palette& p = g_pal;
    const bool hot = (state & BST_HOT) != 0, pushed = (state & BST_PUSHED) != 0, focus = GetFocus() == b;
    if (enabled && (hot || pushed || focus)) {
        const COLORREF fill = pushed ? RGB(58, 58, 58) : hot ? RGB(50, 50, 50) : p.window;
        RoundRectSmooth(dc, rc, static_cast<float>(Scale(b, 4)), fill, true, focus ? p.accent : RGB(80, 80, 80));
    }
    HFONT font = reinterpret_cast<HFONT>(SendMessageW(b, WM_GETFONT, 0, 0));
    LOGFONTW lf{};
    GetObjectW(font ? font : GetStockObject(DEFAULT_GUI_FONT), sizeof(lf), &lf);
    lf.lfWeight = FW_SEMIBOLD;
    HFONT bold = CreateFontIndirectW(&lf);
    SetBkMode(dc, TRANSPARENT);
    const int pad = Scale(b, 10);
    // arrow
    HGDIOBJ old = SelectObject(dc, bold);
    SetTextColor(dc, enabled ? p.accent : p.grey);
    RECT ar{rc.left + pad, rc.top + Scale(b, 7), rc.left + pad + Scale(b, 20), rc.top + Scale(b, 30)};
    DrawTextW(dc, L"→", 1, &ar, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_NOPREFIX);
    // title and note
    const int left = rc.left + pad + Scale(b, 24);
    SetTextColor(dc, enabled ? p.text : p.grey);
    RECT tr{left, rc.top + Scale(b, 7), rc.right - pad, rc.bottom};
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
        SetTextColor(dc, p.grey);
        RECT nr{left, tr.top + tm.tmHeight + Scale(b, 2), rc.right - pad, rc.bottom - Scale(b, 4)};
        DrawTextW(dc, note.c_str(), -1, &nr, DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX | DT_END_ELLIPSIS);
    }
    SelectObject(dc, old);
    DeleteObject(bold);
}

// NM_CUSTOMDRAW from a button, dark mode. Returns -1 to let the button draw itself.
LRESULT DrawButton(HWND top, const NMCUSTOMDRAW& cd) {
    HWND b = cd.hdr.hwndFrom;
    const DWORD type = static_cast<DWORD>(GetWindowLongW(b, GWL_STYLE)) & BS_TYPEMASK;
    const bool check = type == BS_CHECKBOX || type == BS_AUTOCHECKBOX || type == BS_3STATE || type == BS_AUTO3STATE;
    const bool radio = type == BS_RADIOBUTTON || type == BS_AUTORADIOBUTTON;
    const bool link = type == BS_COMMANDLINK || type == BS_DEFCOMMANDLINK;
    if (!check && !radio && !link) return -1;
    if (cd.dwDrawStage != CDDS_PREPAINT) return CDRF_DODEFAULT;  // CDDS_PREERASE comes first: skipping it skips the paint
    HDC dc = cd.hdc;
    const RECT rc = cd.rc;
    // The background the window paints under this control (its WM_CTLCOLORSTATIC brush).
    HBRUSH bg = reinterpret_cast<HBRUSH>(SendMessageW(top, WM_CTLCOLORSTATIC, reinterpret_cast<WPARAM>(dc),
                                                      reinterpret_cast<LPARAM>(b)));
    FillRect(dc, &rc, bg ? bg : Theme_Brush(g_pal.window));
    const UINT state = static_cast<UINT>(SendMessageW(b, BM_GETSTATE, 0, 0));
    const bool enabled = IsWindowEnabled(b) != FALSE;
    if (link) {
        DrawCommandLink(b, dc, rc, state, enabled);
        return CDRF_SKIPDEFAULT;
    }
    const int part = radio ? BP_RADIOBUTTON : BP_CHECKBOX;
    const int look = !enabled ? 3 : (state & BST_PUSHED) ? 2 : (state & BST_HOT) ? 1 : 0;
    const int which = (state & BST_CHECKED) ? 4 : (state & BST_INDETERMINATE) ? 8 : 0;
    const int partState = 1 + look + which;
    SIZE gs{Scale(b, 13), Scale(b, 13)};
    if (HTHEME th = OpenThemeData(b, L"Button")) {  // the dark variant, from SetWindowTheme
        GetThemePartSize(th, dc, part, partState, nullptr, TS_DRAW, &gs);
        const RECT gr{rc.left, (rc.top + rc.bottom - gs.cy) / 2, rc.left + gs.cx, (rc.top + rc.bottom - gs.cy) / 2 + gs.cy};
        DrawThemeBackground(th, dc, part, partState, &gr, nullptr);
        CloseThemeData(th);
    }
    const LRESULT ui = SendMessageW(b, WM_QUERYUISTATE, 0, 0);
    HGDIOBJ old = SelectObject(dc, reinterpret_cast<HFONT>(SendMessageW(b, WM_GETFONT, 0, 0)));
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, enabled ? g_pal.text : g_pal.grey);
    RECT tr = rc;
    tr.left += gs.cx + Scale(b, 4);
    const std::wstring text = TextOf(b);
    const UINT fmt = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | ((ui & UISF_HIDEACCEL) ? DT_HIDEPREFIX : 0);
    DrawTextW(dc, text.c_str(), -1, &tr, fmt);
    if (GetFocus() == b && !(ui & UISF_HIDEFOCUS)) {
        RECT fr = tr;
        DrawTextW(dc, text.c_str(), -1, &fr, fmt | DT_CALCRECT);
        fr.top = tr.top + (tr.bottom - tr.top - (fr.bottom - fr.top)) / 2;
        fr.bottom = fr.top + (fr.bottom - fr.top);
        InflateRect(&fr, 2, 1);
        SetTextColor(dc, g_pal.text);
        DrawFocusRect(dc, &fr);
    }
    SelectObject(dc, old);
    return CDRF_SKIPDEFAULT;
}

// ---- subclasses ----------------------------------------------------------------------------

bool OwnerDrawList(HWND h) { return (GetWindowLongW(h, GWL_STYLE) & (LBS_OWNERDRAWFIXED | LBS_OWNERDRAWVARIABLE)) != 0; }

// On each themed top-level window: text box and drop-down list colors, dark check boxes.
LRESULT CALLBACK TopSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    switch (m) {
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
        if (g_dark && !(m == WM_CTLCOLORLISTBOX && OwnerDrawList(reinterpret_cast<HWND>(lp)))) {
            HDC dc = reinterpret_cast<HDC>(wp);
            SetTextColor(dc, g_pal.text);
            SetBkColor(dc, g_pal.edit);
            return reinterpret_cast<LRESULT>(Theme_Brush(g_pal.edit));
        }
        break;
    case WM_NOTIFY: {
        const auto* n = reinterpret_cast<const NMHDR*>(lp);
        if (g_dark && n->code == NM_CUSTOMDRAW) {
            wchar_t cls[32];
            if (GetClassNameW(n->hwndFrom, cls, 32) && !_wcsicmp(cls, WC_BUTTONW)) {
                const LRESULT r = DrawButton(h, *reinterpret_cast<const NMCUSTOMDRAW*>(lp));
                if (r >= 0) return r;
            }
        }
        break;
    }
    case WM_SETTINGCHANGE:
        if ((lp && !lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet")) || wp == SPI_SETHIGHCONTRAST)
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
    if (m == WM_SETTINGCHANGE &&
        ((lp && !lstrcmpiW(reinterpret_cast<const wchar_t*>(lp), L"ImmersiveColorSet")) || wp == SPI_SETHIGHCONTRAST))
        Theme_Update();
    else if (m == WM_NCDESTROY)
        RemoveWindowSubclass(h, MainSubclass, id);
    return DefSubclassProc(h, m, wp, lp);
}

void SetTheme(HWND c, const wchar_t* dark, const wchar_t* light = nullptr) { SetWindowTheme(c, g_dark ? dark : light, nullptr); }

// On text labels: Windows draws a disabled label engraved (a white shadow), which looks broken on dark.
// Draw it plain grey instead.
LRESULT CALLBACK StaticSubclass(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if ((m == WM_PAINT || m == WM_PRINTCLIENT) && g_dark && !IsWindowEnabled(h)) {
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
            SetTextColor(dc, g_pal.grey);
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
    } else if (!_wcsicmp(cls, WC_STATICW)) {
        SetWindowSubclass(c, StaticSubclass, 1, 0);
    } else if (!_wcsicmp(cls, WC_EDITW)) {
        SetTheme(c, (style & ES_MULTILINE) ? L"DarkMode_Explorer" : L"DarkMode_CFD");
    } else if (!_wcsicmp(cls, WC_COMBOBOXW)) {
        SetTheme(c, L"DarkMode_CFD");
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
        ListView_SetBkColor(c, g_pal.window);
        ListView_SetTextBkColor(c, g_pal.window);
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
    if (g_ready && dark == g_dark) {
        if (!dark) MakePalette(false);  // light keeps Windows' own colors, which may have changed
        return;
    }
    g_ready = true;
    g_dark = dark;
    MakePalette(dark);
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
