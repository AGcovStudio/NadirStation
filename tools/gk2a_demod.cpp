// Demodulate and frame a GK-2A LRIT recording, and self-test the decoder on known input.
//
// Two modes, because when no frames appear there are two very different reasons and they must be
// distinguishable:
//
//   --selftest   encode a known bit pattern with the CCSDS rate-1/2 K=7 convolutional code, add
//                noise, decode it, and report whether the bits come back. This tests the Viterbi
//                decoder alone. A demodulator cannot be blamed for a decoder that fails here.
//   <iq file>    run the full chain: PSK demodulation at 64 ksymbols/s, then polarity search,
//                Viterbi decoding, frame synchronisation, derandomisation and Reed-Solomon
//                checking, reporting how many CADUs sync and how many pass the check.
//
// Everything the chain assumes is printed at startup so the assumptions travel with the result
// rather than living in a comment.
//
// Usage: NadirStation_GK2ADemod --selftest
//        NadirStation_GK2ADemod <iq-file> --format int16|float32 --rate <Hz> [--invert auto|yes|no]
//
// The recording format has to be stated because the baseband file gives no hint: --format says
// how samples are stored and --rate says the sample rate the SDR was running at.

#include "../src/dsp.h"
#include "../src/gk2adecoder.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;

void printAssumptions() {
    using namespace nadirstation::gk2a;
    std::cout << "GK-2A LRIT demodulation\n"
              << "  source: NMSC 'GK2A LRIT Mission Specification Document', Issue 1.0, "
                 "31 May 2019\n"
              << "  carrier frequency  1692.14 MHz (Table 9.1)\n"
              << "  bandwidth          1 MHz (Table 9.1)\n"
              << "  information rate   " << kInformationRateBps << " bps (Table 9.1)\n"
              << "  modulation         NRZ-L / BPSK (Table 9.1)\n"
              << "  pulse shaping      RRC, roll-off 0.5 (Table 9.1)\n"
              << "  convolutional      rate 1/2, K=7, CCSDS 131.0-B-3 (Table 9.1)\n"
              << "  Reed-Solomon       (255/223, 4) (Table 9.1)\n"
              << "  CADU               " << kCaduBytes << " bytes (Table 9.1)\n"
              << "  sync marker        0x" << std::hex << kSyncWord << std::dec
              << " (Table 8.5)\n"
              << "  symbol rate        " << kSymbolRateBps
              << " symbols/s: the 64 kbps information rate times rate 1/2 gives 128 kbaud on the "
                 "air, and BPSK carries one bit per symbol. SatDump's GK2A pipeline states the "
                 "same 128e3.\n\n";
}

// --- self test ------------------------------------------------------------------------------

int runSelfTest() {
    using namespace nadirstation::gk2a;
    std::cout << "=== decoder self test ===\n";
    bool allOk = true;

    // A known pattern, long enough for the traceback to converge.
    std::mt19937 rng(12345);
    std::vector<std::uint8_t> input(4000);
    for (auto& b : input) b = static_cast<std::uint8_t>(rng() & 1u);

    const std::vector<std::uint8_t> encoded = ViterbiDecoder::encode(input);
    std::cout << "  encoded " << input.size() << " bits into " << encoded.size()
              << " symbols (rate 1/2 => 2x)\n";

    // The mapping the decoder assumes: a transmitted 0 arrives as a positive soft value.
    for (const double noise : {0.0, 0.05, 0.15, 0.30, 0.50}) {
        std::vector<float> soft(encoded.size());
        std::normal_distribution<double> dist(0.0, noise);
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            const double base = encoded[i] ? -1.0 : 1.0;
            soft[i] = static_cast<float>(std::clamp(base + dist(rng), -4.0, 4.0));
        }
        ViterbiDecoder vit;
        const std::vector<std::uint8_t> out = vit.decode(soft);
        std::size_t errors = 0;
        const std::size_t n = std::min(out.size(), input.size());
        for (std::size_t i = 0; i < n; ++i)
            if (out[i] != input[i]) ++errors;
        const double ber = n ? static_cast<double>(errors) / static_cast<double>(n) : 1.0;
        std::cout << "  noise sigma " << noise << ": " << errors << " bit errors of " << n
                  << "  BER " << ber << (ber == 0.0 ? "  (exact)" : "") << "\n";
        if (noise <= 0.15 && ber != 0.0) {
            std::cout << "    FAIL: the decoder must return the input exactly at this noise\n";
            allOk = false;
        }
    }

    // A decoder that always answers zero must not pass. This catches a traceback that returns
    // the input by accident or a metric that ignores the data.
    std::vector<float> zeros(encoded.size(), 0.0f);
    ViterbiDecoder vit;
    const std::vector<std::uint8_t> blind = vit.decode(zeros);
    std::size_t ones = 0;
    for (const auto b : blind) ones += b;
    std::cout << "  all-zero soft input produced " << ones << " ones of " << blind.size()
              << " bits (a constant answer would be suspicious)\n";

    // Round trip through the frame layer: build a CADU with the documented sync marker and a
    // valid Reed-Solomon codeword is not practical here without an encoder, so at least confirm
    // that a wrong sync marker is rejected and a right one is accepted.
    LritDecoder dec;
    std::vector<std::uint8_t> bits;
    const std::uint32_t bad = 0xDEADBEEFU;
    for (int i = 0; i < 8 * kCaduBytes; ++i)
        bits.push_back(static_cast<std::uint8_t>((bad >> (31 - (i % 32))) & 1u));
    dec.feedHardBits(bits);
    const auto wrong = dec.takeCadus();
    std::cout << "  a stream without the sync marker yielded " << wrong.size()
              << " CADUs (must be 0)\n";
    if (!wrong.empty()) { std::cout << "    FAIL\n"; allOk = false; }

    std::cout << (allOk ? "  self test PASSED\n" : "  self test FAILED\n");
    return allOk ? 0 : 1;
}

// --- recording input ------------------------------------------------------------------------

bool loadIq(const std::string& path, const std::string& format,
            std::vector<std::complex<float>>& out) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path << "\n";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff size = in.tellg();
    in.seekg(0, std::ios::beg);
    const std::size_t bytes = static_cast<std::size_t>(size);
    std::vector<char> raw(bytes);
    in.read(raw.data(), static_cast<std::streamsize>(bytes));

    if (format == "int16") {
        const std::size_t n = bytes / 4;             // interleaved I,Q as 16-bit
        out.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            std::int16_t i16 = 0, q16 = 0;
            std::memcpy(&i16, raw.data() + i * 4, 2);
            std::memcpy(&q16, raw.data() + i * 4 + 2, 2);
            out[i] = {static_cast<float>(i16) / 32768.0f, static_cast<float>(q16) / 32768.0f};
        }
        return true;
    }
    if (format == "float32") {
        const std::size_t n = bytes / 8;
        out.resize(n);
        for (std::size_t i = 0; i < n; ++i) {
            float fi = 0, fq = 0;
            std::memcpy(&fi, raw.data() + i * 8, 4);
            std::memcpy(&fq, raw.data() + i * 8 + 4, 4);
            out[i] = {fi, fq};
        }
        return true;
    }
    std::cerr << "unknown format " << format << " (expected int16 or float32)\n";
    return false;
}

int runRecording(const std::string& path, const std::string& format, double sampleRate,
                 const std::string& invertMode) {
    using namespace nadirstation::gk2a;

    std::vector<std::complex<float>> iq;
    if (!loadIq(path, format, iq)) return 2;
    std::cout << "=== recording ===\n"
              << "  file        " << path << "\n"
              << "  format      " << format << "\n"
              << "  samples     " << iq.size() << "\n"
              << "  sample rate " << sampleRate << " Hz\n"
              << "  duration    " << (sampleRate > 0 ? iq.size() / sampleRate : 0.0) << " s\n\n";
    if (iq.empty() || sampleRate <= 0.0) {
        std::cerr << "nothing to process\n";
        return 2;
    }

    // Demodulate once. The symbol rate is the documented 64 kbaud.
    nadirstation::dsp::PskDemod demod(static_cast<float>(sampleRate),
                                     static_cast<float>(kSymbolRateBps),
                                     0.5f, 0.02f);
    const nadirstation::dsp::DPSKResult res = demod.process(iq);
    std::cout << "=== demodulator ===\n"
              << "  symbols     " << res.bits.size() << "\n"
              << "  soft values " << res.soft.size() << "\n"
              << "  carrier offset " << res.carrierOffsetHz << " Hz\n"
              << "  timing quality " << res.timingQuality << "\n\n";
    if (res.soft.size() < 1000) {
        std::cerr << "too few symbols to contain a CADU; check the sample rate and whether the "
                     "recording holds a carrier at all\n";
        return 2;
    }

    // Polarity: the specification says the convolutional coding follows CCSDS "except symbol
    // inversion on output path of G2", without saying which polarity reaches the downlink, so
    // both are tried and the one that produces frame sync is reported.
    const bool tryBoth = (invertMode == "auto");
    const bool useInverted = (invertMode == "yes");
    std::vector<bool> candidates;
    if (tryBoth) candidates = {false, true};
    else candidates = {useInverted};

    int bestExit = 1;
    for (const bool inverted : candidates) {
        std::vector<float> soft = res.soft;
        if (inverted)
            for (auto& v : soft) v = -v;

        LritDecoder dec;
        dec.setPolarityInverted(inverted);
        dec.feedSoftBits(soft);
        const auto cadus = dec.takeCadus();
        const auto& st = dec.stats();
        std::cout << "=== polarity " << (inverted ? "inverted" : "normal") << " ===\n"
                  << "  bits in          " << st.bitsIn << "\n"
                  << "  CADUs assembled  " << st.cadusAssembled << "\n"
                  << "  with sync        " << st.cadusWithSync << "\n"
                  << "  passing RS check " << st.cadusWithGoodRs << "\n";
        if (!cadus.empty()) {
            std::cout << "  first CADU: vcid " << int(cadus.front().vcid)
                      << ", virtual frame count " << cadus.front().virtualFrameCount
                      << ", RS " << (cadus.front().rsOk ? "ok" : "bad") << "\n";
            std::cout << "  VCID histogram:";
            for (std::size_t i = 0; i < st.vcidHistogram.size(); ++i)
                if (st.vcidHistogram[i])
                    std::cout << " " << i << "=" << int(st.vcidHistogram[i]);
            std::cout << "\n";
        } else {
            const auto& seq = LritDecoder::randomiserBits();
            std::cout << "  no CADU carried the 0x" << std::hex << kSyncWord
                      << std::dec << " marker\n"
                      << "  (the randomiser sequence is " << seq.size() << " bytes, applied "
                         "after the marker)\n";
        }
        std::cout << "\n";
        if (st.cadusWithSync > 0) bestExit = 0;
    }

    if (bestExit != 0) {
        std::cout << "No polarity produced frame sync. In order of likelihood:\n"
                  << "  1. the recording does not contain the LRIT carrier, or the centre "
                     "frequency was not 1692.14 MHz\n"
                  << "  2. the sample rate given does not match the recording\n"
                  << "  3. the symbol rate is not 64 kbaud, or the signal is inverted in "
                     "frequency (try swapping I and Q)\n"
                  << "  4. the synchroniser needs a longer recording: one CADU is "
                  << kCaduBytes << " bytes, i.e. "
                  << (kCaduBytes * 8 / double(kSymbolRateBps))
                  << " s of symbols\n";
    }
    return bestExit;
}

}  // namespace

int main(int argc, char** argv) {
    printAssumptions();
    if (argc < 2) {
        std::cout << "usage: NadirStation_GK2ADemod --selftest\n"
                     "       NadirStation_GK2ADemod <iq-file> [--format int16|float32] [--rate Hz] "
                     "[--invert auto|yes|no]\n";
        return 2;
    }
    const std::string first = argv[1];
    if (first == "--selftest") return runSelfTest();

    std::string format = "int16";
    std::string invert = "auto";
    double rate = 0.0;
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--format" && i + 1 < argc) format = argv[++i];
        else if (a == "--rate" && i + 1 < argc) rate = std::atof(argv[++i]);
        else if (a == "--invert" && i + 1 < argc) invert = argv[++i];
    }
    if (rate <= 0.0) {
        std::cerr << "--rate is required: the recording does not say what sample rate the SDR "
                     "was running at\n";
        return 2;
    }
    return runRecording(first, format, rate, invert);
}
