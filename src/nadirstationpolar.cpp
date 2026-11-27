// MetOp AHRPT / METEOR HRPT receive path of NadirStationDecoder (formats and DSP: polarhrpt.h).
//
//   MetOp   QpskDemod (RRC 0.5, 2.3333 Msym/s, carrier search +-100 kHz for Doppler) -> HritDecoder
//           (soft Viterbi, tolerant sync, RS) on its own thread -> AVHRR/3 lines (VCID 9)
//   METEOR  MeteorDemod (carrier PLL on the residual carrier, Manchester matched filter, M&M clock)
//           -> soft bits -> MeteorDeframer -> MsuMrReader -> MSU-MR lines
//   both    -> Imager: the picture grows on the Live page while the pass comes in; when the lines
//              stop for 30 s (end of pass) or reception ends, every channel and the composites are
//              written to <root>/<MetOp|METEOR>/<time of the first line>/ and shown on the Images page.
//
// Simulated thresholds (synthetic signal made from the real CADUs of our own passes, Doppler
// 23 kHz drifting 400 Hz/s, 30 ppm clock error): MetOp 99 % of CADUs at Es/N0 1.5 dB (6 MS/s);
// METEOR within 0.1 dB of a receiver that knows the carrier and the clock exactly.
#include "nadirstationdecoder.h"
#include "runtime_logger.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QTimeZone>
#include <QStandardPaths>
#include <algorithm>

#include "passpredict.h"
#include <cmath>

namespace {
constexpr double kMetopSymbolRate = 2333333.0;
constexpr double kMeteorBitRate = 665400.0;
constexpr double kPassEndSilenceMs = 30000.0;
}

void NadirStationDecoder::configurePolar() {
    metopDecoder_.setAsync(false);
    metopDecoder_.reset();
    if (isMetop()) {
        metopDecoder_.setPunctured34(true);   // AHRPT: K=7 code punctured to rate 3/4
        metopDecoder_.setAsync(true);
        metopDemod_.setAcquisition(8, 100000.0f);
        if (sampleRate_ >= 1.05 * kMetopSymbolRate)
            metopDemod_.configure(float(sampleRate_), float(kMetopSymbolRate), 0.5f, 0.0003f, 0.0002f);
    }
    if (isMeteorHrpt() && sampleRate_ >= 2.2 * kMeteorBitRate) meteorDemod_.configure(sampleRate_, kMeteorBitRate);
}

void NadirStationDecoder::resetPolar() {
    metopDecoder_.reset();
    metopDemod_.reset();
    meteorDemod_.reset();
    meteorDeframer_.reset();
    msumr_.reset();
    avhrr_.reset();
    metopInstr_.reset();
    mtvza_.reset();
    polarImager_.reset();
    polarStreamMs_ = polarLastLineMs_ = 0.0;
    polarLinesSeen_ = 0;
    polarPreviewMs_ = polarProgressMs_ = 0;
    polarSnrY1_ = polarSnrY2_ = 0.0;
    polarFramesBefore_ = 0;
}

void NadirStationDecoder::publishPolarPreview(bool force) {
    if (!polarImager_) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (!force && polarPreviewMs_ != 0 && now - polarPreviewMs_ < 1000) return;
    const int lines = polarImager_->lines();
    if (lines < 2) return;
    polarPreviewMs_ = now;
    QString what;
    // The newest 1600 lines at half resolution: the picture scrolls as the pass comes in.
    const QImage img = polarImager_->preview(1600, true, &what);
    const QDateTime t = polarImager_->firstTime();
    const QString stamp = t.isValid() ? t.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")) : QStringLiteral("live");
    emit hritLivePreviewReady(stamp, img, QStringLiteral("%1 · %2 lines · %3")
        .arg(isMetop() ? QStringLiteral("MetOp AVHRR") : QStringLiteral("METEOR MSU-MR")).arg(lines).arg(what));
}

void NadirStationDecoder::finalizePolarPass(const QString& why) {
    if (!polarImager_ || polarImager_->lines() < 2) return;
    const bool metop = isMetop();
    const QDateTime first = polarImager_->firstTime();
    const QString stamp = (first.isValid() ? first : QDateTime::currentDateTimeUtc()).toString(QStringLiteral("yyyyMMdd_HHmmss"));
    const QString dir = QDir(outputRoot_).filePath(QStringLiteral("%1/%2_%3")
        .arg(metop ? QStringLiteral("MetOp") : QStringLiteral("METEOR"), metop ? QStringLiteral("AHRPT") : QStringLiteral("HRPT"), stamp));
    publishPolarPreview(true);
    QStringList paths = polarImager_->save(dir, metop ? QStringLiteral("AVHRR") : QStringLiteral("MSUMR"));
    // The other instruments of the pass, into the same folder.
    QString others;
    QJsonObject ascatJson;
    int scidSeen = -1;
    // TLE for the map products: the Passes page's cache (Celestrak / SatNOGS downloads) and any
    // .tle / .txt beside the program, matched on the NORAD number of the satellite heard.
    auto findTle = [](int norad, double* epoch) {
        nadirstation::pass::TleEntry best;
        *epoch = -1.0;
        if (norad == 0) return best;
        QStringList files;
        const QString cache = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("tle"));
        for (const QFileInfo& fi : QDir(cache).entryInfoList({QStringLiteral("*.txt"), QStringLiteral("*.tle")}, QDir::Files)) files << fi.absoluteFilePath();
        const QString orbits = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("resources/orbits"));
        for (const QFileInfo& fi : QDir(orbits).entryInfoList({QStringLiteral("*.txt"), QStringLiteral("*.tle")}, QDir::Files)) files << fi.absoluteFilePath();
        for (const QString& f : files) {
            QFile file(f);
            if (!file.open(QIODevice::ReadOnly)) continue;
            for (const auto& e : nadirstation::pass::parseTle(file.readAll().toStdString())) {
                if (e.norad != norad) continue;
                nadirstation::pass::Tracker t(e);
                if (t.valid() && t.epochUnix() > *epoch) { *epoch = t.epochUnix(); best = e; }
            }
        }
        return best;
    };
    const QString mapsDir = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("resources/maps"));
    QJsonObject mtvzaJson;
    if (metop) {
        const int scid = metopInstr_.spacecraftId();
        const int norad = scid == 11 ? 38771 : scid == 12 ? 29499 : scid == 13 ? 43689 : 0;
        double bestEpoch = -1.0;
        const nadirstation::pass::TleEntry best = findTle(norad, &bestEpoch);
        metopInstr_.setAscatContext(best.name, best.line1, best.line2, mapsDir);
        paths << metopInstr_.save(dir);
        others = metopInstr_.summary() + QStringLiteral("; ") + metopInstr_.ascatLog() + QStringLiteral("; ") + metopInstr_.mapsLog();
        scidSeen = scid;
        ascatJson = QJsonObject{
            {QStringLiteral("log"), metopInstr_.ascatLog()},
            {QStringLiteral("tle_name"), QString::fromStdString(best.name)},
            {QStringLiteral("tle_line1"), QString::fromStdString(best.line1)},
            {QStringLiteral("tle_line2"), QString::fromStdString(best.line2)},
            {QStringLiteral("tle_epoch_utc"), bestEpoch > 0 ? QDateTime::fromSecsSinceEpoch(qint64(bestEpoch), QTimeZone::UTC).toString(Qt::ISODate) : QString()},
            {QStringLiteral("values"), QStringLiteral("relative backscatter in dB (echo against its pass median per beam and range sample), uncalibrated; not sigma-0, no winds")},
            {QStringLiteral("grid"), QStringLiteral("equirectangular 0.05 deg; *_dBx100.png = dB x 100 + 32768 (0 = no data), georeferenced by the .pgw beside it")},
            {QStringLiteral("time_source"), QStringLiteral("CCSDS packet time; packets without one take the system clock at reception")}};
    } else {
        // Which METEOR-M: the MSU-MR serial number (line byte 12) -> NORAD number.
        scidSeen = msumr_.serial();
        const int norad = scidSeen == 0 ? 40069 : scidSeen == 2 ? 44387 : scidSeen == 3 ? 57166 : scidSeen == 4 ? 59051 : 0;
        double tleEpoch = -1.0;
        const nadirstation::pass::TleEntry tle = findTle(norad, &tleEpoch);
        mtvza_.setContext(scidSeen, tle.name, tle.line1, tle.line2, mapsDir);
        const QStringList m = mtvza_.save(dir);
        paths << m;
        others = mtvza_.scans() == 0 ? QStringLiteral("MTVZA: no frames")
               : m.isEmpty() ? QStringLiteral("MTVZA: %1 scans, all samples empty (instrument not sending data)").arg(mtvza_.scans())
                             : QStringLiteral("MTVZA: %1 scans; %2").arg(mtvza_.scans()).arg(mtvza_.mapsLog());
        double cone = 0, az0 = 0;
        nadirstation::polar::MtvzaReader::geometry(scidSeen, &cone, &az0);
        mtvzaJson = QJsonObject{
            {QStringLiteral("scans"), mtvza_.scans()},
            {QStringLiteral("log"), mtvza_.mapsLog()},
            {QStringLiteral("tle_name"), QString::fromStdString(tle.name)},
            {QStringLiteral("tle_line1"), QString::fromStdString(tle.line1)},
            {QStringLiteral("tle_line2"), QString::fromStdString(tle.line2)},
            {QStringLiteral("tle_epoch_utc"), tleEpoch > 0 ? QDateTime::fromSecsSinceEpoch(qint64(tleEpoch), QTimeZone::UTC).toString(Qt::ISODate) : QString()},
            {QStringLiteral("values"), QStringLiteral("uncalibrated counts (no calibration views in the HRPT stream); single-channel products have the sign flipped so that higher = warmer; bit-error spikes removed")},
            {QStringLiteral("geometry"), QStringLiteral("conical scan, %1 deg off nadir, 200 samples over 144 deg of azimuth (sample 0 at %2 deg left of aft); time = MSU-MR line time at scan end - 10 s").arg(cone).arg(az0)},
            {QStringLiteral("grid"), QStringLiteral("equirectangular 0.1 deg, nearest sample within its footprint; empty cells inside the swath take the median of their neighbours")}};
    }
    metopInstr_.reset();
    mtvza_.reset();

    // Observation manifest (the Images page's Import... reads it).
    {
        auto groupOf = [](const QString& base) {
            static const std::pair<const char*, const char*> rules[] = {
                {"AVHRR_", "AVHRR/3"}, {"MHS_", "MHS"}, {"AMSU_", "AMSU-A"}, {"IASI_IIS", "IASI Imaging"}, {"IASI_", "IASI"},
                {"ASCAT_", "ASCAT"}, {"GOME2_", "GOME-2"}, {"SEM_", "SEM"}, {"MSUMR_", "MSU-MR"}, {"MTVZA_", "MTVZA"}};
            for (const auto& r : rules) if (base.startsWith(QLatin1String(r.first))) return QString::fromLatin1(r.second);
            return QStringLiteral("Other");
        };
        QJsonArray products;
        for (const QString& p : paths) {
            const QString name = QFileInfo(p).fileName();
            const QString base = QFileInfo(p).completeBaseName();
            products.append(QJsonObject{
                {QStringLiteral("path"), name}, {QStringLiteral("type"), QStringLiteral("png")},
                {QStringLiteral("instrument"), groupOf(base)},
                {QStringLiteral("kind"), base.startsWith(QLatin1String("ASCAT_L2_")) ? QStringLiteral("l2")
                                         : base.endsWith(QLatin1String("_Enhanced")) ? QStringLiteral("enhanced") : QStringLiteral("image")}});
        }
        QJsonArray extras;
        for (const QFileInfo& fi : QDir(dir).entryInfoList({QStringLiteral("*.csv"), QStringLiteral("*.pgw")}, QDir::Files, QDir::Name))
            extras.append(fi.fileName());
        const QString sat = metop ? (scidSeen == 11 ? QStringLiteral("MetOp-B") : scidSeen == 12 ? QStringLiteral("MetOp-A")
                                     : scidSeen == 13 ? QStringLiteral("MetOp-C") : QStringLiteral("MetOp"))
                                  : (scidSeen == 0 ? QStringLiteral("METEOR-M N2") : scidSeen == 2 ? QStringLiteral("METEOR-M N2-2")
                                     : scidSeen == 3 ? QStringLiteral("METEOR-M N2-3") : scidSeen == 4 ? QStringLiteral("METEOR-M N2-4")
                                     : QStringLiteral("METEOR-M"));
        QJsonObject link;
        if (metop) {
            const auto& st = metopDecoder_.stats();
            link = QJsonObject{{QStringLiteral("cadus_rs_ok"), qint64(st.cadusWithGoodRs)}, {QStringLiteral("cadus_sync"), qint64(st.cadusWithSync)},
                               {QStringLiteral("cadus_corrected"), qint64(st.cadusCorrected)}, {QStringLiteral("fec"), QStringLiteral("K=7 convolutional, punctured rate 3/4, soft Viterbi; RS(255,223) I=4")}};
        } else {
            link = QJsonObject{{QStringLiteral("frames"), qint64(meteorDeframer_.frames())}, {QStringLiteral("flywheel_frames"), qint64(meteorDeframer_.flywheelFrames())}};
        }
        QJsonObject root{
            {QStringLiteral("schema"), QStringLiteral("NadirStation.Polar.Observation.v1")},
            {QStringLiteral("observation_timestamp"), stamp},
            {QStringLiteral("first_line_utc"), first.isValid() ? first.toUTC().toString(Qt::ISODateWithMs) : QString()},
            {QStringLiteral("created_utc"), QDateTime::currentDateTimeUtc().toString(Qt::ISODate)},
            {QStringLiteral("satellite"), sat},
            {QStringLiteral("spacecraft_id"), scidSeen},
            {QStringLiteral("service"), metop ? QStringLiteral("AHRPT 1701.3 MHz") : QStringLiteral("HRPT 1700 MHz")},
            {QStringLiteral("pass_end"), why},
            {QStringLiteral("imager_lines"), polarImager_->lines()},
            {QStringLiteral("link"), link},
            {QStringLiteral("instruments"), others},
            {QStringLiteral("products"), products},
            {QStringLiteral("other_files"), extras}};
        if (metop) root.insert(QStringLiteral("ascat_l2"), ascatJson);
        else root.insert(QStringLiteral("mtvza"), mtvzaJson);
        QSaveFile jf(QDir(dir).filePath(metop ? QStringLiteral("MetOp_Observation.json") : QStringLiteral("METEOR_Observation.json")));
        if (jf.open(QIODevice::WriteOnly)) { jf.write(QJsonDocument(root).toJson(QJsonDocument::Indented)); jf.commit(); }
    }
    const QString msg = QStringLiteral("%1: pass written (%2), %3 lines, %4 pictures -> %5; %6")
        .arg(metop ? QStringLiteral("MetOp AHRPT") : QStringLiteral("METEOR HRPT"), why)
        .arg(polarImager_->lines()).arg(paths.size()).arg(dir, others);
    nadirstation::log::write(msg);
    emit statusMessage(msg);
    if (!paths.isEmpty()) emit imageReady(stamp, paths);
    ++polarPasses_;
    polarImager_.reset();
    polarLinesSeen_ = 0;
}

void NadirStationDecoder::processPolarSamples(const std::vector<std::complex<float>>& iq) {
    const bool metop = isMetop();
    const QString service = metop ? QStringLiteral("MetOp AHRPT") : QStringLiteral("METEOR HRPT");
    const double minRate = metop ? 1.05 * kMetopSymbolRate : 2.2 * kMeteorBitRate;
    if (sampleRate_ < minRate) {
        if (!lowRateReported_) {
            lowRateReported_ = true;
            emit statusMessage(QStringLiteral("%1 needs IQ at %2 MS/s or more.").arg(service).arg(minRate / 1e6, 0, 'f', 2));
        }
        return;
    }
    if (metop && sampleRate_ < 3.5e6 && !lowRateReported_) {
        lowRateReported_ = true;
        emit statusMessage(QStringLiteral("MetOp AHRPT: %1 MS/s cuts the roll-off of the 2.33 Msym/s signal; experimental, needs a strong signal (6 MS/s recommended).")
            .arg(sampleRate_ / 1e6, 0, 'f', 2));
    }
    if (!polarImager_)
        polarImager_ = std::make_unique<nadirstation::polar::Imager>(metop ? QStringLiteral("AVHRR") : QStringLiteral("MSU-MR"),
                                                                     metop ? 2048 : 1572);
    polarStreamMs_ += 1000.0 * double(iq.size()) / sampleRate_;

    std::size_t good = 0, sync = 0;
    float lockMeasure = 0.0f;
    if (metop) {
        auto d = metopDemod_.process(iq);
        carrierOffsetHz_ = d.carrierOffsetHz;
        lockMeasure = metopDemod_.lockMeasure();
        publishConstellation(d.symbols);
        {
            constexpr double alpha = 0.001, beta = 1.0 - alpha;
            double y1 = polarSnrY1_, y2 = polarSnrY2_;
            for (const auto& z : d.symbols) {
                const double m = double(z.real()) * z.real() + double(z.imag()) * z.imag();
                y1 = alpha * m + beta * y1;
                y2 = alpha * m * m + beta * y2;
            }
            polarSnrY1_ = std::isfinite(y1) ? y1 : 0.0;
            polarSnrY2_ = std::isfinite(y2) ? y2 : 0.0;
        }
        metopDecoder_.feed(std::move(d.symbols), [this](nadirstation::gk2a::Cadu&& c) {
            if (c.rsOk && polarImager_) { avhrr_.feed(c.bytes.data(), *polarImager_); metopInstr_.feed(c.bytes.data()); }
        });
        const auto& st = metopDecoder_.stats();
        good = st.cadusWithGoodRs;
        sync = st.cadusWithSync;
    } else {
        std::vector<float> soft;
        std::vector<std::complex<float>> cons;
        meteorDemod_.process(iq, soft, &cons);
        carrierOffsetHz_ = float(meteorDemod_.carrierOffsetHz());
        lockMeasure = float(meteorDemod_.lockMeasure());
        // Soft bits on the real axis, spread vertically a little so the two clusters read as such.
        for (std::size_t i = 0; i < cons.size(); ++i) cons[i].imag(0.15f * float(int(i % 7) - 3) / 3.0f);
        publishConstellation(cons);
        for (auto& f : meteorDeframer_.feed(soft))
            if (polarImager_) { msumr_.feedFrame(f.data(), *polarImager_); mtvza_.feedFrame(f.data(), msumr_.lastTime()); }
        good = meteorDeframer_.frames() - meteorDeframer_.flywheelFrames();
        sync = meteorDeframer_.frames();
    }

    // Lines arriving / the pass ending.
    const int lines = polarImager_ ? polarImager_->lines() : 0;
    if (lines != polarLinesSeen_) {
        if (polarLinesSeen_ == 0) {
            const QString msg = QStringLiteral("%1: first image line (carrier %2 Hz)").arg(service).arg(carrierOffsetHz_, 0, 'f', 0);
            nadirstation::log::write(msg);
            emit statusMessage(msg);
        }
        polarLinesSeen_ = lines;
        polarLastLineMs_ = polarStreamMs_;
    } else if (lines > 20 && polarStreamMs_ - polarLastLineMs_ > kPassEndSilenceMs) {
        finalizePolarPass(QStringLiteral("no lines for 30 s"));
        // Next pass: search the carrier again.
        if (metop) metopDemod_.reset(); else { meteorDemod_.reset(); meteorDeframer_.reset(); msumr_.reset(); }
        avhrr_.reset();
    }
    publishPolarPreview(false);

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (lastStatsEmitMs_ != 0 && now - lastStatsEmitMs_ < 200) return;
    lastStatsEmitMs_ = now;
    const bool locked = metop ? metopDecoder_.locked() : meteorDeframer_.locked();
    lock_ = locked ? 100 : (sync > 0 ? 50 : 0);
    const auto count = [](std::size_t n) { return int(std::min<std::size_t>(n, 2147483647)); };
    emit decodeStats(lock_, count(good), count(sync - std::min(sync, good)), std::clamp(lockMeasure, 0.0f, 1.0f), carrierOffsetHz_);
    float snr = 0.0f;
    if (metop) {
        const double signal = std::sqrt(std::max(0.0, 2.0 * polarSnrY1_ * polarSnrY1_ - polarSnrY2_));
        const double noise = polarSnrY1_ - signal;
        snr = (signal > 0.0 && noise > 1e-12) ? float(std::max(0.0, 10.0 * std::log10(signal / noise))) : 0.0f;
    } else {
        snr = float(std::max(0.0, meteorDemod_.snrDb()));
    }
    peakDemodSnr_ = std::max(peakDemodSnr_, snr);
    emit demodSnrMetricsReady(snr, peakDemodSnr_, snr);
    if (polarProgressMs_ == 0 || now - polarProgressMs_ > 5000) {
        polarProgressMs_ = now;
        const QString msg = good == 0
            ? QStringLiteral("%1: searching; carrier offset %2 Hz, quality %3%")
                  .arg(service).arg(carrierOffsetHz_, 0, 'f', 0).arg(lockMeasure * 100.0f, 0, 'f', 1)
            : metop ? QStringLiteral("%1: RS ok %2 / sync %3; AVHRR lines %4; carrier %5 Hz")
                          .arg(service).arg(good).arg(sync).arg(lines).arg(carrierOffsetHz_, 0, 'f', 0)
                    : QStringLiteral("%1: frames %2 (%3 on the flywheel); MSU-MR lines %4; carrier %5 Hz")
                          .arg(service).arg(sync).arg(meteorDeframer_.flywheelFrames()).arg(lines).arg(carrierOffsetHz_, 0, 'f', 0);
        emit statusMessage(msg);
        nadirstation::log::write(msg);
    }
}
