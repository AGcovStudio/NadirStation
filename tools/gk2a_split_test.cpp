// Find where a block boundary changes the symbol stream.
//
// The demodulator produces very different results at different block sizes -- on one recording the
// frame count ranges over six to one as only the block size changes -- while the tool is
// deterministic. That means the state carried across a call is not the whole story: something about
// where a block ends changes what happens next.
//
// Reasoning about the source has not settled it. Every piece of state that should persist does
// (carrier phase and frequency, AGC gain, decimation phase and history, matched-filter history,
// the Mueller-Muller accumulator and its fractional offset), and the index arithmetic across the
// history prefix checks out. So this measures instead.
//
// The method: take one run of samples, demodulate it twice -- once as a single call, once split in
// two -- and compare the symbol streams. If the demodulator is continuous the two agree except for
// a short region around the seam, and the first difference says which stage lost the thread. If the
// streams diverge immediately and never recover, the state is not being carried at all.
//
// Usage: NadirStation_GK2ASplit <file> --rate 2000000 --format int16 --symbols 200000 [--split-at 100000]

#include "../src/dsp.h"
#include "../src/gk2adecoder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

// The Viterbi decoder takes soft values with the sign carrying the bit, which is what the
// demodulator's symbols hold after the real part is taken.
std::vector<float> floatsOf(const std::vector<std::complex<float>>& symbols, int polarity) {
    std::vector<float> out;
    out.reserve(symbols.size());
    for (const auto& z : symbols) out.push_back(polarity == 0 ? z.real() : -z.real());
    return out;
}

struct Options {
    std::string path;
    double rate = 2.0e6;
    std::string format = "int16";
    std::size_t symbols = 6000000;
    std::size_t splitAt = 0;      // samples in the first half; 0 means half of what was read
    // This service needs the symbol spectrum inverted; the streaming tool applies the same
    // correction. Without it no frame is ever found, and a comparison of frame counts would be
    // comparing two zeros.
    int polarity = 1;
};

bool readSamples(std::ifstream& in, const std::string& format, std::size_t want,
                 std::vector<std::complex<float>>& out) {
    out.clear();
    out.reserve(want);
    if (format == "int16") {
        std::vector<std::int16_t> raw(want * 2);
        in.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(raw.size() * sizeof(std::int16_t)));
        const std::size_t got = static_cast<std::size_t>(in.gcount()) / (2 * sizeof(std::int16_t));
        for (std::size_t i = 0; i < got; ++i)
            out.emplace_back(static_cast<float>(raw[2 * i]) / 32768.0f,
                             static_cast<float>(raw[2 * i + 1]) / 32768.0f);
    } else {
        std::vector<float> raw(want * 2);
        in.read(reinterpret_cast<char*>(raw.data()),
                static_cast<std::streamsize>(raw.size() * sizeof(float)));
        const std::size_t got = static_cast<std::size_t>(in.gcount()) / (2 * sizeof(float));
        for (std::size_t i = 0; i < got; ++i)
            out.emplace_back(raw[2 * i], raw[2 * i + 1]);
    }
    return !out.empty();
}

}  // namespace

int main(int argc, char** argv) {
    Options o;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) o.rate = std::strtod(argv[++i], nullptr);
        else if (a == "--format" && i + 1 < argc) o.format = argv[++i];
        else if (a == "--symbols" && i + 1 < argc)
            o.symbols = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (a == "--split-at" && i + 1 < argc)
            o.splitAt = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (a == "--polarity" && i + 1 < argc)
            o.polarity = static_cast<int>(std::strtol(argv[++i], nullptr, 10));
        else if (o.path.empty()) o.path = a;
    }
    if (o.path.empty()) {
        std::cout << "usage: NadirStation_GK2ASplit <file> [--rate Hz] [--format int16|float32]\n"
                     "                       [--symbols N] [--split-at samples]\n";
        return 2;
    }

    // The symbol rate this service uses; only the ratio to the sample rate matters here.
    constexpr double kSymbolRate = 128000.0;
    // Enough samples to yield the requested number of symbols, plus slack for the filters.
    const std::size_t want =
        static_cast<std::size_t>(o.symbols * o.rate / kSymbolRate) + 4096;

    std::ifstream in(o.path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << o.path << "\n";
        return 3;
    }
    std::vector<std::complex<float>> samples;
    if (!readSamples(in, o.format, want, samples)) {
        std::cerr << "no samples read\n";
        return 3;
    }
    std::cout << "read " << samples.size() << " samples\n";

    const std::size_t split = o.splitAt > 0 && o.splitAt < samples.size()
                                  ? o.splitAt : samples.size() / 2;

    // Run 1: the whole span in a single call.
    nadirstation::dsp::PskDemod whole(o.rate, kSymbolRate);
    const auto oneCall = whole.process(samples);

    // Run 2: the same span in two calls, with a fresh demodulator.
    nadirstation::dsp::PskDemod split1(o.rate, kSymbolRate);
    std::vector<std::complex<float>> firstHalf(samples.begin(), samples.begin() + split);
    std::vector<std::complex<float>> secondHalf(samples.begin() + split, samples.end());
    const auto partA = split1.process(firstHalf);
    const auto partB = split1.process(secondHalf);

    std::vector<std::complex<float>> twoCalls = partA.symbols;
    twoCalls.insert(twoCalls.end(), partB.symbols.begin(), partB.symbols.end());

    std::cout << "single call : " << oneCall.symbols.size() << " symbols\n";
    std::cout << "two calls   : " << partA.symbols.size() << " + " << partB.symbols.size()
              << " = " << twoCalls.size() << " symbols\n";
    std::cout << "split point : sample " << split << " of " << samples.size() << "\n\n";

    if (oneCall.symbols.size() != twoCalls.size())
        std::cout << "NOTE: the two runs produced different numbers of symbols, so the seam "
                     "changes how many symbols are emitted as well as which\n\n";

    const std::size_t n = std::min(oneCall.symbols.size(), twoCalls.size());
    std::size_t firstDiff = n;
    for (std::size_t i = 0; i < n; ++i) {
        if (oneCall.symbols[i] != twoCalls[i]) { firstDiff = i; break; }
    }

    // Where in the sample stream does that symbol fall?
    const double samplesPerSymbol = o.rate / kSymbolRate;
    const double sampleOfDiff = firstDiff * samplesPerSymbol;
    const double sampleOfSplit = static_cast<double>(split);

    if (firstDiff == n) {
        std::cout << "the first " << n << " symbols are identical: the seam changes nothing "
                     "measurable in this span\n";
    } else {
        std::cout << "first differing symbol: index " << firstDiff << "\n";
        std::cout << "  at about sample " << static_cast<long long>(sampleOfDiff)
                  << ", and the split was at sample " << static_cast<long long>(sampleOfSplit)
                  << "\n";
        const double delta = sampleOfDiff - sampleOfSplit;
        std::cout << "  difference of " << static_cast<long long>(delta) << " samples ("
                  << std::fixed << std::setprecision(1) << delta / samplesPerSymbol
                  << " symbols)\n";
        if (std::fabs(delta) < 4 * samplesPerSymbol)
            std::cout << "\n  the divergence begins at the seam, so the seam is the cause\n";
        else
            std::cout << "\n  the divergence is far from the seam, so the seam is not the direct "
                         "cause -- the two runs drifted apart earlier\n";
    }

    // ---- the layers above the demodulator -------------------------------------------------
    //
    // The symbols agree, so any difference in the frame count must come from here. Each stream is
    // pushed through its own LritDecoder exactly as the streaming tool does it: one feed per block,
    // then drain the frames that became ready.
    {
        nadirstation::gk2a::LritDecoder wholeDec;
        wholeDec.feedSoftBits(floatsOf(oneCall.symbols, o.polarity), true);
        const std::size_t wholeCadus = wholeDec.takeCadus().size();
        const auto wholeStats = wholeDec.stats();

        nadirstation::gk2a::LritDecoder splitDec;
        splitDec.feedSoftBits(floatsOf(partA.symbols, o.polarity), false);
        const auto drainedA = splitDec.takeCadus();
        splitDec.feedSoftBits(floatsOf(partB.symbols, o.polarity), true);
        const auto drainedB = splitDec.takeCadus();
        const auto splitStats = splitDec.stats();

        std::cout << "\n=== above the demodulator ===\n";
        std::cout << "  one call : bits in " << wholeStats.bitsIn << ", CADUs "
                  << wholeCadus << " (sync " << wholeStats.cadusWithSync
                  << ", parity " << wholeStats.cadusWithGoodRs << ")\n";
        std::cout << "  two calls: bits in " << splitStats.bitsIn << ", CADUs "
                  << (drainedA.size() + drainedB.size()) << " (sync " << splitStats.cadusWithSync
                  << ", parity " << splitStats.cadusWithGoodRs << ")\n";
        // Which half is deficient matters: if the first call carries its weight and the second
        // does not, the state is not reaching the second call, which is a different fault from
        // window alignment drifting.
        std::cout << "    first call  " << partA.symbols.size() << " symbols -> "
                  << drainedA.size() << " CADUs\n";
        std::cout << "    second call " << partB.symbols.size() << " symbols -> "
                  << drainedB.size() << " CADUs\n";
        // The window length the Viterbi uses, so the alignment of the seam is checkable.
        std::cout << "    seam at symbol " << partA.symbols.size() << "; a multiple of 256: "
                  << ((partA.symbols.size() % 256 == 0) ? "yes" : "no") << "\n";

        if (wholeCadus == 0 && drainedA.empty() && drainedB.empty()) {
            std::cout << "\n  NEITHER RUN FOUND A FRAME, so this comparison is empty. Widen the "
                         "span, or check the polarity: this service needs the symbol spectrum "
                         "inverted, which is the default here.\n";
        } else if (wholeCadus == drainedA.size() + drainedB.size()) {
            std::cout << "\n  the frame count agrees, so at this span the split changes nothing "
                         "above the demodulator either\n";
            std::cout << "  a full run disagrees, so the difference needs a longer span or a "
                         "different split point to appear\n";
        } else {
            std::cout << "\n  THE FRAME COUNT DIFFERS: one call finds " << wholeCadus
                      << ", two calls find " << (drainedA.size() + drainedB.size()) << "\n";
            std::cout << "  the fault is at or above the Viterbi decoder, not in the "
                         "demodulator\n";
        }
    }

    // A symbol-level look at the seam, in case the streams are equal but shifted.
    if (firstDiff > 0 && firstDiff < n) {
        std::cout << "\n  around the first difference:\n";
        const std::size_t from = firstDiff > 3 ? firstDiff - 3 : 0;
        for (std::size_t i = from; i < std::min(firstDiff + 4, n); ++i) {
            std::cout << "    [" << i << "]  single " << std::setw(10) << oneCall.symbols[i].real()
                      << std::setw(10) << oneCall.symbols[i].imag()
                      << "     two " << std::setw(10) << twoCalls[i].real()
                      << std::setw(10) << twoCalls[i].imag()
                      << (i == firstDiff ? "   <- first difference" : "") << "\n";
        }
    }
    return 0;
}
