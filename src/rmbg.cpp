// Remove background: saves a picture as a PNG whose background is transparent.
//
// The cut-out comes from BiRefNet (MIT license, github.com/ZhengPeng7/BiRefNet), a free AI model,
// run with Microsoft's ONNX Runtime on the graphics card (DirectML), or on the processor when there
// is no usable graphics card. Nothing is bundled: on first use Canivete downloads, from their
// official homes, the model (onnx-community/BiRefNet-ONNX on Hugging Face, ~490 MB) and the ONNX
// Runtime package (NuGet, ~12 MB; plus Microsoft's DirectML package, ~200 MB, only on a Windows
// whose own DirectML.dll is too old). Every file is pinned to an exact version and checked against
// its SHA-256 before use. The model is then rewritten once (rmbg_model.cpp) so it runs ~10x faster.
// All of it lives in %APPDATA%\Canivete\ai.
//
// The model sees the picture at 1024 x 1024. Its mask is brought back to full size with a guided
// filter (edges follow the real picture, so hair stays fine), and the colors along the edge are
// cleaned of the old background (blur-fusion foreground estimation, the method BiRefNet's authors
// use), so the cut-out has no colored fringe on a new background.
#include "app.h"
#include "theme.h"

#include <bcrypt.h>
#include <commctrl.h>
#include <dxgi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wincodec.h>
#include <winhttp.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "onnxruntime_c_api.h"

using Microsoft::WRL::ComPtr;

bool Rmbg_FixModel(const std::wstring& src, const std::wstring& dst, std::wstring& err);  // rmbg_model.cpp

namespace {

// ---- what gets downloaded --------------------------------------------------------------------
struct Download {
    const wchar_t* url;
    uint64_t size;
    const wchar_t* sha;
};
const Download kModel = {
    L"https://huggingface.co/onnx-community/BiRefNet-ONNX/resolve/534d3c82d3bb8b2f0867db6dfbc3a525b8e42f67/onnx/"
    L"model_fp16.onnx",
    489666272, L"3654c741eb80bd926ada8fed1713b506ccf8d30eb1f6487e87eb9f234f33df09"};
const Download kOrtPkg = {
    L"https://api.nuget.org/v3-flatcontainer/microsoft.ml.onnxruntime.directml/1.24.4/"
    L"microsoft.ml.onnxruntime.directml.1.24.4.nupkg",
    12458649, L"57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681"};
constexpr wchar_t kOrtInPkg[] = L"runtimes/win-x64/native/onnxruntime.dll";
constexpr wchar_t kOrtDllSha[] = L"e7eedec6a6f26dc39dc948276a75ef6d2bee3fff944d874ceed0bbd3b97bff40";
const Download kDmlPkg = {L"https://api.nuget.org/v3-flatcontainer/microsoft.ai.directml/1.15.4/microsoft.ai.directml.1.15.4.nupkg",
                          202292617, L"4e7cb7ddce8cf837a7a75dc029209b520ca0101470fcdf275c1f49736a3615b9"};
constexpr wchar_t kDmlInPkg[] = L"bin/x64-win/DirectML.dll";
constexpr wchar_t kDmlDllSha[] = L"9c9e6d822561c6c41b90e6994b3e8857cf1d66dbfb1e0c4c799c7c89b4e92da1";

constexpr int kSide = 1024;           // the model's input size
constexpr int kKeepSessionSec = 180;  // keep the model loaded this long after the last picture

std::wstring AiDir() { return AppDataDir() + L"\\ai"; }
std::wstring ModelPath() { return AiDir() + L"\\birefnet-general-fp16-fast.onnx"; }
std::wstring RuntimeDir() { return AiDir() + L"\\runtime"; }
bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

std::wstring Utf16(const char* s) {
    if (!s) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s, -1, w.data(), n);
    return w;
}

std::wstring FileName(const std::wstring& p) {
    const size_t s = p.find_last_of(L"\\/");
    return s == std::wstring::npos ? p : p.substr(s + 1);
}

// Windows' own DirectML.dll is new enough for this ONNX Runtime (1.15 or later)?
bool SystemDirectMLOk() {
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring p = std::wstring(sys) + L"\\DirectML.dll";
    DWORD h = 0;
    const DWORD n = GetFileVersionInfoSizeW(p.c_str(), &h);
    if (!n) return false;
    std::vector<BYTE> buf(n);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(p.c_str(), 0, n, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&fi), &len) ||
        !fi)
        return false;
    const DWORD major = HIWORD(fi->dwFileVersionMS), minor = LOWORD(fi->dwFileVersionMS);
    return major > 1 || (major == 1 && minor >= 15);
}

bool NeedDml() { return !SystemDirectMLOk() && !Exists(RuntimeDir() + L"\\DirectML.dll"); }

// Megabytes still to download before the first use; 0 = ready.
int MissingMB() {
    uint64_t b = 0;
    if (!Exists(ModelPath())) b += kModel.size;
    if (!Exists(RuntimeDir() + L"\\onnxruntime.dll")) b += kOrtPkg.size;
    if (NeedDml()) b += kDmlPkg.size;
    return static_cast<int>((b + (1 << 20) - 1) >> 20);
}

// ---- progress shared with the window ---------------------------------------------------------
struct Progress {
    std::mutex m;
    std::wstring text;
    int pos = -1;  // 0..1000, -1 = busy without a known end
    std::atomic<bool> cancel{false};
    void Set(const std::wstring& t, int p) {
        std::lock_guard<std::mutex> l(m);
        text = t;
        pos = p;
    }
};

// ---- downloads -------------------------------------------------------------------------------
std::wstring Hex(const BYTE* d, size_t n) {
    std::wstring s;
    for (size_t i = 0; i < n; ++i) {
        wchar_t x[3];
        swprintf_s(x, L"%02x", d[i]);
        s += x;
    }
    return s;
}

std::wstring FileSha256(const std::wstring& path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return {};
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE h = nullptr;
    std::wstring hex;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &h, nullptr, 0, nullptr, 0, 0))) {
        std::vector<BYTE> buf(1 << 20);
        DWORD got = 0;
        bool ok = true;
        while (ok && ReadFile(f, buf.data(), static_cast<DWORD>(buf.size()), &got, nullptr) && got)
            ok = BCRYPT_SUCCESS(BCryptHashData(h, buf.data(), got, 0));
        BYTE d[32];
        if (ok && BCRYPT_SUCCESS(BCryptFinishHash(h, d, 32, 0))) hex = Hex(d, 32);
    }
    if (h) BCryptDestroyHash(h);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    CloseHandle(f);
    return hex;
}

// Downloads d to path (via path.part), checking size and SHA-256 while it arrives.
// done/total: bytes already fetched by earlier downloads of this setup and the overall total, for the bar.
bool Fetch(const Download& d, const std::wstring& path, Progress& pr, uint64_t done, uint64_t total, std::wstring& err) {
    URL_COMPONENTS uc{sizeof(uc)};
    wchar_t host[256], urlPath[2048];
    uc.lpszHostName = host;
    uc.dwHostNameLength = 256;
    uc.lpszUrlPath = urlPath;
    uc.dwUrlPathLength = 2048;
    if (!WinHttpCrackUrl(d.url, 0, 0, &uc)) {
        err = L"Bad download address.";
        return false;
    }
    const std::wstring part = path + L".part";
    const std::wstring agent = std::wstring(L"Canivete/") + CANIVETE_VERSION;
    HINTERNET s = WinHttpOpen(agent.c_str(), WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    HINTERNET c = s ? WinHttpConnect(s, host, uc.nPort, 0) : nullptr;
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", urlPath, nullptr, nullptr, nullptr, WINHTTP_FLAG_SECURE) : nullptr;
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hh = nullptr;
    bool ok = false;
    err = L"Couldn't download the AI files (no internet?).";
    if (r && BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0)) &&
        BCRYPT_SUCCESS(BCryptCreateHash(alg, &hh, nullptr, 0, nullptr, 0, 0))) {
        DWORD redirect = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        WinHttpSetOption(r, WINHTTP_OPTION_REDIRECT_POLICY, &redirect, sizeof(redirect));
        WinHttpSetTimeouts(r, 15000, 15000, 30000, 60000);
        if (WinHttpSendRequest(r, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
            DWORD status = 0, sz = sizeof(status);
            WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &status, &sz, nullptr);
            HANDLE f = status == 200 ? CreateFileW(part.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr)
                                     : INVALID_HANDLE_VALUE;
            if (status != 200) err = L"The download server answered " + std::to_wstring(status) + L".";
            if (f != INVALID_HANDLE_VALUE) {
                std::vector<char> buf(1 << 18);
                uint64_t got = 0;
                ok = true;
                ULONGLONG lastShow = 0;
                for (DWORD n = 0; ok;) {
                    if (pr.cancel) {
                        ok = false;
                        err.clear();
                        break;
                    }
                    if (!WinHttpReadData(r, buf.data(), static_cast<DWORD>(buf.size()), &n)) {
                        ok = false;
                        break;
                    }
                    if (!n) break;
                    DWORD wrote = 0;
                    if (!WriteFile(f, buf.data(), n, &wrote, nullptr) || wrote != n) {
                        ok = false;
                        err = L"Couldn't save the AI files (disk full?).";
                        break;
                    }
                    BCryptHashData(hh, reinterpret_cast<BYTE*>(buf.data()), n, 0);
                    got += n;
                    const ULONGLONG now = GetTickCount64();
                    if (now - lastShow > 200) {
                        lastShow = now;
                        pr.Set(L"Downloading the AI model (one time only)…  " + std::to_wstring((done + got) >> 20) + L" of " +
                                   std::to_wstring(total >> 20) + L" MB",
                               static_cast<int>((done + got) * 1000 / std::max<uint64_t>(total, 1)));
                    }
                }
                CloseHandle(f);
                BYTE dg[32];
                if (ok && (got != d.size || !BCRYPT_SUCCESS(BCryptFinishHash(hh, dg, 32, 0)) || Hex(dg, 32) != d.sha)) {
                    ok = false;
                    err = L"The downloaded AI file is damaged or not the expected one. Try again.";
                    Log(L"rmbg: %s: %llu bytes, wrong size or SHA-256", d.url, got);
                }
            }
        }
    }
    if (hh) BCryptDestroyHash(hh);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    for (HINTERNET h : {r, c, s})
        if (h) WinHttpCloseHandle(h);
    if (ok) ok = MoveFileExW(part.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != FALSE;
    if (!ok) DeleteFileW(part.c_str());
    return ok;
}

// Takes one file out of a .nupkg (a zip) with Windows' own tar.exe, and checks it.
bool Extract(const std::wstring& pkg, const wchar_t* inner, const wchar_t* sha, const std::wstring& dst, std::wstring& err) {
    err = L"Couldn't unpack the AI runtime.";
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    const std::wstring tmp = AiDir() + L"\\unpack";
    SHCreateDirectoryExW(nullptr, tmp.c_str(), nullptr);
    std::wstring cmd = L"\"" + std::wstring(sys) + L"\\tar.exe\" -xf \"" + pkg + L"\" -C \"" + tmp + L"\" " + inner;
    STARTUPINFOW si{sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) return false;
    WaitForSingleObject(pi.hProcess, 120000);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    std::wstring got = tmp + L"\\" + inner;
    std::replace(got.begin(), got.end(), L'/', L'\\');
    bool ok = code == 0 && FileSha256(got) == sha && MoveFileExW(got.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING);
    // remove the unpack folder
    std::wstring dbl = tmp + L'\0';
    SHFILEOPSTRUCTW op{};
    op.wFunc = FO_DELETE;
    op.pFrom = dbl.c_str();
    op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);
    if (ok) err.clear();
    return ok;
}

// Downloads and prepares whatever is missing. Runs on the worker.
bool Setup(Progress& pr, std::wstring& err) {
    const std::wstring ai = AiDir(), rt = RuntimeDir();
    SHCreateDirectoryExW(nullptr, rt.c_str(), nullptr);
    const bool needModel = !Exists(ModelPath()), needOrt = !Exists(rt + L"\\onnxruntime.dll"), needDml = NeedDml();
    if (!needModel && !needOrt && !needDml) return true;
    const uint64_t total = (needModel ? kModel.size : 0) + (needOrt ? kOrtPkg.size : 0) + (needDml ? kDmlPkg.size : 0);
    uint64_t done = 0;
    Log(L"rmbg: setup, downloading %llu MB (model %d, runtime %d, DirectML %d)", total >> 20, needModel, needOrt, needDml);
    if (needOrt) {
        const std::wstring pkg = ai + L"\\onnxruntime.nupkg";
        if (!Fetch(kOrtPkg, pkg, pr, done, total, err)) return false;
        const bool ok = Extract(pkg, kOrtInPkg, kOrtDllSha, rt + L"\\onnxruntime.dll", err);
        DeleteFileW(pkg.c_str());
        if (!ok) return false;
        done += kOrtPkg.size;
    }
    if (needDml) {
        const std::wstring pkg = ai + L"\\directml.nupkg";
        if (!Fetch(kDmlPkg, pkg, pr, done, total, err)) return false;
        const bool ok = Extract(pkg, kDmlInPkg, kDmlDllSha, rt + L"\\DirectML.dll", err);
        DeleteFileW(pkg.c_str());
        if (!ok) return false;
        done += kDmlPkg.size;
    }
    if (needModel) {
        const std::wstring raw = ai + L"\\birefnet-general-fp16.onnx";
        if (!Fetch(kModel, raw, pr, done, total, err)) return false;
        pr.Set(L"Preparing the AI model (one time only)…", -1);
        const std::wstring tmp = ModelPath() + L".tmp";
        const bool ok = Rmbg_FixModel(raw, tmp, err) && MoveFileExW(tmp.c_str(), ModelPath().c_str(), MOVEFILE_REPLACE_EXISTING);
        DeleteFileW(tmp.c_str());
        DeleteFileW(raw.c_str());
        if (!ok) return false;
    }
    return true;
}

// ---- ONNX Runtime ----------------------------------------------------------------------------
class Ai {
public:
    ~Ai() { Unload(); }
    bool Loaded() const { return sess_ != nullptr; }
    bool Gpu() const { return gpu_; }

    bool Load(Progress& pr, std::wstring& err) {
        if (sess_) return true;
        pr.Set(L"Starting the AI…", -1);
        const ULONGLONG t0 = GetTickCount64();
        if (!api_) {
            const std::wstring rt = RuntimeDir();
            if (Exists(rt + L"\\DirectML.dll")) LoadLibraryExW((rt + L"\\DirectML.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            dll_ = LoadLibraryExW((rt + L"\\onnxruntime.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            using GetBase = const OrtApiBase*(ORT_API_CALL*)();
            auto base = dll_ ? reinterpret_cast<GetBase>(GetProcAddress(dll_, "OrtGetApiBase")) : nullptr;
            api_ = base ? base()->GetApi(ORT_API_VERSION) : nullptr;
            if (!api_) {
                err = L"Couldn't start the AI runtime (error " + std::to_wstring(GetLastError()) + L").";
                return false;
            }
            if (!Ok(api_->CreateEnv(ORT_LOGGING_LEVEL_ERROR, "canivete", &env_), err) ||
                !Ok(api_->CreateCpuMemoryInfo(OrtArenaAllocator, OrtMemTypeDefault, &mem_), err))
                return false;
        }
        const int dev = BestAdapter();
        gpu_ = false;
        if (dev >= 0 && Create(dev, err)) gpu_ = true;
        else if (dev >= 0) Log(L"rmbg: graphics card failed (%s), using the processor", err.c_str());
        if (!sess_ && !Create(-1, err)) return false;
        Log(L"rmbg: model loaded on the %s in %llu ms", gpu_ ? L"graphics card" : L"processor", GetTickCount64() - t0);
        return true;
    }

    void Unload() {
        if (api_ && sess_) api_->ReleaseSession(sess_);
        sess_ = nullptr;
    }

    // in: 3 x 1024 x 1024 normalized RGB; out: 1024 x 1024 logits
    bool Run(std::vector<float>& in, std::vector<float>& out, std::wstring& err) {
        const int64_t shape[4] = {1, 3, kSide, kSide};
        OrtValue* v = nullptr;
        OrtValue* o = nullptr;
        if (!Ok(api_->CreateTensorWithDataAsOrtValue(mem_, in.data(), in.size() * sizeof(float), shape, 4,
                                                     ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT, &v),
                err))
            return false;
        const char* inName = "input_image";
        const char* outName = "output_image";
        bool ok = Ok(api_->Run(sess_, nullptr, &inName, &v, 1, &outName, 1, &o), err);
        float* p = nullptr;
        if (ok) ok = Ok(api_->GetTensorMutableData(o, reinterpret_cast<void**>(&p)), err);
        if (ok) out.assign(p, p + kSide * kSide);
        if (o) api_->ReleaseValue(o);
        api_->ReleaseValue(v);
        return ok;
    }

private:
    bool Ok(OrtStatus* s, std::wstring& err) {
        if (!s) return true;
        err = L"The AI failed: " + Utf16(api_->GetErrorMessage(s));
        api_->ReleaseStatus(s);
        return false;
    }

    // dev < 0: processor only
    bool Create(int dev, std::wstring& err) {
        OrtSessionOptions* so = nullptr;
        if (!Ok(api_->CreateSessionOptions(&so), err)) return false;
        bool ok = Ok(api_->SetSessionGraphOptimizationLevel(so, ORT_ENABLE_ALL), err);
        if (ok && dev >= 0) {
            using AppendDml = OrtStatus*(ORT_API_CALL*)(OrtSessionOptions*, int);
            auto append = reinterpret_cast<AppendDml>(GetProcAddress(dll_, "OrtSessionOptionsAppendExecutionProvider_DML"));
            ok = append && Ok(api_->DisableMemPattern(so), err) && Ok(api_->SetSessionExecutionMode(so, ORT_SEQUENTIAL), err) &&
                 Ok(append(so, dev), err);
            if (!append) err = L"no DirectML in the runtime";
        }
        if (ok) ok = Ok(api_->CreateSession(env_, ModelPath().c_str(), so, &sess_), err);
        api_->ReleaseSessionOptions(so);
        return ok;
    }

    // The graphics card with the most memory (DirectML's device number = DXGI's adapter order).
    static int BestAdapter() {
        ComPtr<IDXGIFactory1> f;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return -1;
        int best = -1;
        SIZE_T mem = 0;
        ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; f->EnumAdapters1(i, a.ReleaseAndGetAddressOf()) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d;
            if (FAILED(a->GetDesc1(&d)) || (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) || d.VendorId == 0x1414) continue;
            if (best < 0 || d.DedicatedVideoMemory > mem) {
                best = static_cast<int>(i);
                mem = d.DedicatedVideoMemory;
            }
        }
        return best;
    }

    HMODULE dll_ = nullptr;
    const OrtApi* api_ = nullptr;
    OrtEnv* env_ = nullptr;
    OrtMemoryInfo* mem_ = nullptr;
    OrtSession* sess_ = nullptr;
    bool gpu_ = false;
};

// ---- picture math ----------------------------------------------------------------------------
struct Plane {
    int w = 0, h = 0;
    std::vector<float> v;
    Plane() = default;
    Plane(int w_, int h_) : w(w_), h(h_), v(static_cast<size_t>(w_) * h_) {}
    float* Row(int y) { return v.data() + static_cast<size_t>(y) * w; }
    const float* Row(int y) const { return v.data() + static_cast<size_t>(y) * w; }
};

void ParallelFor(int n, const std::function<void(int, int)>& fn) {
    const int t = std::max(1, std::min<int>(static_cast<int>(std::thread::hardware_concurrency()), n / 16));
    if (t <= 1) {
        fn(0, n);
        return;
    }
    std::vector<std::thread> th;
    for (int i = 0; i < t; ++i) th.emplace_back(fn, n * i / t, n * (i + 1) / t);
    for (auto& x : th) x.join();
}

// Mean over a (2r+1)^2 square, edges averaged over the pixels that exist.
void BoxMean(Plane& p, int r) {
    if (r <= 0 || p.w == 0 || p.h == 0) return;
    const int w = p.w, h = p.h;
    ParallelFor(h, [&](int y0, int y1) {
        std::vector<float> line(w);
        for (int y = y0; y < y1; ++y) {
            float* row = p.Row(y);
            std::copy(row, row + w, line.begin());
            double s = 0;
            int lo = 0, hi = std::min(r, w - 1);
            for (int x = 0; x <= hi; ++x) s += line[x];
            for (int x = 0; x < w; ++x) {
                row[x] = static_cast<float>(s / (hi - lo + 1));
                if (x + 1 + r < w) {
                    s += line[x + 1 + r];
                    hi = x + 1 + r;
                }
                if (x - r >= 0) {
                    s -= line[x - r];
                    lo = x - r + 1;
                }
            }
        }
    });
    ParallelFor(w, [&](int x0, int x1) {
        const int n = x1 - x0, ringN = 2 * r + 2;
        if (n <= 0) return;
        std::vector<double> acc(n, 0.0);
        std::vector<float> ring(static_cast<size_t>(ringN) * n);
        auto keep = [&](int y) {
            float* dst = ring.data() + static_cast<size_t>(y % ringN) * n;
            const float* src = p.Row(y) + x0;
            for (int i = 0; i < n; ++i) {
                dst[i] = src[i];
                acc[i] += src[i];
            }
        };
        int lo = 0, hi = std::min(r, h - 1);
        for (int y = 0; y <= hi; ++y) keep(y);
        for (int y = 0; y < h; ++y) {
            const double inv = 1.0 / (hi - lo + 1);
            float* row = p.Row(y) + x0;
            for (int i = 0; i < n; ++i) row[i] = static_cast<float>(acc[i] * inv);
            if (y + 1 + r < h) {
                keep(y + 1 + r);
                hi = y + 1 + r;
            }
            if (y - r >= 0) {
                const float* old = ring.data() + static_cast<size_t>((y - r) % ringN) * n;
                for (int i = 0; i < n; ++i) acc[i] -= old[i];
                lo = y - r + 1;
            }
        }
    });
}

// Bilinear lookup of a view plane at the pixel centers of a bigger (or smaller) one.
struct Sampler {
    std::vector<int> x0, x1, y0, y1;
    std::vector<float> fx, fy;
    Sampler(int sw, int sh, int dw, int dh) {
        auto axis = [](int s, int d, std::vector<int>& a, std::vector<int>& b, std::vector<float>& f) {
            a.resize(d);
            b.resize(d);
            f.resize(d);
            for (int i = 0; i < d; ++i) {
                float c = (i + 0.5f) * s / d - 0.5f;
                c = std::clamp(c, 0.0f, static_cast<float>(s - 1));
                a[i] = static_cast<int>(c);
                b[i] = std::min(a[i] + 1, s - 1);
                f[i] = c - a[i];
            }
        };
        axis(sw, dw, x0, x1, fx);
        axis(sh, dh, y0, y1, fy);
    }
    void Row(const Plane& p, int y, float* out) const {
        const float* r0 = p.Row(y0[y]);
        const float* r1 = p.Row(y1[y]);
        const float t = fy[y];
        for (size_t x = 0; x < x0.size(); ++x) {
            const float a = r0[x0[x]] + (r0[x1[x]] - r0[x0[x]]) * fx[x];
            const float b = r1[x0[x]] + (r1[x1[x]] - r1[x0[x]]) * fx[x];
            out[x] = a + (b - a) * t;
        }
    }
};

Plane Resize(const Plane& p, int w, int h) {
    if (p.w == w && p.h == h) return p;
    Plane o(w, h);
    const Sampler s(p.w, p.h, w, h);
    for (int y = 0; y < h; ++y) s.Row(p, y, o.Row(y));
    return o;
}

// Scales BGRA pixels with WIC (Fant: proper averaging when shrinking).
bool Scale(IWICImagingFactory* f, const Image& img, int w, int h, std::vector<uint32_t>& out) {
    ComPtr<IWICBitmap> bmp;
    ComPtr<IWICBitmapScaler> sc;
    out.assign(static_cast<size_t>(w) * h, 0);
    return SUCCEEDED(f->CreateBitmapFromMemory(img.w, img.h, GUID_WICPixelFormat32bppBGRA, img.w * 4,
                                               static_cast<UINT>(img.px.size() * 4),
                                               reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.px.data())), &bmp)) &&
           SUCCEEDED(f->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(bmp.Get(), w, h, WICBitmapInterpolationModeFant)) &&
           SUCCEEDED(sc->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.size() * 4), reinterpret_cast<BYTE*>(out.data())));
}

inline float Ch(uint32_t px, int c) { return static_cast<float>((px >> (8 * c)) & 255) * (1.0f / 255.0f); }  // c: 0 B, 1 G, 2 R
inline float Gray(uint32_t px) { return (Ch(px, 0) + Ch(px, 1) + Ch(px, 2)) * (1.0f / 3.0f); }

// Cuts img (BGRA, straight alpha) out: sets its alpha to the subject and cleans the edge colors.
bool Cutout(Ai& ai, IWICImagingFactory* f, Image& img, std::wstring& err) {
    const int W = img.w, H = img.h;
    const ULONGLONG t0 = GetTickCount64();
    // 1. the model's view: 1024 x 1024, RGB, ImageNet-normalized
    std::vector<uint32_t> view;
    if (!Scale(f, img, kSide, kSide, view)) {
        err = L"Couldn't scale the picture.";
        return false;
    }
    std::vector<float> in(3 * kSide * kSide), logits;
    const float mean[3] = {0.485f, 0.456f, 0.406f}, sd[3] = {0.229f, 0.224f, 0.225f};
    for (int c = 0; c < 3; ++c) {  // planes R, G, B
        float* d = in.data() + static_cast<size_t>(c) * kSide * kSide;
        for (int i = 0; i < kSide * kSide; ++i) d[i] = (Ch(view[i], 2 - c) - mean[c]) / sd[c];
    }
    const ULONGLONG t1 = GetTickCount64();
    if (!ai.Run(in, logits, err)) return false;
    const ULONGLONG t2 = GetTickCount64();
    Plane mask(kSide, kSide);
    for (int i = 0; i < kSide * kSide; ++i) mask.v[i] = 1.0f / (1.0f + std::exp(-logits[i]));

    // 2. working size: the picture with its long side at 1024 (or the picture itself when smaller)
    const double s = std::max(W, H) / static_cast<double>(kSide);
    const int lw = s > 1 ? std::max(1, static_cast<int>(std::lround(W / s))) : W;
    const int lh = s > 1 ? std::max(1, static_cast<int>(std::lround(H / s))) : H;
    std::vector<uint32_t> lowPx;
    if (lw == W && lh == H) lowPx = img.px;
    else if (!Scale(f, img, lw, lh, lowPx)) {
        err = L"Couldn't scale the picture.";
        return false;
    }
    const Plane p = Resize(mask, lw, lh);
    Plane g(lw, lh);
    for (size_t i = 0; i < g.v.size(); ++i) g.v[i] = Gray(lowPx[i]);

    // 3. guided filter at working size: the mask's edges snap to the picture's edges
    constexpr int kR = 4;
    constexpr float kEps = 1e-3f;
    Plane mI = g, mp = p, cIp(lw, lh), vI(lw, lh);
    for (size_t i = 0; i < g.v.size(); ++i) {
        cIp.v[i] = g.v[i] * p.v[i];
        vI.v[i] = g.v[i] * g.v[i];
    }
    BoxMean(mI, kR);
    BoxMean(mp, kR);
    BoxMean(cIp, kR);
    BoxMean(vI, kR);
    Plane A(lw, lh), B(lw, lh);
    for (size_t i = 0; i < A.v.size(); ++i) {
        const float var = vI.v[i] - mI.v[i] * mI.v[i], cov = cIp.v[i] - mI.v[i] * mp.v[i];
        A.v[i] = cov / (var + kEps);
        B.v[i] = mp.v[i] - A.v[i] * mI.v[i];
    }
    BoxMean(A, kR);
    BoxMean(B, kR);

    // alpha at working size, for the first color pass
    Plane al(lw, lh);
    for (size_t i = 0; i < al.v.size(); ++i) al.v[i] = std::clamp(A.v[i] * g.v[i] + B.v[i], 0.0f, 1.0f);

    // 4. full-size alpha: a * gray + b, with a and b smoothly enlarged
    Plane alpha(W, H);
    {
        const Sampler up(lw, lh, W, H);
        std::vector<float> ra(W), rb(W);
        for (int y = 0; y < H; ++y) {
            up.Row(A, y, ra.data());
            up.Row(B, y, rb.data());
            float* d = alpha.Row(y);
            const uint32_t* src = img.px.data() + static_cast<size_t>(y) * W;
            for (int x = 0; x < W; ++x) {
                const float a = std::clamp(ra[x] * Gray(src[x]) + rb[x], 0.0f, 1.0f);
                d[x] = a * Ch(src[x], 3);  // keep what was already transparent
            }
        }
    }
    const ULONGLONG t3 = GetTickCount64();

    // 5. edge colors without the old background (blur-fusion, two passes: wide at working size,
    //    then narrow at full size)
    const int r1 = std::max(1, static_cast<int>(std::lround(90.0 * std::max(lw, lh) / kSide)));
    const int r2 = std::clamp(static_cast<int>(std::lround(6.0 * std::max(1.0, s))), 3, 24);
    constexpr float e = 1e-5f;
    Plane bA = al;
    BoxMean(bA, r1);
    Plane bF[3], bB[3];
    for (int c = 0; c < 3; ++c) {
        bF[c] = Plane(lw, lh);
        bB[c] = Plane(lw, lh);
        for (size_t i = 0; i < al.v.size(); ++i) {
            const float I = Ch(lowPx[i], c);
            bF[c].v[i] = I * al.v[i];
            bB[c].v[i] = I * (1 - al.v[i]);
        }
        BoxMean(bF[c], r1);
        BoxMean(bB[c], r1);
        for (size_t i = 0; i < al.v.size(); ++i) {
            bF[c].v[i] /= bA.v[i] + e;
            bB[c].v[i] /= (1 - bA.v[i]) + e;
        }
    }
    Plane bA2 = alpha;
    BoxMean(bA2, r2);
    std::vector<float> outC[3];
    {
        const Sampler up(lw, lh, W, H);
        Plane T1(W, H), T2(W, H);
        std::vector<float> rf(W), rb(W);
        for (int c = 0; c < 3; ++c) {
            for (int y = 0; y < H; ++y) {
                up.Row(bF[c], y, rf.data());
                up.Row(bB[c], y, rb.data());
                const float* a = alpha.Row(y);
                const uint32_t* src = img.px.data() + static_cast<size_t>(y) * W;
                float* q1 = T1.Row(y);
                float* q2 = T2.Row(y);
                for (int x = 0; x < W; ++x) {
                    const float I = Ch(src[x], c);
                    const float F1 = std::clamp(rf[x] + a[x] * (I - a[x] * rf[x] - (1 - a[x]) * rb[x]), 0.0f, 1.0f);
                    q1[x] = F1 * a[x];
                    q2[x] = rb[x] * (1 - a[x]);
                }
            }
            BoxMean(T1, r2);
            BoxMean(T2, r2);
            outC[c].resize(static_cast<size_t>(W) * H);
            ParallelFor(H, [&](int ya, int yb) {
                for (int y = ya; y < yb; ++y) {
                    const float* a = alpha.Row(y);
                    const float* ba = bA2.Row(y);
                    const float* q1 = T1.Row(y);
                    const float* q2 = T2.Row(y);
                    const uint32_t* src = img.px.data() + static_cast<size_t>(y) * W;
                    float* o = outC[c].data() + static_cast<size_t>(y) * W;
                    for (int x = 0; x < W; ++x) {
                        const float I = Ch(src[x], c);
                        if (a[x] >= 0.999f) {
                            o[x] = I;
                            continue;
                        }
                        const float F = q1[x] / (ba[x] + e), Bc = q2[x] / ((1 - ba[x]) + e);
                        o[x] = std::clamp(F + a[x] * (I - a[x] * F - (1 - a[x]) * Bc), 0.0f, 1.0f);
                    }
                }
            });
        }
    }
    for (size_t i = 0; i < img.px.size(); ++i) {
        auto b = [](float v) { return static_cast<uint32_t>(v * 255.0f + 0.5f); };
        img.px[i] = b(outC[0][i]) | (b(outC[1][i]) << 8) | (b(outC[2][i]) << 16) | (b(alpha.v[i]) << 24);
    }
    Log(L"rmbg: %dx%d: prepare %llu ms, AI %llu ms (%s), mask %llu ms, colors %llu ms", W, H, t1 - t0, t2 - t1,
        ai.Gpu() ? L"graphics card" : L"processor", t3 - t2, GetTickCount64() - t3);
    return true;
}

// ---- files -----------------------------------------------------------------------------------
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

// Loads a picture upright (EXIF orientation applied) as straight BGRA.
HRESULT LoadUpright(IWICImagingFactory* f, const std::wstring& path, Image& out) {
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> frame;
    HRESULT hr = f->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->GetFrame(0, &frame);
    if (FAILED(hr)) return hr;
    GUID container{};
    dec->GetContainerFormat(&container);
    UINT orient = 1;
    ComPtr<IWICMetadataQueryReader> q;
    if (SUCCEEDED(frame->GetMetadataQueryReader(&q))) {
        PROPVARIANT v;
        PropVariantInit(&v);
        const wchar_t* key = container == GUID_ContainerFormatJpeg ? L"/app1/ifd/{ushort=274}" : L"/ifd/{ushort=274}";
        if (SUCCEEDED(q->GetMetadataByName(key, &v)) && v.vt == VT_UI2 && v.uiVal >= 1 && v.uiVal <= 8) orient = v.uiVal;
        PropVariantClear(&v);
    }
    ComPtr<IWICFormatConverter> conv;
    if (FAILED(hr = f->CreateFormatConverter(&conv)) ||
        FAILED(hr = conv->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0,
                                     WICBitmapPaletteTypeMedianCut)))
        return hr;
    ComPtr<IWICBitmapSource> cur = conv;
    if (orient != 1) {
        ComPtr<IWICBitmap> mem;
        ComPtr<IWICBitmapFlipRotator> rot;
        if (FAILED(hr = f->CreateBitmapFromSource(cur.Get(), WICBitmapCacheOnLoad, &mem)) ||
            FAILED(hr = f->CreateBitmapFlipRotator(&rot)) || FAILED(hr = rot->Initialize(mem.Get(), OrientTransform(orient))))
            return hr;
        cur = rot;
    }
    UINT w = 0, h = 0;
    if (FAILED(hr = cur->GetSize(&w, &h))) return hr;
    if (static_cast<uint64_t>(w) * h > 200'000'000ull) return E_OUTOFMEMORY;
    out.w = static_cast<int>(w);
    out.h = static_cast<int>(h);
    out.px.assign(static_cast<size_t>(w) * h, 0);
    return cur->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.px.size() * 4), reinterpret_cast<BYTE*>(out.px.data()));
}

HRESULT EncodePngAlpha(IWICImagingFactory* f, const Image& img, IStream* st) {
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    HRESULT hr;
    if (FAILED(hr = f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(hr = enc->Initialize(st, WICBitmapEncoderNoCache)) || FAILED(hr = enc->CreateNewFrame(&fr, nullptr)) ||
        FAILED(hr = fr->Initialize(nullptr)) || FAILED(hr = fr->SetSize(img.w, img.h)))
        return hr;
    WICPixelFormatGUID pf = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(hr = fr->SetPixelFormat(&pf))) return hr;
    if (pf != GUID_WICPixelFormat32bppBGRA) return E_FAIL;
    if (FAILED(hr = fr->WritePixels(img.h, img.w * 4, static_cast<UINT>(img.px.size() * 4),
                                    reinterpret_cast<BYTE*>(const_cast<uint32_t*>(img.px.data())))) ||
        FAILED(hr = fr->Commit()))
        return hr;
    return enc->Commit();
}

bool SavePng(IWICImagingFactory* f, const Image& img, const std::wstring& path, std::vector<BYTE>* bytes) {
    ComPtr<IStream> st;
    st.Attach(SHCreateMemStream(nullptr, 0));
    if (!st || FAILED(EncodePngAlpha(f, img, st.Get()))) return false;
    STATSTG stat{};
    st->Stat(&stat, STATFLAG_NONAME);
    std::vector<BYTE> data(static_cast<size_t>(stat.cbSize.QuadPart));
    LARGE_INTEGER zero{};
    ULONG got = 0;
    st->Seek(zero, STREAM_SEEK_SET, nullptr);
    if (FAILED(st->Read(data.data(), static_cast<ULONG>(data.size()), &got)) || got != data.size()) return false;
    if (!path.empty() && !WriteFileBytes(path, data)) return false;
    if (bytes) *bytes = std::move(data);
    return true;
}

// "photo.jpg" -> "photo (no background).png", "photo (no background 2).png", ...
std::wstring OutPath(const std::wstring& src) {
    const size_t slash = src.find_last_of(L"\\/");
    const size_t dot = src.find_last_of(L'.');
    const std::wstring stem = dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash) ? src.substr(0, dot) : src;
    std::wstring p = stem + L" (no background).png";
    for (int i = 2; Exists(p) && i < 1000; ++i) p = stem + L" (no background " + std::to_wstring(i) + L").png";
    return p;
}

std::wstring HrText(HRESULT hr) {
    if (hr == WINCODEC_ERR_COMPONENTNOTFOUND || hr == WINCODEC_ERR_UNKNOWNIMAGEFORMAT) return L"not a picture Windows can read";
    if (hr == E_OUTOFMEMORY) return L"too big";
    if (hr == HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND)) return L"file not found";
    wchar_t b[32];
    swprintf_s(b, L"error 0x%08X", static_cast<unsigned>(hr));
    return b;
}

// ---- the worker and its window ---------------------------------------------------------------
struct Job {
    std::vector<std::wstring> files;   // from Explorer
    std::unique_ptr<Image> img;        // or a picture from the editor
    std::unique_ptr<CaptureResult> meta;
    HWND notify = nullptr;
};

struct Result {
    bool fromEditor = false;
    HWND notify = nullptr;
    std::vector<std::wstring> written, failed;
    std::wstring error;        // setup / AI problem: nothing done
    bool canceled = false;
    std::vector<BYTE> png;     // editor: the result for the clipboard
    Image flat;                // ... and on white, for apps that don't read PNG from the clipboard
};

constexpr UINT kMsgResult = WM_APP + 50;  // to our own window only
constexpr int kIdCancel = 101;

Progress g_pr;
std::mutex g_jm;
std::condition_variable g_jcv;
std::deque<std::unique_ptr<Job>> g_jobs;
bool g_workerAlive = false;
std::atomic<bool> g_busy{false};
HWND g_wnd = nullptr, g_text = nullptr, g_bar = nullptr, g_cancel = nullptr;
HFONT g_font = nullptr;

void Post(std::unique_ptr<Result> r) {
    if (!g_wnd || !PostMessageW(g_wnd, kMsgResult, 0, reinterpret_cast<LPARAM>(r.get()))) return;
    r.release();
}

void RunJob(Ai& ai, IWICImagingFactory* f, Job& j) {
    auto res = std::make_unique<Result>();
    res->fromEditor = j.img != nullptr;
    res->notify = j.notify;
    std::wstring err;
    if (!Setup(g_pr, err) || !ai.Load(g_pr, err)) {
        res->canceled = g_pr.cancel;
        res->error = res->canceled ? L"" : err;
        if (!err.empty()) Log(L"rmbg: %s", err.c_str());
        Post(std::move(res));
        return;
    }
    if (j.img) {
        g_pr.Set(L"Removing the background…", -1);
        Image& img = *j.img;
        for (uint32_t& p : img.px) p |= 0xff000000u;  // editor pictures are opaque
        if (!Cutout(ai, f, img, err)) {
            res->error = err;
        } else {
            const std::wstring path = Output_NewPath(*j.meta, L" (no background).png");
            if (SavePng(f, img, path, &res->png)) res->written.push_back(path);
            else res->error = L"Couldn't save the picture.";
            res->flat = img;
            for (uint32_t& p : res->flat.px) {
                const uint32_t a = p >> 24;
                auto mix = [&](int sh) { return ((((p >> sh) & 255) * a + 255 * (255 - a) + 127) / 255) << sh; };
                p = mix(0) | mix(8) | mix(16) | 0xff000000u;
            }
        }
        Post(std::move(res));
        return;
    }
    const int n = static_cast<int>(j.files.size());
    for (int i = 0; i < n && !g_pr.cancel; ++i) {
        const std::wstring& src = j.files[i];
        g_pr.Set(L"Removing the background:  " + FileName(src) + (n > 1 ? L"  (" + std::to_wstring(i + 1) + L" of " + std::to_wstring(n) + L")" : L""),
                 n > 1 ? i * 1000 / n : -1);
        Image img;
        const HRESULT hr = LoadUpright(f, src, img);
        if (FAILED(hr)) {
            res->failed.push_back(FileName(src) + L": " + HrText(hr));
            continue;
        }
        if (!Cutout(ai, f, img, err)) {
            res->failed.push_back(FileName(src) + L": " + err);
            continue;
        }
        const std::wstring dst = OutPath(src);
        if (SavePng(f, img, dst, nullptr)) res->written.push_back(dst);
        else res->failed.push_back(FileName(src) + L": couldn't save " + FileName(dst));
    }
    res->canceled = g_pr.cancel;
    Post(std::move(res));
}

void Worker() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    {
        ComPtr<IWICImagingFactory> f;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        Ai ai;
        for (;;) {
            std::unique_ptr<Job> j;
            {
                std::unique_lock<std::mutex> l(g_jm);
                if (!g_jcv.wait_for(l, std::chrono::seconds(kKeepSessionSec), [] { return !g_jobs.empty(); })) {
                    g_workerAlive = false;  // idle for a while: free the graphics card memory
                    break;
                }
                j = std::move(g_jobs.front());
                g_jobs.pop_front();
                g_busy = true;
            }
            g_pr.cancel = false;
            if (f) RunJob(ai, f.Get(), *j);
            std::lock_guard<std::mutex> l(g_jm);
            g_busy = !g_jobs.empty();
        }
    }
    CoUninitialize();
}

void Layout(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    const UINT dpi = GetDpiForWindow(h);
    auto S = [&](int v) { return MulDiv(v, dpi, 96); };
    const int m = S(14), bw = S(88), bh = S(28);
    MoveWindow(g_text, m, m, rc.right - 2 * m, S(36), TRUE);
    MoveWindow(g_bar, m, m + S(42), rc.right - 3 * m - bw, S(14), TRUE);
    MoveWindow(g_cancel, rc.right - m - bw, m + S(42) + S(7) - bh / 2, bw, bh, TRUE);
}

void ShowWindowNow() {
    const UINT dpi = GetDpiForWindow(g_wnd);
    RECT wa;
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    RECT r{0, 0, MulDiv(420, dpi, 96), MulDiv(92, dpi, 96)};
    AdjustWindowRectExForDpi(&r, WS_POPUP | WS_CAPTION | WS_SYSMENU, FALSE, WS_EX_TOOLWINDOW | WS_EX_TOPMOST, dpi);
    const int w = r.right - r.left, h = r.bottom - r.top, gap = MulDiv(16, dpi, 96);
    SetWindowPos(g_wnd, HWND_TOPMOST, wa.right - w - gap, wa.bottom - h - gap, w, h, SWP_NOACTIVATE);
    Layout(g_wnd);
    ShowWindow(g_wnd, SW_SHOWNOACTIVATE);
    SetTimer(g_wnd, 1, 100, nullptr);
}

void OpenAndSelect(const std::wstring& path) {
    PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(path.c_str());
    if (pidl) {
        SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0);
        ILFree(pidl);
    }
}

void CopyResult(const Result& r) {
    if (!OpenClipboard(g_wnd)) return;
    EmptyClipboard();
    if (!r.png.empty()) {
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, r.png.size())) {
            memcpy(GlobalLock(g), r.png.data(), r.png.size());
            GlobalUnlock(g);
            if (!SetClipboardData(RegisterClipboardFormatW(L"PNG"), g)) GlobalFree(g);
        }
    }
    const Image& im = r.flat;
    const size_t bytes = sizeof(BITMAPINFOHEADER) + im.px.size() * 4;
    if (im.w > 0) {
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
            auto* bi = static_cast<BITMAPINFOHEADER*>(GlobalLock(g));
            *bi = {sizeof(BITMAPINFOHEADER), im.w, im.h, 1, 32, BI_RGB};
            auto* dst = reinterpret_cast<uint32_t*>(bi + 1);
            for (int y = 0; y < im.h; ++y)  // bottom-up
                memcpy(dst + static_cast<size_t>(im.h - 1 - y) * im.w, im.px.data() + static_cast<size_t>(y) * im.w, im.w * 4);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_DIB, g)) GlobalFree(g);
        }
    }
    CloseClipboard();
}

void ShowResult(std::unique_ptr<Result> r) {
    if (!r->error.empty()) {
        const std::wstring msg = L"Couldn't remove the background.\n\n" + r->error;
        if (r->fromEditor && IsWindow(r->notify)) PostMessageW(r->notify, WM_APP_NOBG, 0, reinterpret_cast<LPARAM>(new std::wstring(r->error)));
        if (TestMode()) Log(L"Notice: %s", msg.c_str());
        else if (!r->fromEditor) MessageBoxW(nullptr, msg.c_str(), L"Canivete", MB_ICONWARNING | MB_SETFOREGROUND);
        return;
    }
    if (r->fromEditor) {
        if (r->written.empty()) return;
        CopyResult(*r);
        const std::wstring msg = L"Background removed — copied and saved as " + FileName(r->written[0]);
        Log(L"rmbg: editor result %s", r->written[0].c_str());
        if (IsWindow(r->notify)) PostMessageW(r->notify, WM_APP_NOBG, 0, reinterpret_cast<LPARAM>(new std::wstring(msg)));
        else Tray_Notify(L"Background removed", msg);
        return;
    }
    const int n = static_cast<int>(r->written.size());
    Log(L"rmbg: %d written, %d failed%s", n, static_cast<int>(r->failed.size()), r->canceled ? L", canceled" : L"");
    if (n == 1) {
        const std::wstring p = r->written[0];
        Tray_NotifyAction(L"Background removed", L"Saved as " + FileName(p) + L". Click to show it.", [p] { OpenAndSelect(p); });
    } else if (n > 1) {
        const std::wstring p = r->written.back();
        Tray_NotifyAction((L"Background removed from " + std::to_wstring(n) + L" pictures").c_str(),
                          L"Saved next to the originals as PNG files. Click to show them.", [p] { OpenAndSelect(p); });
    }
    if (!r->failed.empty()) {
        std::wstring msg = std::to_wstring(r->failed.size()) + (r->failed.size() == 1 ? L" picture" : L" pictures") +
                           L" didn't work:\n";
        for (size_t i = 0; i < r->failed.size() && i < 15; ++i) msg += L"\n" + r->failed[i];
        if (r->failed.size() > 15) msg += L"\n…";
        if (TestMode()) Log(L"Notice: %s", msg.c_str());
        else MessageBoxW(nullptr, msg.c_str(), L"Canivete Remove background", MB_ICONWARNING | MB_SETFOREGROUND);
    }
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_TIMER: {
        std::wstring t;
        int pos;
        {
            std::lock_guard<std::mutex> l(g_pr.m);
            t = g_pr.text;
            pos = g_pr.pos;
        }
        wchar_t cur[512] = L"";
        GetWindowTextW(g_text, cur, 512);
        if (t != cur) SetWindowTextW(g_text, t.c_str());
        const bool marquee = (GetWindowLongW(g_bar, GWL_STYLE) & PBS_MARQUEE) != 0;
        if ((pos < 0) != marquee) {
            SetWindowLongW(g_bar, GWL_STYLE, (GetWindowLongW(g_bar, GWL_STYLE) & ~PBS_MARQUEE) | (pos < 0 ? PBS_MARQUEE : 0));
            SendMessageW(g_bar, PBM_SETMARQUEE, pos < 0, 30);
        }
        if (pos >= 0) SendMessageW(g_bar, PBM_SETPOS, pos, 0);
        if (!g_busy) {
            KillTimer(h, 1);
            ShowWindow(h, SW_HIDE);
        }
        return 0;
    }
    case kMsgResult:
        ShowResult(std::unique_ptr<Result>(reinterpret_cast<Result*>(lp)));
        return 0;
    case WM_COMMAND:
        if (LOWORD(wp) == kIdCancel) {
            g_pr.cancel = true;
            std::lock_guard<std::mutex> l(g_jm);
            g_jobs.clear();
            g_pr.Set(L"Stopping…", -1);
        }
        return 0;
    case WM_CLOSE:
        SendMessageW(h, WM_COMMAND, kIdCancel, 0);
        return 0;
    case WM_DPICHANGED: {
        const RECT* r = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE);
        Layout(h);
        return 0;
    }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void EnsureWindow() {
    if (g_wnd) return;
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = g_hinst;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.lpszClassName = L"CaniveteRemoveBackground";
    wc.hIcon = LoadIconW(g_hinst, MAKEINTRESOURCEW(1));
    RegisterClassExW(&wc);
    g_wnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, wc.lpszClassName, L"Remove background",
                            WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, 100, 100, nullptr, nullptr, g_hinst, nullptr);
    NONCLIENTMETRICSW ncm{sizeof(ncm)};
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, GetDpiForWindow(g_wnd));
    g_font = CreateFontIndirectW(&ncm.lfMessageFont);
    g_text = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_NOPREFIX | SS_ENDELLIPSIS, 0, 0, 10, 10, g_wnd,
                             nullptr, g_hinst, nullptr);
    g_bar = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE, 0, 0, 10, 10, g_wnd, nullptr, g_hinst, nullptr);
    SendMessageW(g_bar, PBM_SETRANGE32, 0, 1000);
    g_cancel = CreateWindowExW(0, L"BUTTON", L"Cancel", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0, 0, 10, 10, g_wnd,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kIdCancel)), g_hinst, nullptr);
    for (HWND c : {g_text, g_cancel}) SendMessageW(c, WM_SETFONT, reinterpret_cast<WPARAM>(g_font), FALSE);
    Theme_Window(g_wnd);
}

// Asks before the first, big download. true = go ahead.
bool ReadyOrAsk() {
    const int mb = MissingMB();
    if (mb == 0 || TestMode()) return true;
    const std::wstring msg =
        L"Removing backgrounds uses a free AI model (BiRefNet) that runs on this computer. It needs a one-time "
        L"download of about " +
        std::to_wstring(mb) +
        L" MB from Hugging Face and Microsoft.\n\nAfter that it works offline and nothing leaves your computer.\n\n"
        L"Download it now?";
    return MessageBoxW(nullptr, msg.c_str(), L"Canivete Remove background", MB_YESNO | MB_ICONQUESTION | MB_SETFOREGROUND) == IDYES;
}

void Queue(std::unique_ptr<Job> j) {
    EnsureWindow();
    {
        std::lock_guard<std::mutex> l(g_jm);
        g_jobs.push_back(std::move(j));
        g_busy = true;
        if (!g_workerAlive) {
            g_workerAlive = true;
            std::thread(Worker).detach();
        }
    }
    g_jcv.notify_one();
    g_pr.Set(L"Removing the background…", -1);
    ShowWindowNow();
}

}  // namespace

void Rmbg_Files(const std::vector<std::wstring>& files) {
    if (files.empty() || !ReadyOrAsk()) return;
    auto j = std::make_unique<Job>();
    j->files = files;
    Log(L"rmbg: %d file(s) queued", static_cast<int>(files.size()));
    Queue(std::move(j));
}

void Rmbg_Image(const Image& img, const CaptureResult& meta, HWND notify) {
    if (img.w <= 0 || img.h <= 0 || !ReadyOrAsk()) return;
    auto j = std::make_unique<Job>();
    j->img = std::make_unique<Image>(img);
    j->meta = std::make_unique<CaptureResult>();
    j->meta->screenRect = meta.screenRect;
    j->meta->processName = meta.processName;
    j->meta->windowTitle = meta.windowTitle;
    j->notify = notify;
    Queue(std::move(j));
}

// --nobg-bench in out: no window; downloads if needed, cuts `in` out to `out`, timings in the log.
int Rmbg_Bench(const std::wstring& in, const std::wstring& out) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    int rc = 1;
    {
        ComPtr<IWICImagingFactory> f;
        CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f));
        Progress pr;
        Ai ai;
        std::wstring err;
        Image img;
        ULONGLONG t0 = GetTickCount64();
        if (!f || !Setup(pr, err)) {
            Log(L"rmbg bench: setup failed: %s", err.c_str());
        } else if (Log(L"rmbg bench: setup %llu ms", GetTickCount64() - t0), t0 = GetTickCount64(), !ai.Load(pr, err)) {
            Log(L"rmbg bench: load failed: %s", err.c_str());
        } else if (FAILED(LoadUpright(f.Get(), in, img))) {
            Log(L"rmbg bench: can't read %s", in.c_str());
        } else {
            for (int run = 0; run < 2 && rc; ++run) {  // twice: the first run includes warm-up
                Image work = img;
                t0 = GetTickCount64();
                if (!Cutout(ai, f.Get(), work, err)) {
                    Log(L"rmbg bench: %s", err.c_str());
                    break;
                }
                Log(L"rmbg bench: run %d total %llu ms", run + 1, GetTickCount64() - t0);
                if (run == 1 && SavePng(f.Get(), work, out, nullptr)) rc = 0;
            }
        }
    }
    CoUninitialize();
    return rc;
}
