// End-to-end GK-2A LRIT simulation: valid CADUs (RS + randomiser + ASM) -> K=7 r1/2 conv ->
// RRC BPSK at fs with CFO, timing offset and AWGN -> PskDemod -> LritDecoder.
// Built twice (orig / new decoder) so both see bit-identical input.
//
// usage: gk2a_sim <EsN0 dB> [cadus=200] [fs=512000] [seed=1] [cfoHz=1500] [block=65536] [ppm=0]
#include "../src/gk2adecoder.cpp"   // pulls in the anonymous-namespace RS tables and outputFor()
#include "../src/dsp.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <random>

using namespace nadirstation;
using namespace nadirstation::gk2a;

namespace {

// Systematic RS(255,223) encoder in the conventional basis, roots beta^(112+k), beta = alpha^11.
std::array<std::uint8_t, 32> rsParity(const std::array<std::uint8_t, 223>& msg) {
    const Gf256& g = gf();
    // generator g(x) = prod (x - r_k), coefficients gen[0] = highest power
    std::vector<std::uint8_t> gen{1};
    for (int k = 0; k < 32; ++k) {
        const std::uint8_t r = g.exp[static_cast<std::size_t>((11 * (112 + k)) % 255)];
        std::vector<std::uint8_t> next(gen.size() + 1, 0);
        for (std::size_t i = 0; i < gen.size(); ++i) {
            next[i] ^= gen[i];
            next[i + 1] ^= g.mul(gen[i], r);
        }
        gen.swap(next);
    }
    std::array<std::uint8_t, 32> rem{};
    for (int i = 0; i < 223; ++i) {
        const std::uint8_t fb = static_cast<std::uint8_t>(msg[static_cast<std::size_t>(i)] ^ rem[0]);
        for (int j = 0; j < 31; ++j)
            rem[static_cast<std::size_t>(j)] = static_cast<std::uint8_t>(rem[static_cast<std::size_t>(j + 1)] ^ g.mul(fb, gen[static_cast<std::size_t>(j + 1)]));
        rem[31] = g.mul(fb, gen[32]);
    }
    return rem;
}

std::vector<std::uint8_t> makeCadu(std::mt19937& rng, std::uint32_t counter) {
    std::vector<std::uint8_t> f(1024, 0);
    f[0] = 0x1A; f[1] = 0xCF; f[2] = 0xFC; f[3] = 0x1D;
    std::uniform_int_distribution<int> byte(0, 255);
    for (int i = 4; i < 4 + 892; ++i) f[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(byte(rng));
    // VCDU header: version 01, scid, vcid 5, counter
    f[4] = 0x40 | 0x0B; f[5] = static_cast<std::uint8_t>((0x2 << 6) | 5);
    f[6] = static_cast<std::uint8_t>(counter >> 16); f[7] = static_cast<std::uint8_t>(counter >> 8);
    f[8] = static_cast<std::uint8_t>(counter);
    for (int pos = 0; pos < 4; ++pos) {
        std::array<std::uint8_t, 223> m{};
        for (int ii = 0; ii < 223; ++ii) m[static_cast<std::size_t>(ii)] = kFromDualBasis[f[4 + static_cast<std::size_t>(ii * 4 + pos)]];
        const auto p = rsParity(m);
        for (int ii = 0; ii < 32; ++ii)
            f[4 + static_cast<std::size_t>((223 + ii) * 4 + pos)] = kToDualBasis[p[static_cast<std::size_t>(ii)]];
    }
    if (!rsSyndromesZero(f, 4)) { std::fprintf(stderr, "encoder self-check failed\n"); std::exit(3); }
    for (std::size_t i = 4; i < f.size(); ++i) f[i] ^= kCcsdsPn[(i - 4) % 255];
    return f;
}

double rrc(double t, double a) {   // t in symbols
    const double pi = 3.14159265358979323846;
    if (std::fabs(t) < 1e-9) return 1.0 - a + 4 * a / pi;
    if (std::fabs(std::fabs(4 * a * t) - 1.0) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / pi) * std::sin(pi / (4 * a)) + (1 - 2 / pi) * std::cos(pi / (4 * a)));
    return (std::sin(pi * t * (1 - a)) + 4 * a * t * std::cos(pi * t * (1 + a))) / (pi * t * (1 - 16 * a * a * t * t));
}

}  // namespace

int main(int argc, char** argv) {
    const double esn0 = argc > 1 ? std::atof(argv[1]) : 2.0;
    const int nCadu = argc > 2 ? std::atoi(argv[2]) : 200;
    const double fs = argc > 3 ? std::atof(argv[3]) : 512000.0;
    const unsigned seed = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 1u;
    const double cfo = argc > 5 ? std::atof(argv[5]) : 1500.0;
    const std::size_t block = argc > 6 ? static_cast<std::size_t>(std::atol(argv[6])) : 65536;
    const double ppm = argc > 7 ? std::atof(argv[7]) : 0.0;
    const double rs = 128000.0 * (1.0 + ppm * 1e-6);

    std::mt19937 rng(seed);
    std::vector<std::uint8_t> bits;
    bits.reserve(static_cast<std::size_t>(nCadu) * 8192);
    std::vector<std::vector<std::uint8_t>> sent;
    for (int c = 0; c < nCadu; ++c) {
        const auto f = makeCadu(rng, static_cast<std::uint32_t>(c));
        sent.push_back(f);
        for (std::size_t i = 4; i < f.size(); ++i) sent.back()[i] ^= kCcsdsPn[(i - 4) % 255];
        for (const auto b : f) for (int i = 7; i >= 0; --i) bits.push_back(static_cast<std::uint8_t>((b >> i) & 1));
    }
    // continuous convolutional encoding, no flush (a live stream never flushes)
    std::vector<float> sym;
    sym.reserve(bits.size() * 2);
    int reg = 0;
    for (const auto b : bits) {
        reg = ((reg << 1) | b) & kRegisterMask;
        const int o = outputFor(reg);
        sym.push_back((o & 1) ? 1.0f : -1.0f);
        sym.push_back((o & 2) ? 1.0f : -1.0f);
    }
    if (std::getenv("IDEAL")) {
        // matched-filter output with perfect sync: symbol +-1 plus N(0, 1/(2 EsN0))
        std::normal_distribution<float> nd(0.0f, static_cast<float>(std::sqrt(0.5 / std::pow(10.0, esn0 / 10.0))));
        std::vector<float> r(sym.size());
        double m = 0; for (std::size_t i = 0; i < sym.size(); ++i) { r[i] = sym[i] + nd(rng); m += std::fabs(r[i]); }
        m /= static_cast<double>(r.size());
        const std::size_t skip = std::getenv("SKIP") ? std::atoi(std::getenv("SKIP")) : 0;
        LritDecoder dec; std::size_t good = 0, synced = 0;
        const std::size_t blk = 20000;
        for (std::size_t off = skip; off < r.size(); off += blk) {
            std::vector<float> soft;
            for (std::size_t i = off; i < std::min(r.size(), off + blk); ++i) soft.push_back(static_cast<float>(-0.5 * r[i] / m));
            dec.feedSoftBits(soft, false);
            for (const auto& c : dec.takeCadus()) { if (!c.syncFound) continue; ++synced; if (c.rsOk) ++good; }
        }
        std::printf("IDEAL EsN0 %5.2f  cadus %d  sync %zu  rsOK %zu (%.1f%%)  slips %zu\n", esn0, nCadu, synced, good, 100.0 * good / nCadu, dec.stats().pairSlips);
        return 0;
    }
    // pulse shaping at fs, arbitrary ratio, random start timing
    const double sps = fs / rs;
    const std::size_t nSamp = static_cast<std::size_t>((static_cast<double>(sym.size()) + 8) * sps);
    std::vector<std::complex<float>> iq(nSamp);
    std::uniform_real_distribution<double> u(0.0, 1.0);
    const double t0 = u(rng);
    const double ph0 = u(rng) * 6.283185307;
    const int span = 6;
    double ps = 0.0;
    for (std::size_t n = 0; n < nSamp; ++n) {
        const double ts = static_cast<double>(n) / sps - t0;   // in symbols
        const long k0 = static_cast<long>(std::floor(ts)) - span;
        double acc = 0.0;
        for (long k = k0; k <= k0 + 2 * span + 1; ++k) {
            if (k < 0 || k >= static_cast<long>(sym.size())) continue;
            acc += sym[static_cast<std::size_t>(k)] * rrc(ts - static_cast<double>(k), 0.5);
        }
        const double ph = ph0 + 6.283185307179586 * cfo * static_cast<double>(n) / fs;
        iq[n] = std::complex<float>(static_cast<float>(acc * std::cos(ph)), static_cast<float>(acc * std::sin(ph)));
        ps += acc * acc;
    }
    ps /= static_cast<double>(nSamp);
    const double noiseVar = ps * sps / std::pow(10.0, esn0 / 10.0);   // complex variance per sample
    std::normal_distribution<float> nd(0.0f, static_cast<float>(std::sqrt(noiseVar / 2.0)));
    for (auto& z : iq) z += std::complex<float>(nd(rng), nd(rng));
    if (const char* fl = std::getenv("FLIP")) {   // 180-degree phase flips at these fractions of the run
        for (double f : {std::atof(fl), std::atof(fl) + 0.37})
            for (std::size_t n = static_cast<std::size_t>(f * nSamp); n < nSamp; ++n) iq[n] = -iq[n];
    }

    const float kScale = std::getenv("SCALE") ? static_cast<float>(std::atof(std::getenv("SCALE"))) : 0.5f;
    dsp::PskDemod demod(static_cast<float>(fs), 128000.0f, 0.5f, 0.02f);
    LritDecoder dec;
    std::size_t good = 0, synced = 0, corrected = 0, wrong = 0;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t off = 0; off < iq.size(); off += block) {
        const std::size_t end = std::min(iq.size(), off + block);
        std::vector<std::complex<float>> blk(iq.begin() + static_cast<std::ptrdiff_t>(off),
                                             iq.begin() + static_cast<std::ptrdiff_t>(end));
        const auto r = demod.process(blk);
        std::vector<float> soft;
        soft.reserve(r.symbols.size());
        for (const auto& z : r.symbols) soft.push_back(-kScale * z.real());
        dec.feedSoftBits(soft, false);
        for (const auto& c : dec.takeCadus()) {
            if (!c.syncFound) continue;
            ++synced;
            if (c.rsOk) {
                ++good; if (c.rsCorrectedBytes) ++corrected;
                const std::uint32_t cnt = (std::uint32_t(c.bytes[6]) << 16) | (std::uint32_t(c.bytes[7]) << 8) | c.bytes[8];
                if (cnt >= sent.size() || !std::equal(c.bytes.begin() + 4, c.bytes.end(), sent[cnt].begin() + 4)) ++wrong;
            }
        }
    }
    const double secs = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("EsN0 %5.2f dB (EbN0 %5.2f)  cadus %d  sync %zu  rsOK %zu (%.1f%%)  corrected %zu  slips %zu  WRONG %zu  time %.1fs (%.1fx realtime)\n",
                esn0, esn0 + 3.0103, nCadu, synced, good, 100.0 * static_cast<double>(good) / nCadu, corrected,
                dec.stats().pairSlips, wrong, secs, (static_cast<double>(nSamp) / fs) / secs);
    return 0;
}
