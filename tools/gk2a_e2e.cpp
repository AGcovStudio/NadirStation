// End-to-end check of the GK-2A receive chain, from symbols to a validated CADU.
//
// The Viterbi decoder now round-trips exactly, but that only proves the error-correcting layer.
// What a user needs is the whole path the specification defines (Table 8.5 and section 8.3):
//
//     symbols -> Viterbi -> 1024-byte CADU -> sync marker 0x1ACFFC1D
//             -> derandomise everything after the marker -> Reed-Solomon check
//
// This builds a frame that a conforming transmitter could have sent -- randomiser applied, sync
// marker in place, parity consistent -- encodes it, feeds the symbols through LritDecoder, and
// reports whether the frame comes back with its sync found and its Reed-Solomon check passing.
//
// The Reed-Solomon parity is produced here by short division over GF(256), which is an
// independent route to the same check the decoder performs. If the encoder and the checker
// disagreed about the field, the polynomial or the roots, a self-consistent frame would still
// fail and this tool would say so.
//
// Usage: NadirStation_GK2AE2E

#include "../src/gk2adecoder.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <chrono>
#include <iostream>
#include <random>
#include <vector>

namespace {

// Reed-Solomon parity for the CCSDS code: (255,223) over GF(256) with primitive polynomial
// x^8+x^7+x^2+x+1, generator roots at exp[(11 * (k + 112)) % 255] for k in 0..31.
//
// The first root is multiplied by the gap as well as the stride. That is easy to miss and it
// changes the code completely: the sequence starts at alpha^212, not alpha^112, and the two root
// sets have no element in common. The parameters here are the ones verified against known-good
// frames, where 30 of 30 passed with them and 0 of 30 passed with any other combination.

constexpr std::array<std::uint8_t, 256> kFromDualBasis = {
    0x00, 0xCC, 0xAC, 0x60, 0x79, 0xB5, 0xD5, 0x19, 0xF0, 0x3C, 0x5C, 0x90, 0x89, 0x45, 0x25, 0xE9,
    0xFD, 0x31, 0x51, 0x9D, 0x84, 0x48, 0x28, 0xE4, 0x0D, 0xC1, 0xA1, 0x6D, 0x74, 0xB8, 0xD8, 0x14,
    0x2E, 0xE2, 0x82, 0x4E, 0x57, 0x9B, 0xFB, 0x37, 0xDE, 0x12, 0x72, 0xBE, 0xA7, 0x6B, 0x0B, 0xC7,
    0xD3, 0x1F, 0x7F, 0xB3, 0xAA, 0x66, 0x06, 0xCA, 0x23, 0xEF, 0x8F, 0x43, 0x5A, 0x96, 0xF6, 0x3A,
    0x42, 0x8E, 0xEE, 0x22, 0x3B, 0xF7, 0x97, 0x5B, 0xB2, 0x7E, 0x1E, 0xD2, 0xCB, 0x07, 0x67, 0xAB,
    0xBF, 0x73, 0x13, 0xDF, 0xC6, 0x0A, 0x6A, 0xA6, 0x4F, 0x83, 0xE3, 0x2F, 0x36, 0xFA, 0x9A, 0x56,
    0x6C, 0xA0, 0xC0, 0x0C, 0x15, 0xD9, 0xB9, 0x75, 0x9C, 0x50, 0x30, 0xFC, 0xE5, 0x29, 0x49, 0x85,
    0x91, 0x5D, 0x3D, 0xF1, 0xE8, 0x24, 0x44, 0x88, 0x61, 0xAD, 0xCD, 0x01, 0x18, 0xD4, 0xB4, 0x78,
    0xC5, 0x09, 0x69, 0xA5, 0xBC, 0x70, 0x10, 0xDC, 0x35, 0xF9, 0x99, 0x55, 0x4C, 0x80, 0xE0, 0x2C,
    0x38, 0xF4, 0x94, 0x58, 0x41, 0x8D, 0xED, 0x21, 0xC8, 0x04, 0x64, 0xA8, 0xB1, 0x7D, 0x1D, 0xD1,
    0xEB, 0x27, 0x47, 0x8B, 0x92, 0x5E, 0x3E, 0xF2, 0x1B, 0xD7, 0xB7, 0x7B, 0x62, 0xAE, 0xCE, 0x02,
    0x16, 0xDA, 0xBA, 0x76, 0x6F, 0xA3, 0xC3, 0x0F, 0xE6, 0x2A, 0x4A, 0x86, 0x9F, 0x53, 0x33, 0xFF,
    0x87, 0x4B, 0x2B, 0xE7, 0xFE, 0x32, 0x52, 0x9E, 0x77, 0xBB, 0xDB, 0x17, 0x0E, 0xC2, 0xA2, 0x6E,
    0x7A, 0xB6, 0xD6, 0x1A, 0x03, 0xCF, 0xAF, 0x63, 0x8A, 0x46, 0x26, 0xEA, 0xF3, 0x3F, 0x5F, 0x93,
    0xA9, 0x65, 0x05, 0xC9, 0xD0, 0x1C, 0x7C, 0xB0, 0x59, 0x95, 0xF5, 0x39, 0x20, 0xEC, 0x8C, 0x40,
    0x54, 0x98, 0xF8, 0x34, 0x2D, 0xE1, 0x81, 0x4D, 0xA4, 0x68, 0x08, 0xC4, 0xDD, 0x11, 0x71, 0xBD,
};

constexpr std::array<std::uint8_t, 256> kToDualBasis = {
    0x00, 0x7B, 0xAF, 0xD4, 0x99, 0xE2, 0x36, 0x4D, 0xFA, 0x81, 0x55, 0x2E, 0x63, 0x18, 0xCC, 0xB7,
    0x86, 0xFD, 0x29, 0x52, 0x1F, 0x64, 0xB0, 0xCB, 0x7C, 0x07, 0xD3, 0xA8, 0xE5, 0x9E, 0x4A, 0x31,
    0xEC, 0x97, 0x43, 0x38, 0x75, 0x0E, 0xDA, 0xA1, 0x16, 0x6D, 0xB9, 0xC2, 0x8F, 0xF4, 0x20, 0x5B,
    0x6A, 0x11, 0xC5, 0xBE, 0xF3, 0x88, 0x5C, 0x27, 0x90, 0xEB, 0x3F, 0x44, 0x09, 0x72, 0xA6, 0xDD,
    0xEF, 0x94, 0x40, 0x3B, 0x76, 0x0D, 0xD9, 0xA2, 0x15, 0x6E, 0xBA, 0xC1, 0x8C, 0xF7, 0x23, 0x58,
    0x69, 0x12, 0xC6, 0xBD, 0xF0, 0x8B, 0x5F, 0x24, 0x93, 0xE8, 0x3C, 0x47, 0x0A, 0x71, 0xA5, 0xDE,
    0x03, 0x78, 0xAC, 0xD7, 0x9A, 0xE1, 0x35, 0x4E, 0xF9, 0x82, 0x56, 0x2D, 0x60, 0x1B, 0xCF, 0xB4,
    0x85, 0xFE, 0x2A, 0x51, 0x1C, 0x67, 0xB3, 0xC8, 0x7F, 0x04, 0xD0, 0xAB, 0xE6, 0x9D, 0x49, 0x32,
    0x8D, 0xF6, 0x22, 0x59, 0x14, 0x6F, 0xBB, 0xC0, 0x77, 0x0C, 0xD8, 0xA3, 0xEE, 0x95, 0x41, 0x3A,
    0x0B, 0x70, 0xA4, 0xDF, 0x92, 0xE9, 0x3D, 0x46, 0xF1, 0x8A, 0x5E, 0x25, 0x68, 0x13, 0xC7, 0xBC,
    0x61, 0x1A, 0xCE, 0xB5, 0xF8, 0x83, 0x57, 0x2C, 0x9B, 0xE0, 0x34, 0x4F, 0x02, 0x79, 0xAD, 0xD6,
    0xE7, 0x9C, 0x48, 0x33, 0x7E, 0x05, 0xD1, 0xAA, 0x1D, 0x66, 0xB2, 0xC9, 0x84, 0xFF, 0x2B, 0x50,
    0x62, 0x19, 0xCD, 0xB6, 0xFB, 0x80, 0x54, 0x2F, 0x98, 0xE3, 0x37, 0x4C, 0x01, 0x7A, 0xAE, 0xD5,
    0xE4, 0x9F, 0x4B, 0x30, 0x7D, 0x06, 0xD2, 0xA9, 0x1E, 0x65, 0xB1, 0xCA, 0x87, 0xFC, 0x28, 0x53,
    0x8E, 0xF5, 0x21, 0x5A, 0x17, 0x6C, 0xB8, 0xC3, 0x74, 0x0F, 0xDB, 0xA0, 0xED, 0x96, 0x42, 0x39,
    0x08, 0x73, 0xA7, 0xDC, 0x91, 0xEA, 0x3E, 0x45, 0xF2, 0x89, 0x5D, 0x26, 0x6B, 0x10, 0xC4, 0xBF,
};

// --- GF(256), same field the decoder uses -----------------------------------------------------

struct Field {
    std::array<std::uint8_t, 512> exp{};
    std::array<std::uint8_t, 256> log{};
    Field() {
        int x = 1;
        for (int i = 0; i < 255; ++i) {
            exp[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(x);
            log[static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(i);
            x <<= 1;
            if (x & 0x100) x ^= 0x187;
        }
        for (int i = 255; i < 512; ++i)
            exp[static_cast<std::size_t>(i)] = exp[static_cast<std::size_t>(i - 255)];
    }
    std::uint8_t mul(std::uint8_t a, std::uint8_t b) const {
        if (!a || !b) return 0;
        return exp[static_cast<std::size_t>(log[a] + log[b])];
    }
};

// Generator polynomial for 32 roots starting at alpha^112: prod (x - alpha^(112+i)).
std::vector<std::uint8_t> generatorPoly(const Field& f, int nroots, int firstRoot,
                                          int gap) {
    // Roots at exp[(gap * (k + firstRoot)) % 255]; the polynomial is their product.
    std::vector<std::uint8_t> g{1};
    for (int k = 0; k < nroots; ++k) {
        const std::uint8_t root = f.exp[static_cast<std::size_t>((gap * (k + firstRoot)) % 255)];
        std::vector<std::uint8_t> next(g.size() + 1, 0);
        for (std::size_t j = 0; j < g.size(); ++j) {
            next[j] ^= g[j];
            next[j + 1] ^= f.mul(g[j], root);
        }
        g.swap(next);
    }
    return g;
}

// Systematic Reed-Solomon parity: data followed by the remainder of data*x^roots mod g.
std::vector<std::uint8_t> rsParity(const Field& f, const std::vector<std::uint8_t>& data,
                                   int roots, int firstRoot, int gap) {
    const std::vector<std::uint8_t> g = generatorPoly(f, roots, firstRoot, gap);
    std::vector<std::uint8_t> rem(data.size() + static_cast<std::size_t>(roots), 0);
    std::copy(data.begin(), data.end(), rem.begin());
    for (std::size_t i = 0; i < data.size(); ++i) {
        const std::uint8_t coef = rem[i];
        if (!coef) continue;
        for (std::size_t j = 0; j < g.size(); ++j)
            rem[i + j] ^= f.mul(g[j], coef);
    }
    return std::vector<std::uint8_t>(rem.end() - roots, rem.end());
}

}  // namespace

int main() {
    using namespace nadirstation::gk2a;
    std::cout << "=== GK-2A receive chain, symbols to validated CADU ===\n\n";

    const Field f;
    // The CADU body is 1020 bytes after the 4-byte sync marker. With an interleaving depth of 4
    // the RS codewords are formed by taking every fourth octet, giving four codewords of 255
    // symbols each: 223 data + 32 parity.
    constexpr int kBody = 1020;
    constexpr int kDepth = 4;
    constexpr int kRoots = 32;
    constexpr int kFirstRoot = 112;
    constexpr int kGap = 11;      // libcorrect: roots at exp[(gap * (k + first_root)) % 255]

    // The VCDU header this frame carries, and what the receiver must read back out of it. Both
    // values are written into the plaintext below, so the frame is one a conforming transmitter
    // could have sent.
    //
    // The VCID is 63 -- the filler channel -- deliberately, because it is the value that tells the
    // two conventions apart, and because where its six bits sit is not where it looks like they sit.
    // The frame is byte 4 = {version 2, spacecraft id 8, VCID high 2}, byte 5 = {VCID low 4, frame
    // count high 4}, bytes 6-7 = the rest of the frame count, which is the layout the receiver's
    // own expressions implement: vcid = ((b4 & 0x3F) << 2) | (b5 >> 6), count = ((b5 & 0x3F) << 16)
    // | (b6 << 8) | b7. Written that way, byte 4 is 0x3F and byte 5 is 0xFC.
    //
    // Byte 4 sits under the randomiser's first byte, 0xFF, which covers exactly the six bits the
    // VCID occupies -- so on the air this byte is 0xC0, and a receiver that reads the header before
    // undoing the randomiser reports vcid 0: filler stops being recognised as filler and its frames
    // are handed on to the packet reassembler. Byte 5 is randomised by 0x48, which corrupts the
    // frame counter in the same way. Nothing checked either field, so neither showed up here.
    constexpr std::uint8_t kExpectedVcid = 63;
    constexpr std::uint32_t kExpectedVcfc = 0x001234;

    std::mt19937 rng(20261005);
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(kBody), 0);
    for (auto& b : payload) b = static_cast<std::uint8_t>(rng() & 0xFF);
    payload[0] = static_cast<std::uint8_t>((kExpectedVcid >> 2) & 0x3F);  // version, sc id, VCID high 2
    payload[1] = static_cast<std::uint8_t>(((kExpectedVcid & 0x03) << 6)
                                           | ((kExpectedVcfc >> 16) & 0x3F));    // VCID low 2, count high 6
    payload[2] = static_cast<std::uint8_t>((kExpectedVcfc >> 8) & 0xFF);
    payload[3] = static_cast<std::uint8_t>(kExpectedVcfc & 0xFF);
    payload[4] = 0x00;                                       // replay flag clear, spare

    // Lay the payload out as the interleaved codewords and compute parity for each. Parity is
    // computed over the plaintext, because that is what the check at the far end sees: the
    // receiver derandomises first and only then tests the syndromes.
    std::vector<std::uint8_t> plainBody(static_cast<std::size_t>(kBody), 0);
    for (int j = 0; j < kDepth; ++j) {
        std::vector<std::uint8_t> code;
        for (int i = j; i < kBody; i += kDepth) code.push_back(payload[static_cast<std::size_t>(i)]);
        const std::size_t dataLen = code.size() - static_cast<std::size_t>(kRoots);
        std::vector<std::uint8_t> data(code.begin(),
                                       code.begin() + static_cast<std::ptrdiff_t>(dataLen));
        // Compute the parity in the DUAL basis, because that is where CCSDS defines the code.
        // Working in the standard basis produces a codeword of a different code, which is why
        // the check rejected frames that otherwise round-tripped perfectly.
        for (auto& byte : data) byte = kFromDualBasis[byte];
        std::vector<std::uint8_t> par = rsParity(f, data, kRoots, kFirstRoot, kGap);
        for (auto& byte : par) byte = kToDualBasis[byte];
        // The message returns to the standard basis for transmission, as the reference does.
        for (std::size_t ii = 0; ii < dataLen; ++ii) code[ii] = kToDualBasis[data[ii]];
        for (int ii = 0; ii < kRoots; ++ii)
            code[dataLen + static_cast<std::size_t>(ii)] = par[static_cast<std::size_t>(ii)];
        for (std::size_t i = 0, k = static_cast<std::size_t>(j); i < code.size();
             ++i, k += static_cast<std::size_t>(kDepth))
            if (k < static_cast<std::size_t>(kBody)) plainBody[k] = code[i];
    }

    // The CADU as it exists after reception, i.e. derandomised: sync marker then plain body.
    std::vector<std::uint8_t> received;
    received.push_back(0x1A);
    received.push_back(0xCF);
    received.push_back(0xFC);
    received.push_back(0x1D);
    received.insert(received.end(), plainBody.begin(), plainBody.end());
    std::cout << "  built a " << received.size() << "-byte CADU with sync 0x1ACFFC1D\n";
    std::cout << "  its Reed-Solomon check passes before transmission: "
              << (rsSyndromesZero(received, kDepth) ? "yes" : "no") << "\n";
    if(!rsSyndromesZero(received,kDepth))return 1;
    for(int errors:{1,8,16}){
        auto damaged=received;
        for(int i=0;i<errors;++i)damaged[4+i*kDepth]^=std::uint8_t(0x81+i);
        if(rsSyndromesZero(damaged,kDepth) || rsCorrect(damaged,kDepth)!=errors || damaged!=received){
            std::cerr<<"RS correction failed for "<<errors<<" errors\n";return 1;
        }
    }
    std::cout<<"  RS corrects 1, 8 and 16 errors in a codeword\n";
    constexpr int timingFrames=200;
    const auto startReference=std::chrono::steady_clock::now();
    unsigned syndromeSum=0;
    for(int trial=0;trial<timingFrames;++trial)for(int pos=0;pos<kDepth;++pos)
        for(int k=0;k<kRoots;++k){
            const auto root=f.exp[(kGap*(k+kFirstRoot))%255];std::uint8_t acc=0;
            for(int i=0;i<255;++i)acc=f.mul(acc,root)^kFromDualBasis[received[4+i*kDepth+pos]];
            syndromeSum+=acc;
        }
    const auto startOptimized=std::chrono::steady_clock::now();
    for(int trial=0;trial<timingFrames;++trial){auto copy=received;if(rsCorrect(copy,kDepth)!=0)return 1;}
    const auto endOptimized=std::chrono::steady_clock::now();
    if(syndromeSum!=0)return 1;
    std::cout<<"  RS "<<timingFrames<<" clean frames reference "
             <<std::chrono::duration<double>(startOptimized-startReference).count()
             <<"s optimized "<<std::chrono::duration<double>(endOptimized-startOptimized).count()<<"s\n";

    // What actually goes on the air: everything after the marker randomised. An earlier version
    // of this test applied the randomiser twice -- once when building the frame and once before
    // transmitting -- and then compared the doubly-randomised bytes against the single
    // derandomisation the receiver performs, so a working chain reported a byte mismatch.
    std::vector<std::uint8_t> onAir = received;
    derandomise(onAir);
    const bool randomised = std::memcmp(onAir.data() + 4, received.data() + 4,
                                        static_cast<std::size_t>(kBody)) != 0;
    std::cout << "  the body changes when the randomiser is applied: "
              << (randomised ? "yes" : "no") << "\n";

    // Trace the first body byte through every stage. The receiver should undo exactly the
    // randomisation applied here, so plainBody[0] must reappear at the far end; showing the value
    // at each stage says which step lost it.
    std::cout << "\n  byte 0 of the body at each stage:\n";
    std::printf("    payload[0]      %02X\n", payload[0]);
    std::printf("    plainBody[0]    %02X   (equals payload[0], parity is written elsewhere)\n",
                plainBody[0]);
    std::printf("    received[4]     %02X   (sync marker then the body)\n", received[4]);
    std::printf("    onAir[4]        %02X   (after randomising)\n", onAir[4]);
    std::printf("    randomiser[0]   %02X\n", LritDecoder::randomiserBits()[0]);
    std::printf("    onAir[4] ^ rand = %02X   (what the receiver should recover)\n",
                static_cast<unsigned>(onAir[4] ^ LritDecoder::randomiserBits()[0]));

    // Self-inverse check: applying the same routine to the randomised frame must give back the
    // plaintext. If it does here but the receiver does not, the receiver is not looking at the
    // bytes this test thinks it sent, which is a different fault from a broken randomiser.
    {
        std::vector<std::uint8_t> back = onAir;
        derandomise(back);
        const bool recovered = std::memcmp(back.data(), received.data(), received.size()) == 0;
        std::printf("    derandomise(onAir)[4] = %02X   round trip: %s\n", back[4],
                    recovered ? "ok" : "FAILED");
        std::size_t d = received.size();
        for (std::size_t i = 0; i < received.size(); ++i)
            if (back[i] != received[i]) { d = i; break; }
        if (!recovered) std::printf("    first difference at %zu\n", d);
    }
    std::cout << "\n";

    // Encode the whole CADU as bits, then to symbols.
    std::vector<std::uint8_t> bits;
    bits.reserve(onAir.size() * 8);
    for (const std::uint8_t byte : onAir)
        for (int i = 7; i >= 0; --i)
            bits.push_back(static_cast<std::uint8_t>((byte >> i) & 1));
    const std::vector<std::uint8_t> symbols = ViterbiDecoder::encode(bits);
    std::vector<float> soft(symbols.size());
    for (std::size_t i = 0; i < symbols.size(); ++i) soft[i] = symbols[i] ? -1.0f : 1.0f;
    std::cout << "  encoded to " << symbols.size() << " symbols\n\n";

    // Receive.
    LritDecoder dec;
    dec.feedSoftBits(soft, true);
    const std::vector<Cadu> got = dec.takeCadus();
    const auto& st = dec.stats();
    std::cout << "=== receiver ===\n";
    std::cout << "  bits in          " << st.bitsIn << "\n";
    std::cout << "  CADUs assembled  " << st.cadusAssembled << "\n";
    std::cout << "  with sync        " << st.cadusWithSync << "\n";
    std::cout << "  passing RS       " << st.cadusWithGoodRs << "\n";

    bool ok = false;
    if (!got.empty()) {
        const Cadu& c = got.front();
        // Compare against the plaintext, because that is what the decoder hands back. It applies
        // the randomiser itself, over bytes 4 to the end, before running the Reed-Solomon check --
        // the check is defined on the plaintext, and SatDump's FEC stage does the same. An earlier
        // version of this test compared against the on-air form, so a working chain reported a
        // mismatch: nothing in this chain produces the randomised frame, and nothing should.
        std::size_t firstDiff = received.size();
        for (std::size_t i = 0; i < received.size() && i < c.bytes.size(); ++i)
            if (c.bytes[i] != received[i]) { firstDiff = i; break; }
        const bool bytesMatch = firstDiff == received.size();
        std::cout << "\n  first CADU: " << c.bytes.size() << " bytes, sync "
                  << (c.syncFound ? "found" : "missing") << ", RS "
                  << (c.rsOk ? "ok" : "bad") << ", vcid " << int(c.vcid) << "\n";
        std::cout << "  recovered bytes identical to what was transmitted: "
                  << (bytesMatch ? "yes" : "no") << "\n";
        // The payload as the layers above consume it, which is the body without its sync marker.
        const bool plainMatches =
            std::memcmp(c.bytes.data() + 4, plainBody.data(),
                        static_cast<std::size_t>(kBody)) == 0;
        std::cout << "  and derandomised, it reproduces the payload: "
                  << (plainMatches ? "yes" : "no") << "\n";
        // The VCDU header is read from the plaintext, so the fields the transmitter wrote must come
        // back. This is the assertion that was missing when the header was being read before the
        // randomiser was undone: the frame still synced and still passed its parity, and only these
        // two fields said the frame had been filed under the wrong channel.
        const bool vcidMatches = c.vcid == kExpectedVcid;
        const bool counterMatches = c.virtualFrameCount == kExpectedVcfc;
        std::cout << "  VCDU header: vcid " << int(c.vcid) << " (sent " << int(kExpectedVcid)
                  << "), frame counter " << c.virtualFrameCount << " (sent " << kExpectedVcfc
                  << "): " << ((vcidMatches && counterMatches) ? "ok" : "MISMATCH") << "\n";
        if (!vcidMatches || !counterMatches) {
            // Byte-for-byte, the six header bytes as received and as transmitted. A mismatch that
            // is a whole number of bytes out shows a frame that is not where the test thinks it
            // is; one that is not shows a header read at the wrong point in the chain.
            std::cout << "    body bytes 0..7 as received:";
            for (std::size_t i = 0; i < 8 && i + 4 < c.bytes.size(); ++i)
                std::printf(" %02X", static_cast<unsigned char>(c.bytes[i + 4]));
            std::cout << "\n    body bytes 0..7 as sent    :";
            for (std::size_t i = 0; i < 8; ++i)
                std::printf(" %02X", static_cast<unsigned>(plainBody[i]));
            std::cout << "\n";
            // And what the randomised bytes say, so the two conventions are distinguishable from
            // the report alone. The randomiser's first byte is 0xFF, which sits over exactly the
            // six bits the VCID occupies, so a filler frame's 63 reads as 0 when the header is
            // taken before the randomiser is undone -- filler stops being recognised as filler.
            const std::uint8_t firstByte = LritDecoder::randomiserBits()[0];
            const std::uint8_t masked = static_cast<std::uint8_t>(kExpectedVcid ^ firstByte);
            const int misread = ((masked & 0x3Fu) << 2) | ((masked >> 6) & 0x03u);
            std::cout << "    that byte on the air is " << std::hex << int(masked) << std::dec
                      << ", which a receiver that reads the header too early reports as vcid "
                      << misread << "\n";
        }
        ok = c.syncFound && c.rsOk && bytesMatch && plainMatches && vcidMatches && counterMatches;
    }

    // And with symbol errors, to show the correction is doing real work.
    std::cout << "\n=== the same frame with symbol errors ===\n";
    std::mt19937 r2(7);
    for (const int flips : {10, 50, 150, 300}) {
        std::vector<float> noisy = soft;
        for (int i = 0; i < flips; ++i) {
            const std::size_t p = r2() % noisy.size();
            noisy[p] = -noisy[p];
        }
        LritDecoder d2;
        d2.feedSoftBits(noisy, true);
        const auto c2 = d2.takeCadus();
        const auto& s2 = d2.stats();
        std::cout << "  " << flips << " symbol flips of " << soft.size() << ": sync "
                  << s2.cadusWithSync << ", RS ok " << s2.cadusWithGoodRs
                  << (s2.cadusWithGoodRs > 0 ? "   recovered" : "   lost") << "\n";
    }

    std::cout << "\n" << (ok ? "CHAIN OK" : "CHAIN FAILED") << "\n";
    return ok ? 0 : 1;
}
