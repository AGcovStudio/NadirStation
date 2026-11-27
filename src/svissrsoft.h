// Soft-decision stage for an S-VISSR frame, kept free of Qt so it can be tested on its own.
//
// Input: the demodulated symbols of one frame's data part (positive = 0, a clean symbol about 1),
// the running parity of the descrambling mask, where every sector starts, and what the hard path
// already found out about each sector. Output: which sectors are demonstrably aligned (their
// known-zero filler is far from random), and, for aligned sectors whose CRC failed, a replacement
// payload estimated from the symbols (see softpixel.h).
//
// The filler test uses the symbols themselves. Inside a filler all descrambled symbols share one
// sign, so t = |sum| / sqrt(sum of squares) is about N(0,1) for anything that is not aligned and
// grows like sqrt(2048) * a / sqrt(a^2 + sigma^2) when it is: 8 is a 1e-15 event for misaligned
// data and is still passed by a sector at -10 dB Es/N0, where counting differential bit errors
// (each symbol error makes two) cannot tell it from noise.

#pragma once

#include "softpixel.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <future>
#include <thread>
#include <vector>

namespace nadirstation::svissrsoft {

constexpr double kStrongFillerZ = 8.0;
constexpr int kMinStrongSectors = 4;
constexpr int kFillerBits = 2048;

struct SectorGeometry {
    int idBits;
    int dataBits;
    uint32_t expectedId;
};

struct SectorIo {
    std::size_t start = 0;            // index of the sector's first bit in the frame's data part
    bool crcOk = false;               // from the hard path
    bool strong = false;              // out: the filler vouches for the sector
    bool recovered = false;           // out: dataBits replaced by the soft estimate
    std::vector<uint8_t>* dataBits = nullptr;   // in/out
};

struct Outcome {
    int strongSectors = 0;
    int recoveredSectors = 0;
    double llrScale = 0.0;
    bool accepted = false;            // enough strong sectors to trust the frame
};

struct Job {
    int sector;
    int bits;
    int chain;
    softpixel::Prior prior;
};

// Priors: IR high bytes and IR4 follow scan-line imagery closely; VIS is smoother per pixel.
inline const std::array<Job, 8>& jobs() {
    static const std::array<Job, 8> table = {{
        {1, 8, 8, {3.0, 0.04}}, {2, 8, 8, {3.0, 0.04}}, {3, 8, 8, {3.0, 0.04}},
        {4, 6, 6, {1.8, 0.01}}, {5, 6, 6, {1.8, 0.01}}, {6, 6, 6, {1.8, 0.01}}, {7, 6, 6, {1.8, 0.01}},
        {11, 10, 8, {3.0, 0.04}},
    }};
    return table;
}

// `running`: cumulative parity of the descrambling mask, one byte (0/1) per data bit of the frame.
// `x`: the frame's symbols. `io`: one entry per sector (12), `geo`: their geometry.
inline Outcome run(const float* x, const uint8_t* running, const SectorGeometry* geo, std::array<SectorIo, 12>& io) {
    Outcome out;
    std::array<int, 12> fillerParity{};
    std::array<double, 12> fillerZ{};
    for (int si = 0; si < 12; ++si) {
        const std::size_t f0 = io[static_cast<std::size_t>(si)].start +
            static_cast<std::size_t>(geo[si].idBits + geo[si].dataBits + 16);
        double sum = 0.0, sum2 = 0.0;
        for (int k = 0; k < kFillerBits; ++k) {
            const double v = running[f0 + static_cast<std::size_t>(k)] ? -x[f0 + static_cast<std::size_t>(k)]
                                                                       : x[f0 + static_cast<std::size_t>(k)];
            sum += v;
            sum2 += v * v;
        }
        fillerParity[static_cast<std::size_t>(si)] = sum < 0.0 ? 1 : 0;
        fillerZ[static_cast<std::size_t>(si)] = sum2 > 0.0 ? std::fabs(sum) / std::sqrt(sum2) : 0.0;
    }
    for (int si = 1; si < 12; ++si) {
        auto& s = io[static_cast<std::size_t>(si)];
        s.strong = fillerZ[static_cast<std::size_t>(si)] >= kStrongFillerZ;
        if (s.strong) ++out.strongSectors;
    }
    if (out.strongSectors < kMinStrongSectors) return out;
    out.accepted = true;

    // Signal level and noise variance from the aligned fillers (the DOC's too when it passes).
    double sumA = 0.0, sumA2 = 0.0, countA = 0.0;
    for (int si = 0; si < 12; ++si) {
        const bool usable = si == 0 ? fillerZ[0] >= kStrongFillerZ : io[static_cast<std::size_t>(si)].strong;
        if (!usable) continue;
        const std::size_t f0 = io[static_cast<std::size_t>(si)].start +
            static_cast<std::size_t>(geo[si].idBits + geo[si].dataBits + 16);
        const bool flip = fillerParity[static_cast<std::size_t>(si)] != 0;
        for (int k = 0; k < kFillerBits; ++k) {
            double v = running[f0 + static_cast<std::size_t>(k)] ? -x[f0 + static_cast<std::size_t>(k)]
                                                                 : x[f0 + static_cast<std::size_t>(k)];
            if (flip) v = -v;
            sumA += v;
            sumA2 += v * v;
            countA += 1.0;
        }
    }
    const double mean = countA > 0.0 ? sumA / countA : 0.0;
    const double variance = countA > 0.0 ? std::max(1e-4, sumA2 / countA - mean * mean) : 1.0;
    out.llrScale = mean > 0.0 ? 2.0 * mean / variance : 0.0;
    if (out.llrScale <= 0.0) return out;

    // The jobs are independent: each reads the shared symbols and writes only its own sector's bits,
    // and the forward-backward pass is the expensive part of a frame (about 0.3 s on one core at
    // low signal-to-noise ratio, more than the frame's 0.55 s of air time allows for once the demodulator is
    // counted). Run them side by side; the result is the same as running them one after another.
    struct Pending { const Job* job; bool done = false; };
    std::array<Pending, 8> pending{};
    int pendingCount = 0;
    for (const Job& job : jobs()) {
        const auto& s = io[static_cast<std::size_t>(job.sector)];
        if (!s.strong || s.crcOk || !s.dataBits) continue;
        if (static_cast<int>(s.dataBits->size()) != geo[job.sector].dataBits) continue;
        pending[static_cast<std::size_t>(pendingCount++)].job = &job;
    }
    const double llrScale = out.llrScale;
    auto runJob = [&](Pending& p) {
        const Job& job = *p.job;
        auto& s = io[static_cast<std::size_t>(job.sector)];
        const SectorGeometry& g = geo[job.sector];
        const int pixels = g.dataBits / job.bits;
        const std::size_t d0 = s.start + static_cast<std::size_t>(g.idBits);
        // Parity entering the data field: the previous filler's parity, changed by the known ID.
        int startParity = fillerParity[static_cast<std::size_t>(job.sector - 1)];
        for (int b = 0; b < g.idBits; ++b) startParity ^= static_cast<int>((g.expectedId >> (g.idBits - 1 - b)) & 1u);
        std::vector<float> llr(static_cast<std::size_t>(pixels) * static_cast<std::size_t>(job.bits));
        for (std::size_t k = 0; k < llr.size(); ++k) {
            const float v = running[d0 + k] ? -x[d0 + k] : x[d0 + k];
            llr[k] = static_cast<float>(llrScale * v);
        }
        const auto result = softpixel::decodeChain(llr.data(), pixels, job.bits, job.chain, startParity, job.prior);
        auto& bits = *s.dataBits;
        for (int i = 0; i < pixels; ++i) {
            const unsigned value = result.mean[static_cast<std::size_t>(i)];
            // The estimate covers the leading `chain` bits; trailing bits (IR4's two least
            // significant) keep their hard decisions.
            for (int b = 0; b < job.chain; ++b)
                bits[static_cast<std::size_t>(i * job.bits + b)] =
                    static_cast<uint8_t>((value >> (job.chain - 1 - b)) & 1u);
        }
        p.done = true;
    };
    if (pendingCount > 1 && std::thread::hardware_concurrency() > 1) {
        std::array<std::future<void>, 8> futures;
        // The first job stays on this thread; the others go to helpers.
        for (int k = 1; k < pendingCount; ++k)
            futures[static_cast<std::size_t>(k)] = std::async(std::launch::async, [&, k] { runJob(pending[static_cast<std::size_t>(k)]); });
        runJob(pending[0]);
        for (int k = 1; k < pendingCount; ++k) futures[static_cast<std::size_t>(k)].get();
    } else {
        for (int k = 0; k < pendingCount; ++k) runJob(pending[static_cast<std::size_t>(k)]);
    }
    for (int k = 0; k < pendingCount; ++k) {
        if (!pending[static_cast<std::size_t>(k)].done) continue;
        io[static_cast<std::size_t>(pending[static_cast<std::size_t>(k)].job->sector)].recovered = true;
        ++out.recoveredSectors;
    }
    return out;
}

}  // namespace nadirstation::svissrsoft
