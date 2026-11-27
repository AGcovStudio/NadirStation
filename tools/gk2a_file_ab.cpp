// Run a recorded cs16 capture through PskDemod + LritDecoder + LritFileAssembler, as the live
// path does (soft = -0.5 * Re, 65536-sample blocks). Optional extra AWGN, in dB below the
// measured in-band SNR, to find the threshold on a real signal.
// usage: gk2a_file <file.cs16> <rate> [extraNoiseEsN0dB(target, 99 = none)] [seed]
#include "../src/gk2adecoder.h"
#include "../src/gk2adatalink.h"
#include "../src/dsp.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <vector>

using namespace nadirstation;

int main(int argc, char** argv) {
    if (argc < 3) return 2;
    const double fs = std::atof(argv[2]);
    const double target = argc > 3 ? std::atof(argv[3]) : 99.0;
    const unsigned seed = argc > 4 ? static_cast<unsigned>(std::atoi(argv[4])) : 1u;
    std::ifstream in(argv[1], std::ios::binary);
    std::vector<std::int16_t> raw;
    in.seekg(0, std::ios::end);
    const std::size_t bytes = static_cast<std::size_t>(in.tellg());
    in.seekg(0);
    raw.resize(bytes / 2);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(bytes));
    std::vector<std::complex<float>> iq(raw.size() / 2);
    for (std::size_t i = 0; i < iq.size(); ++i) iq[i] = {raw[2 * i] / 2048.0f, raw[2 * i + 1] / 2048.0f};

    // Pass 1 (only when adding noise): measure symbol Es/N0 of the clean capture with M2M4 on the
    // demod output, then add white noise across the whole capture band to reach the target.
    auto runDemodSnr = [&](const std::vector<std::complex<float>>& x) {
        dsp::PskDemod d(static_cast<float>(fs), 128000.0f, 0.5f, 0.02f);
        double m2 = 0, m4 = 0; std::size_t n = 0;
        const auto r = d.process(x);
        for (std::size_t i = r.symbols.size() / 4; i < r.symbols.size(); ++i) {
            const double p = std::norm(r.symbols[i]); m2 += p; m4 += p * p; ++n;
        }
        m2 /= n; m4 /= n;
        const double s = std::sqrt(std::max(0.0, 2 * m2 * m2 - m4));
        return 10 * std::log10(std::max(1e-9, s / std::max(1e-12, m2 - s)));
    };
    const std::size_t probeLen = std::min<std::size_t>(iq.size(), static_cast<std::size_t>(fs * 4));
    const std::vector<std::complex<float>> probe(iq.begin(), iq.begin() + static_cast<std::ptrdiff_t>(probeLen));
    const double clean = runDemodSnr(probe);
    if (target < 90.0) {
        // In-band noise per sample N: Es/N0 = S*fs/Rs / N. Estimate S and N from the clean probe.
        double p = 0; for (const auto& z : iq) p += std::norm(z); p /= static_cast<double>(iq.size());
        const double esn0 = std::pow(10.0, clean / 10.0);
        // p = S + N, S*sps/N = esn0  ->  N = p / (1 + esn0/sps)
        const double sps = fs / 128000.0;
        const double N = p / (1.0 + esn0 / sps);
        const double S = p - N;
        const double Nwant = S * sps / std::pow(10.0, target / 10.0);
        const double add = Nwant - N;
        if (add > 0) {
            std::mt19937 rng(seed);
            std::normal_distribution<float> nd(0.0f, static_cast<float>(std::sqrt(add / 2)));
            for (auto& z : iq) z += std::complex<float>(nd(rng), nd(rng));
        }
    }
    const double now = runDemodSnr(probe.size() == iq.size() ? iq : std::vector<std::complex<float>>(iq.begin(), iq.begin() + static_cast<std::ptrdiff_t>(probeLen)));

    dsp::PskDemod demod(static_cast<float>(fs), 128000.0f, 0.5f, 0.02f);
    gk2a::LritDecoder dec;
    gk2a::LritFileAssembler asmb;
    std::size_t synced = 0, good = 0, files = 0;
    const std::size_t block = 65536;
    for (std::size_t off = 0; off < iq.size(); off += block) {
        std::vector<std::complex<float>> b(iq.begin() + static_cast<std::ptrdiff_t>(off),
                                          iq.begin() + static_cast<std::ptrdiff_t>(std::min(iq.size(), off + block)));
        const auto r = demod.process(b);
        std::vector<float> soft;
        soft.reserve(r.symbols.size());
        for (const auto& z : r.symbols) soft.push_back(-0.5f * z.real());
        dec.feedSoftBits(soft, false);
        for (const auto& c : dec.takeCadus()) {
            if (!c.syncFound) continue;
            ++synced;
            if (!c.rsOk) continue;
            ++good;
            if (std::getenv("DBG")) { const auto v = gk2a::parseVcdu(c.bytes); static std::vector<std::uint8_t> prevFill; if (v.vcid == 63) prevFill = c.bytes; if (v.vcid != 63) { int d=0; std::vector<int> pos; for (std::size_t i=4;i<c.bytes.size()&&i<prevFill.size();++i) if (c.bytes[i]!=prevFill[i]) {++d; if(pos.size()<20) pos.push_back((int)i);} std::printf("  differs from previous fill in %d bytes:", d); for(int q:pos) std::printf(" %d(%02x/%02x)", q, c.bytes[q], prevFill[q]); std::printf("\n"); } if (v.vcid != 63) std::printf("  rsOK frame vcid %d cnt %u fly %d asmErr %d rsFix %d off %d\n", v.vcid, v.counter, c.flywheel, c.asmBitErrors, c.rsCorrectedBytes, c.syncOffsetBits); }
            files += asmb.feed(c.bytes).size();
        }
    }
    const auto& s = dec.stats();
    std::printf("cleanEsN0 %.2f dB  testEsN0 %.2f dB  sync %zu  rsOK %zu  RSbytesFixed %zu  files %zu  vcfill %zu  crcFail %zu  slips %zu  cfo %.0f Hz\n",
                clean, now, synced, good, s.rsBytesCorrected, files, asmb.fillerCadus, asmb.crcFailures, s.pairSlips,
                demod.symbolRate() > 0 ? 0.0 : 0.0);
    return 0;
}
