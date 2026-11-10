// MP4 video writer: H.264 through Media Foundation's sink writer. Frames come in as BGRX; they are
// turned into NV12 (BT.709, video range) here, so every encoder gets the same well-defined input.
// The graphics card's encoder is used when there is one (NVIDIA, AMD, Intel all ship one as a Media
// Foundation transform); otherwise Windows' own software encoder. Quality-based rate control is asked
// for first, so still screens cost almost nothing; encoders that refuse it use a bitrate scaled to size.
// Sound (audio.cpp) goes in as a second stream: 48 kHz stereo 16-bit PCM, encoded to AAC 192 kbps.
#include "app.h"

#include <codecapi.h>
#include <mfapi.h>
#include <mferror.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <strmif.h>
#include <wrl/client.h>

#include <algorithm>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {

UINT32 Bitrate(int w, int h, int fps, int quality) {
    // At quality 70 about 0.08 bits per pixel: 1080p at 30 fps is ~5 Mbps, plenty for screen content.
    const double bpp = 0.08 * std::clamp(quality, 10, 100) / 70.0;
    const double b = static_cast<double>(w) * h * fps * bpp;
    return static_cast<UINT32>(std::clamp(b, 0.3e6, 20e6));
}

void SetColorInfo(IMFMediaType* t) {
    t->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
    t->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
    t->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
    t->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
}

// BGRX -> NV12, BT.709 video range. Chroma is the average of each 2x2 block.
void ToNv12(const uint32_t* px, int w, int h, BYTE* dst) {
    BYTE* yPlane = dst;
    BYTE* uv = dst + static_cast<size_t>(w) * h;
    for (int y = 0; y < h; y += 2) {
        const uint32_t* r0 = px + static_cast<size_t>(y) * w;
        const uint32_t* r1 = r0 + w;
        BYTE* y0 = yPlane + static_cast<size_t>(y) * w;
        BYTE* y1 = y0 + w;
        BYTE* c = uv + static_cast<size_t>(y / 2) * w;
        for (int x = 0; x < w; x += 2) {
            int sr = 0, sg = 0, sb = 0;
            const uint32_t q[4] = {r0[x], r0[x + 1], r1[x], r1[x + 1]};
            BYTE* out[4] = {y0 + x, y0 + x + 1, y1 + x, y1 + x + 1};
            for (int k = 0; k < 4; ++k) {
                const int R = (q[k] >> 16) & 255, G = (q[k] >> 8) & 255, B = q[k] & 255;
                *out[k] = static_cast<BYTE>(((11966 * R + 40254 * G + 4064 * B + 32768) >> 16) + 16);
                sr += R;
                sg += G;
                sb += B;
            }
            // sums of 4: divide by 4 inside the shift (>> 18)
            c[x] = static_cast<BYTE>((-6596 * sr - 22189 * sg + 28785 * sb + (512 << 16) + 131072) >> 18);
            c[x + 1] = static_cast<BYTE>((28785 * sr - 26145 * sg - 2640 * sb + (512 << 16) + 131072) >> 18);
        }
    }
}

std::wstring EncoderName(IMFSinkWriter* w, DWORD stream) {
    ComPtr<IMFSinkWriterEx> ex;
    if (FAILED(w->QueryInterface(IID_PPV_ARGS(&ex)))) return L"?";
    for (DWORD i = 0;; ++i) {
        GUID cat{};
        ComPtr<IMFTransform> t;
        if (FAILED(ex->GetTransformForStream(stream, i, &cat, &t))) break;
        if (cat != MFT_CATEGORY_VIDEO_ENCODER) continue;
        ComPtr<IMFAttributes> a;
        wchar_t name[256] = L"";
        UINT32 n = 0;
        if (SUCCEEDED(t->GetAttributes(&a)) && a) a->GetString(MFT_FRIENDLY_NAME_Attribute, name, 256, &n);
        return name[0] ? name : L"encoder without a name";
    }
    return L"?";
}

}  // namespace

struct Mp4Writer::Impl {
    ComPtr<IMFSinkWriter> w;
    DWORD stream = 0;
    std::wstring path;
    int W = 0, H = 0, fps = 30;
    int quality = 70, audioKbps = 192;
    LONGLONG last = -1;
    bool hw = true;
    bool mfStarted = false;
    bool audio = false;  // has a sound stream
    DWORD audioStream = 0;
    LONGLONG audioLast = -1;
    std::mutex m;  // video and sound are written from different threads
};

static bool AddAudio(Mp4Writer::Impl& p) {
    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
    out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    // AAC encoder takes 96, 128, 160 or 192 kbps
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, static_cast<UINT32>(p.audioKbps) * 125);
    out->SetUINT32(MF_MT_AAC_AUDIO_PROFILE_LEVEL_INDICATION, 0x29);
    if (FAILED(p.w->AddStream(out.Get(), &p.audioStream))) return false;
    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
    in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
    in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
    in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
    in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
    in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
    in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
    return SUCCEEDED(p.w->SetInputMediaType(p.audioStream, in.Get(), nullptr));
}

Mp4Writer::Mp4Writer() : p_(std::make_unique<Impl>()) {}

Mp4Writer::~Mp4Writer() {
    p_->w.Reset();
    if (p_->mfStarted) MFShutdown();
}

static bool TryOpen(Mp4Writer::Impl& p, bool hw, bool quality, std::wstring& info) {
    p.w.Reset();
    DeleteFileW(p.path.c_str());
    ComPtr<IMFAttributes> a;
    MFCreateAttributes(&a, 4);
    a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, hw ? TRUE : FALSE);
    a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, MFTranscodeContainerType_MPEG4);
    // Never make one stream wait for the other (they come from two threads); the encoders keep up.
    a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
    // (MF_MPEG4SINK_MOOV_BEFORE_MDAT is left out on purpose: Finalize fails with access denied, and with
    // a read-write byte stream the file comes out unplayable.)
    if (FAILED(MFCreateSinkWriterFromURL(p.path.c_str(), nullptr, a.Get(), &p.w))) return false;

    ComPtr<IMFMediaType> out, in;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
    out->SetUINT32(MF_MT_AVG_BITRATE, Bitrate(p.W, p.H, p.fps, p.quality));
    out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, p.W, p.H);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, p.fps, 1);
    MFSetAttributeRatio(out.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    SetColorInfo(out.Get());
    if (FAILED(p.w->AddStream(out.Get(), &p.stream))) return false;

    MFCreateMediaType(&in);
    in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
    in->SetUINT32(MF_MT_DEFAULT_STRIDE, static_cast<UINT32>(p.W));
    MFSetAttributeSize(in.Get(), MF_MT_FRAME_SIZE, p.W, p.H);
    MFSetAttributeRatio(in.Get(), MF_MT_FRAME_RATE, p.fps, 1);
    MFSetAttributeRatio(in.Get(), MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
    SetColorInfo(in.Get());

    ComPtr<IMFAttributes> enc;
    MFCreateAttributes(&enc, 4);
    enc->SetUINT32(CODECAPI_AVEncMPVGOPSize, static_cast<UINT32>(p.fps * 2));  // a keyframe every 2 s
    if (quality) {
        enc->SetUINT32(CODECAPI_AVEncCommonRateControlMode, eAVEncCommonRateControlMode_Quality);
        enc->SetUINT32(CODECAPI_AVEncCommonQuality, static_cast<UINT32>(p.quality));
    }
    if (FAILED(p.w->SetInputMediaType(p.stream, in.Get(), enc.Get()))) return false;
    if (p.audio && !AddAudio(p)) return false;
    if (FAILED(p.w->BeginWriting())) return false;

    std::wstring mode = L"bitrate " + std::to_wstring(Bitrate(p.W, p.H, p.fps, p.quality) / 1000) + L" kbps";
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(p.w->GetServiceForStream(p.stream, GUID_NULL, IID_PPV_ARGS(&api))) && api) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(api->GetValue(&CODECAPI_AVEncCommonRateControlMode, &v)) && v.vt == VT_UI4 &&
            v.ulVal == eAVEncCommonRateControlMode_Quality)
            mode = L"quality " + std::to_wstring(p.quality);
        VariantClear(&v);
    }
    info = EncoderName(p.w.Get(), p.stream) + L", " + mode + (p.audio ? L", AAC sound" : L"");
    p.hw = hw;
    return true;
}

bool Mp4Writer::Open(const std::wstring& path, int w, int h, int fps, bool audio, std::wstring& info, int quality,
                     int audioKbps) {
    Impl& p = *p_;
    if (w < 2 || h < 2 || (w & 1) || (h & 1)) return false;
    p.quality = std::clamp(quality, 1, 100);
    p.audioKbps = audioKbps <= 96 ? 96 : audioKbps <= 128 ? 128 : audioKbps <= 160 ? 160 : 192;
    if (!p.mfStarted) {
        if (FAILED(MFStartup(MF_VERSION, MFSTARTUP_LITE))) return false;
        p.mfStarted = true;
    }
    p.path = path;
    p.W = w;
    p.H = h;
    p.fps = fps;
    // Graphics-card encoder first, then Windows' software one; quality mode first, then a bitrate.
    // If nothing works with sound, a video without it is still better than none.
    for (bool a : {audio, false}) {
        p.audio = a;
        for (bool hw : {true, false})
            for (bool q : {true, false})
                if (TryOpen(p, hw, q, info)) return true;
        if (!audio) break;
    }
    p.w.Reset();
    DeleteFileW(path.c_str());
    return false;
}

bool Mp4Writer::Frame(LONGLONG time100ns, const uint32_t* px) {
    Impl& p = *p_;
    if (!p.w || time100ns <= p.last) return false;
    const DWORD bytes = static_cast<DWORD>(p.W) * p.H * 3 / 2;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) return false;
    BYTE* d = nullptr;
    if (FAILED(buf->Lock(&d, nullptr, nullptr))) return false;
    ToNv12(px, p.W, p.H, d);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(buf.Get());
    s->SetSampleTime(time100ns);
    s->SetSampleDuration(10000000LL / p.fps);
    std::lock_guard<std::mutex> lock(p.m);
    if (!p.w) return false;
    HRESULT hr = p.w->WriteSample(p.stream, s.Get());
    if (FAILED(hr) && p.hw && p.last < 0) {
        // Some graphics-card encoders only fail on the first frame: start over with the software one.
        std::wstring info;
        if (TryOpen(p, false, true, info) || TryOpen(p, false, false, info)) {
            Log(L"Video: graphics-card encoder failed (0x%08X), using %s", static_cast<unsigned>(hr), info.c_str());
            hr = p.w->WriteSample(p.stream, s.Get());
        }
    }
    if (FAILED(hr)) return false;
    p.last = time100ns;
    return true;
}

bool Mp4Writer::HasAudio() const { return p_->w && p_->audio; }

bool Mp4Writer::Audio(LONGLONG time100ns, const int16_t* frames, UINT32 count) {
    Impl& p = *p_;
    if (!p.audio || !count || time100ns < p.audioLast) return false;
    const DWORD bytes = count * 4;
    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(MFCreateMemoryBuffer(bytes, &buf))) return false;
    BYTE* d = nullptr;
    if (FAILED(buf->Lock(&d, nullptr, nullptr))) return false;
    memcpy(d, frames, bytes);
    buf->Unlock();
    buf->SetCurrentLength(bytes);
    ComPtr<IMFSample> s;
    MFCreateSample(&s);
    s->AddBuffer(buf.Get());
    s->SetSampleTime(time100ns);
    s->SetSampleDuration(static_cast<LONGLONG>(count) * 10000000LL / 48000);
    std::lock_guard<std::mutex> lock(p.m);
    if (!p.w) return false;
    if (FAILED(p.w->WriteSample(p.audioStream, s.Get()))) return false;
    p.audioLast = time100ns;
    return true;
}

bool Mp4Writer::Close() {
    Impl& p = *p_;
    std::lock_guard<std::mutex> lock(p.m);
    if (!p.w) return false;
    if (p.last < 0) {
        p.w.Reset();
        return false;
    }
    const HRESULT hr = p.w->Finalize();
    if (FAILED(hr)) Log(L"Video: finishing the file failed (0x%08X)", static_cast<unsigned>(hr));
    p.w.Reset();
    return SUCCEEDED(hr);
}

// ---- smaller copies -------------------------------------------------------------------------

// Measured on 10 s of 1080p (scrolling text / full-screen motion), as recorded: 1.8 / 24.5 MB.
// Balanced: 1.0 / 8.6 MB, text looks the same. Smallest: 0.8 / 0.75 MB. Halving the size only pays
// off with lots of motion: scrolling text came out bigger at half size, so calm recordings keep theirs.
const VideoPreset kVideoPresets[kVideoPresetCount] = {
    {L"smallest", L"Smallest file", L"lower quality", 50, 20, 96},
    {L"balanced", L"Balanced", L"a little softer", 100, 35, 128},
    {L"best", L"Best quality", L"as recorded", 100, 0, 192},
};

bool Mp4_IsBusy(uint64_t bytes, uint32_t ms, int w, int h) {
    // bits per pixel of the recording (quality 70): ~0.02 for scrolling text, ~0.3 for full motion
    if (!ms || w <= 0 || h <= 0) return false;
    return bytes * 8.0 / (ms / 1000.0 * 30.0 * w * h) > 0.08;
}

namespace {

// Shrinks a BGRX picture by averaging the source pixels under each target pixel.
void Downscale(const BYTE* src, LONG pitch, int sw, int sh, uint32_t* dst, int dw, int dh) {
    std::vector<int> x0(static_cast<size_t>(dw) + 1);
    for (int x = 0; x <= dw; ++x) x0[x] = static_cast<int>(static_cast<int64_t>(x) * sw / dw);
    for (int y = 0; y < dh; ++y) {
        const int ya = static_cast<int>(static_cast<int64_t>(y) * sh / dh);
        const int yb = std::max(ya + 1, static_cast<int>(static_cast<int64_t>(y + 1) * sh / dh));
        for (int x = 0; x < dw; ++x) {
            const int xa = x0[x], xb = std::max(xa + 1, x0[x + 1]);
            uint32_t r = 0, g = 0, b = 0;
            for (int yy = ya; yy < yb; ++yy) {
                const uint32_t* row = reinterpret_cast<const uint32_t*>(src + static_cast<ptrdiff_t>(yy) * pitch);
                for (int xx = xa; xx < xb; ++xx) {
                    r += (row[xx] >> 16) & 255;
                    g += (row[xx] >> 8) & 255;
                    b += row[xx] & 255;
                }
            }
            const uint32_t n = static_cast<uint32_t>((yb - ya) * (xb - xa));
            dst[static_cast<size_t>(y) * dw + x] = (((r + n / 2) / n) << 16) | (((g + n / 2) / n) << 8) | ((b + n / 2) / n);
        }
    }
}

struct MfScope {
    bool com = false, mf = false;
    MfScope() {
        com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    }
    ~MfScope() {
        if (mf) MFShutdown();
        if (com) CoUninitialize();
    }
};

}  // namespace

void Mp4_ShrinkSize(int w, int h, int percent, bool busy, int& outW, int& outH) {
    double f = busy ? std::clamp(percent, 1, 100) / 100.0 : 1.0;
    f = std::max(f, std::min(1.0, 640.0 / std::max(1, std::max(w, h))));  // never below 640 on the long side
    outW = f >= 1.0 ? w : std::max(16, static_cast<int>(std::lround(w * f)) & ~1);
    outH = f >= 1.0 ? h : std::max(16, static_cast<int>(std::lround(h * f)) & ~1);
}

// from / to (100 ns) trim the video: frames are kept by their time (to the frame), sound to the
// sample, and both move up by `from`, so they stay in step.
bool Mp4_Shrink(const std::wstring& src, const VideoPreset& vp, const std::wstring& outPath,
                const std::atomic<bool>& cancel, std::wstring& err, LONGLONG from, LONGLONG to) {
    from = std::max<LONGLONG>(0, from);
    if (to <= from) to = from + 1;
    MfScope mf;
    if (!mf.mf) {
        err = L"Media Foundation didn't start";
        return false;
    }
    bool ok = false;
    {
        ComPtr<IMFAttributes> a;
        MFCreateAttributes(&a, 1);
        a->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);  // decoder output turned into RGB32
        ComPtr<IMFSourceReader> r;
        if (FAILED(MFCreateSourceReaderFromURL(src.c_str(), a.Get(), &r))) {
            err = L"can't read the recording";
            return false;
        }
        // Find the streams.
        DWORD vIdx = MAXDWORD, aIdx = MAXDWORD;
        int W = 0, H = 0, fps = 30;
        for (DWORD i = 0;; ++i) {
            ComPtr<IMFMediaType> t;
            if (FAILED(r->GetNativeMediaType(i, 0, &t))) break;
            GUID major{};
            t->GetGUID(MF_MT_MAJOR_TYPE, &major);
            if (major == MFMediaType_Video && vIdx == MAXDWORD) {
                vIdx = i;
                UINT32 w = 0, h = 0, n = 0, d = 0;
                MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &h);
                W = static_cast<int>(w);
                H = static_cast<int>(h);
                if (SUCCEEDED(MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &n, &d)) && d && n)
                    fps = std::clamp(static_cast<int>((n + d / 2) / d), 1, 120);
            } else if (major == MFMediaType_Audio && aIdx == MAXDWORD)
                aIdx = i;
        }
        if (vIdx == MAXDWORD || W < 2 || H < 2) {
            err = L"no picture in the recording";
            return false;
        }
        r->SetStreamSelection(static_cast<DWORD>(MF_SOURCE_READER_ALL_STREAMS), FALSE);
        r->SetStreamSelection(vIdx, TRUE);
        ComPtr<IMFMediaType> vt;
        MFCreateMediaType(&vt);
        vt->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        vt->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (FAILED(r->SetCurrentMediaType(vIdx, nullptr, vt.Get()))) {
            err = L"can't decode the picture";
            return false;
        }
        UINT32 fw = 0, fh = 0;  // decoded frame size (may be padded, e.g. 1088 for 1080)
        {
            ComPtr<IMFMediaType> cur;
            r->GetCurrentMediaType(vIdx, &cur);
            MFGetAttributeSize(cur.Get(), MF_MT_FRAME_SIZE, &fw, &fh);
        }
        if (static_cast<int>(fw) < W || static_cast<int>(fh) < H) {
            W = std::min<int>(W, static_cast<int>(fw)) & ~1;
            H = std::min<int>(H, static_cast<int>(fh)) & ~1;
        }
        bool sound = false;
        if (aIdx != MAXDWORD) {
            ComPtr<IMFMediaType> at;
            MFCreateMediaType(&at);
            at->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
            at->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
            at->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
            at->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, 48000);
            at->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
            at->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
            at->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 48000 * 4);
            r->SetStreamSelection(aIdx, TRUE);
            sound = SUCCEEDED(r->SetCurrentMediaType(aIdx, nullptr, at.Get()));
            if (!sound) r->SetStreamSelection(aIdx, FALSE);
        }

        bool busy = false;
        {
            PROPVARIANT v;
            PropVariantInit(&v);
            WIN32_FILE_ATTRIBUTE_DATA fa;
            if (SUCCEEDED(r->GetPresentationAttribute(static_cast<DWORD>(MF_SOURCE_READER_MEDIASOURCE), MF_PD_DURATION, &v)) &&
                v.vt == VT_UI8 && GetFileAttributesExW(src.c_str(), GetFileExInfoStandard, &fa))
                busy = Mp4_IsBusy((static_cast<uint64_t>(fa.nFileSizeHigh) << 32) | fa.nFileSizeLow,
                                  static_cast<uint32_t>(v.uhVal.QuadPart / 10000), W, H);
            PropVariantClear(&v);
        }
        int ow = W, oh = H;
        Mp4_ShrinkSize(W, H, vp.percent, busy, ow, oh);

        Mp4Writer w;
        std::wstring info;
        if (!w.Open(outPath, ow, oh, fps, sound, info, vp.quality ? vp.quality : 70, vp.audioKbps)) {
            err = L"no encoder";
            return false;
        }
        sound = sound && w.HasAudio();
        std::vector<uint32_t> frame(static_cast<size_t>(ow) * oh);
        bool vEnd = false, aEnd = !sound, failed = false;
        uint64_t frames = 0;
        const LONGLONG half = 10000000LL / fps / 2;  // a frame belongs to the time nearest to it
        const LONGLONG fromS = from * 48000 / 10000000, toS = to == LLONG_MAX ? LLONG_MAX : to * 48000 / 10000000;
        while (!(vEnd && aEnd)) {
            if (cancel) {
                err = L"cancelled";
                failed = true;
                break;
            }
            DWORD idx = 0, flags = 0;
            LONGLONG ts = 0;
            ComPtr<IMFSample> s;
            if (FAILED(r->ReadSample(static_cast<DWORD>(MF_SOURCE_READER_ANY_STREAM), 0, &idx, &flags, &ts, &s)) ||
                (flags & MF_SOURCE_READERF_ERROR)) {
                err = L"reading the recording failed";
                failed = true;
                break;
            }
            if (flags & MF_SOURCE_READERF_ENDOFSTREAM) {
                if (idx == vIdx) vEnd = true;
                else aEnd = true;
            }
            if (!s) continue;
            ComPtr<IMFMediaBuffer> buf;
            if (FAILED(s->GetBufferByIndex(0, &buf))) continue;
            if (idx == vIdx) {
                if (ts < from - half) continue;
                if (to != LLONG_MAX && ts >= to - half) {
                    vEnd = true;  // past the end: the rest is not needed
                    r->SetStreamSelection(vIdx, FALSE);
                    continue;
                }
                ComPtr<IMF2DBuffer> b2;
                BYTE* scan0 = nullptr;
                LONG pitch = 0;
                const bool locked2d = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch));
                if (!locked2d) {
                    if (FAILED(buf->Lock(&scan0, nullptr, nullptr))) continue;
                    pitch = static_cast<LONG>(fw) * 4;
                }
                if (ow == W && oh == H)
                    for (int y = 0; y < H; ++y)
                        memcpy(&frame[static_cast<size_t>(y) * ow], scan0 + static_cast<ptrdiff_t>(y) * pitch,
                               static_cast<size_t>(W) * 4);
                else
                    Downscale(scan0, pitch, W, H, frame.data(), ow, oh);
                if (locked2d) b2->Unlock2D();
                else buf->Unlock();
                if (w.Frame(std::max<LONGLONG>(0, ts - from), frame.data())) ++frames;
            } else if (idx == aIdx && sound) {
                BYTE* d = nullptr;
                DWORD len = 0;
                if (FAILED(buf->Lock(&d, nullptr, &len))) continue;
                // This packet's samples, cut to [from, to) to the sample.
                const LONGLONG s0 = (ts * 48000 + 5000000) / 10000000, s1 = s0 + len / 4;
                const LONGLONG k0 = std::max(s0, fromS), k1 = std::min(s1, toS);
                if (k1 > k0)
                    w.Audio((k0 - fromS) * 10000000 / 48000, reinterpret_cast<const int16_t*>(d) + (k0 - s0) * 2,
                            static_cast<UINT32>(k1 - k0));
                buf->Unlock();
                if (s0 >= toS) {
                    aEnd = true;
                    r->SetStreamSelection(aIdx, FALSE);
                }
            }
        }
        if (!failed && frames == 0) {
            err = L"no frames";
            failed = true;
        }
        if (!failed) {
            ok = w.Close();
            if (!ok) err = L"finishing the file failed";
        }
        // (when failed, the writer is dropped unfinished here and the file deleted below)
    }
    if (!ok) DeleteFileW(outPath.c_str());
    return ok;
}
