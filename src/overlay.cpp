// "Show keys and clicks" for GIF and video recordings: mouse clicks become a short expanding ring at
// the pointer (left: one amber ring, right: two blue rings) and key presses a dark rounded pill with
// white text near the bottom of the area ("Ctrl+B", "Tab", repeats as "×3"), fading after 1.5 s.
// Both are drawn into the recorded frames themselves (capture thread), never as a window.
// Privacy: by default only shortcuts (with Ctrl, Alt or Win) and special keys (Tab, Enter, Esc,
// arrows, F-keys, Del...) are shown, never typed letters; "Show all keys" shows everything.
// Input comes from low-level keyboard and mouse hooks on the main thread, only while it is on.
#include "app.h"

#include <objidl.h>  // GDI+ needs IStream, which WIN32_LEAN_AND_MEAN leaves out

#include <algorithm>
namespace Gdiplus {
using std::max;
using std::min;
}  // namespace Gdiplus
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")

#include <bitset>
#include <cmath>
#include <cwctype>
#include <mutex>

namespace {

constexpr double kRingMs = 450;   // a click's ring grows and fades in this time
constexpr double kPillMs = 1500;  // a key pill stays this long...
constexpr double kFadeMs = 400;   // ...the last part fading out
constexpr size_t kMaxPills = 4;

double NowMs() {  // the recorder's clock (QPC)
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

struct Pill {
    std::wstring label;
    int count = 1;
    double t = 0;  // last pressed
};
struct Ring {
    float x = 0, y = 0;  // in the area
    bool right = false;
    double t = 0;
};

struct State {
    std::mutex m;
    RECT area{};
    double scale = 1;
    bool active = false, on = false, paused = false, allKeys = false;
    HHOOK kb = nullptr, mouse = nullptr;
    std::bitset<256> down;
    std::vector<Pill> pills;
    std::vector<Ring> rings;
    std::vector<Hotkey> ignore;
} O;

ULONG_PTR g_gdip = 0;

bool IsModifier(UINT vk) {
    switch (vk) {
    case VK_SHIFT: case VK_LSHIFT: case VK_RSHIFT:
    case VK_CONTROL: case VK_LCONTROL: case VK_RCONTROL:
    case VK_MENU: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN:
        return true;
    }
    return false;
}

// Keys that never type text: shown even without Ctrl, Alt or Win.
bool IsSpecial(UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return true;
    if (vk >= VK_VOLUME_MUTE && vk <= VK_MEDIA_PLAY_PAUSE) return true;
    switch (vk) {
    case VK_TAB: case VK_RETURN: case VK_ESCAPE: case VK_BACK: case VK_DELETE: case VK_INSERT:
    case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_SNAPSHOT: case VK_PAUSE: case VK_APPS:
        return true;
    }
    return false;
}

std::wstring KeyName(UINT vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num " + std::to_wstring(vk - VK_NUMPAD0);
    switch (vk) {
    case VK_TAB: return L"Tab";
    case VK_RETURN: return L"Enter";
    case VK_ESCAPE: return L"Esc";
    case VK_BACK: return L"Backspace";
    case VK_DELETE: return L"Del";
    case VK_INSERT: return L"Ins";
    case VK_HOME: return L"Home";
    case VK_END: return L"End";
    case VK_PRIOR: return L"PgUp";
    case VK_NEXT: return L"PgDn";
    case VK_LEFT: return L"←";
    case VK_RIGHT: return L"→";
    case VK_UP: return L"↑";
    case VK_DOWN: return L"↓";
    case VK_SPACE: return L"Space";
    case VK_SNAPSHOT: return L"PrtSc";
    case VK_PAUSE: return L"Pause";
    case VK_APPS: return L"Menu";
    case VK_MULTIPLY: return L"Num *";
    case VK_ADD: return L"Num +";
    case VK_SUBTRACT: return L"Num −";
    case VK_DECIMAL: return L"Num .";
    case VK_DIVIDE: return L"Num /";
    case VK_VOLUME_MUTE: return L"Mute";
    case VK_VOLUME_DOWN: return L"Volume −";
    case VK_VOLUME_UP: return L"Volume +";
    case VK_MEDIA_NEXT_TRACK: return L"Next track";
    case VK_MEDIA_PREV_TRACK: return L"Previous track";
    case VK_MEDIA_STOP: return L"Stop";
    case VK_MEDIA_PLAY_PAUSE: return L"Play/Pause";
    case VK_CAPITAL: case VK_NUMLOCK: case VK_SCROLL: return L"";
    }
    // Punctuation: what the key types on the keyboard layout of the app in front.
    DWORD tid = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    const UINT ch = MapVirtualKeyExW(vk, MAPVK_VK_TO_CHAR, GetKeyboardLayout(tid)) & 0x7FFF;
    if (ch > L' ') return std::wstring(1, static_cast<wchar_t>(towupper(static_cast<wint_t>(ch))));
    return L"";
}

UINT ModsNow() {  // under O.m
    const auto& d = O.down;
    const bool altGr = d[VK_RMENU];  // AltGr types characters (@, €...): not a shortcut
    UINT mods = 0;
    if (d[VK_RCONTROL] || (!altGr && (d[VK_LCONTROL] || d[VK_CONTROL]))) mods |= MOD_CONTROL;
    if (d[VK_LMENU] || (!altGr && d[VK_MENU])) mods |= MOD_ALT;
    if (d[VK_LSHIFT] || d[VK_RSHIFT] || d[VK_SHIFT]) mods |= MOD_SHIFT;
    if (d[VK_LWIN] || d[VK_RWIN]) mods |= MOD_WIN;
    return mods;
}

LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        Overlay_Key(k->vkCode & 0xFF, wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION && (wp == WM_LBUTTONDOWN || wp == WM_RBUTTONDOWN)) {
        const auto* m = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        Overlay_Click(m->pt, wp == WM_RBUTTONDOWN);
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Unhook() {
    if (O.kb) UnhookWindowsHookEx(O.kb);
    if (O.mouse) UnhookWindowsHookEx(O.mouse);
    O.kb = O.mouse = nullptr;
}

Gdiplus::GraphicsPath* PillPath(Gdiplus::GraphicsPath& p, float x, float y, float w, float h) {
    const float d = std::min(w, h);
    p.AddArc(x, y, d, d, 90, 180);
    p.AddArc(x + w - d, y, d, d, 270, 180);
    p.CloseFigure();
    return &p;
}

BYTE A(double a) { return static_cast<BYTE>(std::clamp(a, 0.0, 1.0) * 255 + 0.5); }

}  // namespace

std::wstring Overlay_KeyLabel(UINT vk, UINT mods, bool allKeys) {
    if (IsModifier(vk)) return L"";
    const std::wstring name = KeyName(vk);
    if (name.empty()) return L"";
    const bool strong = (mods & (MOD_CONTROL | MOD_ALT | MOD_WIN)) != 0;
    if (!allKeys && !strong && !IsSpecial(vk)) return L"";  // typing: never shown unless asked
    std::wstring t;
    if (mods & MOD_CONTROL) t += L"Ctrl+";
    if (mods & MOD_ALT) t += L"Alt+";
    if (mods & MOD_SHIFT) t += L"Shift+";
    if (mods & MOD_WIN) t += L"Win+";
    return t + name;
}

void Overlay_Begin(const RECT& area, bool allKeys, UINT dpi) {
    if (!g_gdip) {
        Gdiplus::GdiplusStartupInput in;
        Gdiplus::GdiplusStartup(&g_gdip, &in, nullptr);
    }
    Overlay_End();
    std::lock_guard<std::mutex> lock(O.m);
    O.area = area;
    O.allKeys = allKeys;
    O.scale = dpi / 96.0;
    O.active = true;
    O.paused = false;
    O.ignore.clear();
}

void Overlay_SetOn(bool on) {
    {
        std::lock_guard<std::mutex> lock(O.m);
        if (!O.active) on = false;
        O.on = on;
        O.pills.clear();
        O.rings.clear();
        O.down.reset();
        if (on)  // keys already held when it starts (Ctrl of a shortcut being pressed)
            for (UINT vk : {VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU, VK_LSHIFT, VK_RSHIFT, VK_LWIN, VK_RWIN})
                if (GetAsyncKeyState(static_cast<int>(vk)) < 0) O.down[vk] = true;
    }
    // Tests (on a hidden desktop) feed input by hand: a hook there must never see the real keyboard.
    if (on && !TestMode()) {
        if (!O.kb) O.kb = SetWindowsHookExW(WH_KEYBOARD_LL, KeyboardProc, g_hinst, 0);
        if (!O.mouse) O.mouse = SetWindowsHookExW(WH_MOUSE_LL, MouseProc, g_hinst, 0);
        if (!O.kb || !O.mouse) Log(L"Show keys and clicks: input hook failed (%lu)", GetLastError());
    } else {
        Unhook();
    }
}

bool Overlay_On() {
    std::lock_guard<std::mutex> lock(O.m);
    return O.on;
}

void Overlay_Ignore(const Hotkey& hk) {
    if (!hk.vk) return;
    std::lock_guard<std::mutex> lock(O.m);
    O.ignore.push_back(hk);
}

void Overlay_Paused(bool paused) {
    std::lock_guard<std::mutex> lock(O.m);
    O.paused = paused;
    O.pills.clear();  // nothing from before the pause shows up after it
    O.rings.clear();
}

void Overlay_End() {
    Unhook();
    std::lock_guard<std::mutex> lock(O.m);
    O.active = O.on = O.paused = false;
    O.pills.clear();
    O.rings.clear();
    O.down.reset();
}

void Overlay_Key(UINT vk, bool down) {
    std::lock_guard<std::mutex> lock(O.m);
    if (vk >= 256) return;
    const bool was = O.down[vk];
    O.down[vk] = down;
    if (!down || !O.active || !O.on || O.paused || IsModifier(vk)) return;
    const UINT mods = ModsNow();
    for (const Hotkey& hk : O.ignore)
        if (hk.vk == vk && (hk.mods & ~MOD_NOREPEAT) == mods) return;
    const std::wstring label = Overlay_KeyLabel(vk, mods, O.allKeys);
    if (label.empty()) return;
    const double now = NowMs();
    if (!O.pills.empty() && O.pills.back().label == label && now - O.pills.back().t < kPillMs) {
        if (!was) ++O.pills.back().count;  // held down (auto-repeat) just keeps it showing
        if (TestMode() && !was) Log(L"Overlay key: %s x%d", label.c_str(), O.pills.back().count);
        O.pills.back().t = now;
        return;
    }
    if (TestMode()) Log(L"Overlay key: %s", label.c_str());
    O.pills.push_back({label, 1, now});
    if (O.pills.size() > kMaxPills) O.pills.erase(O.pills.begin());
}

void Overlay_Click(POINT pt, bool right) {
    std::lock_guard<std::mutex> lock(O.m);
    if (!O.active || !O.on || O.paused) return;
    RECT zone = O.area;
    InflateRect(&zone, 40, 40);
    if (!PtInRect(&zone, pt)) return;
    if (TestMode()) Log(L"Overlay click: %s at %ld,%ld", right ? L"right" : L"left", pt.x - O.area.left, pt.y - O.area.top);
    O.rings.push_back({static_cast<float>(pt.x - O.area.left), static_cast<float>(pt.y - O.area.top), right, NowMs()});
    if (O.rings.size() > 8) O.rings.erase(O.rings.begin());
}

void Overlay_Draw(uint32_t* px, int w, int h) {
    std::vector<Pill> pills;
    std::vector<Ring> rings;
    double s;
    const double now = NowMs();
    {
        std::lock_guard<std::mutex> lock(O.m);
        if (!O.active || !O.on) return;
        std::erase_if(O.pills, [&](const Pill& p) { return now - p.t >= kPillMs; });
        std::erase_if(O.rings, [&](const Ring& r) { return now - r.t >= kRingMs; });
        if (O.pills.empty() && O.rings.empty()) return;
        pills = O.pills;
        rings = O.rings;
        s = O.scale;
    }
    Gdiplus::Bitmap bmp(w, h, w * 4, PixelFormat32bppRGB, reinterpret_cast<BYTE*>(px));
    Gdiplus::Graphics g(&bmp);
    g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
    g.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
    g.SetTextRenderingHint(Gdiplus::TextRenderingHintAntiAliasGridFit);
    const float k = static_cast<float>(s);

    // Clicks: a ring that grows and fades; a dark ring under it keeps it visible on light backgrounds.
    for (const Ring& r : rings) {
        const double f = std::clamp((now - r.t) / kRingMs, 0.0, 1.0);
        const double grow = 1 - (1 - f) * (1 - f) * (1 - f);
        const float rad = k * static_cast<float>(7 + 21 * grow);
        const double a = 1 - f * f;
        const Gdiplus::Color col = r.right ? Gdiplus::Color(A(a), 40, 160, 255) : Gdiplus::Color(A(a), 255, 190, 20);
        Gdiplus::Pen under(Gdiplus::Color(A(a * 0.5), 0, 0, 0), k * 5.0f);
        Gdiplus::Pen ring(col, k * 2.5f);
        auto circle = [&](Gdiplus::Pen& p, float rr) { g.DrawEllipse(&p, r.x - rr, r.y - rr, 2 * rr, 2 * rr); };
        circle(under, rad);
        circle(ring, rad);
        if (r.right) {  // right click: a second ring inside
            circle(under, rad * 0.55f);
            circle(ring, rad * 0.55f);
        }
        if (f < 0.35) {  // the press itself: a dot at the pointer
            Gdiplus::SolidBrush dot(Gdiplus::Color(A((0.35 - f) / 0.35 * 0.9), col.GetR(), col.GetG(), col.GetB()));
            const float d = k * 5;
            g.FillEllipse(&dot, r.x - d, r.y - d, 2 * d, 2 * d);
        }
    }

    // Keys: pills in a row, centered near the bottom, newest on the right.
    if (!pills.empty()) {
        const float fs = std::clamp(k * 15.0f, 11.0f, std::max(11.0f, h / 7.0f));
        Gdiplus::FontFamily family(L"Segoe UI");
        Gdiplus::Font font(&family, fs, Gdiplus::FontStyleBold, Gdiplus::UnitPixel);
        Gdiplus::StringFormat fmt(Gdiplus::StringFormat::GenericTypographic());
        fmt.SetFormatFlags(fmt.GetFormatFlags() | Gdiplus::StringFormatFlagsNoWrap);
        const float ph = fs * 2.0f, pad = fs * 0.9f, gap = fs * 0.5f;
        std::vector<std::wstring> texts;
        std::vector<float> widths;
        for (const Pill& p : pills) {
            std::wstring t = p.label;
            if (p.count > 1) t += L"  ×" + std::to_wstring(p.count);
            Gdiplus::RectF box;
            g.MeasureString(t.c_str(), -1, &font, Gdiplus::PointF(0, 0), &fmt, &box);
            texts.push_back(t);
            widths.push_back(box.Width + 2 * pad);
        }
        // Drop the oldest ones that don't fit.
        size_t first = 0;
        auto total = [&] {
            float sum = 0;
            for (size_t i = first; i < widths.size(); ++i) sum += widths[i] + (i > first ? gap : 0);
            return sum;
        };
        while (first + 1 < widths.size() && total() > w - 2 * gap) ++first;
        float x = (w - total()) / 2;
        const float y = std::max(2.0f, h - ph - std::max(k * 14.0f, h * 0.05f));
        for (size_t i = first; i < pills.size(); ++i) {
            const double age = now - pills[i].t;
            const double a = age < kPillMs - kFadeMs ? 1.0 : (kPillMs - age) / kFadeMs;
            Gdiplus::GraphicsPath path;
            PillPath(path, x, y, widths[i], ph);
            Gdiplus::SolidBrush fill(Gdiplus::Color(A(a * 0.78), 22, 22, 24));
            g.FillPath(&fill, &path);
            Gdiplus::Pen edge(Gdiplus::Color(A(a * 0.30), 255, 255, 255), std::max(1.0f, k));
            g.DrawPath(&edge, &path);
            Gdiplus::SolidBrush text(Gdiplus::Color(A(a), 255, 255, 255));
            Gdiplus::RectF box;
            g.MeasureString(texts[i].c_str(), -1, &font, Gdiplus::PointF(0, 0), &fmt, &box);
            g.DrawString(texts[i].c_str(), -1, &font,
                         Gdiplus::PointF(x + (widths[i] - box.Width) / 2, y + (ph - box.Height) / 2), &fmt, &text);
            x += widths[i] + gap;
        }
    }
    g.Flush(Gdiplus::FlushIntentionSync);
}
