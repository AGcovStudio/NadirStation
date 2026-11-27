// GK-2A HRIT end-to-end simulation: known CADUs -> K7 r1/2 -> QPSK (G1 on I, G2 on Q) -> RRC 0.5
// at fs, CFO, random phase (any quadrant), timing offset, clock error, AWGN at Es/N0 (per QPSK symbol).
// Receive chain: QpskDemod (DEMOD=new, default) or PskDemod in QPSK mode (DEMOD=old), then
// QpskLinkDecoder; every delivered frame is compared byte for byte with what was sent (WRONG).
// usage: NadirStation_HritSim <EsN0> [cadus=300] [fs=6e6] [seed] [cfo=15000] [ppm=20] [oldAlpha] [oldBw]
#include "../src/gk2adecoder.cpp"   // RS tables and outputFor() for the generator
#include "../src/dsp.h"
#include <chrono>
#include <cstdio>
#include <random>
using namespace nadirstation;
using namespace nadirstation::gk2a;
namespace {
std::array<std::uint8_t, 32> rsParity(const std::array<std::uint8_t, 223>& msg) {
    const Gf256& g = gf();
    std::vector<std::uint8_t> gen{1};
    for (int k = 0; k < 32; ++k) {
        const std::uint8_t r = g.exp[static_cast<std::size_t>((11 * (112 + k)) % 255)];
        std::vector<std::uint8_t> next(gen.size() + 1, 0);
        for (std::size_t i = 0; i < gen.size(); ++i) { next[i] ^= gen[i]; next[i + 1] ^= g.mul(gen[i], r); }
        gen.swap(next);
    }
    std::array<std::uint8_t, 32> rem{};
    for (int i = 0; i < 223; ++i) {
        const std::uint8_t fb = static_cast<std::uint8_t>(msg[i] ^ rem[0]);
        for (int j = 0; j < 31; ++j) rem[j] = static_cast<std::uint8_t>(rem[j + 1] ^ g.mul(fb, gen[j + 1]));
        rem[31] = g.mul(fb, gen[32]);
    }
    return rem;
}
std::vector<std::uint8_t> makeCadu(std::mt19937& rng, std::uint32_t counter) {
    std::vector<std::uint8_t> f(1024, 0);
    f[0] = 0x1A; f[1] = 0xCF; f[2] = 0xFC; f[3] = 0x1D;
    std::uniform_int_distribution<int> byte(0, 255);
    for (int i = 4; i < 896; ++i) f[i] = static_cast<std::uint8_t>(byte(rng));
    f[4] = 0x40 | 0x0B; f[5] = static_cast<std::uint8_t>((0x2 << 6) | 5);
    f[6] = static_cast<std::uint8_t>(counter >> 16); f[7] = static_cast<std::uint8_t>(counter >> 8); f[8] = static_cast<std::uint8_t>(counter);
    for (int pos = 0; pos < 4; ++pos) {
        std::array<std::uint8_t, 223> m{};
        for (int ii = 0; ii < 223; ++ii) m[ii] = kFromDualBasis[f[4 + ii * 4 + pos]];
        const auto p = rsParity(m);
        for (int ii = 0; ii < 32; ++ii) f[4 + (223 + ii) * 4 + pos] = kToDualBasis[p[ii]];
    }
    for (std::size_t i = 4; i < f.size(); ++i) f[i] ^= kCcsdsPn[(i - 4) % 255];
    return f;
}
double rrc(double t, double a) {
    const double pi = 3.14159265358979323846;
    if (std::fabs(t) < 1e-9) return 1.0 - a + 4 * a / pi;
    if (std::fabs(std::fabs(4 * a * t) - 1.0) < 1e-9)
        return a / std::sqrt(2.0) * ((1 + 2 / pi) * std::sin(pi / (4 * a)) + (1 - 2 / pi) * std::cos(pi / (4 * a)));
    return (std::sin(pi * t * (1 - a)) + 4 * a * t * std::cos(pi * t * (1 + a))) / (pi * t * (1 - 16 * a * a * t * t));
}
}
int main(int argc, char** argv) {
    const double esn0 = argc > 1 ? std::atof(argv[1]) : 3.0;
    const int nCadu = argc > 2 ? std::atoi(argv[2]) : 300;
    const double fs = argc > 3 ? std::atof(argv[3]) : 6e6;
    const unsigned seed = argc > 4 ? std::atoi(argv[4]) : 1;
    const double cfo = argc > 5 ? std::atof(argv[5]) : 15000.0;
    const double ppm = argc > 6 ? std::atof(argv[6]) : 20.0;
    const float oldAlpha = argc > 7 ? std::atof(argv[7]) : 0.35f;
    const float oldBw = argc > 8 ? std::atof(argv[8]) : 0.002f;
    const double rs = 3e6 * (1 + ppm * 1e-6);
    std::mt19937 rng(seed);
    std::vector<std::vector<std::uint8_t>> sent;
    std::vector<std::uint8_t> bits;
    for (int c = 0; c < nCadu; ++c) {
        auto f = makeCadu(rng, c);
        for (auto b : f) for (int i = 7; i >= 0; --i) bits.push_back((b >> i) & 1);
        for (std::size_t i = 4; i < f.size(); ++i) f[i] ^= kCcsdsPn[(i - 4) % 255];
        sent.push_back(f);
    }
    std::vector<std::complex<float>> sym; sym.reserve(bits.size());
    int reg = 0;
    for (auto b : bits) {
        reg = ((reg << 1) | b) & kRegisterMask;
        const int o = outputFor(reg);
        sym.emplace_back((o & 1) ? -1.f : 1.f, (o & 2) ? -1.f : 1.f);
    }
    const double sps = fs / rs;
    const std::size_t nS = static_cast<std::size_t>((sym.size() + 10) * sps);
    std::vector<std::complex<float>> iq(nS);
    std::uniform_real_distribution<double> u(0, 1);
    const double t0 = u(rng), ph0 = u(rng) * 6.283185307;
    const int span = 6;
    double ps = 0;
    for (std::size_t n = 0; n < nS; ++n) {
        const double ts = n / sps - t0;
        const long k0 = static_cast<long>(std::floor(ts)) - span;
        std::complex<double> acc = 0;
        for (long k = k0; k <= k0 + 2 * span + 1; ++k) {
            if (k < 0 || k >= (long)sym.size()) continue;
            acc += std::complex<double>(sym[k]) * rrc(ts - k, 0.5);
        }
        const double ph = ph0 + 6.283185307179586 * cfo * n / fs;
        acc *= std::polar(1.0, ph);
        iq[n] = std::complex<float>(acc);
        ps += std::norm(acc);
    }
    ps /= nS;
    const double nv = ps * sps / std::pow(10.0, esn0 / 10.0);
    std::normal_distribution<float> nd(0.f, std::sqrt(nv / 2));
    for (auto& z : iq) z += std::complex<float>(nd(rng), nd(rng));
    auto check = [&](const Cadu& c) {
        const std::uint32_t cnt = (std::uint32_t(c.bytes[6]) << 16) | (std::uint32_t(c.bytes[7]) << 8) | c.bytes[8];
        return cnt < sent.size() && std::equal(c.bytes.begin() + 4, c.bytes.end(), sent[cnt].begin() + 4);
    };
    const std::size_t block = 1 << 16;
    const std::string dm = std::getenv("DEMOD") ? std::getenv("DEMOD") : "new";
    dsp::PskDemod od; od.setQpsk(true); od.configure(fs, 3e6f, oldAlpha, oldBw);
    dsp::QpskDemod nd2; nd2.configure(fs, 3e6f, std::getenv("NALPHA") ? std::atof(std::getenv("NALPHA")) : 0.5f,
                                      std::getenv("NCBW") ? std::atof(std::getenv("NCBW")) : 0.001f,
                                      std::getenv("NTBW") ? std::atof(std::getenv("NTBW")) : 0.0008f);
    QpskLinkDecoder nh; if (std::getenv("NORESCUE")) nh.setRescueEnabled(false); if (const char* m = std::getenv("MISSLIMIT")) nh.setMissLimit(std::atoi(m));
    std::size_t good = 0, wrong = 0, syms = 0;
    double lockSum = 0; int lockN = 0;
    for (std::size_t o = 0; o < iq.size(); o += block) {
        std::vector<std::complex<float>> b(iq.begin() + o, iq.begin() + std::min(iq.size(), o + block));
        auto r = dm == "old" ? od.process(b) : nd2.process(b);
        syms += r.symbols.size();
        lockSum += dm == "old" ? od.lockMeasure() : nd2.lockMeasure(); ++lockN;
        nh.feedSymbols(r.symbols); for (auto& c : nh.takeCadus()) if (c.syncFound && c.rsOk) { ++good; if (!check(c)) ++wrong; }
    }
    std::printf("demod %s EsN0 %5.2f  rsOK %3zu/%d (%5.1f%%) WRONG %zu  syms %zu/%zu  lock %.2f  map %d\n",
                dm.c_str(), esn0, good, nCadu, 100.0 * good / nCadu, wrong, syms, sym.size(), lockSum / lockN,
                nh.hypothesis());
    { const auto& s = nh.stats(); std::printf("   sync %zu assembled %zu syncLoss %zu flywheel %zu fwOK %zu rescue %zu/%zu asmTol %zu\n", s.cadusWithSync, s.cadusAssembled, s.syncLosses, s.flywheelFrames, s.flywheelRsOk, s.rescueOk, s.rescueTried, s.asmTolerated); }
}
