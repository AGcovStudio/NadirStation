// ELEKTRO-L LRIT / HRIT receive path of NadirStationDecoder.
//
//   LRIT  BPSK 294 ksym/s, RRC 0.5: pskDemod_ (the BPSK demodulator also used for GK-2A LRIT)
//         -> soft symbols -> LritDecoder (soft Viterbi, tolerant ASM search, RS with soft rescue)
//   HRIT  OQPSK 1.15 Msym/s, RRC 0.85 (the pulses are close to rectangular): QpskDemod in offset mode (Q sampled half a symbol after I)
//         -> HritDecoder with offset pairing (8 I/Q mappings) on its own thread
//   both  -> RS-checked CADUs -> LritFileAssembler (loss tolerant) -> elektro::XritProducts
//
// Checked on off-air recordings (2026-11-14 LRIT at 3 MS/s and 2026-11-15 HRIT
// at 5 MS/s): every CADU in lock passed Reed-Solomon. The HRIT link pairs I_k with Q_(k-1)
// (mapping 4), the LRIT link needs no G2 inversion.
#include "nadirstationdecoder.h"
#include "runtime_logger.h"
#include <algorithm>
#include <cmath>

void NadirStationDecoder::handleElektroFrame(nadirstation::gk2a::Cadu&& frame) {
    if (!frame.rsOk) return;
    for (auto& file : elektroAssembler_.feed(frame.bytes)) {
        ++elektroFiles_;
        if (elektroProducts_) elektroProducts_->submit(std::move(file), outputRoot_);
    }
}

void NadirStationDecoder::publishElektro() {
    if (!elektroProducts_) return;
    auto u = elektroProducts_->take();
    if (!u.message.isEmpty()) emit statusMessage(u.message);
    if (!u.preview.isNull()) emit hritLivePreviewReady(u.stamp, u.preview, u.previewInfo);
    for (const auto& [stamp, paths] : u.completed) emit imageReady(stamp, paths);
}

void NadirStationDecoder::finalizeElektro() {
    if (isElektroHrit())
        elektroHrit_.drain([this](nadirstation::gk2a::Cadu&& c) { handleElektroFrame(std::move(c)); });
    for (auto& file : elektroAssembler_.flush()) {
        ++elektroFiles_;
        if (elektroProducts_) elektroProducts_->submit(std::move(file), outputRoot_);
    }
    if (elektroProducts_) elektroProducts_->finalize();
    publishElektro();
    const auto& stats = isElektroHrit() ? elektroHrit_.stats() : elektroLrit_.stats();
    nadirstation::log::write(QStringLiteral("ELEKTRO-L %1 finished: RS ok %2 / sync %3, corrected %4, packets %5, files %6, image segments %7, fill %8, CRC fail %9")
        .arg(isElektroHrit() ? QStringLiteral("HRIT") : QStringLiteral("LRIT"))
        .arg(stats.cadusWithGoodRs).arg(stats.cadusWithSync).arg(stats.cadusCorrected)
        .arg(elektroAssembler_.packetsSeen).arg(elektroFiles_)
        .arg(elektroProducts_ ? elektroProducts_->segmentsDecoded() : 0)
        .arg(elektroAssembler_.fillerCadus).arg(elektroAssembler_.crcFailures));
}

void NadirStationDecoder::processElektroSamples(const std::vector<std::complex<float>>& iq) {
    const bool hrit = isElektroHrit();
    const QString service = hrit ? QStringLiteral("HRIT") : QStringLiteral("LRIT");
    const double minRate = symbolRate_ * (hrit ? 1.6 : 1.5);
    if (sampleRate_ < minRate) {
        if (!lowRateReported_) {
            lowRateReported_ = true;
            emit statusMessage(QStringLiteral("ELEKTRO-L %1 needs IQ at %2 MS/s or more.").arg(service).arg(minRate / 1e6, 0, 'f', 2));
        }
        return;
    }
    if (!elektroProducts_) elektroProducts_ = std::make_unique<nadirstation::elektro::XritProducts>(service);

    elektroStreamMs_ += 1000.0 * double(iq.size()) / sampleRate_;
    std::vector<std::complex<float>> symbols;
    float lockMeasure = 0.0f;
    if (hrit) {
        auto d = elektroOqpsk_.process(iq);
        carrierOffsetHz_ = d.carrierOffsetHz;
        lockMeasure = elektroOqpsk_.lockMeasure();
        symbols = std::move(d.symbols);
        publishConstellation(symbols);
        elektroHrit_.feed(symbols, [this](nadirstation::gk2a::Cadu&& c) { handleElektroFrame(std::move(c)); });
    } else {
        auto d = pskDemod_.process(iq);
        carrierOffsetHz_ = d.carrierOffsetHz;
        lockMeasure = pskDemod_.lockMeasure();
        symbols = std::move(d.symbols);
        publishConstellation(symbols);
        // Same soft scale as GK-2A LRIT: half the normalised amplitude keeps noisy symbols inside
        // the Viterbi decoder's range instead of clipping them.
        std::vector<float> soft;
        soft.reserve(symbols.size());
        for (const auto& z : symbols) soft.push_back(-0.5f * z.real());
        elektroLrit_.feedSoftBits(soft, false);
        for (auto& c : elektroLrit_.takeCadus())
            if (c.syncFound) handleElektroFrame(std::move(c));
    }

    // Symbol SNR, M2M4 with exponential averages (as SatDump shows it); valid for BPSK and QPSK.
    {
        constexpr double alpha = 0.001, beta = 1.0 - alpha;
        double y1 = elektroSnrY1_, y2 = elektroSnrY2_;
        for (const auto& z : symbols) {
            const double m = double(z.real()) * z.real() + double(z.imag()) * z.imag();
            y1 = alpha * m + beta * y1;
            y2 = alpha * m * m + beta * y2;
        }
        elektroSnrY1_ = std::isfinite(y1) ? y1 : 0.0;
        elektroSnrY2_ = std::isfinite(y2) ? y2 : 0.0;
    }
    publishElektro();
    elektroCheckLock((hrit ? elektroHrit_.stats() : elektroLrit_.stats()).cadusWithGoodRs);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastStatsEmitMs_ != 0 && now - lastStatsEmitMs_ < 200) return;
    lastStatsEmitMs_ = now;
    const auto& stats = hrit ? elektroHrit_.stats() : elektroLrit_.stats();
    const bool locked = hrit ? elektroHrit_.locked() : elektroLrit_.frameLocked();
    lock_ = locked ? 100 : (stats.cadusWithSync > 0 ? 50 : 0);
    const auto count = [](std::size_t n) { return int(std::min<std::size_t>(n, 2147483647)); };
    emit decodeStats(lock_, count(stats.cadusWithGoodRs), count(stats.cadusWithSync - std::min(stats.cadusWithSync, stats.cadusWithGoodRs)),
                     float(std::clamp(lockMeasure, 0.0f, 1.0f)), carrierOffsetHz_);
    {
        const double signal = std::sqrt(std::max(0.0, 2.0 * elektroSnrY1_ * elektroSnrY1_ - elektroSnrY2_));
        const double noise = elektroSnrY1_ - signal;
        const float snr = (signal > 0.0 && noise > 1e-12) ? float(std::max(0.0, 10.0 * std::log10(signal / noise))) : 0.0f;
        peakDemodSnr_ = std::max(peakDemodSnr_, snr);
        emit demodSnrMetricsReady(snr, peakDemodSnr_, snr);
    }
    if (elektroProgressMs_ == 0 || now - elektroProgressMs_ > 5000) {
        elektroProgressMs_ = now;
        const QString msg = stats.cadusWithGoodRs == 0
            ? QStringLiteral("ELEKTRO-L %1: searching; carrier quality %2%, offset %3 Hz")
                  .arg(service).arg(lockMeasure * 100.0f, 0, 'f', 1).arg(carrierOffsetHz_, 0, 'f', 0)
            : QStringLiteral("ELEKTRO-L %1: RS ok %2 / sync %3; packets %4; files %5; segments %6")
                  .arg(service).arg(stats.cadusWithGoodRs).arg(stats.cadusWithSync)
                  .arg(elektroAssembler_.packetsSeen).arg(elektroFiles_)
                  .arg(elektroProducts_ ? elektroProducts_->segmentsDecoded() : 0);
        const QString losses = elektroProducts_ && (elektroProducts_->skippedImages() || elektroProducts_->archiveFailures())
            ? QStringLiteral("; images skipped %1, archive failures %2")
                  .arg(elektroProducts_->skippedImages()).arg(elektroProducts_->archiveFailures())
            : QString();
        emit statusMessage(msg + losses);
        nadirstation::log::write(msg + losses + QStringLiteral("; carrier quality %1% · offset %2 Hz · fill %3 · VC gaps %4 · CRC fail %5")
            .arg(lockMeasure * 100.0f, 0, 'f', 1).arg(carrierOffsetHz_, 0, 'f', 0)
            .arg(elektroAssembler_.fillerCadus).arg(elektroAssembler_.frameDiscontinuities).arg(elektroAssembler_.crcFailures));
    }
}

// Loss of lock and re-acquisition. The demodulators re-measure the carrier on their own only after a
// long stretch with no lock indication, and the OQPSK lock measure is weak at low SNR, so the frame
// layer decides: no CADU through Reed-Solomon for 2.5 s after frames were coming (a fade, a
// retune, a carrier step, a sample-rate glitch) or for 6 s from the start, and the carrier is
// measured again, then every 1.5 s (six times) and every 10 s after that. Successive attempts
// alternate the search span (+-80, +-25, +-150 kHz) so a spur that won once does not win every time.
// Tested: signal gone for 3 s, back 6 kHz off -> frames again 1.5 to 3 s after it returned.
void NadirStationDecoder::elektroCheckLock(std::size_t goodNow) {
    if (goodNow != elektroGoodSeen_) {
        if (!elektroEverLocked_ || elektroReacq_ > 0) {
            const QString msg = QStringLiteral("ELEKTRO-L %1: frames %2 (carrier %3 Hz)")
                .arg(isElektroHrit() ? QStringLiteral("HRIT") : QStringLiteral("LRIT"))
                .arg(elektroReacq_ > 0 ? QStringLiteral("back after re-acquisition %1").arg(elektroReacq_) : QStringLiteral("locked"))
                .arg(carrierOffsetHz_, 0, 'f', 0);
            nadirstation::log::write(msg);
            emit statusMessage(msg);
        }
        elektroGoodSeen_ = goodNow;
        elektroLastGoodMs_ = elektroStreamMs_;
        elektroEverLocked_ = true;
        elektroReacq_ = 0;
        return;
    }
    const double silent = elektroStreamMs_ - elektroLastGoodMs_;
    const double sinceAttempt = elektroStreamMs_ - elektroLastReacqMs_;
    const double first = elektroEverLocked_ ? 2500.0 : 6000.0;
    const double every = elektroReacq_ < 6 ? 1500.0 : 10000.0;
    if (silent < first || sinceAttempt < every) return;
    ++elektroReacq_;
    elektroLastReacqMs_ = elektroStreamMs_;
    static constexpr float kSpans[] = {kElektroAcqMaxHz, 25000.0f, 150000.0f};
    const float span = kSpans[elektroReacq_ % 3];
    if (isElektroHrit()) {
        elektroOqpsk_.setAcquisition(kElektroAcqBlocks, span);
        elektroOqpsk_.reset();
    } else {
        pskDemod_.setInitialCarrierOffset(0.0f);
        pskDemod_.reset();
        elektroLrit_.restartStream();
    }
    const QString msg = QStringLiteral("ELEKTRO-L %1: no frames for %2 s; carrier re-acquisition %3 (search +-%4 kHz)")
        .arg(isElektroHrit() ? QStringLiteral("HRIT") : QStringLiteral("LRIT"))
        .arg(silent / 1000.0, 0, 'f', 1).arg(elektroReacq_).arg(span / 1000.0, 0, 'f', 0);
    nadirstation::log::write(msg);
    emit statusMessage(msg);
}
