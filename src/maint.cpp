// Maintenance window: Overview (everything that needs doing, in one list), Updates (Windows, drivers,
// apps), Installed apps, Cleanup, Startup apps and Health, as tabs in a list on the left like Settings.
// The last results are kept in maintenance.ini, which the daily background check also writes. Checks run on worker threads; jobs that need administrator
// rights run in an elevated copy of Canivete (one Windows prompt per job) that reports back here
// with WM_COPYDATA. Built in code so it lays out again at any monitor DPI.
#include "maint.h"
#include "fluent.h"
#include "theme.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <ctime>
#include <map>
#include <memory>
#include <set>
#include <thread>

using namespace maint;

namespace {

constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN;
constexpr DWORD kExStyle = WS_EX_CONTROLPARENT;
constexpr UINT WM_M_SCAN = WM_APP + 30;      // wp = MAKEWPARAM(tab, gen), lp = new'd ScanResult
constexpr UINT WM_M_PROGRESS = WM_APP + 31;  // wp = MAKEWPARAM(tab, gen), lp = new'd Progress
constexpr UINT WM_M_DONE = WM_APP + 32;      // wp = MAKEWPARAM(tab, gen), lp = exit code
constexpr LPARAM kDeclined = -1000, kNoStart = -1001;
constexpr COLORREF kDotColors[4] = {RGB(16, 137, 62), RGB(222, 145, 0), RGB(196, 43, 28), RGB(160, 160, 160)};
// Layout in 96-DPI pixels.
constexpr int kW = 1000, kH = 680, kMinW = 800, kMinH = 560;
constexpr int kNavW = 220, kNavItem = 40;
constexpr int kCtlH = 32;  // buttons and the search box
constexpr int kRowH = 40;  // list rows

constexpr int kOverview = 0, kUpdates = 1, kApps = 2, kCleanup = 3, kStartup = 4, kHealth = 5, kTabCount = 6;
constexpr int kOk = 0, kWarn = 1, kBad = 2, kInfo = 3;  // Overview row levels (dot colors)

struct TabDef {
    const wchar_t* name;
    const wchar_t* desc;
};
const TabDef kTabs[kTabCount] = {
    {L"Overview", L"Everything that keeps this PC up to date and healthy. Double-click a row to deal with it."},
    {L"Updates", L"Windows, driver and app updates. Nothing is installed until you press Install."},
    {L"Installed apps", L"Every app installed on this PC. Uninstall runs the app's own uninstaller."},
    {L"Cleanup", L"Space used by temporary files. Nothing is deleted until you press Clean; files in use are skipped."},
    {L"Startup apps", L"Apps that start when you sign in. Turning one off doesn't remove it; you can turn it on again."},
    {L"Health", L"Restart advice, memory and disk space, a virus scan, and Windows' own repair and optimize tools."},
};

enum : int {
    IDC_NAV = 100,
    IDC_TITLE,
    IDC_DESC,
    IDC_LIST0 = 110,    // one list per tab (Health has none)
    IDC_STATUS0 = 120,  // one status line per tab
    IDC_UPGPU = 130,
    IDC_UPGPUBTN,
    IDC_UPRESTORE,
    IDC_UPREBOOT,
    IDC_UPREBOOTBTN,
    IDC_UPCHECK,
    IDC_UPSTORE,
    IDC_UPINSTALL,
    IDC_CLMEASURE = 150,
    IDC_CLSTORAGE,
    IDC_CLRUN,
    IDC_STREFRESH = 160,
    IDC_STTASKMGR,
    IDC_STAPPLY,
    IDC_HEINFO = 170,
    IDC_HERESTART,
    IDC_HEREPAIR,
    IDC_HEOPT,
    IDC_HERESTORE,
    IDC_HELOG,
    IDC_HESCAN,
    IDC_OVCHECK = 180,
    IDC_OVDO,
    IDC_APSEARCH = 190,
    IDC_APREFRESH,
    IDC_APSETTINGS,
    IDC_APUNINSTALL,
    IDC_JOBTEXT = 200,  // the job panel: one line of text, the progress bar, Cancel / Close
    IDC_JOBBAR,
    IDC_JOBCANCEL,
};
constexpr int kPanel = -2;     // Ctl::tab of the job panel's controls: shown on the job's tab
constexpr int kPanelH = 60;    // its height (96-DPI pixels), taken from the bottom of the tab's content
constexpr wchar_t kBarClass[] = L"CaniveteMaintBar";

enum FontKind : int { kFontNormal, kFontBold, kFontTitle, kFontCaption };

struct Ctl {
    HWND wnd;
    int tab;  // -1 = all tabs
    int font;
};

struct OverviewData {
    SecurityInfo sec;
    std::vector<DeviceProblem> devices;
    BatteryInfo battery;
    int lastUpdateDays = -1;
    Health health;
    std::vector<GpuInfo> gpus;
};
struct OvRow {
    std::wstring key, what, state, todo;
    int level;
};
struct ScanResult {
    UpdateScan up;
    std::vector<CleanItem> clean;
    std::vector<StartupItem> startup;
    OverviewData ov;
    std::vector<InstalledApp> apps;
    Health health;        // the Health tab
    bool reboot = false;  // after an updates check: Windows waits for a restart
};

// The one job that may run at a time (installing, cleaning, …). Its panel (a line of text, the
// progress bar and Cancel) shows on its tab; after the job it shows the result until closed.
struct JobState {
    int tab = -1;            // the tab showing the panel; -1 = no panel
    bool running = false;
    bool admin = false;      // runs in the elevated copy
    bool cancellable = false, cancelling = false;
    bool wasFront = false;   // the window was in front when the job started
    bool locked = false;     // LockSetForegroundWindow is on (other apps can't jump in front)
    std::wstring task, name; // name: the app being uninstalled
    int pct = -1;            // thousandths of the whole job; -1 = no number known (moving bar)
    int step = 0, steps = 0;
    std::wstring stepKey, stepWhat;
    ULONGLONG freed = 0;
    bool haveFreed = false;
    std::vector<std::wstring> ok, failed, skipped;    // item keys, as they ended
    std::map<std::wstring, std::wstring> itemText;    // key -> its row's last text
    std::wstring text;       // the panel's line
    int outcome = 0;         // when it ended: 0 fine, 1 problems, 2 stopped or not allowed
} J;
// The daily check, done in the background without the window.
struct BgResult {
    UpdateScan up;
    SecurityInfo sec;
};
bool g_bgBusy = false;
long long g_bgRetryAt = 0;  // the daily check was put off (battery, game, offline): not before this
struct Progress {
    std::wstring key, text;
};

struct UI {
    HWND hwnd = nullptr, nav = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr, title = nullptr, caption = nullptr;  // shared, never deleted
    int lineH = 20;  // text height of font
    std::vector<Ctl> ctls;
    std::vector<std::pair<int, RECT>> cards;  // tab, rectangle (device pixels) of the cards behind lists
    RECT searchFrame{};                       // the search box's frame (device pixels)
    int navHot = -1;                          // tab under the mouse
    int tab = 0;
    WORD gen = 0;  // a new number each time the window opens: results for an older window are dropped
    bool busy[kTabCount]{};  // a check (scan) is running for the tab; jobs are in J
    bool scanned[kTabCount]{};
    HANDLE cancel = nullptr;           // the named event Cancel sets (see CancelEventName)
    ITaskbarList3* taskbar = nullptr;  // progress on the taskbar button
    bool filling = false;     // lists being filled: ignore check changes
    bool rebootNeeded = false;
    bool logPartial = false;  // the log's last line is a progress line the next one replaces
    UpdateScan up;
    std::vector<CleanItem> clean;
    std::vector<StartupItem> startup;
    Health health;
    OverviewData ov;
    std::vector<OvRow> ovRows;
    std::vector<InstalledApp> apps;
    std::vector<int> appRows;  // list row -> apps index (the search filters)
    HIMAGELIST dots = nullptr;
    HIMAGELIST rows = nullptr;  // an empty image list that makes the other lists' rows kRowH tall
    bool waitBg = false;  // Updates waits for the background check that is already running
    HWND lastFocus = nullptr;
} U;

int g_lastTab = 0;

int Px(int v) { return MulDiv(v, static_cast<int>(U.dpi), 96); }
HWND Item(int id) { return GetDlgItem(U.hwnd, id); }
HWND List(int t) { return Item(IDC_LIST0 + t); }
void SetText(int id, const std::wstring& t) { SetWindowTextW(Item(id), t.c_str()); }
void SetStatus(int t, const std::wstring& s) { SetText(IDC_STATUS0 + t, s); }

template <class T>
void PostOrDelete(HWND w, UINT msg, WPARAM wp, T* p) {
    if (!PostMessageW(w, msg, wp, reinterpret_cast<LPARAM>(p))) delete p;
}

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int tab, DWORD ex = 0, int font = kFontNormal) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | style, 0, 0, 0, 0, U.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hinst, nullptr);
    U.ctls.push_back({c, tab, font});
    return c;
}
HWND Button(int id, const wchar_t* text, int tab, DWORD style = BS_PUSHBUTTON) {
    return Add(WC_BUTTONW, text, style | WS_TABSTOP, id, tab);
}
HWND Static(int id, const wchar_t* text, int tab, DWORD style = SS_LEFT) {
    return Add(WC_STATICW, text, style | SS_NOPREFIX, id, tab);
}

HWND MakeList(int t, std::initializer_list<std::pair<const wchar_t*, int>> cols, bool checks) {
    HWND lv = Add(WC_LISTVIEWW, L"",
                  LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | LVS_SHAREIMAGELISTS | WS_TABSTOP |
                      (checks ? 0 : LVS_SINGLESEL),
                  IDC_LIST0 + t, t);
    SetWindowTheme(lv, L"Explorer", nullptr);
    ListView_SetExtendedListViewStyle(
        lv, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | (checks ? LVS_EX_CHECKBOXES : 0));
    int i = 0;
    for (const auto& [name, fmt] : cols) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_FMT | LVCF_WIDTH};
        c.fmt = fmt;
        c.cx = 100;
        c.pszText = const_cast<wchar_t*>(name);
        ListView_InsertColumn(lv, i++, &c);
    }
    return lv;
}

void Build() {
    U.nav = Add(WC_LISTBOXW, L"", LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_TABSTOP,
                IDC_NAV, -1);
    for (const TabDef& d : kTabs) SendMessageW(U.nav, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(d.name));
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, IDC_TITLE, -1, 0, kFontTitle);
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX, IDC_DESC, -1);
    for (int t = 0; t < kTabCount; ++t)
        Add(WC_STATICW, L"", SS_LEFT | SS_ENDELLIPSIS | SS_NOPREFIX, IDC_STATUS0 + t, t, 0, kFontCaption);

    MakeList(kOverview, {{L"What", LVCFMT_LEFT}, {L"State", LVCFMT_LEFT}, {L"To do", LVCFMT_LEFT}}, false);
    Button(IDC_OVCHECK, L"Check everything", kOverview);
    Button(IDC_OVDO, L"Do it", kOverview, BS_DEFPUSHBUTTON);

    HWND search = Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_APSEARCH, kApps);
    Theme_FramedEdit(search);
    SendMessageW(search, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"Search by name or publisher"));
    MakeList(kApps, {{L"Name", LVCFMT_LEFT}, {L"Publisher", LVCFMT_LEFT}, {L"Version", LVCFMT_LEFT},
                     {L"Size", LVCFMT_RIGHT}, {L"Installed", LVCFMT_LEFT}},
             false);
    Button(IDC_APREFRESH, L"Refresh", kApps);
    Button(IDC_APSETTINGS, L"Windows app settings", kApps);
    Button(IDC_APUNINSTALL, L"Uninstall…", kApps);

    MakeList(kUpdates, {{L"Update", LVCFMT_LEFT}, {L"Type", LVCFMT_LEFT}, {L"Version", LVCFMT_LEFT},
                        {L"Size", LVCFMT_RIGHT}, {L"Status", LVCFMT_LEFT}},
             true);
    Static(IDC_UPGPU, L"", kUpdates);
    Button(IDC_UPGPUBTN, L"Open NVIDIA app", kUpdates);
    HWND rp = Button(IDC_UPRESTORE, L"Make a restore point first", kUpdates, BS_AUTOCHECKBOX);
    Button_SetCheck(rp, BST_CHECKED);
    Static(IDC_UPREBOOT, L"A restart is needed to finish installing updates.", kUpdates, SS_RIGHT);
    Button(IDC_UPREBOOTBTN, L"Restart now…", kUpdates);
    Button(IDC_UPCHECK, L"Check again", kUpdates);
    Button(IDC_UPSTORE, L"Microsoft Store updates", kUpdates);
    Button(IDC_UPINSTALL, L"Install selected", kUpdates, BS_DEFPUSHBUTTON);

    MakeList(kCleanup, {{L"What", LVCFMT_LEFT}, {L"Size", LVCFMT_RIGHT}, {L"Details", LVCFMT_LEFT}, {L"Result", LVCFMT_LEFT}},
             true);
    Button(IDC_CLMEASURE, L"Measure again", kCleanup);
    Button(IDC_CLSTORAGE, L"Windows storage settings", kCleanup);
    Button(IDC_CLRUN, L"Clean selected", kCleanup, BS_DEFPUSHBUTTON);

    MakeList(kStartup, {{L"App", LVCFMT_LEFT}, {L"Starts for", LVCFMT_LEFT}, {L"Command", LVCFMT_LEFT},
                        {L"Result", LVCFMT_LEFT}},
             true);
    Button(IDC_STREFRESH, L"Refresh", kStartup);
    Button(IDC_STTASKMGR, L"Open Task Manager", kStartup);
    Button(IDC_STAPPLY, L"Apply changes", kStartup, BS_DEFPUSHBUTTON);

    Static(IDC_HEINFO, L"", kHealth);
    Button(IDC_HERESTART, L"Restart now…", kHealth);
    Button(IDC_HEREPAIR, L"Repair system files", kHealth);
    Button(IDC_HEOPT, L"Optimize drives", kHealth);
    Button(IDC_HERESTORE, L"Make a restore point", kHealth);
    Button(IDC_HESCAN, L"Quick virus scan", kHealth);
    HWND log = Add(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, IDC_HELOG,
                   kHealth);
    SendMessageW(log, EM_SETLIMITTEXT, 1 << 20, 0);

    Static(IDC_JOBTEXT, L"", kPanel, SS_LEFT | SS_ENDELLIPSIS);
    Add(kBarClass, L"", 0, IDC_JOBBAR, kPanel);
    Button(IDC_JOBCANCEL, L"Cancel", kPanel);
}

// Round colored dots for the Overview rows, drawn smooth at this DPI.
void MakeDots() {
    const int s = Px(16), H = Px(kRowH);
    HIMAGELIST il = ImageList_Create(s, H, ILC_COLOR32, 4, 0);
    for (COLORREF c : kDotColors) {
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = s;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void* bits = nullptr;
        HBITMAP color = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (!color) continue;
        auto* px = static_cast<uint32_t*>(bits);
        const double rad = s * 0.3, mid = s / 2.0, midY = H / 2.0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < s; ++x) {
                const double a = std::clamp(rad + 0.5 - std::hypot(x + 0.5 - mid, y + 0.5 - midY), 0.0, 1.0);
                px[y * s + x] = (static_cast<uint32_t>(a * 255 + 0.5) << 24) | (GetRValue(c) << 16) | (GetGValue(c) << 8) |
                                GetBValue(c);
            }
        HBITMAP mask = CreateBitmap(s, H, 1, 1, nullptr);
        ICONINFO ii{TRUE, 0, 0, mask, color};
        if (HICON ic = CreateIconIndirect(&ii)) {
            ImageList_ReplaceIcon(il, -1, ic);
            DestroyIcon(ic);
        }
        DeleteObject(mask);
        DeleteObject(color);
    }
    ListView_SetImageList(List(kOverview), il, LVSIL_SMALL);
    if (U.dots) ImageList_Destroy(U.dots);
    U.dots = il;
    // The other lists: a 1 px wide image list with no images, only there for the row height.
    HIMAGELIST rows = ImageList_Create(1, H, ILC_COLOR32, 0, 0);
    for (int t : {kUpdates, kApps, kCleanup, kStartup}) ListView_SetImageList(List(t), rows, LVSIL_SMALL);
    if (U.rows) ImageList_Destroy(U.rows);
    U.rows = rows;
}

void MakeFonts() {
    U.font = Fluent_Font(kTypeBody, U.dpi);
    U.bold = Fluent_Font(kTypeBodyStrong, U.dpi);
    U.title = Fluent_Font(kTypeTitle, U.dpi);
    U.caption = Fluent_Font(kTypeCaption, U.dpi);
    HDC dc = GetDC(U.hwnd);
    HGDIOBJ of = SelectObject(dc, U.font);
    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);
    SelectObject(dc, of);
    ReleaseDC(U.hwnd, dc);
    U.lineH = tm.tmHeight;
    for (const Ctl& c : U.ctls) {
        HFONT f = c.font == kFontTitle    ? U.title
                  : c.font == kFontBold    ? U.bold
                  : c.font == kFontCaption ? U.caption
                                           : U.font;
        SendMessageW(c.wnd, WM_SETFONT, reinterpret_cast<WPARAM>(f), FALSE);
    }
    SendMessageW(Item(IDC_APSEARCH), EM_SETMARGINS, EC_LEFTMARGIN | EC_RIGHTMARGIN, 0);
    MakeDots();
}

void Layout() {
    if (!U.hwnd) return;
    RECT rc;
    GetClientRect(U.hwnd, &rc);
    const int W = MulDiv(rc.right, 96, static_cast<int>(U.dpi)), H = MulDiv(rc.bottom, 96, static_cast<int>(U.dpi));
    const int L = kNavW + 12, R = W - 24, CW = R - L, top = 100;
    const int by = H - 20 - kCtlH;  // footer buttons
    HDWP d = BeginDeferWindowPos(static_cast<int>(U.ctls.size()));
    auto put = [&](int id, int x, int y, int w, int h) {
        if (d) d = DeferWindowPos(d, Item(id), nullptr, Px(x), Px(y), Px(std::max(w, 0)), Px(std::max(h, 0)),
                                  SWP_NOZORDER | SWP_NOACTIVATE);
    };
    // A card (painted by the window) with a control inside it.
    U.cards.clear();
    auto card = [&](int t, int id, int x, int y, int w, int h, int inset) {
        U.cards.push_back({t, RECT{Px(x), Px(y), Px(x + w), Px(y + h)}});
        put(id, x + inset, y + inset, w - 2 * inset, h - 2 * inset);
    };
    put(IDC_NAV, 8, 8, kNavW - 12, kTabCount * kNavItem + 4);
    put(IDC_TITLE, L, 14, CW, 44);
    put(IDC_DESC, L, 62, CW, 22);
    // Footer: status on the left, buttons on the right (widths in order from the right edge).
    auto footer = [&](int t, std::initializer_list<std::pair<int, int>> buttons) {
        int x = R;
        for (const auto& [id, w] : buttons) {
            x -= w;
            put(id, x, by, w, kCtlH);
            x -= 8;
        }
        put(IDC_STATUS0 + t, L, by + 8, x - L - 8, 18);
    };
    // The job panel sits above the footer of its tab; that tab's content ends higher.
    auto ph = [](int t) { return J.tab == t ? kPanelH : 0; };
    const int end = by - 16;  // bottom of a tab's content
    if (J.tab >= 0) {
        const int P = end - kPanelH + 12;
        const bool btn = !J.running || J.cancellable;
        const int tw = btn ? CW - 128 : CW;
        put(IDC_JOBTEXT, L, P, tw, 22);
        put(IDC_JOBBAR, L, P + 26, tw, 12);
        put(IDC_JOBCANCEL, R - 112, P + 2, 112, kCtlH);
    }
    // Overview, Installed apps
    footer(kOverview, {{IDC_OVDO, 140}, {IDC_OVCHECK, 160}});
    card(kOverview, IDC_LIST0 + kOverview, L, top, CW, end - top - ph(kOverview), 4);
    footer(kApps, {{IDC_APUNINSTALL, 120}, {IDC_APSETTINGS, 190}, {IDC_APREFRESH, 110}});
    U.searchFrame = {Px(L), Px(top), Px(L + 320), Px(top + kCtlH)};
    {
        const int x = U.searchFrame.left + Px(11), y = U.searchFrame.top + (Px(kCtlH) - U.lineH) / 2;
        if (d) d = DeferWindowPos(d, Item(IDC_APSEARCH), nullptr, x, y, U.searchFrame.right - Px(11) - x, U.lineH,
                                  SWP_NOZORDER | SWP_NOACTIVATE);
    }
    card(kApps, IDC_LIST0 + kApps, L, top + kCtlH + 12, CW, end - top - kCtlH - 12 - ph(kApps), 4);
    // Updates
    footer(kUpdates, {{IDC_UPINSTALL, 140}, {IDC_UPSTORE, 200}, {IDC_UPCHECK, 120}});
    const int rowY = end - kCtlH - ph(kUpdates);
    put(IDC_UPRESTORE, L, rowY + 4, 260, 24);
    put(IDC_UPREBOOTBTN, R - 140, rowY, 140, kCtlH);
    put(IDC_UPREBOOT, L + 270, rowY + 6, R - 148 - L - 270 - 8, 20);
    const int gpuY = rowY - 56;
    put(IDC_UPGPUBTN, R - 180, gpuY + 6, 180, kCtlH);
    put(IDC_UPGPU, L, gpuY, CW - 196, 44);
    card(kUpdates, IDC_LIST0 + kUpdates, L, top, CW, gpuY - 12 - top, 4);
    // Cleanup, Startup
    footer(kCleanup, {{IDC_CLRUN, 140}, {IDC_CLSTORAGE, 210}, {IDC_CLMEASURE, 140}});
    card(kCleanup, IDC_LIST0 + kCleanup, L, top, CW, end - top - ph(kCleanup), 4);
    footer(kStartup, {{IDC_STAPPLY, 140}, {IDC_STTASKMGR, 170}, {IDC_STREFRESH, 110}});
    card(kStartup, IDC_LIST0 + kStartup, L, top, CW, end - top - ph(kStartup), 4);
    // Health
    footer(kHealth, {});
    U.cards.push_back({kHealth, RECT{Px(L), Px(top), Px(R), Px(top + 144)}});
    put(IDC_HEINFO, L + 16, top + 12, CW - 32, 120);
    int x = L;
    for (const auto& [id, w] : {std::pair{IDC_HERESTART, 112}, {IDC_HEREPAIR, 152}, {IDC_HEOPT, 124}, {IDC_HERESTORE, 160},
                                {IDC_HESCAN, 136}}) {
        put(id, x, top + 156, w, kCtlH);
        x += w + 8;
    }
    const int logY = top + 156 + kCtlH + 12;
    card(kHealth, IDC_HELOG, L, logY, CW, end - logY - ph(kHealth), 6);
    if (d) EndDeferWindowPos(d);
    SendMessageW(U.nav, LB_SETITEMHEIGHT, 0, Px(kNavItem));

    // List columns: the first (or the widest text) column takes what the others leave.
    auto cols = [&](int t, std::initializer_list<int> fixed, int fill) {
        HWND lv = List(t);
        RECT lr;
        GetClientRect(lv, &lr);
        int used = 0, i = 0;
        for (int w : fixed) {
            if (i == fill) ++i;
            ListView_SetColumnWidth(lv, i, Px(w));
            used += Px(w);
            ++i;
        }
        // Room for the vertical scroll bar even before it shows, so no sideways one comes with it.
        const int sb = (GetWindowLongW(lv, GWL_STYLE) & WS_VSCROLL) ? 0 : GetSystemMetricsForDpi(SM_CXVSCROLL, U.dpi);
        ListView_SetColumnWidth(lv, fill, std::max(Px(120), static_cast<int>(lr.right) - sb - used - 2));
    };
    cols(kOverview, {170, 200}, 1);
    cols(kApps, {170, 110, 80, 90}, 0);
    cols(kUpdates, {80, 170, 70, 150}, 0);
    cols(kCleanup, {230, 100, 150}, 2);
    cols(kStartup, {200, 170, 150}, 2);
    InvalidateRect(U.hwnd, nullptr, TRUE);
}

// ---- tab state ---------------------------------------------------------------------------

// One job at a time: while one runs, every button that starts a job is off, and so is the check of
// the job's own tab (its list must stay as the job knows it). Checks of other tabs still work.
void UpdateButtons() {
    const bool job = J.running;
    auto idle = [&](int t) { return !U.busy[t] && !(job && J.tab == t); };  // the tab's check may run
    EnableWindow(Item(IDC_UPCHECK), idle(kUpdates));
    EnableWindow(Item(IDC_UPRESTORE), idle(kUpdates));
    EnableWindow(Item(IDC_UPINSTALL), idle(kUpdates) && !job && U.scanned[kUpdates]);
    EnableWindow(Item(IDC_CLMEASURE), idle(kCleanup));
    EnableWindow(Item(IDC_CLRUN), idle(kCleanup) && !job);
    EnableWindow(Item(IDC_STREFRESH), idle(kStartup));
    EnableWindow(Item(IDC_STAPPLY), idle(kStartup) && !job);
    for (int id : {IDC_HEREPAIR, IDC_HEOPT, IDC_HERESTORE, IDC_HESCAN}) EnableWindow(Item(id), !job);
    EnableWindow(Item(IDC_OVCHECK), !U.busy[kOverview]);
    const int ov = ListView_GetNextItem(List(kOverview), -1, LVNI_SELECTED);
    const bool can = ov >= 0 && ov < static_cast<int>(U.ovRows.size()) && !U.ovRows[ov].todo.empty() &&
                     !(job && U.ovRows[ov].key == L"scan");
    EnableWindow(Item(IDC_OVDO), can);
    SetText(IDC_OVDO, ov >= 0 && ov < static_cast<int>(U.ovRows.size()) && !U.ovRows[ov].todo.empty() ? U.ovRows[ov].todo : L"Do it");
    EnableWindow(Item(IDC_APREFRESH), idle(kApps));
    EnableWindow(Item(IDC_APUNINSTALL), idle(kApps) && !job && ListView_GetNextItem(List(kApps), -1, LVNI_SELECTED) >= 0);
    // The panel's button: Cancel while a job that can stop runs, Close once it has ended.
    SetText(IDC_JOBCANCEL, J.running ? (J.cancelling ? L"Stopping…" : L"Cancel") : L"Close");
    EnableWindow(Item(IDC_JOBCANCEL), !J.running || (J.cancellable && !J.cancelling));
}

void ShowTab(int t) {
    U.tab = g_lastTab = std::clamp(t, 0, kTabCount - 1);
    for (const Ctl& c : U.ctls) {
        bool show = c.tab == -1 || c.tab == U.tab;
        const int id = GetDlgCtrlID(c.wnd);
        if (c.tab == kPanel) show = J.tab >= 0 && J.tab == U.tab && (id != IDC_JOBCANCEL || !J.running || J.cancellable);
        if ((id == IDC_UPREBOOT || id == IDC_UPREBOOTBTN) && !U.rebootNeeded) show = false;
        if ((id == IDC_UPGPU || id == IDC_UPGPUBTN) && U.up.gpus.empty()) show = false;
        if (id == IDC_UPGPUBTN && (U.up.gpus.empty() || U.up.gpus[0].open.empty())) show = false;
        ShowWindow(c.wnd, show ? SW_SHOWNA : SW_HIDE);
    }
    SetText(IDC_TITLE, kTabs[U.tab].name);
    SetText(IDC_DESC, kTabs[U.tab].desc);
    if (SendMessageW(U.nav, LB_GETCURSEL, 0, 0) != U.tab) SendMessageW(U.nav, LB_SETCURSEL, U.tab, 0);
    HWND f = GetFocus();
    if (f && IsChild(U.hwnd, f) && !IsWindowVisible(f)) SetFocus(U.nav);
    InvalidateRect(U.nav, nullptr, FALSE);
    InvalidateRect(U.hwnd, nullptr, TRUE);  // the cards of this tab
}

void StartScan(int t);

void OpenTab(int t) {
    ShowTab(t);
    if (!U.scanned[U.tab] && !U.busy[U.tab]) StartScan(U.tab);
}

// ---- filling the lists -------------------------------------------------------------------

// checked: -1 = a list without check boxes. image: an Overview dot.
int AddRow(HWND lv, int i, const std::vector<std::wstring>& cells, int checked, int image = -1) {
    LVITEMW it{LVIF_TEXT | LVIF_PARAM | (image >= 0 ? LVIF_IMAGE : 0u)};
    it.iItem = i;
    it.lParam = i;
    it.iImage = image;
    it.pszText = const_cast<wchar_t*>(cells[0].c_str());
    const int row = ListView_InsertItem(lv, &it);
    for (size_t c = 1; c < cells.size(); ++c) ListView_SetItemText(lv, row, static_cast<int>(c), const_cast<wchar_t*>(cells[c].c_str()));
    if (checked >= 0) ListView_SetCheckState(lv, row, checked != 0);
    return row;
}

bool Checked(HWND lv, int row) { return ListView_GetCheckState(lv, row) != 0; }

std::set<std::wstring> SkippedApps() {
    wchar_t buf[4096] = L"";
    GetPrivateProfileStringW(L"Maintenance", L"SkipApps", L"", buf, 4096, SettingsPath().c_str());
    std::set<std::wstring> s;
    std::wstring cur;
    for (const wchar_t* p = buf;; ++p) {
        if (*p == L' ' || *p == L',' || !*p) {
            if (!cur.empty()) s.insert(cur);
            cur.clear();
            if (!*p) break;
        } else {
            cur.push_back(*p);
        }
    }
    return s;
}

void Summarize(int t) {
    HWND lv = List(t);
    const int n = ListView_GetItemCount(lv);
    if (t == kUpdates) {
        int sel = 0;
        for (int r = 0; r < n; ++r) sel += Checked(lv, r);
        std::wstring s = n ? std::to_wstring(n) + (n == 1 ? L" update found" : L" updates found") + L", " +
                                 std::to_wstring(sel) + L" selected."
                           : L"Everything is up to date.";
        if (!U.up.wuError.empty()) s += L"  " + U.up.wuError;
        if (!U.up.appError.empty()) s += L"  " + U.up.appError;
        SetStatus(t, s);
    } else if (t == kCleanup) {
        ULONGLONG total = 0;
        bool unknown = false;
        for (int r = 0; r < n; ++r)
            if (Checked(lv, r)) {
                total += U.clean[r].size;
                unknown = unknown || !U.clean[r].measured;
            }
        SetStatus(t, L"Selected: " + SizeText(total) + (unknown ? L" and more" : L"") + L".");
    } else if (t == kStartup) {
        int on = 0, changed = 0;
        for (int r = 0; r < n; ++r) {
            on += Checked(lv, r);
            changed += Checked(lv, r) != U.startup[r].on;
        }
        std::wstring s = std::to_wstring(on) + L" of " + std::to_wstring(n) + L" start with Windows.";
        if (changed) s += L"  " + std::to_wstring(changed) + (changed == 1 ? L" change" : L" changes") + L" not applied yet.";
        SetStatus(t, s);
    }
}

void FillUpdates() {
    HWND lv = List(kUpdates);
    U.filling = true;
    ListView_DeleteAllItems(lv);
    const std::set<std::wstring> skip = SkippedApps();
    for (int i = 0; i < static_cast<int>(U.up.items.size()); ++i) {
        const UpdateItem& u = U.up.items[i];
        const bool app = u.key.rfind(L"app:", 0) == 0;
        const bool on = app ? !skip.count(u.key.substr(4)) : !u.optional;
        std::wstring kind = u.kind;
        if (u.optional) kind += L" (optional)";
        AddRow(lv, i, {u.name, kind, u.version, u.size ? SizeText(u.size) : L"", L""}, on ? 1 : 0);
    }
    U.filling = false;
    // Graphics cards: what is installed, and where the maker's newer drivers come from.
    std::wstring g;
    for (const GpuInfo& gpu : U.up.gpus) {
        if (!g.empty()) g += L"\n";
        g += gpu.name + L": driver " + gpu.version + (gpu.date.empty() ? L"" : L" (" + gpu.date + L")");
    }
    if (!U.up.gpus.empty() && !U.up.gpus[0].open.empty())
        g += L"\nNewer graphics drivers come from the maker's app, not from this list.";
    SetText(IDC_UPGPU, g);
    if (!U.up.gpus.empty() && !U.up.gpus[0].openLabel.empty()) SetText(IDC_UPGPUBTN, U.up.gpus[0].openLabel);
    Summarize(kUpdates);
}

void FillCleanup() {
    HWND lv = List(kCleanup);
    U.filling = true;
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < static_cast<int>(U.clean.size()); ++i) {
        const CleanItem& c = U.clean[i];
        std::wstring size = c.measured ? SizeText(c.size) : (c.key == L"winsxs" ? L"—" : L"needs admin");
        AddRow(lv, i, {c.name, size, c.note, L""}, c.byDefault ? 1 : 0);
    }
    U.filling = false;
    Summarize(kCleanup);
}

void FillStartup() {
    HWND lv = List(kStartup);
    U.filling = true;
    ListView_DeleteAllItems(lv);
    for (int i = 0; i < static_cast<int>(U.startup.size()); ++i) {
        const StartupItem& s = U.startup[i];
        AddRow(lv, i, {s.name, s.where, s.command, L""}, s.on ? 1 : 0);
    }
    U.filling = false;
    Summarize(kStartup);
}

std::wstring Duration(ULONGLONG ms) {
    const ULONGLONG h = ms / 3600000, d = h / 24;
    if (d) return std::to_wstring(d) + (d == 1 ? L" day " : L" days ") + std::to_wstring(h % 24) + (h % 24 == 1 ? L" hour" : L" hours");
    if (h) return std::to_wstring(h) + (h == 1 ? L" hour" : L" hours");
    return std::to_wstring(ms / 60000) + L" minutes";
}

// From U.health, which a check reads on a worker thread (asking Windows Update whether a restart is
// pending can take long while it is busy installing, and the window must keep answering).
void FillHealth() {
    if (!U.scanned[kHealth]) {
        SetText(IDC_HEINFO, L"Reading…");
        return;
    }
    const Health& h = U.health;
    const ULONGLONG days = h.uptimeMs / 86400000;
    const DWORD mem = h.mem.dwMemoryLoad;
    std::wstring advice;
    if (h.rebootPending || U.rebootNeeded) advice = L"Restart to finish installing updates.";
    else if (days >= 14) advice = L"Restart soon: a restart clears out apps and services that slowly use more memory.";
    else if (days >= 7 || mem >= 90) advice = L"A restart is suggested.";
    else advice = L"No restart needed.";
    std::wstring t = L"Windows has been running for " + Duration(h.uptimeMs) + L".  " + advice + L"\n";
    const ULONGLONG total = h.mem.ullTotalPhys, used = total - h.mem.ullAvailPhys;
    t += L"Memory: " + SizeText(used) + L" of " + SizeText(total) + L" in use (" + std::to_wstring(mem) + L"%).\n";
    t += L"Disks: ";
    for (size_t i = 0; i < h.drives.size(); ++i) {
        const DriveSpace& d = h.drives[i];
        if (i) t += L",   ";
        t += d.root + L" " + SizeText(d.free) + L" free of " + SizeText(d.total);
    }
    t += L"\n\nRepair runs DISM and then sfc (15 to 30 minutes). Optimize trims SSDs and defragments hard disks; "
         L"Windows also does this weekly by itself.";
    SetText(IDC_HEINFO, t);
}

// ---- last results (maintenance.ini) -------------------------------------------------------

std::wstring CachePath() { return AppDataDir() + L"\\maintenance.ini"; }
std::wstring CacheGet(const wchar_t* key) {
    std::vector<wchar_t> buf(32768);
    GetPrivateProfileStringW(L"Last", key, L"", buf.data(), static_cast<DWORD>(buf.size()), CachePath().c_str());
    return buf.data();
}
long long CacheNum(const wchar_t* key, long long def = -1) {
    const std::wstring s = CacheGet(key);
    return s.empty() ? def : _wtoi64(s.c_str());
}
void CacheSet(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"Last", key, v.c_str(), CachePath().c_str());
}
long long Now() { return static_cast<long long>(std::time(nullptr)); }

std::wstring Plural(long long n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

struct Counts {
    int win = 0, drv = 0, app = 0;  // -1 = couldn't check
};
// Updates that count as "to do": Windows (not the optional ones), drivers, and apps not skipped.
std::vector<std::wstring> PendingKeys(const UpdateScan& up, Counts* c = nullptr) {
    const std::set<std::wstring> skip = SkippedApps();
    std::vector<std::wstring> keys;
    Counts n;
    for (const UpdateItem& u : up.items) {
        if (u.key.rfind(L"app:", 0) == 0) {
            if (skip.count(u.key.substr(4))) continue;
            ++n.app;
        } else if (u.kind == L"Driver") {
            ++n.drv;
        } else if (!u.optional) {
            ++n.win;
        } else {
            continue;
        }
        keys.push_back(u.key);
    }
    if (!up.wuError.empty()) n.win = n.drv = -1;
    if (!up.appError.empty()) n.app = -1;
    if (c) *c = n;
    return keys;
}
std::wstring JoinKeys(const std::vector<std::wstring>& keys) {
    std::wstring s;
    for (const auto& k : keys) s += (s.empty() ? L"" : L" ") + k;
    return s;
}
// After a check: the counts for the Overview, and which updates have been seen (no notice for them).
void SaveCounts(const UpdateScan& up) {
    Counts c;
    const std::vector<std::wstring> keys = PendingKeys(up, &c);
    CacheSet(L"WinUpdates", std::to_wstring(c.win));
    CacheSet(L"DriverUpdates", std::to_wstring(c.drv));
    CacheSet(L"AppUpdates", std::to_wstring(c.app));
    CacheSet(L"WinError", up.wuError);
    CacheSet(L"AppError", up.appError);
    CacheSet(L"LastCheck", std::to_wstring(Now()));
    CacheSet(L"Notified", JoinKeys(keys));
}
void SaveClean() {
    ULONGLONG b = 0;
    for (const CleanItem& c : U.clean)
        if (c.byDefault && c.measured) b += c.size;
    CacheSet(L"CleanBytes", std::to_wstring(b));
    CacheSet(L"CleanTime", std::to_wstring(Now()));
}

std::wstring DaysText(long long d) {
    return d <= 0 ? L"today" : d == 1 ? L"yesterday" : std::to_wstring(d) + L" days ago";
}
std::wstring Ago(long long t) {
    if (t <= 0) return L"never";
    const long long s = std::max(0LL, Now() - t);
    if (s < 120) return L"just now";
    if (s < 3600) return std::to_wstring(s / 60) + L" minutes ago";
    if (s < 86400) return Plural(s / 3600, L"hour ago", L"hours ago");
    return DaysText(s / 86400);
}
// Days since a "YYYY-MM-DD" date; -1 unknown.
int AgeDays(const std::wstring& date) {
    SYSTEMTIME st{};
    int y = 0, m = 0, d = 0;
    if (swscanf_s(date.c_str(), L"%d-%d-%d", &y, &m, &d) != 3) return -1;
    st.wYear = static_cast<WORD>(y);
    st.wMonth = static_cast<WORD>(m);
    st.wDay = static_cast<WORD>(d);
    FILETIME ft, now;
    if (!SystemTimeToFileTime(&st, &ft)) return -1;
    GetSystemTimeAsFileTime(&now);
    const ULONGLONG a = (static_cast<ULONGLONG>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    const ULONGLONG b = (static_cast<ULONGLONG>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
    return b > a ? static_cast<int>((b - a) / 864000000000ULL) : 0;
}

// ---- Overview ----------------------------------------------------------------------------

void AddOv(const std::wstring& key, const std::wstring& what, int level, const std::wstring& state,
           const std::wstring& todo) {
    U.ovRows.push_back({key, what, state, todo, level});
}

void FillOverview() {
    if (!U.hwnd) return;
    std::wstring selKey;
    HWND lv = List(kOverview);
    const int sel = ListView_GetNextItem(lv, -1, LVNI_SELECTED);
    if (sel >= 0 && sel < static_cast<int>(U.ovRows.size())) selKey = U.ovRows[sel].key;
    U.ovRows.clear();
    const OverviewData& o = U.ov;
    const bool have = U.scanned[kOverview];

    // Updates: from the last check (this window's or the daily one), kept in maintenance.ini.
    const long long when = CacheNum(L"LastCheck", 0);
    const long long win = CacheNum(L"WinUpdates"), drv = CacheNum(L"DriverUpdates"), app = CacheNum(L"AppUpdates");
    const std::wstring notYet = U.busy[kUpdates] ? L"Checking…" : L"Not checked yet";
    const std::wstring checkNow = U.busy[kUpdates] ? L"" : L"Check now";
    if (!when) AddOv(L"wu", L"Windows updates", kInfo, notYet, checkNow);
    else if (win < 0) AddOv(L"wu", L"Windows updates", kWarn, L"Couldn't check: " + CacheGet(L"WinError"), L"Check again");
    else if (win > 0)
        AddOv(L"wu", L"Windows updates", o.lastUpdateDays > 30 ? kBad : kWarn, Plural(win, L"update", L"updates") + L" waiting to install",
              L"Install them");
    else
        AddOv(L"wu", L"Windows updates", kOk,
              o.lastUpdateDays >= 0 ? L"Up to date (last one installed " + DaysText(o.lastUpdateDays) + L")" : L"Up to date", L"");

    if (!o.devices.empty())
        AddOv(L"devices", L"Drivers", kBad,
              Plural(static_cast<long long>(o.devices.size()), L"device has a problem", L"devices have a problem") + L": " +
                  o.devices[0].name,
              L"Open Device Manager");
    else if (when && drv > 0)
        AddOv(L"drv", L"Drivers", kWarn, Plural(drv, L"driver update", L"driver updates") + L" from Windows Update", L"Install them");
    else if (when && drv == 0)
        AddOv(L"drv", L"Drivers", kOk, have ? L"Up to date, no device problems" : L"Up to date", L"");
    else
        AddOv(L"drv", L"Drivers", kInfo, have ? L"No device problems. Driver updates: " + notYet : notYet, L"");

    if (!when) AddOv(L"app", L"App updates", kInfo, notYet, checkNow);
    else if (app < 0) AddOv(L"app", L"App updates", kWarn, L"Couldn't check: " + CacheGet(L"AppError"), L"Check again");
    else if (app > 0) AddOv(L"app", L"App updates", kWarn, Plural(app, L"app has an update", L"apps have updates"), L"Update them");
    else AddOv(L"app", L"App updates", kOk, L"Up to date", L"");

    for (size_t i = 0; i < o.gpus.size(); ++i) {
        const GpuInfo& g = o.gpus[i];
        const int age = AgeDays(g.date);
        const bool old = age > 180;
        std::wstring s = L"Driver " + g.version;
        if (!g.date.empty()) s += L" from " + g.date;
        s += L" (" + g.name + L")";
        if (old) s = Plural(age / 30, L"month", L"months") + L" old: " + s;
        AddOv(L"gpu" + std::to_wstring(i), o.gpus.size() > 1 ? L"Graphics driver " + std::to_wstring(i + 1) : L"Graphics driver", old ? kWarn : kOk, s, old ? g.openLabel : L"");
    }

    const SecurityInfo& sec = o.sec;
    const std::wstring av = sec.product.empty() ? std::wstring(L"Virus protection") : sec.product;
    if (!have) AddOv(L"av", L"Virus protection", kInfo, L"Checking…", L"");
    else if (!sec.known) AddOv(L"av", L"Virus protection", kBad, L"No virus protection found", L"Open Windows Security");
    else if (!sec.on) AddOv(L"av", L"Virus protection", kBad, av + L": real-time protection is off", L"Turn it on");
    else if (!sec.upToDate || sec.sigAgeDays > 7)
        AddOv(L"av", L"Virus protection", kBad,
              av + L": virus definitions are out of date" + (sec.sigAgeDays > 0 ? L" (" + Plural(sec.sigAgeDays, L"day", L"days") + L" old)" : L""),
              L"Update them");
    else if (sec.sigAgeDays > 2)
        AddOv(L"av", L"Virus protection", kWarn, av + L": virus definitions are " + Plural(sec.sigAgeDays, L"day", L"days") + L" old",
              L"Update them");
    else if (sec.defender && (sec.scanAgeDays < 0 || sec.scanAgeDays > 30))
        AddOv(L"scan", L"Virus protection", kWarn,
              av + L" is on; no scan " + (sec.scanAgeDays < 0 ? std::wstring(L"recorded") : L"in " + Plural(sec.scanAgeDays, L"day", L"days")),
              L"Run a quick scan");
    else AddOv(L"av", L"Virus protection", kOk, av + L" is on and up to date", L"");

    const Health& h = have ? o.health : U.health;
    const ULONGLONG days = h.uptimeMs / 86400000;
    if (h.rebootPending || U.rebootNeeded) AddOv(L"restart", L"Restart", kBad, L"Needed to finish installing updates", L"Restart now");
    else if (days >= 14) AddOv(L"restart", L"Restart", kWarn, L"Windows has been running for " + Duration(h.uptimeMs), L"Restart soon");
    else AddOv(L"restart", L"Restart", kOk, L"Running for " + Duration(h.uptimeMs), L"");

    for (const DriveSpace& d : h.drives) {
        if (!d.total) continue;
        const long long pct = static_cast<long long>(d.free * 100 / d.total);
        const int lvl = pct < 5 ? kBad : pct < 15 ? kWarn : kOk;
        AddOv(L"disk", L"Disk space " + d.root, lvl,
              SizeText(d.free) + L" free of " + SizeText(d.total) + L" (" + std::to_wstring(pct) + L"%)",
              lvl == kOk ? L"" : L"Free up space");
    }

    const long long cb = CacheNum(L"CleanBytes");
    if (cb < 0) AddOv(L"clean", L"Cleanup", kInfo, U.busy[kCleanup] ? L"Measuring…" : L"Not measured yet", U.busy[kCleanup] ? L"" : L"Measure");
    else if (cb > 5LL << 30) AddOv(L"clean", L"Cleanup", kWarn, SizeText(cb) + L" of temporary files can go", L"Clean up");
    else AddOv(L"clean", L"Cleanup", kOk, SizeText(cb) + L" of temporary files", L"");

    if (o.battery.present && o.battery.design && o.battery.full) {
        const long long pct = std::min<long long>(100, static_cast<long long>(o.battery.full) * 100 / o.battery.design);
        const int lvl = pct < 60 ? kBad : pct < 80 ? kWarn : kOk;
        AddOv(L"battery", L"Battery", lvl, L"Holds " + std::to_wstring(pct) + L"% of the charge it held when new",
              lvl == kBad ? L"Think about a new battery" : L"");
    }

    const UpdateInfo& ui = Update_Info();
    const std::wstring ver = CANIVETE_VERSION;
    if (Update_Busy()) AddOv(L"canivete", L"Canivete", kInfo, L"Version " + ver + L". Checking for a newer one…", L"");
    else if (ui.available) AddOv(L"canivete", L"Canivete", kWarn, L"Version " + ui.latest + L" is out (this is " + ver + L")", L"Update Canivete");
    else if (ui.checked) AddOv(L"canivete", L"Canivete", kOk, L"Up to date (version " + ver + L")", L"");
    else if (!ui.error.empty()) AddOv(L"canivete", L"Canivete", kInfo, L"Version " + ver + L". Couldn't check: " + ui.error, L"Check again");
    else AddOv(L"canivete", L"Canivete", kInfo, L"Version " + ver, L"Check for updates");

    AddOv(L"store", L"Microsoft Store apps", kInfo, L"They update themselves", L"Open Store updates");

    U.filling = true;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    int attention = 0, keep = -1;
    for (int i = 0; i < static_cast<int>(U.ovRows.size()); ++i) {
        const OvRow& r = U.ovRows[i];
        AddRow(lv, i, {r.what, r.state, r.todo}, -1, r.level);
        attention += r.level == kWarn || r.level == kBad;
        if (!selKey.empty() && r.key == selKey && keep < 0) keep = i;
    }
    if (keep >= 0) ListView_SetItemState(lv, keep, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    U.filling = false;
    std::wstring s = attention ? Plural(attention, L"thing needs", L"things need") + L" attention." : L"All good.";
    s += L"  Updates last checked " + Ago(when) + L".";
    SetStatus(kOverview, s);
    UpdateButtons();
}

// ---- Installed apps ----------------------------------------------------------------------

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    return s;
}

void FillApps() {
    HWND lv = List(kApps);
    const int len = GetWindowTextLengthW(Item(IDC_APSEARCH));
    std::wstring q(len, L'\0');
    GetWindowTextW(Item(IDC_APSEARCH), q.data(), len + 1);
    q = Lower(q);
    U.filling = true;
    SendMessageW(lv, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(lv);
    U.appRows.clear();
    for (int i = 0; i < static_cast<int>(U.apps.size()); ++i) {
        const InstalledApp& a = U.apps[i];
        if (!q.empty() && Lower(a.name).find(q) == std::wstring::npos && Lower(a.publisher).find(q) == std::wstring::npos)
            continue;
        AddRow(lv, static_cast<int>(U.appRows.size()), {a.name, a.publisher, a.version, a.size ? SizeText(a.size) : L"", a.date}, -1);
        U.appRows.push_back(i);
    }
    SendMessageW(lv, WM_SETREDRAW, TRUE, 0);
    U.filling = false;
    const long long n = static_cast<long long>(U.apps.size());
    SetStatus(kApps, q.empty() ? Plural(n, L"app installed.", L"apps installed.")
                               : std::to_wstring(U.appRows.size()) + L" of " + Plural(n, L"app", L"apps") + L" match.");
    UpdateButtons();
}

// ---- checks and jobs ---------------------------------------------------------------------

WPARAM Tag(int t) { return MAKEWPARAM(t, U.gen); }

void StartScan(int t) {
    if (U.busy[t]) return;
    if (J.running && J.tab == t && t != kHealth && t != kOverview) return;  // the job's list stays as it is
    if (t == kUpdates && g_bgBusy) {  // the daily check is running: its results come here too
        U.busy[t] = U.waitBg = true;
        UpdateButtons();
        SetStatus(t, L"Checking for updates…");
        return;
    }
    U.busy[t] = true;
    UpdateButtons();
    SetStatus(t, t == kUpdates    ? L"Checking for updates…"
                 : t == kCleanup  ? L"Measuring…"
                 : t == kStartup  ? L"Reading startup apps…"
                 : t == kApps     ? L"Reading installed apps…"
                 : t == kHealth   ? L""
                                  : L"Checking…");
    HWND w = U.hwnd;
    const WPARAM tag = Tag(t);
    std::thread([w, t, tag] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* r = new ScanResult;
        if (t == kUpdates) {
            r->up = ScanUpdates([w, tag](const std::wstring& k, const std::wstring& s) {
                PostOrDelete(w, WM_M_PROGRESS, tag, new Progress{k, s});
            });
            r->reboot = RebootPending();
        } else if (t == kHealth) {
            r->health = ReadHealth();
        } else if (t == kCleanup) {
            r->clean = ScanCleanup();
        } else if (t == kStartup) {
            r->startup = ScanStartup();
        } else if (t == kApps) {
            r->apps = ScanInstalled();
        } else {
            r->ov.sec = ReadSecurity();
            r->ov.devices = ScanDeviceProblems();
            r->ov.battery = ReadBattery();
            r->ov.lastUpdateDays = DaysSinceLastUpdate();
            r->ov.health = ReadHealth();
            r->ov.gpus = ScanGpus();
        }
        CoUninitialize();
        PostOrDelete(w, WM_M_SCAN, tag, r);
    }).detach();
}

void OnScan(int t, std::unique_ptr<ScanResult> r) {
    U.busy[t] = false;
    U.scanned[t] = true;
    if (t == kUpdates) {
        U.up = std::move(r->up);
        FillUpdates();
        SaveCounts(U.up);
        if (r->reboot) U.rebootNeeded = true;
        ShowTab(U.tab);
    } else if (t == kHealth) {
        U.health = std::move(r->health);
        FillHealth();
    } else if (t == kCleanup) {
        U.clean = std::move(r->clean);
        FillCleanup();
        SaveClean();
    } else if (t == kStartup) {
        U.startup = std::move(r->startup);
        FillStartup();
    } else if (t == kApps) {
        U.apps = std::move(r->apps);
        FillApps();
    } else if (t == kOverview) {
        U.ov = std::move(r->ov);
    }
    FillOverview();
    UpdateButtons();
}

void FillColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// ---- the job panel -----------------------------------------------------------------------

// The name a job's item key stands for in its tab's list ("" if none).
std::wstring ItemName(int t, const std::wstring& key) {
    if (key.empty()) return L"";
    if (t == kUpdates) {
        for (const UpdateItem& u : U.up.items)
            if (u.key == key) return u.name;
    } else if (t == kCleanup) {
        for (const CleanItem& c : U.clean)
            if (c.key == key) return c.name;
    } else if (t == kStartup) {
        for (const StartupItem& s : U.startup)
            if (s.key == key) return s.name;
    }
    return L"";
}

// "Name (why), Name, Name and 2 more"
std::wstring NameList(const std::vector<std::wstring>& keys, bool why) {
    std::wstring s;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (i == 3) {
            s += L" and " + std::to_wstring(keys.size() - 3) + L" more";
            break;
        }
        std::wstring n = ItemName(J.tab, keys[i]);
        if (n.empty()) n = keys[i];
        if (why) {
            const auto it = J.itemText.find(keys[i]);
            if (it != J.itemText.end() && !it->second.empty()) n += L" (" + it->second + L")";
        }
        s += (i ? L", " : L"") + n;
    }
    return s;
}

// The panel's line while the job runs: "2 of 6:  Firefox  —  Downloading 45%  ·  38%".
std::wstring RunningText() {
    std::wstring s;
    if (J.step > 0 && J.steps > 1) s = std::to_wstring(J.step) + L" of " + std::to_wstring(J.steps) + L":  ";
    const std::wstring name = ItemName(J.tab, J.stepKey);
    if (!name.empty()) s += name + (J.stepWhat.empty() ? L"" : L"  —  " + J.stepWhat);
    else s += J.stepWhat;
    if (J.stepWhat.empty() && name.empty()) s = J.text;
    if (J.haveFreed) s += L"  ·  " + SizeText(J.freed) + L" freed so far";
    if (J.pct >= 0) s += L"  ·  " + std::to_wstring(J.pct / 10) + L"%";
    if (J.cancelling) s = L"Stopping after this one…   " + s;
    return s;
}

// The result line once the job has ended; sets J.outcome.
std::wstring ResultText(LPARAM code) {
    if (code == kDeclined) {
        J.outcome = 2;
        if (J.task == L"clean" && (J.freed || !J.ok.empty()))  // the user's own folders were cleaned before the prompt
            return L"Freed " + SizeText(J.freed) +
                   L" in your own folders. The Windows places weren't cleaned: Windows didn't get permission.";
        return L"Not started: Windows didn't get permission to make changes.";
    }
    if (code == kNoStart) {
        J.outcome = 2;
        return L"Couldn't start the job with administrator rights.";
    }
    std::vector<std::wstring> ok = J.ok, failed = J.failed, skipped = J.skipped;
    auto drop = [](std::vector<std::wstring>& v, const wchar_t* k) {
        const auto it = std::find(v.begin(), v.end(), k);
        if (it == v.end()) return false;
        v.erase(it);
        return true;
    };
    const bool restoreFailed = J.task == L"install" && drop(failed, L"restore");
    drop(ok, L"restore");
    drop(skipped, L"restore");
    const bool stopped = J.cancelling && !skipped.empty();
    J.outcome = stopped ? 2 : (!failed.empty() || restoreFailed || code != 0) ? 1 : 0;
    const long long n = static_cast<long long>(ok.size() + failed.size() + skipped.size());
    const long long nOk = static_cast<long long>(ok.size()), nFail = static_cast<long long>(failed.size()),
                    nSkip = static_cast<long long>(skipped.size());
    const bool good = code == 0;
    std::wstring s;
    if (J.task == L"install") {
        if (n == 0) {
            s = good ? L"Finished." : L"Finished, with problems: see the Status column.";
        } else if (!nFail && !nSkip) {
            s = n == 1 ? NameList(ok, false) + L" updated." : L"All " + std::to_wstring(n) + L" updated.";
        } else {
            s = std::to_wstring(nOk) + L" of " + std::to_wstring(n) + L" updated";
            if (nFail) s += L", " + std::to_wstring(nFail) + L" failed: " + NameList(failed, true);
            if (nSkip) s += (nFail ? L";  " : L", ") + std::to_wstring(nSkip) + L" not started";
            s += L".";
            if (stopped) s = L"Stopped: " + s;
        }
        if (restoreFailed) s += L"  No restore point was made.";
        if (U.rebootNeeded) s += L"  Restart to finish.";
    } else if (J.task == L"clean") {
        s = (stopped ? L"Stopped. Freed " : L"Freed ") + SizeText(J.freed) + L".";
        if (nFail) s += L"  Couldn't clean: " + NameList(failed, true) + L".";
        if (nSkip) s += L"  " + Plural(nSkip, L"place", L"places") + L" not cleaned.";
    } else if (J.task == L"startup") {
        s = !nFail ? Plural(nOk, L"change applied.", L"changes applied.")
                   : std::to_wstring(nOk) + L" of " + Plural(n, L"change", L"changes") + L" applied; failed: " +
                         NameList(failed, true) + L".";
    } else if (J.task == L"repair") {
        if (J.cancelling && std::find(J.skipped.begin(), J.skipped.end(), L"sfc") != J.skipped.end()) {
            J.outcome = 2;
            s = L"Stopped: the Windows image was repaired, the system file check didn't run.";
        } else {
            s = good ? L"Repair finished." : L"Repair finished, with problems: see the log below.";
        }
        if (U.rebootNeeded) s += L"  Restart to finish.";
    } else if (J.task == L"optimize") {
        s = good ? L"Drives optimized." : L"Optimizing finished, with problems: see the log below.";
    } else if (J.task == L"scan") {
        s = good ? L"Virus scan finished." : L"Virus scan finished: see the log below.";
    } else if (J.task == L"restore") {
        s = good ? L"Restore point made." : L"The restore point couldn't be made: see the log below.";
    } else if (J.task == L"uninstall") {
        J.outcome = 0;
        s = J.name + L": its uninstaller has closed.";
    } else {
        s = good ? L"Finished." : L"Finished, with problems.";
    }
    return s;
}

void JobFont(bool bold) {
    for (Ctl& c : U.ctls)
        if (GetDlgCtrlID(c.wnd) == IDC_JOBTEXT) c.font = bold ? kFontBold : kFontNormal;
    SendMessageW(Item(IDC_JOBTEXT), WM_SETFONT, reinterpret_cast<WPARAM>(bold ? U.bold : U.font), TRUE);
}

// Progress on the taskbar button too, so it shows while the window is behind other apps.
void TaskbarShow() {
    if (!U.taskbar || !U.hwnd) return;
    if (J.running) {
        if (J.pct < 0) {
            U.taskbar->SetProgressState(U.hwnd, TBPF_INDETERMINATE);
        } else {
            U.taskbar->SetProgressState(U.hwnd, TBPF_NORMAL);
            U.taskbar->SetProgressValue(U.hwnd, static_cast<ULONGLONG>(J.pct), 1000);
        }
    } else if (J.tab >= 0 && J.outcome != 0) {
        U.taskbar->SetProgressState(U.hwnd, J.outcome == 1 ? TBPF_ERROR : TBPF_PAUSED);
        U.taskbar->SetProgressValue(U.hwnd, J.pct >= 0 ? static_cast<ULONGLONG>(J.pct) : 1000, 1000);
    } else {
        U.taskbar->SetProgressState(U.hwnd, TBPF_NOPROGRESS);
    }
}

// While a job runs and the window is in front, other apps (an installer, the elevated copy) can't
// pull the foreground away; a click by the user still switches as usual.
void LockFront() {
    if (J.locked || !J.wasFront || GetForegroundWindow() != U.hwnd) return;
    J.locked = LockSetForegroundWindow(LSFW_LOCK) != FALSE;
}
void UnlockFront() {
    if (!J.locked) return;
    LockSetForegroundWindow(LSFW_UNLOCK);
    J.locked = false;
}

constexpr UINT_PTR kMarqueeTimer = 1;

// Something about the job changed: the line, the bar, the tab list and the taskbar follow.
void JobChanged() {
    if (J.tab < 0) return;
    SetText(IDC_JOBTEXT, J.running ? RunningText() : J.text);
    if (HWND bar = Item(IDC_JOBBAR)) {
        if (J.running && J.pct < 0) SetTimer(bar, kMarqueeTimer, 30, nullptr);
        else KillTimer(bar, kMarqueeTimer);
        InvalidateRect(bar, nullptr, FALSE);
    }
    TaskbarShow();
    InvalidateRect(U.nav, nullptr, FALSE);
}

// Starts the panel for a new job; false (with a note) while another job runs.
bool BeginJob(int t, const std::wstring& task, bool admin) {
    if (J.running) {
        SetStatus(t, std::wstring(L"Wait for the job on the ") + kTabs[J.tab].name + L" tab to finish first.");
        return false;
    }
    J = JobState{};
    J.tab = t;
    J.running = true;
    J.admin = admin && !IsElevated();
    J.task = task;
    J.cancellable = task == L"install" || task == L"clean" || task == L"repair";
    J.wasFront = GetForegroundWindow() == U.hwnd;
    J.text = J.admin && task != L"clean" ? L"Waiting for permission (the Windows prompt)…" : L"Starting…";
    if (!U.cancel) U.cancel = CreateEventW(nullptr, TRUE, FALSE, CancelEventName(GetCurrentProcessId()).c_str());
    else ResetEvent(U.cancel);
    SetCancelEvent(U.cancel);
    JobFont(false);
    Layout();
    ShowTab(U.tab);
    JobChanged();
    UpdateButtons();
    return true;
}

// The panel's Close button (after the job): the tab gets its full height back.
void ClosePanel() {
    if (J.running) return;
    KillTimer(Item(IDC_JOBBAR), kMarqueeTimer);
    J = JobState{};
    Layout();
    ShowTab(U.tab);
    TaskbarShow();
    UpdateButtons();
    InvalidateRect(U.nav, nullptr, FALSE);
}

// Starts the elevated copy for a job and waits for it (call on a worker thread): its exit code, or
// kDeclined / kNoStart. Test copies with simulated jobs start it without the prompt.
LPARAM RunElevated(HWND w, const std::wstring& task, const std::vector<std::wstring>& args, bool fake) {
    std::wstring params = L"--maint-admin " + std::to_wstring(reinterpret_cast<UINT_PTR>(w)) + L" " + task;
    for (const auto& a : args) params += L" " + QuoteArg(a);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    SHELLEXECUTEINFOW sei{sizeof(sei)};
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
    sei.hwnd = w;
    sei.lpVerb = fake ? L"open" : L"runas";
    sei.lpFile = exe;
    sei.lpParameters = params.c_str();
    sei.nShow = SW_HIDE;
    LPARAM code;
    if (!ShellExecuteExW(&sei)) {
        code = GetLastError() == ERROR_CANCELLED ? kDeclined : kNoStart;
    } else {
        DWORD c = 1;
        if (sei.hProcess) {
            WaitForSingleObject(sei.hProcess, INFINITE);
            GetExitCodeProcess(sei.hProcess, &c);
            CloseHandle(sei.hProcess);
        }
        code = static_cast<LPARAM>(c);
    }
    CoUninitialize();
    return code;
}

// Runs a job: here on a worker thread, or in an elevated copy when it needs administrator rights.
// The window stays as it is meanwhile: nothing here waits for the job.
bool StartTask(int t, const std::wstring& task, const std::vector<std::wstring>& args, bool admin) {
    if (!BeginJob(t, task, admin)) return false;
    HWND w = U.hwnd;
    const WPARAM tag = Tag(t);
    const bool fake = FakeMode();
    if (J.admin && task == L"clean") {
        // The places in the user's own folders are cleaned here, without administrator rights; only the
        // Windows places go to the elevated copy, which never touches the user's folders.
        std::thread([w, tag, args, fake] {
            ULONGLONG freed = 0;
            std::vector<std::wstring> more;
            std::vector<std::wstring> mine = args;
            mine.push_back(L"scope:user");
            const int userCode = RunTask(L"clean", mine, [&](const std::wstring& k, const std::wstring& s) {
                if (k == L"part") {  // a place half done here: the elevated copy adds to its row
                    more.push_back(L"pre:" + s);
                    return;
                }
                if (k == L"freed") freed = _wcstoui64(s.c_str(), nullptr, 10);
                PostOrDelete(w, WM_M_PROGRESS, tag, new Progress{k, s});
            });
            LPARAM code = userCode;
            if (!CancelRequested()) {
                PostOrDelete(w, WM_M_PROGRESS, tag, new Progress{L"status", L"Waiting for permission (the Windows prompt)…"});
                std::vector<std::wstring> rest = args;
                rest.push_back(L"freed:" + std::to_wstring(freed));
                rest.insert(rest.end(), more.begin(), more.end());
                code = RunElevated(w, L"clean", rest, fake);
                if (code == 0) code = userCode;
            }
            PostMessageW(w, WM_M_DONE, tag, code);
        }).detach();
    } else if (J.admin) {
        SetStatus(t, L"Waiting for permission (the Windows prompt)…");
        std::thread([w, tag, task, args, fake] { PostMessageW(w, WM_M_DONE, tag, RunElevated(w, task, args, fake)); }).detach();
    } else {
        LockFront();
        std::thread([w, tag, task, args] {
            const int code = RunTask(task, args, [w, tag](const std::wstring& k, const std::wstring& s) {
                PostOrDelete(w, WM_M_PROGRESS, tag, new Progress{k, s});
            });
            PostMessageW(w, WM_M_DONE, tag, code);
        }).detach();
    }
    return true;
}

void LogLine(const std::wstring& text, bool progress) {
    HWND e = Item(IDC_HELOG);
    const int len = GetWindowTextLengthW(e);
    std::wstring t = text;
    if (U.logPartial) {
        const int last = static_cast<int>(SendMessageW(e, EM_GETLINECOUNT, 0, 0)) - 1;
        const int start = static_cast<int>(SendMessageW(e, EM_LINEINDEX, last, 0));
        SendMessageW(e, EM_SETSEL, start, len);
    } else {
        SendMessageW(e, EM_SETSEL, len, len);
        if (len) t = L"\r\n" + t;
    }
    SendMessageW(e, EM_REPLACESEL, FALSE, reinterpret_cast<LPARAM>(t.c_str()));
    U.logPartial = progress;
}

void OnProgress(int t, const std::wstring& key, const std::wstring& text) {
    const bool job = J.running && t == J.tab;
    if (job) {
        if (key == L"pct") {
            J.pct = std::clamp(_wtoi(text.c_str()), -1, 1000);
            JobChanged();
            return;
        }
        if (key == L"step") {  // "<i>|<n>|<key>|<what>"
            const size_t a = text.find(L'|'), b = a == std::wstring::npos ? a : text.find(L'|', a + 1),
                         c = b == std::wstring::npos ? b : text.find(L'|', b + 1);
            if (c != std::wstring::npos) {
                J.step = _wtoi(text.c_str());
                J.steps = _wtoi(text.c_str() + a + 1);
                J.stepKey = text.substr(b + 1, c - b - 1);
                J.stepWhat = text.substr(c + 1);
                JobChanged();
            }
            return;
        }
        if (key == L"freed") {
            J.freed = _wcstoui64(text.c_str(), nullptr, 10);
            J.haveFreed = true;
            JobChanged();
            return;
        }
        if (key == L"ok" || key == L"fail" || key == L"skip") {
            (key == L"ok" ? J.ok : key == L"fail" ? J.failed : J.skipped).push_back(text);
            return;
        }
        if (key == L"hello") {  // the elevated copy runs: the prompt has closed
            if (J.wasFront) SetForegroundWindow(U.hwnd);
            LockFront();
            J.text = L"Starting…";
            SetStatus(t, L"");
            JobChanged();
            return;
        }
    }
    if (key == L"pct" || key == L"step" || key == L"freed" || key == L"ok" || key == L"fail" || key == L"skip" ||
        key == L"hello")
        return;
    if (key == L"status") {
        SetStatus(t, text);
    } else if (key == L"reboot") {
        U.rebootNeeded = true;
        ShowTab(U.tab);
        FillHealth();
    } else if (key == L"log" || key == L"logp") {
        LogLine(text, key == L"logp");
        if (t != kHealth && !text.empty()) SetStatus(t, text);
    } else {
        // An item's row: its last column shows the text.
        if (job) J.itemText[key] = text;
        HWND lv = t == kHealth ? nullptr : List(t);
        if (!lv) return;
        const int n = ListView_GetItemCount(lv);
        for (int r = 0; r < n; ++r) {
            const std::wstring* k = nullptr;
            if (t == kUpdates && r < static_cast<int>(U.up.items.size())) k = &U.up.items[r].key;
            else if (t == kCleanup && r < static_cast<int>(U.clean.size())) k = &U.clean[r].key;
            else if (t == kStartup && r < static_cast<int>(U.startup.size())) k = &U.startup[r].key;
            if (k && *k == key) {
                const int col = Header_GetItemCount(ListView_GetHeader(lv)) - 1;
                ListView_SetItemText(lv, r, col, const_cast<wchar_t*>(text.c_str()));
                ListView_EnsureVisible(lv, r, FALSE);
                break;
            }
        }
    }
}

void OnDone(int t, LPARAM code) {
    if (!(J.running && t == J.tab)) return;
    J.running = false;
    UnlockFront();
    J.text = ResultText(code);
    if (J.outcome == 0) J.pct = 1000;
    JobFont(true);
    Layout();
    ShowTab(U.tab);
    JobChanged();
    if (GetForegroundWindow() != U.hwnd) FlashWindow(U.hwnd, TRUE);
    const bool ran = code != kDeclined && code != kNoStart;
    if (!ran) SetStatus(t, L"");
    // The lists show what is true now.
    if (ran && t == kCleanup) {
        StartScan(kCleanup);
        SetStatus(t, L"Measuring again…");
    } else if (ran && t == kUpdates) {
        StartScan(kUpdates);
    } else if (ran && t == kStartup) {
        StartScan(kStartup);
    } else if (ran && t == kApps) {
        StartScan(kApps);
    } else if (t == kHealth) {
        StartScan(kHealth);
        StartScan(kOverview);  // a virus scan changes what Overview says
    }
    if (U.rebootNeeded) FillHealth();
    UpdateButtons();
}

// ---- the progress bar (a small custom control) ---------------------------------------------

void PaintBar(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    if (rc.right <= 0 || rc.bottom <= 0) return;
    const Palette& p = Pal();
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ old = SelectObject(mem, bmp);
    const COLORREF fill = J.running || J.outcome == 0 ? p.accent : J.outcome == 1 ? p.caution : p.text3;
    const int pos = J.running && J.pct < 0 ? -1 : std::max(J.pct, 0);
    Fluent_DrawProgress(mem, rc, U.dpi, pos, GetTickCount64(), fill, p.window);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
}

LRESULT CALLBACK BarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == kMarqueeTimer) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintBar(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_PRINTCLIENT:
        PaintBar(h, reinterpret_cast<HDC>(wp));
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void Install() {
    HWND lv = List(kUpdates);
    std::vector<std::wstring> args;
    std::wstring skip;
    const int n = ListView_GetItemCount(lv);
    for (int r = 0; r < n && r < static_cast<int>(U.up.items.size()); ++r) {
        const std::wstring& k = U.up.items[r].key;
        const bool app = k.rfind(L"app:", 0) == 0;
        if (Checked(lv, r)) {
            args.push_back(k);
        } else if (app) {
            if (!skip.empty()) skip += L" ";
            skip += k.substr(4);
        }
    }
    // Apps left unticked stay unticked next time.
    SaveSettingValue(L"Maintenance", L"SkipApps", skip);
    if (args.empty()) {
        SetStatus(kUpdates, L"Nothing selected.");
        return;
    }
    if (Button_GetCheck(Item(IDC_UPRESTORE)) == BST_CHECKED) args.insert(args.begin(), L"restore");
    if (!StartTask(kUpdates, L"install", args, true)) return;
    for (int r = 0; r < n && r < static_cast<int>(U.up.items.size()); ++r)
        ListView_SetItemText(lv, r, 4, const_cast<wchar_t*>(Checked(lv, r) ? L"Waiting…" : L""));
}

void CleanSelected() {
    HWND lv = List(kCleanup);
    std::vector<std::wstring> args;
    bool admin = false, recycle = false;
    for (int r = 0; r < ListView_GetItemCount(lv) && r < static_cast<int>(U.clean.size()); ++r)
        if (Checked(lv, r)) {
            args.push_back(U.clean[r].key);
            admin = admin || U.clean[r].admin;
            recycle = recycle || U.clean[r].key == L"recycle";
        }
    if (args.empty()) {
        SetStatus(kCleanup, L"Nothing selected.");
        return;
    }
    if (recycle && MessageBoxW(U.hwnd, L"Empty the Recycle Bin? The files in it can't be brought back afterwards.",
                               L"Canivete Maintenance", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
        return;
    if (!StartTask(kCleanup, L"clean", args, admin)) return;
    for (int r = 0; r < ListView_GetItemCount(lv) && r < static_cast<int>(U.clean.size()); ++r)
        ListView_SetItemText(lv, r, 3, const_cast<wchar_t*>(Checked(lv, r) ? L"Waiting…" : L""));
}

void ApplyStartup() {
    HWND lv = List(kStartup);
    std::vector<std::wstring> args;
    bool admin = false;
    for (int r = 0; r < ListView_GetItemCount(lv) && r < static_cast<int>(U.startup.size()); ++r) {
        const bool on = Checked(lv, r);
        if (on == U.startup[r].on) continue;
        args.push_back(std::wstring(on ? L"1|" : L"0|") + U.startup[r].key);
        admin = admin || U.startup[r].admin;
    }
    if (args.empty()) {
        SetStatus(kStartup, L"No changes to apply.");
        return;
    }
    StartTask(kStartup, L"startup", args, admin);
}

bool EnableShutdownPrivilege() {
    HANDLE t;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &t)) return false;
    TOKEN_PRIVILEGES tp{1};
    LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid);
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    const bool ok = AdjustTokenPrivileges(t, FALSE, &tp, 0, nullptr, nullptr) && GetLastError() == ERROR_SUCCESS;
    CloseHandle(t);
    return ok;
}

void RestartNow() {
    if (MessageBoxW(U.hwnd, L"Restart the computer now?\n\nSave your work in other apps first.", L"Canivete Maintenance",
                    MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
        return;
    if (TestMode()) {
        SetStatus(U.tab, L"Test copy: not restarting.");
        return;
    }
    EnableShutdownPrivilege();
    if (!ExitWindowsEx(EWX_REBOOT, SHTDN_REASON_MAJOR_OPERATINGSYSTEM | SHTDN_REASON_MINOR_UPGRADE | SHTDN_REASON_FLAG_PLANNED))
        SetStatus(U.tab, L"Windows didn't allow the restart. Use Start → Power → Restart.");
}

void Open(const wchar_t* what, const wchar_t* params = nullptr) {
    ShellExecuteW(U.hwnd, L"open", what, params, nullptr, SW_SHOWNORMAL);
}

void UninstallSelected() {
    const int row = ListView_GetNextItem(List(kApps), -1, LVNI_SELECTED);
    if (row < 0 || row >= static_cast<int>(U.appRows.size()) || U.busy[kApps] || J.running) return;
    const InstalledApp a = U.apps[U.appRows[row]];
    std::wstring exe, args;
    if (!a.msi.empty()) {
        wchar_t sys[MAX_PATH];
        GetSystemDirectoryW(sys, MAX_PATH);
        exe = std::wstring(sys) + L"\\msiexec.exe";
        args = L"/x " + a.msi;
    } else if (!SplitCommand(a.uninstall, exe, args)) {
        SetStatus(kApps, a.name + L" has no uninstaller of its own. Try Windows app settings.");
        return;
    }
    if (TestMode()) {
        Log(L"Uninstall (test copy, not run): %s %s", exe.c_str(), args.c_str());
        SetStatus(kApps, L"Test copy: not uninstalling " + a.name + L".");
        return;
    }
    if (MessageBoxW(U.hwnd, (L"Uninstall " + a.name + L"?\n\nIts own uninstaller opens; follow its steps.").c_str(),
                    L"Canivete Maintenance", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    if (!BeginJob(kApps, L"uninstall", false)) return;
    J.name = a.name;
    J.text = L"Uninstalling " + a.name + L"…  Finish the steps in its window.";
    JobChanged();
    HWND w = U.hwnd;
    const WPARAM tag = Tag(kApps);
    std::thread([w, tag, exe, args] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
        sei.hwnd = w;
        sei.lpFile = exe.c_str();
        sei.lpParameters = args.c_str();
        sei.nShow = SW_SHOWNORMAL;
        LPARAM code;
        if (!ShellExecuteExW(&sei)) {
            code = GetLastError() == ERROR_CANCELLED ? kDeclined : kNoStart;
        } else {
            DWORD c = 0;
            if (sei.hProcess) {
                WaitForSingleObject(sei.hProcess, INFINITE);
                GetExitCodeProcess(sei.hProcess, &c);
                CloseHandle(sei.hProcess);
            }
            code = static_cast<LPARAM>(c);
        }
        CoUninitialize();
        PostMessageW(w, WM_M_DONE, tag, code);
    }).detach();
}

// What a row of the Overview leads to.
void DoRow(int row) {
    if (row < 0 || row >= static_cast<int>(U.ovRows.size())) return;
    const std::wstring k = U.ovRows[row].key;
    if (k == L"wu" || k == L"drv" || k == L"app") {
        OpenTab(kUpdates);
    } else if (k == L"devices") {
        Open(L"devmgmt.msc");
    } else if (k.rfind(L"gpu", 0) == 0) {
        const size_t i = static_cast<size_t>(_wtoi(k.c_str() + 3));
        if (i < U.ov.gpus.size() && !U.ov.gpus[i].open.empty()) Open(U.ov.gpus[i].open.c_str());
    } else if (k == L"av") {
        Open(L"windowsdefender:");
    } else if (k == L"scan") {
        OpenTab(kHealth);
        StartTask(kHealth, L"scan", {}, false);
    } else if (k == L"restart") {
        RestartNow();
    } else if (k == L"disk" || k == L"clean") {
        OpenTab(kCleanup);
    } else if (k == L"battery") {
        Open(L"ms-settings:batterysaver");
    } else if (k == L"canivete") {
        if (Update_Info().available) Update_Install();
        else Update_Check(true);
        FillOverview();
    } else if (k == L"store") {
        Open(L"ms-windows-store://downloadsandupdates");
    }
}

// ---- drawing the tab list ----------------------------------------------------------------

constexpr wchar_t kTabGlyphs[kTabCount] = {Glyph::Home, Glyph::Sync, Glyph::AllApps,
                                           Glyph::Delete, Glyph::Power, Glyph::Health};

void DrawNavItem(const DRAWITEMSTRUCT& d) {
    if (d.itemID == static_cast<UINT>(-1)) return;
    HDC dc = d.hDC;
    const Palette& p = Pal();
    const int i = static_cast<int>(d.itemID);
    const bool focus = (d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT);
    Fluent_DrawNavItem(dc, d.rcItem, U.dpi, (d.itemState & ODS_SELECTED) != 0, U.navHot == i, focus, p.nav);
    RECT ir{d.rcItem.left + Px(16), d.rcItem.top, d.rcItem.left + Px(36), d.rcItem.bottom};
    Fluent_DrawGlyph(dc, ir, kTabGlyphs[i], 16, U.dpi, p.text);
    HGDIOBJ of = SelectObject(dc, U.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, p.text);
    RECT tr{d.rcItem.left + Px(48), d.rcItem.top, d.rcItem.right - Px(16), d.rcItem.bottom};
    DrawTextW(dc, kTabs[i].name, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (J.running && J.tab == i) {  // the job's progress
        SetTextColor(dc, p.accentText);
        const std::wstring pc = J.pct >= 0 ? std::to_wstring(J.pct / 10) + L"%" : L"…";
        DrawTextW(dc, pc.c_str(), -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    } else if (U.busy[i]) {
        SetTextColor(dc, p.text2);
        DrawTextW(dc, L"…", -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, of);
}

void InvalidateNavItem(int i) {
    RECT r;
    if (i >= 0 && SendMessageW(U.nav, LB_GETITEMRECT, i, reinterpret_cast<LPARAM>(&r)) != LB_ERR)
        InvalidateRect(U.nav, &r, FALSE);
}

// The tab under the mouse gets the hover color.
LRESULT CALLBACK NavProc(HWND h, UINT m, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    if (m == WM_MOUSEMOVE) {
        const LRESULT hit = SendMessageW(h, LB_ITEMFROMPOINT, 0, lp);
        const int i = HIWORD(hit) ? -1 : LOWORD(hit);
        if (i != U.navHot) {
            InvalidateNavItem(U.navHot);
            U.navHot = i;
            InvalidateNavItem(i);
            TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
            TrackMouseEvent(&t);
        }
    } else if (m == WM_MOUSELEAVE) {
        InvalidateNavItem(U.navHot);
        U.navHot = -1;
    }
    return DefSubclassProc(h, m, wp, lp);
}

// The window background: the cards of the open tab and the search box frame.
void PaintBack(HDC dc, const RECT& rc) {
    const Palette& p = Pal();
    FillColor(dc, rc, p.window);
    for (const auto& [t, r] : U.cards)
        if (t == U.tab) Fluent_DrawCard(dc, r, U.dpi);
    if (U.tab == kApps) {
        HWND s = Item(IDC_APSEARCH);
        Fluent_DrawTextBox(dc, U.searchFrame, U.dpi, GetFocus() == s, IsWindowEnabled(s) != FALSE);
    }
}

void InvalidateSearch() {
    InvalidateRect(U.hwnd, &U.searchFrame, TRUE);
    InvalidateRect(Item(IDC_APSEARCH), nullptr, TRUE);
}

// ---- window ------------------------------------------------------------------------------

void OnCommand(int id, int code) {
    switch (id) {
    case IDCANCEL:
        SendMessageW(U.hwnd, WM_CLOSE, 0, 0);
        break;
    case IDC_JOBCANCEL:
        if (!J.running) {
            ClosePanel();
        } else if (J.cancellable && !J.cancelling) {
            J.cancelling = true;
            if (U.cancel) SetEvent(U.cancel);
            JobChanged();
            UpdateButtons();
        }
        break;
    case IDOK: {  // Enter: the tab's main button
        const int main[kTabCount] = {IDC_OVDO, IDC_UPINSTALL, 0, IDC_CLRUN, IDC_STAPPLY, 0};
        if (main[U.tab] && IsWindowEnabled(Item(main[U.tab]))) OnCommand(main[U.tab], BN_CLICKED);
        break;
    }
    case IDC_NAV:
        if (code == LBN_SELCHANGE) OpenTab(static_cast<int>(SendMessageW(U.nav, LB_GETCURSEL, 0, 0)));
        break;
    case IDC_OVCHECK:
        StartScan(kOverview);
        StartScan(kUpdates);
        StartScan(kCleanup);
        if (!Update_Busy()) Update_Check(false);
        FillOverview();
        break;
    case IDC_OVDO:
        DoRow(ListView_GetNextItem(List(kOverview), -1, LVNI_SELECTED));
        break;
    case IDC_APSEARCH:
        if (code == EN_CHANGE) FillApps();
        else if (code == EN_SETFOCUS || code == EN_KILLFOCUS) InvalidateSearch();
        break;
    case IDC_APREFRESH:
        StartScan(kApps);
        break;
    case IDC_APSETTINGS:
        Open(L"ms-settings:appsfeatures");
        break;
    case IDC_APUNINSTALL:
        UninstallSelected();
        break;
    case IDC_HESCAN:
        StartTask(kHealth, L"scan", {}, false);
        break;
    case IDC_UPCHECK:
        StartScan(kUpdates);
        break;
    case IDC_UPINSTALL:
        Install();
        break;
    case IDC_UPSTORE:
        Open(L"ms-windows-store://downloadsandupdates");
        break;
    case IDC_UPGPUBTN:
        if (!U.up.gpus.empty() && !U.up.gpus[0].open.empty()) Open(U.up.gpus[0].open.c_str());
        break;
    case IDC_UPREBOOTBTN:
    case IDC_HERESTART:
        RestartNow();
        break;
    case IDC_CLMEASURE:
        StartScan(kCleanup);
        break;
    case IDC_CLSTORAGE:
        Open(L"ms-settings:storagesense");
        break;
    case IDC_CLRUN:
        CleanSelected();
        break;
    case IDC_STREFRESH:
        StartScan(kStartup);
        break;
    case IDC_STTASKMGR:
        Open(L"taskmgr.exe", L"/0 /startup");
        break;
    case IDC_STAPPLY:
        ApplyStartup();
        break;
    case IDC_HEREPAIR:
        StartTask(kHealth, L"repair", {}, true);
        break;
    case IDC_HEOPT:
        StartTask(kHealth, L"optimize", {}, true);
        break;
    case IDC_HERESTORE:
        StartTask(kHealth, L"restore", {}, true);
        break;
    }
}

SIZE WindowSize() {
    RECT r{0, 0, Px(kW), Px(kH)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, U.dpi);
    return {r.right - r.left, r.bottom - r.top};
}

LRESULT CALLBACK MaintProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) U.hwnd = h;
    if (U.hwnd != h) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        U.dpi = GetDpiForWindow(h);
        Build();
        SetWindowSubclass(U.nav, NavProc, 1, 0);
        MakeFonts();
        if (SUCCEEDED(CoCreateInstance(CLSID_TaskbarList, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&U.taskbar))) &&
            FAILED(U.taskbar->HrInit())) {
            U.taskbar->Release();
            U.taskbar = nullptr;
        }
        return 0;
    case WM_CLOSE:
        if (J.running && MessageBoxW(h,
                                     L"A job is still running. It keeps going in the background if you close this "
                                     L"window, but you won't see how it ends.\n\nClose anyway?",
                                     L"Canivete Maintenance", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
            return 0;
        DestroyWindow(h);
        return 0;
    case WM_SIZE:
        Layout();
        return 0;
    case WM_GETMINMAXINFO: {
        auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
        RECT r{0, 0, Px(kMinW), Px(kMinH)};
        AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, U.dpi);
        mm->ptMinTrackSize = {r.right - r.left, r.bottom - r.top};
        return 0;
    }
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        // Drawn off screen, then copied: the antialiased cards would flicker otherwise.
        HDC mem = CreateCompatibleDC(dc);
        HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
        HGDIOBJ old = SelectObject(mem, bmp);
        PaintBack(mem, rc);
        BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
        SelectObject(mem, old);
        DeleteObject(bmp);
        DeleteDC(mem);
        return 1;
    }
    case WM_LBUTTONDOWN: {  // a click in the search box frame, beside the box itself
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (U.tab == kApps && PtInRect(&U.searchFrame, pt)) SetFocus(Item(IDC_APSEARCH));
        return 0;
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
        COLORREF fg = p.text;
        if (id == IDC_DESC || id == IDC_UPGPU || (id >= IDC_STATUS0 && id < IDC_STATUS0 + kTabCount)) fg = p.text2;
        if (id == IDC_UPREBOOT) fg = p.critical;
        const COLORREF bg = id == IDC_HEINFO || id == IDC_HELOG ? p.card : p.window;  // on a card
        SetBkColor(dc, bg);
        SetTextColor(dc, fg);
        return reinterpret_cast<LRESULT>(Theme_Brush(bg));
    }
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(Theme_Brush(Pal().window));
    case WM_NOTIFY: {
        const auto* n = reinterpret_cast<const NMHDR*>(lp);
        if (n->code == LVN_ITEMCHANGED && !U.filling) {
            const auto* lv = reinterpret_cast<const NMLISTVIEW*>(lp);
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK))
                Summarize(static_cast<int>(n->idFrom) - IDC_LIST0);
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_SELECTED)) UpdateButtons();
        } else if (n->code == NM_DBLCLK && n->idFrom == IDC_LIST0 + kOverview) {
            DoRow(reinterpret_cast<const NMITEMACTIVATE*>(lp)->iItem);
        }
        return 0;
    }
    case WM_M_SCAN:
    case WM_M_PROGRESS:
    case WM_M_DONE: {
        const int t = LOWORD(wp);
        const bool current = HIWORD(wp) == U.gen && t >= 0 && t < kTabCount;
        if (msg == WM_M_SCAN) {
            std::unique_ptr<ScanResult> r(reinterpret_cast<ScanResult*>(lp));
            if (current) OnScan(t, std::move(r));
        } else if (msg == WM_M_PROGRESS) {
            std::unique_ptr<Progress> p(reinterpret_cast<Progress*>(lp));
            if (current) OnProgress(t, p->key, p->text);
        } else if (current) {
            OnDone(t, lp);
        }
        InvalidateRect(U.nav, nullptr, FALSE);
        return 0;
    }
    case WM_COPYDATA: {  // progress from the elevated copy: "key\ttext"
        const auto* cd = reinterpret_cast<const COPYDATASTRUCT*>(lp);
        if (cd->dwData != kCopyProgress || !J.running || !J.admin || cd->cbData < sizeof(wchar_t)) return FALSE;
        const std::wstring s(static_cast<const wchar_t*>(cd->lpData),
                             wcsnlen(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t)));
        const size_t tab = s.find(L'\t');
        if (tab != std::wstring::npos) {
            const std::wstring key = s.substr(0, tab), text = s.substr(tab + 1);
            // The elevated copy keeps no log of its own: its lines go in this copy's log.
            if (key != L"pct" && key != L"step" && key != L"freed") Log(L"Maintenance (elevated): %s %s", key.c_str(), text.c_str());
            OnProgress(J.tab, key, text);
        }
        InvalidateRect(U.nav, nullptr, FALSE);
        return TRUE;
    }
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND f = GetFocus();
            if (f && IsChild(h, f)) U.lastFocus = f;
            J.locked = false;  // the user switched away: Windows has lifted the lock
        } else {
            if (!J.running && U.taskbar) U.taskbar->SetProgressState(h, TBPF_NOPROGRESS);  // the result was seen
            const bool ok = U.lastFocus && IsWindow(U.lastFocus) && IsWindowVisible(U.lastFocus) && IsWindowEnabled(U.lastFocus);
            SetFocus(ok ? U.lastFocus : U.nav);
        }
        return 0;
    case WM_COMMAND:
        OnCommand(LOWORD(wp), HIWORD(wp));
        return 0;
    case WM_DPICHANGED: {
        U.dpi = HIWORD(wp);
        MakeFonts();
        const RECT* sug = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, sug->left, sug->top, sug->right - sug->left, sug->bottom - sug->top,
                     SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_DESTROY:
        UnlockFront();
        J = JobState{};
        if (U.taskbar) {
            U.taskbar->Release();
            U.taskbar = nullptr;
        }
        U.hwnd = U.nav = nullptr;
        U.ctls.clear();
        U.cards.clear();
        U.navHot = -1;
        U.lastFocus = nullptr;
        for (bool& b : U.busy) b = false;
        for (bool& b : U.scanned) b = false;
        U.logPartial = false;
        U.waitBg = false;
        return 0;
    case WM_NCDESTROY:
        if (U.dots) ImageList_Destroy(U.dots);
        if (U.rows) ImageList_Destroy(U.rows);
        U.dots = U.rows = nullptr;
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

HWND Maint_Window() { return U.hwnd; }

void Maint_Show(int tab) {
    if (U.hwnd) {
        if (IsIconic(U.hwnd)) ShowWindow(U.hwnd, SW_RESTORE);
        if (tab >= 0) OpenTab(tab);
        SetForegroundWindow(U.hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
        InitCommonControlsEx(&icc);
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = MaintProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kWindowClass;
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
        registered = RegisterClassExW(&wc) != 0;
        WNDCLASSEXW bc{sizeof(bc)};
        bc.lpfnWndProc = BarProc;
        bc.hInstance = g_hinst;
        bc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        bc.lpszClassName = kBarClass;
        registered = registered && RegisterClassExW(&bc) != 0;
    }
    ++U.gen;
    U.up = {};
    U.clean.clear();
    U.startup.clear();
    U.ov = {};
    U.ovRows.clear();
    U.apps.clear();
    U.appRows.clear();
    U.waitBg = false;
    U.rebootNeeded = false;
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    HWND h = CreateWindowExW(kExStyle, kWindowClass, L"Canivete Maintenance", kStyle, mi.rcWork.left, mi.rcWork.top, 100,
                             100, nullptr, nullptr, g_hinst, nullptr);
    if (!h) return;
    const SIZE sz = WindowSize();
    const RECT& w = mi.rcWork;
    const int x = w.left + std::max(0L, (w.right - w.left - sz.cx) / 2);
    const int y = w.top + std::max(0L, (w.bottom - w.top - sz.cy) / 2);
    SetWindowPos(h, nullptr, x, y, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    FillHealth();
    UpdateButtons();
    OpenTab(tab >= 0 ? tab : g_lastTab);
    // Overview always; updates and cleanup again when the last results are over 12 hours old.
    StartScan(kOverview);
    if (Now() - CacheNum(L"LastCheck", 0) > 12 * 3600) StartScan(kUpdates);
    if (Now() - CacheNum(L"CleanTime", 0) > 12 * 3600) StartScan(kCleanup);
    if (g_settings.checkUpdates && !Update_Info().checked && !Update_Busy()) Update_Check(false);
    FillOverview();
    Theme_Window(h);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}

void Maint_Refresh() { FillOverview(); }

long long Maint_SecondsToNext() {
    if (!FeatureOn(kFeatMaint) || !g_settings.maintDaily) return LLONG_MAX;
    // 20 hours after the last check, 6 after a try that didn't finish, and not while put off.
    const long long due = std::max({CacheNum(L"LastCheck", 0) + 20 * 3600, CacheNum(L"LastAttempt", 0) + 6 * 3600, g_bgRetryAt});
    return std::max(0LL, due - Now());
}

namespace {
// Why the daily check should wait (empty: go): on battery, a full-screen game or presentation,
// do-not-disturb, or no internet. A forced check ("Check again") ignores all of this.
const wchar_t* PutOffReason() {
    SYSTEM_POWER_STATUS ps{};
    if (GetSystemPowerStatus(&ps) && (ps.ACLineStatus == 0 || (ps.SystemStatusFlag & 1))) return L"on battery";
    QUERY_USER_NOTIFICATION_STATE q{};
    if (SUCCEEDED(SHQueryUserNotificationState(&q)) &&
        (q == QUNS_BUSY || q == QUNS_RUNNING_D3D_FULL_SCREEN || q == QUNS_PRESENTATION_MODE || q == QUNS_APP))
        return L"busy (full screen or presenting)";
    if (!Net_Online()) return L"offline";
    return nullptr;
}
}  // namespace

void Maint_Background(bool force) {
    if (g_bgBusy || !FeatureOn(kFeatMaint)) return;
    if (!force && Maint_SecondsToNext() > 0) return;
    if (U.hwnd && U.busy[kUpdates]) return;  // the window is checking right now
    if (!force) {
        if (const wchar_t* why = PutOffReason()) {
            g_bgRetryAt = Now() + 30 * 60 + (GetTickCount64() / 1000) % (30 * 60);  // 30 to 60 minutes
            Log(L"Maintenance: daily check put off (%s)", why);
            return;
        }
    }
    g_bgRetryAt = 0;
    CacheSet(L"LastAttempt", std::to_wstring(Now()));
    g_bgBusy = true;
    Log(L"Maintenance: daily check started");
    HWND w = g_mainWnd;
    std::thread([w] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);  // low CPU, disk and memory priority
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* r = new BgResult;
        r->up = ScanUpdates([](const std::wstring&, const std::wstring&) {}, true);
        r->sec = ReadSecurity();
        CoUninitialize();
        if (!PostMessageW(w, WM_APP_MAINT, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void Maint_OnBackground(LPARAM lp) {
    std::unique_ptr<BgResult> r(reinterpret_cast<BgResult*>(lp));
    g_bgBusy = false;
    // A notice only for updates not seen (in the window or an earlier notice) yet.
    Counts c;
    const std::vector<std::wstring> keys = PendingKeys(r->up, &c);
    const std::wstring told = L" " + CacheGet(L"Notified") + L" ";
    int fresh = 0;
    for (const auto& k : keys) fresh += told.find(L" " + k + L" ") == std::wstring::npos;
    SaveCounts(r->up);
    std::vector<std::wstring> parts;
    if (c.win > 0) parts.push_back(Plural(c.win, L"Windows update", L"Windows updates"));
    if (c.drv > 0) parts.push_back(Plural(c.drv, L"driver update", L"driver updates"));
    if (c.app > 0) parts.push_back(Plural(c.app, L"app update", L"app updates"));
    std::wstring text;
    if (fresh && !parts.empty()) {
        for (size_t i = 0; i < parts.size(); ++i)
            text += (i == 0 ? L"" : i + 1 == parts.size() ? L" and " : L", ") + parts[i];
        text += keys.size() == 1 ? L" is ready." : L" are ready.";
    }
    const SecurityInfo& s = r->sec;
    if (!s.known || !s.on || !s.upToDate || s.sigAgeDays > 7)
        text += (text.empty() ? L"" : L" ") + std::wstring(L"Virus protection needs a look.");
    Log(L"Maintenance: daily check done: %d Windows, %d driver, %d app updates (%d new)", c.win, c.drv, c.app, fresh);
    if (!text.empty()) Tray_NotifyAction(L"Computer maintenance", text + L" Click to see.", [] { Maint_Show(kOverview); });
    if (U.hwnd && U.waitBg) {
        U.waitBg = false;
        auto res = std::make_unique<ScanResult>();
        res->up = std::move(r->up);
        OnScan(kUpdates, std::move(res));
    } else {
        FillOverview();
    }
}
