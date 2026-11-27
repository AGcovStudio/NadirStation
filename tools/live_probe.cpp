// Drive the decoder from the live receiver, so the sample-rate behaviour can be measured rather
// than argued about.
//
// Every test in this project drove the decoder from a file, and the one fault that mattered -- the
// rates at which the carrier loop does and does not reach a carrier -- is a property of the loop and
// the rate and cannot be seen from a recording at a single rate. This connects to the receiver, sets
// a rate, feeds the decoder exactly as the application does, and says whether it locked.
//
// Usage: NadirStation_LiveProbe [--ip 192.168.3.10] [--freq 1692.14] [--rate 2000000] [--bw 1000000]
//                       [--gain 35] [--symbol-rate 128000] [--seconds 25]

#include "../src/nadirstationdecoder.h"
#include "../src/plutoreceiver.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <cstdio>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);

    QString ip = QStringLiteral("192.168.3.10");
    double freqMHz = 1692.14, rate = 2000000.0, bw = 1000000.0, gain = 35.0;
    double symbolRate = 128000.0, seconds = 25.0;
    double restartAt = 0.0;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        auto next = [&]() { return i + 1 < argc ? QString::fromLocal8Bit(argv[++i]) : QString(); };
        if (a == QStringLiteral("--ip")) ip = next();
        else if (a == QStringLiteral("--freq")) freqMHz = next().toDouble();
        else if (a == QStringLiteral("--rate")) rate = next().toDouble();
        else if (a == QStringLiteral("--bw")) bw = next().toDouble();
        else if (a == QStringLiteral("--gain")) gain = next().toDouble();
        else if (a == QStringLiteral("--symbol-rate")) symbolRate = next().toDouble();
        else if (a == QStringLiteral("--seconds")) seconds = next().toDouble();
        else if (a == QStringLiteral("--restart")) restartAt = next().toDouble();
    }

    std::printf("receiver %s   %.3f MHz   rate %.0f Hz   bw %.0f Hz   gain %.1f dB\n",
                ip.toLocal8Bit().constData(), freqMHz, rate, bw, gain);
    std::printf("symbol rate %.0f Hz   listening %.0f s\n", symbolRate, seconds);

    NadirStationDecoder decoder;
    decoder.setOutputRoot(QStringLiteral("live_probe_out"));
    decoder.setSampleRate(rate);
    decoder.setSymbolRate(symbolRate);
    decoder.setSatelliteName(QStringLiteral("GK-2A"));
    decoder.setSourceLabel(QStringLiteral("live"));

    int acquired = 0;
    int frames = 0;
    int framesBeforeRestart = -1;
    bool restarted = false;
    int strips = 0;
    int bestLock = 0;
    QObject::connect(&decoder, &NadirStationDecoder::statusMessage, &app, [](const QString& t) {
        if (t.contains(QStringLiteral("acquired")) || t.contains(QStringLiteral("nothing for")))
            std::printf("  %s\n", t.toLocal8Bit().constData());
    });
    QObject::connect(&decoder, &NadirStationDecoder::decodeStats, &app,
                     [&](int lock, int f, int bad, float q, float carrier) {
                         bestLock = std::max(bestLock, lock);
                         // The decoder's counter restarts with the decoder, so the running total is
                         // kept here instead of read from it.
                         if (restarted && framesBeforeRestart < 0) framesBeforeRestart = frames;
                         frames = framesBeforeRestart >= 0 ? framesBeforeRestart + f : f;
                         if (lock > 0 && acquired == 0) ++acquired;
                         std::printf("  lock %3d   frames %5d   parity-faults %6d   quality %.3f"
                                     "   carrier %8.1f Hz\n",
                                     lock, f, bad, q, carrier);
                         std::fflush(stdout);
                     });
    QObject::connect(&decoder, &NadirStationDecoder::imageReady, &app,
                     [&](const QString&, const QList<QString>& paths) { strips += paths.size(); });

    PlutoReceiver receiver;
    receiver.setIp(ip);
    receiver.setParameters(freqMHz * 1e6, rate, bw, gain);
    QObject::connect(&receiver, &PlutoReceiver::deviceConnected, &app, [](bool c) {
        std::printf("  device %s\n", c ? "connected" : "NOT connected");
        std::fflush(stdout);
    });
    QObject::connect(&receiver, &PlutoReceiver::samplesReady, &decoder,
                     &NadirStationDecoder::processSamples, Qt::DirectConnection);

    // The application enables backpressure before the receiver starts, so that a DSP that cannot
    // keep up stalls the capture thread rather than letting a queue grow without bound. Without it a
    // long run accumulates every block it could not process.
    bool backpressure = true;
    for (int i = 1; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--no-backpressure")) backpressure = false;
    }
    decoder.setIngressBackpressureEnabled(backpressure);
    std::printf("backpressure %s\n", backpressure ? "on (as the application uses it)" : "off");
    std::fflush(stdout);
    decoder.setEnabled(true);
    if (!receiver.start()) {
        std::printf("receiver failed to start\n");
        return 3;
    }

    if (restartAt > 0.0) {
        QTimer::singleShot(static_cast<int>(restartAt * 1000), &app, [&] {
            std::printf("\n  -- stopping the decoder --\n");
            std::fflush(stdout);
            decoder.setEnabled(false);
            QTimer::singleShot(2000, &app, [&] {
                std::printf("  -- starting it again --\n\n");
                std::fflush(stdout);
                restarted = true;
                decoder.setEnabled(true);
            });
        });
    }
    QTimer::singleShot(static_cast<int>(seconds * 1000), &app, [&] {
        decoder.setEnabled(false);
        receiver.stop();
        std::printf("\nresult: rate %.0f Hz  best lock %d  frames %d  products %d  => %s\n",
                    rate, bestLock, frames, strips,
                    bestLock >= 50 ? "LOCKED" : "NO LOCK");
        if (framesBeforeRestart >= 0)
            std::printf("        before the restart %d frames, after it %d\n",
                        framesBeforeRestart, frames - framesBeforeRestart);
        if (restartAt > 0.0 && framesBeforeRestart >= 0
            && frames - framesBeforeRestart <= 0)
            std::printf("        THE SECOND SESSION DECODED NOTHING\n");
        QCoreApplication::quit();
    });
    return app.exec();
}
