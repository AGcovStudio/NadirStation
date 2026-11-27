// Records IQ from the PlutoSDR through the application's own PlutoReceiver, so what is written is
// exactly what the decoder and the spectrum receive. Also counts the spectrum frames the receiver
// emits, which shows whether IQ reaches the FFT display at all.
//
// Usage: NadirStation_PlutoCapture --out file.cs16 [--ip 192.168.3.10] [--freq 1693.0] [--rate 2500000]
//                          [--bw 1000000] [--gain 50] [--seconds 10]
// Output: interleaved int16 I/Q (the Pluto's 12-bit values as delivered), plus file.txt with stats.
#include "../src/plutoreceiver.h"
#include <QCoreApplication>
#include <QFile>
#include <QTimer>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString ip = QStringLiteral("192.168.3.10"), out;
    double freqMHz = 1693.0, rate = 2500000.0, bw = 1000000.0, gain = 50.0, seconds = 10.0;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        auto next = [&]() { return i + 1 < argc ? QString::fromLocal8Bit(argv[++i]) : QString(); };
        if (a == "--ip") ip = next();
        else if (a == "--freq") freqMHz = next().toDouble();
        else if (a == "--rate") rate = next().toDouble();
        else if (a == "--bw") bw = next().toDouble();
        else if (a == "--gain") gain = next().toDouble();
        else if (a == "--seconds") seconds = next().toDouble();
        else if (a == "--out") out = next();
    }
    if (out.isEmpty()) { std::printf("--out required\n"); return 2; }
    QFile f(out);
    if (!f.open(QIODevice::WriteOnly)) { std::printf("cannot write %s\n", qPrintable(out)); return 2; }
    std::printf("capture %s  %.4f MHz  rate %.0f  bw %.0f  gain %.1f  %.1f s\n",
                qPrintable(ip), freqMHz, rate, bw, gain, seconds);
    std::fflush(stdout);

    std::atomic<long long> samples{0}, blocks{0}, spectra{0}, clipped{0};
    std::atomic<double> sumSq{0.0};
    std::mutex fileLock;
    double lastRate = 0.0;
    PlutoReceiver rx;
    rx.setIp(ip);
    rx.setParameters(freqMHz * 1e6, rate, bw, gain);
    QObject::connect(&rx, &PlutoReceiver::statusMessage, &app, [](const QString& t) {
        std::printf("  status: %s\n", t.toLocal8Bit().constData()); std::fflush(stdout); });
    QObject::connect(&rx, &PlutoReceiver::deviceConnected, &app, [](bool c) {
        std::printf("  device %s\n", c ? "connected" : "NOT connected"); std::fflush(stdout); });
    QObject::connect(&rx, &PlutoReceiver::spectrumReady, &rx,
                     [&](const std::vector<float>& db, double sr) { if (!db.empty()) ++spectra; lastRate = sr; },
                     Qt::DirectConnection);
    QObject::connect(&rx, &PlutoReceiver::samplesReady, &rx,
                     [&](const std::shared_ptr<std::vector<std::complex<float>>>& p) {
                         std::vector<std::int16_t> buf(p->size() * 2);
                         double s2 = 0.0; long long clip = 0;
                         for (std::size_t i = 0; i < p->size(); ++i) {
                             const float I = (*p)[i].real() * 2048.0f, Q = (*p)[i].imag() * 2048.0f;
                             buf[2 * i] = static_cast<std::int16_t>(std::lround(I));
                             buf[2 * i + 1] = static_cast<std::int16_t>(std::lround(Q));
                             s2 += double(I) * I + double(Q) * Q;
                             if (std::fabs(I) >= 2040.0f || std::fabs(Q) >= 2040.0f) ++clip;
                         }
                         {
                             std::lock_guard<std::mutex> g(fileLock);
                             f.write(reinterpret_cast<const char*>(buf.data()), qint64(buf.size() * 2));
                         }
                         samples += static_cast<long long>(p->size());
                         ++blocks;
                         clipped += clip;
                         sumSq = sumSq.load() + s2;
                     }, Qt::DirectConnection);
    rx.start();
    QTimer tick;
    QObject::connect(&tick, &QTimer::timeout, &app, [&] {
        const long long n = samples.load();
        std::printf("  t  samples %lld  blocks %lld  spectra %lld  rms %.1f  clipped %lld\n", n, blocks.load(),
                    spectra.load(), n ? std::sqrt(sumSq.load() / double(n)) : 0.0, clipped.load());
        std::fflush(stdout);
    });
    tick.start(1000);
    QTimer::singleShot(int(seconds * 1000), &app, [&] {
        rx.stop();
        QTimer::singleShot(1500, &app, [&] {
            std::lock_guard<std::mutex> g(fileLock);
            f.close();
            const long long n = samples.load();
            std::printf("done: samples %lld (%.2f s at %.0f)  blocks %lld  spectra %lld  rms %.1f  clipped %lld\n",
                        n, n / rate, rate, blocks.load(), spectra.load(),
                        n ? std::sqrt(sumSq.load() / double(n)) : 0.0, clipped.load());
            std::fflush(stdout);
            QCoreApplication::quit();
        });
    });
    return app.exec();
}
