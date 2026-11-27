// Decode a GK-2A HRIT capture (interleaved int16 I/Q, .cs16) with the receive chain the
// application uses: QpskDemod -> QpskLinkDecoder -> LritFileAssembler. Optionally adds white noise
// to bring the capture down to a target Es/N0, to find the threshold on a real signal.
// usage: NadirStation_HritFile <file.cs16> <rate> [targetEsN0dB, 99 = as recorded] [rrcAlpha=0.5] [carrierBW=0.001] [timingBW=0.0008]
#include "gk2adecoder.h"
#include "gk2adatalink.h"
#include "dsp.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <random>
#include <vector>
using namespace nadirstation;
namespace {
// Es/N0 from the M2M4 moments of the demodulated QPSK symbols.
double esn0Of(const std::vector<std::complex<float>>& s) {
    double m2 = 0, m4 = 0; std::size_t n = 0;
    for (std::size_t i = s.size() / 4; i < s.size(); ++i) { const double p = std::norm(s[i]); m2 += p; m4 += p * p; ++n; }
    if (!n) return -99;
    m2 /= n; m4 /= n;
    const double sig = std::sqrt(std::max(0.0, 2 * m2 * m2 - m4));
    return 10 * std::log10(std::max(1e-9, sig / std::max(1e-12, m2 - sig)));
}
}
int main(int argc, char** argv) {
    if (argc < 3) { std::fprintf(stderr, "usage: %s file.cs16 rate [targetEsN0] [alpha]\n", argv[0]); return 2; }
    const double fs = std::atof(argv[2]);
    const double target = argc > 3 ? std::atof(argv[3]) : 99.0;
    const float alpha = argc > 4 ? std::atof(argv[4]) : 0.5f;
    const float carrierBw=argc>5?std::atof(argv[5]):0.001f;
    const float timingBw=argc>6?std::atof(argv[6]):0.0008f;
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) { std::fprintf(stderr, "cannot open %s\n", argv[1]); return 2; }
    in.seekg(0, std::ios::end); const std::size_t bytes = static_cast<std::size_t>(in.tellg()); in.seekg(0);
    std::vector<std::int16_t> raw(bytes / 2);
    in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2));
    std::vector<std::complex<float>> iq(raw.size() / 2);
    for (std::size_t i = 0; i < iq.size(); ++i) iq[i] = {raw[2 * i] / 2048.0f, raw[2 * i + 1] / 2048.0f};
    std::vector<std::int16_t>().swap(raw);
    auto demodSnr = [&](const std::vector<std::complex<float>>& x) {
        dsp::QpskDemod d; d.configure(fs, 3e6f, alpha, carrierBw, timingBw);
        const std::size_t n = std::min<std::size_t>(x.size(), static_cast<std::size_t>(fs * 2));
        return esn0Of(d.process({x.begin(), x.begin() + static_cast<std::ptrdiff_t>(n)}).symbols);
    };
    const double clean = demodSnr(iq);
    if (target < 90.0) {
        double p = 0; for (const auto& z : iq) p += std::norm(z); p /= static_cast<double>(iq.size());
        const double sps = fs / 3e6, e = std::pow(10.0, clean / 10.0);
        const double N = p / (1.0 + e / sps), S = p - N;
        const double add = S * sps / std::pow(10.0, target / 10.0) - N;
        if (add > 0) {
            std::mt19937 rng(1); std::normal_distribution<float> nd(0.0f, static_cast<float>(std::sqrt(add / 2)));
            for (auto& z : iq) z += std::complex<float>(nd(rng), nd(rng));
        }
    }
    const double test = target < 90.0 ? demodSnr(iq) : clean;
    if (const char* wp = std::getenv("WRITE")) {
        std::ofstream o(wp, std::ios::binary);
        std::vector<std::int16_t> w(iq.size() * 2); std::size_t clipped = 0;
        for (std::size_t i = 0; i < iq.size(); ++i) for (int k = 0; k < 2; ++k) {
            const float v = (k ? iq[i].imag() : iq[i].real()) * 2048.0f;
            const float c = std::max(-32767.0f, std::min(32767.0f, v)); clipped += c != v;
            w[2 * i + k] = static_cast<std::int16_t>(std::lround(c));
        }
        o.write(reinterpret_cast<const char*>(w.data()), static_cast<std::streamsize>(w.size() * 2));
        std::printf("wrote %s (%zu samples, %zu clipped) tested Es/N0 %.2f dB\n", wp, iq.size(), clipped, test);
    }
    dsp::QpskDemod demod; demod.configure(fs, 3e6f, alpha, carrierBw, timingBw);
    gk2a::QpskLinkDecoder link; if(std::getenv("NORESCUE")) link.setRescueEnabled(false); if(const char* m=std::getenv("MISSLIMIT")) link.setMissLimit(std::atoi(m)); gk2a::LritFileAssembler files;
    std::size_t asmHist[33]={0}, flyN=0; std::size_t good = 0, fileCount = 0; std::vector<std::size_t> vc(64, 0);
    std::size_t lastLoss=0;int lastMap=link.hypothesis();std::uint32_t lastCount=0;bool haveCount=false;size_t gaps=0;
    for (std::size_t o = 0; o < iq.size(); o += 65536) {
        const auto r = demod.process({iq.begin() + static_cast<std::ptrdiff_t>(o),
                                      iq.begin() + static_cast<std::ptrdiff_t>(std::min(iq.size(), o + 65536))});
        link.feedSymbols(r.symbols);
        if(link.stats().syncLosses!=lastLoss || link.hypothesis()!=lastMap){
            std::printf("at %.3fs sync_losses %zu mapping %d->%d RSok %zu sync %zu\n",double(o)/fs,link.stats().syncLosses,lastMap,link.hypothesis(),link.stats().cadusWithGoodRs,link.stats().cadusWithSync);
            lastLoss=link.stats().syncLosses;lastMap=link.hypothesis();
        }
        for (auto& c : link.takeCadus()) {
            if (!c.syncFound || !c.rsOk) continue;
            const auto counter=(std::uint32_t(c.bytes[6])<<16)|(std::uint32_t(c.bytes[7])<<8)|c.bytes[8];
            if((c.bytes[5]&63)==63){if(haveCount && counter!=((lastCount+1)&0xffffff)){
                if(gaps++<12)std::printf("filler gap at %.3fs: %u -> %u\n",double(o)/fs,lastCount,counter);
            }lastCount=counter;haveCount=true;}
            ++good; ++vc[c.bytes[5] & 63]; ++asmHist[std::min(c.asmBitErrors,32)]; if(c.flywheel)++flyN;
            fileCount += files.feed(c.bytes).size();
        }
    }
    const auto& s = link.stats();
    std::printf("Es/N0 recorded %.2f dB, tested %.2f dB | CFO %.0f Hz lock %.2f | sync %zu RS-OK %zu (fixed %zu bytes, flywheel %zu) | "
                "mapping %d | files %zu | fill %zu | CRC fail %zu\n",
                clean, test, demod.carrierOffsetHz(), demod.lockMeasure(), s.cadusWithSync, good, s.rsBytesCorrected,
                s.flywheelRsOk, link.hypothesis(), fileCount, files.fillerCadus, files.crcFailures);
    std::printf("asm errors histogram of RS-ok frames:"); for(int i=0;i<=32;++i) if(asmHist[i]) std::printf(" %d:%zu",i,asmHist[i]); std::printf("  (flywheel %zu)\n",flyN);
    std::printf("CADUs per VCID:");
    for (int i = 0; i < 64; ++i) if (vc[i]) std::printf(" %d:%zu", i, vc[i]);
    std::printf("\n");
    std::printf("filler counter gaps %zu\n",gaps);
    std::printf("rescue tried %zu ok %zu\n", s.rescueTried, s.rescueOk);
    std::printf("carrier_bw %.6f timing_bw %.6f alpha %.2f sync_losses %zu asm_tolerated %zu\n",carrierBw,timingBw,alpha,s.syncLosses,s.asmTolerated);
    return 0;
}
