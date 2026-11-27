// Locate the discrepancy between the two self tests: same decoder, same length, different result.
//
// The identity test round-trips a deterministic 2000-bit pattern and gets 2 wrong bits, while the
// demodulator's self test round-trips a random 4000-bit pattern and gets 1974 -- essentially
// random. Both call ViterbiDecoder::encode and ViterbiDecoder::decode. Something differs besides
// the length, and guessing which is wasteful, so this prints the error positions and the error
// pattern for several input sources at a fixed length.
//
// Usage: NadirStation_GK2ADiag

#include "../src/gk2adecoder.h"

#include <cstdint>
#include <iostream>
#include <random>
#include <vector>

namespace {

struct Outcome {
    std::size_t errors = 0;
    std::size_t firstError = 0;
    std::size_t lastError = 0;
    int onesIn = 0;
    int onesOut = 0;
    std::vector<std::size_t> firstErrors;
};

Outcome roundTrip(const std::vector<std::uint8_t>& bits, bool addNoise, double sigma,
                  std::uint32_t seed) {
    using namespace nadirstation::gk2a;
    Outcome o;
    const std::vector<std::uint8_t> enc = ViterbiDecoder::encode(bits);
    std::vector<float> soft(enc.size());
    std::mt19937 rng(seed);
    std::normal_distribution<double> dist(0.0, sigma);
    for (std::size_t i = 0; i < enc.size(); ++i) {
        const double base = enc[i] ? -1.0 : 1.0;      // convention A: a 0 arrives positive
        soft[i] = static_cast<float>(addNoise ? base + dist(rng) : base);
    }
    ViterbiDecoder vit;
    const std::vector<std::uint8_t> out = vit.decode(soft);
    const std::size_t n = std::min(out.size(), bits.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (bits[i]) ++o.onesIn;
        if (out[i]) ++o.onesOut;
        if (out[i] != bits[i]) {
            if (o.errors == 0) o.firstError = i;
            o.lastError = i;
            if (o.firstErrors.size() < 24) o.firstErrors.push_back(i);
            ++o.errors;
        }
    }
    return o;
}

void report(const char* label, const Outcome& o, std::size_t len) {
    std::cout << "  " << label << "\n"
              << "    errors " << o.errors << " of " << len
              << "  (" << (len ? 100.0 * o.errors / len : 0.0) << " %)\n"
              << "    ones in " << o.onesIn << ", ones out " << o.onesOut << "\n";
    if (o.errors) {
        std::cout << "    first error at bit " << o.firstError << ", last at " << o.lastError
                  << "\n    first error positions:";
        for (const auto p : o.firstErrors) std::cout << " " << p;
        std::cout << "\n";
    }
}

std::vector<std::uint8_t> pattern(std::size_t n, int kind, std::uint32_t seed) {
    std::vector<std::uint8_t> v(n);
    std::mt19937 rng(seed);
    for (std::size_t i = 0; i < n; ++i) {
        switch (kind) {
        case 0: v[i] = static_cast<std::uint8_t>((i * 7 + 3) & 1u); break;         // deterministic
        case 1: v[i] = static_cast<std::uint8_t>(rng() & 1u); break;               // mt19937 low
        case 2: v[i] = static_cast<std::uint8_t>((rng() >> 31) & 1u); break;       // mt19937 high
        case 3: v[i] = static_cast<std::uint8_t>(i & 1u); break;                   // alternating
        default: v[i] = 0; break;                                                  // all zeros
        }
    }
    return v;
}

}  // namespace

int main() {
    std::cout << "=== the same decoder, several input sources, 4000 bits ===\n";
    for (int kind = 0; kind <= 4; ++kind) {
        const auto bits = pattern(4000, kind, 12345);
        const Outcome o = roundTrip(bits, false, 0.0, 1);
        static const char* names[] = {"deterministic (i*7+3)&1", "random, mt19937 low bit",
                                      "random, mt19937 high bit", "alternating 0101",
                                      "all zeros"};
        report(names[kind], o, bits.size());
    }

    std::cout << "\n=== same source, increasing length (deterministic pattern) ===\n";
    for (const std::size_t n : {100u, 500u, 1000u, 2000u, 3000u, 4000u, 8000u}) {
        const auto bits = pattern(n, 0, 1);
        const Outcome o = roundTrip(bits, false, 0.0, 1);
        std::cout << "  length " << n << ": errors " << o.errors
                  << (o.errors ? "  first at " : "");
        if (o.errors) std::cout << o.firstError << " last " << o.lastError;
        std::cout << "\n";
    }

    std::cout << "\n=== random source, increasing length ===\n";
    for (const std::size_t n : {100u, 500u, 1000u, 2000u, 3000u, 4000u, 8000u}) {
        const auto bits = pattern(n, 1, 12345);
        const Outcome o = roundTrip(bits, false, 0.0, 1);
        std::cout << "  length " << n << ": errors " << o.errors
                  << (o.errors ? "  first at " : "");
        if (o.errors) std::cout << o.firstError << " last " << o.lastError;
        std::cout << "\n";
    }

    // Hard-decision round trip: map each encoded symbol straight to a bit with no soft values
    // involved. This isolates the trellis from the soft-metric arithmetic, so a failure here is a
    // structural fault and a success narrows the problem to the metrics.
    std::cout << "\n=== hard decision, no soft values ===\n";
    {
        using namespace nadirstation::gk2a;
        for (int kind = 0; kind <= 3; ++kind) {
            const auto bits = pattern(4000, kind, 12345);
            const auto enc = ViterbiDecoder::encode(bits);
            std::vector<float> soft(enc.size());
            for (std::size_t i = 0; i < enc.size(); ++i) soft[i] = enc[i] ? -1.0f : 1.0f;
            ViterbiDecoder vit;
            const auto out = vit.decode(soft);
            std::size_t errors = 0;
            for (std::size_t i = 0; i < bits.size() && i < out.size(); ++i)
                if (out[i] != bits[i]) ++errors;
            static const char* nm[] = {"deterministic", "random low", "random high", "alternating"};
            std::cout << "  " << nm[kind] << ": errors " << errors << " of " << bits.size()
                      << "\n";
        }
    }

    std::cout << "\n=== with noise, deterministic 2000 bits ===\n";
    for (const double sigma : {0.0, 0.1, 0.3, 0.5, 0.8}) {
        const auto bits = pattern(2000, 0, 1);
        const Outcome o = roundTrip(bits, true, sigma, 7);
        std::cout << "  sigma " << sigma << ": errors " << o.errors << " of " << bits.size()
                  << "\n";
    }
    return 0;
}
