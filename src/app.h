// Canivete - shared declarations.
#pragma once
#include <windows.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

constexpr wchar_t kAppName[] = L"Canivete";
constexpr wchar_t kMainClass[] = L"CaniveteMain";

// Messages to the main (hidden) window.
constexpr UINT WM_APP_TRAY = WM_APP + 1;          // tray icon callback
constexpr UINT WM_APP_CAPTURE_DONE = WM_APP + 2;  // lParam = Output job, finished on a worker
constexpr UINT WM_APP_COMMAND = WM_APP + 3;       // wParam = command id (from a second instance)

enum class CaptureMode { Region = 1, Window = 2, Full = 3, Color = 4 };  // also the hotkey ids
constexpr int kHotkeyCount = 4;

// ---- settings.cpp --------------------------------------------------------------------------
struct Hotkey {
    UINT mods = 0;
    UINT vk = 0;
};

struct Settings {
    std::wstring hkRegion, hkWindow, hkFull, hkColor;
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
};

extern Settings g_settings;
extern HINSTANCE g_hinst;
extern HWND g_mainWnd;

std::wstring AppDataDir();
std::wstring SettingsPath();
void LoadSettings();
void SaveSettings(const Settings& s);
bool ParseHotkey(const std::wstring& text, Hotkey& out);
std::wstring HotkeyText(const Hotkey& hk);  // "Ctrl+Shift+PrintScreen", "" when none
void Log(const wchar_t* fmt, ...);

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

// ---- resizer.cpp ---------------------------------------------------------------------------
void Resizer_Open(const std::vector<std::wstring>& files);  // opens the window, or adds to it
HWND Resizer_Window();                                       // for IsDialogMessage, nullptr when closed

// ---- renamer.cpp ---------------------------------------------------------------------------
void Renamer_Open(const std::vector<std::wstring>& paths);
HWND Renamer_Window();

// ---- shell.cpp -----------------------------------------------------------------------------
void Shell_Register();  // Explorer right-click and Send to entries for the resizer and renamer

// ---- settings_ui.cpp -----------------------------------------------------------------------
void SettingsUI_Show();
HWND SettingsUI_Window();  // for IsDialogMessage, nullptr when closed

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
    std::vector<uint32_t> px;  // BGRX, top-down, alpha ignored
};

struct CaptureResult {
    Image img;
    RECT screenRect{};          // where it was on screen (physical pixels)
    std::wstring processName;   // app that was in front when the hotkey was pressed
    std::wstring windowTitle;
};

void Capture_Init();  // creates the (hidden) overlay window up front so the first capture is instant
void Capture_Start(CaptureMode mode);
bool Capture_IsActive();
RECT VirtualScreenRect();
bool GrabScreen(const RECT& r, Image& out, bool withCursor);

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
void Output_EditDone(std::shared_ptr<CaptureResult> r, const std::wstring& path);  // copy, save, preview

// ---- editor.cpp ----------------------------------------------------------------------------
void Editor_Open(std::shared_ptr<CaptureResult> r, const std::wstring& path);
std::vector<BYTE> EncodePng(const Image& img);
std::vector<BYTE> EncodeJpeg(const Image& img, int quality);
std::wstring PreviewFileName(const std::wstring& pattern);  // example name for the Settings window
std::wstring ScreenshotsDir();
bool WriteFileBytes(const std::wstring& path, const std::vector<BYTE>& data);
