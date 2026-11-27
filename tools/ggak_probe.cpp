// Runs the application's GGAK decoder on the PlutoSDR (or on a recorded .cs16 file) and prints its
// status twice a second, so the ELEKTRO-L chain can be checked without the GUI.
//
// Usage: NadirStation_GgakProbe [--ip 192.168.3.10] [--freq 1693.0] [--rate 2500000] [--gain 60]
//                       [--seconds 30] [--file capture.cs16] [--input file.wav|.cs16|.cs8|.cu8] [--out dir]
//
// --file reads a .cs16 directly (scaled as 12-bit Pluto samples). --input plays the file through the
// application's own file input (WavReceiver), so its format detection, sample-rate lookup and
// scaling are exercised; --rate is then the fallback for a raw file that does not state its rate.
// Both print the exact count of frames delivered (from the frame signal, not the twice-a-second
// status line, which can miss the last frames of a short file).
#include "../src/nadirstationdecoder.h"
#include "../src/plutoreceiver.h"
#include "../src/wavreceiver.h"
#include "../src/ggak.h"
#include <QCoreApplication>
#include <QEventLoop>
#include <QFile>
#include <QTimer>
#include <atomic>
#include <cstdint>
#include <cstdio>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QString ip = QStringLiteral("192.168.3.10"), file, input, out = QStringLiteral("ggak_probe_out");
    double freqMHz = 1693.0, rate = 2500000.0, gain = 60.0, seconds = 30.0;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        auto next = [&]() { return i + 1 < argc ? QString::fromLocal8Bit(argv[++i]) : QString(); };
        if (a == "--ip") ip = next();
        else if (a == "--freq") freqMHz = next().toDouble();
        else if (a == "--rate") rate = next().toDouble();
        else if (a == "--gain") gain = next().toDouble();
        else if (a == "--seconds") seconds = next().toDouble();
        else if (a == "--file") file = next();
        else if (a == "--input") input = next();
        else if (a == "--out") out = next();
    }
    NadirStationDecoder decoder;
    decoder.setOutputRoot(out);
    decoder.setSampleRate(rate);
    decoder.setSymbolRate(5000.0);
    decoder.setSatelliteName(QStringLiteral("ELEKTRO-L GGAK"));
    decoder.setSourceLabel(file.isEmpty() && input.isEmpty() ? QStringLiteral("probe live") : QStringLiteral("probe file"));
    std::atomic<qulonglong> exactOk{0}, exactBad{0};
    QObject::connect(&decoder, &NadirStationDecoder::ggakFrames, &app, [&](const QByteArray& frames) {
        const auto* p = reinterpret_cast<const std::uint8_t*>(frames.constData());
        for (qsizetype off = 0; off + nadirstation::ggak::kFrameBytes <= frames.size(); off += nadirstation::ggak::kFrameBytes)
            (nadirstation::ggak::parseFrame(p + off).checksumOk ? exactOk : exactBad)++;
    }, Qt::DirectConnection);
    qulonglong lastGood = 0;
    QObject::connect(&decoder, &NadirStationDecoder::ggakStatus, &app,
                     [&](int sync, double carrier, double level, bool found, double snr, qulonglong good, qulonglong bad, const QString&) {
                         lastGood = good;
                         std::printf("  sync %d  carrier %s %9.1f Hz  level %5.1f dB  Eb/N0 %5.1f dB  frames %llu  checksum errors %llu\n",
                                     sync, found ? "found" : "-----", carrier, level, snr, good, bad);
                         std::fflush(stdout);
                     }, Qt::DirectConnection);
    if (!input.isEmpty()) {
        WavReceiver rx;
        rx.setFilePath(input);
        rx.setPlaybackFactor(1.0e9);
        rx.setFraction(1.0);
        rx.setRawFallbackRate(rate);
        QObject::connect(&rx, &WavReceiver::statusMessage, &app, [](const QString& t) {
            std::printf("  input: %s\n", t.toLocal8Bit().constData()); std::fflush(stdout); }, Qt::DirectConnection);
        if (!rx.prepare()) { std::printf("cannot use %s\n", qPrintable(input)); return 2; }
        std::printf("input format %s, %.0f Hz from %s, %lld samples\n", WavReceiver::formatName(rx.format()),
                    rx.sampleRate(), qPrintable(rx.rateSource()), static_cast<long long>(rx.totalFrames()));
        decoder.setSampleRate(rx.sampleRate());
        decoder.setIngressBackpressureEnabled(true);
        decoder.setEnabled(true);
        QObject::connect(&rx, &WavReceiver::samplesReady, &decoder, &NadirStationDecoder::processSamples, Qt::DirectConnection);
        QEventLoop loop;
        QObject::connect(&rx, &WavReceiver::finished, &loop, &QEventLoop::quit, Qt::QueuedConnection);
        if (!rx.start()) { std::printf("cannot start %s\n", qPrintable(input)); return 2; }
        loop.exec();
        rx.stop();
        decoder.setEnabled(false);
        std::printf("input done: exact frames ok %llu, checksum errors %llu\n", exactOk.load(), exactBad.load());
        return 0;
    }

    decoder.setEnabled(true);

    if (!file.isEmpty()) {
        // Hold the reader at the decoder's high-water mark. Without this the whole file is queued at
        // disk speed and, once the queue limit is passed, the oldest blocks are dropped, so the
        // frame count measured CPU speed rather than the decoder (night gate, 2026-10-18).
        decoder.setIngressBackpressureEnabled(true);
        QFile f(file);
        if (!f.open(QIODevice::ReadOnly)) { std::printf("cannot read %s\n", qPrintable(file)); return 2; }
        std::vector<std::int16_t> raw(65536 * 2);
        qint64 n;
        while ((n = f.read(reinterpret_cast<char*>(raw.data()), qint64(raw.size() * 2))) > 0) {
            const std::size_t samples = static_cast<std::size_t>(n) / 4;
            auto block = std::make_shared<std::vector<std::complex<float>>>(samples);
            for (std::size_t i = 0; i < samples; ++i) (*block)[i] = {raw[2 * i] / 2048.0f, raw[2 * i + 1] / 2048.0f};
            decoder.processSamples(block);
            QCoreApplication::processEvents();
        }
        decoder.setEnabled(false);
        std::printf("file done: frames ok %llu\n", lastGood);
        std::printf("file done: exact frames ok %llu, checksum errors %llu\n", exactOk.load(), exactBad.load());
        return 0;
    }

    PlutoReceiver rx;
    rx.setIp(ip);
    rx.setParameters(freqMHz * 1e6, rate, 1000000.0, gain);
    long long spectra = 0;
    QObject::connect(&rx, &PlutoReceiver::spectrumReady, &rx, [&](const std::vector<float>&, double) { ++spectra; }, Qt::DirectConnection);
    QObject::connect(&rx, &PlutoReceiver::statusMessage, &app, [](const QString& t) {
        std::printf("  status: %s\n", t.toLocal8Bit().constData()); std::fflush(stdout); });
    QObject::connect(&rx, &PlutoReceiver::samplesReady, &decoder, &NadirStationDecoder::processSamples, Qt::DirectConnection);
    decoder.setIngressBackpressureEnabled(true);
    rx.start();
    QTimer::singleShot(int(seconds * 1000), &app, [&] {
        rx.stop();
        decoder.setEnabled(false);
        std::printf("done: frames ok %llu, spectrum frames from the receiver %lld\n", lastGood, spectra);
        std::fflush(stdout);
        QTimer::singleShot(1500, &app, [] { QCoreApplication::quit(); });
    });
    return app.exec();
}
