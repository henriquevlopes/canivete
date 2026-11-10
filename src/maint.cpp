// Maintenance window: Updates (Windows, drivers, apps), Cleanup, Startup apps and Health, as tabs in
// a list on the left like Settings. Checks run on worker threads; jobs that need administrator
// rights run in an elevated copy of Canivete (one Windows prompt per job) that reports back here
// with WM_COPYDATA. Built in code so it lays out again at any monitor DPI.
#include "maint.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <memory>
#include <set>
#include <thread>

using namespace maint;

namespace {

constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD kExStyle = WS_EX_CONTROLPARENT;
constexpr UINT WM_M_SCAN = WM_APP + 30;      // wp = MAKEWPARAM(tab, gen), lp = new'd ScanResult
constexpr UINT WM_M_PROGRESS = WM_APP + 31;  // wp = MAKEWPARAM(tab, gen), lp = new'd Progress
constexpr UINT WM_M_DONE = WM_APP + 32;      // wp = MAKEWPARAM(tab, gen), lp = exit code
constexpr LPARAM kDeclined = -1000, kNoStart = -1001;
constexpr COLORREF kGrey = RGB(105, 105, 105);
constexpr COLORREF kRed = RGB(196, 43, 28);
constexpr COLORREF kNavBg = RGB(243, 243, 243);
constexpr COLORREF kNavSel = RGB(226, 226, 226);
constexpr COLORREF kAccent = RGB(0, 95, 184);
// Layout in 96-DPI pixels.
constexpr int kW = 940, kH = 640, kMinW = 760, kMinH = 520;
constexpr int kNavW = 180, kNavItem = 34;

constexpr int kUpdates = 0, kCleanup = 1, kStartup = 2, kHealth = 3, kTabCount = 4;

struct TabDef {
    const wchar_t* name;
    const wchar_t* desc;
};
const TabDef kTabs[kTabCount] = {
    {L"Updates", L"Windows, driver and app updates. Nothing is installed until you press Install."},
    {L"Cleanup", L"Space used by temporary files. Nothing is deleted until you press Clean; files in use are skipped."},
    {L"Startup apps", L"Apps that start when you sign in. Turning one off doesn't remove it; you can turn it on again."},
    {L"Health", L"Restart advice, memory and disk space, and Windows' own repair and optimize tools."},
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
};

enum FontKind : int { kFontNormal, kFontBold, kFontTitle };

struct Ctl {
    HWND wnd;
    int tab;  // -1 = all tabs
    int font;
};

struct ScanResult {
    UpdateScan up;
    std::vector<CleanItem> clean;
    std::vector<StartupItem> startup;
};
struct Progress {
    std::wstring key, text;
};

struct UI {
    HWND hwnd = nullptr, nav = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr, title = nullptr;
    HBRUSH navBrush = nullptr;
    std::vector<Ctl> ctls;
    int tab = 0;
    WORD gen = 0;  // a new number each time the window opens: results for an older window are dropped
    bool busy[kTabCount]{};
    bool scanned[kTabCount]{};
    bool adminBusy = false;
    int adminTab = -1;
    bool filling = false;     // lists being filled: ignore check changes
    bool rebootNeeded = false;
    bool logPartial = false;  // the log's last line is a progress line the next one replaces
    UpdateScan up;
    std::vector<CleanItem> clean;
    std::vector<StartupItem> startup;
    Health health;
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
    HWND lv = Add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_SHOWSELALWAYS | LVS_NOSORTHEADER | WS_TABSTOP, IDC_LIST0 + t, t,
                  WS_EX_CLIENTEDGE);
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
    for (int t = 0; t < kTabCount; ++t) Static(IDC_STATUS0 + t, L"", t, SS_LEFT | SS_ENDELLIPSIS);

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
    HWND log = Add(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, IDC_HELOG,
                   kHealth, WS_EX_CLIENTEDGE);
    SendMessageW(log, EM_SETLIMITTEXT, 1 << 20, 0);
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
    for (const Ctl& c : U.ctls)
        SendMessageW(c.wnd, WM_SETFONT,
                     reinterpret_cast<WPARAM>(c.font == kFontTitle ? U.title : c.font == kFontBold ? U.bold : U.font), FALSE);
}

void Layout() {
    if (!U.hwnd) return;
    RECT rc;
    GetClientRect(U.hwnd, &rc);
    const int W = MulDiv(rc.right, 96, static_cast<int>(U.dpi)), H = MulDiv(rc.bottom, 96, static_cast<int>(U.dpi));
    const int L = kNavW + 20, R = W - 16, CW = R - L, top = 82;
    const int by = H - 14 - 28;  // footer buttons
    HDWP d = BeginDeferWindowPos(static_cast<int>(U.ctls.size()));
    auto put = [&](int id, int x, int y, int w, int h) {
        if (d) d = DeferWindowPos(d, Item(id), nullptr, Px(x), Px(y), Px(std::max(w, 0)), Px(std::max(h, 0)),
                                  SWP_NOZORDER | SWP_NOACTIVATE);
    };
    put(IDC_NAV, 8, 12, kNavW - 16, kTabCount * kNavItem + 4);
    put(IDC_TITLE, L, 14, CW, 32);
    put(IDC_DESC, L, 50, CW, 20);
    // Footer: status on the left, buttons on the right (widths in order from the right edge).
    auto footer = [&](int t, std::initializer_list<std::pair<int, int>> buttons) {
        int x = R;
        for (const auto& [id, w] : buttons) {
            x -= w;
            put(id, x, by, w, 28);
            x -= 8;
        }
        put(IDC_STATUS0 + t, L, by + 5, x - L - 8, 20);
    };
    // Updates
    footer(kUpdates, {{IDC_UPINSTALL, 130}, {IDC_UPSTORE, 180}, {IDC_UPCHECK, 110}});
    const int rowY = by - 38;
    put(IDC_UPRESTORE, L, rowY + 3, 240, 22);
    put(IDC_UPREBOOTBTN, R - 128, rowY, 128, 28);
    put(IDC_UPREBOOT, L + 250, rowY + 5, R - 136 - L - 250 - 8, 20);
    const int gpuY = rowY - 46;
    put(IDC_UPGPUBTN, R - 170, gpuY + 6, 170, 28);
    put(IDC_UPGPU, L, gpuY, CW - 180, 40);
    put(IDC_LIST0 + kUpdates, L, top, CW, gpuY - 8 - top);
    // Cleanup, Startup
    footer(kCleanup, {{IDC_CLRUN, 130}, {IDC_CLSTORAGE, 190}, {IDC_CLMEASURE, 130}});
    put(IDC_LIST0 + kCleanup, L, top, CW, by - 12 - top);
    footer(kStartup, {{IDC_STAPPLY, 130}, {IDC_STTASKMGR, 150}, {IDC_STREFRESH, 100}});
    put(IDC_LIST0 + kStartup, L, top, CW, by - 12 - top);
    // Health
    footer(kHealth, {});
    put(IDC_HEINFO, L, top, CW, 96);
    int x = L;
    for (const auto& [id, w] : {std::pair{IDC_HERESTART, 120}, {IDC_HEREPAIR, 160}, {IDC_HEOPT, 140}, {IDC_HERESTORE, 170}}) {
        put(id, x, top + 104, w, 28);
        x += w + 8;
    }
    put(IDC_HELOG, L, top + 144, CW, by - 12 - top - 144);
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
        ListView_SetColumnWidth(lv, fill, std::max(Px(120), static_cast<int>(lr.right) - used - 2));
    };
    cols(kUpdates, {80, 170, 70, 150}, 0);
    cols(kCleanup, {230, 100, 150}, 2);
    cols(kStartup, {200, 170, 150}, 2);
    InvalidateRect(U.hwnd, nullptr, TRUE);
}

// ---- tab state ---------------------------------------------------------------------------

void UpdateButtons() {
    const bool up = !U.busy[kUpdates], cl = !U.busy[kCleanup], st = !U.busy[kStartup], he = !U.busy[kHealth];
    for (int id : {IDC_UPCHECK, IDC_UPINSTALL, IDC_UPRESTORE}) EnableWindow(Item(id), up);
    EnableWindow(Item(IDC_UPINSTALL), up && U.scanned[kUpdates]);
    for (int id : {IDC_CLMEASURE, IDC_CLRUN}) EnableWindow(Item(id), cl);
    for (int id : {IDC_STREFRESH, IDC_STAPPLY}) EnableWindow(Item(id), st);
    for (int id : {IDC_HEREPAIR, IDC_HEOPT, IDC_HERESTORE}) EnableWindow(Item(id), he);
}

void ShowTab(int t) {
    U.tab = g_lastTab = std::clamp(t, 0, kTabCount - 1);
    for (const Ctl& c : U.ctls) {
        bool show = c.tab < 0 || c.tab == U.tab;
        const int id = GetDlgCtrlID(c.wnd);
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
}

void StartScan(int t);

void OpenTab(int t) {
    ShowTab(t);
    if (!U.scanned[U.tab] && !U.busy[U.tab]) StartScan(U.tab);
}

// ---- filling the lists -------------------------------------------------------------------

int AddRow(HWND lv, int i, const std::vector<std::wstring>& cells, bool checked) {
    LVITEMW it{LVIF_TEXT | LVIF_PARAM};
    it.iItem = i;
    it.lParam = i;
    it.pszText = const_cast<wchar_t*>(cells[0].c_str());
    const int row = ListView_InsertItem(lv, &it);
    for (size_t c = 1; c < cells.size(); ++c) ListView_SetItemText(lv, row, static_cast<int>(c), const_cast<wchar_t*>(cells[c].c_str()));
    ListView_SetCheckState(lv, row, checked);
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
        AddRow(lv, i, {u.name, kind, u.version, u.size ? SizeText(u.size) : L"", L""}, on);
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
        AddRow(lv, i, {c.name, size, c.note, L""}, c.byDefault);
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
        AddRow(lv, i, {s.name, s.where, s.command, L""}, s.on);
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

void FillHealth() {
    U.health = ReadHealth();
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

// ---- checks and jobs ---------------------------------------------------------------------

WPARAM Tag(int t) { return MAKEWPARAM(t, U.gen); }

void StartScan(int t) {
    if (U.busy[t]) return;
    if (t == kHealth) {
        FillHealth();
        U.scanned[t] = true;
        return;
    }
    U.busy[t] = true;
    UpdateButtons();
    SetStatus(t, t == kUpdates ? L"Checking for updates…" : t == kCleanup ? L"Measuring…" : L"Reading startup apps…");
    HWND w = U.hwnd;
    const WPARAM tag = Tag(t);
    std::thread([w, t, tag] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* r = new ScanResult;
        if (t == kUpdates) {
            r->up = ScanUpdates([w, tag](const std::wstring& k, const std::wstring& s) {
                PostOrDelete(w, WM_M_PROGRESS, tag, new Progress{k, s});
            });
        } else if (t == kCleanup) {
            r->clean = ScanCleanup();
        } else {
            r->startup = ScanStartup();
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
        if (RebootPending()) U.rebootNeeded = true;
        ShowTab(U.tab);
    } else if (t == kCleanup) {
        U.clean = std::move(r->clean);
        FillCleanup();
    } else if (t == kStartup) {
        U.startup = std::move(r->startup);
        FillStartup();
    }
    UpdateButtons();
}

// Runs a job: here on a worker thread, or in an elevated copy when it needs administrator rights.
void StartTask(int t, const std::wstring& task, const std::vector<std::wstring>& args, bool admin) {
    if (U.busy[t]) return;
    HWND w = U.hwnd;
    const WPARAM tag = Tag(t);
    if (admin && !IsElevated()) {
        if (U.adminBusy) {
            SetStatus(t, L"Another job with administrator rights is still running. Try again when it's done.");
            return;
        }
        U.adminBusy = true;
        U.adminTab = t;
        std::wstring params = L"--maint-admin " + std::to_wstring(reinterpret_cast<UINT_PTR>(w)) + L" " + task;
        for (const auto& a : args) params += L" " + QuoteArg(a);
        SetStatus(t, L"Waiting for permission (the Windows prompt)…");
        std::thread([w, tag, params] {
            CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            wchar_t exe[MAX_PATH];
            GetModuleFileNameW(nullptr, exe, MAX_PATH);
            SHELLEXECUTEINFOW sei{sizeof(sei)};
            sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
            sei.hwnd = w;
            sei.lpVerb = L"runas";
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
            PostMessageW(w, WM_M_DONE, tag, code);
        }).detach();
    } else {
        std::thread([w, tag, task, args] {
            const int code = RunTask(task, args, [w, tag](const std::wstring& k, const std::wstring& s) {
                PostOrDelete(w, WM_M_PROGRESS, tag, new Progress{k, s});
            });
            PostMessageW(w, WM_M_DONE, tag, code);
        }).detach();
    }
    U.busy[t] = true;
    UpdateButtons();
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
    if (key == L"status") {
        SetStatus(t, text);
    } else if (key == L"reboot") {
        U.rebootNeeded = true;
        ShowTab(U.tab);
    } else if (key == L"log" || key == L"logp") {
        LogLine(text, key == L"logp");
        if (t != kHealth && !text.empty()) SetStatus(t, text);
    } else {
        // An item's row: its last column shows the text.
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
    if (U.adminTab == t) {
        U.adminBusy = false;
        U.adminTab = -1;
    }
    U.busy[t] = false;
    if (code == kDeclined) SetStatus(t, L"Cancelled: Windows didn't get permission to make changes.");
    else if (code == kNoStart) SetStatus(t, L"Couldn't start the job with administrator rights.");
    if (t == kCleanup && code != kDeclined && code != kNoStart) {
        const std::wstring keep = L"Cleaned. Measuring again…";
        StartScan(kCleanup);
        SetStatus(t, keep);
    } else if (t == kStartup && code != kDeclined && code != kNoStart) {
        StartScan(kStartup);
    } else if (t == kHealth) {
        FillHealth();
    } else if (t == kUpdates && code != kDeclined && code != kNoStart) {
        SetStatus(t, code == 0 ? L"Finished. Press Check again to see what's left." : L"Finished, with problems: see the Status column.");
    }
    if (U.rebootNeeded) FillHealth();
    UpdateButtons();
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
            ListView_SetItemText(lv, r, 4, const_cast<wchar_t*>(L"Waiting…"));
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
    StartTask(kUpdates, L"install", args, true);
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
            ListView_SetItemText(lv, r, 3, const_cast<wchar_t*>(L"Waiting…"));
        }
    if (args.empty()) {
        SetStatus(kCleanup, L"Nothing selected.");
        return;
    }
    if (recycle && MessageBoxW(U.hwnd, L"Empty the Recycle Bin? The files in it can't be brought back afterwards.",
                               L"Canivete Maintenance", MB_OKCANCEL | MB_ICONWARNING) != IDOK)
        return;
    StartTask(kCleanup, L"clean", args, admin);
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

// ---- drawing the tab list ----------------------------------------------------------------

void FillColor(HDC dc, const RECT& r, COLORREF c) {
    SetDCBrushColor(dc, c);
    FillRect(dc, &r, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

void DrawNavItem(const DRAWITEMSTRUCT& d) {
    if (d.itemID == static_cast<UINT>(-1)) return;
    HDC dc = d.hDC;
    FillColor(dc, d.rcItem, kNavBg);
    RECT b = d.rcItem;
    InflateRect(&b, -Px(2), -Px(2));
    if (d.itemState & ODS_SELECTED) {
        SetDCBrushColor(dc, kNavSel);
        SetDCPenColor(dc, kNavSel);
        HGDIOBJ ob = SelectObject(dc, GetStockObject(DC_BRUSH));
        HGDIOBJ op = SelectObject(dc, GetStockObject(DC_PEN));
        RoundRect(dc, b.left, b.top, b.right, b.bottom, Px(8), Px(8));
        SelectObject(dc, ob);
        SelectObject(dc, op);
        RECT bar{b.left, b.top + Px(9), b.left + Px(3), b.bottom - Px(9)};
        FillColor(dc, bar, kAccent);
    }
    HGDIOBJ of = SelectObject(dc, U.font);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_WINDOWTEXT));
    RECT tr = b;
    tr.left += Px(14);
    tr.right -= Px(10);
    DrawTextW(dc, kTabs[d.itemID].name, -1, &tr, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX | DT_END_ELLIPSIS);
    if (U.busy[d.itemID]) {
        SetTextColor(dc, kGrey);
        DrawTextW(dc, L"…", -1, &tr, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
    }
    SelectObject(dc, of);
    if ((d.itemState & ODS_FOCUS) && !(d.itemState & ODS_NOFOCUSRECT)) DrawFocusRect(dc, &b);
}

// ---- window ------------------------------------------------------------------------------

void OnCommand(int id, int code) {
    switch (id) {
    case IDCANCEL:
        DestroyWindow(U.hwnd);
        break;
    case IDOK: {  // Enter: the tab's main button
        const int main[kTabCount] = {IDC_UPINSTALL, IDC_CLRUN, IDC_STAPPLY, 0};
        if (main[U.tab] && IsWindowEnabled(Item(main[U.tab]))) OnCommand(main[U.tab], BN_CLICKED);
        break;
    }
    case IDC_NAV:
        if (code == LBN_SELCHANGE) OpenTab(static_cast<int>(SendMessageW(U.nav, LB_GETCURSEL, 0, 0)));
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
        U.navBrush = CreateSolidBrush(kNavBg);
        Build();
        MakeFonts();
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
        FillRect(dc, &rc, GetSysColorBrush(COLOR_WINDOW));
        RECT nav{0, 0, Px(kNavW), rc.bottom};
        FillColor(dc, nav, kNavBg);
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
        return reinterpret_cast<LRESULT>(U.navBrush);
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        COLORREF fg = GetSysColor(COLOR_WINDOWTEXT);
        if (id == IDC_DESC || id == IDC_UPGPU || (id >= IDC_STATUS0 && id < IDC_STATUS0 + kTabCount)) fg = kGrey;
        if (id == IDC_UPREBOOT) fg = kRed;
        SetBkColor(dc, GetSysColor(COLOR_WINDOW));
        SetTextColor(dc, fg);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    }
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_NOTIFY: {
        const auto* n = reinterpret_cast<const NMHDR*>(lp);
        if (n->code == LVN_ITEMCHANGED && !U.filling) {
            const auto* lv = reinterpret_cast<const NMLISTVIEW*>(lp);
            if ((lv->uChanged & LVIF_STATE) && ((lv->uNewState ^ lv->uOldState) & LVIS_STATEIMAGEMASK))
                Summarize(static_cast<int>(n->idFrom) - IDC_LIST0);
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
        if (cd->dwData != kCopyProgress || !U.adminBusy || cd->cbData < sizeof(wchar_t)) return FALSE;
        const std::wstring s(static_cast<const wchar_t*>(cd->lpData),
                             wcsnlen(static_cast<const wchar_t*>(cd->lpData), cd->cbData / sizeof(wchar_t)));
        const size_t tab = s.find(L'\t');
        if (tab != std::wstring::npos) OnProgress(U.adminTab, s.substr(0, tab), s.substr(tab + 1));
        return TRUE;
    }
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) {
            HWND f = GetFocus();
            if (f && IsChild(h, f)) U.lastFocus = f;
        } else {
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
        U.hwnd = U.nav = nullptr;
        U.ctls.clear();
        U.lastFocus = nullptr;
        for (bool& b : U.busy) b = false;
        for (bool& b : U.scanned) b = false;
        U.adminBusy = false;
        U.adminTab = -1;
        U.logPartial = false;
        return 0;
    case WM_NCDESTROY:
        for (HGDIOBJ o : {static_cast<HGDIOBJ>(U.font), static_cast<HGDIOBJ>(U.bold), static_cast<HGDIOBJ>(U.title),
                          static_cast<HGDIOBJ>(U.navBrush)})
            if (o) DeleteObject(o);
        U.font = U.bold = U.title = nullptr;
        U.navBrush = nullptr;
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
    }
    ++U.gen;
    U.up = {};
    U.clean.clear();
    U.startup.clear();
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
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
}
