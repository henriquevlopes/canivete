// Screen recording, GIF and MP4: a red border around the area, a small bar with the time and
// Stop / Cancel, and the capture thread. GIF: 25 frames a second, only changed tiles go to disk, then
// a save window offers three ready-made GIFs with their real file sizes. MP4: 30 frames a second
// straight into the H.264 encoder (video.cpp); Stop saves it to the screenshots folder at once.
// The border, bar and save window are hidden from screen capture, so they never end up in a recording.
#include "app.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>

#include <algorithm>
#include <atomic>
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
enum { IDC_STOP = 101, IDC_CANCELREC = 102, IDC_PRESET0 = 110, IDC_DISCARD = 120, IDC_INFO = 121 };
constexpr UINT_PTR kTimerTick = 1, kTimerTestStop = 2;

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
} R;

int Px(double v) { return static_cast<int>(std::lround(v * R.scale)); }

std::wstring SizeText(uint64_t b) {
    wchar_t s[32];
    if (b >= 1024 * 1024) swprintf_s(s, L"%.1f MB", b / (1024.0 * 1024.0));
    else swprintf_s(s, L"%llu KB", std::max<uint64_t>(1, (b + 512) / 1024));
    return s;
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
void DrawFake(Dib& d, double t) {
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
        for (uint64_t k = 0; !rec->stop; ++k) {
            const double now = NowMs() - rec->start;
            const double target = static_cast<double>(k) * period;
            if (target > now) wait(target - now);
            else if (now - target > period) {  // fell behind: skip
                const uint64_t nk = static_cast<uint64_t>(now / period);
                rec->dropped += nk - k;
                k = nk;
            }
            if (rec->stop) break;
            const double t = NowMs() - rec->start;
            if (rec->fake) {
                DrawFake(d, t);
            } else {
                BitBlt(d.dc, 0, 0, d.w, d.h, screen, r.left, r.top, SRCCOPY);
                DrawCursorAt(d.dc, r.left, r.top);
            }
            GdiFlush();
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
    rec->durationMs = rec->started ? static_cast<uint32_t>(NowMs() - rec->start) : 0;
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

std::wstring Note(int i) {
    std::wstring s = kGifPresets[i].note;
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
    if (i == R.def && R.enc[i].ok) SetFocus(b);
}

void SaveChoice(int i) {
    Encoded& e = R.enc[i];
    if (!e.done || !e.ok || !R.rec) return;
    const std::wstring path = Output_NewPath(R.rec->meta, L".gif");
    if (!MoveFileExW(e.path.c_str(), path.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        const std::wstring msg = L"The GIF could not be saved to\n" + path;
        if (TestMode()) Log(L"%s", msg.c_str());
        else MessageBoxW(R.save, msg.c_str(), L"Canivete", MB_ICONWARNING);
        return;
    }
    e.path.clear();  // moved: nothing left to clean up
    CopyFileToClipboard(path);
    SaveSettingValue(L"Gif", L"Preset", kGifPresets[i].key);
    g_settings.gifPreset = kGifPresets[i].key;
    Log(L"GIF saved (%s, %s) -> %s", kGifPresets[i].key, SizeText(e.size).c_str(), path.c_str());
    Tray_Notify(L"GIF saved", path.substr(path.find_last_of(L'\\') + 1) + L"  (" + SizeText(e.size) +
                                  L")\nCopied: paste it into a chat or a folder.");
    DestroyWindow(R.save);
}

void Discard() {
    if (!TestMode() &&
        MessageBoxW(R.save, L"Throw this recording away?", L"Canivete", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) !=
            IDYES)
        return;
    Log(L"GIF recording discarded");
    DestroyWindow(R.save);
}

LRESULT CALLBACK SaveProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_GIF_ENCODED:
        if (wp < kGifPresetCount) UpdateButton(static_cast<int>(wp));
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp);
        if (id >= IDC_PRESET0 && id < IDC_PRESET0 + kGifPresetCount) SaveChoice(id - IDC_PRESET0);
        else if (id == IDOK) SaveChoice(R.def);
        else if (id == IDC_DISCARD || id == IDCANCEL) Discard();
        return 0;
    }
    case WM_CLOSE:
        Discard();
        return 0;
    case WM_CTLCOLORSTATIC:
        SetBkMode(reinterpret_cast<HDC>(wp), TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
    case WM_DESTROY: {
        R.cancelEncode = true;
        for (std::thread& t : R.encoders)
            if (t.joinable()) t.join();
        for (Encoded& e : R.enc)
            if (!e.path.empty()) DeleteFileW(e.path.c_str());
        if (R.rec) DeleteFileW(R.rec->file.c_str());
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
    const int W = Px(470), pad = Px(16), btnH = Px(58);
    const int H = pad + Px(24) + Px(8) + kGifPresetCount * (btnH + Px(6)) + Px(10) + Px(28) + pad;
    RECT wr{0, 0, W, H};
    const DWORD style = WS_POPUP | WS_CAPTION | WS_SYSMENU, ex = WS_EX_DLGMODALFRAME;
    AdjustWindowRectExForDpi(&wr, style, FALSE, ex, dpi);
    const RECT work = WorkAreaAt(area);
    const int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
    const int x = work.left + (work.right - work.left - ww) / 2, y = work.top + (work.bottom - work.top - wh) / 2;
    R.save = CreateWindowExW(ex, kSaveClass, L"Save GIF", style, x, y, ww, wh, nullptr, nullptr, g_hinst, nullptr);
    R.saveFont = MessageFont(dpi, 1.0, false);
    R.bigFont = MessageFont(dpi, 1.0, true);

    wchar_t info[160];
    swprintf_s(info, L"%.1f seconds, %d × %d pixels, full size. Pick a GIF:", rec.durationMs / 1000.0,
               rec.rect.right - rec.rect.left, rec.rect.bottom - rec.rect.top);
    HWND t = CreateWindowExW(0, WC_STATICW, info, WS_CHILD | WS_VISIBLE | SS_NOPREFIX, pad, pad, W - 2 * pad, Px(22),
                             R.save, reinterpret_cast<HMENU>(IDC_INFO), g_hinst, nullptr);
    SendMessageW(t, WM_SETFONT, reinterpret_cast<WPARAM>(R.bigFont), FALSE);
    int by = pad + Px(32);
    for (int i = 0; i < kGifPresetCount; ++i) {
        HWND b = CreateWindowExW(0, WC_BUTTONW, kGifPresets[i].title,
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_DISABLED |
                                     (i == R.def ? BS_DEFCOMMANDLINK : BS_COMMANDLINK),
                                 pad, by, W - 2 * pad, btnH, R.save, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_PRESET0 + i)),
                                 g_hinst, nullptr);
        SendMessageW(b, WM_SETFONT, reinterpret_cast<WPARAM>(R.saveFont), FALSE);
        SendMessageW(b, BCM_SETNOTE, 0, reinterpret_cast<LPARAM>(Note(i).c_str()));
        by += btnH + Px(6);
    }
    HWND d = CreateWindowExW(0, WC_BUTTONW, L"Discard", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON,
                             W - pad - Px(100), H - pad - Px(28), Px(100), Px(28), R.save,
                             reinterpret_cast<HMENU>(IDC_DISCARD), g_hinst, nullptr);
    SendMessageW(d, WM_SETFONT, reinterpret_cast<WPARAM>(R.saveFont), FALSE);
    HWND hint = CreateWindowExW(0, WC_STATICW, L"Saved to your screenshots folder and copied.",
                                WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS, pad, H - pad - Px(24),
                                W - 3 * pad - Px(100), Px(22), R.save, nullptr, g_hinst, nullptr);
    SendMessageW(hint, WM_SETFONT, reinterpret_cast<WPARAM>(R.saveFont), FALSE);
    ShowWindow(R.save, SW_SHOW);
    SetForegroundWindow(R.save);
    SetFocus(d);

    // All three GIFs are made at once, so each button can show its real size.
    R.cancelEncode = false;
    const std::wstring file = rec.file;
    HWND notify = R.save;
    for (int i = 0; i < kGifPresetCount; ++i) {
        R.enc[i] = {};
        R.enc[i].path = file.substr(0, file.size() - 4) + L"-" + kGifPresets[i].key + L".gif";
        R.encoders[i] = std::thread([i, file, notify] {
            Encoded& e = R.enc[i];
            const ULONGLONG t0 = GetTickCount64();
            std::wstring err;
            e.ok = Gif_Encode(file, kGifPresets[i], e.path, R.cancelEncode, err);
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

// ---- recording bar and border ----------------------------------------------------------------

void CloseRecordingWindows() {
    if (R.bar) DestroyWindow(R.bar);
    for (HWND& b : R.borders) {
        if (b) DestroyWindow(b);
        b = nullptr;
    }
    R.bar = nullptr;
    if (R.barFont) DeleteObject(R.barFont);
    R.barFont = nullptr;
}

// Video: straight to the screenshots folder, copied as a file, with a tray notice.
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
    const std::wstring path = Output_NewPath(rec.meta, L".mp4");
    if (!MoveFileExW(rec.file.c_str(), path.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        Log(L"Video could not be saved to %s", path.c_str());
        Tray_Notify(L"Video not saved", L"It could not be saved to\n" + path);
        DeleteFileW(rec.file.c_str());
        R.rec.reset();
        return;
    }
    uint64_t size = 0;
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fa))
        size = (static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow;
    CopyFileToClipboard(path);
    const unsigned secs = (rec.durationMs + 500) / 1000;
    wchar_t len[32];
    swprintf_s(len, L"%u:%02u", secs / 60, secs % 60);
    Log(L"Video saved: %u ms, %llu frames (%llu skipped), %s, sound: %s -> %s", rec.durationMs, rec.frames, rec.dropped,
        SizeText(size).c_str(), rec.soundInfo.empty() ? L"off" : rec.soundInfo.c_str(), path.c_str());
    Tray_Notify(L"Video saved", path.substr(path.find_last_of(L'\\') + 1) + L"  (" + len + L", " + SizeText(size) +
                                    L")\nCopied: paste it into a chat or a folder.");
    R.rec.reset();
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

void PaintBar(HWND h, HDC dc) {
    RECT rc;
    GetClientRect(h, &rc);
    FillRect(dc, &rc, GetSysColorBrush(COLOR_BTNFACE));
    const int cy = (rc.bottom - rc.top) / 2, d = Px(10);
    HGDIOBJ oldB = SelectObject(dc, GetStockObject(DC_BRUSH));
    HGDIOBJ oldP = SelectObject(dc, GetStockObject(NULL_PEN));
    // Grey dot and "…" until the first frame is taken (the video encoder needs a moment to start).
    const bool live = R.rec && R.rec->started;
    SetDCBrushColor(dc, live ? kRed : RGB(150, 150, 150));
    Ellipse(dc, Px(10), cy - d / 2, Px(10) + d, cy + d / 2);
    SelectObject(dc, oldB);
    SelectObject(dc, oldP);
    wchar_t s[32] = L"…";
    if (live) {
        const unsigned secs = static_cast<unsigned>((NowMs() - R.rec->start) / 1000);
        swprintf_s(s, L"%u:%02u", secs / 60, secs % 60);
    }
    HGDIOBJ oldF = SelectObject(dc, R.barFont);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, GetSysColor(COLOR_BTNTEXT));
    RECT tr{Px(26), 0, Px(80), rc.bottom};
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
            RECT tr{0, 0, Px(80), Px(40)};
            InvalidateRect(h, &tr, FALSE);
        } else if (wp == kTimerTestStop) {
            KillTimer(h, kTimerTestStop);
            Recorder_Stop();
        }
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == IDC_STOP) Recorder_Stop();
        else if (LOWORD(wp) == IDC_CANCELREC && R.rec) {
            R.rec->cancelled = true;
            R.rec->stop = true;
        }
        return 0;
    case WM_REC_DONE:
        RecordingDone(wp != 0);
        return 0;
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
    const int bw = Px(250), bh = Px(40);
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
    HWND stop = CreateWindowExW(0, WC_BUTTONW, L"Stop", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, Px(84), Px(6), Px(76),
                                bh - Px(14), R.bar, reinterpret_cast<HMENU>(IDC_STOP), g_hinst, nullptr);
    HWND cancel = CreateWindowExW(0, WC_BUTTONW, L"Cancel", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, Px(166), Px(6), Px(76),
                                  bh - Px(14), R.bar, reinterpret_cast<HMENU>(IDC_CANCELREC), g_hinst, nullptr);
    SendMessageW(stop, WM_SETFONT, reinterpret_cast<WPARAM>(R.barFont), FALSE);
    SendMessageW(cancel, WM_SETFONT, reinterpret_cast<WPARAM>(R.barFont), FALSE);
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
