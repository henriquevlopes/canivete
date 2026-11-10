// Image resizer: a small window that resizes a batch of images with WIC, on all CPU cores.
// Opened from the tray, from Explorer's right-click menu (each selected file arrives through
// main.cpp and is gathered into one window) or by dropping files on the window.
#include "app.h"
#include "theme.h"

#include "../res/resource.h"

#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wincodecsdk.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cwctype>
#include <mutex>
#include <thread>

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kClass[] = L"CaniveteResizer";
constexpr UINT WM_RS_PROGRESS = WM_APP + 30;
constexpr UINT WM_RS_FINISHED = WM_APP + 31;
constexpr DWORD kStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
constexpr DWORD kExStyle = WS_EX_CONTROLPARENT | WS_EX_ACCEPTFILES;
// Layout in 96-DPI pixels.
constexpr int kW = 470;
constexpr int kM = 16;
constexpr int kCX = kM + 96;
constexpr int kCW = kW - kM - kCX;

enum : int {
    IDC_STATIC = 300,
    IDC_ADD,
    IDC_LIST,
    IDC_COUNT,
    IDC_SIZE0 = 310,  // 310..313: Small, Medium, Large, Custom
    IDC_W = 320,
    IDC_X,
    IDC_H,
    IDC_UNIT,
    IDC_FIT,
    IDC_SHRINK,
    IDC_ORIENT,
    IDC_OUTPUT,
    IDC_FLABEL,
    IDC_FOLDER,
    IDC_BROWSE,
    IDC_FORMAT,
    IDC_QLABEL,
    IDC_QUALITY,
    IDC_QSPIN,
    IDC_META,
    IDC_STATUS,
};

struct Preset {
    const wchar_t* key;
    const wchar_t* name;
    int w, h;
};
const Preset kPresets[] = {{L"small", L"Small", 854, 480}, {L"medium", L"Medium", 1366, 768}, {L"large", L"Large", 1920, 1080}};
constexpr int kCustom = 3;

const wchar_t* const kFitKeys[] = {L"fit", L"fill", L"stretch"};
const wchar_t* const kOutputKeys[] = {L"next", L"folder", L"replace"};
const wchar_t* const kFormatKeys[] = {L"same", L"jpg", L"png"};
const wchar_t* const kImageExts[] = {L".jpg", L".jpeg", L".jpe",  L".jfif", L".png",  L".bmp", L".dib",
                                     L".gif", L".tif",  L".tiff", L".webp", L".heic", L".heif", L".avif",
                                     L".jxr", L".wdp",  L".ico"};

// What to do, read from the window when Resize is pressed.
struct Options {
    int w = 0, h = 0;  // target box; 0 = follow the other side. With percent, w is the percent.
    bool percent = false;
    int fit = 0;     // 0 fit inside, 1 fill and crop, 2 stretch
    int output = 0;  // 0 next to the originals, 1 into a folder, 2 replace the originals
    int format = 0;  // 0 same as the original, 1 JPG, 2 PNG, 3 TIFF, 4 BMP
    int quality = 90;
    bool convert = false;  // Convert menu: skip images already in the target format
    bool inPlace = false;  // Rotate menu: overwrite the original (no Recycle Bin)
    WICBitmapTransformOptions turn = WICBitmapTransformRotate0;  // Rotate menu: turn after making it upright
    bool shrinkOnly = true, matchOrient = true, keepMeta = true;
    std::wstring folder;
    std::wstring suffix;  // " (Medium)", " (1280 x 720)", " (50%)"
};

struct Job {
    Options o;
    std::vector<std::wstring> files;
    std::atomic<int> next{0}, done{0}, failed{0}, skipped{0}, running{0};
    std::atomic<bool> cancel{false};
    std::mutex m;
    std::vector<std::wstring> errors;
    ULONGLONG start = 0;
    std::wstring what;  // quick action: "turned right", "converted to PNG", ...
};

struct Ctl {
    HWND wnd;
    int x, y, w, h;
    bool bold;
};

struct UI {
    HWND hwnd = nullptr;
    UINT dpi = 96;
    HFONT font = nullptr, bold = nullptr;
    std::vector<Ctl> ctls;
    int footerY = 0, height = 0;
    std::vector<std::wstring> files;
    std::vector<std::wstring> queued;  // arrived while a job was running
    std::shared_ptr<Job> job;
} U;

int Px(int v) { return MulDiv(v, static_cast<int>(U.dpi), 96); }
HWND Item(int id) { return GetDlgItem(U.hwnd, id); }
bool Checked(int id) { return IsDlgButtonChecked(U.hwnd, id) == BST_CHECKED; }
void SetCheck(int id, bool on) { CheckDlgButton(U.hwnd, id, on ? BST_CHECKED : BST_UNCHECKED); }
int Sel(int id) { return static_cast<int>(SendMessageW(Item(id), CB_GETCURSEL, 0, 0)); }

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(towlower(c));
    return s;
}

std::wstring GetText(int id) {
    HWND c = Item(id);
    std::wstring s(GetWindowTextLengthW(c), L'\0');
    GetWindowTextW(c, s.data(), static_cast<int>(s.size()) + 1);
    while (!s.empty() && iswspace(s.back())) s.pop_back();
    size_t i = 0;
    while (i < s.size() && iswspace(s[i])) ++i;
    return s.substr(i);
}

int GetInt(int id) {
    BOOL ok = FALSE;
    const UINT v = GetDlgItemInt(U.hwnd, id, &ok, FALSE);
    return ok ? static_cast<int>(v) : 0;
}

// ---- remembered choices ([Resizer] in settings.ini) -----------------------------------------

std::wstring IniStr(const wchar_t* key, const wchar_t* def) {
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"Resizer", key, def, buf, 1024, SettingsPath().c_str());
    return Lower(buf);
}
int IniInt(const wchar_t* key, int def) { return GetPrivateProfileIntW(L"Resizer", key, def, SettingsPath().c_str()); }
void IniPut(const wchar_t* key, const std::wstring& v) {
    WritePrivateProfileStringW(L"Resizer", key, v.c_str(), SettingsPath().c_str());
}
template <size_t N>
int IndexOf(const wchar_t* const (&keys)[N], const std::wstring& v, int def) {
    for (int i = 0; i < static_cast<int>(N); ++i)
        if (v == keys[i]) return i;
    return def;
}

// ---- the resizing itself (worker threads) ----------------------------------------------------

std::wstring Ext(const std::wstring& path) {
    const size_t slash = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.');
    return dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? L"" : path.substr(dot);
}

bool IsImageName(const std::wstring& path) {
    const std::wstring e = Lower(Ext(path));
    return std::any_of(std::begin(kImageExts), std::end(kImageExts), [&](const wchar_t* x) { return e == x; });
}

std::wstring HrText(HRESULT hr) {
    switch (hr) {
    case WINCODEC_ERR_COMPONENTNOTFOUND:
    case WINCODEC_ERR_UNKNOWNIMAGEFORMAT: return L"not a picture Windows can read (damaged or unsupported)";
    case WINCODEC_ERR_BADHEADER:
    case WINCODEC_ERR_BADIMAGE:
    case WINCODEC_ERR_STREAMREAD: return L"the file is damaged or not an image";
    case E_ACCESSDENIED: return L"access denied";
    case E_OUTOFMEMORY: return L"out of memory";
    case HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION): return L"the file is open in another program";
    case HRESULT_FROM_WIN32(ERROR_DISK_FULL): return L"the disk is full";
    case HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND):
    case HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND): return L"file or folder not found";
    }
    wchar_t b[48];
    swprintf_s(b, L"error 0x%08X", static_cast<unsigned>(hr));
    return b;
}

// Reserves a free name ("base.ext", "base (2).ext", ...) by creating it empty, so two threads
// never pick the same one.
std::wstring ReservePath(const std::wstring& base, const std::wstring& ext) {
    for (int i = 1; i < 1000; ++i) {
        std::wstring p = i == 1 ? base + ext : base + L" (" + std::to_wstring(i) + L")" + ext;
        HANDLE f = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            CloseHandle(f);
            return p;
        }
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS) return L"";
    }
    return L"";
}

std::mutex g_recycle;

bool Recycle(const std::wstring& path) {
    std::lock_guard<std::mutex> lock(g_recycle);
    std::wstring from = path;
    from.push_back(L'\0');  // double-null terminated list
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = from.c_str();
    op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    return SHFileOperationW(&op) == 0 && !op.fAnyOperationsAborted;
}

// EXIF orientation (1..8) -> WIC transform.
WICBitmapTransformOptions OrientTransform(UINT o) {
    switch (o) {
    case 2: return WICBitmapTransformFlipHorizontal;
    case 3: return WICBitmapTransformRotate180;
    case 4: return WICBitmapTransformFlipVertical;
    case 5: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6: return WICBitmapTransformRotate90;
    case 7: return static_cast<WICBitmapTransformOptions>(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8: return WICBitmapTransformRotate270;
    }
    return WICBitmapTransformRotate0;
}

const wchar_t* OrientPath(const GUID& container) {
    return container == GUID_ContainerFormatJpeg ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}";
}

UINT ReadOrientation(IWICBitmapFrameDecode* frame, const GUID& container) {
    ComPtr<IWICMetadataQueryReader> q;
    if (FAILED(frame->GetMetadataQueryReader(&q))) return 1;
    PROPVARIANT v;
    PropVariantInit(&v);
    UINT o = 1;
    if (SUCCEEDED(q->GetMetadataByName(OrientPath(container), &v)) && v.vt == VT_UI2) o = v.uiVal;
    PropVariantClear(&v);
    return o >= 1 && o <= 8 ? o : 1;
}

bool HasAlpha(IWICImagingFactory* f, IWICBitmapSource* s) {
    WICPixelFormatGUID pf;
    ComPtr<IWICComponentInfo> ci;
    ComPtr<IWICPixelFormatInfo2> pi;
    BOOL t = FALSE;
    if (SUCCEEDED(s->GetPixelFormat(&pf)) && SUCCEEDED(f->CreateComponentInfo(pf, &ci)) && SUCCEEDED(ci.As(&pi)))
        pi->SupportsTransparency(&t);
    return t != FALSE;
}

enum class Result { Done, Skipped, Failed };

bool AnyTransparent(IWICBitmap* bmp) {
    ComPtr<IWICBitmapLock> lock;
    UINT w = 0, h = 0, stride = 0, size = 0;
    BYTE* p = nullptr;
    if (FAILED(bmp->Lock(nullptr, WICBitmapLockRead, &lock)) || FAILED(lock->GetSize(&w, &h)) ||
        FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &p)))
        return true;
    for (UINT y = 0; y < h; ++y) {
        const BYTE* row = p + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < w; ++x)
            if (row[x * 4 + 3] != 255) return true;
    }
    return false;
}

// Puts see-through pixels on white (for JPG, which has no transparency). Pixels are premultiplied BGRA.
void FlattenOnWhite(IWICBitmap* bmp) {
    ComPtr<IWICBitmapLock> lock;
    UINT w = 0, h = 0, stride = 0, size = 0;
    BYTE* p = nullptr;
    if (FAILED(bmp->Lock(nullptr, WICBitmapLockRead | WICBitmapLockWrite, &lock)) || FAILED(lock->GetSize(&w, &h)) ||
        FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &p)))
        return;
    for (UINT y = 0; y < h; ++y) {
        BYTE* row = p + static_cast<size_t>(y) * stride;
        for (UINT x = 0; x < w; ++x) {
            BYTE* px = row + x * 4;
            const int add = 255 - px[3];
            for (int c = 0; c < 3; ++c) px[c] = static_cast<BYTE>(std::min(255, px[c] + add));
            px[3] = 255;
        }
    }
}

// Decodes, scales, turns upright and crops one image into memory. `alpha` says whether any pixel is
// see-through (a WebP or 32-bit PNG is often fully opaque). The source file is closed on return.
HRESULT Prepare(IWICImagingFactory* f, const std::wstring& src, const Options& o, ComPtr<IWICBitmap>& out,
                GUID& srcFmt, bool& alpha, bool& skipped, ComPtr<IWICMetadataBlockReader>& meta, UINT& orient) {
    ComPtr<IWICBitmapDecoder> dec;
    HRESULT hr = f->CreateDecoderFromFilename(src.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnLoad, &dec);
    if (FAILED(hr)) return hr;
    dec->GetContainerFormat(&srcFmt);
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(hr = dec->GetFrame(0, &frame))) return hr;
    frame.As(&meta);  // read before the file closes (metadata is cached on load)
    orient = ReadOrientation(frame.Get(), srcFmt);
    const bool sideways = orient >= 5;  // 90 or 270 degrees: stored width is the shown height

    UINT sw = 0, sh = 0;
    if (FAILED(hr = frame->GetSize(&sw, &sh)) || !sw || !sh) return FAILED(hr) ? hr : WINCODEC_ERR_BADIMAGE;
    const UINT w = sideways ? sh : sw, h = sideways ? sw : sh;  // as shown

    // Target size, as shown.
    double tw = w, th = h;
    int cropW = 0, cropH = 0;
    if (o.percent) {
        tw = w * o.w / 100.0;
        th = h * o.w / 100.0;
    } else {
        int W = o.w, H = o.h;
        if (o.matchOrient && W > 0 && H > 0 && w != h && (W > H) != (w > h)) std::swap(W, H);
        if (o.fit == 2) {  // stretch
            tw = W > 0 ? W : w * static_cast<double>(H) / h;
            th = H > 0 ? H : h * static_cast<double>(W) / w;
        } else {
            const double sx = W > 0 ? static_cast<double>(W) / w : 0, sy = H > 0 ? static_cast<double>(H) / h : 0;
            const double s = W <= 0 ? sy : H <= 0 ? sx : o.fit == 1 ? std::max(sx, sy) : std::min(sx, sy);
            tw = w * s;
            th = h * s;
            if (o.fit == 1 && W > 0 && H > 0) {
                cropW = W;
                cropH = H;
            }
        }
    }
    if (o.shrinkOnly && (tw > w + 0.5 || th > h + 0.5)) {  // already small enough
        if (o.output == 2) {
            skipped = true;
            return S_OK;
        }
        tw = w;
        th = h;
        cropW = cropH = 0;
    }
    const UINT ow = std::max(1u, static_cast<UINT>(std::lround(tw)));
    const UINT oh = std::max(1u, static_cast<UINT>(std::lround(th)));

    // Scale in the stored orientation (rows are read in order, fast), keep the result in memory,
    // then turn it upright: rotating a file-backed source re-decodes it for every row.
    const bool srcAlpha = HasAlpha(f, frame.Get());
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(hr = f->CreateFormatConverter(&conv))) return hr;
    if (FAILED(hr = conv->Initialize(frame.Get(), srcAlpha ? GUID_WICPixelFormat32bppPBGRA : GUID_WICPixelFormat32bppBGR,
                                     WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        return hr;
    ComPtr<IWICBitmapSource> cur = conv;
    const UINT scaleW = sideways ? oh : ow, scaleH = sideways ? ow : oh;
    if (scaleW != sw || scaleH != sh) {
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(hr = f->CreateBitmapScaler(&sc))) return hr;
        if (FAILED(hr = sc->Initialize(cur.Get(), scaleW, scaleH, WICBitmapInterpolationModeHighQualityCubic))) return hr;
        cur = sc;
    }
    ComPtr<IWICBitmap> scaled;
    if (FAILED(hr = f->CreateBitmapFromSource(cur.Get(), WICBitmapCacheOnLoad, &scaled))) return hr;
    cur = scaled;
    ComPtr<IWICBitmap> mem = scaled;  // the newest in-memory copy
    if (orient != 1) {
        ComPtr<IWICBitmapFlipRotator> rot;
        if (FAILED(hr = f->CreateBitmapFlipRotator(&rot)) ||
            FAILED(hr = rot->Initialize(cur.Get(), OrientTransform(orient))))
            return hr;
        cur = rot;
    }
    if (o.turn != WICBitmapTransformRotate0) {
        if (cur.Get() != mem.Get()) {  // upright copy in memory first: two chained rotators re-read rows
            if (FAILED(hr = f->CreateBitmapFromSource(cur.Get(), WICBitmapCacheOnLoad, mem.ReleaseAndGetAddressOf())))
                return hr;
            cur = mem;
        }
        ComPtr<IWICBitmapFlipRotator> rot;
        if (FAILED(hr = f->CreateBitmapFlipRotator(&rot)) || FAILED(hr = rot->Initialize(cur.Get(), o.turn))) return hr;
        cur = rot;
    }
    if (cropW > 0 && cropH > 0 && (static_cast<UINT>(cropW) < ow || static_cast<UINT>(cropH) < oh)) {
        const UINT cw = std::min<UINT>(ow, cropW), ch = std::min<UINT>(oh, cropH);
        const WICRect r{static_cast<INT>((ow - cw) / 2), static_cast<INT>((oh - ch) / 2), static_cast<INT>(cw),
                        static_cast<INT>(ch)};
        ComPtr<IWICBitmapClipper> clip;
        if (FAILED(hr = f->CreateBitmapClipper(&clip)) || FAILED(hr = clip->Initialize(cur.Get(), &r))) return hr;
        cur = clip;
    }
    if (cur.Get() == mem.Get()) out = mem;
    else if (FAILED(hr = f->CreateBitmapFromSource(cur.Get(), WICBitmapCacheOnLoad, &out))) return hr;
    alpha = srcAlpha && AnyTransparent(out.Get());
    return S_OK;
}

HRESULT Write(IWICImagingFactory* f, IWICBitmap* bmp, const Options& o, const std::wstring& out, const GUID& outFmt,
              bool alpha, const GUID& srcFmt, IWICMetadataBlockReader* meta, UINT orient) {
    UINT w = 0, h = 0;
    bmp->GetSize(&w, &h);
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fe;
    ComPtr<IPropertyBag2> bag;
    HRESULT hr;
    if (FAILED(hr = f->CreateStream(&stream)) || FAILED(hr = stream->InitializeFromFilename(out.c_str(), GENERIC_WRITE)) ||
        FAILED(hr = f->CreateEncoder(outFmt, nullptr, &enc)) ||
        FAILED(hr = enc->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(hr = enc->CreateNewFrame(&fe, &bag)))
        return hr;
    if (outFmt == GUID_ContainerFormatJpeg) {
        PROPBAG2 opt{};
        opt.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT v;
        VariantInit(&v);
        v.vt = VT_R4;
        v.fltVal = std::clamp(o.quality, 1, 100) / 100.f;
        bag->Write(1, &opt, &v);
    }
    if (FAILED(hr = fe->Initialize(bag.Get())) || FAILED(hr = fe->SetSize(w, h))) return hr;
    WICPixelFormatGUID want = alpha && outFmt != GUID_ContainerFormatJpeg ? GUID_WICPixelFormat32bppBGRA
                                                                         : GUID_WICPixelFormat24bppBGR;
    if (FAILED(hr = fe->SetPixelFormat(&want))) return hr;
    ComPtr<IWICBitmapSource> src;
    if (FAILED(hr = WICConvertBitmapSource(want, bmp, &src))) return hr;
    // Camera info, date, GPS and color profile: copied when the format stays the same. The pixels
    // are already upright, so the orientation tag is reset.
    if (o.keepMeta && meta && outFmt == srcFmt &&
        (srcFmt == GUID_ContainerFormatJpeg || srcFmt == GUID_ContainerFormatTiff || srcFmt == GUID_ContainerFormatPng)) {
        ComPtr<IWICMetadataBlockWriter> bw;
        if (SUCCEEDED(fe.As(&bw)) && SUCCEEDED(bw->InitializeFromBlockReader(meta)) && orient != 1) {
            ComPtr<IWICMetadataQueryWriter> qw;
            if (SUCCEEDED(fe->GetMetadataQueryWriter(&qw))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                v.vt = VT_UI2;
                v.uiVal = 1;
                qw->SetMetadataByName(OrientPath(srcFmt), &v);
            }
        }
    }
    if (FAILED(hr = fe->WriteSource(src.Get(), nullptr)) || FAILED(hr = fe->Commit()) || FAILED(hr = enc->Commit()))
        return hr;
    return S_OK;
}

Result ResizeOne(IWICImagingFactory* f, const std::wstring& src, const Options& o, std::wstring& err) {
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICMetadataBlockReader> meta;
    GUID srcFmt{};
    bool alpha = false, skipped = false;
    UINT orient = 1;
    HRESULT hr = Prepare(f, src, o, bmp, srcFmt, alpha, skipped, meta, orient);
    if (FAILED(hr)) {
        err = HrText(hr);
        return Result::Failed;
    }
    if (skipped) return Result::Skipped;

    // Output format: JPG/PNG/BMP/TIFF stay as they are; others become PNG when see-through, else JPG.
    const std::wstring srcExt = Ext(src);
    GUID outFmt = alpha && o.format != 1 ? GUID_ContainerFormatPng : GUID_ContainerFormatJpeg;
    if (o.format == 2) outFmt = GUID_ContainerFormatPng;
    if (o.format == 3) outFmt = GUID_ContainerFormatTiff;
    if (o.format == 4) outFmt = GUID_ContainerFormatBmp;
    std::wstring ext = outFmt == GUID_ContainerFormatPng    ? L".png"
                       : outFmt == GUID_ContainerFormatTiff ? L".tif"
                       : outFmt == GUID_ContainerFormatBmp  ? L".bmp"
                                                            : L".jpg";
    if (o.format == 0) {
        for (const GUID& g : {GUID_ContainerFormatJpeg, GUID_ContainerFormatPng, GUID_ContainerFormatBmp,
                              GUID_ContainerFormatTiff})
            if (srcFmt == g && !(alpha && g == GUID_ContainerFormatJpeg)) {
                outFmt = g;
                ext = srcExt;  // keep .jpeg / .JPG / .tif as they were
            }
    }

    if (o.convert && outFmt == srcFmt) return Result::Skipped;
    if (o.inPlace && outFmt != srcFmt) {
        err = L"only JPG, PNG, BMP and TIFF files can be turned";
        return Result::Failed;
    }
    if (alpha && outFmt == GUID_ContainerFormatJpeg) {
        FlattenOnWhite(bmp.Get());
        alpha = false;
    }

    const size_t slash = src.find_last_of(L"\\/");
    const std::wstring dir = src.substr(0, slash);
    const std::wstring stem = src.substr(slash + 1, src.size() - slash - 1 - srcExt.size());
    std::wstring out;
    if (o.output == 2 || o.inPlace) {
        out = ReservePath(dir + L"\\" + stem + L".canivete-tmp", ext);
    } else if (o.output == 1) {
        SHCreateDirectoryExW(nullptr, o.folder.c_str(), nullptr);
        out = ReservePath(o.folder + L"\\" + stem, ext);
    } else {
        out = ReservePath(dir + L"\\" + stem + o.suffix, ext);
    }
    if (out.empty()) {
        err = L"can't create the new file (folder read-only?)";
        return Result::Failed;
    }
    hr = Write(f, bmp.Get(), o, out, outFmt, alpha, srcFmt, meta.Get(), orient);
    if (FAILED(hr)) {
        DeleteFileW(out.c_str());
        err = HrText(hr);
        return Result::Failed;
    }
    if (o.inPlace) {  // the turned copy takes the original's place (same name, dates and attributes)
        meta.Reset();
        bmp.Reset();
        if (!ReplaceFileW(src.c_str(), out.c_str(), nullptr, REPLACEFILE_IGNORE_MERGE_ERRORS | REPLACEFILE_IGNORE_ACL_ERRORS,
                          nullptr, nullptr)) {
            const HRESULT e = HRESULT_FROM_WIN32(GetLastError());
            DeleteFileW(out.c_str());
            err = HrText(e);
            return Result::Failed;
        }
        SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, src.c_str(), nullptr);  // new thumbnail
        return Result::Done;
    }
    if (o.output != 2) return Result::Done;

    // Replace: the original goes to the Recycle Bin, the new file takes its name. The metadata
    // reader still holds the source file open, so let go of it first.
    meta.Reset();
    bmp.Reset();
    if (!Recycle(src)) {
        DeleteFileW(out.c_str());
        err = L"couldn't move the original to the Recycle Bin";
        return Result::Failed;
    }
    std::wstring final = dir + L"\\" + stem + ext;
    for (int i = 2; i < 1000 && Lower(final) != Lower(src) && GetFileAttributesW(final.c_str()) != INVALID_FILE_ATTRIBUTES;
         ++i)
        final = dir + L"\\" + stem + L" (" + std::to_wstring(i) + L")" + ext;
    if (!MoveFileExW(out.c_str(), final.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        err = L"resized copy left as " + out.substr(out.find_last_of(L'\\') + 1);
        return Result::Failed;
    }
    SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW | SHCNF_FLUSHNOWAIT, final.c_str(), nullptr);
    return Result::Done;
}

void Worker(std::shared_ptr<Job> j, HWND notify) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> f;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        const int n = static_cast<int>(j->files.size());
        for (int i; f && !j->cancel && (i = j->next++) < n;) {
            std::wstring err;
            const Result r = ResizeOne(f.Get(), j->files[i], j->o, err);
            if (r == Result::Failed) {
                const std::wstring& p = j->files[i];
                std::lock_guard<std::mutex> lock(j->m);
                j->errors.push_back(p.substr(p.find_last_of(L"\\/") + 1) + L":  " + err);
                Log(L"Resize failed: %s (%s)", p.c_str(), err.c_str());
                ++j->failed;
            } else if (r == Result::Skipped) {
                ++j->skipped;
            }
            ++j->done;
            PostMessageW(notify, WM_RS_PROGRESS, 0, 0);
        }
    }
    CoUninitialize();
    if (--j->running == 0) PostMessageW(notify, WM_RS_FINISHED, 0, reinterpret_cast<LPARAM>(j.get()));
}

// ---- window --------------------------------------------------------------------------------

HWND Add(const wchar_t* cls, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h, DWORD ex = 0,
         bool bold = false) {
    HWND c = CreateWindowExW(ex, cls, text, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, U.hwnd,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), g_hinst, nullptr);
    U.ctls.push_back({c, x, y, w, h, bold});
    return c;
}
void Heading(const wchar_t* text, int y) {
    Add(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, IDC_STATIC, kM, y, kW - 2 * kM, 22, 0, true);
}
void Label(const wchar_t* text, int y, int id = IDC_STATIC) {
    Add(WC_STATICW, text, SS_LEFT | SS_NOPREFIX, id, kM, y + 4, kCX - kM - 8, 18);
}
HWND Combo(int id, int x, int y, int w, std::initializer_list<const wchar_t*> items) {
    HWND c = Add(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP | WS_VSCROLL, id, x, y, w, 200);
    for (const wchar_t* s : items) SendMessageW(c, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(s));
    return c;
}
HWND Edit(int id, int x, int y, int w, DWORD style = 0) {
    return Add(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP | style, id, x, y, w, 24, WS_EX_CLIENTEDGE);
}

void Build() {
    int y = 14;
    Heading(L"Images", y);
    Add(WC_BUTTONW, L"Add…", BS_PUSHBUTTON | WS_TABSTOP, IDC_ADD, kW - kM - 84, y - 4, 84, 26);
    y += 28;
    Add(WC_LISTBOXW, L"", LBS_NOINTEGRALHEIGHT | LBS_EXTENDEDSEL | LBS_WANTKEYBOARDINPUT | LBS_NOTIFY | WS_VSCROLL |
                              WS_TABSTOP,
        IDC_LIST, kM, y, kW - 2 * kM, 96, WS_EX_CLIENTEDGE);
    y += 100;
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_COUNT, kM, y, kW - 2 * kM, 18);
    y += 30;

    Heading(L"Size", y);
    y += 28;
    for (int i = 0; i < 3; ++i) {
        wchar_t t[64];
        Add(WC_BUTTONW, kPresets[i].name, BS_AUTORADIOBUTTON | WS_TABSTOP | (i == 0 ? WS_GROUP : 0), IDC_SIZE0 + i, kM + 4, y,
            90, 20);
        swprintf_s(t, L"%d × %d", kPresets[i].w, kPresets[i].h);
        Add(WC_STATICW, t, SS_LEFT | SS_NOPREFIX, IDC_STATIC, kM + 100, y + 2, 120, 18);
        y += 26;
    }
    Add(WC_BUTTONW, L"Custom", BS_AUTORADIOBUTTON | WS_TABSTOP, IDC_SIZE0 + kCustom, kM + 4, y + 2, 90, 20);
    HWND w = Edit(IDC_W, kM + 100, y, 64, ES_NUMBER);
    SendMessageW(w, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"width"));
    Add(WC_STATICW, L"×", SS_CENTER | SS_NOPREFIX, IDC_X, kM + 166, y + 4, 18, 18);
    HWND h = Edit(IDC_H, kM + 186, y, 64, ES_NUMBER);
    SendMessageW(h, EM_SETCUEBANNER, TRUE, reinterpret_cast<LPARAM>(L"height"));
    Combo(IDC_UNIT, kM + 258, y, 100, {L"pixels", L"percent"});
    y += 34;
    Label(L"Fit", y);
    Combo(IDC_FIT, kCX, y, kCW, {L"Fit inside (keep the whole image)", L"Fill and crop the edges",
                                 L"Stretch to exactly this size"});
    y += 34;
    Add(WC_BUTTONW, L"Only make images smaller, never bigger", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_SHRINK, kM, y,
        kW - 2 * kM, 20);
    y += 26;
    Add(WC_BUTTONW, L"Turn the size sideways for tall (portrait) images", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_ORIENT, kM,
        y, kW - 2 * kM, 20);
    y += 36;

    Heading(L"Save", y);
    y += 28;
    Label(L"Save to", y);
    Combo(IDC_OUTPUT, kCX, y, kCW, {L"New files next to the originals", L"New files in a folder",
                                    L"Replace the originals (old ones go to the Recycle Bin)"});
    y += 34;
    Label(L"Folder", y, IDC_FLABEL);
    HWND folder = Edit(IDC_FOLDER, kCX, y, kCW - 90);
    SHAutoComplete(folder, SHACF_FILESYS_DIRS);
    Add(WC_BUTTONW, L"Browse…", BS_PUSHBUTTON | WS_TABSTOP, IDC_BROWSE, kCX + kCW - 84, y - 1, 84, 26);
    y += 34;
    Label(L"Format", y);
    Combo(IDC_FORMAT, kCX, y, 170, {L"Same as the original", L"JPG", L"PNG"});
    Add(WC_STATICW, L"JPG quality", SS_RIGHT | SS_NOPREFIX, IDC_QLABEL, kCX + 176, y + 4, 80, 18);
    HWND q = Edit(IDC_QUALITY, kCX + 262, y, 46, ES_NUMBER);
    HWND spin = Add(UPDOWN_CLASSW, L"", UDS_SETBUDDYINT | UDS_ARROWKEYS | UDS_NOTHOUSANDS | UDS_HOTTRACK, IDC_QSPIN,
                    kCX + 308, y, 18, 24);
    SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(q), 0);
    SendMessageW(spin, UDM_SETRANGE32, 1, 100);
    y += 34;
    Add(WC_BUTTONW, L"Keep photo info (camera, date, location, color profile)", BS_AUTOCHECKBOX | WS_TABSTOP, IDC_META,
        kM, y, kW - 2 * kM, 20);
    y += 34;

    U.footerY = y;
    y += 12;
    Add(WC_STATICW, L"", SS_LEFT | SS_NOPREFIX | SS_ENDELLIPSIS, IDC_STATUS, kM, y + 6, kW - 2 * kM - 2 * 84 - 16, 18);
    Add(WC_BUTTONW, L"Resize", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, kW - kM - 2 * 84 - 8, y, 84, 28);
    Add(WC_BUTTONW, L"Close", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, kW - kM - 84, y, 84, 28);
    U.height = y + 28 + 12;
}

void MakeFonts() {
    if (U.font) DeleteObject(U.font);
    if (U.bold) DeleteObject(U.bold);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, U.dpi);
    LOGFONTW lf = ncm.lfMessageFont;
    U.font = CreateFontIndirectW(&lf);
    lf.lfWeight = FW_SEMIBOLD;
    lf.lfHeight = MulDiv(lf.lfHeight, 112, 100);
    U.bold = CreateFontIndirectW(&lf);
}

void Layout() {
    HDWP d = BeginDeferWindowPos(static_cast<int>(U.ctls.size()));
    for (const Ctl& c : U.ctls) {
        SendMessageW(c.wnd, WM_SETFONT, reinterpret_cast<WPARAM>(c.bold ? U.bold : U.font), FALSE);
        if (d) d = DeferWindowPos(d, c.wnd, nullptr, Px(c.x), Px(c.y), Px(c.w), Px(c.h), SWP_NOZORDER | SWP_NOACTIVATE);
    }
    if (d) EndDeferWindowPos(d);
    InvalidateRect(U.hwnd, nullptr, TRUE);
}

SIZE WindowSize() {
    RECT r{0, 0, Px(kW), Px(U.height)};
    AdjustWindowRectExForDpi(&r, kStyle, FALSE, kExStyle, U.dpi);
    return {r.right - r.left, r.bottom - r.top};
}

int SizeChoice() {
    for (int i = 0; i <= kCustom; ++i)
        if (Checked(IDC_SIZE0 + i)) return i;
    return 1;
}

bool Busy() { return U.job && U.job->running > 0; }

void UpdateEnable() {
    const bool busy = Busy();
    const bool custom = SizeChoice() == kCustom, percent = Sel(IDC_UNIT) == 1;
    for (int id : {IDC_W, IDC_UNIT}) EnableWindow(Item(id), !busy && custom);
    for (int id : {IDC_X, IDC_H}) EnableWindow(Item(id), !busy && custom && !percent);
    EnableWindow(Item(IDC_FIT), !busy && !(custom && percent));
    EnableWindow(Item(IDC_ORIENT), !busy && !(custom && percent));
    const bool folder = Sel(IDC_OUTPUT) == 1;
    for (int id : {IDC_FLABEL, IDC_FOLDER, IDC_BROWSE}) EnableWindow(Item(id), !busy && folder);
    for (int id : {IDC_QLABEL, IDC_QUALITY, IDC_QSPIN}) EnableWindow(Item(id), !busy && Sel(IDC_FORMAT) != 2);
    for (int id : {int{IDC_ADD}, int{IDC_LIST}, IDC_SIZE0 + 0, IDC_SIZE0 + 1, IDC_SIZE0 + 2, IDC_SIZE0 + 3,
                   int{IDC_SHRINK}, int{IDC_OUTPUT}, int{IDC_FORMAT}, int{IDC_META}})
        EnableWindow(Item(id), !busy);
    SetWindowTextW(Item(IDOK), busy ? L"Stop" : L"Resize");
}

void ShowFiles() {
    HWND list = Item(IDC_LIST);
    SendMessageW(list, WM_SETREDRAW, FALSE, 0);
    SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (const std::wstring& f : U.files)
        SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(f.substr(f.find_last_of(L"\\/") + 1).c_str()));
    SendMessageW(list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list, nullptr, TRUE);
    std::wstring t;
    if (U.files.empty()) t = L"Drop images here, or click Add…";
    else if (U.files.size() == 1) t = L"1 image   (Delete key removes the selected ones)";
    else t = std::to_wstring(U.files.size()) + L" images   (Delete key removes the selected ones)";
    SetWindowTextW(Item(IDC_COUNT), t.c_str());
    if (U.hwnd) {
        const std::wstring title =
            U.files.empty()        ? L"Canivete Resize"
            : U.files.size() == 1 ? L"Canivete Resize – 1 image"
                                  : L"Canivete Resize – " + std::to_wstring(U.files.size()) + L" images";
        SetWindowTextW(U.hwnd, title.c_str());
    }
}

void AddPaths(const std::vector<std::wstring>& paths) {
    for (const std::wstring& p : paths) {
        const DWORD a = GetFileAttributesW(p.c_str());
        if (a == INVALID_FILE_ATTRIBUTES) continue;
        std::vector<std::wstring> found;
        if (a & FILE_ATTRIBUTE_DIRECTORY) {  // a folder: the images directly inside it
            WIN32_FIND_DATAW fd;
            HANDLE h = FindFirstFileExW((p + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                        FIND_FIRST_EX_LARGE_FETCH);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && IsImageName(fd.cFileName))
                        found.push_back(p + L"\\" + fd.cFileName);
                } while (FindNextFileW(h, &fd));
                FindClose(h);
            }
        } else if (IsImageName(p)) {
            found.push_back(p);
        }
        for (std::wstring& f : found) {
            const std::wstring lf = Lower(f);
            if (std::none_of(U.files.begin(), U.files.end(), [&](const std::wstring& x) { return Lower(x) == lf; }))
                U.files.push_back(std::move(f));
        }
    }
    ShowFiles();
}

void RemoveSelected() {
    HWND list = Item(IDC_LIST);
    const int n = static_cast<int>(SendMessageW(list, LB_GETSELCOUNT, 0, 0));
    if (n <= 0) return;
    std::vector<int> sel(n);
    SendMessageW(list, LB_GETSELITEMS, n, reinterpret_cast<LPARAM>(sel.data()));
    std::sort(sel.rbegin(), sel.rend());
    for (int i : sel)
        if (i >= 0 && i < static_cast<int>(U.files.size())) U.files.erase(U.files.begin() + i);
    ShowFiles();
}

void Populate() {
    const std::wstring size = IniStr(L"Size", L"medium");
    int sz = 1;
    for (int i = 0; i < 3; ++i)
        if (size == kPresets[i].key) sz = i;
    if (size == L"custom") sz = kCustom;
    SetCheck(IDC_SIZE0 + sz, true);
    const int w = IniInt(L"Width", 1280), h = IniInt(L"Height", 0);
    if (w > 0) SetDlgItemInt(U.hwnd, IDC_W, w, FALSE);
    if (h > 0) SetDlgItemInt(U.hwnd, IDC_H, h, FALSE);
    SendMessageW(Item(IDC_UNIT), CB_SETCURSEL, IniStr(L"Unit", L"px") == L"percent" ? 1 : 0, 0);
    SendMessageW(Item(IDC_FIT), CB_SETCURSEL, IndexOf(kFitKeys, IniStr(L"Fit", L"fit"), 0), 0);
    SetCheck(IDC_SHRINK, IniInt(L"ShrinkOnly", 1) != 0);
    SetCheck(IDC_ORIENT, IniInt(L"MatchOrientation", 1) != 0);
    SendMessageW(Item(IDC_OUTPUT), CB_SETCURSEL, IndexOf(kOutputKeys, IniStr(L"Output", L"next"), 0), 0);
    wchar_t buf[1024];
    GetPrivateProfileStringW(L"Resizer", L"Folder", L"", buf, 1024, SettingsPath().c_str());
    SetWindowTextW(Item(IDC_FOLDER), buf);
    SendMessageW(Item(IDC_FORMAT), CB_SETCURSEL, IndexOf(kFormatKeys, IniStr(L"Format", L"same"), 0), 0);
    SendMessageW(Item(IDC_QSPIN), UDM_SETPOS32, 0, std::clamp(IniInt(L"JpegQuality", 90), 1, 100));
    SetCheck(IDC_META, IniInt(L"KeepMetadata", 1) != 0);
    ShowFiles();
    UpdateEnable();
}

void Browse() {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    d->SetTitle(L"Folder for the resized images");
    const std::wstring cur = GetText(IDC_FOLDER);
    ComPtr<IShellItem> start;
    if (!cur.empty() && SUCCEEDED(SHCreateItemFromParsingName(cur.c_str(), nullptr, IID_PPV_ARGS(&start))))
        d->SetFolder(start.Get());
    ComPtr<IShellItem> result;
    if (FAILED(d->Show(U.hwnd)) || FAILED(d->GetResult(&result))) return;
    PWSTR p = nullptr;
    if (SUCCEEDED(result->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
        SetWindowTextW(Item(IDC_FOLDER), p);
        CoTaskMemFree(p);
    }
}

void AddDialog() {
    ComPtr<IFileOpenDialog> d;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o = 0;
    d->GetOptions(&o);
    d->SetOptions(o | FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST);
    d->SetTitle(L"Images to resize");
    std::wstring spec;
    for (const wchar_t* e : kImageExts) spec += (spec.empty() ? L"*" : L";*") + std::wstring(e);
    const COMDLG_FILTERSPEC f[] = {{L"Images", spec.c_str()}, {L"All files", L"*.*"}};
    d->SetFileTypes(2, f);
    ComPtr<IShellItemArray> items;
    if (FAILED(d->Show(U.hwnd)) || FAILED(d->GetResults(&items))) return;
    DWORD n = 0;
    items->GetCount(&n);
    std::vector<std::wstring> paths;
    for (DWORD i = 0; i < n; ++i) {
        ComPtr<IShellItem> it;
        PWSTR p = nullptr;
        if (SUCCEEDED(items->GetItemAt(i, &it)) && SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
            paths.push_back(p);
            CoTaskMemFree(p);
        }
    }
    AddPaths(paths);
}

void SetStatus(const std::wstring& t) { SetWindowTextW(Item(IDC_STATUS), t.c_str()); }

// Reads the window into Options and remembers it. Returns false (with a message) when something is missing.
bool ReadOptions(Options& o) {
    const int sz = SizeChoice();
    o.percent = sz == kCustom && Sel(IDC_UNIT) == 1;
    if (sz < kCustom) {
        o.w = kPresets[sz].w;
        o.h = kPresets[sz].h;
        o.suffix = std::wstring(L" (") + kPresets[sz].name + L")";
    } else if (o.percent) {
        o.w = GetInt(IDC_W);
        if (o.w < 1 || o.w > 1000) {
            MessageBoxW(U.hwnd, L"Type a percentage between 1 and 1000.", L"Canivete Resize", MB_ICONINFORMATION);
            SetFocus(Item(IDC_W));
            return false;
        }
        o.suffix = L" (" + std::to_wstring(o.w) + L"%)";
    } else {
        o.w = GetInt(IDC_W);
        o.h = GetInt(IDC_H);
        if (o.w <= 0 && o.h <= 0) {
            MessageBoxW(U.hwnd, L"Type a width, a height, or both.", L"Canivete Resize", MB_ICONINFORMATION);
            SetFocus(Item(IDC_W));
            return false;
        }
        o.suffix = o.w > 0 && o.h > 0 ? L" (" + std::to_wstring(o.w) + L" x " + std::to_wstring(o.h) + L")"
                   : o.w > 0          ? L" (" + std::to_wstring(o.w) + L" wide)"
                                      : L" (" + std::to_wstring(o.h) + L" high)";
    }
    o.fit = std::max(0, Sel(IDC_FIT));
    o.shrinkOnly = Checked(IDC_SHRINK);
    o.matchOrient = Checked(IDC_ORIENT);
    o.output = std::max(0, Sel(IDC_OUTPUT));
    o.format = std::max(0, Sel(IDC_FORMAT));
    o.quality = std::clamp(GetInt(IDC_QUALITY), 1, 100);
    o.keepMeta = Checked(IDC_META);
    o.folder = GetText(IDC_FOLDER);
    if (o.output == 1) {
        wchar_t buf[MAX_PATH * 2];
        ExpandEnvironmentStringsW(o.folder.c_str(), buf, MAX_PATH * 2);
        o.folder = buf;
        while (!o.folder.empty() && (o.folder.back() == L'\\' || o.folder.back() == L'/')) o.folder.pop_back();
        if (o.folder.empty() || PathIsRelativeW(o.folder.c_str())) {
            MessageBoxW(U.hwnd, L"Choose the folder for the new files.", L"Canivete Resize", MB_ICONINFORMATION);
            SetFocus(Item(IDC_FOLDER));
            return false;
        }
    }

    IniPut(L"Size", sz < kCustom ? kPresets[sz].key : L"custom");
    if (GetInt(IDC_W) > 0 || GetText(IDC_W).empty()) IniPut(L"Width", std::to_wstring(GetInt(IDC_W)));
    IniPut(L"Height", std::to_wstring(GetInt(IDC_H)));
    IniPut(L"Unit", Sel(IDC_UNIT) == 1 ? L"percent" : L"px");
    IniPut(L"Fit", kFitKeys[o.fit]);
    IniPut(L"ShrinkOnly", o.shrinkOnly ? L"1" : L"0");
    IniPut(L"MatchOrientation", o.matchOrient ? L"1" : L"0");
    IniPut(L"Output", kOutputKeys[o.output]);
    IniPut(L"Folder", GetText(IDC_FOLDER));
    IniPut(L"Format", kFormatKeys[o.format]);
    IniPut(L"JpegQuality", std::to_wstring(o.quality));
    IniPut(L"KeepMetadata", o.keepMeta ? L"1" : L"0");
    return true;
}

void Start() {
    if (U.files.empty()) {
        AddDialog();
        return;
    }
    auto j = std::make_shared<Job>();
    if (!ReadOptions(j->o)) return;
    j->files = U.files;
    j->start = GetTickCount64();
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1,
                                   std::min(8, static_cast<int>(j->files.size())));
    j->running = threads;
    U.job = j;
    Log(L"Resize: %zu images, %d threads", j->files.size(), threads);
    for (int i = 0; i < threads; ++i) std::thread(Worker, j, U.hwnd).detach();
    SetStatus(L"Resizing…");
    UpdateEnable();
}

void Finished() {
    std::shared_ptr<Job> j = U.job;
    if (!j) return;
    const int done = j->done - j->failed - j->skipped;
    const double secs = (GetTickCount64() - j->start) / 1000.0;
    Log(L"Resize finished: %d done, %d skipped, %d failed, %.1f s", done, j->skipped.load(), j->failed.load(), secs);
    std::wstring line = std::to_wstring(done) + (done == 1 ? L" image resized" : L" images resized");
    if (j->skipped) line += L", " + std::to_wstring(j->skipped) + L" already small enough";
    if (j->cancel) line += L" (stopped)";
    if (j->failed) {
        std::wstring msg = line + L".\n\n" + std::to_wstring(j->failed) + L" didn't work:\n";
        for (size_t i = 0; i < j->errors.size() && i < 15; ++i) msg += L"\n" + j->errors[i];
        if (j->errors.size() > 15) msg += L"\n…";
        SetStatus(line + L", " + std::to_wstring(j->failed) + L" failed");
        UpdateEnable();
        MessageBoxW(U.hwnd, msg.c_str(), L"Canivete Resize", MB_ICONWARNING);
        return;
    }
    if (j->cancel) {
        SetStatus(line);
        UpdateEnable();
        return;
    }
    const wchar_t* where = j->o.output == 0   ? L"Saved next to the originals."
                           : j->o.output == 1 ? L"Saved in the chosen folder."
                                              : L"Originals are in the Recycle Bin.";
    Tray_Notify(line.c_str(), where);
    if (!U.queued.empty()) {  // more files arrived meanwhile: keep the window for them
        U.files = std::move(U.queued);
        U.queued.clear();
        U.job.reset();
        ShowFiles();
        SetStatus(line);
        UpdateEnable();
        return;
    }
    DestroyWindow(U.hwnd);
}

LRESULT CALLBACK ResizerProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) U.hwnd = h;
    if (U.hwnd != h) return DefWindowProcW(h, msg, wp, lp);
    switch (msg) {
    case WM_CREATE:
        U.dpi = GetDpiForWindow(h);
        MakeFonts();
        Build();
        Layout();
        return 0;
    case WM_ERASEBKGND: {
        HDC dc = reinterpret_cast<HDC>(wp);
        RECT rc;
        GetClientRect(h, &rc);
        const Palette& p = Pal();
        FillRect(dc, &rc, Theme_Brush(p.window));
        RECT f = rc;
        f.top = Px(U.footerY);
        FillRect(dc, &f, Theme_Brush(p.footer));
        f.bottom = f.top + 1;
        FillRect(dc, &f, Theme_Brush(p.line));
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wp);
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lp));
        const bool footer = id == IDC_STATUS;
        const Palette& p = Pal();
        SetBkColor(dc, footer ? p.footer : p.window);
        SetTextColor(dc, id == IDC_COUNT || footer ? p.grey : p.text);
        return reinterpret_cast<LRESULT>(Theme_Brush(footer ? p.footer : p.window));
    }
    case WM_CTLCOLORBTN: {  // push buttons: the corners around their rounded shape
        RECT r;
        GetWindowRect(reinterpret_cast<HWND>(lp), &r);
        MapWindowPoints(nullptr, h, reinterpret_cast<POINT*>(&r), 2);
        RECT rc;
        GetClientRect(h, &rc);
        return reinterpret_cast<LRESULT>(Theme_Brush(r.top >= Px(U.footerY) ? Pal().footer : Pal().window));
    }
    case DM_GETDEFID:
        return MAKELRESULT(IDOK, DC_HASDEFID);
    case WM_VKEYTOITEM:
        if (LOWORD(wp) == VK_DELETE && !Busy()) {
            RemoveSelected();
            return -2;
        }
        return -1;
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
        if (Busy()) U.queued.insert(U.queued.end(), paths.begin(), paths.end());
        else AddPaths(paths);
        return 0;
    }
    case WM_RS_PROGRESS:
        if (U.job) {
            wchar_t t[64];
            swprintf_s(t, L"Resizing…  %d of %zu", U.job->done.load(), U.job->files.size());
            SetStatus(t);
        }
        return 0;
    case WM_RS_FINISHED:
        Finished();
        return 0;
    case WM_COMMAND: {
        const int id = LOWORD(wp), code = HIWORD(wp);
        switch (id) {
        case IDOK:
            if (Busy()) U.job->cancel = true;
            else Start();
            break;
        case IDCANCEL:
            if (Busy()) U.job->cancel = true;
            DestroyWindow(h);
            break;
        case IDC_ADD:
            AddDialog();
            break;
        case IDC_BROWSE:
            Browse();
            break;
        case IDC_SIZE0:
        case IDC_SIZE0 + 1:
        case IDC_SIZE0 + 2:
        case IDC_SIZE0 + 3:
            UpdateEnable();
            break;
        case IDC_W:
        case IDC_H:
            if (code == EN_SETFOCUS && SizeChoice() != kCustom) {
                for (int i = 0; i <= kCustom; ++i) SetCheck(IDC_SIZE0 + i, i == kCustom);
                UpdateEnable();
            }
            break;
        case IDC_UNIT:
        case IDC_OUTPUT:
        case IDC_FORMAT:
            if (code == CBN_SELCHANGE) UpdateEnable();
            break;
        }
        return 0;
    }
    case WM_DPICHANGED: {
        U.dpi = HIWORD(wp);
        MakeFonts();
        const RECT* sug = reinterpret_cast<const RECT*>(lp);
        const SIZE sz = WindowSize();
        SetWindowPos(h, nullptr, sug->left, sug->top, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout();
        return 0;
    }
    case WM_DESTROY:
        if (Busy()) U.job->cancel = true;
        U.job.reset();
        U.hwnd = nullptr;
        U.ctls.clear();
        U.files.clear();
        U.queued.clear();
        return 0;
    case WM_NCDESTROY:
        DeleteObject(U.font);
        DeleteObject(U.bold);
        U.font = U.bold = nullptr;
        break;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ---- quick actions (Explorer's Canivete submenu): no window, a tray notice when done ----------

std::vector<std::shared_ptr<Job>> g_quick;
HWND g_quickWnd = nullptr;

std::wstring Count(int n, const wchar_t* one, const wchar_t* many) {
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

void QuickFinished(const Job& j) {
    const int done = j.done - j.failed - j.skipped;
    Log(L"Quick action finished: %s, %d done, %d skipped, %d failed", j.what.c_str(), done, j.skipped.load(),
        j.failed.load());
    const std::wstring title = Count(done, L"image", L"images") + L" " + j.what;
    std::wstring text = j.o.inPlace ? L"Saved over the originals." : L"Saved next to the originals.";
    if (j.skipped)
        text = Count(j.skipped, L"image was", L"images were") + (j.o.convert ? L" already in that format. " : L" skipped. ") +
               text;
    if (!j.failed) {
        Tray_Notify(title.c_str(), text);
        return;
    }
    std::wstring msg = title + L".\n\n" + std::to_wstring(j.failed.load()) + L" didn't work:\n";
    for (size_t i = 0; i < j.errors.size() && i < 15; ++i) msg += L"\n" + j.errors[i];
    if (j.errors.size() > 15) msg += L"\n…";
    if (TestMode()) Log(L"Notice: %s", msg.c_str());
    else MessageBoxW(nullptr, msg.c_str(), L"Canivete", MB_ICONWARNING | MB_SETFOREGROUND);
}

LRESULT CALLBACK QuickProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_RS_FINISHED) {
        auto it = std::find_if(g_quick.begin(), g_quick.end(),
                               [&](const std::shared_ptr<Job>& j) { return reinterpret_cast<LPARAM>(j.get()) == lp; });
        if (it != g_quick.end()) {
            const std::shared_ptr<Job> j = *it;
            g_quick.erase(it);
            QuickFinished(*j);
        }
        return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND QuickWindow() {
    if (g_quickWnd) return g_quickWnd;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = QuickProc;
    wc.hInstance = g_hinst;
    wc.lpszClassName = L"CaniveteQuick";
    RegisterClassExW(&wc);
    g_quickWnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, g_hinst, nullptr);
    return g_quickWnd;
}

// Copy to Clipboard: the first image, upright, see-through parts on white.
void CopyFirst(const std::wstring& file) {
    ComPtr<IWICImagingFactory> f;
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICMetadataBlockReader> meta;
    ComPtr<IWICBitmapSource> bgr;
    GUID fmt{};
    bool alpha = false, skipped = false;
    UINT orient = 1, w = 0, h = 0;
    Options o;
    o.percent = true;
    o.w = 100;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
    if (SUCCEEDED(hr)) hr = Prepare(f.Get(), file, o, bmp, fmt, alpha, skipped, meta, orient);
    if (SUCCEEDED(hr) && alpha) FlattenOnWhite(bmp.Get());
    if (SUCCEEDED(hr)) hr = WICConvertBitmapSource(GUID_WICPixelFormat32bppBGR, bmp.Get(), &bgr);
    Image img;
    if (SUCCEEDED(hr)) hr = bgr->GetSize(&w, &h);
    if (SUCCEEDED(hr)) {
        img.w = static_cast<int>(w);
        img.h = static_cast<int>(h);
        img.px.resize(static_cast<size_t>(w) * h);
        hr = bgr->CopyPixels(nullptr, w * 4, static_cast<UINT>(img.px.size() * 4), reinterpret_cast<BYTE*>(img.px.data()));
    }
    const std::wstring name = file.substr(file.find_last_of(L"\\/") + 1);
    if (FAILED(hr)) {
        Log(L"Copy image failed: %s (%s)", file.c_str(), HrText(hr).c_str());
        Tray_Notify(L"Couldn't copy the image", name + L": " + HrText(hr));
        return;
    }
    Output_CopyImage(img);
    Log(L"Copied image to the clipboard: %s (%dx%d)", file.c_str(), img.w, img.h);
    Tray_Notify(L"Image copied", name + L"  (" + std::to_wstring(w) + L" × " + std::to_wstring(h) + L")");
}

}  // namespace

void Resizer_Quick(const std::wstring& action, const std::vector<std::wstring>& paths) {
    const std::wstring a = Lower(action);
    if (a == L"custom") {
        Resizer_Open(paths);
        return;
    }
    std::vector<std::wstring> files;
    for (const std::wstring& p : paths) {
        const DWORD attr = GetFileAttributesW(p.c_str());
        if (attr == INVALID_FILE_ATTRIBUTES || (attr & FILE_ATTRIBUTE_DIRECTORY) || !IsImageName(p)) continue;
        const std::wstring lp = Lower(p);
        if (std::none_of(files.begin(), files.end(), [&](const std::wstring& x) { return Lower(x) == lp; }))
            files.push_back(p);
    }
    if (files.empty()) return;
    if (a == L"copy") {
        CopyFirst(files[0]);
        return;
    }

    auto j = std::make_shared<Job>();
    Options& o = j->o;
    o.quality = std::clamp(IniInt(L"JpegQuality", 90), 1, 100);
    o.keepMeta = IniInt(L"KeepMetadata", 1) != 0;
    o.percent = true;  // convert and turn keep the size
    o.w = 100;
    struct Turn {
        const wchar_t* key;
        WICBitmapTransformOptions t;
        const wchar_t* what;
    };
    const Turn turns[] = {{L"right", WICBitmapTransformRotate90, L"turned right"},
                          {L"left", WICBitmapTransformRotate270, L"turned left"},
                          {L"180", WICBitmapTransformRotate180, L"turned upside down"},
                          {L"fliph", WICBitmapTransformFlipHorizontal, L"flipped left to right"},
                          {L"flipv", WICBitmapTransformFlipVertical, L"flipped top to bottom"}};
    const wchar_t* const formats[] = {L"", L"jpg", L"png", L"tif", L"bmp"};
    const wchar_t* const formatNames[] = {L"", L"JPG", L"PNG", L"TIFF", L"BMP"};
    bool known = false;
    for (const Preset& p : kPresets)
        if (a == p.key) {
            o.percent = false;
            o.w = p.w;
            o.h = p.h;
            o.suffix = std::wstring(L" (") + p.name + L")";
            j->what = std::wstring(L"resized to ") + p.name;
            known = true;
        }
    if (a == L"half") {
        o.w = 50;
        o.suffix = L" (50%)";
        j->what = L"resized to half";
        known = true;
    }
    for (int i = 1; i < 5; ++i)
        if (a == formats[i]) {
            o.format = i;
            o.convert = true;
            j->what = std::wstring(L"converted to ") + formatNames[i];
            known = true;
        }
    for (const Turn& t : turns)
        if (a == t.key) {
            o.turn = t.t;
            o.inPlace = true;
            o.quality = std::max(o.quality, 95);  // saved again over the original: keep it close
            j->what = t.what;
            known = true;
        }
    if (!known) {
        Log(L"Unknown quick action: %s", action.c_str());
        return;
    }
    j->files = std::move(files);
    j->start = GetTickCount64();
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1,
                                   std::min(8, static_cast<int>(j->files.size())));
    j->running = threads;
    g_quick.push_back(j);
    HWND notify = QuickWindow();
    Log(L"Quick action %s: %zu images, %d threads", a.c_str(), j->files.size(), threads);
    for (int i = 0; i < threads; ++i) std::thread(Worker, j, notify).detach();
}

HWND Resizer_Window() { return U.hwnd; }

void Resizer_Open(const std::vector<std::wstring>& files) {
    if (U.hwnd) {
        if (Busy()) U.queued.insert(U.queued.end(), files.begin(), files.end());
        else AddPaths(files);
        if (IsIconic(U.hwnd)) ShowWindow(U.hwnd, SW_RESTORE);
        SetForegroundWindow(U.hwnd);
        return;
    }
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{sizeof(wc)};
        wc.lpfnWndProc = ResizerProc;
        wc.hInstance = g_hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_LARGE, &wc.hIcon);
        LoadIconMetric(g_hinst, MAKEINTRESOURCEW(IDI_APP), LIM_SMALL, &wc.hIconSm);
        registered = RegisterClassExW(&wc) != 0;
    }
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    HWND h = CreateWindowExW(kExStyle, kClass, L"Canivete Resize", kStyle, mi.rcWork.left, mi.rcWork.top, 100, 100,
                             nullptr, nullptr, g_hinst, nullptr);
    if (!h) return;
    const SIZE sz = WindowSize();
    const RECT& w = mi.rcWork;
    const int x = w.left + std::max(0L, (w.right - w.left - sz.cx) / 2);
    const int y = w.top + std::max(0L, (w.bottom - w.top - sz.cy) / 2);
    SetWindowPos(h, nullptr, x, y, sz.cx, sz.cy, SWP_NOZORDER | SWP_NOACTIVATE);
    Populate();
    AddPaths(files);
    Theme_Window(h);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    SetFocus(Item(IDOK));
}
