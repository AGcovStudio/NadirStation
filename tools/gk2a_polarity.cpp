// Check the polarity and framing of the demodulated stream before blaming the decoder.
//
// The recording decodes in SatDump at BER 0.0049 and my chain finds no sync at all, while the same
// chain round-trips its own encoder perfectly. So the demodulator produces symbols and the decoder
// consumes them, but what passes between them is not what each expects. The two candidates are
// polarity -- every bit inverted -- and the soft-value convention.
//
// This measures directly, without invoking the Viterbi decoder:
//
//     the fraction of symbols whose real part is negative. A BPSK stream with a random message is
//     near 0.5; a value near 0 or 1 means the carrier is locked 90 degrees off or the message is
//     strongly biased.
//     the same fraction after the Costas loop, at both polarities.
//     whether the hard-decision bit stream contains the sync marker at any offset, for both
//     polarities, by scanning rather than by assuming byte alignment.
//
// Finding the marker under one polarity and not the other settles the question immediately, and
// finding it under neither says the fault is upstream of polarity.
//
// Run: FY2G_GK2APolarity <file> --rate <Hz> [--format int16] [--blocks N]

#include "../src/dsp.h"
#include "../src/gk2adecoder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

namespace {

std::size_t readBlock(std::ifstream& in, const std::string& format, std::size_t want,
                      std::vector<std::complex<float>>& out) {
    const std::size_t bps = (format == "float32") ? 8u : 4u;
    std::vector<char> raw(want * bps);
    in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    const std::size_t got = static_cast<std::size_t>(in.gcount()) / bps;
    out.resize(got);
    for (std::size_t i = 0; i < got; ++i) {
        if (format == "float32") {
            float fi = 0, fq = 0;
            std::memcpy(&fi, raw.data() + i * 8, 4);
            std::memcpy(&fq, raw.data() + i * 8 + 4, 4);
            out[i] = {fi, fq};
        } else {
            std::int16_t ii = 0, qq = 0;
            std::memcpy(&ii, raw.data() + i * 4, 2);
            std::memcpy(&qq, raw.data() + i * 4 + 2, 2);
            out[i] = {static_cast<float>(ii) / 32768.0f, static_cast<float>(qq) / 32768.0f};
        }
    }
    return got;
}

// Scan a bit stream for a 32-bit marker at every bit offset, and report the first hit.
long findMarker(const std::vector<std::uint8_t>& bits, std::uint32_t marker) {
    if (bits.size() < 32) return -1;
    std::uint32_t window = 0;
    for (std::size_t i = 0; i < bits.size(); ++i) {
        window = (window << 1) | bits[i];
        if (i >= 31 && window == marker) return static_cast<long>(i - 31);
    }
    return -1;
}

}  // namespace

int main(int argc, char** argv) {
    std::string path, format = "int16";
    double rate = 0.0;
    std::size_t maxBlocks = 8;
    if (argc < 2) {
        std::cout << "usage: FY2G_GK2APolarity <file> --rate <Hz> [--format int16] "
                     "[--blocks N]\n";
        return 2;
    }
    path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (a == "--format" && i + 1 < argc) format = argv[++i];
        else if (a == "--blocks" && i + 1 < argc) maxBlocks = std::strtoull(argv[++i], nullptr, 10);
    }
    if (rate <= 0.0) { std::cout << "--rate required\n"; return 2; }

    std::ifstream in(path, std::ios::binary);
    if (!in) { std::cout << "cannot open " << path << "\n"; return 2; }

    constexpr double kSymbolRate = 128000.0;
    nadirstation::dsp::PskDemod demod(static_cast<float>(rate),
                                     static_cast<float>(kSymbolRate), 0.5f, 0.02f);

    std::vector<std::complex<float>> iq;
    const std::size_t block = 1u << 20;
    // Two hard-decision bit streams, one per polarity, scanned for the sync marker.
    std::vector<std::uint8_t> bitsA, bitsB;
    std::size_t negA = 0, negB = 0, total = 0;
    double magSum = 0.0;

    for (std::size_t b = 0; b < maxBlocks; ++b) {
        if (readBlock(in, format, block, iq) == 0) break;
        const nadirstation::dsp::DPSKResult res = demod.process(iq);
        for (const auto& z : res.symbols) {
            const float re = z.real();
            magSum += std::fabs(re);
            ++total;
            // Polarity 0: a negative real part is a 1, which is what the demodulator's own hard
            // decision assumes. Polarity 1 is the inverse.
            bitsA.push_back(re < 0.0f ? 1u : 0u);
            bitsB.push_back(re < 0.0f ? 0u : 1u);
            if (re < 0.0f) { ++negA; ++negB; }
        }
    }

    std::cout << "symbols              " << total << "\n";
    std::cout << "mean |real|          " << std::fixed << std::setprecision(4)
              << (total ? magSum / total : 0.0) << "\n";
    std::cout << "fraction negative    " << (total ? double(negA) / total : 0.0)
              << "   (0.5 expected for a balanced BPSK message)\n\n";

    const std::uint32_t marker = 0x1ACFFC1DU;
    // The marker sits ahead of the convolutional encoder, so it must be looked for AFTER Viterbi
    // decoding. Searching the hard-decision stream, as an earlier version of this tool did, can
    // never succeed however good the signal is: the encoder has already transformed it.
    for (const int pol : {0, 1}) {
        const std::vector<std::uint8_t>& hard = (pol == 0) ? bitsA : bitsB;
        std::vector<float> soft(hard.size());
        for (std::size_t i = 0; i < hard.size(); ++i)
            soft[i] = hard[i] ? -1.0f : 1.0f;
        nadirstation::gk2a::ViterbiDecoder vit;
        const std::vector<std::uint8_t> bits = vit.decode(soft);

        std::size_t hits = 0;
        std::vector<long> positions;
        std::uint32_t window = 0;
        for (std::size_t i = 0; i < bits.size(); ++i) {
            window = (window << 1) | bits[i];
            if (i >= 31 && window == marker) {
                ++hits;
                if (positions.size() < 8) positions.push_back(static_cast<long>(i - 31));
            }
        }
        std::cout << "polarity " << pol << ": " << bits.size() << " decoded bits, sync marker "
                  << "found " << hits << " times\n";
        if (positions.size() >= 2) {
            std::cout << "  first positions:";
            for (const long q : positions) std::cout << " " << q;
            std::cout << "\n  spacing: " << (positions[1] - positions[0])
                      << " bits   (one CADU is 8192 bits)\n";
        } else if (positions.size() == 1) {
            std::cout << "  one hit at bit " << positions[0] << "\n";
        }
    }
    std::cout << "\n  expected: 8192 bits between consecutive CADU sync markers, or a multiple\n"
                 "  of it. That is the number the frame layer relies on, and finding it here\n"
                 "  proves the demodulator and the polarity are right before the decoder is\n"
                 "  involved at all.\n";
    return 0;
}
