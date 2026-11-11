// Extract text: reads the text in a picked screen area with Windows' own OCR (Windows.Media.Ocr), puts it on
// the clipboard and tells you. Nothing is downloaded; it reads in the languages of the user's Windows profile.
#include "app.h"

#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Globalization.h>
#include <winrt/Windows.Graphics.Imaging.h>
#include <winrt/Windows.Media.Ocr.h>
#include <winrt/Windows.Storage.Streams.h>

#include <algorithm>
#include <cmath>
#include <thread>

namespace {

namespace wr = winrt;
using namespace wr::Windows::Graphics::Imaging;
using namespace wr::Windows::Media::Ocr;
using namespace wr::Windows::Storage::Streams;

void Say(const std::wstring& text) {
    PostMessageW(g_mainWnd, WM_APP_TEXT, 0, reinterpret_cast<LPARAM>(new std::wstring(text)));
}

void CopyText(const std::wstring& t) {
    bool open = false;
    for (int i = 0; i < 10 && !(open = OpenClipboard(g_mainWnd) != FALSE); ++i) Sleep(15);
    if (!open) return;
    EmptyClipboard();
    const size_t bytes = (t.size() + 1) * sizeof(wchar_t);
    if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, bytes)) {
        if (void* p = GlobalLock(g)) {
            memcpy(p, t.c_str(), bytes);
            GlobalUnlock(g);
            if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
        } else {
            GlobalFree(g);
        }
    }
    CloseClipboard();
}

// The picture, upscaled when it is small (Windows' OCR reads small type badly), as a Bgra8 bitmap.
SoftwareBitmap MakeBitmap(const Image& img, int scale) {
    const int w = img.w * scale, h = img.h * scale;
    std::vector<uint32_t> px(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; ++y) {
        // Bilinear along both axes, cheap enough for a screen region.
        const float fy = (y + 0.5f) / scale - 0.5f;
        const int y0 = std::clamp(static_cast<int>(std::floor(fy)), 0, img.h - 1), y1 = std::min(y0 + 1, img.h - 1);
        const float wy = std::clamp(fy - y0, 0.f, 1.f);
        for (int x = 0; x < w; ++x) {
            const float fx = (x + 0.5f) / scale - 0.5f;
            const int x0 = std::clamp(static_cast<int>(std::floor(fx)), 0, img.w - 1), x1 = std::min(x0 + 1, img.w - 1);
            const float wx = std::clamp(fx - x0, 0.f, 1.f);
            const uint32_t p00 = img.px[static_cast<size_t>(y0) * img.w + x0], p10 = img.px[static_cast<size_t>(y0) * img.w + x1],
                           p01 = img.px[static_cast<size_t>(y1) * img.w + x0], p11 = img.px[static_cast<size_t>(y1) * img.w + x1];
            uint32_t o = 0xFF000000u;
            for (int sh = 0; sh <= 16; sh += 8) {
                const float a = ((p00 >> sh) & 255) * (1 - wx) + ((p10 >> sh) & 255) * wx;
                const float b = ((p01 >> sh) & 255) * (1 - wx) + ((p11 >> sh) & 255) * wx;
                o |= static_cast<uint32_t>(a * (1 - wy) + b * wy + 0.5f) << sh;
            }
            px[static_cast<size_t>(y) * w + x] = o;
        }
    }
    DataWriter dw;
    dw.WriteBytes(wr::array_view<const uint8_t>(reinterpret_cast<const uint8_t*>(px.data()),
                                                reinterpret_cast<const uint8_t*>(px.data()) + px.size() * 4));
    return SoftwareBitmap::CreateCopyFromBuffer(dw.DetachBuffer(), BitmapPixelFormat::Bgra8, w, h, BitmapAlphaMode::Ignore);
}

std::wstring Read(const Image& img, bool* noEngine) {
    OcrEngine engine = OcrEngine::TryCreateFromUserProfileLanguages();
    if (!engine) engine = OcrEngine::TryCreateFromLanguage(wr::Windows::Globalization::Language(L"en-US"));
    if (!engine) {
        *noEngine = true;
        return L"";
    }
    const int maxDim = static_cast<int>(OcrEngine::MaxImageDimension());
    int scale = img.h < 60 ? 4 : img.h < 120 ? 3 : img.h < 300 ? 2 : 1;
    while (scale > 1 && (img.w * scale > maxDim || img.h * scale > maxDim)) --scale;
    if (img.w > maxDim || img.h > maxDim) return L"";
    const OcrResult res = engine.RecognizeAsync(MakeBitmap(img, scale)).get();
    std::wstring out;
    for (const OcrLine& line : res.Lines()) {
        if (!out.empty()) out += L"\r\n";
        out += line.Text().c_str();
    }
    return out;
}

}  // namespace

void Ocr_Image(std::unique_ptr<Image> img) {
    std::thread([p = std::shared_ptr<Image>(std::move(img))] {
        try {
            wr::init_apartment(wr::apartment_type::multi_threaded);
            bool noEngine = false;
            const std::wstring text = Read(*p, &noEngine);
            if (noEngine) {
                Say(L"Windows has no text recognition language installed (Settings > Time & language > Language & region).");
            } else if (text.empty()) {
                Say(L"No text found in that area.");
            } else {
                CopyText(text);
                Log(L"Text extracted: %d characters", static_cast<int>(text.size()));
                Say(L"Copied: " + (text.size() > 120 ? text.substr(0, 120) + L"…" : text));
            }
        } catch (const wr::hresult_error& e) {
            Log(L"Text extractor failed: 0x%08X", static_cast<unsigned>(e.code().value));
            Say(L"Couldn't read the text (Windows OCR failed).");
        } catch (...) {
            Say(L"Couldn't read the text.");
        }
    }).detach();
}
