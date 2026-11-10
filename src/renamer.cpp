// File and folder renamer: search and replace (plain or regex) with a live preview, counters,
// date tokens and text case. Renames in one IFileOperation, so Ctrl+Z in Explorer undoes it.
#include "app.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <optional>
#include <regex>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClass[] = L"CaniveteRenamer";
constexpr wchar_t kTitle[] = L"Canivete Rename";
constexpr DWORD kStyle = WS_OVERLAPPEDWINDOW;
constexpr DWORD kExStyle = WS_EX_CONTROLPARENT | WS_EX_ACCEPTFILES;
constexpr UINT_PTR kTimerPreview = 1;
constexpr size_t kMaxItems = 20000;
constexpr COLORREF kGrey = RGB(120, 120, 120);
constexpr COLORREF kRed = RGB(200, 30, 30);
// Layout in 96-DPI pixels.
constexpr int kW = 680, kH = 620, kMinW = 560, kMinH = 440;
constexpr int kM = 16, kCX = 112, kFooter = 56;

enum : int {
    IDC_STATIC = 400,
    IDC_SEARCH,
    IDC_REPLACE,
    IDC_TOKENS,
    IDC_REGEX,
    IDC_ALL,
    IDC_MATCHCASE,
    IDC_APPLY,
    IDC_CASE,
    IDC_FILES,
    IDC_FOLDERS,
    IDC_SUB,
    IDC_LIST,
    IDC_STATUS,
};

enum : UINT { ID_TOKEN0 = 500 };
struct Token {
    const wchar_t* label;
    const wchar_t* text;
};
const Token kTokens[] = {
    {L"Counter 1, 2, 3\t${}", L"${}"},
    {L"Counter 001, 002, 003\t${padding=3}", L"${padding=3}"},
    {L"Counter from 10 in steps of 10\t${start=10,increment=10}", L"${start=10,increment=10}"},
    {nullptr, nullptr},
    {L"Year (file date)\t$YYYY", L"$YYYY"},
    {L"Month\t$MM", L"$MM"},
    {L"Day\t$DD", L"$DD"},
    {L"Hour\t$hh", L"$hh"},
    {L"Minute\t$mm", L"$mm"},
    {L"Second\t$ss", L"$ss"},
    {nullptr, nullptr},
    {L"Whole match (regular expression)\t$&", L"$&"},
    {L"Group 1 (regular expression)\t$1", L"$1"},
};

// Anchors: which edges a control follows when the window is resized.
enum : int { kRight = 1, kStretchW = 2, kBottom = 4, kStretchH = 8 };

struct Ctl {
    HWND wnd;
    int x, y, w, h;  // kRight: x = distance from the right edge; kStretchW: w = right margin (same for y/h)
    int anchor;
    bool bold;
};

struct Item {
    std::wstring path, dir, name;
    std::wstring shown;  // name, or "folder\sub\name" for things found inside a chosen folder
    bool isDir = false;
    bool nested = false;    // found inside a chosen folder
    bool expanded = false;  // its contents were added
    int depth = 0;
    FILETIME modified{};
};

struct Row {
    size_t item;
    std::wstring newName;
    const wchar_t* problem = nullptr;
    bool changes = false;
};

struct Opts {
    std::wstring search, replace;
    bool regex = false, all = true, matchCase = false;
    int applyTo = 0;   // 0 name, 1 extension, 2 both
    int textCase = 0;  // 0 unchanged, 1 lower, 2 UPPER, 3 Title, 4 Sentence
    bool files = true, folders = true, sub = false;
};

struct UI {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    std::vector<Ctl> ctls;
    std::vector<Item> items;
    std::vector<Row> rows;
    int changes = 0, problems = 0;
    bool badRegex = false;
} U;

int Px(int v) { return MulDiv(v, static_cast<int>(U.dpi), 96); }
HWND Ctrl(int id) { return GetDlgItem(U.hwnd, id); }
bool Checked(int id) { return IsDlgButtonChecked(U.hwnd, id) == BST_CHECKED; }
void SetCheck(int id, bool on) { CheckDlgButton(U.hwnd, id, on ? BST_CHECKED : BST_UNCHECKED); }
int Sel(int id) { return static_cast<int>(SendMessageW(Ctrl(id), CB_GETCURSEL, 0, 0)); }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring GetText(int id) {
    HWND c = Ctrl(id);
    std::wstring s(GetWindowTextLengthW(c), L'\0');
    GetWindowTextW(c, s.data(), static_cast<int>(s.size()) + 1);
    return s;
}

// ---- remembered choices ([Renamer] in settings.ini) -----------------------------------------

std::wstring IniStr(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(L"Renamer", key, def, buf, 2048, SettingsPath().c_str());
    return buf;
}
int IniInt(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"Renamer", key, def, SettingsPath().c_str()); }
void IniPut(const wchar_t* key, const std::wstring& v) {
    // INI values lose surrounding spaces and quotes, so wrap them in quotes (stripped on read).
    WritePrivateProfileStringW(L"Renamer", key, v.c_str(), SettingsPath().c_str());
}
void IniPutQuoted(const wchar_t* key, const std::wstring& v) { IniPut(key, L"\"" + v + L"\""); }
void IniPutInt(const wchar_t* key, int v) { IniPut(key, std::to_wstring(v)); }

// ---- the renaming rules -------------------------------------------------------------------

// ${} counters and $YYYY-style date tokens (file's last-modified date, local time).
std::wstring ExpandTokens(const std::wstring& s, int index, const FILETIME& modified) {
    if (s.find(L'$') == std::wstring::npos) return s;
    SYSTEMTIME st{};
    FILETIME local;
    FileTimeToLocalFileTime(&modified, &local);
    FileTimeToSystemTime(&local, &st);
    auto two = [](int v) {
        wchar_t b[8];
        swprintf_s(b, L"%02d", v);
        return std::wstring(b);
    };
    std::wstring out;
    for (size_t i = 0; i < s.size();) {
        if (s[i] != L'$') {
            out += s[i++];
            continue;
        }
        const std::wstring rest = s.substr(i);
        if (rest.rfind(L"${", 0) == 0) {
            const size_t end = s.find(L'}', i);
            if (end != std::wstring::npos) {
                long long start = 1, inc = 1;
                int pad = 0;
                std::wstring args = s.substr(i + 2, end - i - 2);
                size_t p = 0;
                while (p <= args.size()) {
                    size_t comma = args.find(L',', p);
                    if (comma == std::wstring::npos) comma = args.size();
                    const std::wstring kv = args.substr(p, comma - p);
                    const size_t eq = kv.find(L'=');
                    if (eq != std::wstring::npos) {
                        std::wstring k = Lower(kv.substr(0, eq));
                        k.erase(std::remove_if(k.begin(), k.end(), iswspace), k.end());
                        const long long v = _wtoi64(kv.c_str() + eq + 1);
                        if (k == L"start") start = v;
                        else if (k == L"increment") inc = v;
                        else if (k == L"padding") pad = static_cast<int>(std::clamp<long long>(v, 0, 20));
                    }
                    p = comma + 1;
                }
                const long long n = start + inc * index;
                std::wstring num = std::to_wstring(n < 0 ? -n : n);
                if (static_cast<int>(num.size()) < pad) num.insert(0, pad - num.size(), L'0');
                out += (n < 0 ? L"-" : L"") + num;
                i = end + 1;
                continue;
            }
        }
        struct {
            const wchar_t* tok;
            std::wstring val;
        } const dates[] = {{L"$YYYY", std::to_wstring(st.wYear)}, {L"$YY", two(st.wYear % 100)},
                           {L"$MM", two(st.wMonth)},              {L"$DD", two(st.wDay)},
                           {L"$hh", two(st.wHour)},               {L"$mm", two(st.wMinute)},
                           {L"$ss", two(st.wSecond)}};
        bool hit = false;
        for (const auto& d : dates) {
            const size_t n = wcslen(d.tok);
            if (s.compare(i, n, d.tok) == 0) {
                out += d.val;
                i += n;
                hit = true;
                break;
            }
        }
        if (!hit) out += s[i++];
    }
    return out;
}

std::wstring ReplacePlain(const std::wstring& text, const Opts& o, const std::wstring& repl) {
    const std::wstring hay = o.matchCase ? text : Lower(text);
    const std::wstring needle = o.matchCase ? o.search : Lower(o.search);
    std::wstring out;
    size_t from = 0;
    for (;;) {
        const size_t at = hay.find(needle, from);
        if (at == std::wstring::npos) break;
        out += text.substr(from, at - from) + repl;
        from = at + needle.size();
        if (!o.all) break;
    }
    return out + text.substr(from);
}

std::wstring ApplyCase(std::wstring s, int mode) {
    if (s.empty() || mode == 0) return s;
    if (mode == 1) {
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
    } else if (mode == 2) {
        CharUpperBuffW(s.data(), static_cast<DWORD>(s.size()));
    } else {
        CharLowerBuffW(s.data(), static_cast<DWORD>(s.size()));
        bool start = true;
        for (auto& c : s) {
            if (iswalnum(c)) {
                if (start) CharUpperBuffW(&c, 1);
                start = false;
                if (mode == 4) break;  // sentence: only the first letter
            } else if (mode == 3 && c != L'\'') {
                start = true;
            }
        }
    }
    return s;
}

const wchar_t* InvalidName(const std::wstring& n) {
    if (n.empty()) return L"empty name";
    if (n.find_first_of(L"<>:\"/\\|?*") != std::wstring::npos) return L"has a character Windows doesn't allow";
    if (std::any_of(n.begin(), n.end(), [](wchar_t c) { return c < 32; })) return L"has a control character";
    if (n.back() == L' ' || n.back() == L'.') return L"ends with a space or dot";
    if (n.size() > 255) return L"name too long";
    std::wstring stem = n.substr(0, n.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    CharUpperBuffW(stem.data(), static_cast<DWORD>(stem.size()));
    static const wchar_t* const kReserved[] = {L"CON", L"PRN", L"AUX", L"NUL"};
    if (std::any_of(std::begin(kReserved), std::end(kReserved), [&](const wchar_t* r) { return stem == r; }) ||
        (stem.size() == 4 && (stem.rfind(L"COM", 0) == 0 || stem.rfind(L"LPT", 0) == 0) && stem[3] >= L'1' &&
         stem[3] <= L'9'))
        return L"name reserved by Windows";
    return nullptr;
}

bool Included(const Item& it, const Opts& o) { return (it.isDir ? o.folders : o.files) && (!it.nested || o.sub); }

Opts ReadOpts() {
    Opts o;
    o.search = GetText(IDC_SEARCH);
    o.replace = GetText(IDC_REPLACE);
    o.regex = Checked(IDC_REGEX);
    o.all = Checked(IDC_ALL);
    o.matchCase = Checked(IDC_MATCHCASE);
    o.applyTo = std::max(0, Sel(IDC_APPLY));
    o.textCase = std::max(0, Sel(IDC_CASE));
    o.files = Checked(IDC_FILES);
    o.folders = Checked(IDC_FOLDERS);
    o.sub = Checked(IDC_SUB);
    return o;
}

// ---- items ---------------------------------------------------------------------------------

Item MakeItem(const std::wstring& path, const WIN32_FIND_DATAW* fd) {
    Item it;
    it.path = path;
    const size_t slash = path.find_last_of(L'\\');
    it.dir = path.substr(0, slash);
    it.name = path.substr(slash + 1);
    it.shown = it.name;
    it.depth = static_cast<int>(std::count(path.begin(), path.end(), L'\\'));
    if (fd) {
        it.isDir = (fd->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.modified = fd->ftLastWriteTime;
    } else {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a);
        it.isDir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        it.modified = a.ftLastWriteTime;
    }
    return it;
}

// Adds everything inside `root` (files and subfolders, all levels). Skips system files such as
// desktop.ini and doesn't follow links (no loops).
void Expand(Item& root, std::vector<Item>& out) {
    root.expanded = true;
    std::vector<std::pair<std::wstring, std::wstring>> todo{{root.path, root.name}};
    while (!todo.empty() && U.items.size() + out.size() < kMaxItems) {
        auto [dir, shown] = todo.back();
        todo.pop_back();
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                    FIND_FIRST_EX_LARGE_FETCH);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_SYSTEM) continue;
            Item it = MakeItem(dir + L"\\" + fd.cFileName, &fd);
            it.nested = true;
            it.shown = shown + L"\\" + it.name;
            if (it.isDir && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) todo.push_back({it.path, it.shown});
            out.push_back(std::move(it));
        } while (FindNextFileW(h, &fd) && U.items.size() + out.size() < kMaxItems);
        FindClose(h);
    }
}

void SortItems() {
    std::stable_sort(U.items.begin(), U.items.end(), [](const Item& a, const Item& b) {
        const int d = StrCmpLogicalW(a.dir.c_str(), b.dir.c_str());
        return d != 0 ? d < 0 : StrCmpLogicalW(a.name.c_str(), b.name.c_str()) < 0;
    });
}

// When "what's inside folders" is on, adds the contents of chosen folders not yet expanded.
void ExpandChosenFolders() {
    if (!Checked(IDC_SUB)) return;
    std::vector<Item> found;
    for (Item& it : U.items)
        if (it.isDir && !it.nested && !it.expanded) Expand(it, found);
    if (found.empty()) return;
    std::unordered_map<std::wstring, bool> have;
    for (const Item& it : U.items) have[Lower(it.path)] = true;
    for (Item& f : found)
        if (!have[Lower(f.path)]) U.items.push_back(std::move(f));
    SortItems();
}

void AddPaths(const std::vector<std::wstring>& paths) {
    std::unordered_map<std::wstring, bool> have;
    for (const Item& it : U.items) have[Lower(it.path)] = true;
    for (std::wstring p : paths) {
        while (p.size() > 3 && (p.back() == L'\\' || p.back() == L'/')) p.pop_back();
        if (p.size() <= 3 || GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) continue;  // not drives
        if (have[Lower(p)]) {  // already there (maybe found inside a folder): make it a chosen item
            for (Item& it : U.items)
                if (Lower(it.path) == Lower(p)) {
                    it.nested = false;
                    it.shown = it.name;
                }
            continue;
        }
        have[Lower(p)] = true;
        U.items.push_back(MakeItem(p, nullptr));
        if (U.items.size() >= kMaxItems) break;
    }
    SortItems();
    ExpandChosenFolders();
}

// ---- preview -------------------------------------------------------------------------------

void SetStatus(const std::wstring& t) { SetWindowTextW(Ctrl(IDC_STATUS), t.c_str()); }

void Recompute() {
    KillTimer(U.hwnd, kTimerPreview);
    const Opts o = ReadOpts();
    std::optional<std::wregex> re;
    U.badRegex = false;
    if (o.regex && !o.search.empty()) {
        try {
            re.emplace(o.search, o.matchCase ? std::regex::ECMAScript : std::regex::ECMAScript | std::regex::icase);
        } catch (const std::regex_error&) {
            U.badRegex = true;
        }
    }
    U.rows.clear();
    int index = 0;
    for (size_t i = 0; i < U.items.size(); ++i) {
        const Item& it = U.items[i];
        if (!Included(it, o)) continue;
        Row r{i, it.name};
        if (!U.badRegex) {
            std::wstring stem = it.name, ext;
            const size_t dot = it.name.find_last_of(L'.');
            if (!it.isDir && dot != std::wstring::npos && dot > 0) {
                stem = it.name.substr(0, dot);
                ext = it.name.substr(dot + 1);
            }
            std::wstring& part = o.applyTo == 0 ? stem : o.applyTo == 1 ? ext : r.newName;
            if (o.applyTo != 2) r.newName.clear();
            // Only items the search actually hits use up a counter number.
            const bool hit = o.search.empty() ||
                             (re ? std::regex_search(part, *re) : ReplacePlain(part, o, L"￿") != part);
            if (hit && (!o.search.empty() || !o.replace.empty())) {
                const std::wstring repl = ExpandTokens(o.replace, index++, it.modified);
                if (o.search.empty()) {
                    part = repl;  // nothing to search for: the new text replaces the whole part
                } else if (re) {
                    try {
                        part = std::regex_replace(part, *re, repl,
                                                  o.all ? std::regex_constants::format_default
                                                        : std::regex_constants::format_first_only);
                    } catch (const std::regex_error&) {
                        U.badRegex = true;
                    }
                } else {
                    part = ReplacePlain(part, o, repl);
                }
            }
            part = ApplyCase(part, o.textCase);
            if (o.applyTo != 2) r.newName = ext.empty() ? stem : stem + L"." + ext;  // no extension: no dot
        }
        r.changes = r.newName != it.name;
        U.rows.push_back(std::move(r));
    }

    // Problems: bad names, two items getting the same name, or a name that's already taken.
    std::unordered_map<std::wstring, int> taken;
    for (const Row& r : U.rows)
        if (r.changes) ++taken[Lower(U.items[r.item].dir + L"\\" + r.newName)];
    U.changes = U.problems = 0;
    for (Row& r : U.rows) {
        if (!r.changes) continue;
        const Item& it = U.items[r.item];
        const std::wstring target = it.dir + L"\\" + r.newName;
        if (const wchar_t* bad = InvalidName(r.newName)) r.problem = bad;
        else if (taken[Lower(target)] > 1) r.problem = L"two items would get this name";
        else if (Lower(r.newName) != Lower(it.name) && GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES)
            r.problem = L"that name is already taken";
        else if (it.path.size() - it.name.size() + r.newName.size() >= 32000) r.problem = L"path too long";
        if (r.problem) ++U.problems;
        else ++U.changes;
    }

    HWND list = Ctrl(IDC_LIST);
    ListView_SetItemCountEx(list, static_cast<int>(U.rows.size()), LVSICF_NOSCROLL);
    InvalidateRect(list, nullptr, FALSE);
    std::wstring status;
    if (U.items.empty()) status = L"Drop files or folders here.";
    else if (U.badRegex) status = L"The search isn't a valid regular expression.";
    else {
        status = std::to_wstring(U.changes) + L" of " + std::to_wstring(U.rows.size()) + L" will be renamed";
        if (U.problems) status += L",  " + std::to_wstring(U.problems) + L" can't (in red)";
    }
    SetStatus(status);
    EnableWindow(Ctrl(IDOK), U.changes > 0);
    const std::wstring title = U.items.empty() ? std::wstring(kTitle)
                                               : std::wstring(kTitle) + L" – " + std::to_wstring(U.rows.size()) +
                                                     (U.rows.size() == 1 ? L" item" : L" items");
    SetWindowTextW(U.hwnd, title.c_str());
}

void RemoveSelected() {
    HWND list = Ctrl(IDC_LIST);
    std::vector<std::wstring> gone;
    for (int i = -1; (i = ListView_GetNextItem(list, i, LVNI_SELECTED)) >= 0;)
        if (i < static_cast<int>(U.rows.size())) gone.push_back(Lower(U.items[U.rows[i].item].path));
    if (gone.empty()) return;
    std::erase_if(U.items, [&](const Item& it) {
        const std::wstring p = Lower(it.path);
        return std::any_of(gone.begin(), gone.end(), [&](const std::wstring& g) {
            return p == g || (p.size() > g.size() && p.compare(0, g.size(), g) == 0 && p[g.size()] == L'\\');
        });
    });
    ListView_SetItemState(list, -1, 0, LVIS_SELECTED);
    Recompute();
}

// ---- renaming ------------------------------------------------------------------------------

// Counts what IFileOperation actually renamed.
struct Sink : IFileOperationProgressSink {
    int ok = 0, failed = 0;
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IUnknown || riid == __uuidof(IFileOperationProgressSink)) {
            *ppv = static_cast<IFileOperationProgressSink*>(this);
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return 2; }  // lives on the stack
    STDMETHODIMP_(ULONG) Release() override { return 1; }
    STDMETHODIMP PostRenameItem(DWORD, IShellItem*, LPCWSTR, HRESULT hr, IShellItem*) override {
        // Success comes back as S_OK or a COPYENGINE_S_* code (e.g. 0x00270008).
        (SUCCEEDED(hr) ? ok : failed)++;
        if (FAILED(hr)) Log(L"Rename failed: 0x%08X", static_cast<unsigned>(hr));
        return S_OK;
    }
    STDMETHODIMP StartOperations() override { return S_OK; }
    STDMETHODIMP FinishOperations(HRESULT) override { return S_OK; }
    STDMETHODIMP PreRenameItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PreMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostMoveItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostCopyItem(DWORD, IShellItem*, IShellItem*, LPCWSTR, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreDeleteItem(DWORD, IShellItem*) override { return S_OK; }
    STDMETHODIMP PostDeleteItem(DWORD, IShellItem*, HRESULT, IShellItem*) override { return S_OK; }
    STDMETHODIMP PreNewItem(DWORD, IShellItem*, LPCWSTR) override { return S_OK; }
    STDMETHODIMP PostNewItem(DWORD, IShellItem*, LPCWSTR, LPCWSTR, DWORD, HRESULT, IShellItem*) override {
        return S_OK;
    }
    STDMETHODIMP UpdateProgress(UINT, UINT) override { return S_OK; }
    STDMETHODIMP ResetTimer() override { return S_OK; }
    STDMETHODIMP PauseTimer() override { return S_OK; }
    STDMETHODIMP ResumeTimer() override { return S_OK; }
};

void SaveOptions(const Opts& o) {
    IniPutQuoted(L"Search", o.search);
    IniPutQuoted(L"Replace", o.replace);
    IniPutInt(L"Regex", o.regex);
    IniPutInt(L"AllMatches", o.all);
    IniPutInt(L"MatchCase", o.matchCase);
    IniPutInt(L"ApplyTo", o.applyTo);
    IniPutInt(L"TextCase", o.textCase);
    IniPutInt(L"Files", o.files);
    IniPutInt(L"Folders", o.folders);
    IniPutInt(L"Subfolders", o.sub);
}

void DoRename() {
    Recompute();
    if (U.changes == 0) return;
    if (U.problems) {
        const std::wstring q = std::to_wstring(U.problems) + L" item(s) in red can't be renamed and will be left as they are.\n\nRename the other " +
                               std::to_wstring(U.changes) + L"?";
        if (MessageBoxW(U.hwnd, q.c_str(), kTitle, MB_ICONQUESTION | MB_YESNO) != IDYES) return;
    }
    SaveOptions(ReadOpts());
    std::vector<const Row*> todo;
    for (const Row& r : U.rows)
        if (r.changes && !r.problem) todo.push_back(&r);
    // Deepest first, so a folder is renamed after everything inside it.
    std::stable_sort(todo.begin(), todo.end(),
                     [](const Row* a, const Row* b) { return U.items[a->item].depth > U.items[b->item].depth; });

    ComPtr<IFileOperation> op;
    if (FAILED(CoCreateInstance(CLSID_FileOperation, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&op)))) return;
    op->SetOwnerWindow(U.hwnd);
    op->SetOperationFlags(FOF_ALLOWUNDO | FOFX_ADDUNDORECORD | FOF_NOCONFIRMMKDIR | FOFX_SHOWELEVATIONPROMPT);
    Sink sink;
    DWORD cookie = 0;
    op->Advise(&sink, &cookie);
    int queued = 0;
    for (const Row* r : todo) {
        ComPtr<IShellItem> si;
        if (SUCCEEDED(SHCreateItemFromParsingName(U.items[r->item].path.c_str(), nullptr, IID_PPV_ARGS(&si))) &&
            SUCCEEDED(op->RenameItem(si.Get(), r->newName.c_str(), nullptr)))
            ++queued;
    }
    SetStatus(L"Renaming…");
    const HRESULT hr = op->PerformOperations();
    BOOL aborted = FALSE;
    op->GetAnyOperationsAborted(&aborted);
    op->Unadvise(cookie);
    const int failed = sink.failed + (queued - sink.ok - sink.failed) + static_cast<int>(todo.size()) - queued;
    Log(L"Rename: %d renamed, %d not (hr 0x%08X%s)", sink.ok, failed, static_cast<unsigned>(hr),
        aborted ? L", cancelled" : L"");

    if (failed == 0 && !aborted && SUCCEEDED(hr)) {
        Tray_Notify((std::to_wstring(sink.ok) + (sink.ok == 1 ? L" item renamed" : L" items renamed")).c_str(),
                    L"Ctrl+Z in Explorer undoes it.");
        DestroyWindow(U.hwnd);
        return;
    }
    // Some didn't work: keep the window, pointing at the items as they are now.
    for (const Row* r : todo) {
        Item& it = U.items[r->item];
        const std::wstring now = it.dir + L"\\" + r->newName;
        if (GetFileAttributesW(it.path.c_str()) == INVALID_FILE_ATTRIBUTES &&
            GetFileAttributesW(now.c_str()) != INVALID_FILE_ATTRIBUTES) {
            it.path = now;
            it.name = r->newName;
            if (!it.nested) it.shown = it.name;
        }
    }
    std::erase_if(U.items, [](const Item& it) { return it.nested; });  // re-read folder contents
    for (Item& it : U.items) it.expanded = false;
    std::erase_if(U.items, [](const Item& it) { return GetFileAttributesW(it.path.c_str()) == INVALID_FILE_ATTRIBUTES; });
    SortItems();
    ExpandChosenFolders();
    Recompute();
    SetStatus(std::to_wstring(sink.ok) + L" renamed, " + std::to_wstring(failed) + L" not" +
              (aborted ? L" (stopped)" : L""));
}

// ---- window --------------------------------------------------------------------------------

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, int anchor = 0,
         DWORD ex = 0, bool bold = false) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, U.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hinst, nullptr);
    U.ctls.push_back({c, x, y, w, h, anchor, bold});
    return c;
}
void Label(const wchar_t* text, int y) { Add(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, IDC_STATIC, kM, y + 4, kCX - kM - 6, 18); }
HWND Check(const wchar_t* text, int id, int x, int y, int w) {
    return Add(WC_BUTTONW, text, BS_AUTOCHECKBOX | WS_TABSTOP, id, x, y + 2, w, 20);
}
HWND Combo(int id, int x, int y, int w, std::initializer_list<const wchar_t*> items) {
    HWND c = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, id, x, y, w, 220);
    for (const wchar_t* s : items) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    return c;
}

void Build() {
    int y = 14;
    Label(L"Search for", y);
    HWND s = Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_SEARCH, kCX, y, kM, 24, kStretchW, WS_EX_CLIENTEDGE);
    SendMessageW(s, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"empty = the whole name"));
    y += 34;
    Label(L"Replace with", y);
    HWND r = Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP, IDC_REPLACE, kCX, y, kM + 96, 24, kStretchW,
                 WS_EX_CLIENTEDGE);
    SendMessageW(r, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"new text (tokens: counter, date …)"));
    Add(WC_BUTTONW, L"Insert ▾", BS_PUSHBUTTON | WS_TABSTOP, IDC_TOKENS, kM + 90, y - 1, 90, 26, kRight);
    y += 34;
    Check(L"Regular expression", IDC_REGEX, kCX, y, 150);
    Check(L"Every match", IDC_ALL, kCX + 160, y, 110);
    Check(L"Match case", IDC_MATCHCASE, kCX + 280, y, 110);
    y += 30;
    Label(L"Change", y);
    Combo(IDC_APPLY, kCX, y, 180, {L"Name only", L"Extension only", L"Name and extension"});
    Add(WC_STATICW, L"Text case", SS_RIGHT | SS_NOPREFIX, IDC_STATIC, kCX + 190, y + 4, 70, 18);
    Combo(IDC_CASE, kCX + 268, y, 170, {L"Unchanged", L"lowercase", L"UPPERCASE", L"Title Case", L"Sentence case"});
    y += 34;
    Label(L"Include", y);
    Check(L"Files", IDC_FILES, kCX, y, 70);
    Check(L"Folders", IDC_FOLDERS, kCX + 80, y, 80);
    Check(L"Everything inside the folders", IDC_SUB, kCX + 170, y, 240);
    y += 34;
    HWND list = Add(WC_LISTVIEWW, L"", LVS_REPORT | LVS_OWNERDATA | LVS_SHOWSELALWAYS | WS_TABSTOP, IDC_LIST, kM, y,
                    kM, kFooter + 12, kStretchW | kStretchH, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const wchar_t* cols[] = {L"Name", L"New name", L"Problem"};
    for (int i = 0; i < 3; ++i) {
        LVCOLUMNW c{LVCF_TEXT | LVCF_WIDTH};
        c.pszText = const_cast<LPWSTR>(cols[i]);
        c.cx = 100;
        ListView_InsertColumn(list, i, &c);
    }
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_STATUS, kM, 36, kM + 2 * 90 + 16, 18,
        kBottom | kStretchW);
    Add(WC_BUTTONW, L"Rename", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, kM + 2 * 90 + 8, 42, 90, 28, kRight | kBottom);
    Add(WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, kM + 90, 42, 90, 28, kRight | kBottom);
}

void MakeFonts() {
    if (U.font) DeleteObject(U.font);
    if (U.bold) DeleteObject(U.bold);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, U.dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    U.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    U.bold = CreateFontIndirectW(&lf);
}

void Layout() {
    RECT rc;
    GetClientRect(U.hwnd, &rc);
    const int cw = rc.right, ch = rc.bottom;
    HDWP d = BeginDeferWindowPos(static_cast<int>(U.ctls.size()));
    for (const Ctl& c : U.ctls) {
        const int x = c.anchor & kRight ? cw - Px(c.x) : Px(c.x);
        const int y = c.anchor & kBottom ? ch - Px(c.y) : Px(c.y);
        const int w = c.anchor & kStretchW ? std::max(Px(20), cw - x - Px(c.w)) : Px(c.w);
        const int h = c.anchor & kStretchH ? std::max(Px(40), ch - y - Px(c.h)) : Px(c.h);
        if (d) d = DeferWindowPos(d, c.wnd, nullptr, x, y, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (d) EndDeferWindowPos(d);
    HWND list = Ctrl(IDC_LIST);
    RECT lr;
    GetClientRect(list, &lr);
    const int lw = lr.right - GetSystemMetricsForDpi(SM_CXVSCROLL, U.dpi);
    ListView_SetColumnWidth(list, 0, lw * 38 / 100);
    ListView_SetColumnWidth(list, 1, lw * 38 / 100);
    ListView_SetColumnWidth(list, 2, lw - 2 * (lw * 38 / 100));
    InvalidateRect(U.hwnd, nullptr, TRUE);
}

void ApplyFonts() {
    for (const Ctl& c : U.ctls)
        SendMessageW(c.wnd, WM_SETFONT, reinterpret_cast<WPARAM>(c.bold ? U.bold : U.font), FALSE);
}

void Populate() {
    auto unquote = [](std::wstring s) {
        if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
        return s;
    };
    SetWindowTextW(Ctrl(IDC_SEARCH), unquote(IniStr(L"Search", L"")).c_str());
    SetWindowTextW(Ctrl(IDC_REPLACE), unquote(IniStr(L"Replace", L"")).c_str());
    SetCheck(IDC_REGEX, IniInt(L"Regex", 0) != 0);
    SetCheck(IDC_ALL, IniInt(L"AllMatches", 1) != 0);
    SetCheck(IDC_MATCHCASE, IniInt(L"MatchCase", 0) != 0);
    SendMessageW(Ctrl(IDC_APPLY), CB_SETCURSEL, std::clamp(IniInt(L"ApplyTo", 0), 0, 2), 0);
    SendMessageW(Ctrl(IDC_CASE), CB_SETCURSEL, std::clamp(IniInt(L"TextCase", 0), 0, 4), 0);
    SetCheck(IDC_FILES, IniInt(L"Files", 1) != 0);
    SetCheck(IDC_FOLDERS, IniInt(L"Folders", 1) != 0);
    SetCheck(IDC_SUB, IniInt(L"Subfolders", 0) != 0);
}

void TokensMenu() {
    HMENU m = CreatePopupMenu();
    for (UINT i = 0; i < std::size(kTokens); ++i) {
        if (kTokens[i].label) AppendMenuW(m, MF_STRING, ID_TOKEN0 + i, kTokens[i].label);
        else AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    RECT r;
    GetWindowRect(Ctrl(IDC_TOKENS), &r);
    const UINT id = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTALIGN | TPM_TOPALIGN, r.right, r.bottom, U.hwnd, nullptr);
    DestroyMenu(m);
    if (id < ID_TOKEN0 || id >= ID_TOKEN0 + std::size(kTokens)) return;
    const Token& t = kTokens[id - ID_TOKEN0];
    if (t.text[1] == L'&' || iswdigit(t.text[1])) SetCheck(IDC_REGEX, true);
    HWND e = Ctrl(IDC_REPLACE);
    SetFocus(e);
    SendMessageW(e, EM_REPLACESEL, TRUE, reinterpret_cast<LPARAM>(t.text));
    Recompute();
}

LRESULT OnNotify(NMHDR* nm) {
    if (nm->idFrom != IDC_LIST) return 0;
    if (nm->code == LVN_GETDISPINFOW) {
        auto* di = reinterpret_cast<NMLVDISPINFOW*>(nm);
        if (!(di->item.mask & LVIF_TEXT) || di->item.iItem < 0 || di->item.iItem >= static_cast<int>(U.rows.size()))
            return 0;
        const Row& r = U.rows[di->item.iItem];
        const Item& it = U.items[r.item];
        std::wstring t;
        if (di->item.iSubItem == 0) t = it.isDir ? it.shown + L"\\" : it.shown;
        else if (di->item.iSubItem == 1) t = r.changes ? r.newName : L"";
        else if (r.problem) t = r.problem;
        wcsncpy_s(di->item.pszText, di->item.cchTextMax, t.c_str(), _TRUNCATE);
        return 0;
    }
    if (nm->code == NM_CUSTOMDRAW) {
        auto* cd = reinterpret_cast<NMLVCUSTOMDRAW*>(nm);
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT: return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: return CDRF_NOTIFYSUBITEMDRAW;
        case CDDS_ITEMPREPAINT | CDDS_SUBITEM: {
            const size_t i = cd->nmcd.dwItemSpec;
            if (i < U.rows.size()) {
                const Row& r = U.rows[i];
                cd->clrText = r.problem ? kRed : !r.changes ? kGrey : GetSysColor(COLOR_WINDOWTEXT);
                SelectObject(cd->nmcd.hdc, cd->iSubItem == 1 && r.changes && !r.problem ? U.bold : U.font);
                return CDRF_NEWFONT;
            }
            return CDRF_DODEFAULT;
        }
        }
        return CDRF_DODEFAULT;
    }
    if (nm->code == LVN_KEYDOWN && reinterpret_cast<NMLVKEYDOWN*>(nm)->wVKey == VK_DELETE) RemoveSelected();
    return 0;
}

LRESULT CALLBACK RenamerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) U.hwnd = h;
    if (U.hwnd != h) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        U.dpi = GetDpiForWindow(h);
        MakeFonts();
        Build();
        ApplyFonts();
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
        RECT f = rc;
        f.top = rc.bottom - Px(kFooter);
        FillRect(dc, &f, GetSysColorBrush(COLOR_BTNFACE));
        f.bottom = f.top + 1;
        SetDCBrushColor(dc, RGB(220, 220, 220));
        FillRect(dc, &f, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const bool footer = GetDlgCtrlID(reinterpret_cast<HWND>(lp)) == IDC_STATUS;
        SetBkColor(dc, GetSysColor(footer ? COLOR_BTNFACE : COLOR_WINDOW));
        SetTextColor(dc, footer ? (U.badRegex ? kRed : kGrey) : GetSysColor(COLOR_WINDOWTEXT));
        return reinterpret_cast<LRESULT>(GetSysColorBrush(footer ? COLOR_BTNFACE : COLOR_WINDOW));
    }
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_BTNFACE));
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_NOTIFY:
        return OnNotify(reinterpret_cast<NMHDR*>(lp));
    case WM_TIMER:
        if (wp == kTimerPreview) Recompute();
        return 0;
    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wp);
        std::vector<std::wstring> paths;
        const UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            std::wstring p(DragQueryFileW(drop, i, nullptr, 0), L'\0');
            DragQueryFileW(drop, i, p.data(), static_cast<UINT>(p.size()) + 1);
            paths.push_back(p);
        }
        DragFinish(drop);
        AddPaths(paths);
        Recompute();
        return 0;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDOK:
            DoRename();
            break;
        case IDCANCEL:
            DestroyWindow(h);
            break;
        case IDC_TOKENS:
            TokensMenu();
            break;
        case IDC_SEARCH:
        case IDC_REPLACE:
            if (code == EN_CHANGE) SetTimer(h, kTimerPreview, 120, nullptr);
            break;
        case IDC_SUB:
            ExpandChosenFolders();
            Recompute();
            break;
        case IDC_REGEX:
        case IDC_ALL:
        case IDC_MATCHCASE:
        case IDC_FILES:
        case IDC_FOLDERS:
            Recompute();
            break;
        case IDC_APPLY:
        case IDC_CASE:
            if (code == CBN_SELCHANGE) Recompute();
            break;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        U.dpi = HIWORD(wp);
        MakeFonts();
        ApplyFonts();
        const RECT* s = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, s->left, s->top, s->right - s->left, s->bottom - s->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_DESTROY:
        U.hwnd = nullptr;
        U.ctls.clear();
        U.items.clear();
        U.rows.clear();
        return 0;
    case WM_NCDESTROY:
        DeleteObject(U.font);
        DeleteObject(U.bold);
        U.font = U.bold = nullptr;
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

HWND Renamer_Window() { return U.hwnd; }

void Renamer_Open(const std::vector<std::wstring>& paths) {
    if (U.hwnd) {
        AddPaths(paths);
        Recompute();
        if (IsIconic(U.hwnd)) ShowWindow(U.hwnd, SW_RESTORE);
        SetForegroundWindow(U.hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = RenamerProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
        registered = RegisterClassExW(&wc) != 0;
    }
    POINT p;
    GetCursorPos(&p);
    HMONITOR mon = MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    HWND h = CreateWindowExW(kExStyle, kClass, kTitle, kStyle, mi.rcWork.left, mi.rcWork.top, 100, 100, nullptr,
                             nullptr, g_hinst, nullptr);
    if (!h) return;
    RECT r{0, 0, Px(kW), Px(kH)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, U.dpi);
    const RECT& w = mi.rcWork;
    const int ww = std::min<int>(r.right - r.left, w.right - w.left), wh = std::min<int>(r.bottom - r.top, w.bottom - w.top);
    SetWindowPos(h, nullptr, w.left + (w.right - w.left - ww) / 2, w.top + (w.bottom - w.top - wh) / 2, ww, wh,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    Layout();
    Populate();
    AddPaths(paths);
    Recompute();
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    HWND search = Ctrl(IDC_SEARCH);
    SetFocus(search);
    SendMessageW(search, EM_SETSEL, 0, -1);
}
