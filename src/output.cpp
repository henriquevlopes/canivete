// What happens after a capture: PNG/JPG encode (WIC) + save on a worker thread, then clipboard and a
// small "saved" notification with a thumbnail on the main thread.
#include "app.h"

#include <shellapi.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

struct Job {
    std::unique_ptr<CaptureResult> r;
    std::vector<BYTE> png;  // for the clipboard (empty when not copying)
    std::wstring path;      // empty if not saved
};

// ---- file names ----------------------------------------------------------------------------

std::wstring Sanitize(std::wstring s, size_t maxLen) {
    for (auto& c : s)
        if (c < 32 || wcschr(L"<>:\"/\\|?*", c)) c = L'_';
    if (s.size() > maxLen) s.resize(maxLen);
    while (!s.empty() && (s.back() == L' ' || s.back() == L'.')) s.pop_back();
    return s;
}

std::wstring ExpandPattern(const std::wstring& pat, const CaptureResult& r, const SYSTEMTIME& t) {
    auto two = [](int v) {
        wchar_t b[8];
        swprintf_s(b, L"%02d", v);
        return std::wstring(b);
    };
    wchar_t ms[8];
    swprintf_s(ms, L"%03d", t.wMilliseconds);
    // Longest tokens first so %mo is not read as %m + o etc.
    const std::pair<const wchar_t*, std::wstring> tokens[] = {
        {L"%height", std::to_wstring(r.img.h)},
        {L"%width", std::to_wstring(r.img.w)},
        {L"%ms", ms},
        {L"%mi", two(t.wMinute)},
        {L"%mo", two(t.wMonth)},
        {L"%pn", Sanitize(r.processName.empty() ? L"Desktop" : r.processName, 40)},
        {L"%yy", two(t.wYear % 100)},
        {L"%y", std::to_wstring(t.wYear)},
        {L"%d", two(t.wDay)},
        {L"%h", two(t.wHour)},
        {L"%s", two(t.wSecond)},
        {L"%t", Sanitize(r.windowTitle.empty() ? L"Screen" : r.windowTitle, 60)},
    };
    std::wstring out;
    for (size_t i = 0; i < pat.size();) {
        bool hit = false;
        if (pat[i] == L'%')
            for (const auto& [tok, val] : tokens) {
                const size_t n = wcslen(tok);
                if (pat.compare(i, n, tok) == 0) {
                    out += val;
                    i += n;
                    hit = true;
                    break;
                }
            }
        if (!hit) out += pat[i++];
    }
    // The pattern itself may contain \ for subfolders; clean each part.
    std::wstring clean, part;
    for (wchar_t c : out + L'\\') {
        if (c == L'\\' || c == L'/') {
            part = Sanitize(part, 120);
            if (!part.empty()) clean += (clean.empty() ? L"" : L"\\") + part;
            part.clear();
        } else {
            part += c;
        }
    }
    return clean.empty() ? L"Screenshot" : clean;
}

std::wstring UniquePath(const std::wstring& base, const std::wstring& ext) {
    std::wstring p = base + ext;
    for (int i = 2; GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES && i < 1000; ++i)
        p = base + L" (" + std::to_wstring(i) + L")" + ext;
    return p;
}

// One WIC encoder for both formats. quality < 0 = PNG, otherwise JPG quality 1..100.
std::vector<BYTE> Encode(const Image& img, int quality) {
    const bool jpg = quality >= 0;
    std::vector<BYTE> out;
    ComPtr<IWICImagingFactory> f;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))) return out;
    ComPtr<IStream> st;
    st.Attach(SHCreateMemStream(nullptr, 0));
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> frame;
    ComPtr<IPropertyBag2> props;
    HRESULT hr =
        st ? f->CreateEncoder(jpg ? GUID_ContainerFormatJpeg : GUID_ContainerFormatPng, nullptr, &enc) : E_OUTOFMEMORY;
    if (SUCCEEDED(hr)) hr = enc->Initialize(st.Get(), WICBitmapEncoderNoCache);
    if (SUCCEEDED(hr)) hr = enc->CreateNewFrame(&frame, &props);
    if (SUCCEEDED(hr) && jpg) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = std::clamp(quality, 1, 100) / 100.0f;
        props->Write(1, &opt, &v);
    }
    if (SUCCEEDED(hr)) hr = frame->Initialize(props.Get());
    if (SUCCEEDED(hr)) hr = frame->SetSize(img.w, img.h);
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != GUID_WICPixelFormat24bppBGR) hr = E_FAIL;
    if (SUCCEEDED(hr)) {
        const UINT stride = (img.w * 3 + 3) & ~3u;
        std::vector<BYTE> rows(static_cast<size_t>(stride) * img.h);
        for (int y = 0; y < img.h; ++y) {
            const uint32_t* s = &img.px[static_cast<size_t>(y) * img.w];
            BYTE* d = &rows[static_cast<size_t>(y) * stride];
            for (int x = 0; x < img.w; ++x) {
                d[x * 3 + 0] = static_cast<BYTE>(s[x]);
                d[x * 3 + 1] = static_cast<BYTE>(s[x] >> 8);
                d[x * 3 + 2] = static_cast<BYTE>(s[x] >> 16);
            }
        }
        hr = frame->WritePixels(img.h, stride, static_cast<UINT>(rows.size()), rows.data());
    }
    if (SUCCEEDED(hr)) hr = frame->Commit();
    if (SUCCEEDED(hr)) hr = enc->Commit();
    if (SUCCEEDED(hr)) {
        STATSTG stat{};
        st->Stat(&stat, STATFLAG_NONAME);
        out.resize(static_cast<size_t>(stat.cbSize.QuadPart));
        LARGE_INTEGER zero{};
        st->Seek(zero, STREAM_SEEK_SET, nullptr);
        ULONG read = 0;
        st->Read(out.data(), static_cast<ULONG>(out.size()), &read);
        out.resize(read);
    } else {
        Log(L"%s encode failed 0x%08X", jpg ? L"JPG" : L"PNG", hr);
    }
    return out;
}

// ---- clipboard -----------------------------------------------------------------------------

void SetClipboardImage(const Image& img, const std::vector<BYTE>& png) {
    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(g_mainWnd)); ++i) Sleep(15);
    if (!open) {
        Log(L"Clipboard busy, image not copied");
        return;
    }
    EmptyClipboard();
    // CF_DIB, 24-bit bottom-up: every app understands it, and no alpha to misinterpret.
    const UINT stride = (img.w * 3 + 3) & ~3u;
    const SIZE_T size = sizeof(BITMAPINFOHEADER) + static_cast<SIZE_T>(stride) * img.h;
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, size)) {
        auto* p = static_cast<BYTE*>(GlobalLock(g));
        auto* bh = reinterpret_cast<BITMAPINFOHEADER*>(p);
        *bh = {};
        bh->biSize = sizeof(BITMAPINFOHEADER);
        bh->biWidth = img.w;
        bh->biHeight = img.h;
        bh->biPlanes = 1;
        bh->biBitCount = 24;
        bh->biCompression = BI_RGB;
        bh->biSizeImage = stride * img.h;
        BYTE* rows = p + sizeof(BITMAPINFOHEADER);
        for (int y = 0; y < img.h; ++y) {
            const uint32_t* s = &img.px[static_cast<size_t>(img.h - 1 - y) * img.w];
            BYTE* d = rows + static_cast<size_t>(y) * stride;
            for (int x = 0; x < img.w; ++x) {
                d[x * 3 + 0] = static_cast<BYTE>(s[x]);
                d[x * 3 + 1] = static_cast<BYTE>(s[x] >> 8);
                d[x * 3 + 2] = static_cast<BYTE>(s[x] >> 16);
            }
        }
        GlobalUnlock(g);
        if (!SetClipboardData(CF_DIB, g)) GlobalFree(g);
    }
    // Also PNG: browsers, Discord, Office and Photoshop prefer it.
    static const UINT cfPng = RegisterClipboardFormatW(L"PNG");
    if (!png.empty())
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, png.size())) {
            memcpy(GlobalLock(g), png.data(), png.size());
            GlobalUnlock(g);
            if (!SetClipboardData(cfPng, g)) GlobalFree(g);
        }
    CloseClipboard();
}

// ---- notification toast --------------------------------------------------------------------

constexpr wchar_t kToastClass[] = L"CaniveteToast";
constexpr UINT_PTR kTimerHold = 1, kTimerFade = 2;
constexpr int kAlphaFull = 245;

struct Toast {
    HWND hwnd = nullptr;
    Dib thumb;
    RECT thumbRect{};
    std::wstring path, line1, line2;
    double scale = 1;
    int alpha = 0;
} T;

void ToastHide() {
    KillTimer(T.hwnd, kTimerHold);
    KillTimer(T.hwnd, kTimerFade);
    ShowWindow(T.hwnd, SW_HIDE);
    T.thumb.Free();
}

void ToastPaint(HDC dc) {
    RECT rc;
    GetClientRect(T.hwnd, &rc);
    SetDCBrushColor(dc, RGB(32, 32, 32));
    FillRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetDCBrushColor(dc, RGB(80, 80, 80));
    FrameRect(dc, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if (T.thumb.dc)
        BitBlt(dc, T.thumbRect.left, T.thumbRect.top, T.thumb.w, T.thumb.h, T.thumb.dc, 0, 0, SRCCOPY);
    const int pad = static_cast<int>(std::lround(12 * T.scale));
    const int lineH = static_cast<int>(std::lround(20 * T.scale));
    HFONT bold = CreateFontW(-static_cast<int>(std::lround(14 * T.scale)), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0,
                             DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    HFONT normal = CreateFontW(-static_cast<int>(std::lround(13 * T.scale)), 0, 0, 0, FW_NORMAL, 0, 0, 0,
                               DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    SetBkMode(dc, TRANSPARENT);
    RECT r1{pad, T.thumbRect.bottom + pad / 2, rc.right - pad, T.thumbRect.bottom + pad / 2 + lineH};
    HGDIOBJ old = SelectObject(dc, bold);
    SetTextColor(dc, RGB(255, 255, 255));
    DrawTextW(dc, T.line1.c_str(), -1, &r1, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT r2{pad, r1.bottom, rc.right - pad, r1.bottom + lineH};
    SelectObject(dc, normal);
    SetTextColor(dc, RGB(180, 180, 180));
    DrawTextW(dc, T.line2.c_str(), -1, &r2, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(dc, old);
    DeleteObject(bold);
    DeleteObject(normal);
}

LRESULT CALLBACK ToastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        ToastPaint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_MOUSEMOVE:  // keep it up while the mouse is on it
        KillTimer(h, kTimerFade);
        T.alpha = kAlphaFull;
        SetLayeredWindowAttributes(h, 0, static_cast<BYTE>(T.alpha), LWA_ALPHA);
        SetTimer(h, kTimerHold, 2500, nullptr);
        return 0;
    case WM_LBUTTONUP:
        if (!T.path.empty()) ShellExecuteW(nullptr, L"open", T.path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        ToastHide();
        return 0;
    case WM_RBUTTONUP:
        if (!T.path.empty()) {
            std::wstring args = L"/select,\"" + T.path + L"\"";
            ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
        }
        ToastHide();
        return 0;
    case WM_TIMER:
        if (wp == kTimerHold) {
            KillTimer(h, kTimerHold);
            SetTimer(h, kTimerFade, 16, nullptr);
        } else if (wp == kTimerFade) {
            T.alpha -= 20;
            if (T.alpha <= 0) ToastHide();
            else SetLayeredWindowAttributes(h, 0, static_cast<BYTE>(T.alpha), LWA_ALPHA);
        }
        return 0;
    case WM_DPICHANGED:
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void ToastShow(const CaptureResult& r, const std::wstring& path, bool copied) {
    HMONITOR mon = MonitorFromRect(&r.screenRect, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    UINT dpi = 96, dy;
    GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dpi, &dy);
    T.scale = dpi / 96.0;
    const double s = T.scale;
    const int pad = static_cast<int>(std::lround(12 * s));
    const int W = static_cast<int>(std::lround(300 * s));
    const int maxTh = static_cast<int>(std::lround(170 * s));
    const int innerW = W - 2 * pad;
    const double k = std::min({1.0, static_cast<double>(innerW) / r.img.w, static_cast<double>(maxTh) / r.img.h});
    const int tw = std::max(1, static_cast<int>(r.img.w * k)), th = std::max(1, static_cast<int>(r.img.h * k));

    // Thumbnail: copy into a DIB, then high-quality downscale with HALFTONE.
    T.thumb.Free();
    Dib src;
    if (src.Create(r.img.w, r.img.h) && T.thumb.Create(tw, th)) {
        memcpy(src.bits, r.img.px.data(), r.img.px.size() * sizeof(uint32_t));
        SetStretchBltMode(T.thumb.dc, HALFTONE);
        SetBrushOrgEx(T.thumb.dc, 0, 0, nullptr);
        StretchBlt(T.thumb.dc, 0, 0, tw, th, src.dc, 0, 0, r.img.w, r.img.h, SRCCOPY);
    }
    const int lineH = static_cast<int>(std::lround(20 * s));
    const int H = pad + th + pad / 2 + 2 * lineH + pad;
    T.thumbRect = {pad + (innerW - tw) / 2, pad, pad + (innerW - tw) / 2 + tw, pad + th};
    T.path = path;
    T.line1 = copied && !path.empty() ? L"Copied and saved" : copied ? L"Copied to clipboard" : L"Saved";
    wchar_t sz[64];
    swprintf_s(sz, L"%d × %d", r.img.w, r.img.h);
    T.line2 = path.empty() ? std::wstring(sz) : std::wstring(sz) + L"   " + PathFindFileNameW(path.c_str());

    const int margin = static_cast<int>(std::lround(16 * s));
    const int x = mi.rcWork.right - W - margin, y = mi.rcWork.bottom - H - margin;
    KillTimer(T.hwnd, kTimerFade);
    T.alpha = kAlphaFull;
    SetLayeredWindowAttributes(T.hwnd, 0, static_cast<BYTE>(T.alpha), LWA_ALPHA);
    SetWindowPos(T.hwnd, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(T.hwnd, nullptr, FALSE);
    SetTimer(T.hwnd, kTimerHold, 2500, nullptr);
}

}  // namespace

// ---- public --------------------------------------------------------------------------------

void Output_ShowColor(COLORREF c, POINT at, const std::wstring& line1, const std::wstring& line2) {
    CaptureResult r;
    r.img.w = 600;
    r.img.h = 120;
    r.img.px.assign(static_cast<size_t>(r.img.w) * r.img.h,
                    (static_cast<uint32_t>(GetRValue(c)) << 16) | (GetGValue(c) << 8) | GetBValue(c));
    r.screenRect = {at.x, at.y, at.x + 1, at.y + 1};
    ToastShow(r, L"", false);
    T.line1 = line1;
    T.line2 = line2;
    InvalidateRect(T.hwnd, nullptr, FALSE);
}

std::vector<BYTE> EncodePng(const Image& img) { return Encode(img, -1); }

std::vector<BYTE> EncodeJpeg(const Image& img, int quality) { return Encode(img, quality); }

std::wstring PreviewFileName(const std::wstring& pattern) {
    CaptureResult r;
    r.img.w = 1920;
    r.img.h = 1080;
    r.processName = L"firefox";
    r.windowTitle = L"Canivete - GitHub";
    SYSTEMTIME t;
    GetLocalTime(&t);
    return ExpandPattern(pattern, r, t);
}


std::wstring ScreenshotsDir() {
    if (!g_settings.folder.empty()) {
        wchar_t buf[MAX_PATH * 2];
        ExpandEnvironmentStringsW(g_settings.folder.c_str(), buf, MAX_PATH * 2);
        return buf;
    }
    PWSTR p = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Screenshots, KF_FLAG_CREATE, nullptr, &p))) dir = p;
    CoTaskMemFree(p);
    p = nullptr;
    if (dir.empty() && SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &p)))
        dir = std::wstring(p) + L"\\Screenshots";
    CoTaskMemFree(p);
    return dir;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<BYTE>& data) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD n = 0;
    const BOOL ok = WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &n, nullptr);
    CloseHandle(f);
    return ok && n == data.size();
}

void Output_Init() {
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = ToastProc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = kToastClass;
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    RegisterClassExW(&wc);
    T.hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED, kToastClass,
                             L"Canivete", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, g_hinst, nullptr);
    // Never let the notification itself end up in the next screenshot.
    SetWindowDisplayAffinity(T.hwnd, WDA_EXCLUDEFROMCAPTURE);
}

void Output_Process(std::unique_ptr<CaptureResult> r) {
    const Settings s = g_settings;
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::thread([r = std::move(r), s, t]() mutable {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto job = std::make_unique<Job>();
        const ULONGLONG t0 = GetTickCount64();
        const bool jpg = s.format == L"jpg";
        if (s.copyToClipboard || (s.saveToFile && !jpg)) job->png = EncodePng(r->img);
        if (s.saveToFile) {
            const std::vector<BYTE> jpgData = jpg ? EncodeJpeg(r->img, s.jpegQuality) : std::vector<BYTE>();
            const std::vector<BYTE>& data = jpg ? jpgData : job->png;
            const std::wstring dir = ScreenshotsDir();
            const std::wstring base = dir + L"\\" + ExpandPattern(s.fileName, *r, t);
            const std::wstring folder = base.substr(0, base.find_last_of(L'\\'));
            SHCreateDirectoryExW(nullptr, folder.c_str(), nullptr);
            const std::wstring path = UniquePath(base, jpg ? L".jpg" : L".png");
            if (data.empty()) Log(L"Nothing to save, encode failed");
            else if (WriteFileBytes(path, data)) job->path = path;
            else Log(L"Could not write %s (error %lu)", path.c_str(), GetLastError());
        }
        Log(L"Encoded %dx%d in %llu ms -> %s", r->img.w, r->img.h, GetTickCount64() - t0,
            job->path.empty() ? L"(not saved)" : job->path.c_str());
        job->r = std::move(r);
        if (!PostMessageW(g_mainWnd, WM_APP_CAPTURE_DONE, 0, reinterpret_cast<LPARAM>(job.get()))) {
            CoUninitialize();
            return;
        }
        job.release();
        CoUninitialize();
    }).detach();
}

void Output_OnDone(LPARAM lp) {
    std::unique_ptr<Job> job(reinterpret_cast<Job*>(lp));
    if (g_settings.copyToClipboard) SetClipboardImage(job->r->img, job->png);
    if (g_settings.showNotification) ToastShow(*job->r, job->path, g_settings.copyToClipboard);
}
