// What happens after a capture: PNG/JPG encode (WIC) + save on a worker thread, then clipboard and a
// small "saved" notification with a thumbnail on the main thread.
#include "app.h"
#include "fluent.h"
#include "theme.h"

#include <ole2.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <windowsx.h>
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
    const bool keepAlpha = img.alpha && !jpg;  // a transparent PNG: 32-bit with its alpha
    const GUID want = keepAlpha ? GUID_WICPixelFormat32bppBGRA : GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID fmt = want;
    if (SUCCEEDED(hr)) hr = frame->SetPixelFormat(&fmt);
    if (SUCCEEDED(hr) && fmt != want) hr = E_FAIL;
    if (SUCCEEDED(hr) && keepAlpha) {
        hr = frame->WritePixels(img.h, img.w * 4, static_cast<UINT>(img.px.size() * 4),
                                reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.px.data())));
    } else if (SUCCEEDED(hr)) {
        const UINT stride = (img.w * 3 + 3) & ~3u;
        std::vector<BYTE> rows(static_cast<size_t>(stride) * img.h);
        for (int y = 0; y < img.h; ++y) {
            const uint32_t* s = &img.px[static_cast<size_t>(y) * img.w];
            BYTE* d = &rows[static_cast<size_t>(y) * stride];
            for (int x = 0; x < img.w; ++x) {
                const uint32_t v = img.alpha ? OverWhite(s[x]) : s[x];
                d[x * 3 + 0] = static_cast<BYTE>(v);
                d[x * 3 + 1] = static_cast<BYTE>(v >> 8);
                d[x * 3 + 2] = static_cast<BYTE>(v >> 16);
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
                const uint32_t v = img.alpha ? OverWhite(s[x]) : s[x];
                d[x * 3 + 0] = static_cast<BYTE>(v);
                d[x * 3 + 1] = static_cast<BYTE>(v >> 8);
                d[x * 3 + 2] = static_cast<BYTE>(v >> 16);
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
    std::shared_ptr<CaptureResult> last;  // the capture shown, so a click can open it in the editor
    double scale = 1;
    int alpha = 0;
    bool canUpload = false;  // shows the Upload button
    POINT down{};
    bool pressed = false;
    bool overUpload = false;
    RECT uploadRect{};
} T;

void UploadShown() {
    std::shared_ptr<CaptureResult> last = T.last;
    if (!T.path.empty()) Upload_File(T.path, false);
    else if (last) Upload_Image(last->img, *last);
}

void ToastHide() {
    KillTimer(T.hwnd, kTimerHold);
    KillTimer(T.hwnd, kTimerFade);
    ShowWindow(T.hwnd, SW_HIDE);
    T.thumb.Free();
    T.last.reset();
}

void ToastPaint(HDC dc) {
    RECT rc;
    GetClientRect(T.hwnd, &rc);
    const Palette& p = Pal();
    const UINT dpi = static_cast<UINT>(std::lround(96 * T.scale));
    // Drawn off screen, then copied: no flicker when the Upload button changes.
    HDC mem = CreateCompatibleDC(dc);
    HBITMAP bmp = CreateCompatibleBitmap(dc, rc.right, rc.bottom);
    HGDIOBJ oldBmp = SelectObject(mem, bmp);
    // Windows 11 flyout colors: the card color in dark mode, the window color in light mode.
    const COLORREF bg = p.dark ? p.card : p.window;
    SetDCBrushColor(mem, bg);
    FillRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    if (!Fluent_IsWin11() || p.highContrast) {  // Windows 11 draws the thin border itself
        SetDCBrushColor(mem, p.highContrast ? p.text : p.divider);
        FrameRect(mem, &rc, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    if (T.thumb.dc)
        BitBlt(mem, T.thumbRect.left, T.thumbRect.top, T.thumb.w, T.thumb.h, T.thumb.dc, 0, 0, SRCCOPY);
    const int pad = static_cast<int>(std::lround(12 * T.scale));
    const int lineH = static_cast<int>(std::lround(20 * T.scale));
    HFONT strong = Fluent_Font(kTypeBodyStrong, dpi), body = Fluent_Font(kTypeBody, dpi),
          caption = Fluent_Font(kTypeCaption, dpi);
    SetBkMode(mem, TRANSPARENT);
    RECT r1{pad, T.thumbRect.bottom + pad / 2, rc.right - pad, T.thumbRect.bottom + pad / 2 + lineH};
    T.uploadRect = {};
    HGDIOBJ old = SelectObject(mem, body);
    if (T.canUpload) {
        const wchar_t* label = L"Upload";
        SIZE ts{};
        GetTextExtentPoint32W(mem, label, 6, &ts);
        const int bw = ts.cx + 2 * pad, bh = static_cast<int>(std::lround(28 * T.scale));
        const int top = r1.top + (2 * lineH - bh) / 2;  // centered on the two lines of text
        T.uploadRect = {r1.right - bw, top, r1.right, top + bh};
        Fluent_DrawButton(mem, T.uploadRect, dpi, label, body, true, T.overUpload, false, true, false);
        r1.right = T.uploadRect.left - pad / 2;
    }
    SelectObject(mem, strong);
    SetTextColor(mem, p.text);
    DrawTextW(mem, T.line1.c_str(), -1, &r1, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    RECT r2{pad, r1.bottom, r1.right, r1.bottom + lineH};
    SelectObject(mem, caption);
    SetTextColor(mem, p.text2);
    DrawTextW(mem, T.line2.c_str(), -1, &r2, DT_SINGLELINE | DT_VCENTER | DT_PATH_ELLIPSIS | DT_NOPREFIX);
    SelectObject(mem, old);
    BitBlt(dc, 0, 0, rc.right, rc.bottom, mem, 0, 0, SRCCOPY);
    SelectObject(mem, oldBmp);
    DeleteObject(bmp);
    DeleteDC(mem);
}

// Dragging the thumbnail out gives the saved file to whatever it's dropped on (a chat, a folder...).
void HoldToast() {  // (re)starts the "stay visible" countdown; 0 seconds = never fades by itself
    KillTimer(T.hwnd, kTimerHold);
    if (g_settings.previewSeconds > 0) SetTimer(T.hwnd, kTimerHold, g_settings.previewSeconds * 1000, nullptr);
}

struct DropSource : IDropSource {
    LONG rc = 1;
    STDMETHODIMP QueryInterface(REFIID id, void** o) override {
        if (id == IID_IUnknown || id == IID_IDropSource) { *o = this; AddRef(); return S_OK; }
        *o = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&rc); }
    STDMETHODIMP_(ULONG) Release() override {
        const LONG n = InterlockedDecrement(&rc);
        if (!n) delete this;
        return n;
    }
    STDMETHODIMP QueryContinueDrag(BOOL esc, DWORD keys) override {
        if (esc) return DRAGDROP_S_CANCEL;
        return (keys & MK_LBUTTON) ? S_OK : DRAGDROP_S_DROP;
    }
    STDMETHODIMP GiveFeedback(DWORD) override { return DRAGDROP_S_USEDEFAULTCURSORS; }
};

bool DragFile(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = nullptr;
    if (FAILED(SHParseDisplayName(path.c_str(), nullptr, &pidl, 0, nullptr)) || !pidl) {
        Log(L"Drag: couldn't read %s", path.c_str());
        return false;
    }
    IDataObject* data = nullptr;
    PCUITEMID_CHILD_ARRAY items[1] = {nullptr};
    PCUITEMID_CHILD child = nullptr;
    IShellFolder* folder = nullptr;
    HRESULT hr = SHBindToParent(pidl, IID_IShellFolder, reinterpret_cast<void**>(&folder), &child);
    if (SUCCEEDED(hr)) {
        PCUITEMID_CHILD arr[1] = {child};
        hr = folder->GetUIObjectOf(nullptr, 1, arr, IID_IDataObject, nullptr, reinterpret_cast<void**>(&data));
        folder->Release();
    }
    (void)items;
    CoTaskMemFree(pidl);
    if (FAILED(hr) || !data) {
        Log(L"Drag: no data object (0x%08X)", static_cast<unsigned>(hr));
        return false;
    }
    auto* src = new DropSource;
    DWORD effect = 0;
    const HRESULT dr = DoDragDrop(data, src, DROPEFFECT_COPY | DROPEFFECT_MOVE | DROPEFFECT_LINK, &effect);
    Log(L"Drag of %s: result 0x%08X, effect %lu", path.c_str(), static_cast<unsigned>(dr), effect);
    src->Release();
    data->Release();
    return true;
}

LRESULT CALLBACK ToastProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_ERASEBKGND:
        return 1;  // WM_PAINT covers it all
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC dc = BeginPaint(h, &ps);
        ToastPaint(dc);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE;
    case WM_LBUTTONDOWN:
        T.down = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        T.pressed = true;
        SetCapture(h);
        return 0;
    case WM_MOUSEMOVE: {  // keep it up while the mouse is on it
        if (T.pressed && (wp & MK_LBUTTON) && !T.path.empty() &&
            (abs(GET_X_LPARAM(lp) - T.down.x) > GetSystemMetrics(SM_CXDRAG) ||
             abs(GET_Y_LPARAM(lp) - T.down.y) > GetSystemMetrics(SM_CYDRAG))) {
            T.pressed = false;
            ReleaseCapture();
            KillTimer(h, kTimerHold);
            const std::wstring path = T.path;
            DragFile(path);
            ToastHide();
            return 0;
        }
        KillTimer(h, kTimerFade);
        T.alpha = kAlphaFull;
        SetLayeredWindowAttributes(h, 0, static_cast<BYTE>(T.alpha), LWA_ALPHA);
        HoldToast();
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        const bool over = T.canUpload && PtInRect(&T.uploadRect, pt);
        if (over != T.overUpload) {
            T.overUpload = over;
            InvalidateRect(h, nullptr, FALSE);
            if (over) {  // to notice when the mouse leaves the window
                TRACKMOUSEEVENT tme{sizeof(tme), TME_LEAVE, h, 0};
                TrackMouseEvent(&tme);
            }
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        if (T.overUpload) {
            T.overUpload = false;
            InvalidateRect(h, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP: {  // click = edit the screenshot; the Upload button uploads it
        T.pressed = false;
        if (GetCapture() == h) ReleaseCapture();
        const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
        if (T.canUpload && PtInRect(&T.uploadRect, pt)) {
            UploadShown();
            ToastHide();
            return 0;
        }
        std::shared_ptr<CaptureResult> last = T.last;
        const std::wstring path = T.path;
        ToastHide();
        if (last) Editor_Open(std::move(last), path);
        else if (!path.empty()) ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);  // a GIF or video: play it
        return 0;
    }
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
        if (r.img.alpha) std::transform(r.img.px.begin(), r.img.px.end(), src.bits, OverWhite);
        else memcpy(src.bits, r.img.px.data(), r.img.px.size() * sizeof(uint32_t));
        SetStretchBltMode(T.thumb.dc, HALFTONE);
        SetBrushOrgEx(T.thumb.dc, 0, 0, nullptr);
        StretchBlt(T.thumb.dc, 0, 0, tw, th, src.dc, 0, 0, r.img.w, r.img.h, SRCCOPY);
    }
    const int lineH = static_cast<int>(std::lround(20 * s));
    const int H = pad + th + pad / 2 + 2 * lineH + pad;
    T.thumbRect = {pad + (innerW - tw) / 2, pad, pad + (innerW - tw) / 2 + tw, pad + th};
    T.path = path;
    T.canUpload = Upload_Ready();
    T.overUpload = false;
    T.line1 = copied && !path.empty() ? L"Copied and saved" : copied ? L"Copied to clipboard" : L"Saved";
    wchar_t sz[64];
    swprintf_s(sz, L"%d × %d", r.img.w, r.img.h);
    T.line2 = path.empty() ? std::wstring(sz) : std::wstring(sz) + L"   " + PathFindFileNameW(path.c_str());

    const int margin = static_cast<int>(std::lround(16 * s));
    const int x = mi.rcWork.right - W - margin, y = mi.rcWork.bottom - H - margin;
    KillTimer(T.hwnd, kTimerFade);
    T.alpha = kAlphaFull;
    SetLayeredWindowAttributes(T.hwnd, 0, static_cast<BYTE>(T.alpha), LWA_ALPHA);
    Fluent_StylePopup(T.hwnd);  // rounded corners and the border color of the current theme
    SetWindowPos(T.hwnd, HWND_TOPMOST, x, y, W, H, SWP_NOACTIVATE | SWP_SHOWWINDOW);
    InvalidateRect(T.hwnd, nullptr, FALSE);
    HoldToast();
}

}  // namespace

// ---- public --------------------------------------------------------------------------------

// A saved GIF or video: the same corner toast, with the file's own thumbnail; drag it into a chat.
void Output_ShowFile(const std::wstring& path, const std::wstring& line1, const std::wstring& line2) {
    CaptureResult r;
    POINT pt{};
    GetCursorPos(&pt);
    r.screenRect = {pt.x, pt.y, pt.x + 1, pt.y + 1};
    HBITMAP hb = nullptr;
    IShellItemImageFactory* f = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) && f) {
        f->GetImage({512, 340}, SIIGBF_BIGGERSIZEOK | SIIGBF_THUMBNAILONLY, &hb);
        if (!hb) f->GetImage({256, 256}, SIIGBF_BIGGERSIZEOK, &hb);
        f->Release();
    }
    BITMAP bm{};
    if (hb && GetObjectW(hb, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmHeight > 0) {
        r.img.w = bm.bmWidth;
        r.img.h = bm.bmHeight;
        r.img.px.assign(static_cast<size_t>(r.img.w) * r.img.h, 0);
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), r.img.w, -r.img.h, 1, 32, BI_RGB};
        HDC dc = GetDC(nullptr);
        GetDIBits(dc, hb, 0, r.img.h, r.img.px.data(), &bi, DIB_RGB_COLORS);
        ReleaseDC(nullptr, dc);
        r.img.alpha = false;
    } else {
        r.img.w = 600;
        r.img.h = 120;
        r.img.px.assign(static_cast<size_t>(r.img.w) * r.img.h, 0x202020);
    }
    if (hb) DeleteObject(hb);
    ToastShow(r, path, false);
    T.last.reset();
    T.line1 = line1;
    T.line2 = line2;
    InvalidateRect(T.hwnd, nullptr, FALSE);
}

void Output_ShowColor(COLORREF c, POINT at, const std::wstring& line1, const std::wstring& line2) {
    CaptureResult r;
    r.img.w = 600;
    r.img.h = 120;
    r.img.px.assign(static_cast<size_t>(r.img.w) * r.img.h,
                    (static_cast<uint32_t>(GetRValue(c)) << 16) | (GetGValue(c) << 8) | GetBValue(c));
    r.screenRect = {at.x, at.y, at.x + 1, at.y + 1};
    ToastShow(r, L"", false);
    T.last.reset();
    T.canUpload = false;
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
    OleInitialize(nullptr);  // drag and drop of the saved file
    // Never let the notification itself end up in the next screenshot.
    if (!TestMode()) SetWindowDisplayAffinity(T.hwnd, WDA_EXCLUDEFROMCAPTURE);  // tests capture it to look at it
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
    std::shared_ptr<CaptureResult> r(std::move(job->r));
    const bool upload = Upload_Auto(UploadKind::Screenshot);
    if (g_settings.openEditor) {
        r->autoUpload = upload;  // after editing, when Done is pressed
        Editor_Open(std::move(r), job->path);
        return;
    }
    if (upload) {
        if (!job->path.empty()) Upload_File(job->path, false);
        else Upload_Image(r->img, *r);
    }
    if (g_settings.showNotification) {
        ToastShow(*r, job->path, g_settings.copyToClipboard);
        if (upload) {
            T.canUpload = false;
            T.line1 += L" · uploading";
        }
        T.last = std::move(r);
    }
}

bool LoadImageFile(const std::wstring& path, Image& out) {
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) ||
        FAILED(f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) ||
        FAILED(dec->GetFrame(0, &frame)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGR, WICBitmapDitherTypeNone, nullptr, 0,
                                WICBitmapPaletteTypeCustom)))
        return false;
    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (!w || !h || static_cast<uint64_t>(w) * h > (1ull << 28)) return false;  // 268 megapixels: more than memory allows
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.px.resize(static_cast<size_t>(w) * h);
    return SUCCEEDED(conv->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.px.size() * 4),
                                      reinterpret_cast<BYTE*>(out.px.data())));
}

void Output_CopyImage(const Image& img) { SetClipboardImage(img, EncodePng(img)); }

std::wstring Output_SaveImage(const Image& img, const CaptureResult& meta, const std::wstring& path) {
    std::wstring target = path;
    if (target.empty()) {
        SYSTEMTIME t;
        GetLocalTime(&t);
        const std::wstring base = ScreenshotsDir() + L"\\" + ExpandPattern(g_settings.fileName, meta, t);
        SHCreateDirectoryExW(nullptr, base.substr(0, base.find_last_of(L'\\')).c_str(), nullptr);
        target = UniquePath(base, g_settings.format == L"jpg" ? L".jpg" : L".png");
    }
    const wchar_t* ext = PathFindExtensionW(target.c_str());
    const bool jpg = !_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg");
    const std::vector<BYTE> data = jpg ? EncodeJpeg(img, g_settings.jpegQuality) : EncodePng(img);
    if (data.empty()) return L"";
    // Write next to it first, then swap, so a failed write never destroys the original.
    const std::wstring tmp = target + L".tmp";
    DeleteFileW(tmp.c_str());
    if (!WriteFileBytes(tmp, data) || !MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        Log(L"Could not save the edited image to %s (error %lu)", target.c_str(), GetLastError());
        DeleteFileW(tmp.c_str());
        return L"";
    }
    Log(L"Edited image saved -> %s", target.c_str());
    return target;
}

void Output_EditDone(std::shared_ptr<CaptureResult> r, const std::wstring& path) {
    SetClipboardImage(r->img, EncodePng(r->img));
    std::wstring saved;
    if (!path.empty() || g_settings.saveToFile) saved = Output_SaveImage(r->img, *r, path);
    const bool upload = r->autoUpload;
    r->autoUpload = false;
    if (upload) {
        if (!saved.empty()) Upload_File(saved, false);
        else Upload_Image(r->img, *r);
    }
    ToastShow(*r, saved, true);
    T.line1 = saved.empty() ? L"Edited image copied" : L"Edited image copied and saved";
    if (upload) {
        T.canUpload = false;
        T.line1 += L" · uploading";
    }
    T.last = std::move(r);
}

std::wstring Output_NewPath(const CaptureResult& meta, const wchar_t* ext) {
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::wstring base = ScreenshotsDir() + L"\\" + ExpandPattern(g_settings.fileName, meta, t);
    SHCreateDirectoryExW(nullptr, base.substr(0, base.find_last_of(L'\\')).c_str(), nullptr);
    return UniquePath(base, ext);
}
