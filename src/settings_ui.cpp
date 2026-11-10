// Settings window. Built in code (no dialog resource) so it can be laid out again at any monitor DPI.
// Hotkey boxes record keys through a low-level keyboard hook that is only installed while one of
// them has focus, so Win and PrintScreen combinations can be picked too.
#include "app.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <bitset>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClass[] = L"CaniveteSettings";
constexpr UINT WM_HK_CHANGED = WM_APP + 20;  // posted by the keyboard hook; wParam 1 = a hotkey changed
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD kExStyle = WS_EX_CONTROLPARENT;
constexpr COLORREF kFocusBg = RGB(255, 236, 236);  // hotkey box waiting for keys
constexpr COLORREF kGrey = RGB(105, 105, 105);
constexpr COLORREF kRed = RGB(200, 30, 40);
// Layout in 96-DPI pixels.
constexpr int kW = 500;             // client width
constexpr int kM = 16;              // outer margin
constexpr int kCX = kM + 140;       // left edge of the controls column
constexpr int kCW = kW - kM - kCX;  // width of the controls column

enum : int {
    IDC_HK0 = 200,    // hotkey boxes 200..203
    IDC_HKST0 = 210,  // their status texts 210..213
    IDC_STATIC = 220,
    IDC_HINT,
    IDC_SAVE = 230,
    IDC_FOLDER,
    IDC_BROWSE,
    IDC_NAME,
    IDC_TOKENS,
    IDC_EXAMPLE,
    IDC_FORMAT,
    IDC_QLABEL,
    IDC_QUALITY,
    IDC_QSPIN,
    IDC_FULLMODE,
    IDC_CLIP,
    IDC_NOTIFY,
    IDC_CURSOR,
    IDC_MAGNIFIER,
    IDC_EDITOR,
    IDC_AUTOSTART,
    IDC_COLORFMT,
    IDC_APPLY,
};

const wchar_t* const kHkNames[kHotkeyCount] = {L"Region screenshot", L"Window screenshot", L"Full screenshot",
                                                L"Color picker", L"Record GIF"};

struct Ctl {
    HWND wnd;
    int x, y, w, h;  // 96-DPI pixels
    bool bold;
};

struct UI {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    HBRUSH focusBrush = nullptr;
    std::vector<Ctl> ctls;
    int footerY = 0, height = 0;
    HWND lastFocus = nullptr;
    // Hotkeys being edited (saved only on OK / Apply).
    Hotkey hk[kHotkeyCount];
    bool bad[kHotkeyCount]{};
    int capturing = -1;    // which hotkey box has focus, -1 = none
    UINT liveMods = 0;     // Alt / Win held right now (seen by the hook)
    bool accepted = false;  // a key was taken while the current modifiers are held
    std::bitset<256> swallowed;
    HHOOK hook = nullptr;
} U;

int Px(int v) { return MulDiv(v, static_cast<int>(U.dpi), 96); }
HWND Item(int id) { return GetDlgItem(U.hwnd, id); }

std::wstring Trim(std::wstring s) {
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

std::wstring GetText(int id) {
    HWND c = Item(id);
    std::wstring s(GetWindowTextLengthW(c), L'\0');
    GetWindowTextW(c, s.data(), static_cast<int>(s.size()) + 1);
    return s;
}

bool Checked(int id) { return IsDlgButtonChecked(U.hwnd, id) == BST_CHECKED; }
void SetCheck(int id, bool on) { CheckDlgButton(U.hwnd, id, on ? BST_CHECKED : BST_UNCHECKED); }

// ---- building and laying out ---------------------------------------------------------------

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, DWORD ex = 0,
         bool bold = false) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, U.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hinst, nullptr);
    U.ctls.push_back({c, x, y, w, h, bold});
    return c;
}

void Heading(const wchar_t* text, int y) {
    Add(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, IDC_STATIC, kM, y, kW - 2 * kM, 22, 0, true);
}
void Label(const wchar_t* text, int y) {
    Add(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, IDC_STATIC, kM, y + 4, kCX - kM - 8, 18);
}
void Check(int id, const wchar_t* text, int y) {
    Add(WC_BUTTONW, text, BS_AUTOCHECKBOX | WS_TABSTOP, id, kM, y, kW - 2 * kM, 20);
}
HWND Edit(int id, int x, int y, int w, DWORD style = 0) {
    return Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP | style, id, x, y, w, 24, WS_EX_CLIENTEDGE);
}
HWND Button(int id, const wchar_t* text, int x, int y, int w, int h, DWORD style = BS_PUSHBUTTON) {
    return Add(WC_BUTTONW, text, style | WS_TABSTOP, id, x, y, w, h);
}

LRESULT CALLBACK HotkeyBoxProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);

// Hotkey row 1 (Window screenshot) is gone: the region tool highlights windows, a click takes one.
constexpr bool Shown(int i) { return i != 1; }

void Build() {
    int y = 14;
    Heading(L"Hotkeys", y);
    y += 28;
    for (int i = 0; i < kHotkeyCount; ++i) {
        if (!Shown(i)) continue;
        Label(kHkNames[i], y);
        HWND box = Edit(IDC_HK0 + i, kCX, y, 170, ES_READONLY);
        SetWindowSubclass(box, HotkeyBoxProc, 1, 0);
        Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_HKST0 + i, kCX + 180, y + 4, kCW - 180, 18);
        y += 30;
    }
    Add(WC_STATICW, L"Click a box, then press the new keys. Backspace turns a hotkey off.", SS_LEFT | SS_NOPREFIX,
        IDC_HINT, kCX, y - 2, kCW, 34);
    y += 42;

    Heading(L"Saving", y);
    y += 28;
    Check(IDC_SAVE, L"Save each screenshot to a file", y);
    y += 28;
    Label(L"Folder", y);
    HWND folder = Edit(IDC_FOLDER, kCX, y, kCW - 90);
    SendMessageW(folder, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Windows Screenshots folder"));
    SHAutoComplete(folder, SHACF_FILESYS_DIRS);
    Button(IDC_BROWSE, L"Browse…", kCX + kCW - 84, y - 1, 84, 26);
    y += 32;
    Label(L"File name", y);
    Edit(IDC_NAME, kCX, y, kCW - 90);
    Button(IDC_TOKENS, L"Insert ▾", kCX + kCW - 84, y - 1, 84, 26);
    y += 28;
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, IDC_EXAMPLE, kCX, y + 2, kCW, 18);
    y += 28;
    Label(L"Format", y);
    HWND fmt = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_FORMAT, kCX, y, 150, 200);
    SendMessageW(fmt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PNG (sharpest)"));
    SendMessageW(fmt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"JPG (smaller files)"));
    Add(WC_STATICW, L"Quality", SS_RIGHT | SS_NOPREFIX, IDC_QLABEL, kCX + 160, y + 4, 52, 18);
    HWND q = Edit(IDC_QUALITY, kCX + 218, y, 46, ES_NUMBER);
    HWND spin = Add(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK, IDC_QSPIN,
                    kCX + 264, y, 18, 24);
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(q), 0);
    SendMessageW(spin, UDM_SETRANGE32, 1, 100);
    y += 38;

    Heading(L"Capture", y);
    y += 28;
    Label(L"Full screenshot of", y);
    HWND full = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_FULLMODE, kCX, y, 230, 200);
    SendMessageW(full, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All monitors"));
    SendMessageW(full, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"The monitor under the mouse"));
    y += 34;
    Check(IDC_CLIP, L"Copy to the clipboard", y);
    y += 26;
    Check(IDC_NOTIFY, L"Show a preview after each capture (click it to draw on the screenshot)", y);
    y += 26;
    Check(IDC_EDITOR, L"Open the editor right after each capture", y);
    y += 26;
    Check(IDC_CURSOR, L"Include the mouse pointer", y);
    y += 26;
    Check(IDC_MAGNIFIER, L"Show the magnifier when selecting a region", y);
    y += 36;

    Heading(L"Color picker", y);
    y += 28;
    Label(L"Default format", y);
    HWND cf = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_COLORFMT, kCX, y, 260, 240);
    for (const ColorFormatDef& f : kColorFormats) SendMessageW(cf, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f.label));
    y += 38;

    Heading(L"Windows", y);
    y += 28;
    Check(IDC_AUTOSTART, L"Start Canivete with Windows", y);
    y += 34;

    U.footerY = y;
    y += 12;
    const int bw = 84;
    Button(IDOK, L"OK", kW - kM - 3 * bw - 16, y, bw, 28, BS_DEFPUSHBUTTON);
    Button(IDCANCEL, L"Cancel", kW - kM - 2 * bw - 8, y, bw, 28);
    Button(IDC_APPLY, L"Apply", kW - kM - bw, y, bw, 28);
    U.height = y + 28 + 12;
}

void MakeFonts() {
    if (U.font) DeleteObject(U.font);
    if (U.bold) DeleteObject(U.bold);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, U.dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    U.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = MulDiv(lf.lfHeight, 112, 100);
    U.bold = CreateFontIndirectW(&lf);
}

void Layout() {
    HDWP d = BeginDeferWindowPos(static_cast<int>(U.ctls.size()));
    for (const Ctl& c : U.ctls) {
        SendMessageW(c.wnd, WM_SETFONT, reinterpret_cast<WPARAM>(c.bold ? U.bold : U.font), FALSE);
        if (d) d = DeferWindowPos(d, c.wnd, nullptr, Px(c.x), Px(c.y), Px(c.w), Px(c.h), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (d) EndDeferWindowPos(d);
    InvalidateRect(U.hwnd, nullptr, TRUE);
}

SIZE WindowSize() {
    RECT r{0, 0, Px(kW), Px(U.height)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, U.dpi);
    return {r.right - r.left, r.bottom - r.top};
}

// ---- hotkey boxes --------------------------------------------------------------------------

bool SameKey(const Hotkey& a, const Hotkey& b) {
    return a.vk == b.vk && (a.mods & ~MOD_NOREPEAT) == (b.mods & ~MOD_NOREPEAT);
}

void ShowHotkey(int i) {
    std::wstring t;
    if (U.capturing == i && U.liveMods && !U.accepted) {
        if (U.liveMods & MOD_CONTROL) t += L"Ctrl+";
        if (U.liveMods & MOD_ALT) t += L"Alt+";
        if (U.liveMods & MOD_SHIFT) t += L"Shift+";
        if (U.liveMods & MOD_WIN) t += L"Win+";
        t += L"…";
    } else {
        t = U.hk[i].vk ? HotkeyText(U.hk[i]) : L"None";
    }
    SetWindowTextW(Item(IDC_HK0 + i), t.c_str());
}

// Status next to each box: off, duplicate, taken by another app.
void CheckHotkeys() {
    Hotkey owned[kHotkeyCount];  // what Canivete itself has registered (saved settings)
    ParseHotkey(g_settings.hkRegion, owned[0]);
    ParseHotkey(g_settings.hkFull, owned[2]);
    ParseHotkey(g_settings.hkColor, owned[3]);
    ParseHotkey(g_settings.hkGif, owned[4]);
    for (int i = 0; i < kHotkeyCount; ++i) {
        if (!Shown(i)) continue;
        std::wstring st;
        bool bad = false;
        const Hotkey& hk = U.hk[i];
        if (!hk.vk) {
            st = L"Off";
        } else {
            for (int j = 0; j < kHotkeyCount && !bad; ++j)
                if (j != i && U.hk[j].vk && SameKey(hk, U.hk[j])) {
                    st = std::wstring(L"Same as ") + kHkNames[j];
                    bad = true;
                }
            const bool mine =
                std::any_of(owned, owned + kHotkeyCount, [&](const Hotkey& o) { return SameKey(hk, o); });
            if (!bad && !mine) {
                if (RegisterHotKey(U.hwnd, 900 + i, hk.mods, hk.vk)) {
                    UnregisterHotKey(U.hwnd, 900 + i);
                } else if (!(hk.mods & MOD_WIN)) {  // Win combos Windows reserves still work (keyboard hook)
                    st = L"Used by another app";
                    bad = true;
                }
            }
        }
        if (!bad && U.capturing == i) st = L"Press the new keys";
        U.bad[i] = bad;
        SetWindowTextW(Item(IDC_HKST0 + i), st.c_str());
    }
}

UINT ModOf(UINT vk) {
    switch (vk) {
    case VK_LCONTROL: case VK_RCONTROL: case VK_CONTROL: return MOD_CONTROL;
    case VK_LMENU: case VK_RMENU: case VK_MENU: return MOD_ALT;
    case VK_LSHIFT: case VK_RSHIFT: case VK_SHIFT: return MOD_SHIFT;
    case VK_LWIN: case VK_RWIN: return MOD_WIN;
    }
    return 0;
}

// Keys that type or move the caret need Ctrl, Alt or Win, or they would stop working everywhere.
bool NeedsModifier(UINT vk) {
    return !(vk == VK_SNAPSHOT || vk == VK_PAUSE || vk == VK_SCROLL || vk == VK_INSERT ||
             (vk >= VK_F1 && vk <= VK_F24));
}

LRESULT CALLBACK KeyHook(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && U.capturing >= 0 && GetForegroundWindow() == U.hwnd) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        const UINT vk = k->vkCode & 0xFF;
        const bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        if (const UINT m = ModOf(vk)) {
            if (down) U.liveMods |= m;
            else U.liveMods &= ~m;
            if (!U.liveMods) U.accepted = false;
            PostMessageW(U.hwnd, WM_HK_CHANGED, 0, 0);
            // Ctrl and Shift go through (so Shift+Tab still works); Alt and Win are kept from the
            // system so they don't open the window menu or the Start menu.
            if (m == MOD_ALT || m == MOD_WIN) return 1;
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        if (!down) {
            if (U.swallowed[vk]) {
                U.swallowed[vk] = false;
                return 1;
            }
            return CallNextHookEx(nullptr, code, wp, lp);
        }
        const UINT mods = U.liveMods;
        const bool strong = (mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) != 0;
        bool take = false, changed = false;
        if (vk == VK_TAB && !strong) {
            // let Tab / Shift+Tab move to the next control
        } else if (!mods && (vk == VK_BACK || vk == VK_DELETE)) {
            U.hk[U.capturing] = {};
            take = changed = true;
        } else {
            const Hotkey cand{mods | MOD_NOREPEAT, vk};
            if (!HotkeyText(cand).empty() && (strong || !NeedsModifier(vk))) {
                U.hk[U.capturing] = cand;
                U.accepted = mods != 0;
                take = changed = true;
            } else if (mods) {
                take = true;  // a combination that can't be a hotkey: ignore it
            }
        }
        if (take) {
            U.swallowed[vk] = true;
            if (changed) PostMessageW(U.hwnd, WM_HK_CHANGED, 1, 0);
            return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void StartCapture(int i) {
    U.capturing = i;
    U.accepted = false;
    U.swallowed.reset();
    U.liveMods = 0;
    if (GetAsyncKeyState(VK_CONTROL) < 0) U.liveMods |= MOD_CONTROL;
    if (GetAsyncKeyState(VK_SHIFT) < 0) U.liveMods |= MOD_SHIFT;
    Hotkeys_Suspend(true);  // so pressing the current hotkey doesn't start a capture
    if (!U.hook) U.hook = SetWindowsHookExW(WH_KEYBOARD_LL, KeyHook, g_hinst, 0);
    if (!U.hook) Log(L"Settings: keyboard hook failed (%lu)", GetLastError());
    ShowHotkey(i);
    CheckHotkeys();
    InvalidateRect(Item(IDC_HK0 + i), nullptr, TRUE);
}

void StopCapture() {
    if (U.hook) UnhookWindowsHookEx(U.hook);
    U.hook = nullptr;
    const int i = U.capturing;
    U.capturing = -1;
    U.liveMods = 0;
    Hotkeys_Suspend(false);
    if (i >= 0) {
        ShowHotkey(i);
        InvalidateRect(Item(IDC_HK0 + i), nullptr, TRUE);
    }
    CheckHotkeys();
}

// Read-only edit that never shows a caret or a selection; keys come from the hook.
LRESULT CALLBACK HotkeyBoxProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_GETDLGCODE:
        return DLGC_WANTARROWS | DLGC_WANTCHARS;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK:
        SetFocus(h);
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONUP:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
    case WM_CONTEXTMENU:
    case WM_CHAR:
    case WM_KEYDOWN:
        return 0;
    case WM_SETFOCUS: {
        const LRESULT r = DefSubclassProc(h, m, wp, lp);
        SendMessageW(h, EM_SETSEL, static_cast<WPARAM>(-1), 0);
        HideCaret(h);
        return r;
    }
    }
    return DefSubclassProc(h, m, wp, lp);
}

// ---- the other controls --------------------------------------------------------------------

bool JpgSelected() { return SendMessageW(Item(IDC_FORMAT), CB_GETCURSEL, 0, 0) == 1; }

void UpdateExample() {
    std::wstring pat = Trim(GetText(IDC_NAME));
    if (pat.empty()) pat = L"%y-%mo-%d %h.%mi.%s";
    const std::wstring t = L"Example:  " + PreviewFileName(pat) + (JpgSelected() ? L".jpg" : L".png");
    SetWindowTextW(Item(IDC_EXAMPLE), t.c_str());
}

void UpdateEnable() {
    const bool save = Checked(IDC_SAVE);
    for (int id : {IDC_FOLDER, IDC_BROWSE, IDC_NAME, IDC_TOKENS, IDC_EXAMPLE, IDC_FORMAT})
        EnableWindow(Item(id), save);
    for (int id : {IDC_QLABEL, IDC_QUALITY, IDC_QSPIN}) EnableWindow(Item(id), save && JpgSelected());
}

void Populate() {
    const Settings& s = g_settings;
    ParseHotkey(s.hkRegion, U.hk[0]);
    U.hk[1] = {};  // Window screenshot: now part of the region tool
    ParseHotkey(s.hkFull, U.hk[2]);
    ParseHotkey(s.hkColor, U.hk[3]);
    ParseHotkey(s.hkGif, U.hk[4]);
    for (int i = 0; i < kHotkeyCount; ++i) ShowHotkey(i);
    int cf = 0;
    for (int i = 0; i < static_cast<int>(std::size(kColorFormats)); ++i)
        if (s.colorFormat == kColorFormats[i].key) cf = i;
    SendMessageW(Item(IDC_COLORFMT), CB_SETCURSEL, cf, 0);
    SetCheck(IDC_SAVE, s.saveToFile);
    SetWindowTextW(Item(IDC_FOLDER), s.folder.c_str());
    SetWindowTextW(Item(IDC_NAME), s.fileName.c_str());
    SendMessageW(Item(IDC_FORMAT), CB_SETCURSEL, s.format == L"jpg" ? 1 : 0, 0);
    SendMessageW(Item(IDC_QSPIN), UDM_SETPOS32, 0, s.jpegQuality);
    SendMessageW(Item(IDC_FULLMODE), CB_SETCURSEL, s.fullScreen == L"monitor" ? 1 : 0, 0);
    SetCheck(IDC_CLIP, s.copyToClipboard);
    SetCheck(IDC_NOTIFY, s.showNotification);
    SetCheck(IDC_CURSOR, s.includeCursor);
    SetCheck(IDC_MAGNIFIER, s.showMagnifier);
    SetCheck(IDC_EDITOR, s.openEditor);
    SetCheck(IDC_AUTOSTART, AutostartEnabled());
    CheckHotkeys();
    UpdateEnable();
    UpdateExample();
}

void Apply() {
    Settings s = g_settings;
    std::wstring* hks[kHotkeyCount] = {&s.hkRegion, &s.hkWindow, &s.hkFull, &s.hkColor, &s.hkGif};
    for (int i = 0; i < kHotkeyCount; ++i) *hks[i] = U.hk[i].vk ? HotkeyText(U.hk[i]) : L"None";
    s.saveToFile = Checked(IDC_SAVE);
    s.folder = Trim(GetText(IDC_FOLDER));
    s.fileName = Trim(GetText(IDC_NAME));
    if (s.fileName.empty()) s.fileName = L"%y-%mo-%d %h.%mi.%s";
    s.format = JpgSelected() ? L"jpg" : L"png";
    BOOL ok = FALSE;
    const UINT q = GetDlgItemInt(U.hwnd, IDC_QUALITY, &ok, FALSE);
    s.jpegQuality = ok ? std::clamp(static_cast<int>(q), 1, 100) : 90;
    s.fullScreen = SendMessageW(Item(IDC_FULLMODE), CB_GETCURSEL, 0, 0) == 1 ? L"monitor" : L"all";
    s.copyToClipboard = Checked(IDC_CLIP);
    s.showNotification = Checked(IDC_NOTIFY);
    s.includeCursor = Checked(IDC_CURSOR);
    s.showMagnifier = Checked(IDC_MAGNIFIER);
    s.openEditor = Checked(IDC_EDITOR);
    const LRESULT cf = SendMessageW(Item(IDC_COLORFMT), CB_GETCURSEL, 0, 0);
    if (cf >= 0 && cf < static_cast<LRESULT>(std::size(kColorFormats))) s.colorFormat = kColorFormats[cf].key;
    SaveSettings(s);
    const bool autostart = Checked(IDC_AUTOSTART);
    if (autostart != AutostartEnabled()) SetAutostart(autostart);
    Settings_Applied();
    SetDlgItemInt(U.hwnd, IDC_QUALITY, s.jpegQuality, FALSE);
    CheckHotkeys();
}

void Browse() {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Folder for screenshots");
    std::wstring cur = Trim(GetText(IDC_FOLDER));
    if (!cur.empty()) {
        wchar_t buf[MAX_PATH * 2];
        ExpandEnvironmentStringsW(cur.c_str(), buf, MAX_PATH * 2);
        cur = buf;
    } else if (g_settings.folder.empty()) {
        cur = ScreenshotsDir();
    }
    ComPtr<IShellItem> start;
    if (!cur.empty() && SUCCEEDED(SHCreateItemFromParsingName(cur.c_str(), nullptr, IID_PPV_ARGS(&start))))
        d->SetFolder(start.Get());
    ComPtr<IShellItem> result;
    if (FAILED(d->Show(U.hwnd)) || FAILED(d->GetResult(&result))) return;
    PWSTR p = nullptr;
    if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        SetWindowTextW(Item(IDC_FOLDER), p);
        CoTaskMemFree(p);
    }
}

void TokenMenu() {
    struct Tok {
        const wchar_t* tok;
        const wchar_t* desc;
    };
    static const Tok kTokens[] = {
        {L"%y", L"Year"},          {L"%yy", L"Year, 2 digits"}, {L"%mo", L"Month"},
        {L"%d", L"Day"},           {L"%h", L"Hour"},            {L"%mi", L"Minute"},
        {L"%s", L"Second"},        {L"%ms", L"Milliseconds"},   {nullptr, nullptr},
        {L"%pn", L"App name"},     {L"%t", L"Window title"},    {L"%width", L"Width"},
        {L"%height", L"Height"},   {nullptr, nullptr},          {L"\\", L"Put what comes before in a folder"},
    };
    HMENU m = CreatePopupMenu();
    for (UINT i = 0; i < std::size(kTokens); ++i) {
        const Tok& t = kTokens[i];
        if (!t.tok) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        std::wstring text = std::wstring(t.desc) + L"\t" + t.tok;
        if (t.tok[0] == L'%') text += L"   " + PreviewFileName(t.tok);
        AppendMenuW(m, MF_STRING, 1000 + i, text.c_str());
    }
    RECT r;
    GetWindowRect(Item(IDC_TOKENS), &r);
    const UINT cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, U.hwnd,
                                      nullptr);
    DestroyMenu(m);
    if (cmd >= 1000 && cmd < 1000 + std::size(kTokens)) {
        HWND name = Item(IDC_NAME);
        SetFocus(name);
        SendMessageW(name, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(kTokens[cmd - 1000].tok));
    }
}

// ---- window --------------------------------------------------------------------------------

LRESULT CALLBACK SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) U.hwnd = h;
    if (U.hwnd != h) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        U.dpi = GetDpiForWindow(h);
        MakeFonts();
        U.focusBrush = CreateSolidBrush(kFocusBg);
        Build();
        Layout();
        return 0;
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
        RECT f = rc;
        f.top = Px(U.footerY);
        FillRect(dc, &f, GetSysColorBrush(COLOR_BTNFACE));
        f.bottom = f.top + 1;
        SetDCBrushColor(dc, RGB(220, 220, 220));
        FillRect(dc, &f, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        COLORREF bg = GetSysColor(COLOR_WINDOW), fg = GetSysColor(COLOR_WINDOWTEXT);
        HBRUSH br = GetSysColorBrush(COLOR_WINDOW);
        if (id >= IDC_HK0 && id < IDC_HK0 + kHotkeyCount && U.capturing == id - IDC_HK0) {
            bg = kFocusBg;
            br = U.focusBrush;
        } else if (id >= IDC_HKST0 && id < IDC_HKST0 + kHotkeyCount) {
            fg = U.bad[id - IDC_HKST0] ? kRed : kGrey;
        } else if (id == IDC_HINT || id == IDC_EXAMPLE) {
            fg = kGrey;
        }
        SetBkColor(dc, bg);
        SetTextColor(dc, fg);
        return reinterpret_cast<LRESULT>(br);
    }
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    case WM_HK_CHANGED:
        if (U.capturing >= 0) ShowHotkey(U.capturing);
        if (wp) CheckHotkeys();
        return 0;
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND f = GetFocus();
            if (f && IsChild(h, f)) U.lastFocus = f;
        } else {
            SetFocus(U.lastFocus && IsWindow(U.lastFocus) ? U.lastFocus : Item(IDOK));
        }
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        if (id >= IDC_HK0 && id < IDC_HK0 + kHotkeyCount) {
            if (code == EN_SETFOCUS) StartCapture(id - IDC_HK0);
            else if (code == EN_KILLFOCUS) StopCapture();
            return 0;
        }
        switch (id) {
        case IDOK:
            Apply();
            DestroyWindow(h);
            break;
        case IDCANCEL:
            DestroyWindow(h);
            break;
        case IDC_APPLY:
            Apply();
            break;
        case IDC_SAVE:
            UpdateEnable();
            break;
        case IDC_FORMAT:
            if (code == CBN_SELCHANGE) {
                UpdateEnable();
                UpdateExample();
            }
            break;
        case IDC_NAME:
            if (code == EN_CHANGE) UpdateExample();
            break;
        case IDC_BROWSE:
            Browse();
            break;
        case IDC_TOKENS:
            TokenMenu();
            break;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        U.dpi = HIWORD(wp);
        MakeFonts();
        const RECT* sug = reinterpret_cast<const RECT*>(lp);
        const SIZE sz = WindowSize();
        SetWindowPos(h, nullptr, sug->left, sug->top, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_DESTROY:
        if (U.hook) UnhookWindowsHookEx(U.hook);
        U.hook = nullptr;
        U.capturing = -1;
        Hotkeys_Suspend(false);
        U.hwnd = nullptr;
        U.ctls.clear();
        U.lastFocus = nullptr;
        return 0;
    case WM_NCDESTROY:
        DeleteObject(U.font);
        DeleteObject(U.bold);
        DeleteObject(U.focusBrush);
        U.font = U.bold = nullptr;
        U.focusBrush = nullptr;
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

HWND SettingsUI_Window() { return U.hwnd; }

void SettingsUI_Show() {
    if (U.hwnd) {
        if (IsIconic(U.hwnd)) ShowWindow(U.hwnd, SW_RESTORE);
        SetForegroundWindow(U.hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = SettingsProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
        registered = RegisterClassExW(&wc) != 0;
    }
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    // Created on the monitor under the mouse so it starts at that monitor's DPI.
    HWND h = CreateWindowExW(kExStyle, kClass, L"Canivete Settings", kStyle, mi.rcWork.left, mi.rcWork.top, 100, 100,
                             nullptr, nullptr, g_hinst, nullptr);
    if (!h) return;
    const SIZE sz = WindowSize();
    const RECT& w = mi.rcWork;
    const int x = w.left + std::max(0L, (w.right - w.left - sz.cx) / 2);
    const int y = w.top + std::max(0L, (w.bottom - w.top - sz.cy) / 2);
    SetWindowPos(h, nullptr, x, y, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    Populate();
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}
