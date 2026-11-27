// Drive the real decoder with a recording, so "no image appears" can be reproduced rather than
// reasoned about.
//
// The application's decode path is not reachable from a command line, which is why every previous
// check of it stopped at "it compiles" and "it starts". NadirStationDecoder::processSamples is public
// though, and the signals the image page listens to are public too, so this builds the same object
// the application builds, connects the same signals, and feeds it the same samples from the same
// file. Whatever the application would show, this reports.
//
// It prints every signal that matters -- stats, preview paths, finished images -- together with
// whatever the decoder logs, so a failure says where it happened rather than only that it did.
//
// Usage: NadirStation_DecodeProbe <file.cs16|file.cs8> [--rate 2000000] [--symbol-rate 128000] [--out dir]
//                         [--polarity 1] [--seconds 60] [--skip s] [--satellite FY-2G]
// A .cs8 file is raw signed 8-bit I/Q (two bytes a sample); anything else is 16-bit.
// For an FY-2 S-VISSR recording: --satellite FY-2G --symbol-rate 660000 --rate <file's rate>.

#include "../src/nadirstationdecoder.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <complex>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    QString path, outDir = QStringLiteral("probe_out"), satellite = QStringLiteral("GK-2A");
    double rate = 2.0e6, symbolRate = 128000.0, seconds = 60.0;
    double skipSeconds = 0.0;
    double pllBw = 0.0;   // 0 leaves the decoder's own default
    std::size_t blockSamples = 1u << 20;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--rate") && i + 1 < argc)
            rate = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--symbol-rate") && i + 1 < argc)
            symbolRate = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--satellite") && i + 1 < argc)
            satellite = QString::fromLocal8Bit(argv[++i]);
        else if (a == QStringLiteral("--out") && i + 1 < argc)
            outDir = QString::fromLocal8Bit(argv[++i]);
        else if (a == QStringLiteral("--seconds") && i + 1 < argc)
            seconds = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--skip") && i + 1 < argc)
            skipSeconds = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--pll") && i + 1 < argc)
            pllBw = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--block") && i + 1 < argc)
            blockSamples = static_cast<std::size_t>(
                QString::fromLocal8Bit(argv[++i]).toLongLong());
        else if (path.isEmpty())
            path = a;
    }
    if (path.isEmpty()) {
        std::cout << "usage: NadirStation_DecodeProbe <file.cs16> [--rate Hz] [--symbol-rate Hz]\n"
                     "                       [--out dir] [--seconds N]\n";
        return 2;
    }

    NadirStationDecoder decoder;
    decoder.setOutputRoot(outDir);
    decoder.setSampleRate(rate);
    decoder.setSymbolRate(symbolRate);
    decoder.setSatelliteName(satellite);
    decoder.setSourceLabel(satellite);
    if (pllBw <= 0.0 && satellite.startsWith(QStringLiteral("FY"), Qt::CaseInsensitive)) pllBw = 0.02;   // the sweep tool's FY-2 setting
    if (pllBw > 0.0) {
        decoder.setDemodParameters(0.5f, static_cast<float>(pllBw));
        std::printf("  pll bandwidth %.4f\n", pllBw);
    }
    decoder.setEnabled(true);

    int statsCount = 0;
    int lastLock = -1;
    // Elapsed seconds for the stats callback, so the frame count can be read against time rather
    // than against the number of callbacks.
    double elapsedSeconds = 0.0;
    int lastFrames = 0;
    QObject::connect(&decoder, &NadirStationDecoder::decodeStats, &app,
                     [&](int lock, int frames, int bad, float q, float carrier) {
                         ++statsCount;
                         // Print on a time cadence, not a callback cadence, and always print the
                         // last value seen. A count that flattens shows where decoding stopped.
                         static double lastPrinted = -1.0;
                         const bool due = elapsedSeconds - lastPrinted >= 2.0;
                         if (due || lock != lastLock) {
                             lastPrinted = elapsedSeconds;
                             lastLock = lock;
                             std::printf("  t=%6.1f s  lock %3d  frames %5d (+%d)  "
                                         "parity-faults %5d  carrier %8.1f Hz\n",
                                         elapsedSeconds, lock, frames, frames - lastFrames, bad,
                                         carrier);
                             std::fflush(stdout);
                         }
                         lastFrames = frames;
                     });
    QObject::connect(&decoder, &NadirStationDecoder::imagePreviewReady, &app,
                     [](const QString& stamp, const QList<QString>& paths) {
                         std::printf("  PREVIEW %s  (%d path%s)\n", stamp.toLocal8Bit().constData(),
                                     static_cast<int>(paths.size()),
                                     paths.size() == 1 ? "" : "s");
                         for (const QString& p : paths)
                             std::printf("      %s  %s\n", p.toLocal8Bit().constData(),
                                         QFileInfo::exists(p) ? "exists" : "MISSING");
                         std::fflush(stdout);
                     });
    QObject::connect(&decoder, &NadirStationDecoder::imageReady, &app,
                     [](const QString& stamp, const QList<QString>& paths) {
                         std::printf("  IMAGE READY %s  (%d paths)\n",
                                     stamp.toLocal8Bit().constData(),
                                     static_cast<int>(paths.size()));
                         std::fflush(stdout);
                     });
    QObject::connect(&decoder, &NadirStationDecoder::demodSnrMetricsReady, &app,
                     [](float snr, float peak, float avg) {
                         static int n = 0;
                         if (++n <= 5 || n % 40 == 0)
                             std::printf("  SNR now %6.2f dB  peak %6.2f dB  avg %6.2f dB\n",
                                         snr, peak, avg);
                     });
    QObject::connect(&decoder, &NadirStationDecoder::statusMessage, &app,
                     [](const QString& text) {
                         std::printf("  status: %s\n", text.toLocal8Bit().constData());
                         std::fflush(stdout);
                     });

    std::ifstream in(path.toLocal8Bit().constData(), std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << path.toLocal8Bit().constData() << "\n";
        return 3;
    }

    // Set by --block; the application's WAV path uses 65536, far below the default here.
    // The skip is what makes this a test of the tail rather than of the whole file: seeking past
    // the part that decodes leaves a receiver starting from nothing on the part that does not.
    const bool cs8 = path.endsWith(QStringLiteral(".cs8"), Qt::CaseInsensitive) || path.endsWith(QStringLiteral(".s8"), Qt::CaseInsensitive) ||
                      path.endsWith(QStringLiteral(".c8"), Qt::CaseInsensitive);
    const std::size_t bytesPerComponent = cs8 ? 1 : sizeof(std::int16_t);
    const std::size_t skipSamples = static_cast<std::size_t>(skipSeconds * rate);
    if (skipSamples > 0)
        in.seekg(static_cast<std::streamoff>(skipSamples * 2 * bytesPerComponent), std::ios::beg);
    const std::size_t totalSamples = static_cast<std::size_t>(seconds * rate);
    std::size_t done = 0;
    std::vector<std::int16_t> raw(blockSamples * 2);
    std::vector<std::int8_t> raw8(cs8 ? blockSamples * 2 : 0);
    int blocks = 0;

    std::cout << "feeding " << path.toLocal8Bit().constData() << " at " << rate
              << " Hz, symbol rate " << symbolRate << ", " << seconds << " s\n";

    while (done < totalSamples) {
        const std::size_t want = std::min(blockSamples, totalSamples - done);
        std::size_t got = 0;
        if (cs8) {
            in.read(reinterpret_cast<char*>(raw8.data()), static_cast<std::streamsize>(want * 2));
            got = static_cast<std::size_t>(in.gcount()) / 2;
        } else {
            in.read(reinterpret_cast<char*>(raw.data()),
                    static_cast<std::streamsize>(want * 2 * sizeof(std::int16_t)));
            got = static_cast<std::size_t>(in.gcount()) / (2 * sizeof(std::int16_t));
        }
        if (got == 0) break;
        auto samples = std::make_shared<std::vector<std::complex<float>>>();
        samples->reserve(got);
        for (std::size_t i = 0; i < got; ++i) {
            if (cs8)
                samples->emplace_back(static_cast<float>(raw8[2 * i]) / 128.0f,
                                      static_cast<float>(raw8[2 * i + 1]) / 128.0f);
            else
                samples->emplace_back(static_cast<float>(raw[2 * i]) / 32768.0f,
                                      static_cast<float>(raw[2 * i + 1]) / 32768.0f);
        }
        decoder.processSamples(samples);
        // Drain before the next block. Handing over faster than the DSP consumes piles the whole
        // recording into the ingress queue and processes it at the end, which is a condition the
        // application never creates: a WAV plays at real time and Pluto delivers at the sample
        // rate, so the decoder keeps up. Draining makes this probe measure the decoder rather than
        // its own feeding rate.
        decoder.drainQueuedSamples();
        done += got;
        ++blocks;
        elapsedSeconds = static_cast<double>(done) / rate;
    }
    decoder.finishDecoding();

    std::cout << "\n" << blocks << " blocks, " << statsCount << " stats emissions\n";
    std::cout << "output root: " << QDir(outDir).absolutePath().toLocal8Bit().constData() << "\n";
    QDirIterator it(QDir(outDir).absolutePath(), QStringList{QStringLiteral("*")}, QDir::Files,
                    QDirIterator::Subdirectories);
    int files = 0;
    while (it.hasNext()) {
        it.next();
        std::printf("  %s  (%lld bytes)\n",
                    QDir(outDir).relativeFilePath(it.filePath()).toLocal8Bit().constData(),
                    static_cast<long long>(it.fileInfo().size()));
        ++files;
    }
    std::cout << files << " file(s) written\n";
    return 0;
}
