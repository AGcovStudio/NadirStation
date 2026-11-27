// FY-2 S-VISSR decoding against signal-to-noise ratio, through the application's own decoder.
//
// A clean recording is read once to measure its Es/N0, then decoded again and again with white
// Gaussian noise added (in memory, before the decoder) so that each run sits at a chosen Es/N0.
// Each run gets a fresh NadirStationDecoder configured as the application configures it for FY-2 (660
// kbaud, RRC alpha 0.50, PLL bandwidth 0.020) and its own output folder, so the images can be
// put side by side.
//
// How the Es/N0 is measured. S-VISSR is unfiltered PSK, so its spectrum is a sinc^2 with its
// first nulls at +/- the symbol rate. At a null the signal contributes nothing and the averaged
// power spectral density is the noise density N0. The signal power is the main lobe above that
// floor divided by 0.9028, the fraction of a sinc^2's power inside its main lobe, and
// Es/N0 = S / (N0 * Rs). Noise is then added with density N0_add = S / (Rs * 10^(t/10)) - N0,
// i.e. a complex variance N0_add * fs. The same measurement is repeated on the noisy samples and
// printed, so the setting can be checked rather than trusted. The decoder's own SNR readout (the
// figure the application shows) is printed beside it.
//
// Usage: NadirStation_Fy2SnrSweep <recording.wav|.cs16|.cs8> [--rate 2000000] [--skip 0] [--seconds 60]
//                         [--snr 10,8,5,...] [--satellite FY-2G] [--out dir] [--measure-only]

#include "../src/nadirstationdecoder.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <vector>
#include <atomic>
#include <chrono>

namespace nadirstation { extern std::atomic<double> g_stageSeconds[4]; }

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kSymbolRate = 660000.0;
constexpr double kMainLobeFraction = 0.9028;

using cf = std::complex<float>;

// Byte offset of the sample data: a WAV's "data" chunk, or 0 for raw cs16.
std::int64_t dataOffset(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return -1;
    const QByteArray head = f.read(4096);
    if (!head.startsWith("RIFF")) return 0;
    int pos = 12;
    while (pos + 8 <= head.size()) {
        const QByteArray id = head.mid(pos, 4);
        std::uint32_t size = 0;
        std::memcpy(&size, head.constData() + pos + 4, 4);
        if (id == "data") return pos + 8;
        pos += 8 + static_cast<int>(size);
    }
    return 44;
}

void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / static_cast<double>(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

struct Measure {
    double n0 = 0.0, signal = 0.0, esn0Db = 0.0, centreHz = 0.0;
    bool ok = false;
};

// Averaged periodogram (Hann, 4096 points) and the sinc^2 null / main-lobe measurement above.
Measure measure(const std::vector<cf>& x, double fs) {
    constexpr std::size_t N = 4096;
    Measure m;
    if (x.size() < N * 8) return m;
    std::vector<double> win(N), psd(N, 0.0);
    double u = 0.0;
    for (std::size_t i = 0; i < N; ++i) {
        win[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (N - 1));
        u += win[i] * win[i];
    }
    std::vector<std::complex<double>> buf(N);
    const std::size_t blocks = x.size() / N;
    for (std::size_t b = 0; b < blocks; ++b) {
        for (std::size_t i = 0; i < N; ++i) buf[i] = std::complex<double>(x[b * N + i]) * win[i];
        fft(buf);
        for (std::size_t i = 0; i < N; ++i) psd[(i + N / 2) % N] += std::norm(buf[i]);
    }
    for (double& p : psd) p /= static_cast<double>(blocks) * u * fs;   // power per Hz
    const double df = fs / N;
    auto freq = [&](std::size_t i) { return (static_cast<double>(i) - N / 2.0) * df; };
    // Centre: the power-weighted mean frequency of the strongest part of the spectrum.
    const double peak = *std::max_element(psd.begin(), psd.end());
    double wsum = 0.0, fsum = 0.0;
    for (std::size_t i = 0; i < N; ++i)
        if (psd[i] > peak * 0.25) { wsum += psd[i]; fsum += psd[i] * freq(i); }
    m.centreHz = wsum > 0.0 ? fsum / wsum : 0.0;
    // Noise density: the lowest 9-bin average near each first null, both sides averaged.
    auto nullFloor = [&](double sign) {
        double best = 1e300;
        for (std::size_t i = 4; i + 4 < N; ++i) {
            const double off = sign * (freq(i) - m.centreHz);
            if (off < 0.85 * kSymbolRate || off > 1.15 * kSymbolRate) continue;
            if (std::fabs(freq(i)) > 0.47 * fs) continue;
            double s = 0.0;
            for (int k = -4; k <= 4; ++k) s += psd[i + k];
            best = std::min(best, s / 9.0);
        }
        return best;
    };
    const double a = nullFloor(1.0), b = nullFloor(-1.0);
    if (a >= 1e299 || b >= 1e299) return m;
    m.n0 = 0.5 * (a + b);
    double lobe = 0.0;
    for (std::size_t i = 0; i < N; ++i)
        if (std::fabs(freq(i) - m.centreHz) < kSymbolRate) lobe += (psd[i] - m.n0) * df;
    m.signal = std::max(0.0, lobe / kMainLobeFraction);
    m.esn0Db = 10.0 * std::log10(m.signal / (m.n0 * kSymbolRate));
    m.ok = m.signal > 0.0;
    return m;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString path, outDir = QStringLiteral("fy2_snr_out"), satellite = QStringLiteral("FY-2G");
    double rate = 2.0e6, skip = 0.0, seconds = 60.0;
    bool measureOnly = false;
    QList<double> targets = {10, 8, 5, 3.5, 3, 2.5, 2.3, 2, 1.8, 1.5, 1.3};
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        auto next = [&] { return QString::fromLocal8Bit(argv[++i]); };
        if (a == QStringLiteral("--rate") && i + 1 < argc) rate = next().toDouble();
        else if (a == QStringLiteral("--skip") && i + 1 < argc) skip = next().toDouble();
        else if (a == QStringLiteral("--seconds") && i + 1 < argc) seconds = next().toDouble();
        else if (a == QStringLiteral("--out") && i + 1 < argc) outDir = next();
        else if (a == QStringLiteral("--satellite") && i + 1 < argc) satellite = next();
        else if (a == QStringLiteral("--measure-only")) measureOnly = true;
        else if (a == QStringLiteral("--snr") && i + 1 < argc) {
            targets.clear();
            for (const QString& s : next().split(QLatin1Char(','), Qt::SkipEmptyParts)) targets << s.toDouble();
        } else if (path.isEmpty()) path = a;
    }
    if (path.isEmpty()) {
        std::printf("usage: NadirStation_Fy2SnrSweep <recording.wav|.cs16|.cs8> [--rate Hz] [--skip s] [--seconds s] "
                    "[--snr list] [--satellite FY-2G] [--out dir] [--measure-only]\n");
        return 2;
    }
    const std::int64_t offset = dataOffset(path);
    std::ifstream in(path.toLocal8Bit().constData(), std::ios::binary);
    if (!in || offset < 0) { std::printf("cannot open %s\n", path.toLocal8Bit().constData()); return 3; }
    const std::size_t want = static_cast<std::size_t>(seconds * rate);
    // Raw int8 I/Q (.cs8, SatDump's default) is two bytes a sample; WAV and .cs16 are four.
    const bool cs8 = path.endsWith(QStringLiteral(".cs8"), Qt::CaseInsensitive);
    const int bytesPerSample = cs8 ? 2 : 4;
    in.seekg(offset + static_cast<std::int64_t>(skip * rate) * bytesPerSample, std::ios::beg);
    std::size_t got = 0;
    std::vector<cf> clean;
    if (cs8) {
        std::vector<std::int8_t> raw8(want * 2);
        in.read(reinterpret_cast<char*>(raw8.data()), static_cast<std::streamsize>(raw8.size()));
        got = static_cast<std::size_t>(in.gcount()) / 2;
        clean.resize(got);
        for (std::size_t i = 0; i < got; ++i)
            clean[i] = cf(raw8[2 * i] / 128.0f, raw8[2 * i + 1] / 128.0f);
    } else {
        std::vector<std::int16_t> raw(want * 2);
        in.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(raw.size() * 2));
        got = static_cast<std::size_t>(in.gcount()) / 4;
        clean.resize(got);
        for (std::size_t i = 0; i < got; ++i)
            clean[i] = cf(raw[2 * i] / 32768.0f, raw[2 * i + 1] / 32768.0f);
    }
    std::printf("file %s\nrate %.0f Hz  skip %.1f s  read %.1f s (%zu samples)\n",
                QFileInfo(path).fileName().toLocal8Bit().constData(), rate, skip, got / rate, got);

    const std::size_t measN = std::min<std::size_t>(got, static_cast<std::size_t>(10.0 * rate));
    const Measure m0 = measure(std::vector<cf>(clean.begin(), clean.begin() + measN), rate);
    if (!m0.ok) { std::printf("measurement failed (no sinc^2 nulls found)\n"); return 4; }
    std::printf("clean: centre %+.1f kHz  N0 %.4g /Hz  S %.4g  Es/N0 %.2f dB\n",
                m0.centreHz / 1e3, m0.n0, m0.signal, m0.esn0Db);
    if (measureOnly) return 0;

    QDir().mkpath(outDir);
    QFile summary(QDir(outDir).filePath(QStringLiteral("summary.csv")));
    summary.open(QIODevice::WriteOnly | QIODevice::Text);
    summary.write("target_db,input_measured_db,decoder_snr_mean_db,frames,parity_faults,lock_final,noise_std,folder\n");

    for (double t : targets) {
        double sigmaC = 0.0;   // per-component standard deviation of the added noise
        const double n0Total = m0.signal / (kSymbolRate * std::pow(10.0, t / 10.0));
        if (n0Total > m0.n0) sigmaC = std::sqrt((n0Total - m0.n0) * rate / 2.0);
        const QString label = QStringLiteral("snr_%1").arg(t, 0, 'f', 1);
        const QString dir = QDir(outDir).filePath(label);
        QDir().mkpath(dir);

        std::mt19937 rng(12345);
        std::normal_distribution<float> gauss(0.0f, static_cast<float>(sigmaC));
        // The measured input Es/N0 of this run, from its first ten seconds.
        std::vector<cf> head(clean.begin(), clean.begin() + measN);
        if (sigmaC > 0.0) for (auto& v : head) v += cf(gauss(rng), gauss(rng));
        const Measure mi = measure(head, rate);
        head = {};
        rng.seed(12345);

        auto decoder = std::make_unique<NadirStationDecoder>();
        decoder->setOutputRoot(dir);
        decoder->setSampleRate(rate);
        decoder->setSymbolRate(kSymbolRate);
        decoder->setSatelliteName(satellite);
        decoder->setSourceLabel(satellite);
        decoder->setDemodParameters(0.5f, 0.02f);
        decoder->setEnabled(true);
        double snrSum = 0.0;
        int snrN = 0, frames = 0, bad = 0, lock = 0;
        QObject::connect(decoder.get(), &NadirStationDecoder::demodSnrMetricsReady, &app,
                         [&](float snr, float, float) { snrSum += snr; ++snrN; });
        QObject::connect(decoder.get(), &NadirStationDecoder::decodeStats, &app,
                         [&](int l, int f, int b, float, float) { lock = l; frames = f; bad = b; });

        constexpr std::size_t block = 65536;   // the application's WAV block
        for (auto& g : nadirstation::g_stageSeconds) g.store(0.0);
        double decodeSeconds = 0.0;   // wall time inside the decoder only: noise generation is left out
        using Clock = std::chrono::steady_clock;
        for (std::size_t pos = 0; pos < got; pos += block) {
            const std::size_t n = std::min(block, got - pos);
            auto s = std::make_shared<std::vector<cf>>(clean.begin() + pos, clean.begin() + pos + n);
            if (sigmaC > 0.0) for (auto& v : *s) v += cf(gauss(rng), gauss(rng));
            const auto t0 = Clock::now();
            decoder->processSamples(s);
            decoder->drainQueuedSamples();
            decodeSeconds += std::chrono::duration<double>(Clock::now() - t0).count();
        }
        const auto tFinish = Clock::now();
        decoder->finishDecoding();
        QCoreApplication::processEvents();
        const double finishSeconds = std::chrono::duration<double>(Clock::now() - tFinish).count();
        decodeSeconds += finishSeconds;
        {
            const double signalSeconds = static_cast<double>(got) / rate;
            const double dem = nadirstation::g_stageSeconds[0].load();
            const double lck = nadirstation::g_stageSeconds[1].load();
            const double frm = nadirstation::g_stageSeconds[2].load();
            const double sft = nadirstation::g_stageSeconds[3].load();
            std::printf("  timing: %.2f s of signal, decoder wall %.2f s (%.2fx real time)  "
                        "demod %.2f  locker %.2f  frames %.2f (soft %.2f)  other %.2f  | end-of-run flush %.2f => streaming %.2fx real time\n",
                        signalSeconds, decodeSeconds, decodeSeconds / signalSeconds, dem, lck, frm, sft,
                        decodeSeconds - dem - lck - frm - finishSeconds, finishSeconds,
                        (decodeSeconds - finishSeconds) / signalSeconds);
        }
        const double snrMean = snrN ? snrSum / snrN : 0.0;
        std::printf("target %5.1f dB  input measured %6.2f dB  decoder SNR %6.2f dB  frames %5d  "
                    "parity-faults %6d  lock %d  noise std %.3g%s\n",
                    t, mi.esn0Db, snrMean, frames, bad, lock, sigmaC,
                    sigmaC == 0.0 ? "  (recording itself, no noise added)" : "");
        std::fflush(stdout);
        summary.write(QStringLiteral("%1,%2,%3,%4,%5,%6,%7,%8\n").arg(t).arg(mi.esn0Db, 0, 'f', 2)
                          .arg(snrMean, 0, 'f', 2).arg(frames).arg(bad).arg(lock).arg(sigmaC, 0, 'g', 4)
                          .arg(label).toUtf8());
        summary.flush();
        decoder.reset();
        QDirIterator it(dir, {QStringLiteral("*.png")}, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            std::printf("    %s  %lld bytes\n", QDir(dir).relativeFilePath(it.filePath()).toLocal8Bit().constData(),
                        static_cast<long long>(it.fileInfo().size()));
        }
    }
    return 0;
}
