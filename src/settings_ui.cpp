// Settings window: a list of tabs on the left (General plus one per feature), the tab's controls on
// the right. Each feature tab has an On switch; a feature that is off loses its hotkeys, tray menu
// entries and Explorer entries. Built in code (no dialog resource) so it can be laid out again at
// any monitor DPI. Hotkey boxes record keys through a low-level keyboard hook that is only installed
// while one of them has focus, so Win and PrintScreen combinations can be picked too.
#include "app.h"
#include "theme.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <windowsx.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <bitset>
#include <cwctype>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClass[] = L"CaniveteSettings";
constexpr UINT WM_HK_CHANGED = WM_APP + 20;  // posted by the keyboard hook; wParam 1 = a hotkey changed
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD kExStyle = WS_EX_CONTROLPARENT;
// Layout in 96-DPI pixels.
constexpr int kW = 684;              // client width
constexpr int kM = 16;               // outer margin
constexpr int kNavW = 196;           // width of the tab list panel
constexpr int kL = kNavW + 24;       // left edge of a tab's content
constexpr int kR = kW - 20;          // right edge of a tab's content
constexpr int kCW = kR - kL;         // content width
constexpr int kCX = kL + 128;        // left edge of the controls column (labels to its left)
constexpr int kFW = kR - kCX;        // width of the controls column
constexpr int kTop = 98;             // first row under a tab's title and description
constexpr int kNavItem = 34;         // height of one tab in the list

enum : int {
    IDC_HK0 = 200,    // hotkey boxes 200..205
    IDC_HKST0 = 210,  // their status texts 210..215
    IDC_STATIC = 220,
    IDC_HINT,
    IDC_DESC,
    IDC_TITLE,
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
    IDC_VSOUND,
    IDC_VMIC,
    IDC_COLORFMT,
    IDC_APPLY,
    IDC_NAV,
    IDC_OPENRESIZE,
    IDC_OPENRENAME,
    IDC_OPENMAINT,
    IDC_MAINTDAILY,
    IDC_CHECKUPD,
    IDC_UPDNOW,
    IDC_UPSERVICE,
    IDC_UPHELP,
    IDC_UPSHOTS,
    IDC_UPGIF,
    IDC_UPVIDEO,
    IDC_DRVID,
    IDC_DRVSECRET,
    IDC_DRVCONNECT,
    IDC_DRVSTATUS,
    IDC_DRVFOLDER,
    IDC_DRVSHARE,
    IDC_S3BUCKET,
    IDC_S3REGION,
    IDC_S3PREFIX,
    IDC_S3KEYID,
    IDC_S3SECRET,
    IDC_S3LINK,
    IDC_S3PRIVATE,
    IDC_S3TEST,
    IDC_S3STATUS,
    IDC_THEME,
    IDC_OFFNOTE0 = 300,  // "Turned off" notes 300..307, one per Feature
};

const wchar_t* const kHkNames[kHotkeyCount] = {L"Region screenshot", L"Window screenshot", L"Full screenshot",
                                                L"Color picker", L"Record GIF", L"Record video"};
constexpr Feature kHkFeature[kHotkeyCount] = {kFeatShots, kFeatShots, kFeatShots, kFeatColor, kFeatGif, kFeatVideo};

struct TabDef {
    const wchar_t* name;
    int feature;  // -1 = General (no switch)
    const wchar_t* desc;
};
const TabDef kTabs[] = {
    {L"General", -1, L"Where captures are saved, how files are named, and starting with Windows."},
    {L"Screenshots", kFeatShots,
     L"Capture a region, a window or the whole screen. Includes the editor and Make alpha."},
    {L"Color picker", kFeatColor, L"Pick any color on the screen and copy its value."},
    {L"GIF recording", kFeatGif, L"Record an area of the screen as an animated GIF. You pick the size afterwards."},
    {L"Video recording", kFeatVideo, L"Record an area of the screen as an MP4 video. You pick the size afterwards."},
    {L"Uploads", kFeatUpload,
     L"Put screenshots, GIFs and videos on Google Drive or Amazon S3 and copy the link: with the Upload "
     L"buttons, or by itself."},
    {L"Image tools", kFeatImages,
     L"Resize, convert, rotate and copy pictures: from the tray menu, Send to, and Explorer's right-click menu "
     L"(Show more options → Canivete)."},
    {L"Renamer", kFeatRename,
     L"Rename many files and folders at once: from the tray menu, Send to, and Explorer's right-click menu."},
    {L"Maintenance", kFeatMaint,
     L"Windows, driver and app updates, cleanup, startup apps and restart advice: from the tray menu and the "
     L"Start menu (Computer Maintenance)."},
};
constexpr int kTabCount = static_cast<int>(std::size(kTabs));

enum FontKind : int { kFontNormal, kFontBold, kFontTitle };

struct Ctl {
    HWND wnd;
    int x, y, w, h;  // 96-DPI pixels
    int font;
    int tab;      // -1 = always shown (tab list, footer)
    bool header;  // title, description and switch: stay enabled when the feature is off
    int group;    // Uploads tab: 0 = always, 1 = Google Drive only, 2 = Amazon S3 only
};

struct UI {
    HWND hwnd = nullptr, nav = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr, title = nullptr;
    std::vector<Ctl> ctls;
    int buildTab = -1;
    bool buildHeader = false;
    int buildGroup = 0;
    bool driveBad = false, s3Bad = false;  // their status line shows an error (red)
    int maxY = 0, footerY = 0, height = 0;
    int tab = 0;
    HWND lastFocus = nullptr;
    bool on[kFeatCount]{};  // feature switches being edited (saved only on OK / Apply)
    // Hotkeys being edited (saved only on OK / Apply).
    Hotkey hk[kHotkeyCount];
    bool bad[kHotkeyCount]{};
    int capturing = -1;    // which hotkey box has focus, -1 = none
    UINT liveMods = 0;     // Alt / Win held right now (seen by the hook)
    bool accepted = false;  // a key was taken while the current modifiers are held
    std::bitset<256> swallowed;
    HHOOK hook = nullptr;
} U;

int g_lastTab = 0;  // the window opens on the tab it was closed on

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

// Hotkey row 1 (Window screenshot) is gone: the region tool highlights windows, a click takes one.
constexpr bool Shown(int i) { return i != 1; }
bool HotkeyOn(int i) { return Shown(i) && U.on[kHkFeature[i]]; }

// ---- building and laying out ---------------------------------------------------------------

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, DWORD ex = 0,
         int font = kFontNormal) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, U.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hinst, nullptr);
    U.ctls.push_back({c, x, y, w, h, font, U.buildTab, U.buildHeader, U.buildGroup});
    U.maxY = std::max(U.maxY, y + std::min(h, 34));  // (a combo box's height includes its drop-down list)
    return c;
}

void Heading(const wchar_t* text, int y) {
    Add(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, IDC_STATIC, kL, y, kCW, 22, 0, kFontBold);
}
void Label(const wchar_t* text, int y) {
    Add(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, IDC_STATIC, kL, y + 4, kCX - kL - 8, 18);
}
void Check(int id, const wchar_t* text, int y) {
    Add(WC_BUTTONW, text, BS_AUTOCHECKBOX | WS_TABSTOP, id, kL, y, kCW, 20);
}
HWND Edit(int id, int x, int y, int w, DWORD style = 0) {
    return Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP | style, id, x, y, w, 24, WS_EX_CLIENTEDGE);
}
HWND Button(int id, const wchar_t* text, int x, int y, int w, int h, DWORD style = BS_PUSHBUTTON) {
    return Add(WC_BUTTONW, text, style | WS_TABSTOP, id, x, y, w, h);
}
void Hint(const wchar_t* text, int x, int y, int w, int h) {
    Add(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, IDC_HINT, x, y, w, h);
}

LRESULT CALLBACK HotkeyBoxProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);
LRESULT CALLBACK NavProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR);

void HotkeyRow(int i, int y) {
    Label(kHkNames[i], y);
    HWND box = Edit(IDC_HK0 + i, kCX, y, 160, ES_READONLY);
    SetWindowSubclass(box, HotkeyBoxProc, 1, 0);
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_HKST0 + i, kCX + 168, y + 4, kFW - 168, 18);
}
void HotkeyHint(int y) {
    Hint(L"Click the box, then press the new keys. Backspace turns the hotkey off.", kCX, y, kFW, 34);
}

// Title, description and (for a feature) its On switch. Returns the y of the first row.
int BeginTab(int t) {
    U.buildTab = t;
    U.buildHeader = true;
    const TabDef& d = kTabs[t];
    Add(WC_STATICW, d.name, SS_LEFT | SS_NOPREFIX, IDC_TITLE, kL, 16, kCW - 210, 30, 0, kFontTitle);
    if (d.feature >= 0)
        Add(WC_STATICW, L"Turned off (switch in the list)", SS_RIGHT | SS_NOPREFIX, IDC_OFFNOTE0 + d.feature,
            kR - 200, 26, 200, 18);
    Add(WC_STATICW, d.desc, SS_LEFT | SS_NOPREFIX, IDC_DESC, kL, 52, kCW, 36);
    U.buildHeader = false;
    return kTop;
}

void Build() {
    U.buildTab = -1;
    U.nav = Add(WC_LISTBOXW, L"",
                LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP, IDC_NAV, 8, 12,
                kNavW - 16, kTabCount * kNavItem + 4);
    for (const TabDef& d : kTabs) SendMessageW(U.nav, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name));
    SetWindowSubclass(U.nav, NavProc, 2, 0);

    // General
    int y = BeginTab(0);
    Check(IDC_SAVE, L"Save each capture to a file", y);
    y += 30;
    Label(L"Folder", y);
    HWND folder = Edit(IDC_FOLDER, kCX, y, kFW - 90);
    SendMessageW(folder, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Windows Screenshots folder"));
    SHAutoComplete(folder, SHACF_FILESYS_DIRS);
    Button(IDC_BROWSE, L"Browse…", kR - 84, y - 1, 84, 26);
    y += 32;
    Label(L"File name", y);
    Edit(IDC_NAME, kCX, y, kFW - 90);
    Button(IDC_TOKENS, L"Insert ▾", kR - 84, y - 1, 84, 26);
    y += 28;
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_PATHELLIPSIS, IDC_EXAMPLE, kCX, y + 2, kFW, 18);
    y += 28;
    Label(L"Format", y);
    HWND fmt = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_FORMAT, kCX, y, 150, 200);
    SendMessageW(fmt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"PNG (sharpest)"));
    SendMessageW(fmt, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"JPG (smaller files)"));
    Add(WC_STATICW, L"Quality", SS_RIGHT | SS_NOPREFIX, IDC_QLABEL, kCX + 156, y + 4, 52, 18);
    HWND q = Edit(IDC_QUALITY, kCX + 214, y, 46, ES_NUMBER);
    HWND spin = Add(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK, IDC_QSPIN,
                    kCX + 260, y, 18, 24);
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(q), 0);
    SendMessageW(spin, UDM_SETRANGE32, 1, 100);
    y += 44;
    Heading(L"Windows", y);
    y += 28;
    Label(L"Appearance", y);
    HWND theme = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_THEME, kCX, y, 150, 200);
    for (const wchar_t* t : {L"Follow Windows", L"Light", L"Dark"})
        SendMessageW(theme, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(t));
    y += 36;
    Check(IDC_AUTOSTART, L"Start Canivete with Windows", y);
    y += 26;
    Check(IDC_CHECKUPD, L"Tell me when a new version of Canivete is out", y);
    y += 30;
    Button(IDC_UPDNOW, L"Check for updates now", kL, y, 190, 28);

    // Screenshots
    y = BeginTab(1);
    HotkeyRow(0, y);
    y += 30;
    HotkeyRow(2, y);
    y += 28;
    HotkeyHint(y);
    y += 42;
    Label(L"Full screenshot of", y);
    HWND full = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_FULLMODE, kCX, y, 230, 200);
    SendMessageW(full, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"All monitors"));
    SendMessageW(full, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"The monitor under the mouse"));
    y += 36;
    Check(IDC_CLIP, L"Copy to the clipboard", y);
    y += 26;
    Check(IDC_NOTIFY, L"Show a preview after each capture (click it to edit)", y);
    y += 26;
    Check(IDC_EDITOR, L"Open the editor right after each capture", y);
    y += 26;
    Check(IDC_CURSOR, L"Include the mouse pointer", y);
    y += 26;
    Check(IDC_MAGNIFIER, L"Show the magnifier when selecting a region", y);

    // Color picker
    y = BeginTab(2);
    HotkeyRow(3, y);
    y += 28;
    HotkeyHint(y);
    y += 42;
    Label(L"Default format", y);
    HWND cf = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_COLORFMT, kCX, y, 260, 240);
    for (const ColorFormatDef& f : kColorFormats) SendMessageW(cf, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f.label));

    // GIF recording
    y = BeginTab(3);
    HotkeyRow(4, y);
    y += 28;
    HotkeyHint(y);

    // Video recording
    y = BeginTab(4);
    HotkeyRow(5, y);
    y += 28;
    HotkeyHint(y);
    y += 42;
    Check(IDC_VSOUND, L"Record the computer's sound", y);
    y += 26;
    Check(IDC_VMIC, L"Record the microphone", y);

    // Uploads
    y = BeginTab(kSettingsTabUploads);
    Label(L"Upload to", y);
    HWND svc = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, IDC_UPSERVICE, kCX, y, 170, 200);
    SendMessageW(svc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Google Drive"));
    SendMessageW(svc, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Amazon S3"));
    Button(IDC_UPHELP, L"How to set it up…", kR - 140, y - 1, 140, 26);
    y += 36;
    Label(L"Upload by itself", y);
    Add(WC_BUTTONW, L"Screenshots", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_UPSHOTS, kCX, y + 2, 110, 20);
    Add(WC_BUTTONW, L"GIFs", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_UPGIF, kCX + 116, y + 2, 70, 20);
    Add(WC_BUTTONW, L"Videos", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_UPVIDEO, kCX + 192, y + 2, 90, 20);
    y += 32;
    const int y0 = y;
    U.buildGroup = 1;  // Google Drive
    Label(L"Client ID", y);
    Edit(IDC_DRVID, kCX, y, kFW);
    y += 32;
    Label(L"Client secret", y);
    Edit(IDC_DRVSECRET, kCX, y, kFW, ES_PASSWORD);
    y += 32;
    Label(L"Account", y);
    Button(IDC_DRVCONNECT, L"Connect…", kCX, y - 1, 104, 26);
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_DRVSTATUS, kCX + 112, y + 4, kFW - 112, 18);
    y += 34;
    Label(L"Drive folder", y);
    SendMessageW(Edit(IDC_DRVFOLDER, kCX, y, 200), EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"My Drive itself"));
    y += 32;
    Add(WC_BUTTONW, L"Anyone with the link can view", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_DRVSHARE, kCX, y, kFW, 20);
    y = y0;
    U.buildGroup = 2;  // Amazon S3
    Label(L"Bucket", y);
    Edit(IDC_S3BUCKET, kCX, y, 170);
    Add(WC_STATICW, L"Region", SS_RIGHT | SS_NOPREFIX, IDC_STATIC, kCX + 172, y + 4, 52, 18);
    SendMessageW(Edit(IDC_S3REGION, kCX + 230, y, kFW - 230), EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"eu-west-1"));
    y += 32;
    Label(L"Folder", y);
    SendMessageW(Edit(IDC_S3PREFIX, kCX, y, kFW), EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"The top of the bucket"));
    y += 32;
    Label(L"Access key ID", y);
    Edit(IDC_S3KEYID, kCX, y, kFW);
    y += 32;
    Label(L"Secret key", y);
    Edit(IDC_S3SECRET, kCX, y, kFW, ES_PASSWORD);
    y += 32;
    Label(L"Link address", y);
    SendMessageW(Edit(IDC_S3LINK, kCX, y, kFW), EM_SETCUEBANNER, TRUE,
                 reinterpret_cast<LPARAM>(L"Optional: https://files.example.com"));
    y += 32;
    Add(WC_BUTTONW, L"Private bucket: links work for 7 days", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_S3PRIVATE, kCX, y + 3,
        kFW - 92, 20);
    Button(IDC_S3TEST, L"Test", kR - 84, y, 84, 26);
    y += 30;
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_S3STATUS, kCX, y, kFW, 18);
    U.buildGroup = 0;

    // Image tools, Renamer
    y = BeginTab(kSettingsTabUploads + 1);
    Button(IDC_OPENRESIZE, L"Open Resize images…", kL, y, 170, 28);
    y = BeginTab(kSettingsTabUploads + 2);
    Button(IDC_OPENRENAME, L"Open Rename files…", kL, y, 170, 28);
    y = BeginTab(kSettingsTabUploads + 3);
    Button(IDC_OPENMAINT, L"Open Maintenance…", kL, y, 170, 28);
    y += 44;
    Check(IDC_MAINTDAILY, L"Check once a day and tell me when something needs doing", y);

    U.buildTab = -1;
    U.footerY = std::max(U.maxY + 18, 12 + kTabCount * kNavItem + 16);
    y = U.footerY + 12;
    const int bw = 84;
    Button(IDOK, L"OK", kW - kM - 3 * bw - 16, y, bw, 28, BS_DEFPUSHBUTTON);
    Button(IDCANCEL, L"Cancel", kW - kM - 2 * bw - 8, y, bw, 28);
    Button(IDC_APPLY, L"Apply", kW - kM - bw, y, bw, 28);
    U.height = y + 28 + 12;
}

void MakeFonts() {
    for (HFONT f : {U.font, U.bold, U.title})
        if (f) DeleteObject(f);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, U.dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    U.font = CreateFontIndirectW(&lf);
    const LONG h = lf.lfHeight;
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = MulDiv(h, 112, 100);
    U.bold = CreateFontIndirectW(&lf);
    lf.lfHeight = MulDiv(h, 160, 100);
    U.title = CreateFontIndirectW(&lf);
}

void Layout() {
    HDWP d = BeginDeferWindowPos(static_cast<int>(U.ctls.size()));
    for (const Ctl& c : U.ctls) {
        HFONT f = c.font == kFontTitle ? U.title : c.font == kFontBold ? U.bold : U.font;
        SendMessageW(c.wnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
        if (d) d = DeferWindowPos(d, c.wnd, nullptr, Px(c.x), Px(c.y), Px(c.w), Px(c.h), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (d) EndDeferWindowPos(d);
    SendMessageW(U.nav, LB_SETITEMHEIGHT, 0, Px(kNavItem));
    InvalidateRect(U.hwnd, nullptr, TRUE);
}

SIZE WindowSize() {
    RECT r{0, 0, Px(kW), Px(U.height)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, U.dpi);
    return {r.right - r.left, r.bottom - r.top};
}

// "Turned off" next to the title of a feature tab that is off.
void ShowOffNote() {
    for (int f = 0; f < kFeatCount; ++f)
        ShowWindow(Item(IDC_OFFNOTE0 + f), kTabs[U.tab].feature == f && !U.on[f] ? SW_SHOWNA : SW_HIDE);
}

// Which service's boxes the Uploads tab shows: 1 Google Drive, 2 Amazon S3.
int UploadGroup() { return SendMessageW(Item(IDC_UPSERVICE), CB_GETCURSEL, 0, 0) == 1 ? 2 : 1; }

void ShowTab(int t) {
    U.tab = g_lastTab = std::clamp(t, 0, kTabCount - 1);
    const int group = UploadGroup();
    for (const Ctl& c : U.ctls)
        if (c.tab >= 0)
            ShowWindow(c.wnd, c.tab == U.tab && (!c.group || c.group == group) ? SW_SHOWNA : SW_HIDE);
    if (SendMessageW(U.nav, LB_GETCURSEL, 0, 0) != U.tab) SendMessageW(U.nav, LB_SETCURSEL, U.tab, 0);
    HWND f = GetFocus();
    if (f && IsChild(U.hwnd, f) && !IsWindowVisible(f)) SetFocus(U.nav);
    ShowOffNote();
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
    const std::wstring* saved[kHotkeyCount] = {&g_settings.hkRegion, nullptr, &g_settings.hkFull,
                                               &g_settings.hkColor, &g_settings.hkGif, &g_settings.hkVideo};
    for (int i = 0; i < kHotkeyCount; ++i)
        if (saved[i] && FeatureOn(kHkFeature[i])) ParseHotkey(*saved[i], owned[i]);
    for (int i = 0; i < kHotkeyCount; ++i) {
        if (!Shown(i)) continue;
        std::wstring st;
        bool bad = false;
        const Hotkey& hk = U.hk[i];
        if (!HotkeyOn(i)) {
            // the whole tab is greyed out
        } else if (!hk.vk) {
            st = L"Off";
        } else {
            for (int j = 0; j < kHotkeyCount && !bad; ++j)
                if (j != i && HotkeyOn(j) && U.hk[j].vk && SameKey(hk, U.hk[j])) {
                    st = std::wstring(L"Same as ") + kHkNames[j];
                    bad = true;
                }
            const bool mine =
                std::any_of(owned, owned + kHotkeyCount, [&](const Hotkey& o) { return o.vk && SameKey(hk, o); });
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
    // A feature that is off: everything on its tab but the title and the switch is greyed out.
    for (const Ctl& c : U.ctls)
        if (c.tab > 0 && !c.header) EnableWindow(c.wnd, U.on[kTabs[c.tab].feature]);
    const bool save = Checked(IDC_SAVE);
    for (int id : {IDC_FOLDER, IDC_BROWSE, IDC_NAME, IDC_TOKENS, IDC_EXAMPLE, IDC_FORMAT})
        EnableWindow(Item(id), save);
    for (int id : {IDC_QLABEL, IDC_QUALITY, IDC_QSPIN}) EnableWindow(Item(id), save && JpgSelected());
}

void Toggle(int f) {
    U.on[f] = !U.on[f];
    ShowOffNote();
    InvalidateRect(U.nav, nullptr, TRUE);
    UpdateEnable();
    CheckHotkeys();
}

// Secret boxes stay empty: typing replaces the saved secret, which is never shown again.
void SecretBoxes() {
    const std::pair<int, const wchar_t*> boxes[] = {{IDC_DRVSECRET, L"DriveClientSecret"}, {IDC_S3SECRET, L"S3SecretKey"}};
    for (const auto& [id, name] : boxes) {
        SetWindowTextW(Item(id), L"");
        SendMessageW(Item(id), EM_SETCUEBANNER, TRUE,
                     reinterpret_cast<LPARAM>(Upload_LoadSecret(name).empty() ? L"Paste it here"
                                                                               : L"Saved (type to replace)"));
    }
}

void DriveStatus(const std::wstring& error) {
    std::wstring text;
    const std::wstring account = Upload_DriveAccount();
    if (!error.empty()) text = error;
    else if (Upload_DriveConnecting()) text = L"Waiting for the browser: sign in and allow access…";
    else if (!account.empty()) text = L"Connected: " + account;
    else text = L"Not connected";
    U.driveBad = !error.empty();
    SetWindowTextW(Item(IDC_DRVSTATUS), text.c_str());
    SetWindowTextW(Item(IDC_DRVCONNECT), account.empty() ? L"Connect…" : L"Disconnect");
    EnableWindow(Item(IDC_DRVCONNECT), !Upload_DriveConnecting() && U.on[kFeatUpload]);
    InvalidateRect(Item(IDC_DRVSTATUS), nullptr, TRUE);
}

void S3Status(const std::wstring& text, bool bad) {
    U.s3Bad = bad;
    SetWindowTextW(Item(IDC_S3STATUS), text.c_str());
    InvalidateRect(Item(IDC_S3STATUS), nullptr, TRUE);
}

void UploadHelp() {
    if (UploadGroup() == 1) {
        const int r = MessageBoxW(
            U.hwnd,
            L"One-time setup, about 5 minutes, in your own Google account:\n\n"
            L"1. Open console.cloud.google.com and make a project (any name, e.g. Canivete).\n"
            L"2. APIs & Services → Library → Google Drive API → Enable.\n"
            L"3. Google Auth Platform (OAuth consent screen): app name Canivete, your e-mail, audience "
            L"External.\n"
            L"4. Audience → Publish app (In production). Important: while it stays in Testing, Google signs "
            L"Canivete out every 7 days. It doesn't need Google's verification for your own use: the first "
            L"sign-in shows a warning, click Advanced → Go to Canivete.\n"
            L"5. Clients → Create client → Desktop app. Copy its Client ID and Client secret into these boxes.\n"
            L"6. Click Connect… and allow access in the browser.\n\n"
            L"Canivete only sees the files it uploads itself, in the Drive folder set here.\n\n"
            L"Open the Google Cloud console now?",
            L"Google Drive uploads", MB_YESNO | MB_ICONINFORMATION);
        if (r == IDYES)
            ShellExecuteW(U.hwnd, L"open", L"https://console.cloud.google.com/apis/credentials", nullptr, nullptr,
                          SW_SHOWNORMAL);
        return;
    }
    std::wstring bucket = Trim(GetText(IDC_S3BUCKET)), prefix = Trim(GetText(IDC_S3PREFIX));
    if (bucket.empty()) bucket = L"YOUR-BUCKET";
    while (!prefix.empty() && (prefix.front() == L'/' || prefix.front() == L'\\')) prefix.erase(0, 1);
    if (!prefix.empty() && prefix.back() != L'/') prefix += L'/';
    const std::wstring policy =
        L"{\"Version\":\"2012-10-17\",\"Statement\":[{\"Effect\":\"Allow\",\"Action\":[\"s3:PutObject\","
        L"\"s3:GetObject\"],\"Resource\":\"arn:aws:s3:::" +
        bucket + L"/" + prefix + L"*\"}]}";
    const int r = MessageBoxW(
        U.hwnd,
        (L"One-time setup in the AWS console:\n\n"
         L"1. S3 → Create bucket, in a region near you (e.g. eu-west-1). Put the bucket name and region here.\n"
         L"2. Public links: allow public reads of the folder (bucket policy, or CloudFront: put its address in "
         L"Link address). Or keep the bucket private and tick Private bucket: links then work for 7 days.\n"
         L"3. IAM → Users → Create user (e.g. canivete-upload), without console access.\n"
         L"4. Give it an inline policy that only allows this folder of this bucket:\n" +
         policy +
         L"\n5. Security credentials → Create access key (\"Application running outside AWS\"). Copy the key ID "
         L"and secret key here. This kind of key doesn't expire. Don't use temporary keys (from SSO or a role): "
         L"they stop working after a few hours.\n6. Click Test.\n\nCopy the policy to the clipboard?")
            .c_str(),
        L"Amazon S3 uploads", MB_YESNO | MB_ICONINFORMATION);
    if (r == IDYES && OpenClipboard(U.hwnd)) {
        EmptyClipboard();
        const size_t bytes = (policy.size() + 1) * sizeof(wchar_t);
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            memcpy(GlobalLock(g), policy.c_str(), bytes);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        }
        CloseClipboard();
    }
}

void Populate() {
    const Settings& s = g_settings;
    for (int f = 0; f < kFeatCount; ++f) U.on[f] = s.features[f];
    ParseHotkey(s.hkRegion, U.hk[0]);
    U.hk[1] = {};  // Window screenshot: now part of the region tool
    ParseHotkey(s.hkFull, U.hk[2]);
    ParseHotkey(s.hkColor, U.hk[3]);
    ParseHotkey(s.hkGif, U.hk[4]);
    ParseHotkey(s.hkVideo, U.hk[5]);
    for (int i = 0; i < kHotkeyCount; ++i)
        if (Shown(i)) ShowHotkey(i);
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
    SetCheck(IDC_VSOUND, s.videoSound);
    SetCheck(IDC_VMIC, s.videoMic);
    SetCheck(IDC_AUTOSTART, AutostartEnabled());
    SetCheck(IDC_CHECKUPD, s.checkUpdates);
    SetCheck(IDC_MAINTDAILY, s.maintDaily);
    SendMessageW(Item(IDC_UPSERVICE), CB_SETCURSEL, s.uploadService == L"s3" ? 1 : 0, 0);
    SetCheck(IDC_UPSHOTS, s.uploadShots);
    SetCheck(IDC_UPGIF, s.uploadGif);
    SetCheck(IDC_UPVIDEO, s.uploadVideo);
    SetWindowTextW(Item(IDC_DRVID), s.driveClientId.c_str());
    SetWindowTextW(Item(IDC_DRVFOLDER), s.driveFolder.c_str());
    SetCheck(IDC_DRVSHARE, s.driveShare);
    SetWindowTextW(Item(IDC_S3BUCKET), s.s3Bucket.c_str());
    SetWindowTextW(Item(IDC_S3REGION), s.s3Region.c_str());
    SetWindowTextW(Item(IDC_S3PREFIX), s.s3Prefix.c_str());
    SetWindowTextW(Item(IDC_S3KEYID), s.s3KeyId.c_str());
    SetWindowTextW(Item(IDC_S3LINK), s.s3LinkBase.c_str());
    SetCheck(IDC_S3PRIVATE, s.s3Private);
    SecretBoxes();
    SendMessageW(Item(IDC_THEME), CB_SETCURSEL, s.theme == L"light" ? 1 : s.theme == L"dark" ? 2 : 0, 0);
    CheckHotkeys();
    UpdateEnable();
    DriveStatus(L"");
    UpdateExample();
    InvalidateRect(U.hwnd, nullptr, TRUE);
}

void Apply() {
    Settings s = g_settings;
    for (int f = 0; f < kFeatCount; ++f) s.features[f] = U.on[f];
    std::wstring* hks[kHotkeyCount] = {&s.hkRegion, &s.hkWindow, &s.hkFull, &s.hkColor, &s.hkGif, &s.hkVideo};
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
    s.videoSound = Checked(IDC_VSOUND);
    s.videoMic = Checked(IDC_VMIC);
    s.checkUpdates = Checked(IDC_CHECKUPD);
    s.maintDaily = Checked(IDC_MAINTDAILY);
    const LRESULT cf = SendMessageW(Item(IDC_COLORFMT), CB_GETCURSEL, 0, 0);
    if (cf >= 0 && cf < static_cast<LRESULT>(std::size(kColorFormats))) s.colorFormat = kColorFormats[cf].key;
    s.uploadService = UploadGroup() == 2 ? L"s3" : L"drive";
    s.uploadShots = Checked(IDC_UPSHOTS);
    s.uploadGif = Checked(IDC_UPGIF);
    s.uploadVideo = Checked(IDC_UPVIDEO);
    s.driveClientId = Trim(GetText(IDC_DRVID));
    s.driveFolder = Trim(GetText(IDC_DRVFOLDER));
    s.driveShare = Checked(IDC_DRVSHARE);
    s.s3Bucket = Trim(GetText(IDC_S3BUCKET));
    s.s3Region = Trim(GetText(IDC_S3REGION));
    for (auto& c : s.s3Region) c = static_cast<wchar_t>(towlower(c));
    s.s3Prefix = Trim(GetText(IDC_S3PREFIX));
    s.s3KeyId = Trim(GetText(IDC_S3KEYID));
    s.s3LinkBase = Trim(GetText(IDC_S3LINK));
    s.s3Private = Checked(IDC_S3PRIVATE);
    // A new Client ID belongs with a new sign-in.
    if (s.driveClientId != g_settings.driveClientId && !Upload_DriveAccount().empty()) Upload_DriveDisconnect();
    const std::pair<int, const wchar_t*> secrets[] = {{IDC_DRVSECRET, L"DriveClientSecret"},
                                                      {IDC_S3SECRET, L"S3SecretKey"}};
    bool newSecret = false;
    for (const auto& [id, name] : secrets) {
        const std::wstring v = Trim(GetText(id));
        if (v.empty()) continue;
        Upload_SaveSecret(name, v);
        newSecret = true;
    }
    if (newSecret) SecretBoxes();
    SaveSettings(s);
    const bool autostart = Checked(IDC_AUTOSTART);
    if (autostart != AutostartEnabled()) SetAutostart(autostart);
    Settings_Applied();
    SetDlgItemInt(U.hwnd, IDC_QUALITY, s.jpegQuality, FALSE);
    CheckHotkeys();
    DriveStatus(L"");
}

// ---- drawing the tab list and the On switches ----------------------------------------------

void FillColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// Smooth (antialiased) shapes through GDI+; plain GDI draws rounded corners jagged.
Gdiplus::Color GpColor(COLORREF c) { return Gdiplus::Color(255, GetRValue(c), GetGValue(c), GetBValue(c)); }

struct Smooth {
    Gdiplus::Graphics g;
    explicit Smooth(HDC dc) : g(dc) {
        static ULONG_PTR token = 0;
        if (!token) {
            Gdiplus::GdiplusStartupInput in;
            Gdiplus::GdiplusStartup(&token, &in, nullptr);
        }
        g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
        g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    }
};

// A rounded rectangle; radius = h/2 or more makes a pill. Outline drawn inside the rect.
void RoundShape(Gdiplus::Graphics& g, Gdiplus::RectF r, float radius, COLORREF fill, COLORREF line, float lineW) {
    if (lineW > 0) {
        const float in = lineW / 2;
        r.X += in;
        r.Y += in;
        r.Width -= lineW;
        r.Height -= lineW;
    }
    radius = std::min({radius, r.Width / 2, r.Height / 2});
    const float d = radius * 2;
    Gdiplus::GraphicsPath p;
    p.AddArc(r.X, r.Y, d, d, 180, 90);
    p.AddArc(r.X + r.Width - d, r.Y, d, d, 270, 90);
    p.AddArc(r.X + r.Width - d, r.Y + r.Height - d, d, d, 0, 90);
    p.AddArc(r.X, r.Y + r.Height - d, d, d, 90, 90);
    p.CloseFigure();
    Gdiplus::SolidBrush b(GpColor(fill));
    g.FillPath(&b, &p);
    if (lineW > 0) {
        Gdiplus::Pen pen(GpColor(line), lineW);
        g.DrawPath(&pen, &p);
    }
}

Gdiplus::RectF ToRectF(const RECT& r) {
    return Gdiplus::RectF(static_cast<float>(r.left), static_cast<float>(r.top), static_cast<float>(r.right - r.left),
                          static_cast<float>(r.bottom - r.top));
}

void RoundFill(HDC dc, const RECT& r, int radius, COLORREF fill, COLORREF line) {
    Smooth s(dc);
    RoundShape(s.g, ToRectF(r), radius / 2.0f, fill, line, line == fill ? 0.0f : 1.0f);
}

// The small On switch at the right end of a tab in the list.
RECT SwitchRect(const RECT& item) {
    const int w = Px(30), h = Px(16);
    const int right = item.right - Px(10), top = (item.top + item.bottom - h) / 2;
    return {right - w, top, right, top + h};
}

void DrawSwitch(HDC dc, const RECT& pill, bool on) {
    Smooth s(dc);
    const Gdiplus::RectF r = ToRectF(pill);
    const float h = r.Height;
    const float line = std::max(1.0f, Px(10) / 10.0f);
    const Palette& p = Pal();
    if (on) RoundShape(s.g, r, h / 2, p.accent, p.accent, 0);
    else RoundShape(s.g, r, h / 2, p.nav, p.switchLine, line);
    // Knob like Windows 11: a bit bigger when on.
    const float k = on ? h - Px(8) : h - Px(9);
    const float pad = (h - k) / 2;
    const float kx = on ? r.X + r.Width - pad - k : r.X + pad;
    Gdiplus::SolidBrush kb(GpColor(on ? p.onAccent : p.switchKnob));
    s.g.FillEllipse(&kb, kx, r.Y + pad, k, k);
}

void DrawNavItem(const DRAWITEMSTRUCT& d) {
    if (d.itemID == static_cast<UINT>(-1)) return;
    HDC dc = d.hDC;
    const Palette& p = Pal();
    FillColor(dc, d.rcItem, p.nav);
    RECT b = d.rcItem;
    InflateRect(&b, -Px(2), -Px(2));
    if (d.itemState & ODS_SELECTED) {
        RoundFill(dc, b, Px(8), p.navSel, p.navSel);
        RECT bar{b.left, b.top + Px(9), b.left + Px(3), b.bottom - Px(9)};
        FillColor(dc, bar, p.accent);
    }
    const TabDef& t = kTabs[d.itemID];
    const bool off = t.feature >= 0 && !U.on[t.feature];
    HGDIOBJ of = SelectObject(dc, U.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, off ? p.grey : p.text);
    RECT tr = b;
    tr.left += Px(14);
    tr.right -= Px(10);
    if (t.feature >= 0) {
        const RECT sw = SwitchRect(d.rcItem);
        DrawSwitch(dc, sw, !off);
        tr.right = sw.left - Px(6);
    }
    DrawTextW(dc, t.name, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    SelectObject(dc, of);
    if ((d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT)) {
        SetTextColor(dc, p.text);
        SetBkColor(dc, p.nav);
        DrawFocusRect(dc, &b);
    }
}

// Click a switch in the tab list to turn its feature on or off; Space does it for the selected tab.
LRESULT CALLBACK NavProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (m) {
    case WM_LBUTTONDOWN:
    case WM_LBUTTONDBLCLK: {
        const LRESULT hit = SendMessageW(h, LB_ITEMFROMPOINT, 0, lp);
        const int i = LOWORD(hit);
        if (HIWORD(hit) || i < 0 || i >= kTabCount || kTabs[i].feature < 0) break;
        RECT item;
        SendMessageW(h, LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&item));
        RECT sw = SwitchRect(item);
        InflateRect(&sw, Px(6), Px(6));  // a little easier to hit
        const POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (!PtInRect(&sw, p)) break;
        SetFocus(h);
        ShowTab(i);
        Toggle(kTabs[i].feature);
        return 0;
    }
    case WM_KEYDOWN:
        if (wp == VK_SPACE) {
            if (kTabs[U.tab].feature >= 0) Toggle(kTabs[U.tab].feature);
            return 0;
        }
        break;
    case WM_CHAR:
        if (wp == L' ') return 0;
        break;
    }
    return DefSubclassProc(h, m, wp, lp);
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
        Build();
        Layout();
        return 0;
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        const Palette& p = Pal();
        FillColor(dc, rc, p.window);
        RECT nav{0, 0, Px(kNavW), Px(U.footerY)};
        FillColor(dc, nav, p.nav);
        RECT f = rc;
        f.top = Px(U.footerY);
        FillColor(dc, f, p.footer);
        f.bottom = f.top + 1;
        FillColor(dc, f, p.line);
        return 1;
    }
    case WM_MEASUREITEM: {
        auto* m = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (m->CtlType == ODT_LISTBOX) m->itemHeight = Px(kNavItem);
        return TRUE;
    }
    case WM_DRAWITEM: {
        const auto* d = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (d->CtlType == ODT_LISTBOX) DrawNavItem(*d);
        return TRUE;
    }
    case WM_CTLCOLORLISTBOX:
        return reinterpret_cast<LRESULT>(Theme_Brush(Pal().nav));
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        const Palette& p = Pal();
        const bool hotkeyBox = id >= IDC_HK0 && id < IDC_HK0 + kHotkeyCount;
        COLORREF bg = hotkeyBox ? p.edit : p.window, fg = p.text;
        if (hotkeyBox && U.capturing == id - IDC_HK0) {
            bg = p.focusBg;
        } else if (hotkeyBox && !IsWindowEnabled(reinterpret_cast<HWND>(lp))) {
            bg = p.window;
            fg = p.grey;
        } else if (id >= IDC_HKST0 && id < IDC_HKST0 + kHotkeyCount) {
            fg = U.bad[id - IDC_HKST0] ? p.red : p.grey;
        } else if (id == IDC_DRVSTATUS || id == IDC_S3STATUS) {
            fg = (id == IDC_DRVSTATUS ? U.driveBad : U.s3Bad) ? p.red : p.grey;
        } else if (id == IDC_HINT || id == IDC_EXAMPLE || id == IDC_DESC ||
                   (id >= IDC_OFFNOTE0 && id < IDC_OFFNOTE0 + static_cast<int>(kFeatCount))) {
            fg = p.grey;
        }
        SetBkColor(dc, bg);
        SetTextColor(dc, fg);
        return reinterpret_cast<LRESULT>(Theme_Brush(bg));
    }
    case WM_CTLCOLORBTN: {  // push buttons: the corners around their rounded shape
        RECT r;
        GetWindowRect(reinterpret_cast<HWND>(lp), &r);
        MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&r), 2);
        return reinterpret_cast<LRESULT>(Theme_Brush(r.top >= Px(U.footerY) ? Pal().footer : Pal().window));
    }
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
            // Never come back into a hotkey box: that switches every hotkey off while Settings is in
            // front, so a capture hotkey pressed after returning to Settings would silently do nothing.
            const int lastId = U.lastFocus ? GetDlgCtrlID(U.lastFocus) : 0;
            const bool hkBox = lastId >= IDC_HK0 && lastId < IDC_HK0 + kHotkeyCount;
            const bool ok = U.lastFocus && !hkBox && IsWindow(U.lastFocus) && IsWindowVisible(U.lastFocus) &&
                            IsWindowEnabled(U.lastFocus);
            SetFocus(ok ? U.lastFocus : U.nav);
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
        case IDC_NAV:
            if (code == LBN_SELCHANGE) ShowTab(static_cast<int>(SendMessageW(U.nav, LB_GETCURSEL, 0, 0)));
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
        case IDC_OPENRESIZE:
            Resizer_Open({});
            break;
        case IDC_OPENRENAME:
            Renamer_Open({});
            break;
        case IDC_OPENMAINT:
            Maint_Show();
            break;
        case IDC_UPDNOW:
            Update_Check(true);
            break;
        case IDC_UPSERVICE:
            if (code == CBN_SELCHANGE) ShowTab(U.tab);
            break;
        case IDC_UPHELP:
            UploadHelp();
            break;
        case IDC_DRVCONNECT:
            if (!Upload_DriveAccount().empty()) {
                Upload_DriveDisconnect();
                DriveStatus(L"");
                break;
            }
            Apply();
            if (g_settings.driveClientId.empty() || Upload_LoadSecret(L"DriveClientSecret").empty()) {
                DriveStatus(L"Fill in the Client ID and Client secret first (How to set it up…).");
                break;
            }
            Upload_DriveConnect();
            DriveStatus(L"");
            break;
        case IDC_S3TEST: {
            Apply();
            const Settings& s = g_settings;
            if (s.s3Bucket.empty() || s.s3Region.empty() || s.s3KeyId.empty() ||
                Upload_LoadSecret(L"S3SecretKey").empty()) {
                S3Status(L"Fill in the bucket, region, access key ID and secret key first.", true);
                break;
            }
            S3Status(L"Testing…", false);
            Upload_S3Test();
            break;
        }
        case IDC_THEME:  // applies at once, to every open window
            if (code == CBN_SELCHANGE) {
                const LRESULT i = SendMessageW(Item(IDC_THEME), CB_GETCURSEL, 0, 0);
                g_settings.theme = i == 1 ? L"light" : i == 2 ? L"dark" : L"system";
                SaveSettingValue(L"General", L"Theme", g_settings.theme);
                Theme_Update();
            }
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
        U.hwnd = U.nav = nullptr;
        U.ctls.clear();
        U.lastFocus = nullptr;
        U.maxY = 0;
        return 0;
    case WM_NCDESTROY:
        for (HGDIOBJ o : {static_cast<HGDIOBJ>(U.font), static_cast<HGDIOBJ>(U.bold), static_cast<HGDIOBJ>(U.title)})
            if (o) DeleteObject(o);
        U.font = U.bold = U.title = nullptr;
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

HWND SettingsUI_Window() { return U.hwnd; }

// Ctrl+Tab / Ctrl+Shift+Tab and Ctrl+PageDown / Ctrl+PageUp switch tabs.
bool SettingsUI_Key(const MSG& m) {
    if (!U.hwnd || m.message != WM_KEYDOWN || U.capturing >= 0) return false;
    if (m.hwnd != U.hwnd && !IsChild(U.hwnd, m.hwnd)) return false;
    if (GetKeyState(VK_CONTROL) >= 0) return false;
    int step = 0;
    if (m.wParam == VK_TAB) step = GetKeyState(VK_SHIFT) < 0 ? -1 : 1;
    else if (m.wParam == VK_NEXT) step = 1;
    else if (m.wParam == VK_PRIOR) step = -1;
    if (!step) return false;
    ShowTab((U.tab + step + kTabCount) % kTabCount);
    return true;
}

void SettingsUI_UploadStatus(const std::wstring& error) {
    if (!U.hwnd) return;
    if (UploadGroup() == 2) S3Status(error.empty() ? L"It works: a test file went up." : error, !error.empty());
    else DriveStatus(error);
}

void SettingsUI_Show(int tab) {
    if (U.hwnd) {
        if (IsIconic(U.hwnd)) ShowWindow(U.hwnd, SW_RESTORE);
        if (tab >= 0) ShowTab(tab);
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
    ShowTab(tab >= 0 ? tab : g_lastTab);
    Theme_Window(h);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}
