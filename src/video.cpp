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

UINT32 Bitrate(int w, int h, int fps) {
    // About 0.08 bits per pixel: 1080p at 30 fps is ~5 Mbps, plenty for screen content.
    const double b = static_cast<double>(w) * h * fps * 0.08;
    return static_cast<UINT32>(std::clamp(b, 1.5e6, 20e6));
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
    out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000);  // 192 kbps
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
    out->SetUINT32(MF_MT_AVG_BITRATE, Bitrate(p.W, p.H, p.fps));
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
        enc->SetUINT32(CODECAPI_AVEncCommonQuality, 70);
    }
    if (FAILED(p.w->SetInputMediaType(p.stream, in.Get(), enc.Get()))) return false;
    if (p.audio && !AddAudio(p)) return false;
    if (FAILED(p.w->BeginWriting())) return false;

    std::wstring mode = L"bitrate " + std::to_wstring(Bitrate(p.W, p.H, p.fps) / 1000) + L" kbps";
    ComPtr<ICodecAPI> api;
    if (SUCCEEDED(p.w->GetServiceForStream(p.stream, GUID_NULL, IID_PPV_ARGS(&api))) && api) {
        VARIANT v;
        VariantInit(&v);
        if (SUCCEEDED(api->GetValue(&CODECAPI_AVEncCommonRateControlMode, &v)) && v.vt == VT_UI4 &&
            v.ulVal == eAVEncCommonRateControlMode_Quality)
            mode = L"quality 70";
        VariantClear(&v);
    }
    info = EncoderName(p.w.Get(), p.stream) + L", " + mode + (p.audio ? L", AAC sound" : L"");
    p.hw = hw;
    return true;
}

bool Mp4Writer::Open(const std::wstring& path, int w, int h, int fps, bool audio, std::wstring& info) {
    Impl& p = *p_;
    if (w < 2 || h < 2 || (w & 1) || (h & 1)) return false;
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
