// GIF recording, the part without windows: the recording file (only the screen tiles that changed
// since the last frame) and the GIF encoder that turns it into a small file.
//
// Recording file: "CGF1", int32 width, int32 height, then records
//   uint32 time in ms, uint32 tile count, per tile: uint16 x, y, w, h + w*h BGRX pixels.
// The first record holds the whole picture; the last one has tile count kEndMark (the stop time).
//
// Encoder: one palette for the whole GIF (Wu's color cube cuts on a 5-bit histogram, refined by
// k-means on a 6-bit one, or the exact colors when there are few), then per frame only the pixels
// that really changed are written; everything else is transparent and costs almost nothing.
#include "app.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>

const GifPreset kGifPresets[kGifPresetCount] = {
    {L"smallest", L"Smallest file", L"10 frames a second, 64 colors", 10, 64, 10, false},
    {L"balanced", L"Balanced", L"15 frames a second, 256 colors", 15, 256, 3, false},
    {L"smooth", L"Smoothest", L"25 frames a second, 256 colors, smooth gradients", 25, 256, 0, true},
};

namespace {

constexpr uint32_t kEndMark = 0xFFFFFFFF;
constexpr int kTile = 32;

// ---- recording file reader -------------------------------------------------------------------

struct Reader {
    FILE* f = nullptr;
    int w = 0, h = 0;
    ~Reader() {
        if (f) fclose(f);
    }
    bool Open(const std::wstring& path) {
        if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return false;
        setvbuf(f, nullptr, _IOFBF, 1 << 20);
        char magic[4];
        int32_t wh[2];
        if (fread(magic, 1, 4, f) != 4 || memcmp(magic, "CGF1", 4) || fread(wh, 4, 2, f) != 2) return false;
        w = wh[0];
        h = wh[1];
        return w > 0 && h > 0 && w <= 16384 && h <= 16384;
    }
    // Reads the next record into `canvas` (w*h). Calls tile(x, y, w, h) for each tile.
    // Returns false at the end; `end` is true when the end mark was read (time = stop time).
    template <class F>
    bool Next(uint32_t& time, bool& end, std::vector<uint32_t>& canvas, F&& tile) {
        uint32_t head[2];
        if (fread(head, 4, 2, f) != 2) return false;
        time = head[0];
        end = head[1] == kEndMark;
        if (end) return true;
        for (uint32_t i = 0; i < head[1]; ++i) {
            uint16_t r[4];
            if (fread(r, 2, 4, f) != 4) return false;
            const int x = r[0], y = r[1], tw = r[2], th = r[3];
            if (x + tw > w || y + th > h) return false;
            for (int row = 0; row < th; ++row)
                if (fread(&canvas[static_cast<size_t>(y + row) * w + x], 4, tw, f) != static_cast<size_t>(tw))
                    return false;
            tile(x, y, tw, th);
        }
        return true;
    }
};

// ---- palette ---------------------------------------------------------------------------------

struct Rgb {
    int r, g, b;
};

inline int Dist(const Rgb& a, int r, int g, int b) {
    const int dr = a.r - r, dg = a.g - g, db = a.b - b;
    return 3 * dr * dr + 4 * dg * dg + 2 * db * db;
}

// Xiaolin Wu's color quantizer on a 33^3 cumulative moment cube (index 0 is the empty border).
class Wu {
public:
    static constexpr int N = 33;
    std::vector<double> wt, mr, mg, mb, m2;
    Wu() : wt(N * N * N), mr(N * N * N), mg(N * N * N), mb(N * N * N), m2(N * N * N) {}
    static int Idx(int r, int g, int b) { return (r * N + g) * N + b; }

    void Add(int r5, int g5, int b5, double count, double sr, double sg, double sb, double s2) {
        const int i = Idx(r5 + 1, g5 + 1, b5 + 1);
        wt[i] += count;
        mr[i] += sr;
        mg[i] += sg;
        mb[i] += sb;
        m2[i] += s2;
    }

    std::vector<Rgb> Run(int k) {
        Moments();
        std::vector<Box> cube(k);
        std::vector<double> vv(k);
        cube[0] = {0, N - 1, 0, N - 1, 0, N - 1};
        int next = 0, made = 1;
        for (int i = 1; i < k; ++i) {
            if (Cut(cube[next], cube[i])) {
                vv[next] = Volume(cube[next], wt) > 1 ? Var(cube[next]) : 0;
                vv[i] = Volume(cube[i], wt) > 1 ? Var(cube[i]) : 0;
                made = i + 1;
            } else {
                vv[next] = 0;
                --i;
            }
            next = 0;
            double best = vv[0];
            for (int j = 1; j <= i; ++j)
                if (vv[j] > best) {
                    best = vv[j];
                    next = j;
                }
            if (best <= 0) break;
        }
        std::vector<Rgb> out;
        for (int i = 0; i < made; ++i) {
            const double w = Volume(cube[i], wt);
            if (w <= 0) continue;
            out.push_back({static_cast<int>(Volume(cube[i], mr) / w + 0.5), static_cast<int>(Volume(cube[i], mg) / w + 0.5),
                           static_cast<int>(Volume(cube[i], mb) / w + 0.5)});
        }
        return out;
    }

private:
    struct Box {
        int r0, r1, g0, g1, b0, b1;  // r0 exclusive, r1 inclusive
    };
    enum Dir { R, G, B };

    void Moments() {
        for (int r = 1; r < N; ++r) {
            double area[N]{}, ar[N]{}, ag[N]{}, ab[N]{}, a2[N]{};
            for (int g = 1; g < N; ++g) {
                double line = 0, lr = 0, lg = 0, lb = 0, l2 = 0;
                for (int b = 1; b < N; ++b) {
                    const int i1 = Idx(r, g, b), i2 = Idx(r - 1, g, b);
                    line += wt[i1];
                    lr += mr[i1];
                    lg += mg[i1];
                    lb += mb[i1];
                    l2 += m2[i1];
                    area[b] += line;
                    ar[b] += lr;
                    ag[b] += lg;
                    ab[b] += lb;
                    a2[b] += l2;
                    wt[i1] = wt[i2] + area[b];
                    mr[i1] = mr[i2] + ar[b];
                    mg[i1] = mg[i2] + ag[b];
                    mb[i1] = mb[i2] + ab[b];
                    m2[i1] = m2[i2] + a2[b];
                }
            }
        }
    }
    static double Volume(const Box& c, const std::vector<double>& m) {
        return m[Idx(c.r1, c.g1, c.b1)] - m[Idx(c.r1, c.g1, c.b0)] - m[Idx(c.r1, c.g0, c.b1)] + m[Idx(c.r1, c.g0, c.b0)] -
               m[Idx(c.r0, c.g1, c.b1)] + m[Idx(c.r0, c.g1, c.b0)] + m[Idx(c.r0, c.g0, c.b1)] - m[Idx(c.r0, c.g0, c.b0)];
    }
    static double Bottom(const Box& c, Dir d, const std::vector<double>& m) {
        switch (d) {
        case R:
            return -m[Idx(c.r0, c.g1, c.b1)] + m[Idx(c.r0, c.g1, c.b0)] + m[Idx(c.r0, c.g0, c.b1)] - m[Idx(c.r0, c.g0, c.b0)];
        case G:
            return -m[Idx(c.r1, c.g0, c.b1)] + m[Idx(c.r1, c.g0, c.b0)] + m[Idx(c.r0, c.g0, c.b1)] - m[Idx(c.r0, c.g0, c.b0)];
        default:
            return -m[Idx(c.r1, c.g1, c.b0)] + m[Idx(c.r1, c.g0, c.b0)] + m[Idx(c.r0, c.g1, c.b0)] - m[Idx(c.r0, c.g0, c.b0)];
        }
    }
    static double Top(const Box& c, Dir d, int pos, const std::vector<double>& m) {
        switch (d) {
        case R:
            return m[Idx(pos, c.g1, c.b1)] - m[Idx(pos, c.g1, c.b0)] - m[Idx(pos, c.g0, c.b1)] + m[Idx(pos, c.g0, c.b0)];
        case G:
            return m[Idx(c.r1, pos, c.b1)] - m[Idx(c.r1, pos, c.b0)] - m[Idx(c.r0, pos, c.b1)] + m[Idx(c.r0, pos, c.b0)];
        default:
            return m[Idx(c.r1, c.g1, pos)] - m[Idx(c.r1, c.g0, pos)] - m[Idx(c.r0, c.g1, pos)] + m[Idx(c.r0, c.g0, pos)];
        }
    }
    double Var(const Box& c) const {
        const double dr = Volume(c, mr), dg = Volume(c, mg), db = Volume(c, mb);
        return Volume(c, m2) - (dr * dr + dg * dg + db * db) / Volume(c, wt);
    }
    double Maximize(const Box& c, Dir d, int first, int last, int& cut, double wr, double wg, double wb, double ww) const {
        const double br = Bottom(c, d, mr), bg = Bottom(c, d, mg), bb = Bottom(c, d, mb), bw = Bottom(c, d, wt);
        double best = 0;
        cut = -1;
        for (int i = first; i < last; ++i) {
            double hr = br + Top(c, d, i, mr), hg = bg + Top(c, d, i, mg), hb = bb + Top(c, d, i, mb), hw = bw + Top(c, d, i, wt);
            if (hw <= 0) continue;
            double t = (hr * hr + hg * hg + hb * hb) / hw;
            hr = wr - hr;
            hg = wg - hg;
            hb = wb - hb;
            hw = ww - hw;
            if (hw <= 0) continue;
            t += (hr * hr + hg * hg + hb * hb) / hw;
            if (t > best) {
                best = t;
                cut = i;
            }
        }
        return best;
    }
    bool Cut(Box& a, Box& b) const {
        const double wr = Volume(a, mr), wg = Volume(a, mg), wb = Volume(a, mb), ww = Volume(a, wt);
        int cr, cg, cb;
        const double maxr = Maximize(a, R, a.r0 + 1, a.r1, cr, wr, wg, wb, ww);
        const double maxg = Maximize(a, G, a.g0 + 1, a.g1, cg, wr, wg, wb, ww);
        const double maxb = Maximize(a, B, a.b0 + 1, a.b1, cb, wr, wg, wb, ww);
        Dir d;
        if (maxr >= maxg && maxr >= maxb) {
            d = R;
            if (cr < 0) return false;
        } else if (maxg >= maxr && maxg >= maxb) {
            d = G;
        } else {
            d = B;
        }
        b.r1 = a.r1;
        b.g1 = a.g1;
        b.b1 = a.b1;
        switch (d) {
        case R:
            b.r0 = a.r1 = cr;
            b.g0 = a.g0;
            b.b0 = a.b0;
            break;
        case G:
            if (cg < 0) return false;
            b.g0 = a.g1 = cg;
            b.r0 = a.r0;
            b.b0 = a.b0;
            break;
        case B:
            if (cb < 0) return false;
            b.b0 = a.b1 = cb;
            b.r0 = a.r0;
            b.g0 = a.g0;
            break;
        }
        return true;
    }
};

// Counts of every color seen, at 6 bits per channel, plus the exact colors while there are few.
struct Histogram {
    static constexpr int kBins = 64 * 64 * 64;
    std::vector<uint32_t> count = std::vector<uint32_t>(kBins);
    std::vector<uint64_t> sr = std::vector<uint64_t>(kBins), sg = std::vector<uint64_t>(kBins),
                          sb = std::vector<uint64_t>(kBins);
    std::vector<uint64_t> seen = std::vector<uint64_t>((1 << 24) / 64);  // bitset of exact colors
    std::vector<uint32_t> exact;                                         // until there are more than 256
    bool many = false;
    uint32_t lastPx = 0xFFFFFFFF;

    void Add(uint32_t px) {
        px &= 0xFFFFFF;
        const int r = px >> 16, g = (px >> 8) & 255, b = px & 255;
        const int i = ((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2);
        ++count[i];
        sr[i] += r;
        sg[i] += g;
        sb[i] += b;
        if (many || px == lastPx) return;
        lastPx = px;
        uint64_t& word = seen[px >> 6];
        const uint64_t bit = 1ull << (px & 63);
        if (word & bit) return;
        word |= bit;
        exact.push_back(px);
        if (exact.size() > 256) many = true;
    }
};

std::vector<Rgb> BuildPalette(const Histogram& hist, int colors) {
    std::vector<Rgb> pal;
    if (!hist.many && static_cast<int>(hist.exact.size()) <= colors) {
        for (uint32_t c : hist.exact) pal.push_back({static_cast<int>(c >> 16), static_cast<int>((c >> 8) & 255), static_cast<int>(c & 255)});
        return pal;
    }
    Wu wu;
    for (int i = 0; i < Histogram::kBins; ++i) {
        if (!hist.count[i]) continue;
        const int r6 = i >> 12, g6 = (i >> 6) & 63, b6 = i & 63;
        const double n = hist.count[i];
        const double mr = static_cast<double>(hist.sr[i]) / n, mg = static_cast<double>(hist.sg[i]) / n,
                     mb = static_cast<double>(hist.sb[i]) / n;
        wu.Add(r6 >> 1, g6 >> 1, b6 >> 1, n, static_cast<double>(hist.sr[i]), static_cast<double>(hist.sg[i]),
               static_cast<double>(hist.sb[i]), n * (mr * mr + mg * mg + mb * mb));
    }
    pal = wu.Run(colors);

    // k-means on the 6-bit bins: moves each color to the true middle of the pixels it stands for.
    struct Bin {
        int r, g, b;
        double n;
    };
    std::vector<Bin> bins;
    for (int i = 0; i < Histogram::kBins; ++i)
        if (hist.count[i]) {
            const double n = hist.count[i];
            bins.push_back({static_cast<int>(hist.sr[i] / hist.count[i]), static_cast<int>(hist.sg[i] / hist.count[i]),
                            static_cast<int>(hist.sb[i] / hist.count[i]), n});
        }
    for (int iter = 0; iter < 4; ++iter) {
        std::vector<double> ar(pal.size()), ag(pal.size()), ab(pal.size()), an(pal.size());
        for (const Bin& b : bins) {
            int best = 0, bd = INT32_MAX;
            for (size_t p = 0; p < pal.size(); ++p) {
                const int d = Dist(pal[p], b.r, b.g, b.b);
                if (d < bd) {
                    bd = d;
                    best = static_cast<int>(p);
                }
            }
            ar[best] += b.r * b.n;
            ag[best] += b.g * b.n;
            ab[best] += b.b * b.n;
            an[best] += b.n;
        }
        for (size_t p = 0; p < pal.size(); ++p)
            if (an[p] > 0)
                pal[p] = {static_cast<int>(ar[p] / an[p] + 0.5), static_cast<int>(ag[p] / an[p] + 0.5),
                          static_cast<int>(ab[p] / an[p] + 0.5)};
    }
    return pal;
}

// Nearest palette color, cached per 6-bit cell (screen content repeats the same colors a lot).
class Mapper {
public:
    explicit Mapper(const std::vector<Rgb>& pal) : pal_(pal), key_(1 << 18, 0xFFFFFFFF), idx_(1 << 18) {}
    uint8_t operator()(uint32_t px) {
        px &= 0xFFFFFF;
        const int r = px >> 16, g = (px >> 8) & 255, b = px & 255;
        const int cell = ((r >> 2) << 12) | ((g >> 2) << 6) | (b >> 2);
        if (key_[cell] == px) return idx_[cell];
        const uint8_t i = Nearest(r, g, b);
        key_[cell] = px;
        idx_[cell] = i;
        return i;
    }
    uint8_t Nearest(int r, int g, int b) const {
        int best = 0, bd = INT32_MAX;
        for (size_t p = 0; p < pal_.size(); ++p) {
            const int d = Dist(pal_[p], r, g, b);
            if (d < bd) {
                bd = d;
                best = static_cast<int>(p);
            }
        }
        return static_cast<uint8_t>(best);
    }

private:
    const std::vector<Rgb>& pal_;
    std::vector<uint32_t> key_;
    std::vector<uint8_t> idx_;
};

// ---- LZW (as giflib writes it) ---------------------------------------------------------------

class Lzw {
public:
    Lzw() : next_(4096 * 256) {}

    void Encode(const uint8_t* px, size_t n, int minBits, std::vector<uint8_t>& out) {
        out.push_back(static_cast<uint8_t>(minBits));
        out_ = &out;
        block_.clear();
        acc_ = 0;
        accBits_ = 0;
        clear_ = 1 << minBits;
        eoi_ = clear_ + 1;
        minBits_ = minBits;
        Reset();
        Emit(clear_);
        int cur = px[0];
        for (size_t i = 1; i < n; ++i) {
            const int c = px[i];
            const int k = next_[cur * 256 + c];
            if (k) {
                cur = k;
                continue;
            }
            Emit(cur);
            if (running_ >= 4095) {
                Emit(clear_);
                Reset();
            } else {
                next_[cur * 256 + c] = static_cast<uint16_t>(running_);
                used_.push_back(cur * 256 + c);
                ++running_;
            }
            cur = c;
        }
        Emit(cur);
        Emit(eoi_);
        if (accBits_ > 0) Byte(static_cast<uint8_t>(acc_ & 0xFF));
        FlushBlock();
        out.push_back(0);
    }

private:
    std::vector<uint16_t> next_;
    std::vector<int> used_;
    std::vector<uint8_t>* out_ = nullptr;
    std::vector<uint8_t> block_;
    uint32_t acc_ = 0;
    int accBits_ = 0, bits_ = 0, running_ = 0, maxCode_ = 0, clear_ = 0, eoi_ = 0, minBits_ = 0;

    void Reset() {
        for (int k : used_) next_[k] = 0;
        used_.clear();
        running_ = eoi_ + 1;
        bits_ = minBits_ + 1;
        maxCode_ = 1 << bits_;
    }
    void Emit(int code) {
        acc_ |= static_cast<uint32_t>(code) << accBits_;
        accBits_ += bits_;
        while (accBits_ >= 8) {
            Byte(static_cast<uint8_t>(acc_ & 0xFF));
            acc_ >>= 8;
            accBits_ -= 8;
        }
        if (running_ >= maxCode_ && bits_ < 12) maxCode_ = 1 << ++bits_;
    }
    void Byte(uint8_t b) {
        block_.push_back(b);
        if (block_.size() == 255) FlushBlock();
    }
    void FlushBlock() {
        if (block_.empty()) return;
        out_->push_back(static_cast<uint8_t>(block_.size()));
        out_->insert(out_->end(), block_.begin(), block_.end());
        block_.clear();
    }
};

const int kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

}  // namespace

// ---- recording writer ------------------------------------------------------------------------

bool GifWriter::Open(const std::wstring& path, int width, int height) {
    if (_wfopen_s(&f_, path.c_str(), L"wb") || !f_) return false;
    setvbuf(f_, nullptr, _IOFBF, 1 << 20);
    w_ = width;
    h_ = height;
    prev_.clear();
    fwrite("CGF1", 1, 4, f_);
    const int32_t wh[2] = {w_, h_};
    fwrite(wh, 4, 2, f_);
    bytes_ = 12;
    return true;
}

void GifWriter::Frame(uint32_t timeMs, const uint32_t* px) {
    if (!f_) return;
    struct T {
        uint16_t x, y, w, h;
    };
    std::vector<T> tiles;
    const bool first = prev_.empty();
    if (first) prev_.assign(px, px + static_cast<size_t>(w_) * h_);
    for (int ty = 0; ty < h_; ty += kTile)
        for (int tx = 0; tx < w_; tx += kTile) {
            const int tw = std::min(kTile, w_ - tx), th = std::min(kTile, h_ - ty);
            bool changed = first;
            for (int row = 0; row < th && !changed; ++row) {
                const size_t o = static_cast<size_t>(ty + row) * w_ + tx;
                changed = memcmp(px + o, &prev_[o], tw * 4) != 0;
            }
            if (changed)
                tiles.push_back({static_cast<uint16_t>(tx), static_cast<uint16_t>(ty), static_cast<uint16_t>(tw),
                                 static_cast<uint16_t>(th)});
        }
    if (tiles.empty()) return;
    const uint32_t head[2] = {timeMs, static_cast<uint32_t>(tiles.size())};
    fwrite(head, 4, 2, f_);
    bytes_ += 8;
    for (const T& t : tiles) {
        fwrite(&t, 2, 4, f_);
        bytes_ += 8;
        for (int row = 0; row < t.h; ++row) {
            const size_t o = static_cast<size_t>(t.y + row) * w_ + t.x;
            fwrite(px + o, 4, t.w, f_);
            memcpy(&prev_[o], px + o, t.w * 4);
        }
        bytes_ += static_cast<uint64_t>(t.w) * t.h * 4;
    }
}

bool GifWriter::Close(uint32_t timeMs) {
    if (!f_) return false;
    const uint32_t head[2] = {timeMs, kEndMark};
    fwrite(head, 4, 2, f_);
    const bool ok = fclose(f_) == 0;
    f_ = nullptr;
    prev_.clear();
    prev_.shrink_to_fit();
    return ok;
}

GifWriter::~GifWriter() {
    if (f_) fclose(f_);
}

// ---- encoder ---------------------------------------------------------------------------------

bool Gif_Encode(const std::wstring& recording, const GifPreset& p, const std::wstring& outPath,
                const std::atomic<bool>& cancel, std::wstring& err) {
    // Pass 1: every color that ever shows, for the palette.
    Histogram hist;
    int W = 0, H = 0;
    {
        Reader rd;
        if (!rd.Open(recording)) {
            err = L"can't read the recording";
            return false;
        }
        W = rd.w;
        H = rd.h;
        std::vector<uint32_t> canvas(static_cast<size_t>(W) * H);
        uint32_t t;
        bool end = false;
        while (!end && rd.Next(t, end, canvas, [&](int x, int y, int tw, int th) {
            for (int row = 0; row < th; ++row) {
                const uint32_t* s = &canvas[static_cast<size_t>(y + row) * W + x];
                for (int i = 0; i < tw; ++i) hist.Add(s[i]);
            }
        })) {
            if (cancel) return false;
        }
    }
    const int colors = std::max(2, p.colors - 1);  // the last index is "transparent"
    const std::vector<Rgb> pal = BuildPalette(hist, colors);
    int bits = 1;
    while ((1 << bits) < static_cast<int>(pal.size()) + 1) ++bits;
    const int tableSize = 1 << bits;
    const uint8_t transparent = static_cast<uint8_t>(tableSize - 1);
    Mapper map(pal);

    FILE* out = nullptr;
    if (_wfopen_s(&out, outPath.c_str(), L"wb") || !out) {
        err = L"can't write the GIF";
        return false;
    }
    setvbuf(out, nullptr, _IOFBF, 1 << 20);
    auto put16 = [&](int v) {
        fputc(v & 255, out);
        fputc((v >> 8) & 255, out);
    };
    fwrite("GIF89a", 1, 6, out);
    put16(W);
    put16(H);
    fputc(0xF0 | (bits - 1), out);  // global color table, 8-bit source, size
    fputc(0, out);
    fputc(0, out);
    for (int i = 0; i < tableSize; ++i) {
        const Rgb c = i < static_cast<int>(pal.size()) ? pal[i] : Rgb{0, 0, 0};
        fputc(c.r, out);
        fputc(c.g, out);
        fputc(c.b, out);
    }
    fwrite("\x21\xFF\x0BNETSCAPE2.0\x03\x01\x00\x00\x00", 1, 19, out);  // loop forever

    // Pass 2: frames.
    Reader rd;
    rd.Open(recording);
    std::vector<uint32_t> canvas(static_cast<size_t>(W) * H);
    std::vector<uint8_t> shown(static_cast<size_t>(W) * H, transparent);
    std::vector<uint8_t> next(static_cast<size_t>(W) * H);
    std::vector<uint8_t> changed(static_cast<size_t>(W) * H);
    const int tilesX = (W + kTile - 1) / kTile, tilesY = (H + kTile - 1) / kTile;
    std::vector<uint8_t> dirty(static_cast<size_t>(tilesX) * tilesY);
    bool anyDirty = false;
    const int tol2 = p.tolerance * p.tolerance * 9;  // in Dist() units (weights 3+4+2)
    const int ditherAt = 6 * 6 * 9;

    struct Pending {
        bool has = false;
        uint32_t time = 0;
        int x = 0, y = 0, w = 0, h = 0;
        std::vector<uint8_t> lzw;
    } pending;
    Lzw lzw;
    std::vector<uint8_t> sub;
    int64_t writtenCs = 0;  // total delay written so far, for exact timing without drift
    uint32_t firstTime = 0;
    bool haveFirst = false;

    auto writePending = [&](uint32_t until) {
        if (!pending.has) return;
        const int64_t endCs = (static_cast<int64_t>(until) - firstTime + 5) / 10;
        int delay = static_cast<int>(endCs - writtenCs);
        if (delay < 2) delay = 2;
        writtenCs += delay;
        const uint8_t gce[8] = {0x21, 0xF9, 4, static_cast<uint8_t>((1 << 2) | 1), static_cast<uint8_t>(delay & 255),
                                static_cast<uint8_t>(delay >> 8), transparent, 0};
        fwrite(gce, 1, 8, out);
        fputc(0x2C, out);
        put16(pending.x);
        put16(pending.y);
        put16(pending.w);
        put16(pending.h);
        fputc(0, out);
        fwrite(pending.lzw.data(), 1, pending.lzw.size(), out);
        pending.has = false;
    };

    // Turns the canvas into a GIF frame: only pixels that look different from what is shown.
    auto emit = [&](uint32_t time) {
        int x0 = W, y0 = H, x1 = -1, y1 = -1;
        for (int ty = 0; ty < tilesY; ++ty)
            for (int tx = 0; tx < tilesX; ++tx) {
                uint8_t& d = dirty[static_cast<size_t>(ty) * tilesX + tx];
                if (!d) continue;
                d = 0;
                const int ex = std::min(W, (tx + 1) * kTile), ey = std::min(H, (ty + 1) * kTile);
                for (int y = ty * kTile; y < ey; ++y)
                    for (int x = tx * kTile; x < ex; ++x) {
                        const size_t o = static_cast<size_t>(y) * W + x;
                        const uint32_t px = canvas[o];
                        uint8_t idx = map(px);
                        const int r = (px >> 16) & 255, g = (px >> 8) & 255, b = px & 255;
                        if (p.dither && Dist(pal[idx], r, g, b) > ditherAt) {
                            const int off = (kBayer[y & 3][x & 3] - 8) * 2;
                            idx = map(static_cast<uint32_t>(std::clamp(r + off, 0, 255) << 16 |
                                                            std::clamp(g + off, 0, 255) << 8 | std::clamp(b + off, 0, 255)));
                        }
                        const uint8_t old = shown[o];
                        bool diff = idx != old;
                        if (diff && old != transparent && tol2 > 0 && Dist(pal[old], r, g, b) <= tol2) diff = false;
                        changed[o] = diff;
                        if (!diff) continue;
                        next[o] = idx;
                        x0 = std::min(x0, x);
                        y0 = std::min(y0, y);
                        x1 = std::max(x1, x);
                        y1 = std::max(y1, y);
                    }
            }
        anyDirty = false;
        if (x1 < 0) return;  // nothing visible changed: the previous frame just stays longer
        writePending(time);
        const int fw = x1 - x0 + 1, fh = y1 - y0 + 1;
        sub.resize(static_cast<size_t>(fw) * fh);
        for (int y = 0; y < fh; ++y)
            for (int x = 0; x < fw; ++x) {
                const size_t o = static_cast<size_t>(y0 + y) * W + x0 + x;
                if (changed[o]) {
                    sub[static_cast<size_t>(y) * fw + x] = next[o];
                    shown[o] = next[o];
                    changed[o] = 0;
                } else {
                    sub[static_cast<size_t>(y) * fw + x] = transparent;
                }
            }
        pending.lzw.clear();
        lzw.Encode(sub.data(), sub.size(), std::max(2, bits), pending.lzw);
        pending = {true, time, x0, y0, fw, fh, std::move(pending.lzw)};
    };

    const uint32_t interval = 1000 / std::max(1, p.fps);
    const uint32_t slack = std::min<uint32_t>(10, interval / 4);  // capture timer jitter
    uint32_t nextSlot = 0, t = 0, lastT = 0;
    bool end = false;
    while (!end && rd.Next(t, end, canvas, [&](int x, int y, int, int) {
        dirty[static_cast<size_t>(y / kTile) * tilesX + x / kTile] = 1;
        anyDirty = true;
    })) {
        if (cancel) {
            fclose(out);
            DeleteFileW(outPath.c_str());
            return false;
        }
        if (end) break;
        if (!haveFirst) {
            haveFirst = true;
            firstTime = t;
            nextSlot = t;
        }
        lastT = t;
        if (t + slack >= nextSlot) {  // this record's time slot is due: show the screen as it is now
            emit(t);
            while (nextSlot <= t + slack) nextSlot += interval;
        }
    }
    if (anyDirty) emit(lastT);  // the last changes, even when they fell between slots
    writePending(end ? std::max(t, lastT + 100) : lastT + 100);
    fputc(0x3B, out);
    const bool ok = fclose(out) == 0 && haveFirst;
    if (!ok) {
        err = L"the GIF could not be written";
        DeleteFileW(outPath.c_str());
    }
    return ok;
}
