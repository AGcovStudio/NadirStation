// Trace the convolutional round trip at a size small enough to check by hand.
//
// Every attempt so far has been tested on thousands of bits, where the output is a number and the
// cause is hidden. This prints, for a handful of inputs, the encoder's symbols, the decoder's
// bits, and the state the encoder was in at each step, so the first place the two disagree is
// visible. The encoder trace is computed here independently of ViterbiDecoder::encode, so the
// class is checked against a hand trace rather than against itself.
//
// Usage: NadirStation_GK2ATrace

#include "../src/gk2adecoder.h"

#include <cstdint>
#include <iostream>
#include <vector>

namespace {

int popcount8(std::uint8_t v) {
    int n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

int outputFor(int reg, std::uint8_t g1, std::uint8_t g2) {
    const int b0 = popcount8(static_cast<std::uint8_t>(reg & g1)) & 1;
    const int b1 = popcount8(static_cast<std::uint8_t>(reg & g2)) & 1;
    return b0 | (b1 << 1);
}

}  // namespace

int main() {
    using namespace nadirstation::gk2a;

    std::cout << "K = " << kConvolutionK << ", states = " << kStates
              << ", register mask = 0x" << std::hex << kRegisterMask << std::dec << "\n";
    std::cout << "G1 = 0x" << std::hex << int(kG1) << ", G2 = 0x" << int(kG2) << std::dec
              << "\n\n";

    const std::vector<std::uint8_t> input = {1, 0, 1, 1, 0, 0, 1, 0};
    std::cout << "=== encoder trace, computed independently ===\n";
    int state = 0;
    std::vector<int> expected;
    for (std::size_t i = 0; i < input.size(); ++i) {
        state = ((state << 1) | input[i]) & kRegisterMask;
        const int o = outputFor(state, kG1, kG2);
        expected.push_back(o & 1);
        expected.push_back((o >> 1) & 1);
        std::cout << "  in " << int(input[i]) << " -> state " << state << " (0x" << std::hex
                  << state << std::dec << ")  out " << (o & 1) << " " << ((o >> 1) & 1) << "\n";
    }

    const std::vector<std::uint8_t> enc = ViterbiDecoder::encode(input);
    std::cout << "\nViterbiDecoder::encode produced " << enc.size()
              << " symbols, which includes the " << (kConvolutionK + 1) << "-bit flush\n";
    std::cout << "  class  :";
    for (std::size_t i = 0; i < enc.size() && i < 2 * input.size(); ++i)
        std::cout << " " << int(enc[i]);
    std::cout << "\n  by hand:";
    for (const int b : expected) std::cout << " " << b;
    bool same = enc.size() >= expected.size();
    for (std::size_t i = 0; i < expected.size() && same; ++i)
        if (enc[i] != expected[i]) same = false;
    std::cout << "\n  agree on the message part: "
              << (same ? "yes" : "NO -- the class disagrees with the hand trace") << "\n";

    std::cout << "\n=== decode the noiseless symbols ===\n";
    std::vector<float> soft(enc.size());
    for (std::size_t i = 0; i < enc.size(); ++i) soft[i] = enc[i] ? -1.0f : 1.0f;
    ViterbiDecoder vit;
    const std::vector<std::uint8_t> out = vit.decode(soft);
    std::cout << "  decoded " << out.size() << " bits:";
    for (std::size_t i = 0; i < out.size() && i < 16; ++i) std::cout << " " << int(out[i]);
    std::cout << "\n  input      " << input.size() << " bits:";
    for (const auto b : input) std::cout << " " << int(b);
    std::cout << "\n";
    std::size_t errors = 0;
    for (std::size_t i = 0; i < input.size() && i < out.size(); ++i)
        if (out[i] != input[i]) ++errors;
    std::cout << "  errors in the message part: " << errors << " of " << input.size() << "\n";

    std::cout << "\n=== single bit, where the answer is forced ===\n";
    for (const std::uint8_t b : {0, 1}) {
        const std::vector<std::uint8_t> one = {b};
        const auto e1 = ViterbiDecoder::encode(one);
        std::vector<float> s1(e1.size());
        for (std::size_t i = 0; i < e1.size(); ++i) s1[i] = e1[i] ? -1.0f : 1.0f;
        ViterbiDecoder v2;
        const auto o1 = v2.decode(s1);
        std::cout << "  input " << int(b) << " -> symbols";
        for (std::size_t i = 0; i < e1.size() && i < 4; ++i) std::cout << " " << int(e1[i]);
        std::cout << " -> first decoded bit " << (o1.empty() ? -1 : int(o1[0]))
                  << (o1.empty() || o1[0] != b ? "   MISMATCH" : "   ok") << "\n";
    }
    return 0;
}
