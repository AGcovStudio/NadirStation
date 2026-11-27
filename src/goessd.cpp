// GOES-N/O/P Sensor Data receive chain: see goessd.h for the format and the approach.
#include "goessd.h"
#include "geomap.h"
#include "runtime_logger.h"
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimeZone>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace nadirstation::goessd {
namespace {

// 256 x 256 RGB, row = IR 10.7 um, column = visible: SatDump's GOES false-colour table.
const unsigned char kFalseColorLut[256 * 256 * 3] = {
#include "gvar_fc_lut.inc"
};

// Fixed 60-byte sequence every imager block is XORed with (its first 14 bits are the sync word 0x2B50).
const uint8_t kPn[60] = {
    0xad, 0x43, 0xc4, 0x7e, 0x31, 0x6c, 0x28, 0xae, 0xde, 0x63, 0xd0, 0x93, 0x2f, 0x10, 0xf0, 0x07, 0xc2, 0x0e, 0x8c, 0xdf,
    0x6b, 0x12, 0xe1, 0x83, 0x27, 0x56, 0xe3, 0x92, 0xa3, 0xb3, 0xbb, 0xfd, 0x6e, 0x7b, 0x1a, 0xa7, 0x90, 0xb2, 0x37, 0x5e,
    0xa5, 0x81, 0x36, 0xd2, 0x06, 0xca, 0xcc, 0x7e, 0x73, 0x5c, 0xb4, 0x05, 0xd3, 0x8a, 0x69, 0x87, 0x04, 0x5f, 0x29, 0x22};
constexpr uint16_t kSync = 0x2B50;
constexpr uint32_t kSounderMarker = 0x3683A2E6u;
constexpr int kBlockBits = 480;
constexpr int kSounderBits = 4224, kSounderSkip = 544;
constexpr double kAspect = 1.75;           // visible samples (0.57 km) per row spacing (1 km); IR: 2.28 km per 4 km
constexpr int kMaxScans = 360;             // one output segment (about 7 minutes)
constexpr int kMinScans = 4;

struct Tables {
    uint8_t pnBit[kBlockBits];
    float pnSign[kBlockBits];
    uint8_t sndMask[kSounderBits];
    Tables() {
        for (int i = 0; i < kBlockBits; ++i) {
            pnBit[i] = (kPn[i >> 3] >> (7 - (i & 7))) & 1;
            pnSign[i] = pnBit[i] ? -1.f : 1.f;
        }
        // 13-stage whitening sequence of the sounder, restarted 68 bytes after the marker.
        uint8_t p[kSounderBits] = {};
        const uint8_t seed[2] = {0xF5, 0xDB};
        for (int i = 0; i < 13; ++i) p[i] = (seed[i >> 3] >> (7 - (i & 7))) & 1;
        for (int i = 13; i < kSounderBits; ++i) p[i] = p[i - 1] ^ p[i - 11] ^ p[i - 12] ^ p[i - 13];
        for (int i = 0; i < kSounderBits; ++i) sndMask[i] = p[i] ^ 1;
    }
};
const Tables& tables() { static const Tables t; return t; }

constexpr float kBitWeights[5] = {100.f, 35.f, 13.f, 4.f, 1.f};

QRgb stops(const std::vector<std::pair<double, QColor>>& s, double t);

// ---- channel table ------------------------------------------------------------------------------
// IR sample group g (word pairs 10/11 of the g-th 12-word group) -> instrument channel. Identified by correlating each
// group with a GVAR sector of the same satellite (2026-11-22 06:15 UTC): group 1 -> 3.9 um (r = 0.74), group 2 ->
// 10.7 um (r = 0.81), group 3 -> 6.5 um (r = 0.57). Group 4 carries a constant (13.3 um is not in the stream).
enum class Kind { Window, WaterVapour, Shortwave, Plain };
struct IrChannel { const char* file; const char* label; double micron; Kind kind; };
const IrChannel kIr[4] = {
    {"GOES_SD_IR1.png", "IR 3.9 um (group 1)", 3.9, Kind::Shortwave},
    {"GOES_SD_IR2.png", "IR 10.7 um (group 2)", 10.7, Kind::Window},
    {"GOES_SD_IR3.png", "IR 6.5 um water vapour (group 3)", 6.5, Kind::WaterVapour},
    {"GOES_SD_IR4.png", "IR group 4 (constant, no image)", 0.0, Kind::Plain},
};

// ---- approximate calibration ----------------------------------------------------------------------
// SD carries raw detector counts. They are mapped to GVAR counts by a line through the space count (zero radiance),
// gain fitted against the GVAR sector above (~3000 co-located blocks, BT residual MAD 0.2 K on uniform scenes):
//     C_gvar = gain * (C - C_space) + b,   C = 1024 - (stored sample, which rises as the scene gets colder)
// then NOAA OSPO's GOES-15 tables: R = (C_gvar - b) / m, Teff = c2 n / ln(1 + c1 n^3 / R), T = a + beta Teff.
struct IrCal { double gain, m, b, n, a, beta, defaultSpace; };
const IrCal kIrCal[3] = {
    {1.0181, 227.3889, 68.2167, 2562.7905, -1.5693377, 1.0025034, 57.0},     // 3.9 um
    {1.0302, 5.2285, 15.6854, 935.83788, -0.35733864, 1.00126425, 58.0},     // 10.7 um
    {1.9169, 38.8383, 29.1287, 1521.36325, -3.47310565, 1.0093067, 60.0},    // 6.5 um
};
constexpr double kC1 = 1.191066e-5, kC2 = 1.438833;

// Brightness temperature (K) for every 10-bit stored sample, or 0 where the radiance is not positive.
std::array<float, 1024> btTable(int group, double space) {
    std::array<float, 1024> t{};
    const IrCal& c = kIrCal[group];
    for (int v = 1; v < 1024; ++v) {
        const double counts = 1024.0 - double(v);
        const double r = c.gain * (counts - space) / c.m;
        if (r <= 1e-4) { t[size_t(v)] = 0.0f; continue; }
        const double teff = kC2 * c.n / std::log1p(kC1 * c.n * c.n * c.n / r);
        t[size_t(v)] = float(c.a + c.beta * teff);
    }
    return t;
}

// Fixed palettes in kelvin, so the pictures from different passes compare.
QRgb paletteCloudTopK(double k) {      // grey for warm scenes, colours below 240 K
    static const std::vector<std::pair<double, QColor>> s{
        {0.0, QColor(255, 255, 255)}, {8.0 / 155, QColor(255, 0, 255)}, {15.0 / 155, QColor(220, 0, 0)},
        {25.0 / 155, QColor(255, 120, 0)}, {35.0 / 155, QColor(255, 235, 0)}, {45.0 / 155, QColor(0, 200, 70)},
        {55.0 / 155, QColor(0, 90, 255)}, {64.9 / 155, QColor(0, 200, 255)}, {65.0 / 155, QColor(240, 240, 240)},
        {75.0 / 155, QColor(215, 215, 215)}, {115.0 / 155, QColor(110, 110, 110)}, {1.0, QColor(20, 20, 20)}};
    return stops(s, (k - 175.0) / 155.0);   // 175 .. 330 K
}
QRgb paletteWaterVapourK(double k) {
    static const std::vector<std::pair<double, QColor>> s{
        {0.0, QColor(230, 0, 0)}, {10.0 / 95, QColor(255, 230, 0)}, {20.0 / 95, QColor(0, 170, 60)},
        {33.0 / 95, QColor(0, 70, 200)}, {45.0 / 95, QColor(120, 170, 255)}, {60.0 / 95, QColor(235, 235, 235)},
        {75.0 / 95, QColor(170, 90, 20)}, {1.0, QColor(60, 30, 0)}};
    return stops(s, (k - 195.0) / 95.0);    // 195 .. 290 K
}

constexpr int kFalseColourIr = 1;           // group that plays "IR 10.7" in the false-colour table
constexpr bool kRawHighIsCold = true;       // IR counts rise as the scene gets colder

QRgb stops(const std::vector<std::pair<double, QColor>>& s, double t) {
    t = std::clamp(t, 0.0, 1.0);
    for (size_t i = 1; i < s.size(); ++i) {
        if (t <= s[i].first) {
            const double u = (t - s[i - 1].first) / std::max(1e-9, s[i].first - s[i - 1].first);
            const QColor& a = s[i - 1].second; const QColor& b = s[i].second;
            return qRgb(int(a.red() + (b.red() - a.red()) * u + 0.5), int(a.green() + (b.green() - a.green()) * u + 0.5),
                        int(a.blue() + (b.blue() - a.blue()) * u + 0.5));
        }
    }
    return s.back().second.rgb();
}
QRgb rampWaterVapour(double t) {   // t: 0 dry/warm .. 1 moist/cold
    static const std::vector<std::pair<double, QColor>> s{{0.0, QColor(20, 14, 10)}, {0.3, QColor(120, 100, 70)},
        {0.5, QColor(225, 220, 205)}, {0.68, QColor(120, 215, 235)}, {0.84, QColor(40, 110, 225)}, {1.0, QColor(235, 70, 215)}};
    return stops(s, t);
}
QRgb rampShortwave(double t) {      // t: 0 cold .. 1 hot: grey scale, the hottest few per cent in orange / red
    static const std::vector<std::pair<double, QColor>> s{{0.0, QColor(0, 0, 0)}, {0.93, QColor(235, 235, 235)},
        {0.965, QColor(255, 190, 0)}, {1.0, QColor(255, 20, 0)}};
    return stops(s, t);
}

// ---- image processing helpers -------------------------------------------------------------------

// Bit-plane vote of every pixel over a (wr x wc) neighbourhood from the soft bits of its 10 bits.
void voteDenoise(const int8_t* soft, int rows, int cols, int wr, int wc, float side, uint16_t* out) {
    const size_t n = size_t(rows) * size_t(cols);
    std::fill(out, out + n, uint16_t(0));
    std::vector<float> prefix(size_t(cols) + 1), hs(n);
    const int hc = wc / 2, hr = wr / 2;
    for (int k = 0; k < 10; ++k) {
        for (int r = 0; r < rows; ++r) {
            const int8_t* s = soft + size_t(r) * size_t(cols) * 10 + k;
            prefix[0] = 0.f;
            for (int c = 0; c < cols; ++c) prefix[size_t(c) + 1] = prefix[size_t(c)] + float(s[size_t(c) * 10]);
            float* h = hs.data() + size_t(r) * size_t(cols);
            for (int c = 0; c < cols; ++c) h[c] = prefix[size_t(std::min(c + hc, cols - 1)) + 1] - prefix[size_t(std::max(c - hc, 0))];
        }
        const uint16_t bit = uint16_t(1u << (9 - k));
        for (int r = 0; r < rows; ++r) {
            const int r0 = std::max(r - hr, 0), r1 = std::min(r + hr, rows - 1);
            uint16_t* o = out + size_t(r) * size_t(cols);
            for (int c = 0; c < cols; ++c) {
                float v = 0.f;
                for (int q = r0; q <= r1; ++q) v += (q == r ? 1.f : side) * hs[size_t(q) * size_t(cols) + size_t(c)];
                if (v > 0.f) o[c] |= bit;
            }
        }
    }
}

// Isolated dots left by a wrong high-order bit: only a pixel far from the median of its 3 x 3 neighbourhood takes it.
void removeDots(uint16_t* a, int rows, int cols) {
    const size_t n = size_t(rows) * size_t(cols);
    std::vector<uint16_t> med(n);
    std::vector<int> dev(n);
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            uint16_t w[9];
            int m = 0;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc)
                    w[m++] = a[size_t(std::clamp(r + dr, 0, rows - 1)) * size_t(cols) + size_t(std::clamp(c + dc, 0, cols - 1))];
            std::nth_element(w, w + 4, w + 9);
            med[size_t(r) * size_t(cols) + size_t(c)] = w[4];
            dev[size_t(r) * size_t(cols) + size_t(c)] = std::abs(int(a[size_t(r) * size_t(cols) + size_t(c)]) - int(w[4]));
        }
    }
    std::vector<int> d2(dev);
    std::nth_element(d2.begin(), d2.begin() + long(n / 2), d2.end());
    const int limit = std::max(24, 6 * d2[n / 2]);
    for (size_t i = 0; i < n; ++i) if (dev[i] > limit) a[i] = med[i];
}

// IR rows are independent (two detectors on different ground): outliers against the median of the same row only.
void removeDotsRow(uint16_t* a, int rows, int cols, int half) {
    std::vector<uint16_t> med(size_t(rows) * size_t(cols));
    std::vector<int> dev(size_t(rows) * size_t(cols));
    std::vector<uint16_t> w;
    for (int r = 0; r < rows; ++r)
        for (int c = 0; c < cols; ++c) {
            w.clear();
            for (int k = std::max(0, c - half); k <= std::min(cols - 1, c + half); ++k) w.push_back(a[size_t(r) * size_t(cols) + size_t(k)]);
            std::nth_element(w.begin(), w.begin() + long(w.size() / 2), w.end());
            const size_t i = size_t(r) * size_t(cols) + size_t(c);
            med[i] = w[w.size() / 2];
            dev[i] = std::abs(int(a[i]) - int(med[i]));
        }
    std::vector<int> d2(dev);
    std::nth_element(d2.begin(), d2.begin() + long(d2.size() / 2), d2.end());
    const int limit = std::max(20, 5 * d2[d2.size() / 2]);
    for (size_t i = 0; i < dev.size(); ++i) if (dev[i] > limit) a[i] = med[i];
}

// Along-scan brightness profile (median of 16-sample bins over all rows), for scan-to-scan alignment.
std::vector<float> scanProfile(const std::vector<uint16_t>& vis, int width, int bin = 16) {
    const int bins = width / bin;
    std::vector<float> p(size_t(std::max(bins, 0)));
    std::vector<uint16_t> w(size_t(8 * bin));
    for (int b = 0; b < bins; ++b) {
        int m = 0;
        for (int r = 0; r < 8; ++r) for (int k = 0; k < bin; ++k) w[size_t(m++)] = vis[size_t(r) * size_t(width) + size_t(b * bin + k)];
        std::nth_element(w.begin(), w.begin() + long(w.size() / 2), w.end());
        p[size_t(b)] = w[w.size() / 2];
    }
    if (!p.empty()) {
        std::vector<float> q(p);
        std::nth_element(q.begin(), q.begin() + long(q.size() / 2), q.end());
        const float m = q[q.size() / 2];
        for (auto& v : p) v -= m;
    }
    return p;
}
double stdev(const std::vector<float>& v) {
    if (v.size() < 2) return 0.0;
    double m = 0, s = 0;
    for (float x : v) m += x;
    m /= double(v.size());
    for (float x : v) s += (x - m) * (x - m);
    return std::sqrt(s / double(v.size()));
}
double correlation(const float* a, const float* b, int n) {
    double ma = 0, mb = 0;
    for (int i = 0; i < n; ++i) { ma += a[i]; mb += b[i]; }
    ma /= n; mb /= n;
    double sab = 0, saa = 0, sbb = 0;
    for (int i = 0; i < n; ++i) { const double x = a[i] - ma, y = b[i] - mb; sab += x * y; saa += x * x; sbb += y * y; }
    return saa > 0 && sbb > 0 ? sab / std::sqrt(saa * sbb) : 0.0;
}
// Does `cur` run the other way than `prev`, and by how many bins is it offset? false: the scene has no structure.
bool orient(const std::vector<float>& prev, const std::vector<float>& cur, int maxShift, bool& flip, int& shiftBins, double& score) {
    if (prev.empty() || cur.empty() || stdev(prev) < 4 || stdev(cur) < 4) return false;
    bool have = false;
    std::vector<float> rev(cur.rbegin(), cur.rend());
    for (int f = 0; f < 2; ++f) {
        const std::vector<float>& c = f ? rev : cur;
        for (int shift = -maxShift; shift <= maxShift; ++shift) {
            const int a0 = std::max(0, shift), b0 = std::max(0, -shift);
            const int n = std::min(int(prev.size()) - a0, int(c.size()) - b0);
            if (n < 40) continue;
            const double sc = correlation(prev.data() + a0, c.data() + b0, n);
            if (!have || sc > score) { have = true; score = sc; flip = f != 0; shiftBins = shift; }
        }
    }
    return have;
}

void rollRow(uint16_t* row, int n, int shift) {
    if (shift == 0) return;
    if (shift > 0) {
        std::memmove(row + shift, row, size_t(std::max(0, n - shift)) * sizeof(uint16_t));
        std::fill(row, row + std::min(shift, n), uint16_t(0));
    } else {
        const int s = -shift;
        std::memmove(row, row + s, size_t(std::max(0, n - s)) * sizeof(uint16_t));
        std::fill(row + std::max(0, n - s), row + n, uint16_t(0));
    }
}

}  // namespace

// ---- data structures ----------------------------------------------------------------------------

struct Engine::Scan {
    int width = 0;                                  // visible samples; 4 per block
    std::vector<uint16_t> vis;                      // 8 x width, 10-bit counts after the vote, 0 = no data
    std::array<std::vector<uint16_t>, 4> ir;        // 2 x width/4
};

struct Engine::Observation {
    QString stamp;
    int width = 0;
    std::vector<Scan> scans;
    std::vector<std::array<uint16_t, 264>> sounder;
    std::vector<uint16_t> preview;                  // 2 rows per scan, width/8 columns
    uint64_t flips = 0, blanks = 0;
    double mirrorVote = 0.0;
    double syncBer = 0.0, sndParity = 0.0, snr = 0.0;
    uint64_t sndParityOk = 0, sndParityAll = 0;
    double firstSec = 0.0, lastSec = 0.0;
};

// ---- lifetime -----------------------------------------------------------------------------------

Engine::Engine() {
    imager_.setFilterFirst(true);
}

Engine::~Engine() {
    if (job_.valid()) job_.get();
    for (auto& w : writes_) if (w.valid()) w.get();
}

bool Engine::configure(double sampleRate) {
    if (job_.valid()) job_.get();
    sampleRate_ = sampleRate;
    // 2.62 Msym/s with an excess bandwidth near 1: the PSK chain needs about 1.3 samples a symbol.
    if (sampleRate < 1.3 * kImagerRate) { configured_ = false; return false; }
    pendingFront_.clear();
    imager_.configure(float(sampleRate), float(kImagerRate), 0.9f, 0.003f);
    sounder_.configure(240000.0f, float(kSounderRate), 0.5f, 0.01f);
    decA_ = sampleRate / 1.2e6;
    configured_ = true;
    return true;
}

void Engine::reset() {
    finish();
    imager_.reset();
    sounder_.reset();
    pendingFront_.clear();
    decPos_ = 0.0; accA_ = accB_ = {}; nB_ = 0;
    prevSym_ = 0.f; level_ = 0.f; reg_ = 0; bitCount_ = 0;
    hist_.fill(0.f);
    phase_ = -1; pendingPhase_ = -1; pendingVotes_ = 0; nextStart_ = 0; inBlock_ = false;
    blkBits_.clear(); blkSoft_.clear();
    syncErrBits_ = syncBitsSeen_ = blocksSinceEval_ = lastGoodBlock_ = 0;
    buf_.clear(); base_ = 0; lastEnd_ = -100000; prevHeader_ = -1; prevHdrSoft_ = 0.f; sinceCheck_ = 0;
    period_ = 6286.0; lastScanEnd_ = -1; lastProfile_.clear(); lastProfileNo_ = -1000;
    pending_.clear(); refWidth_ = 0; scanNo_ = 0; lastFlip_ = false; altVote_ = 1.0; lowRun_ = emptyRun_ = structured_ = 0;
    outliers_ = 0; modes_ = 0; widthOut_ = 0;
    sPrev_ = 0.f; sReg_ = 0; sCount_ = sBase_ = 0; sBits_.clear(); sPending_.clear(); sLastMarker_ = -1;
    sndParityOk_ = sndParityAll_ = 0; sndLastSec_ = -100.0;
    obs_.reset();
    streamSec_ = 0.0; snrY1_ = snrY2_ = 0.0; lastLiveMs_ = 0;
    {
        std::lock_guard<std::mutex> l(uMutex_);
        live_ = Update();
        completed_.clear();
    }
    lock_ = 0.f; carrierHz_ = 0.f; snr_ = 0.0; syncBer_ = 0.0; sndParity_ = 0.0;
    syncs_ = blocks_ = scans_ = sndFrames_ = flips_ = 0;
    locked_ = false; sndLocked_ = false; phaseOut_ = -1;
}

void Engine::finish() {
    if (job_.valid()) job_.get();
    if (!configured_) {
        for (auto& w : writes_) if (w.valid()) w.get();
        writes_.clear();
        return;
    }
    findScans();
    closeObservation(QStringLiteral("end of reception"));
    for (auto& w : writes_) if (w.valid()) w.get();
    writes_.clear();
}

Engine::Update Engine::takeLive() {
    std::lock_guard<std::mutex> l(uMutex_);
    Update u = std::move(live_);
    live_ = Update();
    return u;
}

std::vector<Engine::Update> Engine::takeCompleted() {
    std::lock_guard<std::mutex> l(uMutex_);
    std::vector<Update> out;
    out.swap(completed_);
    return out;
}

// ---- sample path --------------------------------------------------------------------------------

void Engine::sounderDecimate(const std::vector<std::complex<float>>& in, std::vector<std::complex<float>>& out) {
    out.clear();
    out.reserve(in.size() / 24 + 4);
    for (const auto& x : in) {
        const double room = decA_ - decPos_;
        std::complex<float> done;
        bool got = false;
        if (room >= 1.0 - 1e-9) {
            accA_ += x;
            decPos_ += 1.0;
            if (decPos_ >= decA_ - 1e-9) { done = accA_ / float(decA_); accA_ = {}; decPos_ = 0.0; got = true; }
        } else {
            accA_ += x * float(room);
            done = accA_ / float(decA_);
            accA_ = x * float(1.0 - room);
            decPos_ = 1.0 - room;
            got = true;
        }
        if (got) {
            accB_ += done;
            if (++nB_ == 5) { out.push_back(accB_ / 5.0f); accB_ = {}; nB_ = 0; }
        }
    }
}

dsp::DPSKResult Engine::process(const std::vector<std::complex<float>>& iq) {
    dsp::DPSKResult d;
    if (!configured_ || iq.empty()) return d;
    std::vector<std::complex<float>> snd;
    sounderDecimate(iq, snd);
    // Imager: the filter stage of this block on a helper thread, the loops of the block before here.
    auto front = std::async(std::launch::async, [this, &iq] { return imager_.frontStage(iq); });
    if (!pendingFront_.empty()) d = imager_.backStage(pendingFront_);
    dsp::DPSKResult sd = sounder_.process(snd);
    if (!d.symbols.empty()) {
        carrierHz_ = d.carrierOffsetHz;
        lock_ = imager_.lockMeasure();
    }
    if (job_.valid()) job_.get();
    if (!d.symbols.empty() || !sd.symbols.empty())
        job_ = std::async(std::launch::async, [this, a = d.symbols, b = std::move(sd.symbols)]() mutable { handle(std::move(a), std::move(b)); });
    pendingFront_ = front.get();
    return d;
}

void Engine::handle(std::vector<std::complex<float>> imager, std::vector<std::complex<float>> sounder) {
    imagerSymbols(imager);
    sounderSymbols(sounder);
    // M2M4 signal-to-noise estimate over the real (in-phase) part of the imager symbols: for BPSK of amplitude A in
    // Gaussian noise, M2 = A^2 + s^2 and M4 = A^4 + 6 A^2 s^2 + 3 s^4, so A^2 = sqrt((3 M2^2 - M4) / 2)
    constexpr double alpha = 0.0003, beta = 1.0 - alpha;
    double y1 = snrY1_, y2 = snrY2_;
    for (const auto& z : imager) {
        const double m = double(z.real()) * z.real();
        y1 = alpha * m + beta * y1;
        y2 = alpha * m * m + beta * y2;
    }
    snrY1_ = std::isfinite(y1) ? y1 : 0.0;
    snrY2_ = std::isfinite(y2) ? y2 : 0.0;
    const double a2 = std::sqrt(std::max(0.0, (3.0 * snrY1_ * snrY1_ - snrY2_) / 2.0));
    const double noise = snrY1_ - a2;
    snr_ = (a2 > 0.0 && noise > 1e-12) ? std::max(0.0, 10.0 * std::log10(a2 / noise)) : 0.0;
    pushLive(false);
}

// ---- imager: soft bits, block sync, blocks ------------------------------------------------------

void Engine::imagerSymbols(const std::vector<std::complex<float>>& s) {
    const Tables& T = tables();
    for (const auto& z : s) {
        const float v = z.real();
        if (level_ <= 0.f) level_ = std::max(std::fabs(v), 1e-6f);
        level_ += (std::fabs(v) - level_) * 1e-4f;
        // NRZ-M: a 1 is a change of the channel symbol; reliability = the weaker of the two symbols (min-sum)
        const float mag = std::min(std::fabs(v), std::fabs(prevSym_)) / level_;
        const bool change = (v > 0.f) != (prevSym_ > 0.f);
        const float soft = change ? mag : -mag;
        prevSym_ = v;
        const uint64_t i = bitCount_++;
        const uint8_t bit = change ? 1 : 0;
        if (i % kBlockBits == 0 && i > 0) {
            for (auto& h : hist_) h *= 0.9995f;                       // evidence memory of about 2000 blocks (0.37 s)
            if ((i / kBlockBits) % 64 == 0) evaluatePhase();
        }
        reg_ = uint16_t(((reg_ << 1) | bit) & 0x3fff);
        if (i >= 13) {
            unsigned diff = unsigned(reg_ ^ kSync), errors = 0;
            while (diff && errors < 5) { ++errors; diff &= diff - 1; }
            if (errors < 5) hist_[(i - 13) % kBlockBits] += kBitWeights[errors];
        }
        if (phase_ >= 0) {
            if (i == nextStart_) { inBlock_ = true; blkBits_.clear(); blkSoft_.clear(); }
            if (inBlock_) {
                blkBits_.push_back(bit);
                blkSoft_.push_back(soft);
                if (int(blkBits_.size()) == kBlockBits) {
                    Block b;
                    int ones = 0;
                    for (int k = 0; k < kBlockBits; ++k) {
                        const uint8_t hb = blkBits_[size_t(k)] ^ T.pnBit[k];
                        ones += hb;
                        b.words[size_t(k / 10)] = uint16_t((b.words[size_t(k / 10)] << 1) | hb);
                        b.soft[size_t(k)] = int8_t(std::clamp(std::lround(blkSoft_[size_t(k)] * T.pnSign[k] * 32.f), -127l, 127l));
                    }
                    b.ones = ones;
                    int syncErr = 0;
                    for (int k = 0; k < 14; ++k) syncErr += blkBits_[size_t(k)] ^ T.pnBit[k];
                    syncErrBits_ += uint64_t(syncErr);
                    syncBitsSeen_ += 14;
                    if (syncErr == 0) { ++syncs_; lastGoodBlock_ = blocks_.load(); }
                    inBlock_ = false;
                    nextStart_ += kBlockBits;
                    takeBlock(std::move(b));
                }
            }
        }
    }
    streamSec_ = double(bitCount_) / kImagerRate;
}

void Engine::evaluatePhase() {
    int best = 0;
    for (int k = 1; k < kBlockBits; ++k) if (hist_[size_t(k)] > hist_[size_t(best)]) best = k;
    float second = 0.f;
    for (int k = 0; k < kBlockBits; ++k) if (k != best && hist_[size_t(k)] > second) second = hist_[size_t(k)];
    const float ratio = (hist_[size_t(best)] + 1.f) / (second + 1.f);
    if (phase_ < 0) {
        if (ratio >= 1.5f && hist_[size_t(best)] > 150.f) {
            phase_ = best;
            nextStart_ = bitCount_ + uint64_t((best - int(bitCount_ % kBlockBits) + kBlockBits) % kBlockBits);
            inBlock_ = false;
            phaseOut_ = phase_;
        }
    } else if (best != phase_ && ratio >= 2.0f && hist_[size_t(best)] > 150.f) {
        if (best == pendingPhase_) ++pendingVotes_; else { pendingPhase_ = best; pendingVotes_ = 1; }
        if (pendingVotes_ >= 3) {
            // the symbol clock slipped: restart the block cut at the new phase
            phase_ = best;
            nextStart_ = bitCount_ + uint64_t((best - int(bitCount_ % kBlockBits) + kBlockBits) % kBlockBits);
            inBlock_ = false;
            phaseOut_ = phase_;
            pendingVotes_ = 0;
        }
    } else {
        pendingVotes_ = 0;
    }
    locked_ = phase_ >= 0 && ratio >= 1.5f;
}

void Engine::takeBlock(Block&& b) {
    ++blocks_;
    buf_.push_back(std::move(b));
    if (blocks_ % 512 == 0 && syncBitsSeen_ > 0) {
        syncBer_ = 100.0 * double(syncErrBits_) / double(syncBitsSeen_);
        if (syncBitsSeen_ > 200000) { syncErrBits_ /= 2; syncBitsSeen_ /= 2; }
    }
    if (++sinceCheck_ >= 128) {
        sinceCheck_ = 0;
        findScans();
    }
}

// ---- scans ----------------------------------------------------------------------------------------

void Engine::findScans() {
    const int n = int(buf_.size());
    if (n < 1600) return;
    // 41-block moving average of the ones per block: fill blocks (all zero data) stay far below noise-like image blocks
    std::vector<float> w, pre;
    w.resize(size_t(n));
    pre.assign(size_t(n) + 1, 0.f);
    for (int i = 0; i < n; ++i) { w[size_t(i)] = float(buf_[size_t(i)].ones); pre[size_t(i) + 1] = pre[size_t(i)] + w[size_t(i)]; }
    std::vector<uint8_t> quiet;
    quiet.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        const int a = std::max(0, i - 20), b = std::min(n - 1, i + 20);
        quiet[size_t(i)] = (pre[size_t(b) + 1] - pre[size_t(a)]) / float(b - a + 1) < 158.f;
    }
    for (int i = 1; i < n; ++i) {
        if (!(quiet[size_t(i) - 1] && !quiet[size_t(i)])) continue;
        int edge = i;
        if (edge < 300) continue;
        bool allQuiet = true;
        for (int k = edge - 270; k < edge - 12; ++k) if (!quiet[size_t(k)]) { allQuiet = false; break; }
        if (!allQuiet) continue;
        if (edge + 461 > n) break;
        // the smoothed edge sits early: re-estimate it as a two-level change point on the unsmoothed counts
        {
            const int a = edge - 32;
            double m1 = 0, m2 = 0;
            for (int k = 0; k < 14; ++k) { m1 += w[size_t(a + k)]; m2 += w[size_t(a + 63 - k)]; }
            const double mid = (m1 + m2) / 28.0;
            double acc = 0, best = 0;
            int bi = 0;
            for (int k = 0; k < 64; ++k) {
                acc += w[size_t(a + k)] - mid;
                if (acc < best) { best = acc; bi = k + 1; }
            }
            edge = a + bi;
        }
        const int64_t end = base_ + edge;
        if (end <= lastEnd_ + 200) continue;
        if (edge + 461 > n) break;
        // scan preamble: the type bit 2 of word 1 is set far more often in the first blocks after the fill
        int pre2 = 0, norm = 0;
        for (int k = 22; k < 130; ++k) pre2 += (buf_[size_t(edge + k)].words[1] & 4) ? 1 : 0;
        for (int k = 195; k < 460; ++k) norm += (buf_[size_t(edge + k)].words[1] & 4) ? 1 : 0;
        if (double(pre2) / 108.0 - double(norm) / 265.0 < 0.23) continue;
        // header 173 blocks after the end of the long fill, trailer 916 blocks before the next one
        const int header = edge + 173;
        const float hdrSoft = float(buf_[size_t(header)].soft[33]);     // direction flag (word 3, bit 6): positive = reverse sweep
        if (prevHeader_ >= 0) {
            const int start = int(prevHeader_ - base_ + 1), stop = edge - 916;
            if (start >= 0 && stop - start >= 400 && stop - start <= 6000) {
                int img = 0;
                for (int k = start; k < stop; ++k) img += (buf_[size_t(k)].words[1] & 31) == 26 ? 1 : 0;
                if (double(img) / double(stop - start) >= 0.18) {
                    addScan(start, stop, prevHdrSoft_, end);
                }
            }
        }
        prevHdrSoft_ = hdrSoft;
        prevHeader_ = base_ + header;
        lastEnd_ = end;
    }
    if (int(buf_.size()) > 16000) {
        const int cut = int(buf_.size()) - 13500;
        buf_.erase(buf_.begin(), buf_.begin() + cut);
        base_ += cut;
    }
}

// A scan between two long fills, denoised in the order the data came; width tolerance and direction come next.
struct Engine::Raw {
    int nb = 0;
    int64_t end = 0;
    float headerSoft = 0.f;                         // direction flag of its header (one bit), soft: positive = reverse sweep
    std::vector<uint16_t> vis;                      // 8 x 4nb
    std::array<std::vector<uint16_t>, 4> ir;        // 2 x nb
};

void Engine::addScan(int start, int stop, float headerSoft, int64_t endBlock) {
    const int nb = stop - start;
    const int W = nb * 4;
    auto r = std::make_shared<Raw>();
    r->nb = nb;
    r->end = endBlock;
    r->headerSoft = headerSoft;
    std::vector<uint16_t> visHard(size_t(8) * size_t(W));
    r->vis.resize(size_t(8) * size_t(W));
    std::vector<int8_t> visSoft(size_t(8) * size_t(W) * 10);
    std::array<std::vector<uint16_t>, 4> irHard;
    std::array<std::vector<int8_t>, 4> irSoft;
    for (int c = 0; c < 4; ++c) {
        irHard[size_t(c)].resize(size_t(2) * size_t(nb));
        r->ir[size_t(c)].resize(size_t(2) * size_t(nb));
        irSoft[size_t(c)].resize(size_t(2) * size_t(nb) * 10);
    }
    for (int j = 0; j < nb; ++j) {
        const Block& b = buf_[size_t(start + j)];
        for (int g = 0; g < 4; ++g) {
            for (int d = 0; d < 8; ++d) {
                const int wi = g * 12 + 9 - d;
                const int col = 4 * j + g;
                visHard[size_t(d) * size_t(W) + size_t(col)] = b.words[size_t(wi)];
                std::memcpy(&visSoft[(size_t(d) * size_t(W) + size_t(col)) * 10], &b.soft[size_t(wi) * 10], 10);
            }
            for (int q = 0; q < 2; ++q) {
                const int wi = g * 12 + 11 - q;
                irHard[size_t(g)][size_t(q) * size_t(nb) + size_t(j)] = b.words[size_t(wi)];
                std::memcpy(&irSoft[size_t(g)][(size_t(q) * size_t(nb) + size_t(j)) * 10], &b.soft[size_t(wi) * 10], 10);
            }
        }
    }
    if (!soft_) {
        for (size_t p = 0; p < visHard.size(); ++p)
            for (int k = 0; k < 10; ++k) visSoft[p * 10 + size_t(k)] = ((visHard[p] >> (9 - k)) & 1) ? 32 : -32;
        for (int g = 0; g < 4; ++g)
            for (size_t p = 0; p < irHard[size_t(g)].size(); ++p)
                for (int k = 0; k < 10; ++k) irSoft[size_t(g)][p * 10 + size_t(k)] = ((irHard[size_t(g)][p] >> (9 - k)) & 1) ? 32 : -32;
    }
    voteDenoise(visSoft.data(), 8, W, 3, 5, 1.0f, r->vis.data());
    removeDots(r->vis.data(), 8, W);
    for (int g = 0; g < 4; ++g) {
        // the two IR detectors see ground 4 km apart: the other row counts half
        voteDenoise(irSoft[size_t(g)].data(), 2, nb, 3, 5, 0.5f, r->ir[size_t(g)].data());
        removeDotsRow(r->ir[size_t(g)].data(), 2, nb, 4);
    }

    // Width: a few per cent of wobble is the same picture; a different width has to repeat in 3 scans in a row
    // to start another one, and a lone odd scan is dropped.
    const auto matches = [](int a, int b) { return std::abs(a - b) <= std::max(64, int(std::lround(0.015 * a))); };
    if (refWidth_ > 0 && matches(refWidth_, W)) {
        outliers_ += pending_.size();
        pending_.clear();
        commitScan(r);
        return;
    }
    if (!pending_.empty() && !matches(pending_[0]->nb * 4, W)) {
        outliers_ += pending_.size();
        pending_.clear();
    }
    pending_.push_back(r);
    if (pending_.size() < 3) return;
    std::vector<int> widths;
    for (const auto& p : pending_) widths.push_back(p->nb * 4);
    std::sort(widths.begin(), widths.end());
    if (refWidth_ > 0) {
        closeObservation(QStringLiteral("scan width changed"));
        ++modes_;
    }
    refWidth_ = widths[1] / 8 * 8;
    widthOut_ = refWidth_;
    auto batch = std::move(pending_);
    pending_.clear();
    for (auto& p : batch) commitScan(p);
}

void Engine::commitScan(std::shared_ptr<Raw> rp) {
    Raw& r = *rp;
    const int W = refWidth_, n4 = W / 4;
    period_ = double(n4 + 1090);          // active blocks plus the turnaround and the fill, as measured
    if (lastScanEnd_ >= 0 && double(r.end - lastScanEnd_) / period_ > 6.5) closeObservation(QStringLiteral("gap in the scans"));
    if (!obs_) {
        obs_ = std::make_shared<Observation>();
        obs_->stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
        obs_->firstSec = streamSec_;
    }
    if (obs_->width == 0) obs_->width = W;
    Observation& o = *obs_;

    // to the picture width: crop or pad about the centre
    std::vector<uint16_t> vis(size_t(8) * size_t(W), 0);
    std::array<std::vector<uint16_t>, 4> ir;
    {
        const int Wr = r.nb * 4, diff = Wr - W;
        const int srcLeft = diff > 0 ? diff / 2 : 0, dstLeft = diff < 0 ? (-diff) / 2 : 0;
        const int n = std::min(Wr, W);
        for (int d = 0; d < 8; ++d)
            std::copy(r.vis.begin() + long(size_t(d) * size_t(Wr) + size_t(srcLeft)),
                      r.vis.begin() + long(size_t(d) * size_t(Wr) + size_t(srcLeft + n)), vis.begin() + long(size_t(d) * size_t(W) + size_t(dstLeft)));
        const int di = r.nb - n4;
        const int sl = di > 0 ? di / 2 : 0, dl = di < 0 ? (-di) / 2 : 0, ni = std::min(r.nb, n4);
        for (int g = 0; g < 4; ++g) {
            ir[size_t(g)].assign(size_t(2) * size_t(n4), 0);
            for (int q = 0; q < 2; ++q)
                std::copy(r.ir[size_t(g)].begin() + long(size_t(q) * size_t(r.nb) + size_t(sl)),
                          r.ir[size_t(g)].begin() + long(size_t(q) * size_t(r.nb) + size_t(sl + ni)), ir[size_t(g)].begin() + long(size_t(q) * size_t(n4) + size_t(dl)));
        }
    }

    // missing scans (a lost turnaround): keep the geometry with blank scans; the directions keep alternating through them
    const bool alt = altVote_ >= 0.0;
    if (lastScanEnd_ >= 0) {
        const double gap = double(r.end - lastScanEnd_) / period_;
        const long k = std::lround(gap);
        if (k >= 2 && k <= 6 && std::fabs(gap - double(k)) < 0.25) {
            for (long q = 1; q < k; ++q) {
                Scan blank;
                blank.width = W;
                blank.vis.assign(size_t(8) * size_t(W), 0);
                for (auto& rr : blank.ir) rr.assign(size_t(2) * size_t(n4), 0);
                o.scans.push_back(std::move(blank));
                o.preview.insert(o.preview.end(), size_t(2) * size_t(W / 8), uint16_t(0));
                ++o.blanks;
                ++scanNo_;
                if (alt) lastFlip_ = !lastFlip_;
            }
        }
    }
    lastScanEnd_ = r.end;

    std::vector<float> prof = scanProfile(vis, W);
    const bool structure = stdev(prof) >= 4.0;

    // Scenery after a long run of empty (space) scans: the next picture begins here.
    if (structure && emptyRun_ >= 24 && structured_ >= 24) {
        closeObservation(QStringLiteral("end of picture (space)"));
        obs_ = std::make_shared<Observation>();
        obs_->stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
        obs_->firstSec = streamSec_;
        obs_->width = W;
        lastScanEnd_ = r.end;
    }
    Observation& ob = *obs_;

    // Direction and position. Consecutive scans run in opposite directions; the profile of this scan is matched
    // against the last one with structure, as it is and mirrored. Where the match is weak, the alternation decides.
    bool flip = false, confident = false;
    int shiftBins = 0;
    double score = 0.0;
    bool haveRef = !lastProfile_.empty() && scanNo_ - lastProfileNo_ <= 8;
    if (haveRef && structure) {
        bool f = false;
        int sb = 0;
        if (orient(lastProfile_, prof, 48, f, sb, score)) {
            if (score > 0.5) { confident = true; flip = f; shiftBins = sb; lowRun_ = 0; }
            else if (score < 0.15) ++lowRun_;
        }
    }
    if (confident) {
        if (scanNo_ - lastProfileNo_ == 1) altVote_ += ((flip != lastFlip_) ? 1.0 : -1.0) * score;
        altVote_ = std::clamp(altVote_, -20.0, 20.0);
    } else {
        flip = scanNo_ == 0 ? false : (alt ? !lastFlip_ : lastFlip_);
    }
    if (lowRun_ >= 4 && structured_ >= 8) {
        // the scenery stopped matching: another picture
        closeObservation(QStringLiteral("scene change"));
        obs_ = std::make_shared<Observation>();
        obs_->stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
        obs_->firstSec = streamSec_;
        obs_->width = W;
        lastScanEnd_ = r.end;
        flip = false;
        confident = false;
    }
    Observation& oc = *obs_;
    if (flip) {
        for (int d = 0; d < 8; ++d) std::reverse(vis.begin() + long(size_t(d) * size_t(W)), vis.begin() + long(size_t(d + 1) * size_t(W)));
        for (int g = 0; g < 4; ++g)
            for (int q = 0; q < 2; ++q)
                std::reverse(ir[size_t(g)].begin() + long(size_t(q) * size_t(n4)), ir[size_t(g)].begin() + long(size_t(q + 1) * size_t(n4)));
        ++flips_;
        ++oc.flips;
        prof = scanProfile(vis, W);
    }
    int fine = shiftBins * 16;
    std::vector<float> prof4;
    if (confident) {
        // refine the offset from 16 to 4 samples: profiles of 4-sample bins around the coarse position
        prof4 = scanProfile(vis, W, 4);
        if (!lastProfile4_.empty() && !prof4.empty()) {
            double bestScore = -2.0;
            for (int s4 = shiftBins * 4 - 5; s4 <= shiftBins * 4 + 5; ++s4) {
                const int a0 = std::max(0, s4), b0 = std::max(0, -s4);
                const int n = std::min(int(lastProfile4_.size()) - a0, int(prof4.size()) - b0);
                if (n < 200) continue;
                const double sc = correlation(lastProfile4_.data() + a0, prof4.data() + b0, n);
                if (sc > bestScore) { bestScore = sc; fine = s4 * 4; }
            }
        }
    }
    if (confident && fine) {
        const int shift = fine;
        for (int d = 0; d < 8; ++d) rollRow(vis.data() + size_t(d) * size_t(W), W, shift);
        for (int g = 0; g < 4; ++g)
            for (int q = 0; q < 2; ++q) rollRow(ir[size_t(g)].data() + size_t(q) * size_t(n4), n4, shift / 4);
        prof = scanProfile(vis, W);
        prof4.clear();
    }
    if (structure) {
        lastProfile_ = prof;
        lastProfile4_ = prof4.empty() ? scanProfile(vis, W, 4) : prof4;
        lastProfileNo_ = scanNo_;
        emptyRun_ = 0;
        ++structured_;
    } else {
        ++emptyRun_;
    }
    // the header flag says which way this scan really swept: with the flip applied it votes for the whole picture
    // being mirrored or not
    if (std::getenv("GOESSD_DEBUG")) std::fprintf(stderr, "scan %d flip=%d conf=%d score=%.2f shift=%d hdrSoft=%.0f structure=%d\n", scanNo_, int(flip), int(confident), score, fine, double(r.headerSoft), int(structure));
    oc.mirrorVote += flip ? -double(r.headerSoft) : double(r.headerSoft);
    lastFlip_ = flip;
    ++scanNo_;

    Scan sc;
    sc.width = W;
    sc.vis = std::move(vis);
    sc.ir = std::move(ir);
    // quick-look rows: 2 per scan (4 detector rows each), 8 samples per column
    {
        const int pw = W / 8;
        for (int q = 0; q < 2; ++q)
            for (int x = 0; x < pw; ++x) {
                unsigned sum = 0, cnt = 0;
                for (int d = 0; d < 4; ++d)
                    for (int k = 0; k < 8; ++k) {
                        const unsigned v = sc.vis[size_t(q * 4 + d) * size_t(W) + size_t(x * 8 + k)];
                        if (v) { sum += v; ++cnt; }
                    }
                oc.preview.push_back(cnt ? uint16_t(sum / cnt) : uint16_t(0));
            }
    }
    oc.scans.push_back(std::move(sc));
    oc.lastSec = streamSec_;
    ++scans_;
    pushLive(true);
    if (int(oc.scans.size()) >= kMaxScans) closeObservation(QStringLiteral("segment complete"));
}

void Engine::pushLive(bool force) {
    const int64_t now = QDateTime::currentMSecsSinceEpoch();
    if (!force && now - lastLiveMs_ < 900) return;
    if (!force && !obs_) return;
    lastLiveMs_ = now;
    QString message = QStringLiteral("GOES SD · imager %1 blocks · sync errors %2% · scans %3 · direction fixes %4 · sounder frames %5 (parity %6%)")
        .arg(qulonglong(blocks_.load())).arg(syncBer_.load(), 0, 'f', 1).arg(qulonglong(scans_.load())).arg(qulonglong(flips_.load()))
        .arg(qulonglong(sndFrames_.load())).arg(sndParity_.load(), 0, 'f', 2);
    QImage live;
    QString stamp;
    if (obs_ && !obs_->scans.empty()) {
        const Observation& o = *obs_;
        const int pw = o.width / 8, rows = int(o.preview.size() / size_t(std::max(pw, 1)));
        if (pw > 0 && rows > 0) {
            uint32_t hist[1024] = {};
            uint64_t total = 0;
            for (uint16_t v : o.preview) if (v) { ++hist[std::min<int>(v, 1023)]; ++total; }
            int lo = 0, hi = 1023;
            if (total > 200) {
                uint64_t acc = 0;
                for (int v = 1; v < 1024; ++v) { acc += hist[v]; if (acc >= total / 100) { lo = v; break; } }
                acc = 0;
                for (int v = 1; v < 1024; ++v) { acc += hist[v]; if (acc >= total - total / 200) { hi = v; break; } }
                if (hi < lo + 16) hi = std::min(1023, lo + 16);
            }
            live = QImage(pw, rows, QImage::Format_Grayscale8);
            for (int y = 0; y < rows; ++y) {
                uint8_t* d = live.scanLine(y);
                for (int x = 0; x < pw; ++x) {
                    const int v = o.preview[size_t(y) * size_t(pw) + size_t(o.mirrorVote < 0.0 ? pw - 1 - x : x)];
                    d[x] = v ? uint8_t(std::clamp((v - lo) * 254 / std::max(1, hi - lo) + 1, 1, 255)) : 0;
                }
            }
            stamp = o.stamp;
        }
    }
    std::lock_guard<std::mutex> l(uMutex_);
    live_.message = message;
    if (!live.isNull()) { live_.live = std::move(live); live_.stamp = stamp; }
}

// ---- sounder ------------------------------------------------------------------------------------

void Engine::sounderSymbols(const std::vector<std::complex<float>>& s) {
    for (const auto& z : s) {
        const float v = z.real();
        const bool change = (v > 0.f) != (sPrev_ > 0.f);
        sPrev_ = v;
        sBits_.push_back(change ? 1 : 0);
        ++sCount_;
        sReg_ = (sReg_ << 1) | (change ? 1u : 0u);
        if (sCount_ >= 32) {
            const uint64_t start = sCount_ - 32;
            const int dist = __builtin_popcount(sReg_ ^ kSounderMarker);
            const bool expected = sLastMarker_ >= 0 && int64_t(start) == sLastMarker_ + kSounderBits;
            if (dist == 0 || (expected && dist <= 3)) {
                if (sLastMarker_ < 0 || int64_t(start) > sLastMarker_ + 64) {
                    sLastMarker_ = int64_t(start);
                    sPending_.push_back(start);
                }
            }
        }
        while (!sPending_.empty() && sCount_ >= sPending_.front() + kSounderSkip + kSounderBits) {
            sounderRecord(sPending_.front());
            sPending_.pop_front();
        }
    }
    // drop bits that no pending record can need
    const uint64_t keepFrom = sPending_.empty() ? (sCount_ > 64 ? sCount_ - 64 : 0) : sPending_.front();
    if (keepFrom > sBase_ + 20000) {
        const size_t cut = size_t(keepFrom - sBase_);
        sBits_.erase(sBits_.begin(), sBits_.begin() + long(cut));
        sBase_ = keepFrom;
    }
}

void Engine::sounderRecord(uint64_t start) {
    const Tables& T = tables();
    const uint64_t first = start + kSounderSkip;
    if (first < sBase_) return;
    std::array<uint16_t, 264> words{};
    int okWords = 0, checked = 0;
    for (int wi = 0; wi < 264; ++wi) {
        uint16_t w = 0;
        int ones = 0;
        for (int b = 0; b < 16; ++b) {
            const int k = wi * 16 + b;
            const uint8_t bit = sBits_[size_t(first - sBase_) + size_t(k)] ^ T.sndMask[k];
            w = uint16_t((w << 1) | bit);
            ones += bit;
        }
        words[size_t(wi)] = w;
        if (wi >= 4 && wi <= 248) { ++checked; if (ones & 1) ++okWords; }
    }
    sndParityOk_ += uint64_t(okWords);
    sndParityAll_ += uint64_t(checked);
    ++sndFrames_;
    sndParity_ = 100.0 * double(sndParityOk_) / double(std::max<uint64_t>(1, sndParityAll_));
    sndLastSec_ = streamSec_;
    sndLocked_ = true;
    if (!obs_) {
        obs_ = std::make_shared<Observation>();
        obs_->stamp = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss"));
        obs_->firstSec = streamSec_;
    }
    obs_->sounder.push_back(words);
    obs_->sndParityOk += uint64_t(okWords);
    obs_->sndParityAll += uint64_t(checked);
}

// ---- observations -------------------------------------------------------------------------------

void Engine::closeObservation(const QString& why) {
    if (!obs_) return;
    std::shared_ptr<Observation> o = std::move(obs_);
    obs_.reset();
    lastProfile_.clear(); lastProfile4_.clear();
    lastProfileNo_ = -1000;
    lastScanEnd_ = -1;
    scanNo_ = 0; lastFlip_ = false; lowRun_ = emptyRun_ = structured_ = 0;
    o->syncBer = syncBer_.load();
    o->snr = snr_.load();
    if (int(o->scans.size()) < kMinScans && o->sounder.size() < 10) {
        nadirstation::log::write(QStringLiteral("GOES SD: observation %1 dropped (%2 scans, %3 sounder frames; %4)")
                                     .arg(o->stamp).arg(o->scans.size()).arg(o->sounder.size()).arg(why));
        return;
    }
    for (auto it = writes_.begin(); it != writes_.end();) {
        if (it->valid() && it->wait_for(std::chrono::seconds(0)) == std::future_status::ready) { it->get(); it = writes_.erase(it); }
        else ++it;
    }
    writes_.push_back(std::async(std::launch::async, [this, o] { writeObservation(o); }));
}

void Engine::writeObservation(std::shared_ptr<Observation> op) {
    Observation& o = *op;
    Update update;
    update.stamp = o.stamp;
    const QString dir = QDir(outputRoot_).filePath(QStringLiteral("GOES SD/") + o.stamp);
    if (!QDir().mkpath(dir)) {
        update.message = QStringLiteral("GOES SD: cannot create output folder %1").arg(dir);
        std::lock_guard<std::mutex> l(uMutex_);
        completed_.push_back(update);
        return;
    }
    QStringList paths;
    QJsonArray products;
    QJsonObject calib;
    const int S = int(o.scans.size()), W = o.width, n4 = W / 4;
    const int visRows = S * 8, irRows = S * 2;
    // square pixels: visible samples are 0.57 km apart along the scan against 1 km between detector rows, IR 2.28 km against 4 km
    std::vector<int> col;
    for (double x = 0; std::lround(x) < W; x += kAspect) col.push_back(int(std::lround(x)));
    std::vector<int> icol;
    for (double x = 0; std::lround(x) < n4; x += kAspect) icol.push_back(int(std::lround(x)));
    const bool mirrored = o.mirrorVote < 0.0;      // the header flags say the picture came out left-right reversed (polarity checked on two recordings against the illumination)
    if (mirrored) { std::reverse(col.begin(), col.end()); std::reverse(icol.begin(), icol.end()); }
    const int vw = int(col.size()), iw = int(icol.size());

    const auto save = [&](const QImage& img, const QString& file, const QString& name, double micron, const QString& note) {
        const QString path = QDir(dir).filePath(file);
        if (!img.save(path, "PNG")) return;
        paths << path;
        QJsonObject j;
        j[QStringLiteral("name")] = name;
        j[QStringLiteral("path")] = file;
        if (micron > 0) j[QStringLiteral("wavelength_um")] = micron;
        j[QStringLiteral("width")] = img.width();
        j[QStringLiteral("height")] = img.height();
        j[QStringLiteral("note")] = note;
        products.append(j);
    };

    if (S >= kMinScans && W > 0) {
        // ---- visible, square pixels, 16 bit (10-bit counts in the high bits), 0 = no data
        QImage vis16(vw, visRows, QImage::Format_Grayscale16);
        vis16.fill(0);
        std::vector<uint32_t> vh(1024, 0);
        uint64_t vtotal = 0;
        for (int s = 0; s < S; ++s)
            for (int d = 0; d < 8; ++d) {
                const uint16_t* src = o.scans[size_t(s)].vis.data() + size_t(d) * size_t(W);
                uint16_t* dst = reinterpret_cast<uint16_t*>(vis16.scanLine(s * 8 + d));
                for (int x = 0; x < vw; ++x) {
                    const uint16_t v = src[col[size_t(x)]];
                    dst[x] = uint16_t(v << 6);
                    if (v) { ++vh[std::min<int>(v, 1023)]; ++vtotal; }
                }
            }
        const auto percentile = [](const std::vector<uint32_t>& h, uint64_t total, double p) {
            uint64_t acc = 0;
            const uint64_t want = uint64_t(double(total) * p);
            for (int v = 1; v < 1024; ++v) { acc += h[size_t(v)]; if (acc >= want) return v; }
            return 1023;
        };
        save(vis16, QStringLiteral("GOES_SD_VIS.png"), QStringLiteral("Visible 0.65 um (counts)"), 0.65,
             QStringLiteral("raw 10-bit counts after the bit-plane vote, not calibrated; 0 = no data"));

        // ---- visible enhanced: percentile stretch with a gamma lift
        QImage visEnh(vw, visRows, QImage::Format_Grayscale8);
        if (vtotal > 1000) {
            const int lo = percentile(vh, vtotal, 0.01), hi = std::max(lo + 16, percentile(vh, vtotal, 0.997));
            uint8_t lut[1024];
            for (int v = 0; v < 1024; ++v) {
                if (v == 0) { lut[v] = 0; continue; }
                const double t = std::clamp(double(v - lo) / double(hi - lo), 0.0, 1.0);
                lut[v] = uint8_t(std::clamp(std::lround(std::pow(t, 0.8) * 254.0) + 1, 1l, 255l));
            }
            for (int y = 0; y < visRows; ++y) {
                const uint16_t* s = reinterpret_cast<const uint16_t*>(vis16.constScanLine(y));
                uint8_t* d = visEnh.scanLine(y);
                for (int x = 0; x < vw; ++x) d[x] = lut[s[x] >> 6];
            }
            save(visEnh, QStringLiteral("GOES_SD_VIS_Enhanced.png"), QStringLiteral("Visible enhanced"), 0.65,
                 QStringLiteral("1st to 99.7th percentile stretch, gamma 0.8"));
        }

        // ---- IR channels
        std::array<QImage, 4> ir16;
        std::array<std::vector<uint8_t>, 4> irCold8;   // coldness 0..255 per IR sample, for the false colour
        for (int g = 0; g < 3; ++g) {      // group 4 carries a constant (an unused channel): no image
            ir16[size_t(g)] = QImage(iw, irRows, QImage::Format_Grayscale16);
            ir16[size_t(g)].fill(0);
            std::vector<uint32_t> h(1024, 0);
            uint64_t total = 0;
            for (int s = 0; s < S; ++s)
                for (int r = 0; r < 2; ++r) {
                    const uint16_t* src = o.scans[size_t(s)].ir[size_t(g)].data() + size_t(r) * size_t(n4);
                    uint16_t* dst = reinterpret_cast<uint16_t*>(ir16[size_t(g)].scanLine(s * 2 + r));
                    for (int x = 0; x < iw; ++x) {
                        const uint16_t raw = src[icol[size_t(x)]];
                        if (!raw) { dst[x] = 0; continue; }
                        const int cold = kRawHighIsCold ? raw : 1023 - raw;
                        dst[x] = uint16_t(std::clamp(cold, 1, 1023) << 6);
                        ++h[size_t(std::clamp(cold, 1, 1023))];
                        ++total;
                    }
                }
            const IrChannel& ch = kIr[g];
            save(ir16[size_t(g)], QString::fromLatin1(ch.file), QString::fromLatin1(ch.label), ch.micron,
                 QStringLiteral("10-bit counts, colder = brighter, not calibrated; 4 km pixels (square); 0 = no data"));
            if (total > 500) {
                const int lo = percentile(h, total, 0.005), hi = std::max(lo + 16, percentile(h, total, 0.995));
                QImage enh(iw, irRows, QImage::Format_RGB888);
                for (int y = 0; y < irRows; ++y) {
                    const uint16_t* s = reinterpret_cast<const uint16_t*>(ir16[size_t(g)].constScanLine(y));
                    uchar* d = enh.scanLine(y);
                    for (int x = 0; x < iw; ++x) {
                        const int v = s[x] >> 6;
                        QRgb c = qRgb(0, 0, 0);
                        if (v) {
                            const double t = std::clamp(double(v - lo) / double(hi - lo), 0.0, 1.0);
                            switch (ch.kind) {
                            case Kind::Window: c = polar::rampCloudTop(t); break;
                            case Kind::WaterVapour: c = rampWaterVapour(t); break;
                            case Kind::Shortwave: c = rampShortwave(1.0 - t); break;
                            case Kind::Plain: { const int gy = int(t * 255.0 + 0.5); c = qRgb(gy, gy, gy); break; }
                            }
                        }
                        d[3 * x] = uchar(qRed(c)); d[3 * x + 1] = uchar(qGreen(c)); d[3 * x + 2] = uchar(qBlue(c));
                    }
                }
                save(enh, QStringLiteral("GOES_SD_IR%1_Enhanced.png").arg(g + 1), QString::fromLatin1(ch.label) + QStringLiteral(" enhanced"), ch.micron,
                     QStringLiteral("percentile stretch (0.5 to 99.5) and colour table; relative, not calibrated"));
                // ---- approximate calibration: brightness temperature (16 bit, 0.01 K, 0 = no data) and a fixed-scale colour picture
                {
                    // space count: the most common value 30..90 counts (stored samples 934..994); needs the off-disc ends of the scans
                    int best = 0, bestV = 0; uint64_t spaceN = 0;
                    for (int v = 934; v <= 994; ++v) { spaceN += h[size_t(v)]; if (int(h[size_t(v)]) > best) { best = int(h[size_t(v)]); bestV = v; } }
                    const bool measured = spaceN > 2000;
                    const double space = measured ? 1024.0 - double(bestV) : kIrCal[g].defaultSpace;
                    const auto bt = btTable(g, space);
                    QImage bt16(iw, irRows, QImage::Format_Grayscale16);
                    QImage btc(iw, irRows, QImage::Format_RGB888);
                    for (int y = 0; y < irRows; ++y) {
                        const uint16_t* sv = reinterpret_cast<const uint16_t*>(ir16[size_t(g)].constScanLine(y));
                        uint16_t* d16 = reinterpret_cast<uint16_t*>(bt16.scanLine(y));
                        uchar* dc = btc.scanLine(y);
                        for (int x = 0; x < iw; ++x) {
                            const int v = sv[x] >> 6;
                            const float k = v ? bt[size_t(v)] : 0.0f;
                            d16[x] = k > 0 ? uint16_t(std::lround(k * 100.0f)) : 0;
                            QRgb c = qRgb(0, 0, 0);
                            if (k > 0) {
                                if (g == 1) c = paletteCloudTopK(k);
                                else if (g == 2) c = paletteWaterVapourK(k);
                                else { const int gy = int(std::clamp((330.0 - k) / 110.0, 0.0, 1.0) * 255.0 + 0.5); c = qRgb(gy, gy, gy); }
                            }
                            dc[3 * x] = uchar(qRed(c)); dc[3 * x + 1] = uchar(qGreen(c)); dc[3 * x + 2] = uchar(qBlue(c));
                        }
                    }
                    const QString tag = g == 0 ? QStringLiteral("3p9") : g == 1 ? QStringLiteral("10p7") : QStringLiteral("6p5");
                    const QString how = QStringLiteral("approximate: SD counts mapped to GVAR counts (space count %1 %2, gain %3), NOAA GOES-15 tables")
                                            .arg(space, 0, 'f', 1).arg(measured ? QStringLiteral("measured") : QStringLiteral("default")).arg(kIrCal[g].gain, 0, 'f', 4);
                    save(bt16, QStringLiteral("GOES_SD_BT%1_K100.png").arg(tag), QStringLiteral("Brightness temperature %1 um (0.01 K per count)").arg(ch.micron), ch.micron, how);
                    save(btc, QStringLiteral("GOES_SD_BT%1_Color.png").arg(tag),
                         QStringLiteral("Brightness temperature %1 um, fixed colour scale (%2)").arg(ch.micron)
                             .arg(g == 1 ? QStringLiteral("175-330 K, colours below 240 K") : g == 2 ? QStringLiteral("195-290 K") : QStringLiteral("220-330 K, cold = white")),
                         ch.micron, how);
                    QJsonObject cj;
                    cj[QStringLiteral("space_count")] = space;
                    cj[QStringLiteral("space_count_measured")] = measured;
                    cj[QStringLiteral("gain_sd_to_gvar")] = kIrCal[g].gain;
                    cj[QStringLiteral("gvar_m")] = kIrCal[g].m;
                    cj[QStringLiteral("gvar_b")] = kIrCal[g].b;
                    cj[QStringLiteral("wavenumber")] = kIrCal[g].n;
                    cj[QStringLiteral("a")] = kIrCal[g].a;
                    cj[QStringLiteral("beta")] = kIrCal[g].beta;
                    calib[QStringLiteral("IR%1_%2um").arg(g + 1).arg(tag)] = cj;
                }
                if (g == kFalseColourIr) {
                    irCold8[size_t(g)].assign(size_t(irRows) * size_t(n4), 0);
                    for (int s = 0; s < S; ++s)
                        for (int r = 0; r < 2; ++r)
                            for (int x = 0; x < n4; ++x) {
                                const uint16_t raw = o.scans[size_t(s)].ir[size_t(g)][size_t(r) * size_t(n4) + size_t(x)];
                                if (!raw) continue;
                                const int cold = kRawHighIsCold ? raw : 1023 - raw;
                                const double t = std::clamp(double(cold - lo) / double(hi - lo), 0.0, 1.0);
                                irCold8[size_t(g)][size_t(s * 2 + r) * size_t(n4) + size_t(x)] = uint8_t(1 + std::lround(t * 253.0));   // row 255 of the table has black cells
                            }
                }
            }
        }

        // ---- false colour: visible x IR, SatDump's table, at the visible resolution (the IR samples repeated, no smoothing)
        if (!irCold8[kFalseColourIr].empty() && vtotal > 1000) {
            QImage fc(vw, visRows, QImage::Format_RGB888);
            const int lo = percentile(vh, vtotal, 0.01), hi = std::max(lo + 16, percentile(vh, vtotal, 0.997));
            for (int y = 0; y < visRows; ++y) {
                const int s = y / 8, d = y % 8;
                const int irRow = s * 2 + d / 4;
                const uint16_t* v16 = reinterpret_cast<const uint16_t*>(vis16.constScanLine(y));
                uchar* out = fc.scanLine(y);
                for (int x = 0; x < vw; ++x) {
                    const int v = v16[x] >> 6;
                    unsigned vv = 0, ir = 0;
                    if (v) vv = unsigned(std::clamp(std::lround(std::pow(std::clamp(double(v - lo) / double(hi - lo), 0.0, 1.0), 0.8) * 254.0), 0l, 254l));   // column 255 of the table is black
                    const int ix = std::min(n4 - 1, int(std::lround(col[size_t(x)] / 4.0 - 0.5)));
                    ir = irCold8[kFalseColourIr][size_t(irRow) * size_t(n4) + size_t(std::max(ix, 0))];
                    const unsigned char* c = &kFalseColorLut[(ir * 256u + vv) * 3u];
                    out[3 * x] = c[0]; out[3 * x + 1] = c[1]; out[3 * x + 2] = c[2];
                }
            }
            const QString fcPath = QDir(dir).filePath(QStringLiteral("GOES_SD_FalseColor.png"));
            if (fc.save(fcPath, "PNG")) {
                paths.prepend(fcPath);
                QJsonObject j;
                j[QStringLiteral("name")] = QStringLiteral("False colour (VIS x IR, SatDump table)");
                j[QStringLiteral("path")] = QStringLiteral("GOES_SD_FalseColor.png");
                j[QStringLiteral("width")] = fc.width();
                j[QStringLiteral("height")] = fc.height();
                products.append(j);
            }
        }
    }

    // ---- sounder
    QJsonObject snd;
    if (!o.sounder.empty()) {
        QByteArray bin;
        QByteArray csv = "frame,counter(word12>>1),parity_failures_words_4_to_248\n";
        int i = 0;
        for (const auto& f : o.sounder) {
            int bad = 0;
            for (int w = 4; w <= 248; ++w) {
                int ones = 0;
                for (int b = 0; b < 16; ++b) ones += (f[size_t(w)] >> b) & 1;
                if (!(ones & 1)) ++bad;
            }
            csv += QByteArray::number(i++) + "," + QByteArray::number(f[12] >> 1) + "," + QByteArray::number(bad) + "\n";
            for (uint16_t w : f) { bin.append(char(w >> 8)); bin.append(char(w & 255)); }
        }
        QFile fb(QDir(dir).filePath(QStringLiteral("GOES_SD_Sounder.bin")));
        if (fb.open(QIODevice::WriteOnly | QIODevice::Truncate)) fb.write(bin);
        QFile fc(QDir(dir).filePath(QStringLiteral("GOES_SD_Sounder.csv")));
        if (fc.open(QIODevice::WriteOnly | QIODevice::Truncate)) fc.write(csv);
        snd[QStringLiteral("frames")] = int(o.sounder.size());
        snd[QStringLiteral("frame_words")] = 264;
        snd[QStringLiteral("frame_bits")] = kSounderBits;
        snd[QStringLiteral("parity_ok_percent")] = 100.0 * double(o.sndParityOk) / double(std::max<uint64_t>(1, o.sndParityAll));
        snd[QStringLiteral("file")] = QStringLiteral("GOES_SD_Sounder.bin");
        snd[QStringLiteral("format")] = QStringLiteral("big-endian 16-bit words, 264 per frame, de-whitened; words 4..248 carry odd parity in the least significant bit");
    }

    QJsonObject root;
    root[QStringLiteral("satellite")] = QStringLiteral("GOES-N/O/P Sensor Data (1676 MHz)");
    root[QStringLiteral("observation_start")] = o.stamp;
    root[QStringLiteral("scans")] = S;
    root[QStringLiteral("blank_scans_inserted")] = qint64(o.blanks);
    root[QStringLiteral("direction_fixes")] = qint64(o.flips);
    root[QStringLiteral("mirrored_by_header_vote")] = mirrored;
    root[QStringLiteral("width_outliers_dropped")] = qint64(outliers_.load());
    root[QStringLiteral("width_changes")] = qint64(modes_.load());
    root[QStringLiteral("visible_samples_per_scan")] = W;
    root[QStringLiteral("sync_bit_error_percent")] = o.syncBer;
    root[QStringLiteral("snr_db_m2m4")] = o.snr;
    root[QStringLiteral("products")] = products;
    if (!calib.isEmpty()) {
        calib[QStringLiteral("note")] = QStringLiteral("unofficial, approximate (about 0.3 K on uniform scenes, 1-3 K at the extremes); "
                                                       "transferred from GVAR of the same satellite");
        root[QStringLiteral("calibration")] = calib;
    }
    root[QStringLiteral("sounder")] = snd;
    QFile jf(QDir(dir).filePath(QStringLiteral("GOES_SD_Observation.json")));
    if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) jf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));

    update.paths = paths;
    update.message = QStringLiteral("GOES SD · %1 · %2 scans · %3 sounder frames · %4 images saved").arg(o.stamp).arg(S).arg(o.sounder.size()).arg(paths.size());
    nadirstation::log::write(update.message);
    std::lock_guard<std::mutex> l(uMutex_);
    completed_.push_back(std::move(update));
}

}  // namespace nadirstation::goessd
