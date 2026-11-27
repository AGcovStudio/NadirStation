// GOES-N/O/P Sensor Data glue for NadirStationDecoder. The receive chain is in goessd.cpp.
#include "nadirstationdecoder.h"
#include "goessd.h"
#include "runtime_logger.h"
#include <QDateTime>
#include <algorithm>

void NadirStationDecoder::publishGoesSdUpdates() {
    if (!goessd_) return;
    auto live = goessd_->takeLive();
    if (!live.message.isEmpty()) emit statusMessage(live.message);
    if (!live.live.isNull()) emit hritLivePreviewReady(live.stamp, live.live, live.message);
    for (auto& done : goessd_->takeCompleted()) {
        if (!done.message.isEmpty()) { emit statusMessage(done.message); nadirstation::log::write(done.message); }
        if (!done.paths.isEmpty()) emit imageReady(done.stamp, done.paths);
    }
}

void NadirStationDecoder::finalizeGoesSdObservation() {
    if (!goessd_) return;
    goessd_->finish();
    publishGoesSdUpdates();
}

void NadirStationDecoder::processGoesSdSamples(const std::vector<std::complex<float>>& iq) {
    if (!goessd_ || !goessd_->configured()) {
        if (!lowRateReported_) {
            lowRateReported_ = true;
            emit statusMessage(QStringLiteral("GOES SD needs IQ at 3 MS/s or more (imager 2.6208 Mbit/s); set the sample rate to 6 MS/s."));
        }
        return;
    }
    hritIqSamples_ += iq.size();
    const auto demod = goessd_->process(iq);
    carrierOffsetHz_ = demod.carrierOffsetHz;
    publishConstellation(demod.symbols);
    publishGoesSdUpdates();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastStatsEmitMs_ == 0 || now - lastStatsEmitMs_ >= 200) {
        const auto count = [](uint64_t n) { return int(std::min<uint64_t>(n, 2147483647u)); };
        const uint64_t syncs = goessd_->syncs();
        if (syncs != goesSdLastSyncs_) { goesSdLastSyncs_ = syncs; goesSdLastSyncMs_ = now; }
        lock_ = (goessd_->imagerLocked() && goesSdLastSyncMs_ != 0 && now - goesSdLastSyncMs_ <= 1500) ? 100 : 0;
        emit decodeStats(lock_, count(goessd_->scans()), count(goessd_->widthOutliers()), std::clamp(goessd_->lockMeasure(), 0.f, 1.f), carrierOffsetHz_);
        const float snr = float(goessd_->snrDb());
        peakDemodSnr_ = std::max(peakDemodSnr_, snr);
        emit demodSnrMetricsReady(snr, peakDemodSnr_, snr);
        lastStatsEmitMs_ = now;
        if (lastHritProgressStatusMs_ == 0 || now - lastHritProgressStatusMs_ > 5000) {
            const QString message = QStringLiteral("GOES SD: sync BER %1% · blocks %2 · scans %3 · direction fixes %4 · sounder frames %5 (parity %6%)")
                .arg(goessd_->syncBitErrorPct(), 0, 'f', 1).arg(qulonglong(goessd_->blocks())).arg(qulonglong(goessd_->scans()))
                .arg(qulonglong(goessd_->directionFixes())).arg(qulonglong(goessd_->sounderFrames()))
                .arg(goessd_->sounderParityPct(), 0, 'f', 0);
            emit statusMessage(message);
            nadirstation::log::write(message + QStringLiteral(" · offset %1 Hz · SNR %2 dB")
                .arg(carrierOffsetHz_, 0, 'f', 0).arg(goessd_->snrDb(), 0, 'f', 1));
            lastHritProgressStatusMs_ = now;
        }
    }
}
