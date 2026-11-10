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

enum class CaptureMode { Region = 1, Window = 2, Full = 3 };

// ---- settings.cpp --------------------------------------------------------------------------
struct Hotkey {
    UINT mods = 0;
    UINT vk = 0;
};

struct Settings {
    std::wstring hkRegion, hkWindow, hkFull;
    std::wstring folder;    // empty = Windows "Screenshots" folder
    std::wstring fileName;  // pattern with %tokens
    std::wstring fullScreen;  // "all" or "monitor"
    std::wstring format;      // "png" or "jpg"
    int jpegQuality = 90;     // 1..100
    bool saveToFile = true;
    bool copyToClipboard = true;
    bool includeCursor = false;
    bool showNotification = true;
    bool showMagnifier = true;
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

// ---- main.cpp ------------------------------------------------------------------------------
std::wstring Settings_Applied();     // reload after a save; returns hotkey problems ("" = all fine)
void Hotkeys_Suspend(bool suspend);  // while a hotkey box in Settings has focus
bool AutostartEnabled();
void SetAutostart(bool on);

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
std::vector<BYTE> EncodePng(const Image& img);
std::vector<BYTE> EncodeJpeg(const Image& img, int quality);
std::wstring PreviewFileName(const std::wstring& pattern);  // example name for the Settings window
std::wstring ScreenshotsDir();
bool WriteFileBytes(const std::wstring& path, const std::vector<BYTE>& data);
