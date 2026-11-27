// Decoding threshold of any link, through the application's own decoder.
//
// A stretch of a real recording is loaded once. Its Es/N0 is estimated from the spectrum: the
// noise density is the median of the power spectral density outside the signal (|f| > 1.15 x
// --half-bw), the signal power is the total power minus that noise, and Es/N0 = S / (N0 * Rs).
// When the recording leaves no noise-only band (--half-bw 0), the original noise is neglected:
// S is the total power, which is right to within a fraction of a dB when the recording is clean.
//
// Then for each requested Es/N0, white Gaussian noise is added, the result is quantised to 16-bit
// I/Q (so it is exactly the file another decoder would be given, see --write), and a fresh
// NadirStationDecoder configured as the application configures that link decodes it. The counts
// come from the decoder's own pipeline snapshot (the Decoder page's loss funnel): for every stage,
// the units that passed and the units it threw away.
//
// Usage: NadirStation_ThresholdProbe <recording.cs16|.cs8|.wav> --satellite NAME --symbol-rate Rs
//            --rate Fs [--skip s] [--seconds s] [--half-bw Hz] [--esn0 8,6,4,...] [--write dir]
//            [--seed n] [--pll 0.02] [--alpha 0.5] [--no-clean]
// Output: one line per point,
//   POINT <label> target=<dB> measured=<dB> snr=<decoder dB> <stage>=<passed>/<rejected> ...

#include "../src/nadirstationdecoder.h"
#include "../src/pipelinediag.h"

#include <QCoreApplication>
#include <QDir>
#include <QString>
#include <QStringList>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <numeric>
#include <random>
#include <vector>

namespace {

using cf = std::complex<float>;
constexpr double kPi = 3.14159265358979323846;

// Radix-2 FFT, in place.
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / double(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

struct Estimate { double signal = 0, noiseVar = 0, esn0Db = 0; };

// Signal power and complex noise variance (as if white over the whole band) of x.
Estimate estimate(const std::vector<cf>& x, double fs, double rs, double halfBw) {
    double total = 0;
    for (const cf& v : x) total += std::norm(v);
    total /= double(x.size());
    Estimate e;
    if (halfBw > 0) {
        const std::size_t N = 8192;
        std::vector<double> win(N), P(N, 0.0);
        double w2 = 0;
        for (std::size_t i = 0; i < N; ++i) { win[i] = 0.5 - 0.5 * std::cos(2 * kPi * i / N); w2 += win[i] * win[i]; }
        std::vector<std::complex<double>> buf(N);
        int blocks = 0;
        for (std::size_t at = 0; at + N <= x.size() && blocks < 2000; at += N * 3) {
            for (std::size_t i = 0; i < N; ++i) buf[i] = std::complex<double>(x[at + i]) * win[i];
            fft(buf);
            for (std::size_t i = 0; i < N; ++i) P[i] += std::norm(buf[i]);
            ++blocks;
        }
        std::vector<double> sel;
        for (std::size_t i = 0; i < N; ++i) {
            const double f = (i < N / 2 ? double(i) : double(i) - double(N)) * fs / double(N);
            if (std::fabs(f) > halfBw * 1.15 && std::fabs(f) < 0.42 * fs) sel.push_back(P[i] / (blocks * w2));
        }
        if (sel.size() > 16) {
            std::nth_element(sel.begin(), sel.begin() + sel.size() / 2, sel.end());
            e.noiseVar = sel[sel.size() / 2];
        }
    }
    e.signal = std::max(total - e.noiseVar, 1e-12);
    const double n0 = e.noiseVar / fs;
    e.esn0Db = n0 > 0 ? 10 * std::log10((e.signal / rs) / n0) : 99.0;
    return e;
}

bool load(const QString& path, double fs, double skip, double seconds, std::vector<cf>& out) {
    std::ifstream in(path.toLocal8Bit().constData(), std::ios::binary);
    if (!in) return false;
    const QString lower = path.toLower();
    const bool cs8 = lower.endsWith(".cs8") || lower.endsWith(".s8");
    const bool cu8 = lower.endsWith(".cu8");
    std::streamoff header = 0;
    if (lower.endsWith(".wav")) {
        std::vector<char> h(65536);
        in.read(h.data(), std::streamsize(h.size()));
        const std::string s(h.data(), std::size_t(in.gcount()));
        const auto p = s.find("data");
        if (p == std::string::npos) return false;
        header = std::streamoff(p + 8);
        in.clear();
    }
    const std::size_t bytes = (cs8 || cu8) ? 1 : 2;
    const std::size_t n = std::size_t(seconds * fs);
    in.seekg(header + std::streamoff(std::size_t(skip * fs) * 2 * bytes), std::ios::beg);
    std::vector<char> raw(n * 2 * bytes);
    in.read(raw.data(), std::streamsize(raw.size()));
    const std::size_t got = std::size_t(in.gcount()) / (2 * bytes);
    out.resize(got);
    for (std::size_t i = 0; i < got; ++i) {
        if (cs8) out[i] = cf(float(std::int8_t(raw[2 * i])) / 128.f, float(std::int8_t(raw[2 * i + 1])) / 128.f);
        else if (cu8) out[i] = cf((float(std::uint8_t(raw[2 * i])) - 127.5f) / 128.f, (float(std::uint8_t(raw[2 * i + 1])) - 127.5f) / 128.f);
        else {
            const auto* r = reinterpret_cast<const std::int16_t*>(raw.data());
            out[i] = cf(float(r[2 * i]) / 32768.f, float(r[2 * i + 1]) / 32768.f);
        }
    }
    return got > 0;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString path, satellite, writeDir;
    double fs = 0, rs = 0, skip = 0, seconds = 10, halfBw = 0, pll = 0, alpha = 0.5;
    unsigned seed = 1;
    bool noClean = false;
    QList<double> grid;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        const auto next = [&] { return QString::fromLocal8Bit(argv[++i]); };
        if (a == "--satellite" && i + 1 < argc) satellite = next();
        else if (a == "--symbol-rate" && i + 1 < argc) rs = next().toDouble();
        else if (a == "--rate" && i + 1 < argc) fs = next().toDouble();
        else if (a == "--skip" && i + 1 < argc) skip = next().toDouble();
        else if (a == "--seconds" && i + 1 < argc) seconds = next().toDouble();
        else if (a == "--half-bw" && i + 1 < argc) halfBw = next().toDouble();
        else if (a == "--pll" && i + 1 < argc) pll = next().toDouble();
        else if (a == "--alpha" && i + 1 < argc) alpha = next().toDouble();
        else if (a == "--seed" && i + 1 < argc) seed = next().toUInt();
        else if (a == "--write" && i + 1 < argc) writeDir = next();
        else if (a == "--no-clean") noClean = true;
        else if (a == "--esn0" && i + 1 < argc) { for (const QString& v : next().split(',')) grid << v.toDouble(); }
        else if (path.isEmpty()) path = a;
    }
    if (path.isEmpty() || satellite.isEmpty() || fs <= 0 || rs <= 0) {
        std::cerr << "usage: NadirStation_ThresholdProbe <file> --satellite NAME --symbol-rate Rs --rate Fs\n"
                     "       [--skip s] [--seconds s] [--half-bw Hz] [--esn0 8,6,4] [--write dir] [--seed n]\n";
        return 2;
    }
    if (pll <= 0) pll = satellite.startsWith("FY", Qt::CaseInsensitive) ? 0.02 : 0.02;

    std::vector<cf> clean;
    if (!load(path, fs, skip, seconds, clean)) { std::cerr << "cannot read " << path.toStdString() << "\n"; return 3; }
    const Estimate e0 = estimate(clean, fs, rs, halfBw);
    std::printf("LOADED %zu samples (%.2f s)  signal %.4g  noise %.4g  clean Es/N0 %.2f dB%s\n", clean.size(),
                double(clean.size()) / fs, e0.signal, e0.noiseVar, e0.esn0Db,
                halfBw > 0 ? "" : " (no noise-only band: original noise neglected)");
    std::fflush(stdout);
    if (!writeDir.isEmpty()) QDir().mkpath(writeDir);

    // Scale for 16-bit quantisation: the noisiest point decides, RMS at about 1/8 of full scale.
    std::vector<double> points;   // NaN = the clean recording
    if (!noClean) points.push_back(std::nan(""));
    for (double t : grid) if (t < e0.esn0Db - 0.2) points.push_back(t);

    std::vector<cf> noisy(clean.size());
    std::vector<std::int16_t> q(clean.size() * 2);
    int idx = 0;
    for (double target : points) {
        ++idx;
        double sigma = 0;   // per component, in float units
        if (!std::isnan(target)) {
            const double n0Total = e0.signal * fs / (rs * std::pow(10.0, target / 10.0));
            sigma = std::sqrt(std::max(n0Total - e0.noiseVar, 0.0) / 2.0);
        }
        std::mt19937_64 rng(seed * 1000003ull + idx);
        std::normal_distribution<float> g(0.f, float(sigma));
        double p = 0;
        for (std::size_t i = 0; i < clean.size(); ++i) {
            noisy[i] = clean[i] + (sigma > 0 ? cf(g(rng), g(rng)) : cf(0, 0));
            p += std::norm(noisy[i]);
        }
        const double rms = std::sqrt(p / double(clean.size()));
        const float scale = float(4096.0 / std::max(rms, 1e-9));
        for (std::size_t i = 0; i < clean.size(); ++i) {
            q[2 * i] = std::int16_t(std::clamp(std::lround(noisy[i].real() * scale), -32768L, 32767L));
            q[2 * i + 1] = std::int16_t(std::clamp(std::lround(noisy[i].imag() * scale), -32768L, 32767L));
            noisy[i] = cf(float(q[2 * i]) / 32768.f, float(q[2 * i + 1]) / 32768.f);
        }
        const Estimate em = estimate(noisy, fs, rs, halfBw);
        const QString label = std::isnan(target) ? QStringLiteral("clean") : QString::number(target, 'f', 1);
        if (!writeDir.isEmpty()) {
            const QString f = QDir(writeDir).filePath(QStringLiteral("pt_%1.cs16").arg(label));
            std::ofstream o(f.toLocal8Bit().constData(), std::ios::binary);
            o.write(reinterpret_cast<const char*>(q.data()), std::streamsize(q.size() * sizeof(std::int16_t)));
        }

        // A fresh decoder per point, configured as the application configures this link.
        const QString outDir = QDir(writeDir.isEmpty() ? QStringLiteral("threshold_out") : writeDir)
                                   .filePath(QStringLiteral("ns_%1").arg(label));
        QDir(outDir).removeRecursively();
        auto decoder = std::make_unique<NadirStationDecoder>();
        decoder->setOutputRoot(outDir);
        decoder->setSampleRate(fs);
        decoder->setSymbolRate(rs);
        decoder->setSatelliteName(satellite);
        decoder->setSourceLabel(satellite);
        decoder->setDemodParameters(float(alpha), float(pll));
        decoder->setEnabled(true);
        std::mutex m;
        nadirstation::diag::Snapshot last;
        bool have = false;
        float snr = std::nanf("");
        QObject::connect(decoder.get(), &NadirStationDecoder::pipelineSnapshot, decoder.get(),
                         [&](const nadirstation::diag::Snapshot& s) { std::lock_guard<std::mutex> l(m); last = s; have = true; },
                         Qt::DirectConnection);
        QObject::connect(decoder.get(), &NadirStationDecoder::demodSnrMetricsReady, decoder.get(),
                         [&](float now, float, float) { std::lock_guard<std::mutex> l(m); snr = now; }, Qt::DirectConnection);

        const std::size_t block = 1u << 18;
        for (std::size_t at = 0; at < noisy.size(); at += block) {
            const std::size_t n = std::min(block, noisy.size() - at);
            auto b = std::make_shared<std::vector<cf>>(noisy.begin() + std::ptrdiff_t(at), noisy.begin() + std::ptrdiff_t(at + n));
            decoder->processSamples(b);
            decoder->drainQueuedSamples();
            app.processEvents();
        }
        decoder->finishDecoding();
        // The snapshot is cumulative and emitted at most four times a second (wall clock); one more
        // short block after a pause makes sure the counts include the end of the recording.
        QThread::msleep(300);
        {
            std::mt19937 r2(7);
            std::normal_distribution<float> g2(0.f, 1e-4f);
            auto tail = std::make_shared<std::vector<cf>>(std::size_t(fs * 0.01));
            for (auto& v : *tail) v = cf(g2(r2), g2(r2));
            decoder->processSamples(tail);
            decoder->drainQueuedSamples();
        }
        app.processEvents();

        std::lock_guard<std::mutex> l(m);
        std::printf("POINT %s target=%s measured=%.2f snr=%.2f", label.toLocal8Bit().constData(),
                    std::isnan(target) ? "nan" : QString::number(target, 'f', 2).toLocal8Bit().constData(),
                    em.esn0Db, double(snr));
        if (have)
            for (const auto& st : last.stages)
                std::printf(" %s=%.0f/%.0f", st.key.toLocal8Bit().constData(), st.passed, st.rejected);
        std::printf("\n");
        std::fflush(stdout);
        decoder.reset();
    }
    std::printf("DONE\n");
    return 0;
}
