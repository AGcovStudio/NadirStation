// GOES GVAR glue for NadirStationDecoder: samples in, link statistics / live view / finished
// observations out. The receive chain itself is in gvarengine.cpp.
#include "nadirstationdecoder.h"
#include "gvarengine.h"
#include "runtime_logger.h"
#include <QDateTime>
#include <algorithm>

void NadirStationDecoder::publishGvarUpdates() {
    if (!gvar_) return;
    auto live = gvar_->takeLive();
    if (!live.message.isEmpty()) {
        emit statusMessage(live.message);
    }
    if (!live.live.isNull()) emit hritLivePreviewReady(live.stamp, live.live, live.message);
    for (auto& done : gvar_->takeCompleted()) {
        if (!done.message.isEmpty()) { emit statusMessage(done.message); nadirstation::log::write(done.message); }
        if (!done.paths.isEmpty()) emit imageReady(done.stamp, done.paths);
    }
}

void NadirStationDecoder::publishGvarTelemetry(bool force) {
    if (!gvar_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && lastGvarTelemetryMs_ != 0 && now - lastGvarTelemetryMs_ < 1000) return;
    lastGvarTelemetryMs_ = now;
    nadirstation::gvar::Engine::Telemetry t;
    if (gvar_->takeTelemetry(t) && !t.block0.empty()) {
        QVector<int> stats{t.window, int(std::min<uint64_t>(t.blocks0, 2147483647u)), int(std::min<uint64_t>(t.parityOk, 2147483647u)), t.errBits, t.comparedBits};
        QVector<int> info;
        QList<QByteArray> payloads;
        for (const auto& p : t.products) {
            info << p.blockId << p.productId << p.wordSize << p.wordCount << int(std::min<uint64_t>(p.count, 2147483647u));
            payloads << QByteArray(reinterpret_cast<const char*>(p.bytes.data()), int(p.bytes.size()));
        }
        QVector<int> votes(t.votes.begin(), t.votes.end());
        emit gvarTelemetryUpdated(QByteArray(reinterpret_cast<const char*>(t.block0.data()), int(t.block0.size())),
                                  QByteArray(reinterpret_cast<const char*>(t.consensus.data()), int(t.consensus.size())), votes, stats, info, payloads);
    }
    gvar_->saveTelemetry(force);
}

void NadirStationDecoder::finalizeGvarObservation() {
    if (!gvar_) return;
    gvar_->finish();
    publishGvarUpdates();
    publishGvarTelemetry(true);
}

void NadirStationDecoder::processGvarSamples(const std::vector<std::complex<float>>& iq) {
    if (!gvar_ || !gvar_->configured()) {
        if (!lowRateReported_) {
            lowRateReported_ = true;
            emit statusMessage(QStringLiteral("GOES GVAR needs IQ at 2.75 MS/s or more (2.111 Msym/s, roll-off 0.5); set the sample rate to 3 MS/s or more."));
        }
        return;
    }
    hritIqSamples_ += iq.size();
    const auto demod = gvar_->process(iq);
    carrierOffsetHz_ = demod.carrierOffsetHz;
    publishConstellation(demod.symbols);
    publishGvarUpdates();
    publishGvarTelemetry(false);
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastStatsEmitMs_ == 0 || now - lastStatsEmitMs_ >= 200) {
        const auto good = gvar_->blocksGood(), bad = gvar_->blocksBad();
        const auto count = [](uint64_t n) { return int(std::min<uint64_t>(n, 2147483647u)); };
        // Locked while sync words keep arriving. The carrier measure (mean cos 2*phase error) falls with the
        // signal-to-noise ratio, to about 0.3 at 0 dB and 0.27 at -2 dB Es/N0, while the blocks still decode; a
        // fixed 0.3 called that "unlocked". A sync word needs a correct carrier and symbol clock over 10032
        // symbols, so one in the last 1.5 s (a block is 0.04..0.1 s) is the better test; the carrier measure only
        // counts when it is clearly locked, or at the very start.
        {
            const uint64_t syncs = gvar_->syncs();
            if (syncs != gvarLastSyncs_) { gvarLastSyncs_ = syncs; gvarLastSyncMs_ = now; }
            const bool recentSync = syncs > 0 && gvarLastSyncMs_ != 0 && now - gvarLastSyncMs_ <= 1500;
            lock_ = (recentSync || gvar_->lockMeasure() > 0.5f) ? 100 : 0;
        }
        emit decodeStats(lock_, count(good), count(bad), std::clamp(gvar_->lockMeasure(), 0.f, 1.f), carrierOffsetHz_);
        const float snr = float(gvar_->snrDb());
        peakDemodSnr_ = std::max(peakDemodSnr_, snr);
        emit demodSnrMetricsReady(snr, peakDemodSnr_, snr);
        lastStatsEmitMs_ = now;
        if (lastHritProgressStatusMs_ == 0 || now - lastHritProgressStatusMs_ > 5000) {
            const QString message = QStringLiteral("GOES GVAR: sync %1 · header CRC ok %2 / bad %3 · scan lines %4 · soft blocks %5 · queue %6 blocks")
                .arg(qulonglong(gvar_->syncs())).arg(qulonglong(good)).arg(qulonglong(bad))
                .arg(qulonglong(gvar_->linesStored())).arg(qulonglong(gvar_->softBlocks())).arg(gvar_->queuedBlocks());
            emit statusMessage(message);
            nadirstation::log::write(message + QStringLiteral(" · carrier quality %1% · offset %2 Hz · SNR %3 dB · IQ queue %4 samples")
                .arg(gvar_->lockMeasure() * 100.f, 0, 'f', 1).arg(carrierOffsetHz_, 0, 'f', 0)
                .arg(gvar_->snrDb(), 0, 'f', 1).arg(ingressQueuedSamples_.load()));
            lastHritProgressStatusMs_ = now;
        }
    }
}
