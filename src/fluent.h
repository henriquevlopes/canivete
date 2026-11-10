// Windows 11 (Fluent) look shared by every Canivete window, on top of theme.h.
//
// Native controls stay native; this layer adds what Windows 11 apps have and plain Win32 doesn't:
// the window frame (rounded corners, Mica title bar), the Segoe UI Variable type ramp, the system
// accent color, and a few shapes drawn the Windows 11 way (cards, the selected tab in a list,
// switches, buttons, progress bars, keyboard focus rings). Colors always come from Pal().
//
// Windows 10 gets the same layout with Segoe UI and Segoe MDL2 Assets and no DWM extras; High
// Contrast keeps Windows' own colors and frame.
#pragma once
#include <windows.h>

// Type ramp (Windows 11 sizes, in 96-DPI pixels).
enum FluentType : int {
    kTypeCaption,     // 12 px
    kTypeBody,        // 14 px
    kTypeBodyStrong,  // 14 px semibold
    kTypeBodyLarge,   // 18 px
    kTypeSubtitle,    // 20 px semibold
    kTypeTitle,       // 28 px semibold
};
// A font of the ramp at this DPI. Fonts are made once and kept for the whole run: never delete them.
HFONT Fluent_Font(FluentType type, UINT dpi);
HFONT Fluent_FontPx(int px, bool semibold, UINT dpi);
// The icon font (Segoe Fluent Icons, else Segoe MDL2 Assets) at px (96-DPI pixels).
HFONT Fluent_IconFont(int px, UINT dpi);
const wchar_t* Fluent_IconFace();

// Glyphs of the icon font.
namespace Glyph {
constexpr wchar_t Save = 0xE74E, Copy = 0xE8C8, Paste = 0xE77F, Undo = 0xE7A7, Redo = 0xE7A6, Delete = 0xE74D,
                  Cancel = 0xE711, Close = 0xE8BB, Settings = 0xE713, Folder = 0xE8B7, Cut = 0xE8C6, Share = 0xE72D,
                  Upload = 0xE898, Accept = 0xE8FB, CheckMark = 0xE73E, Refresh = 0xE72C, Search = 0xE721,
                  Back = 0xE72B, Add = 0xE710, Camera = 0xE722, Video = 0xE714, Pin = 0xE718, Sync = 0xE895,
                  More = 0xE712, ChevronDown = 0xE70D, Home = 0xE80F, AllApps = 0xE71D, Power = 0xE7E8,
                  Health = 0xE95E, Color = 0xE790, Photo = 0xE91B, Rename = 0xE8AC, Repair = 0xE90F,
                  Slideshow = 0xE786, Broom = 0xEA99, Record = 0xE7C8, Stop = 0xE71A;
}

int Fluent_Px(UINT dpi, int v);   // 96-DPI pixels at dpi
bool Fluent_IsWin11();            // build 22000 or later
bool Fluent_HighContrast();
bool Fluent_Animations();         // Windows' "Animation effects" setting

// The window frame: rounded corners, Mica title bar (22621+) or a title bar in the window color
// (22000..22620). Theme_Window calls it, and again on every theme or accent change.
void Fluent_StyleWindow(HWND top);
// A popup without a title bar (toast, recording bar): rounded corners and a thin border color.
void Fluent_StylePopup(HWND popup, bool compact = false);

// The accent palette Windows derives from the accent color (Settings > Personalization > Colors).
struct FluentAccent {
    COLORREF light3, light2, light1, base, dark1, dark2, dark3;
};
FluentAccent Fluent_ReadAccent();

// ---- drawing (antialiased; colors from Pal()) ------------------------------------------------
// A rounded rectangle; the outline (if any) lies inside r.
void Fluent_RoundRect(HDC dc, const RECT& r, float radius, COLORREF fill, COLORREF stroke = CLR_INVALID,
                      float strokeW = 1.0f);
// A card: card fill, 1 px card stroke, radius 8 (or the radius given, in 96-DPI pixels).
void Fluent_DrawCard(HDC dc, const RECT& r, UINT dpi, int radius = 8);
// A text box frame: control fill, 1 px stroke, and the bottom line (2 px accent when focused).
// fill = CLR_INVALID uses the control fill.
void Fluent_DrawTextBox(HDC dc, const RECT& r, UINT dpi, bool focused, bool enabled, COLORREF fill = CLR_INVALID);
// The background of one entry of a tab list (nav pane): the hover / selected pill, the accent
// indicator on the selected one, and the keyboard focus ring. bg = the pane color under it.
void Fluent_DrawNavItem(HDC dc, const RECT& item, UINT dpi, bool selected, bool hot, bool focus, COLORREF bg);
// A switch (40 x 20 track at 96 DPI).
void Fluent_DrawSwitch(HDC dc, const RECT& track, UINT dpi, bool on, bool hot, bool enabled);
SIZE Fluent_SwitchSize(UINT dpi);
// A button drawn by hand (picture windows, the toast): standard or accent.
void Fluent_DrawButton(HDC dc, const RECT& r, UINT dpi, const wchar_t* text, HFONT font, bool accent, bool hot,
                       bool pressed, bool enabled, bool focus, UINT textFlags = 0);
// A progress bar: 1 px track, 3 px rounded fill. pos 0..1000, or -1 for a segment that slides
// (pass the time in ms as phase). fill = CLR_INVALID uses the accent color.
void Fluent_DrawProgress(HDC dc, const RECT& r, UINT dpi, int pos, ULONGLONG phase, COLORREF fill = CLR_INVALID,
                         COLORREF bg = CLR_INVALID);
// The keyboard focus ring around r (2 px outer, 1 px inner), radius in 96-DPI pixels.
void Fluent_DrawFocus(HDC dc, const RECT& r, UINT dpi, int radius = 4);
// A glyph of the icon font centered in r.
void Fluent_DrawGlyph(HDC dc, const RECT& r, wchar_t glyph, int px, UINT dpi, COLORREF color);
