#include "dsp.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <numeric>
#include <string>
#include <vector>

namespace {
uint32_t readU32(const unsigned char* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
        (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}
uint16_t readU16(const unsigned char* p) {
    return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
}

struct WavData {
    uint32_t sampleRate = 0;
    uint16_t channels = 0;
    uint16_t bits = 0;
    uint64_t dataOffset = 0;
    uint32_t dataBytes = 0;
};

bool readWavHeader(std::ifstream& f, WavData& w) {
    std::array<unsigned char, 12> riff{};
    if (!f.read(reinterpret_cast<char*>(riff.data()), riff.size()) ||
        std::string(reinterpret_cast<char*>(riff.data()), 4) != "RIFF" ||
        std::string(reinterpret_cast<char*>(riff.data() + 8), 4) != "WAVE") return false;
    while (f) {
        std::array<unsigned char, 8> chunk{};
        if (!f.read(reinterpret_cast<char*>(chunk.data()), chunk.size())) break;
        const uint32_t size = readU32(chunk.data() + 4);
        const std::string id(reinterpret_cast<char*>(chunk.data()), 4);
        const auto body = static_cast<uint64_t>(f.tellg());
        if (id == "fmt ") {
            std::array<unsigned char, 16> fmt{};
            if (size < fmt.size() || !f.read(reinterpret_cast<char*>(fmt.data()), fmt.size())) return false;
            if (readU16(fmt.data()) != 1) return false;
            w.channels = readU16(fmt.data() + 2);
            w.sampleRate = readU32(fmt.data() + 4);
            w.bits = readU16(fmt.data() + 14);
        } else if (id == "data") {
            w.dataOffset = body;
            w.dataBytes = size;
            return w.sampleRate > 0 && w.channels == 2 && w.bits == 16 && w.dataBytes > 0;
        }
        f.seekg(static_cast<std::streamoff>(body + size + (size & 1u)));
    }
    return w.sampleRate > 0 && w.channels == 2 && w.bits == 16 && w.dataBytes > 0;
}

float estimateM2M4(const std::vector<std::complex<float>>& symbols) {
    if (symbols.empty()) return 0.0f;
    double m2 = 0.0, m4 = 0.0;
    for (const auto& z : symbols) {
        const double p = std::norm(z);
        m2 += p;
        m4 += p * p;
    }
    m2 /= symbols.size();
    m4 /= symbols.size();
    const double signal = std::sqrt(std::max(0.0, 2.0 * m2 * m2 - m4));
    const double noise = m2 - signal;
    return signal > 0.0 && noise > 0.0
        ? static_cast<float>(std::max(0.0, 10.0 * std::log10(signal / noise))) : 0.0f;
}
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "Usage: NadirStation_DemodSweep input.wav output.csv\n";
        return 2;
    }
    std::ifstream wav(argv[1], std::ios::binary);
    WavData header;
    if (!wav || !readWavHeader(wav, header)) {
        std::cerr << "Expected a little-endian stereo PCM16 WAV.\n";
        return 3;
    }
    const uint64_t frameCount = header.dataBytes / (header.channels * sizeof(int16_t));
    const uint64_t wantedFrames = std::min<uint64_t>(frameCount, static_cast<uint64_t>(header.sampleRate) * 8);
    const uint64_t startFrame = std::min<uint64_t>(frameCount - wantedFrames, frameCount / 2);
    wav.seekg(static_cast<std::streamoff>(header.dataOffset + startFrame * 4));
    std::vector<unsigned char> bytes(static_cast<std::size_t>(wantedFrames * 4));
    if (!wav.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()))) {
        std::cerr << "Unable to read selected WAV excerpt.\n";
        return 4;
    }
    std::vector<std::complex<float>> iq(static_cast<std::size_t>(wantedFrames));
    for (std::size_t i = 0; i < iq.size(); ++i) {
        const auto uI = readU16(bytes.data() + i * 4);
        const auto uQ = readU16(bytes.data() + i * 4 + 2);
        const int16_t I = static_cast<int16_t>(uI), Q = static_cast<int16_t>(uQ);
        iq[i] = {static_cast<float>(I) / 32768.0f, static_cast<float>(Q) / 32768.0f};
    }

    std::ofstream csv(argv[2], std::ios::trunc);
    if (!csv) return 5;
    csv << "rrc_alpha,pll_bw,symbols,snr_m2m4_db,timing_quality,soft_quality,mean_abs_carrier_hz,excerpt_start_s,excerpt_duration_s\n";
    constexpr std::array<float, 3> alphas{0.35f, 0.50f, 0.65f};
    constexpr std::array<float, 4> loops{0.005f, 0.010f, 0.020f, 0.040f};
    constexpr std::size_t blockSize = 65536;
    const double startSec = static_cast<double>(startFrame) / header.sampleRate;
    const double durationSec = static_cast<double>(wantedFrames) / header.sampleRate;

    for (float alpha : alphas) for (float pll : loops) {
        nadirstation::dsp::PskDemod demod(static_cast<float>(header.sampleRate), 660000.0f, alpha, pll);
        std::vector<std::complex<float>> symbols;
        double timing = 0.0, soft = 0.0, carrier = 0.0;
        uint64_t chunks = 0;
        for (std::size_t pos = 0; pos < iq.size(); pos += blockSize) {
            const auto end = std::min(iq.size(), pos + blockSize);
            std::vector<std::complex<float>> block(iq.begin() + static_cast<std::ptrdiff_t>(pos),
                                                   iq.begin() + static_cast<std::ptrdiff_t>(end));
            const auto result = demod.process(block);
            if (!result.symbols.empty()) {
                symbols.insert(symbols.end(), result.symbols.begin(), result.symbols.end());
                timing += result.timingQuality;
                soft += result.soft.empty() ? 0.0 : std::accumulate(result.soft.begin(), result.soft.end(), 0.0);
                carrier += std::fabs(result.carrierOffsetHz);
                ++chunks;
            }
        }
        csv << alpha << ',' << pll << ',' << symbols.size() << ',' << estimateM2M4(symbols) << ','
            << (chunks ? timing / chunks : 0.0) << ','
            << (symbols.empty() ? 0.0 : soft / symbols.size()) << ','
            << (chunks ? carrier / chunks : 0.0) << ',' << startSec << ',' << durationSec << '\n';
    }
    std::cout << "Wrote 12 parameter combinations from " << durationSec << " seconds of input.\n";
    return 0;
}
