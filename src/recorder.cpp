// Screen recording, GIF and MP4: a red border around the area, a small bar with the time and
// Stop / Cancel, and the capture thread. GIF: 25 frames a second, only changed tiles go to disk, then
// a save window offers three ready-made GIFs with their real file sizes. MP4: 30 frames a second
// straight into the H.264 encoder (video.cpp); Stop saves it to the screenshots folder at once.
// The border, bar and save window are hidden from screen capture, so they never end up in a recording.
// Pause (bar button or hotkey): the capture clock stops, so the paused time is simply not in the
// recording, and the sound (audio.cpp) leaves the same span out. "Show keys and clicks" (overlay.cpp)
// is drawn into each frame. The save window has a timeline (trim.cpp) to cut off the start or the end.
#include "app.h"
#include "theme.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cmath>
#include <thread>

namespace {

constexpr wchar_t kBarClass[] = L"CaniveteGifBar";
constexpr wchar_t kBorderClass[] = L"CaniveteGifBorder";
constexpr wchar_t kSaveClass[] = L"CaniveteGifSave";
constexpr UINT WM_REC_DONE = WM_APP + 40;     // capture thread finished; wParam 1 = hit the size limit
constexpr UINT WM_GIF_ENCODED = WM_APP + 41;  // wParam = preset index
constexpr int kFrameMs = 40;                  // 25 frames a second, the most any preset uses
constexpr int kVideoFps = 30;
constexpr uint64_t kMaxBytes = 4ull << 30;    // the recording stops itself at 4 GB of changes
constexpr COLORREF kRed = RGB(232, 44, 56);
constexpr DWORD kExcludeFromCapture = 0x11;   // WDA_EXCLUDEFROMCAPTURE
enum {
    IDC_STOP = 101,
    IDC_CANCELREC = 102,
    IDC_PAUSE = 103,
    IDC_KEYS = 104,
    IDC_PRESET0 = 110,
    IDC_DISCARD = 120,
    IDC_INFO = 121,
    IDC_UPLOADTOO = 122,
    IDC_TRIM = 123,
};
constexpr UINT_PTR kTimerTick = 1, kTimerTestStop = 2;
constexpr int kPauseHotkey = 1;  // the bar's own hotkey id (pause / resume), only while recording
constexpr int kThumbMax = 200;   // timeline pictures kept; past that, every other one goes

double NowMs() {
    static const double freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart) / 1000.0;
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return static_cast<double>(c.QuadPart) / freq;
}

struct Recording {
    RECT rect{};
    std::wstring file;  // the recording (.cgf, or the .mp4 being written)
    GifWriter writer;
    bool video = false;
    Mp4Writer mp4;
    std::wstring error;  // video: what went wrong
    std::wstring soundInfo;  // video: what sound was recorded, for the log
    uint64_t frames = 0, dropped = 0;
    std::thread thread;
    std::atomic<bool> stop{false};
    std::atomic<bool> started{false};
    double start = 0;  // NowMs() of the first frame
    uint32_t durationMs = 0;
    bool cancelled = false;
    bool fake = false;  // test mode: draw a moving test picture instead of grabbing the screen
    CaptureResult meta;  // app in front and size, for the file name
    // Pause: the bar asks, the capture thread does it (and keeps the paused time out of the clock).
    std::atomic<bool> pauseReq{false}, paused{false};
    std::atomic<uint32_t> activeMs{0};  // recording time so far, pauses left out (the bar's clock)
    double pausedMs = 0;                // capture thread: total time paused
    // Timeline pictures for the save window (capture thread; read once it has finished).
    std::vector<TrimThumb> thumbs;
    uint32_t thumbEvery = 100, nextThumb = 0;
    // Test mode hooks (environment): a pause at a recording time, and made-up keys and clicks.
    uint32_t testPauseAt = UINT32_MAX, testPauseFor = 0;
    struct TestInput {
        uint32_t ms;
        wchar_t kind;  // 'L' / 'R' click at pt, 'K' key combination
        POINT pt;
        Hotkey key;
    };
    std::vector<TestInput> testInput;
};

struct Encoded {
    std::wstring path;
    uint64_t size = 0;
    ULONGLONG ms = 0;
    bool done = false, ok = false;
};

struct State {
    std::unique_ptr<Recording> rec;
    HWND bar = nullptr, save = nullptr;
    HWND borders[4]{};
    HFONT barFont = nullptr, saveFont = nullptr, bigFont = nullptr;
    std::atomic<bool> cancelEncode{false};
    std::thread encoders[kGifPresetCount];
    Encoded enc[kGifPresetCount];
    int def = 1;  // preset Enter takes
    double scale = 1;
    HWND trim = nullptr;               // the save window's timeline
    uint32_t trimFrom = 0, trimTo = 0;  // what it keeps (ms)
    bool pauseKey = false;             // the bar's pause hotkey is registered
} R;

int Px(double v) { return static_cast<int>(std::lround(v * R.scale)); }

std::wstring SizeText(uint64_t b) {
    wchar_t s[32];
    if (b >= 1024 * 1024) swprintf_s(s, L"%.1f MB", b / (1024.0 * 1024.0));
    else swprintf_s(s, L"%llu KB", std::max<uint64_t>(1, (b + 512) / 1024));
    return s;
}

uint64_t FileSize(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa)) return 0;
    return (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
}

HFONT MessageFont(UINT dpi, double grow, bool bold) {
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    ncm.lfMessageFont.lfHeight = static_cast<LONG>(std::lround(ncm.lfMessageFont.lfHeight * grow));
    if (bold) ncm.lfMessageFont.lfWeight = FW_SEMIBOLD;
    return CreateFontIndirectW(&ncm.lfMessageFont);
}

UINT DpiAt(const RECT& r) {
    UINT dx = 96, dy = 96;
    if (FAILED(GetDpiForMonitor(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
    return dx;
}

RECT WorkAreaAt(const RECT& r) {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromRect(&r, MONITOR_DEFAULTTONEAREST), &mi);
    return mi.rcWork;
}

// Where recordings live until saved. The test copy keeps its own, so it never touches the real one's.
std::wstring TempFolder() {
    wchar_t t[MAX_PATH];
    GetTempPathW(MAX_PATH, t);
    std::wstring dir = TestMode() ? AppDataDir() + L"\\gif-temp" : std::wstring(t) + L"Canivete";
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

// Leftovers of a recording that never finished (Canivete closed or crashed meanwhile).
void DeleteLeftovers() {
    const std::wstring dir = TempFolder();
    for (const wchar_t* pattern : {L"\\gif-*", L"\\vid-*"}) {
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileW((dir + pattern).c_str(), &fd);
        if (f == INVALID_HANDLE_VALUE) continue;
        do DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        while (FindNextFileW(f, &fd));
        FindClose(f);
    }
}

void ProcessOf(HWND h, std::wstring& proc, std::wstring& title) {
    proc.clear();
    title.clear();
    if (!h) return;
    wchar_t buf[512];
    if (GetWindowTextW(h, buf, 512)) title = buf;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        DWORD n = 512;
        if (QueryFullProcessImageNameW(p, 0, buf, &n)) {
            std::wstring s = buf;
            s = s.substr(s.find_last_of(L'\\') + 1);
            if (s.size() > 4 && !_wcsicmp(s.c_str() + s.size() - 4, L".exe")) s.resize(s.size() - 4);
            proc = s;
        }
        CloseHandle(p);
    }
}

// Test picture: a still gradient, a box moving across, and a counter, so frames change a little.
// The top-right corner is white for the first 100 ms of every whole second of real time (pauses
// included) and black otherwise: the test sound beeps on the same seconds (audio.cpp).
void DrawFake(Dib& d, double t, double wall) {
    for (int y = 0; y < d.h; ++y)
        for (int x = 0; x < d.w; ++x)
            d.bits[static_cast<size_t>(y) * d.w + x] =
                (static_cast<uint32_t>(x * 255 / std::max(1, d.w - 1)) << 16) |
                (static_cast<uint32_t>(y * 255 / std::max(1, d.h - 1)) << 8) | 0x60;
    const int bx = static_cast<int>(t / 8) % std::max(1, d.w - 60);
    RECT box{bx, d.h / 3, bx + 50, d.h / 3 + 50};
    SetDCBrushColor(d.dc, RGB(255, 255, 255));
    FillRect(d.dc, &box, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    wchar_t s[32];
    swprintf_s(s, L"%.1f s", t / 1000);
    SetBkMode(d.dc, TRANSPARENT);
    SetTextColor(d.dc, RGB(0, 0, 0));
    TextOutW(d.dc, 10, 10, s, static_cast<int>(wcslen(s)));
    const int fs = std::min(32, std::min(d.w, d.h) / 4);
    RECT flash{d.w - fs, 0, d.w, fs};
    SetDCBrushColor(d.dc, std::fmod(wall, 1000.0) < 100 ? RGB(255, 255, 255) : RGB(0, 0, 0));
    FillRect(d.dc, &flash, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
}

// A small copy of a frame for the timeline (box filter, fits in 360 x 200).
Image MakeThumb(const uint32_t* px, int w, int h) {
    const double f = std::min({1.0, 360.0 / w, 200.0 / h});
    Image img;
    img.w = std::max(1, static_cast<int>(w * f));
    img.h = std::max(1, static_cast<int>(h * f));
    img.px.resize(static_cast<size_t>(img.w) * img.h);
    for (int y = 0; y < img.h; ++y) {
        const int y0 = y * h / img.h, y1 = std::max(y0 + 1, (y + 1) * h / img.h);
        for (int x = 0; x < img.w; ++x) {
            const int x0 = x * w / img.w, x1 = std::max(x0 + 1, (x + 1) * w / img.w);
            // at most 4 x 4 samples per output pixel: plenty for a preview
            const int sx = std::max(1, (x1 - x0) / 4), sy = std::max(1, (y1 - y0) / 4);
            uint32_t r = 0, g = 0, b = 0, n = 0;
            for (int yy = y0; yy < y1; yy += sy)
                for (int xx = x0; xx < x1; xx += sx) {
                    const uint32_t c = px[static_cast<size_t>(yy) * w + xx];
                    r += (c >> 16) & 255;
                    g += (c >> 8) & 255;
                    b += c & 255;
                    ++n;
                }
            img.px[static_cast<size_t>(y) * img.w + x] = (r / n) << 16 | (g / n) << 8 | (b / n);
        }
    }
    return img;
}

// Test mode: "300:L:40,40;600:K:Ctrl+B;900:R:100,60" (recording ms, then a click at a point in the
// area or a key combination) and "2000,1500" (pause at 2 s of recording, for 1.5 s).
void ReadTestHooks(Recording& rec) {
    if (!TestMode()) return;
    wchar_t buf[2048];
    if (GetEnvironmentVariableW(L"CANIVETE_REC_PAUSE", buf, 2048) > 0) {
        unsigned at = 0, len = 0;
        if (swscanf_s(buf, L"%u,%u", &at, &len) == 2) {
            rec.testPauseAt = at;
            rec.testPauseFor = len;
        }
    }
    if (GetEnvironmentVariableW(L"CANIVETE_REC_INPUT", buf, 2048) > 0) {
        std::wstring all = buf;
        size_t i = 0;
        while (i < all.size()) {
            size_t e = all.find(L';', i);
            if (e == std::wstring::npos) e = all.size();
            const std::wstring item = all.substr(i, e - i);
            i = e + 1;
            const size_t c1 = item.find(L':'), c2 = c1 == std::wstring::npos ? c1 : item.find(L':', c1 + 1);
            if (c2 == std::wstring::npos) continue;
            Recording::TestInput in{};
            in.ms = static_cast<uint32_t>(_wtoi(item.substr(0, c1).c_str()));
            in.kind = item[c1 + 1];
            const std::wstring arg = item.substr(c2 + 1);
            if (in.kind == L'K') {
                if (!_wcsicmp(arg.c_str(), L"Tab")) in.key = {0, VK_TAB};  // (not a hotkey name)
                else if (!_wcsicmp(arg.c_str(), L"Esc")) in.key = {0, VK_ESCAPE};
                else if (!ParseHotkey(arg, in.key)) continue;
            } else {
                int x = 0, y = 0;
                if (swscanf_s(arg.c_str(), L"%d,%d", &x, &y) != 2) continue;
                in.pt = {rec.rect.left + x, rec.rect.top + y};
            }
            rec.testInput.push_back(in);
        }
    }
}

// Test mode: a made-up key combination or click, as the hooks would pass it on.
void InjectTestInput(const Recording::TestInput& in) {
    if (in.kind != L'K') {
        Overlay_Click(in.pt, in.kind == L'R');
        return;
    }
    const std::pair<UINT, UINT> mods[] = {
        {MOD_CONTROL, VK_LCONTROL}, {MOD_ALT, VK_LMENU}, {MOD_SHIFT, VK_LSHIFT}, {MOD_WIN, VK_LWIN}};
    for (const auto& [m, vk] : mods)
        if (in.key.mods & m) Overlay_Key(vk, true);
    Overlay_Key(in.key.vk, true);
    Overlay_Key(in.key.vk, false);
    for (const auto& [m, vk] : mods)
        if (in.key.mods & m) Overlay_Key(vk, false);
}

// The capture thread: grabs the area on a fixed clock. GIF: every 40 ms, writes the tiles that
// changed. Video: 30 times a second, each frame goes to the encoder.
void CaptureLoop(Recording* rec, HWND notify, int delayMs) {
    const RECT r = rec->rect;
    const double period = rec->video ? 1000.0 / kVideoFps : kFrameMs;
    Dib d;
    bool full = false;
    std::unique_ptr<AudioCapture> audio;
    std::thread audioThread;
    std::atomic<bool> audioStop{false};
    if (rec->video) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (g_settings.videoSound || g_settings.videoMic) {
            audio = std::make_unique<AudioCapture>();
            if (!audio->Open(g_settings.videoSound, g_settings.videoMic, rec->soundInfo)) audio.reset();
            Log(L"Video sound: %s", rec->soundInfo.c_str());
        }
        std::wstring info;
        if (rec->mp4.Open(rec->file, r.right - r.left, r.bottom - r.top, kVideoFps, audio != nullptr, info))
            Log(L"Video encoder: %s", info.c_str());
        else
            rec->error = L"Windows couldn't start a video encoder for this area.";
        if (!rec->mp4.HasAudio()) audio.reset();
    }
    if (rec->error.empty() && d.Create(r.right - r.left, r.bottom - r.top)) {
        HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        if (!timer) timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
        auto wait = [&](double ms) {
            if (ms <= 0) return;
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>(ms * 10000);
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
        };
        wait(delayMs);  // let the area picker fade away first
        HDC screen = GetDC(nullptr);
        rec->start = NowMs();
        rec->started = true;
        if (audio)
            audioThread = std::thread([&audio, &audioStop, rec] {
                CoInitializeEx(nullptr, COINIT_MULTITHREADED);
                audio->Run(std::llround(rec->start * 10000), audioStop,
                           [rec](LONGLONG t, const int16_t* f, UINT32 n) { rec->mp4.Audio(t, f, n); });
                CoUninitialize();
            });
        size_t nextInput = 0;
        bool testPaused = false;
        double testResumeAt = 0;
        for (uint64_t k = 0; !rec->stop; ++k) {
            if (!testPaused && rec->testPauseFor && rec->activeMs >= rec->testPauseAt) {
                testPaused = true;
                testResumeAt = NowMs() + rec->testPauseFor;
                rec->pauseReq = true;
                Log(L"Test: pause at %u ms for %u ms", rec->activeMs.load(), rec->testPauseFor);
            }
            if (rec->pauseReq) {  // paused: the clock stops until resumed (or stopped)
                const double p0 = NowMs();
                if (audio) audio->Pause(std::llround(p0 * 10000));
                Overlay_Paused(true);
                rec->paused = true;
                while (rec->pauseReq && !rec->stop) {
                    wait(10);
                    if (testPaused && testResumeAt && NowMs() >= testResumeAt) {
                        testResumeAt = 0;
                        rec->pauseReq = false;
                    }
                }
                const double p1 = NowMs();
                rec->pausedMs += p1 - p0;
                if (audio) audio->Resume(std::llround(p1 * 10000));
                Overlay_Paused(false);
                rec->paused = false;
                Log(L"Recording %s after %.0f ms paused", rec->stop ? L"stopped" : L"resumed", p1 - p0);
                if (rec->stop) break;
            }
            const double now = NowMs() - rec->start - rec->pausedMs;
            const double target = static_cast<double>(k) * period;
            if (target > now) wait(target - now);
            else if (now - target > period) {  // fell behind: skip
                const uint64_t nk = static_cast<uint64_t>(now / period);
                rec->dropped += nk - k;
                k = nk;
            }
            if (rec->stop || rec->pauseReq) {  // (a pause asked for while waiting: no frame)
                --k;
                if (rec->stop) break;
                continue;
            }
            const double wall = NowMs() - rec->start;
            const double t = wall - rec->pausedMs;
            rec->activeMs = static_cast<uint32_t>(t);
            for (; nextInput < rec->testInput.size() && rec->testInput[nextInput].ms <= t; ++nextInput)
                InjectTestInput(rec->testInput[nextInput]);
            if (rec->fake) {
                DrawFake(d, t, wall);
            } else {
                BitBlt(d.dc, 0, 0, d.w, d.h, screen, r.left, r.top, SRCCOPY);
                DrawCursorAt(d.dc, r.left, r.top);
            }
            GdiFlush();
            Overlay_Draw(d.bits, d.w, d.h);  // keys and clicks, when on
            if (t >= rec->nextThumb) {
                rec->thumbs.push_back({static_cast<uint32_t>(t), MakeThumb(d.bits, d.w, d.h)});
                rec->nextThumb = static_cast<uint32_t>(t) + rec->thumbEvery;
                if (rec->thumbs.size() >= kThumbMax) {  // long recording: keep every other one
                    std::vector<TrimThumb> half;
                    for (size_t i = 0; i < rec->thumbs.size(); i += 2) half.push_back(std::move(rec->thumbs[i]));
                    rec->thumbs = std::move(half);
                    rec->thumbEvery *= 2;
                }
            }
            if (rec->video) {
                if (!rec->mp4.Frame(static_cast<LONGLONG>(k) * 10000000LL / kVideoFps, d.bits)) {
                    rec->error = L"The video encoder stopped working.";
                    break;
                }
                ++rec->frames;
                continue;
            }
            rec->writer.Frame(static_cast<uint32_t>(t), d.bits);
            if (rec->writer.Bytes() > kMaxBytes) {
                full = true;
                break;
            }
        }
        ReleaseDC(nullptr, screen);
        if (timer) CloseHandle(timer);
    }
    rec->durationMs = rec->started ? static_cast<uint32_t>(NowMs() - rec->start - rec->pausedMs) : 0;
    audioStop = true;
    if (audioThread.joinable()) audioThread.join();
    if (audio) {
        wchar_t s[128];
        swprintf_s(s, L", sound %.2f s (%llu ms silence added, %llu ms cut)", audio->Frames() / 48000.0,
                   audio->Padded() / 48, audio->Cut() / 48);
        rec->soundInfo += s;
    }
    if (rec->video) {
        if (!rec->mp4.Close() && rec->frames > 0 && rec->error.empty()) rec->error = L"The video file couldn't be finished.";
        CoUninitialize();
    } else {
        rec->writer.Close(rec->durationMs);
    }
    if (notify) PostMessageW(notify, WM_REC_DONE, full ? 1 : 0, 0);
}

void CopyFileToClipboard(const std::wstring& path) {
    const size_t bytes = sizeof(DROPFILES) + (path.size() + 2) * sizeof(wchar_t);
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes);
    if (!g) return;
    auto* df = static_cast<DROPFILES*>(GlobalLock(g));
    df->pFiles = sizeof(DROPFILES);
    df->fWide = TRUE;
    memcpy(reinterpret_cast<BYTE*>(df) + sizeof(DROPFILES), path.c_str(), path.size() * sizeof(wchar_t));
    GlobalUnlock(g);
    HGLOBAL effect = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
    if (effect) {
        *static_cast<DWORD*>(GlobalLock(effect)) = DROPEFFECT_COPY;
        GlobalUnlock(effect);
    }
    if (!OpenClipboard(g_mainWnd)) {
        GlobalFree(g);
        if (effect) GlobalFree(effect);
        return;
    }
    EmptyClipboard();
    SetClipboardData(CF_HDROP, g);
    if (effect) SetClipboardData(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), effect);
    CloseClipboard();
}

// ---- save window -----------------------------------------------------------------------------

// The save window serves both GIFs and videos; these pick the right list of choices.
static_assert(kGifPresetCount == kVideoPresetCount, "the save window has one button per choice");
bool SavingVideo() { return R.rec && R.rec->video; }
const wchar_t* PresetKey(int i) { return SavingVideo() ? kVideoPresets[i].key : kGifPresets[i].key; }
const wchar_t* PresetTitle(int i) { return SavingVideo() ? kVideoPresets[i].title : kGifPresets[i].title; }

bool Trimmed() { return R.rec && (R.trimFrom > 0 || R.trimTo < R.rec->durationMs); }

// Video "Best quality" keeps the recording as it is; cut, it is made again at high quality.
VideoPreset VideoChoice(int i) {
    VideoPreset p = kVideoPresets[i];
    if (!p.quality && Trimmed()) p.quality = 90;
    return p;
}

std::wstring Note(int i) {
    std::wstring s = SavingVideo() ? kVideoPresets[i].note : kGifPresets[i].note;
    if (SavingVideo() && !kVideoPresets[i].quality && Trimmed()) s = L"made again at high quality";
    if (SavingVideo()) {  // the real picture size, which depends on the recording
        const int W = R.rec->rect.right - R.rec->rect.left, H = R.rec->rect.bottom - R.rec->rect.top;
        int w = 0, h = 0;
        Mp4_ShrinkSize(W, H, kVideoPresets[i].percent, Mp4_IsBusy(FileSize(R.rec->file), R.rec->durationMs, W, H), w, h);
        s = std::to_wstring(w) + L" × " + std::to_wstring(h) + L", " + s;
    }
    const Encoded& e = R.enc[i];
    if (!e.done) s += L"   ·   working out the size…";
    else if (!e.ok) s += L"   ·   failed";
    else s += L"   ·   " + SizeText(e.size);
    return s;
}

void UpdateButton(int i) {
    HWND b = GetDlgItem(R.save, IDC_PRESET0 + i);
    SendMessageW(b, BCM_SETNOTE, 0, reinterpret_cast<LPARAM>(Note(i).c_str()));
    EnableWindow(b, R.enc[i].done && R.enc[i].ok);
    if (i == R.def && R.enc[i].ok && GetFocus() != R.trim) SetFocus(b);
}

void UpdateInfo() {
    if (!R.save || !R.rec) return;
    const Recording& rec = *R.rec;
    wchar_t info[160];
    const int w = rec.rect.right - rec.rect.left, h = rec.rect.bottom - rec.rect.top;
    const wchar_t* pick = rec.video ? L"Pick a size:" : L"Pick a GIF:";
    if (Trimmed())
        swprintf_s(info, L"%.1f of %.1f seconds, %d × %d pixels. %s", (R.trimTo - R.trimFrom) / 1000.0,
                   rec.durationMs / 1000.0, w, h, pick);
    else
        swprintf_s(info, L"%.1f seconds, %d × %d pixels. %s", rec.durationMs / 1000.0, w, h, pick);
    SetDlgItemTextW(R.save, IDC_INFO, info);
}

void StopEncoders() {
    R.cancelEncode = true;
    for (std::thread& t : R.encoders)
        if (t.joinable()) t.join();
    for (Encoded& e : R.enc)
        if (!e.path.empty() && (!R.rec || e.path != R.rec->file)) DeleteFileW(e.path.c_str());
}

// Makes the three choices for the part of the recording that is kept, all at once, so each
// button can show its real size. Called again whenever the timeline changes.
void StartEncoders() {
    StopEncoders();
    R.cancelEncode = false;
    const Recording& rec = *R.rec;
    const std::wstring file = rec.file;
    HWND notify = R.save;
    const bool trimmed = Trimmed();
    const uint32_t from = R.trimFrom, to = R.trimTo >= rec.durationMs ? UINT32_MAX : R.trimTo;
    for (int i = 0; i < kGifPresetCount; ++i) {
        R.enc[i] = {};
        UpdateButton(i);
    }
    if (trimmed) Log(L"Trim: %u to %u ms of %u", R.trimFrom, R.trimTo, rec.durationMs);
    if (rec.video) {
        const LONGLONG from100 = static_cast<LONGLONG>(from) * 10000;
        const LONGLONG to100 = to == UINT32_MAX ? LLONG_MAX : static_cast<LONGLONG>(to) * 10000;
        for (int i = 0; i < kVideoPresetCount; ++i) {
            const VideoPreset vp = VideoChoice(i);
            if (!vp.quality) {  // the recording as it is
                R.enc[i].path = file;
                R.enc[i].size = FileSize(file);
                R.enc[i].ok = R.enc[i].done = true;
                UpdateButton(i);
                continue;
            }
            R.enc[i].path = file.substr(0, file.size() - 4) + L"-" + kVideoPresets[i].key + L".mp4";
            R.encoders[i] = std::thread([i, file, notify, vp, from100, to100] {
                Encoded& e = R.enc[i];
                const ULONGLONG t0 = GetTickCount64();
                std::wstring err;
                e.ok = Mp4_Shrink(file, vp, e.path, R.cancelEncode, err, from100, to100);
                e.ms = GetTickCount64() - t0;
                e.size = e.ok ? FileSize(e.path) : 0;
                if (!R.cancelEncode)
                    Log(L"Video %s: %s in %llu ms%s%s", kVideoPresets[i].key, SizeText(e.size).c_str(), e.ms,
                        e.ok ? L"" : L", failed: ", err.c_str());
                e.done = true;
                PostMessageW(notify, WM_GIF_ENCODED, i, 0);
            });
        }
        return;
    }
    for (int i = 0; i < kGifPresetCount; ++i) {
        R.enc[i].path = file.substr(0, file.size() - 4) + L"-" + kGifPresets[i].key + L".gif";
        R.encoders[i] = std::thread([i, file, notify, from, to] {
            Encoded& e = R.enc[i];
            const ULONGLONG t0 = GetTickCount64();
            std::wstring err;
            e.ok = Gif_Encode(file, kGifPresets[i], e.path, R.cancelEncode, err, from, to);
            e.ms = GetTickCount64() - t0;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (e.ok && GetFileAttributesExW(e.path.c_str(), GetFileExInfoStandard, &fa))
                e.size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
            if (!R.cancelEncode)
                Log(L"GIF %s: %s in %llu ms%s%s", kGifPresets[i].key, SizeText(e.size).c_str(), e.ms,
                    e.ok ? L"" : L", failed: ", err.c_str());
            e.done = true;
            PostMessageW(notify, WM_GIF_ENCODED, i, 0);
        });
    }
}

void SaveChoice(int i) {
    Encoded& e = R.enc[i];
    if (!e.done || !e.ok || !R.rec) return;
    const bool video = R.rec->video;
    // Stop the other encoders first: a video's "as recorded" choice is the file they are reading.
    R.cancelEncode = true;
    for (std::thread& t : R.encoders)
        if (t.joinable()) t.join();
    const std::wstring path = Output_NewPath(R.rec->meta, video ? L".mp4" : L".gif");
    if (!MoveFileExW(e.path.c_str(), path.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        const std::wstring msg = std::wstring(video ? L"The video" : L"The GIF") + L" could not be saved to\n" + path;
        if (TestMode()) Log(L"%s", msg.c_str());
        else MessageBoxW(R.save, msg.c_str(), L"Canivete", MB_ICONWARNING);
        // The ones not finished yet were stopped: show which are still there to pick.
        for (int k = 0; k < kGifPresetCount; ++k)
            if (!R.enc[k].done || !FileSize(R.enc[k].path)) R.enc[k].ok = false, R.enc[k].done = true, UpdateButton(k);
        return;
    }
    e.path.clear();  // moved: nothing left to clean up
    CopyFileToClipboard(path);
    SaveSettingValue(video ? L"Video" : L"Gif", L"Preset", PresetKey(i));
    (video ? g_settings.videoPreset : g_settings.gifPreset) = PresetKey(i);
    std::wstring what = SizeText(e.size);
    if (video) {
        const unsigned secs = (R.trimTo - R.trimFrom + 500) / 1000;
        wchar_t len[32];
        swprintf_s(len, L"%u:%02u, ", secs / 60, secs % 60);
        what = len + what;
        Log(L"Video saved (%s, %s, %u ms, sound: %s) -> %s", PresetKey(i), SizeText(e.size).c_str(),
            R.trimTo - R.trimFrom, R.rec->soundInfo.empty() ? L"off" : R.rec->soundInfo.c_str(), path.c_str());
    } else
        Log(L"GIF saved (%s, %s, %u ms) -> %s", PresetKey(i), SizeText(e.size).c_str(), R.trimTo - R.trimFrom,
            path.c_str());
    HWND up = GetDlgItem(R.save, IDC_UPLOADTOO);
    if (up && SendMessageW(up, BM_GETCHECK, 0, 0) == BST_CHECKED) Upload_File(path, false);  // its own notices
    else
        Output_ShowFile(path, video ? L"Video saved and copied" : L"GIF saved and copied",
                        path.substr(path.find_last_of(L'\\') + 1) + L"  (" + what + L")");
    DestroyWindow(R.save);
}

void Discard() {
    if (!TestMode() &&
        MessageBoxW(R.save, L"Throw this recording away?", L"Canivete", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) !=
            IDYES)
        return;
    Log(SavingVideo() ? L"Video recording discarded" : L"GIF recording discarded");
    DestroyWindow(R.save);
}

LRESULT CALLBACK SaveProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_GIF_ENCODED:
        if (wp < kGifPresetCount) UpdateButton(static_cast<int>(wp));
        return 0;
    case WM_ERASEBKGND: {
        RECT rc;
        GetClientRect(h, &rc);
        FillRect(reinterpret_cast<HDC>(wp), &rc, Theme_Brush(Pal().window));
        return 1;
    }
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id == IDC_TRIM) {
            if (HIWORD(wp) == kTrimChanged && R.trim) {
                Trim_Get(R.trim, R.trimFrom, R.trimTo);
                UpdateInfo();
                StartEncoders();
            }
            return 0;
        }
        if (id >= IDC_PRESET0 && id < IDC_PRESET0 + kGifPresetCount) SaveChoice(id - IDC_PRESET0);
        else if (id == IDOK) SaveChoice(R.def);
        else if (id == IDC_DISCARD || id == IDCANCEL) Discard();
        else if (id == IDC_UPLOADTOO && HIWORD(wp) == BN_CLICKED) {  // remembered for next time
            const bool on = SendMessageW(reinterpret_cast<HWND>(lp), BM_GETCHECK, 0, 0) == BST_CHECKED;
            const bool video = SavingVideo();
            (video ? g_settings.uploadVideo : g_settings.uploadGif) = on;
            SaveSettingValue(L"Upload", video ? L"AutoVideo" : L"AutoGif", on ? L"1" : L"0");
        }
        return 0;
    }
    case WM_CLOSE:
        Discard();
        return 0;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        SetTextColor(reinterpret_cast<HDC>(wp), Pal().text);
        return reinterpret_cast<LRESULT>(Theme_Brush(Pal().window));
    case WM_DESTROY: {
        StopEncoders();
        if (R.rec) DeleteFileW(R.rec->file.c_str());
        R.trim = nullptr;  // (destroyed right after this; it no longer paints the pictures freed here)
        R.rec.reset();
        R.save = nullptr;
        if (R.saveFont) DeleteObject(R.saveFont);
        if (R.bigFont) DeleteObject(R.bigFont);
        R.saveFont = R.bigFont = nullptr;
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void OpenSaveWindow() {
    Recording& rec = *R.rec;
    const RECT area = rec.rect;
    const UINT dpi = DpiAt(area);
    R.scale = dpi / 96.0;
    const int W = Px(500), pad = Px(16), btnH = Px(58), trimH = Px(214);
    const bool canUpload = Upload_Ready();
    const int H = pad + Px(24) + Px(8) + kGifPresetCount * (btnH + Px(6)) + (canUpload ? Px(30) : 0) + Px(8) +
                  trimH + Px(12) + Px(28) + pad;
    RECT wr{0, 0, W, H};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
    AdjustWindowRectExForDpi(&wr, style, FALSE, ex, dpi);
    const RECT work = WorkAreaAt(area);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    const int x = work.left + (work.right - work.left - ww) / 2, y = work.top + (work.bottom - work.top - wh) / 2;
    // Enter takes the choice made last time.
    const std::wstring& last = rec.video ? g_settings.videoPreset : g_settings.gifPreset;
    R.def = 1;
    for (int i = 0; i < kGifPresetCount; ++i)
        if (last == PresetKey(i)) R.def = i;
    R.save = CreateWindowExW(ex, kSaveClass, rec.video ? L"Save video" : L"Save GIF", style, x, y, ww, wh, nullptr,
                             nullptr, g_hinst, nullptr);
    R.saveFont = MessageFont(dpi, 1.0, false);
    R.bigFont = MessageFont(dpi, 1.0, true);

    R.trimFrom = 0;
    R.trimTo = rec.durationMs;
    HWND t = CreateWindowExW(0, WC_STATICW, L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX, pad, pad, W - 2 * pad, Px(22),
                             R.save, reinterpret_cast<HMENU>(IDC_INFO), g_hinst, nullptr);
    SendMessageW(t, WM_SETFONT, reinterpret_cast<WPARAM>(R.bigFont), FALSE);
    int by = pad + Px(32);
    for (int i = 0; i < kGifPresetCount; ++i) {
        HWND b = CreateWindowExW(0, WC_BUTTONW, PresetTitle(i),
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED |
                                     (i == R.def ? BS_DEFCOMMANDLINK : BS_COMMANDLINK),
                                 pad, by, W - 2 * pad, btnH, R.save, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRESET0 + i)),
                                 g_hinst, nullptr);
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(R.saveFont), FALSE);
        SendMessageW(b, BCM_SETNOTE, 0, reinterpret_cast<LPARAM>(Note(i).c_str()));
        by += btnH + Px(6);
    }
    if (canUpload) {
        const std::wstring label = L"Also upload to " + Upload_ServiceName() + L" and copy the link";
        HWND c = CreateWindowExW(0, WC_BUTTONW, label.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 pad, by + Px(2), W - 2 * pad, Px(24), R.save,
                                 reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_UPLOADTOO)), g_hinst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(R.saveFont), FALSE);
        if (Upload_Auto(rec.video ? UploadKind::Video : UploadKind::Gif)) SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
        by += Px(30);
    }
    // The timeline: cut off the start or the end. The choices above are made again when it changes.
    R.trim = Trim_Create(R.save, IDC_TRIM, pad, by + Px(8), W - 2 * pad, trimH, &rec.thumbs, rec.durationMs,
                         rec.video ? kVideoFps : 1000 / kFrameMs, dpi);
    SendMessageW(R.trim, WM_SETFONT, reinterpret_cast<WPARAM>(R.saveFont), FALSE);
    if (TestMode()) {  // "from,to" in ms: as if the handles were moved
        wchar_t tv[64];
        unsigned from = 0, to = 0;
        if (GetEnvironmentVariableW(L"CANIVETE_REC_TRIM", tv, 64) > 0 && swscanf_s(tv, L"%u,%u", &from, &to) == 2) {
            const int fps = rec.video ? kVideoFps : 1000 / kFrameMs;
            Trim_Set(R.trim, Trim_SnapMs(from, fps), std::min(Trim_SnapMs(to, fps), rec.durationMs));
            Trim_Get(R.trim, R.trimFrom, R.trimTo);
        }
    }
    UpdateInfo();
    HWND d = CreateWindowExW(0, WC_BUTTONW, L"Discard", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             W - pad - Px(100), H - pad - Px(28), Px(100), Px(28), R.save,
                             reinterpret_cast<HMENU>(IDC_DISCARD), g_hinst, nullptr);
    SendMessageW(d, WM_SETFONT, reinterpret_cast<WPARAM>(R.saveFont), FALSE);
    HWND hint = CreateWindowExW(0, WC_STATICW, L"Saved to your screenshots folder and copied.",
                                WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS, pad, H - pad - Px(24),
                                W - 3 * pad - Px(100), Px(22), R.save, nullptr, g_hinst, nullptr);
    SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(R.saveFont), FALSE);
    Theme_Window(R.save);
    ShowWindow(R.save, SW_SHOW);
    SetForegroundWindow(R.save);
    SetFocus(d);

    StartEncoders();
}

// ---- recording bar and border ----------------------------------------------------------------

void CloseRecordingWindows() {
    if (R.pauseKey && R.bar) UnregisterHotKey(R.bar, kPauseHotkey);
    R.pauseKey = false;
    Overlay_End();
    if (R.bar) DestroyWindow(R.bar);
    for (HWND& b : R.borders) {
        if (b) DestroyWindow(b);
        b = nullptr;
    }
    R.bar = nullptr;
    if (R.barFont) DeleteObject(R.barFont);
    R.barFont = nullptr;
}

// Video: the save window offers the recording as it is plus two smaller copies.
void VideoDone() {
    Recording& rec = *R.rec;
    if (rec.cancelled || (rec.error.empty() && rec.durationMs < 100)) {
        DeleteFileW(rec.file.c_str());
        Log(L"Video recording cancelled");
        R.rec.reset();
        return;
    }
    if (!rec.error.empty()) {
        DeleteFileW(rec.file.c_str());
        Log(L"Video recording failed: %s", rec.error.c_str());
        Tray_Notify(L"Video recording failed", rec.error);
        R.rec.reset();
        return;
    }
    Log(L"Video recording stopped: %u ms, %llu frames (%llu skipped), %s, sound: %s", rec.durationMs, rec.frames,
        rec.dropped, SizeText(FileSize(rec.file)).c_str(), rec.soundInfo.empty() ? L"off" : rec.soundInfo.c_str());
    OpenSaveWindow();
}

void RecordingDone(bool full) {
    if (R.rec->thread.joinable()) R.rec->thread.join();
    CloseRecordingWindows();
    if (R.rec->video) {
        VideoDone();
        return;
    }
    if (R.rec->cancelled || R.rec->durationMs < 100) {
        DeleteFileW(R.rec->file.c_str());
        Log(L"GIF recording cancelled");
        R.rec.reset();
        return;
    }
    Log(L"GIF recording stopped: %u ms, %llu KB of changes", R.rec->durationMs, R.rec->writer.Bytes() / 1024);
    if (full) Tray_Notify(L"GIF recording stopped", L"It got very large (4 GB of screen changes), so it stopped itself.");
    OpenSaveWindow();
}

void TogglePause() {
    if (!R.rec || !R.rec->started || R.rec->stop) return;
    const bool p = !R.rec->pauseReq;
    R.rec->pauseReq = p;
    Log(p ? L"Recording paused" : L"Recording resumed");
    if (R.bar) {
        SetDlgItemTextW(R.bar, IDC_PAUSE, p ? L"Resume" : L"Pause");
        RECT tr{0, 0, Px(92), Px(40)};
        InvalidateRect(R.bar, &tr, FALSE);
    }
}

void PaintBar(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, Theme_Brush(Pal().footer));
    const int cy = (rc.bottom - rc.top) / 2, d = Px(10);
    HGDIOBJ oldB = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
    // Grey dot and "…" until the first frame is taken (the video encoder needs a moment to start).
    // Paused: two bars instead of the dot, and "Paused" instead of the time (which stands still).
    const bool live = R.rec && R.rec->started;
    const bool paused = live && R.rec->paused;
    if (paused) {
        SetDCBrushColor(dc, Pal().grey);
        const RECT b1{Px(10), cy - d / 2, Px(10) + d * 2 / 5, cy + d / 2}, b2{Px(10) + d * 3 / 5, cy - d / 2, Px(10) + d, cy + d / 2};
        FillRect(dc, &b1, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        FillRect(dc, &b2, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    } else {
        SetDCBrushColor(dc, live ? kRed : RGB(150, 150, 150));
        Ellipse(dc, Px(10), cy - d / 2, Px(10) + d, cy + d / 2);
    }
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    wchar_t s[32] = L"…";
    if (paused) {
        wcscpy_s(s, L"Paused");
    } else if (live) {
        const unsigned secs = R.rec->activeMs / 1000;
        swprintf_s(s, L"%u:%02u", secs / 60, secs % 60);
    }
    HGDIOBJ oldF = SelectObject(dc, R.barFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, Pal().text);
    RECT tr{Px(26), 0, Px(92), rc.bottom};
    DrawTextW(dc, s, -1, &tr, DT_SINGLELINE | DT_VCENTER | DT_LEFT);
    SelectObject(dc, oldF);
}

LRESULT CALLBACK BarProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        PaintBar(h, dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_TIMER:
        if (wp == kTimerTick) {
            RECT tr{0, 0, Px(92), Px(40)};
            InvalidateRect(h, &tr, FALSE);
            if (R.rec) {  // the button follows the state (the pause may come from the hotkey or a test)
                const bool p = R.rec->pauseReq;
                SetDlgItemTextW(h, IDC_PAUSE, p ? L"Resume" : L"Pause");
            }
        } else if (wp == kTimerTestStop) {
            KillTimer(h, kTimerTestStop);
            Recorder_Stop();
        }
        return 0;
    case WM_HOTKEY:
        if (wp == kPauseHotkey) TogglePause();
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_STOP) Recorder_Stop();
        else if (LOWORD(wp) == IDC_PAUSE) TogglePause();
        else if (LOWORD(wp) == IDC_KEYS && HIWORD(wp) == BN_CLICKED)
            Overlay_SetOn(SendMessageW(reinterpret_cast<HWND>(lp), BM_GETCHECK, 0, 0) == BST_CHECKED);
        else if (LOWORD(wp) == IDC_CANCELREC && R.rec) {
            R.rec->cancelled = true;
            R.rec->stop = true;
        }
        return 0;
    case WM_REC_DONE:
        RecordingDone(wp != 0);
        return 0;
    case WM_CTLCOLORBTN:
        return reinterpret_cast<LRESULT>(Theme_Brush(Pal().footer));
    case WM_CTLCOLORSTATIC:  // the Keys check box
        SetBkColor(reinterpret_cast<HDC>(wp), Pal().footer);
        SetTextColor(reinterpret_cast<HDC>(wp), Pal().text);
        return reinterpret_cast<LRESULT>(Theme_Brush(Pal().footer));
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;  // clicking Stop must not take focus from the app being recorded
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void RegisterClasses() {
    static bool done = false;
    if (done) return;
    done = true;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = BarProc;
    wc.lpszClassName = kBarClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = DefWindowProcW;
    wc.lpszClassName = kBorderClass;
    wc.hbrBackground = CreateSolidBrush(kRed);
    RegisterClassExW(&wc);
    wc.lpfnWndProc = SaveProc;
    wc.lpszClassName = kSaveClass;
    wc.hbrBackground = GetSysColorBrush(COLOR_WINDOW);
    wc.hIcon = LoadIconW(g_hinst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
}

void ShowRecordingWindows(const RECT& r) {
    const UINT dpi = DpiAt(r);
    R.scale = dpi / 96.0;
    const DWORD ex = WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE;
    // Border just outside the area.
    const int t = std::max(2, Px(2));
    const RECT sides[4] = {{r.left - t, r.top - t, r.right + t, r.top},
                           {r.left - t, r.bottom, r.right + t, r.bottom + t},
                           {r.left - t, r.top, r.left, r.bottom},
                           {r.right, r.top, r.right + t, r.bottom}};
    for (int i = 0; i < 4; ++i) {
        const RECT& s = sides[i];
        R.borders[i] = CreateWindowExW(ex | WS_EX_LAYERED | WS_EX_TRANSPARENT, kBorderClass, L"", WS_POPUP, s.left, s.top,
                                       s.right - s.left, s.bottom - s.top, nullptr, nullptr, g_hinst, nullptr);
        SetLayeredWindowAttributes(R.borders[i], 0, 230, LWA_ALPHA);
        SetWindowDisplayAffinity(R.borders[i], kExcludeFromCapture);
        ShowWindow(R.borders[i], SW_SHOWNOACTIVATE);
    }
    // Bar: below the area, else above it, else inside its bottom edge.
    const int bw = Px(422), bh = Px(40);
    const RECT work = WorkAreaAt(r);
    int x = std::clamp(static_cast<int>(r.left), static_cast<int>(work.left), static_cast<int>(work.right) - bw);
    int y = r.bottom + t + Px(6);
    if (y + bh > work.bottom) y = r.top - t - Px(6) - bh;
    if (y < work.top) y = r.bottom - bh - Px(8);
    R.bar = CreateWindowExW(ex, kBarClass, R.rec && R.rec->video ? L"Canivete video recording" : L"Canivete GIF recording",
                            WS_POPUP | WS_BORDER, x, y, bw, bh, nullptr, nullptr,
                            g_hinst, nullptr);
    SetWindowDisplayAffinity(R.bar, kExcludeFromCapture);
    R.barFont = MessageFont(dpi, 1.0, true);
    const bool video = R.rec && R.rec->video;
    struct {
        int id;
        const wchar_t* text;
        DWORD style;
        int x, w;
    } const buttons[] = {{IDC_PAUSE, L"Pause", BS_PUSHBUTTON, 96, 76},
                         {IDC_KEYS, L"Keys", BS_AUTOCHECKBOX, 180, 70},
                         {IDC_STOP, L"Stop", BS_DEFPUSHBUTTON, 256, 76},
                         {IDC_CANCELREC, L"Cancel", BS_PUSHBUTTON, 338, 76}};
    for (const auto& b : buttons) {
        HWND c = CreateWindowExW(0, WC_BUTTONW, b.text, WS_CHILD | WS_VISIBLE | b.style, Px(b.x), Px(6), Px(b.w),
                                 bh - Px(14), R.bar, reinterpret_cast<HMENU>(static_cast<INT_PTR>(b.id)), g_hinst, nullptr);
        SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(R.barFont), FALSE);
    }
    // Show keys and clicks: as set for this kind of recording; the check box turns it on or off.
    const bool keys = video ? g_settings.videoShowKeys : g_settings.gifShowKeys;
    Overlay_Begin(r, video ? g_settings.videoAllKeys : g_settings.gifAllKeys, dpi);
    Hotkey hk;
    for (const std::wstring* k : {&g_settings.hkGif, &g_settings.hkVideo, &g_settings.hkGifPause, &g_settings.hkVideoPause})
        if (ParseHotkey(*k, hk)) Overlay_Ignore(hk);
    Overlay_SetOn(keys);
    CheckDlgButton(R.bar, IDC_KEYS, keys ? BST_CHECKED : BST_UNCHECKED);
    // The pause hotkey is only taken while recording (tests never take hotkeys).
    if (!TestMode() && ParseHotkey(video ? g_settings.hkVideoPause : g_settings.hkGifPause, hk) && hk.vk) {
        R.pauseKey = RegisterHotKey(R.bar, kPauseHotkey, hk.mods, hk.vk) != 0;
        if (!R.pauseKey) Log(L"Pause hotkey %s is taken by another app", HotkeyText(hk).c_str());
    }
    Theme_Window(R.bar);
    ShowWindow(R.bar, SW_SHOWNOACTIVATE);
    SetTimer(R.bar, kTimerTick, 250, nullptr);
}

std::unique_ptr<Recording> NewRecording(const RECT& r, bool video) {
    auto rec = std::make_unique<Recording>();
    rec->rect = r;
    rec->video = video;
    rec->file = TempFolder() + (video ? L"\\vid-" : L"\\gif-") + std::to_wstring(GetTickCount64()) + (video ? L".mp4" : L".cgf");
    wchar_t fake[8];
    rec->fake = TestMode() && GetEnvironmentVariableW(L"CANIVETE_GIF_FAKE", fake, 8) > 0;
    ProcessOf(GetForegroundWindow(), rec->meta.processName, rec->meta.windowTitle);
    rec->meta.screenRect = r;
    rec->meta.img.w = r.right - r.left;
    rec->meta.img.h = r.bottom - r.top;
    if (!video && !rec->writer.Open(rec->file, r.right - r.left, r.bottom - r.top)) return nullptr;
    ReadTestHooks(*rec);
    return rec;
}

}  // namespace

void Recorder_Start(const RECT& area, bool video) {
    RECT r = area;
    if (video) {  // video encoders need an even width and height
        r.right -= (r.right - r.left) & 1;
        r.bottom -= (r.bottom - r.top) & 1;
    }
    if (R.rec || r.right - r.left < 2 || r.bottom - r.top < 2) return;
    if (video && (r.right - r.left < 16 || r.bottom - r.top < 16)) {
        Tray_Notify(L"Video not recorded", L"The area is too small for a video (16 x 16 pixels at least).");
        return;
    }
    RegisterClasses();
    if (!R.save) DeleteLeftovers();
    R.rec = NewRecording(r, video);
    if (!R.rec) {
        Tray_Notify(L"GIF recording failed", L"Couldn't create the recording file in the temp folder.");
        return;
    }
    Log(L"%s recording started: %dx%d at %d,%d", video ? L"Video" : L"GIF", r.right - r.left, r.bottom - r.top, r.left,
        r.top);
    ShowRecordingWindows(r);
    R.rec->thread = std::thread(CaptureLoop, R.rec.get(), R.bar, 150);
}

bool Recorder_Active() { return R.rec != nullptr; }

void Recorder_Stop() {
    if (R.save) {
        SetForegroundWindow(R.save);
        return;
    }
    if (R.rec) R.rec->stop = true;  // the thread finishes the file and tells the bar
}

HWND Recorder_Window() { return R.save; }

void Recorder_Shutdown() {
    if (R.save) DestroyWindow(R.save);  // joins the encoders, deletes temp files
    if (R.rec) {
        R.rec->cancelled = true;
        R.rec->stop = true;
        if (R.rec->thread.joinable()) R.rec->thread.join();
        DeleteFileW(R.rec->file.c_str());
        R.rec.reset();
    }
    CloseRecordingWindows();
}

void Recorder_Test(const RECT& r, int ms, bool video) {
    Recorder_Start(r, video);
    if (R.bar) SetTimer(R.bar, kTimerTestStop, ms, nullptr);
}

int Recorder_BenchVideo(const RECT& area, int ms, const std::wstring& outPath) {
    RECT r = area;
    r.right -= (r.right - r.left) & 1;
    r.bottom -= (r.bottom - r.top) & 1;
    auto rec = NewRecording(r, true);
    const double t0 = NowMs();
    rec->thread = std::thread(CaptureLoop, rec.get(), nullptr, 0);
    Sleep(ms);
    rec->stop = true;
    rec->thread.join();
    const double total = NowMs() - t0;
    bool ok = rec->error.empty() && MoveFileExW(rec->file.c_str(), outPath.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING);
    uint64_t size = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (ok && GetFileAttributesExW(outPath.c_str(), GetFileExInfoStandard, &fa))
        size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    FILETIME c, e, k, u;
    GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
    const double cpuMs = ((static_cast<uint64_t>(k.dwHighDateTime) << 32 | k.dwLowDateTime) +
                          (static_cast<uint64_t>(u.dwHighDateTime) << 32 | u.dwLowDateTime)) / 10000.0;
    Log(L"Video bench: %dx%d, %u ms recorded (%.0f ms with setup), %llu frames, %llu skipped, CPU %.0f ms (%.0f%% of one core), %s%s%s",
        r.right - r.left, r.bottom - r.top, rec->durationMs, total, rec->frames, rec->dropped, cpuMs, 100 * cpuMs / total,
        ok ? SizeText(size).c_str() : L"FAILED", rec->error.empty() ? L"" : L": ", rec->error.c_str());
    Log(L"Video bench sound: %s", rec->soundInfo.empty() ? L"off" : rec->soundInfo.c_str());
    DeleteFileW(rec->file.c_str());
    return ok ? 0 : 3;
}

int Recorder_BenchShrink(const std::wstring& in, const std::wstring& out, const VideoPreset& p) {
    std::atomic<bool> cancel{false};
    std::wstring err;
    const double t0 = NowMs();
    const bool ok = Mp4_Shrink(in, p, out, cancel, err);
    const double ms = NowMs() - t0;
    Log(L"Video shrink: %d%% size, quality %d, sound %d kbps: %s in %.0f ms%s%s", p.percent, p.quality, p.audioKbps,
        ok ? SizeText(FileSize(out)).c_str() : L"FAILED", ms, ok ? L"" : L": ", err.c_str());
    return ok ? 0 : 3;
}

int Recorder_Bench(const RECT& r, int ms, const std::wstring& outDir) {
    auto rec = NewRecording(r, false);
    if (!rec) return 2;
    rec->thread = std::thread(CaptureLoop, rec.get(), nullptr, 0);
    Sleep(ms);
    rec->stop = true;
    rec->thread.join();
    std::wstring report = L"recording " + std::to_wstring(rec->durationMs) + L" ms, " + SizeText(rec->writer.Bytes()) +
                          L" of changes\r\n";
    std::atomic<bool> cancel{false};
    Encoded enc[kGifPresetCount];
    std::thread th[kGifPresetCount];
    for (int i = 0; i < kGifPresetCount; ++i) {
        enc[i].path = outDir + L"\\" + kGifPresets[i].key + L".gif";
        th[i] = std::thread([&, i] {
            const ULONGLONG t0 = GetTickCount64();
            std::wstring err;
            enc[i].ok = Gif_Encode(rec->file, kGifPresets[i], enc[i].path, cancel, err);
            enc[i].ms = GetTickCount64() - t0;
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (GetFileAttributesExW(enc[i].path.c_str(), GetFileExInfoStandard, &fa))
                enc[i].size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
        });
    }
    for (std::thread& t : th) t.join();
    for (int i = 0; i < kGifPresetCount; ++i)
        report += std::wstring(kGifPresets[i].key) + L": " + (enc[i].ok ? SizeText(enc[i].size) : L"FAILED") + L" in " +
                  std::to_wstring(enc[i].ms) + L" ms\r\n";
    DeleteFileW(rec->file.c_str());
    Log(L"GIF bench: %s", report.c_str());
    FILE* f = nullptr;
    if (!_wfopen_s(&f, (outDir + L"\\bench.txt").c_str(), L"wb, ccs=UTF-8") && f) {
        fputws(report.c_str(), f);
        fclose(f);
    }
    return 0;
}
