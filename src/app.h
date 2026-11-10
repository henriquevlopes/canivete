// Canivete - shared declarations.
#pragma once
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <vector>

constexpr wchar_t kAppName[] = L"Canivete";
constexpr wchar_t kMainClass[] = L"CaniveteMain";

// Messages to the main (hidden) window.
constexpr UINT WM_APP_TRAY = WM_APP + 1;          // tray icon callback
constexpr UINT WM_APP_CAPTURE_DONE = WM_APP + 2;  // lParam = Output job, finished on a worker
constexpr UINT WM_APP_COMMAND = WM_APP + 3;       // wParam = command id (from a second instance)
constexpr UINT WM_APP_MAINT = WM_APP + 4;         // lParam = background maintenance check result
constexpr UINT WM_APP_UPDATE = WM_APP + 5;        // Canivete update check / download finished (update.cpp)
constexpr UINT WM_APP_UPLOAD = WM_APP + 6;        // upload started / progress / finished (upload.cpp)
constexpr UINT WM_APP_NOBG = WM_APP + 7;          // to the editor: lParam = new std::wstring, Remove background's result

enum class CaptureMode { Region = 1, Window = 2, Full = 3, Color = 4, Gif = 5, Video = 6 };  // also the hotkey ids
constexpr int kHotkeyCount = 6;

// ---- settings.cpp --------------------------------------------------------------------------
struct Hotkey {
    UINT mods = 0;
    UINT vk = 0;
};

// Features that can be turned off in Settings (each tab's On switch). Off = no hotkeys, no tray menu
// entries and no Explorer entries.
enum Feature : int {
    kFeatShots, kFeatColor, kFeatGif, kFeatVideo, kFeatImages, kFeatRename, kFeatMaint, kFeatUpload, kFeatCount
};
inline constexpr const wchar_t* kFeatureKeys[kFeatCount] = {L"Screenshots", L"ColorPicker", L"Gif",
                                                             L"Video",       L"ImageTools",  L"Renamer",
                                                             L"Maintenance", L"Uploads"};

struct Settings {
    bool features[kFeatCount] = {true, true, true, true, true, true, true, true};
    std::wstring hkRegion, hkWindow, hkFull, hkColor, hkGif, hkVideo;
    std::wstring folder;    // empty = Windows "Screenshots" folder
    std::wstring fileName;  // pattern with %tokens
    std::wstring fullScreen;  // "all" or "monitor"
    std::wstring format;      // "png" or "jpg"
    int jpegQuality = 90;     // 1..100
    std::wstring colorFormat;  // key from kColorFormats
    bool saveToFile = true;
    bool copyToClipboard = true;
    bool includeCursor = false;
    bool showNotification = true;
    bool showMagnifier = true;
    bool openEditor = false;  // open the editor after each capture instead of the preview
    bool videoSound = true;   // videos: record what the computer plays
    bool videoMic = false;    // videos: record the microphone
    std::wstring hkGifPause, hkVideoPause;  // pause / resume while recording (only held while recording)
    bool gifShowKeys = false, videoShowKeys = false;  // draw keys and clicks into the recording
    bool gifAllKeys = false, videoAllKeys = false;    // every key, not only shortcuts and special keys
    std::wstring gifPreset;   // last GIF choice: a kGifPresets key
    std::wstring videoPreset; // last video choice: a kVideoPresets key
    bool maintDaily = true;   // Maintenance: check once a day in the background and tell me what needs doing
    bool checkUpdates = true; // General: tell me when a new version of Canivete is out
    // Uploads (secrets are in secrets.ini, encrypted: see upload.cpp)
    std::wstring uploadService = L"drive";  // "drive" or "s3"
    bool uploadShots = false, uploadGif = false, uploadVideo = false;  // upload by itself after each one
    std::wstring driveClientId;
    std::wstring driveFolder = L"Canivete";  // "" = My Drive itself
    bool driveShare = true;                  // anyone with the link can view
    std::wstring s3Bucket, s3Region, s3Prefix = L"canivete/", s3KeyId;
    std::wstring s3LinkBase;  // public address in front of the key (CloudFront, website); "" = the bucket's
    bool s3Private = false;   // presigned links that work for 7 days
    std::wstring theme;       // General: "system" (follow Windows), "light" or "dark"
};

extern Settings g_settings;
inline bool FeatureOn(Feature f) { return g_settings.features[f]; }
extern HINSTANCE g_hinst;
extern HWND g_mainWnd;

bool TestMode();  // CANIVETE_TEST set: private test copy, see settings.cpp
std::wstring AppDataDir();
std::wstring SettingsPath();
void LoadSettings();
void SaveSettings(const Settings& s);
bool ParseHotkey(const std::wstring& text, Hotkey& out);
std::wstring HotkeyText(const Hotkey& hk);  // "Ctrl+Shift+PrintScreen", "" when none
void Log(const wchar_t* fmt, ...);
std::wstring LogPath();   // %APPDATA%\Canivete\logs\canivete.log
void Log_DisableFile();  // the elevated helper copy writes nothing into the user's folder
void SaveSettingValue(const wchar_t* section, const wchar_t* key, const std::wstring& value);

// Color picker copy formats: settings key and the label shown in Settings.
struct ColorFormatDef {
    const wchar_t* key;
    const wchar_t* label;
};
inline constexpr ColorFormatDef kColorFormats[] = {
    {L"gray", L"Gray 0-1   0.502"},
    {L"hex", L"HEX   1e90ff"},
    {L"rgb", L"RGB   rgb(30, 144, 255)"},
    {L"hsl", L"HSL   hsl(210, 100%, 56%)"},
};

// ---- color.cpp -----------------------------------------------------------------------------
std::wstring FormatColor(COLORREF c, const std::wstring& formatKey);
void Color_Picked(COLORREF c, POINT at);  // copy in the chosen format, remember it, show a toast
std::vector<COLORREF> RecentColors();     // newest first, up to 10

// ---- main.cpp ------------------------------------------------------------------------------
std::wstring Settings_Applied();     // reload after a save; returns hotkey problems ("" = all fine)
void Hotkeys_Suspend(bool suspend);  // while a hotkey box in Settings has focus
bool AutostartEnabled();
void SetAutostart(bool on);
void Tray_Notify(const wchar_t* title, const std::wstring& text);  // balloon from the tray icon
// The same, and clicking the balloon runs onClick.
void Tray_NotifyAction(const wchar_t* title, const std::wstring& text, std::function<void()> onClick);
void Tray_SetTip(const std::wstring& tip);  // tray icon tooltip; "" = the usual one

// ---- resizer.cpp ---------------------------------------------------------------------------
void Resizer_Open(const std::vector<std::wstring>& files);  // opens the window, or adds to it
HWND Resizer_Window();                                       // for IsDialogMessage, nullptr when closed
// Explorer's Canivete submenu: small/medium/large/half/custom, jpg/png/tif/bmp, right/left/180/fliph/flipv, copy.
// Runs without a window and shows a tray notice when done.
void Resizer_Quick(const std::wstring& action, const std::vector<std::wstring>& files);

// ---- renamer.cpp ---------------------------------------------------------------------------
void Renamer_Open(const std::vector<std::wstring>& paths);
HWND Renamer_Window();

// ---- shell.cpp -----------------------------------------------------------------------------
void Shell_Register();  // Explorer right-click and Send to entries for the resizer and renamer
void Shell_Unregister();  // removes them all (and the Start menu Maintenance shortcut): --uninstall

// ---- settings_ui.cpp -----------------------------------------------------------------------
constexpr int kSettingsTabUploads = 5;
void SettingsUI_Show(int tab = -1);  // -1 = the tab it was closed on
bool SettingsUI_Key(const MSG& m);  // Ctrl+Tab between tabs; true = handled
HWND SettingsUI_Window();  // for IsDialogMessage, nullptr when closed
void SettingsUI_UploadStatus(const std::wstring& error);  // after a Drive sign-in or S3 test ("" = fine)

// ---- upload.cpp ----------------------------------------------------------------------------
enum class UploadKind { Screenshot, Gif, Video };
bool Upload_Ready();                // on, and the chosen service is set up
std::wstring Upload_ServiceName();  // "Google Drive" / "Amazon S3"
bool Upload_Auto(UploadKind k);     // ready, and set to upload this kind by itself
// Queues a file; the link goes on the clipboard when it's up. deleteAfter: a temporary file.
void Upload_File(const std::wstring& path, bool deleteAfter = false);
struct Image;
struct CaptureResult;
void Upload_Image(const Image& img, const CaptureResult& meta);  // a picture that has no file: temp PNG
void Upload_DriveConnect();  // browser sign-in (uses the saved Client ID and secret)
bool Upload_DriveConnecting();
void Upload_DriveDisconnect();       // forgets the sign-in and tells Google
void Upload_Uninstall();  // --uninstall: revokes the Google sign-in (waits for it), deletes secrets.ini
std::wstring Upload_DriveAccount();  // the connected account, "" = not connected
void Upload_S3Test();                // a small test file with the saved settings
void Upload_OnMessage(WPARAM wp, LPARAM lp);
std::wstring Upload_LoadSecret(const wchar_t* name);                     // DPAPI-encrypted, in secrets.ini
void Upload_SaveSecret(const wchar_t* name, const std::wstring& value);  // "" removes it
int Upload_SelfTest();  // --upload-selftest: signing against AWS's examples, PKCE, DPAPI

// ---- maint.cpp / maint_ops.cpp --------------------------------------------------------------
void Maint_Show(int tab = -1);  // tab: 0 Overview, 1 Updates, 2 Installed apps, 3 Cleanup, 4 Startup apps,
                                // 5 Health; -1 = last one
HWND Maint_Window();
int Maint_AdminMain(int argc, wchar_t** argv);  // --maint-admin: the elevated copy that runs one job
int Maint_WalkTest(const wchar_t* dir);  // --clean-walk-test (test copies): empties dir like a cleanup place
void Maint_Background(bool force = false);  // daily check when due; force: now, whatever the time
long long Maint_SecondsToNext();             // until the daily check is due (0 = now, LLONG_MAX = never)
void Maint_OnBackground(LPARAM result);      // WM_APP_MAINT
void Maint_Refresh();                        // the Overview list again (after a Canivete update check)

// ---- update.cpp ----------------------------------------------------------------------------
struct UpdateInfo {
    bool checked = false;    // GitHub answered
    bool available = false;  // a newer, downloadable and verifiable version
    std::wstring latest;     // "0.3.0"
    std::wstring url, sha;   // setup download and its SHA-256 (hex)
    std::wstring error;
};
const UpdateInfo& Update_Info();
bool Update_Busy();
void Update_Check(bool manual);  // manual: always says what it found
void Update_Background();        // once a day when the setting is on
long long Update_SecondsToNext();  // until the next background check is due (LLONG_MAX = never)
bool Net_Online();               // Windows says there is internet (true when it can't tell)
void Update_Install();           // asks, downloads, checks, runs the setup and exits
void Update_OnMessage(WPARAM wp, LPARAM lp);
int Update_SelfTest();           // --update-test: download and check the latest release, install nothing
int Update_VerifyTest(int argc, wchar_t** argv);  // --verify-update <version> <sha256> <sig>: 0 = valid

// ---- theme.cpp: GDI+ is started once for the whole process ------------------------------------
void Gdiplus_Ensure();
void Gdiplus_Shutdown();  // at exit, after all windows are gone

// ---- 32-bit top-down DIB section with its own memory DC --------------------------------------
struct Dib {
    HDC dc = nullptr;
    HBITMAP bmp = nullptr, old = nullptr;
    uint32_t* bits = nullptr;  // BGRX, top-down
    int w = 0, h = 0;

    bool Create(int W, int H) {
        Free();
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(bi.bmiHeader);
        bi.bmiHeader.biWidth = W;
        bi.bmiHeader.biHeight = -H;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        dc = CreateCompatibleDC(nullptr);
        bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), nullptr, 0);
        if (!bmp) {
            DeleteDC(dc);
            dc = nullptr;
            bits = nullptr;
            return false;
        }
        old = static_cast<HBITMAP>(SelectObject(dc, bmp));
        w = W;
        h = H;
        return true;
    }
    void Free() {
        if (dc) {
            SelectObject(dc, old);
            DeleteObject(bmp);
            DeleteDC(dc);
        }
        dc = nullptr;
        bmp = old = nullptr;
        bits = nullptr;
        w = h = 0;
    }
    Dib() = default;
    Dib(const Dib&) = delete;
    Dib& operator=(const Dib&) = delete;
    ~Dib() { Free(); }
};

// ---- capture.cpp ---------------------------------------------------------------------------
struct Image {
    int w = 0, h = 0;
    std::vector<uint32_t> px;  // BGRX, top-down; alpha ignored unless `alpha`
    bool alpha = false;        // px is BGRA, straight alpha: PNG keeps it, JPG and the clipboard DIB get white behind
};

// A pixel of an alpha image over white (for JPG and the clipboard's DIB).
inline uint32_t OverWhite(uint32_t v) {
    const uint32_t a = v >> 24, k = 255 - a;
    auto ch = [&](int s) { return (((v >> s) & 255) * a + 255 * k + 127) / 255; };
    return (ch(16) << 16) | (ch(8) << 8) | ch(0);
}

struct CaptureResult {
    Image img;
    RECT screenRect{};          // where it was on screen (physical pixels)
    std::wstring processName;   // app that was in front when the hotkey was pressed
    std::wstring windowTitle;
    bool autoUpload = false;  // the editor's Done should also upload (auto mode or its Upload button)
};

void Capture_Init();  // creates the (hidden) overlay window up front so the first capture is instant
void Capture_Start(CaptureMode mode);
bool Capture_IsActive();
RECT VirtualScreenRect();
bool GrabScreen(const RECT& r, Image& out, bool withCursor);
void DrawCursorAt(HDC dc, int ox, int oy);  // the mouse pointer, for a grab whose top-left is ox, oy

// ---- output.cpp ----------------------------------------------------------------------------
void Output_Init();
void Output_Process(std::unique_ptr<CaptureResult> r);  // async: encode, save, clipboard, toast
void Output_OnDone(LPARAM job);                          // main thread part
void Output_ShowColor(COLORREF c, POINT at, const std::wstring& line1, const std::wstring& line2);
void Output_CopyImage(const Image& img);  // DIB + PNG on the clipboard
bool LoadImageFile(const std::wstring& path, Image& out);
// Saves over `path` (format from its extension), or to a new file name when `path` is empty.
// Returns the path written, "" on failure.
std::wstring Output_SaveImage(const Image& img, const CaptureResult& meta, const std::wstring& path);
std::wstring Output_NewPath(const CaptureResult& meta, const wchar_t* ext);  // free name in the screenshots folder
void Output_EditDone(std::shared_ptr<CaptureResult> r, const std::wstring& path);  // copy, save, preview

// ---- editor.cpp ----------------------------------------------------------------------------
void Editor_Open(std::shared_ptr<CaptureResult> r, const std::wstring& path);
std::vector<BYTE> EncodePng(const Image& img);
std::vector<BYTE> EncodeJpeg(const Image& img, int quality);
std::wstring PreviewFileName(const std::wstring& pattern);  // example name for the Settings window
std::wstring ScreenshotsDir();
bool WriteFileBytes(const std::wstring& path, const std::vector<BYTE>& data);

// ---- gif.cpp -------------------------------------------------------------------------------
struct GifPreset {
    const wchar_t* key;    // in settings.ini
    const wchar_t* title;  // on the button
    const wchar_t* note;
    int fps, colors, tolerance;  // tolerance: color change (0-255) still counted as "the same"
    bool dither;
};
constexpr int kGifPresetCount = 3;
extern const GifPreset kGifPresets[kGifPresetCount];

// Writes a recording: each frame stores only the 32x32 tiles that changed.
class GifWriter {
public:
    bool Open(const std::wstring& path, int width, int height);
    void Frame(uint32_t timeMs, const uint32_t* px);  // width*height BGRX
    bool Close(uint32_t timeMs);                      // the stop time
    uint64_t Bytes() const { return bytes_; }
    ~GifWriter();

private:
    FILE* f_ = nullptr;
    int w_ = 0, h_ = 0;
    uint64_t bytes_ = 0;
    std::vector<uint32_t> prev_;
};

// fromMs / toMs: only that part of the recording (trim), in recording time.
bool Gif_Encode(const std::wstring& recording, const GifPreset& p, const std::wstring& outPath,
                const std::atomic<bool>& cancel, std::wstring& err, uint32_t fromMs = 0, uint32_t toMs = UINT32_MAX);

// ---- video.cpp -----------------------------------------------------------------------------
// MP4 (H.264) writer. Width and height must be even. Frames are BGRX, top-down.
class Mp4Writer {
public:
    struct Impl;
    Mp4Writer();
    ~Mp4Writer();
    // audio: add a sound track (fed with Audio). info: the encoder used.
    // quality: the encoder's 1-100 quality (or a bitrate scaled to it); audioKbps: AAC bitrate.
    bool Open(const std::wstring& path, int width, int height, int fps, bool audio, std::wstring& info,
              int quality = 70, int audioKbps = 192);
    bool Frame(LONGLONG time100ns, const uint32_t* px);
    bool Audio(LONGLONG time100ns, const int16_t* frames, UINT32 count);  // 48 kHz stereo 16-bit
    bool HasAudio() const;
    bool Close();  // finishes the file

private:
    std::unique_ptr<Impl> p_;
};

// The choices offered after a video recording. The last one keeps the recording as it is.
struct VideoPreset {
    const wchar_t* key;    // in settings.ini
    const wchar_t* title;  // on the button
    const wchar_t* note;
    int percent;           // picture size, % of the recorded one (100 = as recorded)
    int quality;           // encoder quality 1-100 (0 = keep the recording, no re-encode)
    int audioKbps;
};
constexpr int kVideoPresetCount = 3;
extern const VideoPreset kVideoPresets[kVideoPresetCount];

// A recording with lots of motion (judged by its size): only those get a smaller picture.
bool Mp4_IsBusy(uint64_t bytes, uint32_t ms, int w, int h);
// The picture size a preset gives: busy recordings percent of w x h (never below 640 on the long
// side, even); calm ones keep their size.
void Mp4_ShrinkSize(int w, int h, int percent, bool busy, int& outW, int& outH);
// Re-encodes a recorded MP4 smaller (size and/or quality), sound included. Runs on any thread.
// from100 / to100: keep only the frames from..to (100 ns units, trim), sound cut to match.
bool Mp4_Shrink(const std::wstring& src, const VideoPreset& p, const std::wstring& outPath,
                const std::atomic<bool>& cancel, std::wstring& err, LONGLONG from100 = 0, LONGLONG to100 = LLONG_MAX);

// ---- audio.cpp -----------------------------------------------------------------------------
// Sound for videos: computer sound (loopback) and/or the microphone, mixed, 48 kHz stereo 16-bit.
class AudioCapture {
public:
    struct Impl;
    using Writer = std::function<void(LONGLONG time100ns, const int16_t* frames, UINT32 count)>;
    AudioCapture();
    ~AudioCapture();
    bool Open(bool desktop, bool mic, std::wstring& info);  // false: nothing to record (info says why)
    // Records until stop, calling write on this thread. start100ns: QPC time (100 ns) of video frame 0.
    void Run(LONGLONG start100ns, const std::atomic<bool>& stop, const Writer& write);
    // Recording paused / resumed at this QPC time (100 ns): sound from the pause is left out and the
    // rest moves up, so it stays in step with the picture. Called from another thread.
    void Pause(LONGLONG at100ns);
    void Resume(LONGLONG at100ns);
    uint64_t Frames() const;
    uint64_t Padded() const;  // silence added for gaps
    uint64_t Cut() const;     // overlapping frames dropped

private:
    std::unique_ptr<Impl> p_;
};

// ---- recorder.cpp --------------------------------------------------------------------------
void Recorder_Start(const RECT& r, bool video = false);  // records that screen area until Recorder_Stop
bool Recorder_Active();              // recording, or its save window is open
void Recorder_Stop();                // stop recording (or bring the save window forward)
void Recorder_Shutdown();            // app is closing: stop and throw away anything in progress
HWND Recorder_Window();              // the save window, for IsDialogMessage
void Recorder_Test(const RECT& r, int ms, bool video);  // test mode: record r for ms, then save
int Recorder_BenchVideo(const RECT& r, int ms, const std::wstring& outPath);  // headless MP4 test
int Recorder_BenchShrink(const std::wstring& in, const std::wstring& out, const VideoPreset& p);  // logs size
int Recorder_Bench(const RECT& r, int ms, const std::wstring& outDir);  // headless: record + all presets

// ---- overlay.cpp ---------------------------------------------------------------------------
// "Show keys and clicks": key presses (a pill near the bottom) and mouse clicks (a ring) drawn into
// the recorded frames. Input comes from low-level hooks on the main thread (never in test mode).
void Overlay_Begin(const RECT& area, bool allKeys, UINT dpi);  // a recording starts (hooks still off)
void Overlay_SetOn(bool on);      // main thread: hooks on / off
bool Overlay_On();
void Overlay_Ignore(const Hotkey& hk);  // Canivete's own hotkeys are never shown
void Overlay_Paused(bool paused);  // input while paused is dropped; resuming starts clean
void Overlay_End();               // hooks off, everything cleared
void Overlay_Key(UINT vk, bool down);           // what the keyboard hook passes on (tests call it too)
void Overlay_Click(POINT screen, bool right);   // a button press at this screen point
void Overlay_Draw(uint32_t* px, int w, int h);  // capture thread: draws onto a BGRX frame (if anything)
std::wstring Overlay_KeyLabel(UINT vk, UINT mods, bool allKeys);  // "Ctrl+B", "" = not shown (tests)

// ---- trim.cpp ------------------------------------------------------------------------------
// The save window's timeline: a preview picture and a start and an end handle.
struct TrimThumb {
    uint32_t ms = 0;  // recording time of this picture
    Image img;        // small copy
};
constexpr WORD kTrimChanged = 0x4001;  // WM_COMMAND notification: the user let go of a handle
// fps: the handles move a frame at a time and land on frame boundaries (0 = 50 ms steps).
HWND Trim_Create(HWND parent, int id, int x, int y, int w, int h, const std::vector<TrimThumb>* thumbs,
                 uint32_t durationMs, int fps, UINT dpi);
void Trim_Get(HWND trim, uint32_t& fromMs, uint32_t& toMs);
void Trim_Set(HWND trim, uint32_t fromMs, uint32_t toMs);
uint32_t Trim_SnapMs(uint32_t ms, int fps);  // to the nearest frame boundary

// ---- alpha.cpp -----------------------------------------------------------------------------
void Alpha_Open(const Image& img, const CaptureResult& meta);  // "Make alpha" window for this picture
HWND Alpha_Window();
// Headless: picks = detail clicks (image pixels), box = area to use (empty = all). Writes the PNG.
int Alpha_Bench(const std::wstring& in, const std::wstring& out, const std::vector<POINT>& picks, const RECT& box);

// ---- rmbg.cpp ------------------------------------------------------------------------------
// Remove background with a local AI model (downloaded on first use); saves a transparent PNG.
void Rmbg_Files(const std::vector<std::wstring>& files);  // each saved next to the original
void Rmbg_Image(const Image& img, const CaptureResult& meta, HWND notify);  // editor: saved, copied, WM_APP_NOBG to notify
int Rmbg_Bench(const std::wstring& in, const std::wstring& out);  // headless: timings in the log
