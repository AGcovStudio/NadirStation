// Minimal identity test for the Viterbi chain, to locate a failure precisely.
//
// The full self test reports BER 0.4935 at every noise level including zero, which means the
// decoder is not corrupted by noise but is producing essentially random bits -- so the fault is a
// convention mismatch between the encoder and the decoder, not a weak algorithm. This prints the
// first few symbols of a short known input alongside the metric signs the decoder computes for
// them, which shows which convention is in force rather than leaving it to be inferred.
//
// Usage: NadirStation_GK2ATest

#include "../src/gk2adecoder.h"

#include <cstdint>
#include <iostream>
#include <vector>

int main() {
    using namespace nadirstation::gk2a;

    std::cout << "=== encoder output for a known input ===\n";
    const std::vector<std::uint8_t> input = {1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0};
    const std::vector<std::uint8_t> encoded = ViterbiDecoder::encode(input);
    std::cout << "  input   :";
    for (const auto b : input) std::cout << " " << int(b);
    std::cout << "\n  encoded :";
    for (const auto b : encoded) std::cout << " " << int(b);
    std::cout << "\n  (pairs are (G1, G2) per input bit, two symbols each)\n";

    // Convention A: a transmitted 0 arrives as a POSITIVE soft value.
    // Convention B: a transmitted 0 arrives as a NEGATIVE soft value.
    // The decoder header documents A; this checks which one it actually implements.
    for (const int convention : {0, 1}) {
        std::vector<float> soft(encoded.size());
        for (std::size_t i = 0; i < encoded.size(); ++i) {
            const float positive = encoded[i] ? -1.0f : 1.0f;   // convention A
            soft[i] = convention == 0 ? positive : -positive;
        }
        ViterbiDecoder vit;
        const std::vector<std::uint8_t> out = vit.decode(soft);
        std::size_t errors = 0;
        for (std::size_t i = 0; i < input.size() && i < out.size(); ++i)
            if (out[i] != input[i]) ++errors;
        std::cout << "\n  convention " << (convention == 0 ? "A (0 -> +1)" : "B (0 -> -1)")
                  << ": decoded";
        for (std::size_t i = 0; i < out.size() && i < 16; ++i) std::cout << " " << int(out[i]);
        std::cout << "\n    errors " << errors << " of " << input.size() << "\n";
    }

    // Longer input, so the traceback has time to converge: with 12 bits the survivor path may not
    // have converged at all, which would confound the diagnosis above.
    std::cout << "\n=== longer input, 2000 bits ===\n";
    std::vector<std::uint8_t> big(2000);
    for (std::size_t i = 0; i < big.size(); ++i) big[i] = static_cast<std::uint8_t>((i * 7 + 3) & 1u);
    const std::vector<std::uint8_t> enc2 = ViterbiDecoder::encode(big);
    for (const int convention : {0, 1}) {
        std::vector<float> soft(enc2.size());
        for (std::size_t i = 0; i < enc2.size(); ++i) {
            const float positive = enc2[i] ? -1.0f : 1.0f;
            soft[i] = convention == 0 ? positive : -positive;
        }
        ViterbiDecoder vit;
        const std::vector<std::uint8_t> out = vit.decode(soft);
        std::size_t errors = 0;
        for (std::size_t i = 0; i < big.size() && i < out.size(); ++i)
            if (out[i] != big[i]) ++errors;
        std::cout << "  convention " << (convention == 0 ? "A" : "B") << ": errors " << errors
                  << " of " << big.size() << "\n";
    }
    return 0;
}
