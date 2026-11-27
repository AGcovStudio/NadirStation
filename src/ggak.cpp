#include "ggak.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace nadirstation::ggak {
namespace {
constexpr double kPi = 3.141592653589793238462643383279502884;

std::uint16_t be16(const std::uint8_t* p) { return static_cast<std::uint16_t>((p[0] << 8) | p[1]); }
std::uint32_t be32(const std::uint8_t* p) {
    return (std::uint32_t(p[0]) << 24) | (std::uint32_t(p[1]) << 16) | (std::uint32_t(p[2]) << 8) | p[3];
}

void fftInPlace(std::vector<std::complex<float>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        for (std::size_t i = 0; i < n; i += len) {
            for (std::size_t j = 0; j < len / 2; ++j) {
                const std::complex<float> w(static_cast<float>(std::cos(ang * j)), static_cast<float>(std::sin(ang * j)));
                const auto u = a[i + j];
                const auto v = a[i + j + len / 2] * w;
                a[i + j] = u + v;
                a[i + j + len / 2] = u - v;
            }
        }
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Frames and instruments
// ---------------------------------------------------------------------------------------------

Frame parseFrame(const std::uint8_t* b) {
    Frame f;
    std::copy(b, b + kFrameBytes, f.bytes.begin());
    std::uint16_t sum = 0;
    for (int i = 0; i < 222; ++i) sum = static_cast<std::uint16_t>(sum + b[i]);
    f.checksumOk = sum == be16(b + 222);
    f.id = b[4];
    f.masterCounter = be16(b + 5);
    f.channelCounter = be16(b + 7);
    f.timeRaw = be32(b + 9);
    // Seconds since 2000-01-01 00:00 Moscow time (UTC+3): 10957 days after the Unix epoch, less 3 h.
    f.unixTime = static_cast<double>(f.timeRaw) + 10957.0 * 86400.0 - 3.0 * 3600.0;
    return f;
}

const char* frameIdName(int id) {
    switch (id) {
        case kIdSkl: return "SKL (charged-particle spectrometer)";
        case kIdSkif: return "SKIF (particle flux spectrometer)";
        case kIdFiller: return "filler";
        // N4: ID 28 byte 2 follows the independent GOES XRS-B flare curve (2026-10-31;
        // r=0.969 over 252 one-minute pairs). The other assignments below remain inferred.
        case 12: return "VUSS-E? solar UV sensor (assignment by elimination)";
        case 28: return "DIR-E solar X-ray sensor (temporal match; uncalibrated)";
        case 44: return "SKIF-6? particle spectrometer (0x2c, legacy SKIF 0x20)";
        case 60: return "SKL-E? particle spectrometer (0x3c, legacy SKL 0x30)";
        case 64: return "GALS-E? galactic cosmic-ray detector (assignment inferred)";
        case 80: return "FM-E? magnetometer (assignment inferred)";
        case 112: return "ISP-2M? solar irradiance / housekeeping (assignment inferred)";
        default: return "unidentified instrument (raw)";
    }
}

const char* sklChannelName(int i) {
    static const char* names[kSklChannels] = {
        "Electrons > 0.15 MeV, protons > 1.5 MeV",
        "Electrons > 3 MeV, protons > 7 MeV",
        "Electrons 3-6 MeV",
        "Electrons 6-9 MeV",
        "Electrons 8-15 MeV",
        "Protons 9-20 MeV",
        "Protons 20-40 MeV",
        "Protons 40-110 MeV",
        "Protons 110-300 MeV",
        "Electrons > 0.6 MeV, protons 1.6-200 MeV",
        "Electrons > 1.3 MeV, protons 1.6-200 MeV (not transmitted)",
        "Protons > 100 MeV",
    };
    return (i >= 0 && i < kSklChannels) ? names[i] : "?";
}

std::vector<SklRecord> parseSkl(const Frame& frame) {
    std::vector<SklRecord> out;
    if (frame.id != kIdSkl) return out;
    const std::uint8_t* payload = frame.bytes.data() + 13;
    for (int r = 0; r < 14; ++r) {
        const std::uint8_t* p = payload + r * 14;
        SklRecord rec;
        // One record a second, the first at the frame time (as SKIF's thirteen records are). Giving
        // all fourteen records the frame time stacked fourteen values on one instant in every plot
        // and CSV. Not yet checked against a real SKL frame: ELEKTRO-L N4 does not send ID 48.
        rec.unixTime = frame.unixTime + r;
        for (int c = 0; c < 11; ++c) rec.raw[static_cast<std::size_t>(c)] = p[3 + c];
        for (int c = 0; c < 11; ++c) {
            const double raw = p[3 + c];
            double v;
            if (c == 0 || c == 9) v = std::exp((raw - 67.0) / 29.02) * 100.0 * 10.0;
            else v = raw * 10.0;
            if (c == 10) rec.value[11] = v;
            else rec.value[static_cast<std::size_t>(c)] = v;
        }
        rec.value[10] = std::numeric_limits<double>::quiet_NaN();
        out.push_back(rec);
    }
    return out;
}

namespace {
struct SkifLut { int recordId, position; SkifChannel channel; double scale; };
// (record id, position) -> channel; reverse engineered by others, see THIRD_PARTY_NOTICES.md.
const std::vector<SkifLut>& skifLut() {
    static const std::vector<SkifLut> lut = [] {
        std::vector<SkifLut> v;
        auto add = [&](SkifChannel ch, std::initializer_list<std::array<double, 3>> items) {
            for (const auto& it : items) v.push_back({static_cast<int>(it[0]), static_cast<int>(it[1]), ch, it[2]});
        };
        const int ids20[] = {10, 133, 137, 2, 6, 130, 134, 138, 3, 7, 131, 135, 1, 4, 8, 132, 136, 193, 5, 9};
        for (int pos : {3, 4})
            for (int id : ids20) v.push_back({id, pos, pos == 3 ? Skif1 : Skif2, 1.0});
        add(Skif3, {{10,5,1},{133,5,1},{137,5,1},{2,5,1},{6,5,1},{130,5,1},{134,5,1},{138,5,1},{3,5,1},{7,5,1},
                    {130,9,.5},{134,9,.5},{138,9,.5},{3,9,.5},{7,9,.5},{131,5,1},{135,5,1},{1,5,1},{4,5,1},{8,5,1},
                    {132,5,1},{136,5,1},{193,5,1},{5,5,1},{9,5,1},{132,9,.5},{136,9,.5},{1,9,.5},{5,9,.5},{9,9,.5}});
        add(SkifE1, {{2,10,1},{4,10,1},{6,10,1},{8,10,1},{10,10,1},{131,10,1},{133,10,1},{135,10,1},{137,10,1},{193,10,1}});
        add(SkifE2, {{1,1,1},{3,1,1},{130,1,1},{131,1,1},{133,1,1}});
        add(SkifE3, {{2,1,1},{132,1,1},{193,1,1}});
        add(SkifE4, {{1,6,1},{3,6,1},{5,6,1},{7,6,1},{9,6,1},{130,6,1},{132,6,1},{134,6,1},{136,6,1},{138,6,1}});
        add(SkifE5, {{2,6,1},{4,6,1},{6,6,1},{8,6,1},{10,6,1},{131,6,1},{133,6,1},{135,6,1},{137,6,1},{193,6,1}});
        add(SkifE6, {{5,1,1},{135,1,1}});
        add(SkifE7, {{6,1,1},{136,1,1}});
        add(SkifE8, {{7,1,1},{137,1,1}});
        add(SkifE9, {{8,1,1},{138,1,1}});
        add(SkifE10, {{9,1,1},{10,1,1}});
        add(SkifP1, {{3,10,1},{5,10,1},{7,10,1},{9,10,1},{130,10,1},{132,10,1},{134,10,1},{136,10,1},{138,10,1}});
        add(SkifP2, {{4,1,1},{134,1,1}});
        add(SkifP3, {{2,9,1},{4,9,1},{6,9,1},{8,9,1},{10,9,1},{131,9,1},{133,9,1},{135,9,1},{137,9,1},{193,9,1}});
        add(SkifP4, {{1,8,1},{3,8,1},{5,8,1},{7,8,1},{9,8,1},{130,8,1},{132,8,1},{134,8,1},{136,8,1},{138,8,1}});
        add(SkifP5, {{10,8,1},{133,8,1},{137,8,1},{2,8,1},{6,8,1},{131,8,1},{135,8,1},{193,8,1},{4,8,1},{8,8,1}});
        add(SkifP6, {{10,7,.5},{133,7,.5},{137,7,.5},{2,7,.5},{6,7,.5},{130,7,1},{134,7,1},{138,7,1},{3,7,1},{7,7,1},
                     {131,7,.5},{135,7,.5},{1,7,1},{4,7,.5},{8,7,.5},{132,7,1},{136,7,1},{193,7,.5},{5,7,1},{9,7,1}});
        add(SkifP7, {{1,2,1},{132,2,1},{135,2,1},{193,2,1},{4,2,1},{130,2,1},{133,2,1},{136,2,1},{2,2,1},{5,2,1},
                     {131,2,1},{134,2,1},{137,2,1},{3,2,1},{6,2,1}});
        add(SkifP8, {{7,2,1}});
        add(SkifP9, {{8,2,1},{138,2,1}});
        add(SkifP10, {{9,2,1},{10,2,1}});
        return v;
    }();
    return lut;
}
}  // namespace

const char* skifChannelName(int c) {
    static const char* names[] = {
        "SKIF 1", "SKIF 2", "SKIF 3",
        "Electrons E1", "Electrons E2", "Electrons E3", "Electrons E4", "Electrons E5",
        "Electrons E6", "Electrons E7", "Electrons E8", "Electrons E9", "Electrons E10",
        "Protons P1", "Protons P2", "Protons P3", "Protons P4", "Protons P5",
        "Protons P6", "Protons P7", "Protons P8", "Protons P9", "Protons P10",
        "unmapped",
    };
    return (c >= 0 && c <= SkifUnknown) ? names[c] : "?";
}

bool skifScalingUncertain(int c) {
    // Linear scalings the reference itself marks as guesses.
    return (c >= SkifE1 && c <= SkifE10) || (c >= SkifP1 && c <= SkifP4) || c == SkifUnknown;
}

std::vector<SkifRecord> parseSkif(const Frame& frame) {
    std::vector<SkifRecord> out;
    if (frame.id != kIdSkif) return out;
    static const double linear[] = {17710, 17710, 12710, 3710, 3710, 2710, 1310, 910, 610, 220};   // E1..E10
    static const double linearP[] = {187710, 187710, 107710, 107710};                               // P1..P4
    const std::uint8_t* payload = frame.bytes.data() + 13;
    for (int r = 0; r < 13; ++r) {
        const std::uint8_t* p = payload + r * 15;
        const int recordId = p[3];
        for (int i = 0; i < 10; ++i) {
            SkifRecord rec;
            rec.unixTime = frame.unixTime + r;
            rec.recordId = recordId;
            rec.position = i + 1;
            rec.raw = p[5 + i];
            double scale = 1.0;
            for (const auto& l : skifLut())
                if (l.recordId == recordId && l.position == i + 1) { rec.channel = l.channel; scale = l.scale; break; }
            const double x = rec.raw;
            switch (rec.channel) {
                case Skif1: case Skif2: case Skif3: rec.value = std::exp((x + 230.1144214) / 28.68) * scale; break;
                case SkifP5: rec.value = std::exp((x + 180) / 14.5); break;
                case SkifP6: rec.value = std::exp((x + 180) / 17.5) * scale; break;
                case SkifP7: rec.value = std::exp((x + 200) / 22); break;
                case SkifP8: rec.value = std::exp((x + 215) / 25); break;
                case SkifP9: rec.value = std::exp((x + 200) / 25); break;
                case SkifP10: rec.value = std::exp((x + 160) / 23); break;
                case SkifUnknown: rec.value = std::exp((x + 230.1144214) / 28.68); break;
                default:
                    if (rec.channel >= SkifE1 && rec.channel <= SkifE10) rec.value = x * linear[rec.channel - SkifE1];
                    else rec.value = x * linearP[rec.channel - SkifP1];
                    break;
            }
            out.push_back(rec);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Deframer
// ---------------------------------------------------------------------------------------------

namespace {
constexpr int kFrameBits = kFrameBytes * 8;
constexpr int kSearchErrors = 2;    // candidate marker
constexpr int kConfirmErrors = 6;   // marker one frame after a candidate
constexpr int kLockedErrors = 8;    // marker while locked
constexpr int kMaxMisses = 3;
}  // namespace

void Deframer::reset() { *this = Deframer{}; }

int Deframer::markerErrors(const std::vector<std::uint8_t>& b, std::size_t at, bool inverted) {
    int errors = 0;
    for (int i = 0; i < 32; ++i) {
        const std::uint8_t want = static_cast<std::uint8_t>(((kAsm >> (31 - i)) & 1u) ^ (inverted ? 1u : 0u));
        errors += (b[at + static_cast<std::size_t>(i)] != want) ? 1 : 0;
    }
    return errors;
}

void Deframer::cut(std::size_t at, std::array<std::uint8_t, kFrameBytes>& frame) const {
    for (int byte = 0; byte < kFrameBytes; ++byte) {
        std::uint8_t v = 0;
        for (int i = 0; i < 8; ++i)
            v = static_cast<std::uint8_t>((v << 1) | (bits_[at + static_cast<std::size_t>(byte * 8 + i)] ^ (inverted_ ? 1u : 0u)));
        frame[static_cast<std::size_t>(byte)] = v;
    }
    // The marker is written clean, so a frame that was cut with a few wrong marker bits still
    // reads as a frame; the checksum decides whether its contents are good.
    frame[0] = static_cast<std::uint8_t>(kAsm >> 24);
    frame[1] = static_cast<std::uint8_t>(kAsm >> 16);
    frame[2] = static_cast<std::uint8_t>(kAsm >> 8);
    frame[3] = static_cast<std::uint8_t>(kAsm);
}

void Deframer::push(const std::vector<std::uint8_t>& bits, std::vector<std::array<std::uint8_t, kFrameBytes>>& out) {
    for (std::uint8_t b : bits) bits_.push_back(b & 1u);
    const std::size_t need = static_cast<std::size_t>(kFrameBits + 32);   // a frame and the next marker
    std::array<std::uint8_t, kFrameBytes> frame{};
    while (pos_ + need <= bits_.size()) {
        if (state_ == 2) {
            const int errors = markerErrors(bits_, pos_, inverted_);
            cut(pos_, frame);
            if (errors <= kLockedErrors) {
                misses_ = 0;
                out.push_back(frame);
            } else if (++misses_ >= kMaxMisses) {
                state_ = 0;
                misses_ = 0;
                ++pos_;
                continue;
            } else {
                out.push_back(frame);   // flywheel: the checksum still judges it
            }
            pos_ += static_cast<std::size_t>(kFrameBits);
            continue;
        }
        // Searching: a marker of either polarity.
        bool found = false;
        for (int pol = 0; pol < 2 && !found; ++pol) {
            const bool inv = pol == 1;
            if (markerErrors(bits_, pos_, inv) > kSearchErrors) continue;
            const bool nextOk = markerErrors(bits_, pos_ + static_cast<std::size_t>(kFrameBits), inv) <= kConfirmErrors;
            const bool saved = inverted_;
            inverted_ = inv;
            cut(pos_, frame);
            const bool sumOk = parseFrame(frame.data()).checksumOk;
            if (nextOk || sumOk) {
                found = true;
                out.push_back(frame);
                state_ = nextOk ? 2 : 1;
                misses_ = 0;
                pos_ += static_cast<std::size_t>(kFrameBits);
                if (state_ == 1) state_ = 2;   // checksum-confirmed: treat as locked, misses will tell
            } else {
                inverted_ = saved;
            }
        }
        if (!found) ++pos_;
    }
    // Keep only what is still needed.
    if (pos_ > 65536) {
        bits_.erase(bits_.begin(), bits_.begin() + static_cast<std::ptrdiff_t>(pos_));
        pos_ = 0;
    }
}

// ---------------------------------------------------------------------------------------------
// Demodulator
// ---------------------------------------------------------------------------------------------

namespace {
// Carrier search FFT: about 150 Hz per bin whatever the input rate (16384 points at 2.5 MS/s, as
// before), within 1024..16384 points. A fixed 16384 meant 6.5 s of samples before the search at
// 60 kS/s (a narrow baseband), which lost the first third of a short recording.
constexpr std::size_t kCoarseMinN = 1024, kCoarseMaxN = 16384;
constexpr double kCoarseBinHz = 150.0;
constexpr int kCoarseBlocks = 24;
constexpr std::size_t kFineN = 16384;
constexpr double kSearchSpanHz = 150000.0;
constexpr double kFineSpanHz = 400.0;
constexpr double kSymLoHz = 1000.0, kSymHiHz = 9000.0;
constexpr double kMinSymmetryDb = 1.5;
constexpr double kLoopBandwidthHz = 60.0;

// Spectrum in dB above its median, after an fftshift-free mapping: index i is frequency
// (i < n/2 ? i : i - n) * hz.
std::vector<float> excessDb(const std::vector<float>& power) {
    std::vector<float> sorted(power);
    std::nth_element(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(sorted.size() / 2), sorted.end());
    const float median = std::max(sorted[sorted.size() / 2], 1e-30f);
    std::vector<float> out(power.size());
    for (std::size_t i = 0; i < power.size(); ++i)
        out[i] = std::max(0.0f, 10.0f * std::log10(std::max(power[i], 1e-30f) / median));
    return out;
}

// Mean over d of min(L[c+d], L[c-d]) for d in [d1, d2] bins; c is a signed bin offset.
double symmetryScore(const std::vector<float>& l, long c, long d1, long d2) {
    const long n = static_cast<long>(l.size());
    auto at = [&](long k) { k %= n; if (k < 0) k += n; return l[static_cast<std::size_t>(k)]; };
    double sum = 0.0;
    for (long d = d1; d <= d2; ++d) sum += std::min(at(c + d), at(c - d));
    return sum / static_cast<double>(d2 - d1 + 1);
}

// Best symmetry centre within +-span; returns (offset Hz, score dB).
std::pair<double, double> findCentre(const std::vector<float>& l, double hz, double span, bool skipDc) {
    const long n = static_cast<long>(l.size());
    const long d1 = std::max(1L, static_cast<long>(std::ceil(kSymLoHz / hz)));
    const long d2 = std::max(d1, std::min(n / 4, static_cast<long>(std::floor(kSymHiHz / hz))));
    const long cmax = std::min(n / 2 - d2 - 1, static_cast<long>(span / hz));
    double best = -1.0;
    long bestC = 0;
    for (long c = -cmax; c <= cmax; ++c) {
        if (skipDc && std::labs(c) <= 2) continue;
        const double s = symmetryScore(l, c, d1, d2);
        if (s > best) { best = s; bestC = c; }
    }
    return {static_cast<double>(bestC) * hz, best};
}

void accumulateSpectrum(const std::complex<float>* x, std::size_t n, std::vector<float>& acc) {
    std::vector<std::complex<float>> a(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double w = 0.5 - 0.5 * std::cos(2 * kPi * static_cast<double>(i) / static_cast<double>(n - 1));
        a[i] = x[i] * static_cast<float>(w);
    }
    fftInPlace(a);
    for (std::size_t i = 0; i < n; ++i) acc[i] += std::norm(a[i]);
}
}  // namespace

void Demodulator::configure(double sampleRate) {
    if (configured_ && std::fabs(sampleRate - declaredRate_) < 0.5) return;
    *this = Demodulator{};
    declaredRate_ = sampleRate;
    configureAt(sampleRate);
}

void Demodulator::configureAt(double sampleRate) {
    inRate_ = sampleRate;
    stats_.rateScale = declaredRate_ > 0.0 ? sampleRate / declaredRate_ : 1.0;
    decim_ = std::max(1, static_cast<int>(std::lround(sampleRate / (kSymbolRate * 10.0))));
    rate_ = inRate_ / decim_;
    sps_ = rate_ / kSymbolRate;
    coarseN_ = kCoarseMinN;
    while (coarseN_ < kCoarseMaxN && static_cast<double>(coarseN_) * kCoarseBinHz < inRate_) coarseN_ *= 2;
    accum_.clear();

    // Decimation low-pass: keeps +-15 kHz around the carrier (both data lobes and the first odd
    // subcarrier harmonics), Blackman-windowed sinc.
    const int taps = std::min(1023, 8 * decim_ + 1) | 1;
    lpf_.assign(static_cast<std::size_t>(taps), 0.0f);
    const double fc = std::min(0.45, 15000.0 / inRate_);
    double sum = 0.0;
    for (int i = 0; i < taps; ++i) {
        const double m = i - taps / 2;
        const double sn = m == 0 ? 2 * fc : std::sin(2 * kPi * fc * m) / (kPi * m);
        const double w = taps > 1 ? 0.42 - 0.5 * std::cos(2 * kPi * i / (taps - 1)) + 0.08 * std::cos(4 * kPi * i / (taps - 1)) : 1.0;
        lpf_[static_cast<std::size_t>(i)] = static_cast<float>(sn * w);
        sum += sn * w;
    }
    for (auto& v : lpf_) v = static_cast<float>(v / sum);
    hist_.assign(lpf_.size(), {0, 0});

    // Second-order carrier loop, damping 0.707; the phase detector gain is 1 for unit power.
    const double zeta = 0.70710678;
    const double theta = (kLoopBandwidthHz / rate_) / (zeta + 1.0 / (4.0 * zeta));
    const double d = 1.0 + 2.0 * zeta * theta + theta * theta;
    kp_ = 4.0 * zeta * theta / d;
    ki_ = 4.0 * theta * theta / d;

    stats_.symbolsPerSecond = kSymbolRate;
    configured_ = true;
}

void Demodulator::resetAcquisition() {
    accum_.clear();
    accumBlocks_ = 0;
    pending_.clear();
    fineBuf_.clear();
    stats_.tracking = false;
}

void Demodulator::setMixer(double offsetHz) {
    mixOffsetHz_ = offsetHz;
    const double w = -2.0 * kPi * offsetHz / inRate_;
    loStep_ = {std::cos(w), std::sin(w)};
}

void Demodulator::coarseSearch(const std::vector<std::complex<float>>& iq) {
    if (accum_.size() != coarseN_) accum_.assign(coarseN_, 0.0f);
    pending_.insert(pending_.end(), iq.begin(), iq.end());
    std::size_t used = 0;
    while (accumBlocks_ < kCoarseBlocks && pending_.size() - used >= coarseN_) {   // stop at what the search needs
        accumulateSpectrum(pending_.data() + used, coarseN_, accum_);
        used += coarseN_;
        ++accumBlocks_;
    }
    pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(used));
    if (accumBlocks_ < kCoarseBlocks) return;

    const double hz = inRate_ / static_cast<double>(coarseN_);
    const std::vector<float> l = excessDb(accum_);
    const auto [offset, score] = findCentre(l, hz, std::min(kSearchSpanHz, inRate_ * 0.4), true);
    // A recording whose header states the wrong rate (typically twice or half the real one) puts
    // the data lobes at +-10 or +-2.5 kHz instead of +-5 kHz, and nothing downstream can lock: the
    // carrier is still found (the spectrum is symmetric at any scale), at twice or half its real
    // offset. So the first time round, the lobe power at 2.5, 5 and 10 kHz from the centre is
    // compared; a lobe 3 dB stronger away from 5 kHz means the declared rate is wrong, and the
    // demodulator restarts at the rate the signal fits. Measured margins on the 10-18 captures:
    // 4-10 dB in favour of the right answer at the right rate, and the same with a wrong header.
    if (!rateChecked_) {
        rateChecked_ = true;
        // Average excess around the carrier +- dist, over a window that scales with dist.
        auto lobe = [&](double dist) {
            const long n = static_cast<long>(coarseN_);
            const long c0 = std::lround(offset / hz);
            const long d = std::lround(dist / hz), w = std::max(1L, std::lround(0.25 * dist / hz));
            double sum = 0.0; int cnt = 0;
            for (int sgn : {-1, 1})
                for (long k = -w; k <= w; ++k) {
                    long idx = (c0 + sgn * d + k) % n;
                    if (idx < 0) idx += n;
                    sum += l[static_cast<std::size_t>(idx)]; ++cnt;
                }
            return cnt ? sum / cnt : 0.0;
        };
        const double p5 = lobe(5000.0), p10 = lobe(10000.0), p25 = lobe(2500.0);
        double scale = 1.0;
        if (p10 > p5 + 3.0 && p10 >= p25) scale = 0.5;        // lobes at 10 kHz: true rate is half
        else if (p25 > p5 + 3.0 && p25 > p10) scale = 2.0;    // lobes at 2.5 kHz: true rate is twice
        if (scale != 1.0 && score >= kMinSymmetryDb) {
            const double declared = declaredRate_;
            *this = Demodulator{};
            declaredRate_ = declared;
            rateChecked_ = true;
            configureAt(declared * scale);
            return;
        }
    }
    // Signal level: the highest excess within +-10 kHz of the centre.
    float peak = 0.0f;
    const long n = static_cast<long>(coarseN_);
    const long c = std::lround(offset / hz);
    for (long k = -static_cast<long>(10000.0 / hz); k <= static_cast<long>(10000.0 / hz); ++k) {
        long idx = (c + k) % n;
        if (idx < 0) idx += n;
        peak = std::max(peak, l[static_cast<std::size_t>(idx)]);
    }
    stats_.carrierToNoiseDb = peak;
    accum_.assign(coarseN_, 0.0f);
    accumBlocks_ = 0;
    // Samples after those the search used go on to the next stage instead of being dropped; with
    // one-second input blocks that used to cost the rest of the block (several frames).
    leftover_.swap(pending_);
    pending_.clear();
    if (score < kMinSymmetryDb) {
        leftover_.clear();
        stats_.carrierFound = false;
        return;
    }
    stats_.carrierFound = true;
    stats_.coarseOffsetHz = offset;
    setMixer(offset);
    fineBuf_.clear();
    mode_ = Mode::Fine;
}

void Demodulator::decimate(const std::vector<std::complex<float>>& iq, std::vector<std::complex<float>>& out) {
    const std::size_t n = lpf_.size();
    out.reserve(out.size() + iq.size() / static_cast<std::size_t>(decim_) + 2);
    for (const auto& x : iq) {
        hist_[histPos_] = x * std::complex<float>(static_cast<float>(lo_.real()), static_cast<float>(lo_.imag()));
        lo_ *= loStep_;
        if (++loRenorm_ >= 4096) { lo_ /= std::abs(lo_); loRenorm_ = 0; }
        histPos_ = (histPos_ + 1) % n;
        if (++decimPhase_ < decim_) continue;
        decimPhase_ = 0;
        std::complex<float> acc{0, 0};
        std::size_t idx = histPos_;
        for (std::size_t k = 0; k < n; ++k) {
            acc += hist_[idx] * lpf_[k];
            if (++idx == n) idx = 0;
        }
        out.push_back(acc);
    }
}

void Demodulator::fineSearch(const std::vector<std::complex<float>>& base) {
    fineBuf_.insert(fineBuf_.end(), base.begin(), base.end());
    if (fineBuf_.size() < kFineN) return;
    // The residual carrier is a line in the spectrum of z with amplitude cos(beta); squaring
    // removes the +-1 data and leaves a line at twice the offset with amplitude |cos(2 beta)|.
    // One of the two is strong for any modulation index, so the clearer one sets the frequency.
    const std::complex<float>* src = fineBuf_.data() + (fineBuf_.size() - kFineN);
    std::vector<std::complex<float>> sq(kFineN);
    float p = 0.0f;
    for (std::size_t i = 0; i < kFineN; ++i) p += std::norm(src[i]);
    p = std::max(p / static_cast<float>(kFineN), 1e-30f);
    for (std::size_t i = 0; i < kFineN; ++i) sq[i] = src[i] * src[i] / p;
    std::vector<float> acc1(kFineN, 0.0f), acc2(kFineN, 0.0f);
    accumulateSpectrum(src, kFineN, acc1);
    accumulateSpectrum(sq.data(), kFineN, acc2);
    heldFine_.swap(fineBuf_);   // demodulated once the offset is known, instead of being dropped
    fineBuf_.clear();
    const double hz = rate_ / static_cast<double>(kFineN);
    auto line = [&](const std::vector<float>& acc, double span, double& atHz) {
        const std::vector<float> l = excessDb(acc);
        const long n = static_cast<long>(kFineN);
        const long m = static_cast<long>(span / hz);
        float best = -1.0f;
        long bestK = 0;
        for (long k = -m; k <= m; ++k) {
            const float v = l[static_cast<std::size_t>((k + n) % n)];
            if (v > best) { best = v; bestK = k; }
        }
        // Parabolic interpolation on the linear powers around the peak.
        const double a = acc[static_cast<std::size_t>((bestK - 1 + n) % n)];
        const double b = acc[static_cast<std::size_t>((bestK + n) % n)];
        const double c = acc[static_cast<std::size_t>((bestK + 1 + n) % n)];
        const double d = a - 2.0 * b + c;
        const double frac = std::fabs(d) > 1e-30 ? std::clamp(0.5 * (a - c) / d, -0.5, 0.5) : 0.0;
        atHz = (static_cast<double>(bestK) + frac) * hz;
        return best;
    };
    double f1 = 0.0, f2 = 0.0;
    const float s1 = line(acc1, kFineSpanHz, f1);
    const float s2 = line(acc2, 2.0 * kFineSpanHz, f2);
    const double offset = s2 > s1 ? f2 / 2.0 : f1;
    const double score = std::max(s1, s2);
    if (score < 10.0) {   // no clear line: start again
        heldFine_.clear();
        mode_ = Mode::Coarse;
        resetAcquisition();
        return;
    }
    stats_.coarseOffsetHz = mixOffsetHz_ + offset;
    setMixer(stats_.coarseOffsetHz);
    phase_ = freq_ = 0.0;
    cAmp_ = sAmp_ = 0.5;
    power_ = 1.0f;
    csum_.assign(1, 0.0);
    csumRe_.assign(1, 0.0);
    period_ = sps_;
    tau_ = period_;   // leaves room for the early gate
    absAvg_ = 0.0;
    onAcc_ = offAcc_ = 0.0;
    ambCount_ = 0;
    m2_ = m4_ = 0.0;
    moments_ = 0;
    stats_.tracking = true;
    mode_ = Mode::Track;
    // The held samples were mixed with the coarse offset; remove the fine part too, ending at
    // phase zero where the new mixer setting takes over.
    const double w = -2.0 * kPi * offset / rate_;
    const std::size_t n = heldFine_.size();
    for (std::size_t i = 0; i < n; ++i) {
        const double ph = w * (static_cast<double>(i) - static_cast<double>(n));
        heldFine_[i] *= std::complex<float>(static_cast<float>(std::cos(ph)), static_cast<float>(std::sin(ph)));
    }
}

void Demodulator::track(const std::vector<std::complex<float>>& base, std::vector<float>& soft) {
    const double sumAlpha = 1.0 / (0.1 * rate_);   // carrier/data amplitude averages, 0.1 s
    for (auto z : base) {
        power_ += 0.002f * (std::norm(z) - power_);
        z /= std::sqrt(std::max(power_, 1e-20f));
        const std::complex<float> y = z * std::complex<float>(static_cast<float>(std::cos(phase_)), static_cast<float>(-std::sin(phase_)));
        const double m = y.imag() >= 0.0f ? 1.0 : -1.0;
        // e = Im(y * conj(c + j s m)) = Im(y) c - Re(y) s m
        const double e = static_cast<double>(y.imag()) * cAmp_ - static_cast<double>(y.real()) * sAmp_ * m;
        cAmp_ += sumAlpha * (static_cast<double>(y.real()) - cAmp_);
        sAmp_ += sumAlpha * (std::fabs(static_cast<double>(y.imag())) - sAmp_);
        freq_ += ki_ * e;
        freq_ = std::clamp(freq_, -2 * kPi * 200.0 / rate_, 2 * kPi * 200.0 / rate_);
        phase_ += freq_ + kp_ * e;
        if (phase_ > kPi) phase_ -= 2 * kPi;
        if (phase_ < -kPi) phase_ += 2 * kPi;
        csum_.push_back(csum_.back() + static_cast<double>(y.imag()));
        csumRe_.push_back(csumRe_.back() + static_cast<double>(y.real()) - cAmp_);
    }
    stats_.pllOffsetHz = freq_ * rate_ / (2 * kPi);
    stats_.modulationIndexDeg = std::atan2(std::max(0.0, sAmp_), cAmp_) * 180.0 / kPi;

    auto at = [](const std::vector<double>& c, double x) {
        const std::size_t i = static_cast<std::size_t>(x);
        const double f = x - static_cast<double>(i);
        return c[i] + f * (c[i + 1] - c[i]);
    };
    // Bi-phase integral of the symbol starting at x: first half minus second half.
    auto biphase = [&](const std::vector<double>& c, double x) {
        return 2.0 * at(c, x + period_ / 2.0) - at(c, x) - at(c, x + period_);
    };
    const double gate = period_ / 4.0;
    while (tau_ - gate >= 0.0 && tau_ + 1.5 * period_ + gate + 2.0 < static_cast<double>(csum_.size())) {
        const double r = biphase(csum_, tau_);
        const double early = std::fabs(biphase(csum_, tau_ - gate));
        const double late = std::fabs(biphase(csum_, tau_ + gate));
        const double other = std::fabs(biphase(csum_, tau_ + period_ / 2.0));
        if (absAvg_ <= 0.0) absAvg_ = std::max(1e-9, std::fabs(r));
        absAvg_ += 0.01 * (std::fabs(r) - absAvg_);
        const double e = std::clamp((late - early) / absAvg_, -1.0, 1.0);

        const double v = r / absAvg_;
        soft.push_back(static_cast<float>(v));
        const double q = biphase(csumRe_, tau_) / absAvg_;
        lastSymbols_.push_back({static_cast<float>(v), static_cast<float>(q)});

        // Moments for Eb/N0, about the last 2000 symbols.
        const double a = 1.0 / 2000.0;
        m2_ += a * (v * v - m2_);
        m4_ += a * (v * v * v * v - m4_);
        ++moments_;
        if (moments_ > 200) {
            const double s2 = std::sqrt(std::max(0.0, (3.0 * m2_ * m2_ - m4_) / 2.0));
            const double n2 = m2_ - s2;
            // No signal component at all (s2 = 0) reads as 0 dB rather than as log10 of nothing.
            stats_.snrDb = s2 <= 0.0 ? 0.0
                         : n2 > 1e-9 ? std::clamp(10.0 * std::log10(s2 / (2.0 * n2)), -10.0, 40.0) : 40.0;
        }

        // Half-symbol ambiguity: the integrator half a symbol away should be much weaker.
        onAcc_ += std::fabs(r);
        offAcc_ += other;
        if (++ambCount_ >= 400) {
            if (offAcc_ > 1.25 * onAcc_) tau_ += period_ / 2.0;
            onAcc_ = offAcc_ = 0.0;
            ambCount_ = 0;
        }

        period_ = std::clamp(period_ + 2e-4 * period_ * e, sps_ * 0.995, sps_ * 1.005);
        tau_ += period_ + 0.03 * period_ * e;
    }
    stats_.symbolsPerSecond = rate_ / period_;

    // Drop what is behind the integrator.
    const std::size_t keep = static_cast<std::size_t>(std::max(0.0, tau_ - 2.0 * period_));
    if (keep > 50000) {
        csum_.erase(csum_.begin(), csum_.begin() + static_cast<std::ptrdiff_t>(keep));
        csumRe_.erase(csumRe_.begin(), csumRe_.begin() + static_cast<std::ptrdiff_t>(keep));
        tau_ -= static_cast<double>(keep);
    }
    if (lastSymbols_.size() > 1500) lastSymbols_.erase(lastSymbols_.begin(), lastSymbols_.end() - 1500);
}

std::vector<float> Demodulator::process(const std::vector<std::complex<float>>& input) {
    std::vector<float> soft;
    if (!configured_ || input.empty()) return soft;
    // A single non-finite sample (driver hiccup, corrupt file) would otherwise run through the
    // mixer, decimator and loops and leave them NaN for good: in a test, 50 NaN samples halfway
    // through a capture stopped decoding for the rest of it. Such samples are treated as silence.
    const std::vector<std::complex<float>>* src = &input;
    std::vector<std::complex<float>> clean;
    for (const auto& x : input)
        if (!std::isfinite(x.real()) || !std::isfinite(x.imag())) {
            clean = input;
            for (auto& y : clean)
                if (!std::isfinite(y.real()) || !std::isfinite(y.imag())) y = {0.0f, 0.0f};
            src = &clean;
            break;
        }
    const std::vector<std::complex<float>>& iq = *src;
    if (mode_ == Mode::Coarse) {
        coarseSearch(iq);
        if (mode_ == Mode::Coarse || leftover_.empty()) return soft;
        std::vector<std::complex<float>> rest;
        rest.swap(leftover_);
        return process(rest);
    }
    std::vector<std::complex<float>> base;
    decimate(iq, base);
    if (mode_ == Mode::Fine) {
        fineSearch(base);
        if (mode_ == Mode::Track && !heldFine_.empty()) {
            std::vector<std::complex<float>> held;
            held.swap(heldFine_);
            track(held, soft);
        }
        return soft;
    }
    track(base, soft);
    return soft;
}

// ---------------------------------------------------------------------------------------------
// ELEKTRO-L N4 records and unit identification
// ---------------------------------------------------------------------------------------------

const L4Layout* l4Layout(int id) {
    static const L4Layout layouts[] = {
        {12, 12, "VUSS-E? solar UV sensor (inferred)"},
        {28, 12, "DIR-E solar X-ray sensor (uncalibrated raw response)"},
        {44, 69, "SKIF-6? particle spectrometer (inferred from ID family)"},
        {60, 69, "SKL-E? particle spectrometer (inferred from ID family)"},
        {64, 19, "cosmic-ray counter (GALS-E?)"},
        {80, 104, "magnetometer (FM-E?)"},
        {112, 15, "solar constant or housekeeping (ISP-2M?)"},
    };
    for (const auto& l : layouts)
        if (l.id == id) return &l;
    return nullptr;
}

std::vector<L4Record> splitL4Records(const Frame& frame) {
    std::vector<L4Record> out;
    const L4Layout* layout = l4Layout(frame.id);
    if (!layout || !frame.checksumOk) return out;
    const std::uint8_t* payload = frame.bytes.data() + 13;
    const int n = 209 / layout->recordBytes;
    for (int r = 0; r < n; ++r) {
        const std::uint8_t* p = payload + r * layout->recordBytes;
        // A record of 0x33 filler bytes ends the frame early.
        bool filler = true;
        for (int k = 0; k < std::min(4, layout->recordBytes); ++k) filler = filler && p[k] == 0x33;
        if (filler) break;
        L4Record rec;
        rec.id = frame.id;
        rec.index = r;
        // One record a second, the first at the frame time. Measured on the 10-18 N4 frames: each ID
        // sends as many records per frame as there are seconds between its frames (80: 2 in 2 s,
        // 44/60: 3 in 3 s, 64: 11 in 11 s, 112: 13 in 13 s, 12/28: 17 in 17 s), and the record
        // counters step by one record to record and across frames, so counter minus (frame time +
        // index) is the same for every record. The 2.65 s used before spread a frame's records past
        // the start of the next frame, so records of neighbouring frames interleaved and the plots
        // showed several values around one instant.
        rec.unixTime = frame.unixTime + 1.0 * r;
        rec.bytes.assign(p, p + layout->recordBytes);
        switch (frame.id) {
            case 12: case 28: rec.counter = p[1]; break;
            case 44: case 60: case 64: case 80: rec.counter = (p[1] << 8) | p[2]; break;
            default: break;
        }
        out.push_back(std::move(rec));
    }
    return out;
}

const std::vector<L4Field>& l4Fields() {
    static const std::vector<L4Field> fields = {
        {80, "B1 (24-bit)", 4, 3}, {80, "B2 (24-bit)", 7, 3}, {80, "B3 (24-bit)", 10, 3},
        {64, "count 1", 3, 1}, {64, "count 2", 5, 1}, {64, "count 3", 7, 1},
        {64, "word 9", 9, 1}, {64, "word 11", 11, 1}, {64, "word 13", 13, 1}, {64, "word 15", 15, 1},
        {44, "byte 5", 5, 0}, {44, "word 6", 6, 1}, {44, "word 8", 8, 1}, {44, "byte 11", 11, 0}, {44, "word 48", 48, 1},
        {60, "word 8", 8, 1}, {60, "word 10", 10, 1}, {60, "word 12", 12, 1}, {60, "word 14", 14, 1},
        {60, "count 17", 17, 0}, {60, "count 25", 25, 0}, {60, "count 41", 41, 0}, {60, "word 48", 48, 1},
        {28, "byte 2", 2, 0}, {28, "byte 3", 3, 0},
        {12, "byte 3", 3, 0}, {12, "byte 5", 5, 0},
        {112, "word 0", 0, 1}, {112, "word 2", 2, 1}, {112, "word 4", 4, 1}, {112, "word 6", 6, 1},
    };
    return fields;
}

double l4FieldValue(const L4Record& record, const L4Field& field) {
    const auto& b = record.bytes;
    const std::size_t o = static_cast<std::size_t>(field.offset);
    if (record.id != field.id) return std::numeric_limits<double>::quiet_NaN();
    // ID 80 carries a second record type (first byte 0x28); only 0xA8 records have the field triple.
    if (record.id == 80 && (b.empty() || b[0] != 0xA8)) return std::numeric_limits<double>::quiet_NaN();
    switch (field.kind) {
        case 0: return o < b.size() ? b[o] : std::numeric_limits<double>::quiet_NaN();
        case 1: return o + 1 < b.size() ? static_cast<double>((b[o] << 8) | b[o + 1]) : std::numeric_limits<double>::quiet_NaN();
        case 3: {
            if (o + 2 >= b.size()) return std::numeric_limits<double>::quiet_NaN();
            std::int32_t v = (b[o] << 16) | (b[o + 1] << 8) | b[o + 2];
            if (v & 0x800000) v -= 0x1000000;
            return v;
        }
        default: return std::numeric_limits<double>::quiet_NaN();
    }
}

void UnitIdentifier::note(int id, bool checksumOk) {
    if (!checksumOk || id < 0 || id > 255 || id == kIdFiller) return;
    ++count_[static_cast<std::size_t>(id)];
    ++total_;
}

namespace {
constexpr int kN4Ids[] = {12, 28, 44, 60, 64, 80, 112};
}

int UnitIdentifier::confidence() const {
    if (total_ < 20) return 0;
    std::uint32_t n4 = 0, published = count_[kIdSkl] + count_[kIdSkif];
    int distinctN4 = 0;
    for (int id : kN4Ids) {
        n4 += count_[static_cast<std::size_t>(id)];
        if (count_[static_cast<std::size_t>(id)] > 0) ++distinctN4;
    }
    const double share = std::max(n4, published) / static_cast<double>(total_);
    const int base = n4 >= published ? distinctN4 * 12 : 60;
    return std::clamp(static_cast<int>(base * share + (total_ > 200 ? 15 : 0)), 0, 99);
}

std::string UnitIdentifier::label() const {
    if (total_ < 20) return "identifying (need 20 instrument frames)";
    std::uint32_t n4 = 0;
    int distinctN4 = 0;
    for (int id : kN4Ids) {
        n4 += count_[static_cast<std::size_t>(id)];
        if (count_[static_cast<std::size_t>(id)] > 0) ++distinctN4;
    }
    const std::uint32_t published = count_[kIdSkl] + count_[kIdSkif];
    if (n4 > published && distinctN4 >= 4 && n4 * 10 >= total_ * 8) return "ELEKTRO-L N4 (165.8E)";
    if (published > n4 && published * 10 >= total_ * 3) return "ELEKTRO-L, not N4 (published layout: N2/N3 or ARKTIKA-M?)";
    return "unknown unit (new ID set, e.g. N5?)";
}

std::string UnitIdentifier::reason() const {
    std::string ids;
    for (int id = 0; id < 256; ++id)
        if (count_[static_cast<std::size_t>(id)] > 0) {
            if (!ids.empty()) ids += ", ";
            ids += std::to_string(id) + "x" + std::to_string(count_[static_cast<std::size_t>(id)]);
        }
    return "instrument frame IDs seen: " + (ids.empty() ? std::string("none") : ids) +
           ". N4 set = 12, 28, 44, 60, 64, 80, 112 (measured on N4); 32/48 = the published layout. "
           "GGAK frames carry no spacecraft ID, so this is a fingerprint, not a decoded field.";
}

}  // namespace nadirstation::ggak
