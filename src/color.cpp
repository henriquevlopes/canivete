// Color picker results: text formats, clipboard, recent colors (kept in %APPDATA%\Canivete\colors.txt).
#include "app.h"

#include <algorithm>
#include <cmath>

namespace {

std::vector<COLORREF> g_recent;
bool g_loaded = false;
constexpr size_t kMaxRecent = 10;

std::wstring RecentPath() { return AppDataDir() + L"\\colors.txt"; }

void LoadRecent() {
    if (g_loaded) return;
    g_loaded = true;
    HANDLE f = CreateFileW(RecentPath().c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    char buf[512] = {};
    DWORD n = 0;
    ReadFile(f, buf, sizeof(buf) - 1, &n, nullptr);
    CloseHandle(f);
    for (const char* p = buf; *p && g_recent.size() < kMaxRecent;) {
        unsigned v = 0;
        int used = 0;
        if (sscanf_s(p, "%6x%n", &v, &used) == 1 && used == 6)
            g_recent.push_back(RGB((v >> 16) & 255, (v >> 8) & 255, v & 255));
        while (*p && *p != '\n') ++p;
        if (*p) ++p;
    }
}

void SaveRecent() {
    std::string text;
    for (COLORREF c : g_recent) {
        char line[16];
        sprintf_s(line, "%02X%02X%02X\r\n", GetRValue(c), GetGValue(c), GetBValue(c));
        text += line;
    }
    HANDLE f = CreateFileW(RecentPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD n = 0;
    WriteFile(f, text.data(), static_cast<DWORD>(text.size()), &n, nullptr);
    CloseHandle(f);
}

std::wstring Num(double v) {
    wchar_t b[32];
    swprintf_s(b, L"%.3f", v);
    return b;
}

bool CopyText(const std::wstring& t) {
    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(g_mainWnd)); ++i) Sleep(15);
    if (!open) return false;
    EmptyClipboard();
    bool ok = false;
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (t.size() + 1) * sizeof(wchar_t))) {
        memcpy(GlobalLock(g), t.c_str(), (t.size() + 1) * sizeof(wchar_t));
        GlobalUnlock(g);
        ok = SetClipboardData(CF_UNICODETEXT, g) != nullptr;
        if (!ok) GlobalFree(g);
    }
    CloseClipboard();
    return ok;
}

}  // namespace

std::wstring FormatColor(COLORREF c, const std::wstring& key) {
    const int r = GetRValue(c), g = GetGValue(c), b = GetBValue(c);
    wchar_t buf[96];
    if (key == L"hex") {
        swprintf_s(buf, L"%02x%02x%02x", r, g, b);
    } else if (key == L"rgb") {
        swprintf_s(buf, L"rgb(%d, %d, %d)", r, g, b);
    } else if (key == L"hsl") {
        const double R = r / 255.0, G = g / 255.0, B = b / 255.0;
        const double mx = std::max({R, G, B}), mn = std::min({R, G, B}), d = mx - mn;
        const double l = (mx + mn) / 2;
        const double s = d == 0 ? 0 : d / (1 - std::fabs(2 * l - 1));
        double h = 0;
        if (d != 0) {
            if (mx == R) h = 60 * std::fmod((G - B) / d, 6.0);
            else if (mx == G) h = 60 * ((B - R) / d + 2);
            else h = 60 * ((R - G) / d + 4);
            if (h < 0) h += 360;
        }
        swprintf_s(buf, L"hsl(%d, %d%%, %d%%)", static_cast<int>(std::lround(h)) % 360,
                   static_cast<int>(std::lround(s * 100)), static_cast<int>(std::lround(l * 100)));
    } else {  // gray: brightness 0-1 (equals each channel for a gray pixel)
        return Num((0.2126 * r + 0.7152 * g + 0.0722 * b) / 255.0);
    }
    return buf;
}

// Menu of every format with this color's value; the one chosen in Settings is first and bold, so
// Enter takes it. Returns the format key, or "" when the menu was dismissed.
std::wstring AskFormat(COLORREF c, POINT at) {
    HMENU m = CreatePopupMenu();
    std::vector<int> order;
    for (int i = 0; i < static_cast<int>(std::size(kColorFormats)); ++i)
        if (g_settings.colorFormat == kColorFormats[i].key) order.insert(order.begin(), i);
        else order.push_back(i);
    for (int i : order) {
        std::wstring label = kColorFormats[i].label;
        label = label.substr(0, label.find(L"   "));  // "Gray 0-1", "HEX", ...
        const std::wstring item = label + L"\t" + FormatColor(c, kColorFormats[i].key);
        AppendMenuW(m, MF_STRING, 1 + i, item.c_str());
    }
    SetMenuDefaultItem(m, 1 + order.front(), FALSE);
    SetForegroundWindow(g_mainWnd);
    const int cmd = TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, at.x, at.y, g_mainWnd, nullptr);
    PostMessageW(g_mainWnd, WM_NULL, 0, 0);
    DestroyMenu(m);
    return cmd > 0 ? kColorFormats[cmd - 1].key : L"";
}

void Color_Picked(COLORREF c, POINT at) {
    LoadRecent();
    const std::wstring format = AskFormat(c, at);
    if (format.empty()) return;
    const std::wstring text = FormatColor(c, format);
    const bool ok = CopyText(text);
    g_recent.erase(std::remove(g_recent.begin(), g_recent.end(), c), g_recent.end());
    g_recent.insert(g_recent.begin(), c);
    if (g_recent.size() > kMaxRecent) g_recent.resize(kMaxRecent);
    SaveRecent();
    wchar_t second[64];
    swprintf_s(second, L"#%02X%02X%02X    rgb(%d, %d, %d)", GetRValue(c), GetGValue(c), GetBValue(c), GetRValue(c),
               GetGValue(c), GetBValue(c));
    Output_ShowColor(c, at, (ok ? L"Copied   " : L"Clipboard busy, not copied:  ") + text, second);
    Log(L"Color picked %s -> %s", second, text.c_str());
}

std::vector<COLORREF> RecentColors() {
    LoadRecent();
    return g_recent;
}
