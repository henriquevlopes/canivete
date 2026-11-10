// Sound for video recordings: what the computer plays (loopback of the default output device) and,
// if wanted, the microphone, mixed into one 48 kHz stereo track. Windows converts each device's own
// format to 48 kHz stereo float for us (AUTOCONVERTPCM).
// While recording, a silent stream plays on the output device: without it, loopback delivers nothing
// while no app makes a sound (OBS does the same). Each packet carries the time it was captured, so
// the track stays in step with the video: gaps are filled with silence, overlaps are cut.
// Pauses: sound captured while paused is dropped and every later packet moves up by the paused time,
// the same way the recorder moves the picture up, so the two stay in step after resuming.
#include "app.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <mutex>

using Microsoft::WRL::ComPtr;

namespace {

constexpr UINT32 kRate = 48000;
constexpr LONGLONG kSlack = kRate / 20;  // 50 ms: timing jitter tolerated before padding or cutting
constexpr size_t kMicMax = kRate / 2;    // at most 0.5 s of microphone waiting to be mixed

LONGLONG Now100ns() {
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return (c.QuadPart / f.QuadPart) * 10000000LL + (c.QuadPart % f.QuadPart) * 10000000LL / f.QuadPart;
}

struct Stream {
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;

    HRESULT Open(IMMDevice* dev, bool loopback) {
        HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
        if (FAILED(hr)) return hr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        wf.nChannels = 2;
        wf.nSamplesPerSec = kRate;
        wf.wBitsPerSample = 32;
        wf.nBlockAlign = 8;
        wf.nAvgBytesPerSec = kRate * 8;
        const DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY |
                            (loopback ? AUDCLNT_STREAMFLAGS_LOOPBACK : 0);
        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 10000000 /* 1 s buffer */, 0, &wf, nullptr);
        if (FAILED(hr)) return hr;
        return client->GetService(IID_PPV_ARGS(&cap));
    }

    // Calls f(frames or nullptr for silence, count, capture time) for every packet waiting.
    template <class F>
    void Drain(F&& f) {
        UINT32 next = 0;
        while (cap && SUCCEEDED(cap->GetNextPacketSize(&next)) && next) {
            BYTE* d = nullptr;
            UINT32 n = 0;
            DWORD flags = 0;
            UINT64 pos = 0, qpc = 0;
            if (FAILED(cap->GetBuffer(&d, &n, &flags, &pos, &qpc))) break;
            f((flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : reinterpret_cast<const float*>(d), n,
              static_cast<LONGLONG>(qpc));
            cap->ReleaseBuffer(n);
        }
    }
};

}  // namespace

struct AudioCapture::Impl {
    Stream desk, mic;
    ComPtr<IAudioClient> silence;  // plays nothing on the output, keeps loopback flowing
    bool hasDesk = false, hasMic = false, fake = false;
    uint64_t frames = 0, padded = 0, cut = 0;
    std::mutex m;
    std::vector<std::pair<LONGLONG, LONGLONG>> pauses;  // [from, to) in QPC 100 ns; to = LLONG_MAX while paused

    // Recording time of a QPC time: the paused spans before it taken out (inside a pause: its start).
    LONGLONG Active(LONGLONG t) {
        std::lock_guard<std::mutex> lock(m);
        LONGLONG cutOut = 0;
        for (const auto& [a, b] : pauses) {
            if (t >= b) cutOut += b - a;
            else if (t > a) cutOut += t - a;
        }
        return t - cutOut;
    }
    bool Paused(LONGLONG t) {
        std::lock_guard<std::mutex> lock(m);
        for (const auto& [a, b] : pauses)
            if (t >= a && t < b) return true;
        return false;
    }
    // The other way round (test tone): the QPC time at which recording time t was live.
    LONGLONG Wall(LONGLONG t) {
        std::lock_guard<std::mutex> lock(m);
        LONGLONG shift = 0;
        for (const auto& [a, b] : pauses)
            if (b != LLONG_MAX && t + shift >= a) shift += b - a;
        return t + shift;
    }
};

void AudioCapture::Pause(LONGLONG at100) {
    std::lock_guard<std::mutex> lock(p_->m);
    if (p_->pauses.empty() || p_->pauses.back().second != LLONG_MAX) p_->pauses.push_back({at100, LLONG_MAX});
}

void AudioCapture::Resume(LONGLONG at100) {
    std::lock_guard<std::mutex> lock(p_->m);
    if (!p_->pauses.empty() && p_->pauses.back().second == LLONG_MAX)
        p_->pauses.back().second = std::max(at100, p_->pauses.back().first);
}

AudioCapture::AudioCapture() : p_(std::make_unique<Impl>()) {}
AudioCapture::~AudioCapture() = default;
uint64_t AudioCapture::Frames() const { return p_->frames; }
uint64_t AudioCapture::Padded() const { return p_->padded; }
uint64_t AudioCapture::Cut() const { return p_->cut; }

bool AudioCapture::Open(bool desktop, bool mic, std::wstring& info) {
    Impl& p = *p_;
    wchar_t fake[8];
    if (TestMode() && GetEnvironmentVariableW(L"CANIVETE_AUDIO_FAKE", fake, 8) > 0) {
        p.fake = true;
        info = L"test tone";
        return true;
    }
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        info = L"no sound system";
        return false;
    }
    std::wstring problems;
    if (desktop) {
        ComPtr<IMMDevice> dev;
        HRESULT hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = p.desk.Open(dev.Get(), true);
        if (SUCCEEDED(hr)) {
            p.hasDesk = true;
            // The silent stream: fill its whole buffer with silence once; it keeps the device running.
            WAVEFORMATEX* mf = nullptr;
            ComPtr<IAudioRenderClient> rc;
            UINT32 n = 0;
            BYTE* d = nullptr;
            if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                        reinterpret_cast<void**>(p.silence.GetAddressOf()))) &&
                SUCCEEDED(p.silence->GetMixFormat(&mf)) &&
                SUCCEEDED(p.silence->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, 10000000, 0, mf, nullptr)) &&
                SUCCEEDED(p.silence->GetBufferSize(&n)) && SUCCEEDED(p.silence->GetService(IID_PPV_ARGS(&rc))) &&
                SUCCEEDED(rc->GetBuffer(n, &d)))
                rc->ReleaseBuffer(n, AUDCLNT_BUFFERFLAGS_SILENT);
            else
                p.silence.Reset();
            if (mf) CoTaskMemFree(mf);
        } else {
            wchar_t s[64];
            swprintf_s(s, L"computer sound unavailable (0x%08X) ", static_cast<unsigned>(hr));
            problems += s;
        }
    }
    if (mic) {
        ComPtr<IMMDevice> dev;
        HRESULT hr = en->GetDefaultAudioEndpoint(eCapture, eConsole, &dev);
        if (SUCCEEDED(hr)) hr = p.mic.Open(dev.Get(), false);
        if (SUCCEEDED(hr)) p.hasMic = true;
        else {
            wchar_t s[64];
            swprintf_s(s, L"microphone unavailable (0x%08X) ", static_cast<unsigned>(hr));
            problems += s;
        }
    }
    info = p.hasDesk && p.hasMic ? L"computer sound + microphone" : p.hasDesk ? L"computer sound" : p.hasMic ? L"microphone" : L"none";
    if (!problems.empty()) info += L"; " + problems;
    return p.hasDesk || p.hasMic;
}

void AudioCapture::Run(LONGLONG start100, const std::atomic<bool>& stop, const Writer& write) {
    Impl& p = *p_;
    std::vector<float> micFifo;  // microphone frames (stereo) waiting to be mixed into computer sound
    std::vector<int16_t> out;
    uint64_t written = 0;  // frames given to the writer; frame i belongs at start + i / 48000 s
    const bool mixMic = p.hasDesk && p.hasMic;

    // Hands n frames to the writer: src (nullptr = silence) plus the microphone when mixing.
    auto push = [&](const float* src, UINT32 n) {
        while (n) {
            const UINT32 c = std::min<UINT32>(n, kRate / 10);
            out.resize(static_cast<size_t>(c) * 2);
            const size_t micHave = mixMic ? std::min(micFifo.size(), out.size()) : 0;
            for (size_t i = 0; i < out.size(); ++i) {
                float v = src ? src[i] : 0.0f;
                if (i < micHave) v += micFifo[i];
                out[i] = static_cast<int16_t>(std::lround(std::clamp(v, -1.0f, 1.0f) * 32767.0f));
            }
            if (micHave) micFifo.erase(micFifo.begin(), micFifo.begin() + static_cast<ptrdiff_t>(micHave));
            write(static_cast<LONGLONG>(written * 10000000ULL / kRate), out.data(), c);
            written += c;
            p.frames += c;
            if (src) src += static_cast<size_t>(c) * 2;
            n -= c;
        }
    };
    auto pad = [&](LONGLONG upTo) {
        if (upTo > static_cast<LONGLONG>(written)) {
            p.padded += static_cast<uint64_t>(upTo - static_cast<LONGLONG>(written));
            push(nullptr, static_cast<UINT32>(upTo - static_cast<LONGLONG>(written)));
        }
    };
    auto frameAt = [&](LONGLONG t100) {
        return (p.Active(t100) - start100) * static_cast<LONGLONG>(kRate) / 10000000LL;
    };
    // A packet of the stream that sets the pace (computer sound, or the microphone alone).
    auto master = [&](const float* src, UINT32 n, LONGLONG qpc) {
        if (p.Paused(qpc)) return;  // captured while paused: left out
        LONGLONG at = frameAt(qpc);
        if (at + n <= 0) return;  // from before the recording started
        if (at > static_cast<LONGLONG>(written) + kSlack) pad(at);  // a gap: fill with silence
        if (at < static_cast<LONGLONG>(written)) {                 // overlaps what's written: cut that part
            const LONGLONG skip = std::min<LONGLONG>(static_cast<LONGLONG>(written) - at, n);
            if (at < static_cast<LONGLONG>(written) - kSlack || at < 0) {
                p.cut += static_cast<uint64_t>(skip);
                if (src) src += skip * 2;
                n -= static_cast<UINT32>(skip);
            }
        }
        if (n) push(src, n);
    };
    auto micIn = [&](const float* src, UINT32 n, LONGLONG qpc) {
        if (p.Paused(qpc)) return;
        if (src) micFifo.insert(micFifo.end(), src, src + static_cast<size_t>(n) * 2);
        else micFifo.insert(micFifo.end(), static_cast<size_t>(n) * 2, 0.0f);
        if (micFifo.size() > kMicMax * 2) micFifo.erase(micFifo.begin(), micFifo.end() - kMicMax * 2);
    };

    if (p.silence) p.silence->Start();
    if (p.hasDesk) p.desk.client->Start();
    if (p.hasMic) p.mic.client->Start();
    std::vector<float> tone;
    double phase = 0;
    while (!stop) {
        Sleep(10);
        if (p.fake) {
            // Test mode: a quiet 440 Hz tone, exactly in time, with a loud 1 kHz beep for the first
            // 100 ms of every whole second since the start (real time, pauses included). The test
            // picture flashes on the same seconds, so the two can be checked to line up.
            const LONGLONG upTo = frameAt(Now100ns());
            if (upTo > static_cast<LONGLONG>(written)) {
                const UINT32 n = static_cast<UINT32>(upTo - static_cast<LONGLONG>(written));
                tone.resize(static_cast<size_t>(n) * 2);
                for (UINT32 i = 0; i < n; ++i) {
                    const LONGLONG t = start100 + static_cast<LONGLONG>((written + i) * 10000000ULL / kRate);
                    const bool beep = (p.Wall(t) - start100) % 10000000LL < 1000000LL;
                    phase += 2 * 3.14159265358979 * (beep ? 1000 : 440) / kRate;
                    tone[2 * i] = tone[2 * i + 1] = static_cast<float>((beep ? 0.6 : 0.1) * std::sin(phase));
                }
                push(tone.data(), n);
            }
            continue;
        }
        if (mixMic) p.mic.Drain(micIn);
        if (p.hasDesk) p.desk.Drain(master);
        else p.mic.Drain(master);
        // Nothing came for a while (a device stalled): keep the track in step with silence.
        const LONGLONG now = frameAt(Now100ns());
        if (now - static_cast<LONGLONG>(written) > kRate / 5) pad(now - kSlack);
    }
    if (!p.fake) {
        if (mixMic) p.mic.Drain(micIn);
        if (p.hasDesk) p.desk.Drain(master);
        else p.mic.Drain(master);
    }
    pad(frameAt(Now100ns()));  // run to the end of the recording
    if (p.hasDesk) p.desk.client->Stop();
    if (p.hasMic) p.mic.client->Stop();
    if (p.silence) p.silence->Stop();
}
