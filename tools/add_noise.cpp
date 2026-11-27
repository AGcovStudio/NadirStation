// add_noise.cpp - add white Gaussian noise to a cs8 / cs16 baseband recording (streaming, any file size).
// usage: add_noise <in> <out> <cs8|cs16> <snr_now_dB> <snr_target_dB> [seed]
// The recording's present SNR (whatever scale you read it on, e.g. SatDump's estimate) is lowered to the target:
// the added noise makes the SNR fall by exactly (snr_now - snr_target) dB, assuming the original noise is white.
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cmath>
#include <random>
#include <vector>
#include <algorithm>
int main(int argc, char** argv) {
    if (argc < 6) { std::fprintf(stderr, "usage: %s in out cs8|cs16 snr_now_dB snr_target_dB [seed]\n", argv[0]); return 2; }
    const bool c8 = std::string(argv[3]) == "cs8";
    const double now = std::atof(argv[4]), tgt = std::atof(argv[5]);
    const unsigned seed = argc > 6 ? (unsigned)std::atoi(argv[6]) : 1u;
    if (tgt > now) { std::fprintf(stderr, "target must be lower than the present SNR\n"); return 2; }
    const std::size_t bytesPer = c8 ? 1 : 2, chunk = 1u << 22;
    FILE* fi = std::fopen(argv[1], "rb"); if (!fi) { std::perror("open input"); return 1; }
    // pass 1: mean power per component
    double e = 0; std::uint64_t n = 0; std::vector<unsigned char> buf(chunk * bytesPer);
    for (;;) {
        const std::size_t got = std::fread(buf.data(), bytesPer, chunk, fi); if (!got) break;
        for (std::size_t i = 0; i < got; ++i) {
            const double v = c8 ? (double)(std::int8_t)buf[i] : (double)((std::int16_t)(buf[2 * i] | (buf[2 * i + 1] << 8)));
            e += v * v;
        }
        n += got;
    }
    if (!n) { std::fprintf(stderr, "empty file\n"); return 1; }
    const double P = e / n;                                    // total power per component
    const double r0 = std::pow(10.0, now / 10.0);
    const double pn0 = P / (1.0 + r0);                         // original noise power per component
    const double add = pn0 * (std::pow(10.0, (now - tgt) / 10.0) - 1.0);
    const double sd = std::sqrt(add);
    std::fprintf(stderr, "samples %llu, rms %.2f, added noise rms %.2f (%.2f dB below to %.2f dB)\n", (unsigned long long)n, std::sqrt(P), sd, now, tgt);
    std::rewind(fi);
    FILE* fo = std::fopen(argv[2], "wb"); if (!fo) { std::perror("open output"); return 1; }
    std::mt19937_64 rng(seed); std::normal_distribution<double> nd(0.0, sd);
    const double lo = c8 ? -127.0 : -32767.0, hi = c8 ? 127.0 : 32767.0; std::uint64_t clip = 0;
    std::vector<unsigned char> out(chunk * bytesPer);
    for (;;) {
        const std::size_t got = std::fread(buf.data(), bytesPer, chunk, fi); if (!got) break;
        for (std::size_t i = 0; i < got; ++i) {
            double v = c8 ? (double)(std::int8_t)buf[i] : (double)((std::int16_t)(buf[2 * i] | (buf[2 * i + 1] << 8)));
            v += nd(rng); if (v < lo) { v = lo; ++clip; } else if (v > hi) { v = hi; ++clip; }
            const long q = std::lround(v);
            if (c8) out[i] = (unsigned char)(std::int8_t)q; else { out[2 * i] = (unsigned char)(q & 0xFF); out[2 * i + 1] = (unsigned char)((q >> 8) & 0xFF); }
        }
        std::fwrite(out.data(), bytesPer, got, fo);
    }
    std::fclose(fi); std::fclose(fo);
    std::fprintf(stderr, "done, %llu samples clipped\n", (unsigned long long)clip);
    return 0;
}
