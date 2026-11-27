// Pipeline diagnostics for the Decoder page: after every block of samples the decoder thread adds
// up signal time and DSP time, and about four times a second it reads the state of every stage of
// the link in use -- input queue, carrier loop, demodulator, frame sync, error correction, data
// layer, products -- grades each one green / amber / red, says why, and queues the snapshot to the
// GUI. Nothing here changes how a link decodes; it only reads counters the links already keep.
#include "nadirstationdecoder.h"
#include "gvarengine.h"
#include "goessd.h"
#include "runtime_logger.h"
#include <QDateTime>
#include <QLocale>
#include <algorithm>
#include <cmath>

using namespace nadirstation::diag;

namespace {

constexpr qint64 kEmitMs = 250;           // snapshot period
constexpr double kAcquireGraceS = 6.0;    // after start, "no sync yet" is amber, not red
constexpr int SVISSR_SCAN_LINES = 2291;   // scan lines in an FY-2 full-disk picture

QString grouped(double v) {
    if (!std::isfinite(v)) return QStringLiteral("—");
    const bool neg = v < 0;
    QString d = QString::number(qint64(std::llround(std::fabs(v))));
    for (int i = d.size() - 3; i > 0; i -= 3) d.insert(i, QChar(0x2009));
    return neg ? QStringLiteral("−") + d : d;
}
QString n0(double v) {
    if (std::isfinite(v) && std::fabs(v) >= 1e8) return QStringLiteral("%1 M").arg(v / 1e6, 0, 'f', 0);
    return grouped(v);
}
QString n1(double v, int prec = 1) {
    return std::isfinite(v) ? QString::number(v, 'f', prec) : QStringLiteral("—");
}
QString pct(double fraction, int prec = 1) {
    return std::isfinite(fraction) ? QStringLiteral("%1 %").arg(fraction * 100.0, 0, 'f', prec) : QStringLiteral("—");
}
QString hz(double v) {
    if (!std::isfinite(v)) return QStringLiteral("—");
    return std::fabs(v) >= 1000.0 ? QStringLiteral("%1 kHz").arg(v / 1000.0, 0, 'f', 2)
                                   : QStringLiteral("%1 Hz").arg(v, 0, 'f', 0);
}
QString perS(double v) {
    if (!std::isfinite(v)) return QStringLiteral("—");
    return v >= 100.0 ? QStringLiteral("%1 /s").arg(v, 0, 'f', 0) : QStringLiteral("%1 /s").arg(v, 0, 'f', 1);
}
Metric metric(const QString& name, const QString& value, int health = Ok, const QString& tip = QString()) {
    Metric m;
    m.name = name;
    m.value = value;
    m.health = health;
    m.tip = tip;
    return m;
}
// Fraction of units rejected among those that arrived in the window, from two rates.
double rejectFraction(double passRate, double rejectRate) {
    const double total = passRate + rejectRate;
    return total > 1e-9 ? rejectRate / total : std::nan("");
}
int gradeReject(double fraction, double warnAt, double errorAt) {
    if (!std::isfinite(fraction)) return Idle;
    return fraction >= errorAt ? Error : fraction >= warnAt ? Warn : Ok;
}

}  // namespace

void NadirStationDecoder::resetPipelineDiag() {
    diagStartMs_ = QDateTime::currentMSecsSinceEpoch();
    diagLastEmitMs_ = 0;
    diagWinSignal_ = diagWinDsp_ = 0.0;
    diagStreamSeconds_ = 0.0;
    diagDroppedAtLastEmit_ = ingressDroppedSamples_.load();
    diagWaitAtLastEmit_ = ingressWaitMs_.load();
    diagLastDropMs_ = 0;
    diagLastSyncProgressMs_ = 0;
    diagLastSyncCount_ = -1.0;
    diagCounters_.clear();
    diagSnrDb_.store(std::numeric_limits<float>::quiet_NaN());
    fy2TimingQuality_ = 0.0f;
}

// Rate of a cumulative counter, per second of wall time, smoothed a little so a bursty link (HRIT
// frames come in bunches from the decoder thread) does not flicker between red and green.
double NadirStationDecoder::diagRate(const QString& key, double value, double seconds) {
    DiagCounter& c = diagCounters_[key];
    if (seconds <= 0.0) return c.rate;
    const double inst = value >= c.value ? (value - c.value) / seconds : 0.0;   // a reset reads as 0
    c.rate = c.value == 0.0 && c.rate == 0.0 ? inst : 0.5 * c.rate + 0.5 * inst;
    c.value = value;
    return c.rate;
}

void NadirStationDecoder::pipelineTick(std::size_t samples, double dspSeconds) noexcept {
    try {
        if (sampleRate_ <= 0.0) return;
        const double signal = double(samples) / sampleRate_;
        diagWinSignal_ += signal;
        diagWinDsp_ += dspSeconds;
        diagStreamSeconds_ += signal;
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (diagStartMs_ == 0) diagStartMs_ = now;
        if (diagLastEmitMs_ == 0) { diagLastEmitMs_ = now; return; }
        if (now - diagLastEmitMs_ < kEmitMs) return;
        const double wall = double(now - diagLastEmitMs_) / 1000.0;
        Snapshot s = buildPipelineSnapshot(diagWinSignal_, wall, diagWinDsp_);
        s.ms = now;
        s.streamSeconds = diagStreamSeconds_;
        diagWinSignal_ = diagWinDsp_ = 0.0;
        diagLastEmitMs_ = now;
        emit pipelineSnapshot(s);
    } catch (...) {
        // Diagnostics must never take the receive chain down with them.
    }
}

Snapshot NadirStationDecoder::buildPipelineSnapshot(double winSignal, double winWall, double winDsp) {
    Snapshot snap;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const double uptime = diagStartMs_ ? double(now - diagStartMs_) / 1000.0 : 0.0;
    const bool acquiring = uptime < kAcquireGraceS;

    // ---- which link, and how its stages are built ------------------------------------------------
    enum class Link { Fy2, Gk2aLrit, Gk2aHrit, ElektroLrit, ElektroHrit, Gvar, GoesSd, Metop, Meteor, Ggak };
    const Link link = isGgak() ? Link::Ggak
                    : isPolar() ? (isMetop() ? Link::Metop : Link::Meteor)
                    : isElektro() ? (isElektroHrit() ? Link::ElektroHrit : Link::ElektroLrit)
                    : isGoesSd() ? Link::GoesSd
                    : isGvar() ? Link::Gvar
                    : isHrit() ? Link::Gk2aHrit
                    : isGk2a() ? Link::Gk2aLrit
                    : Link::Fy2;
    // SNR zones (dB on the read-out): below the first, frames do not come; below the second, little
    // margin. The same numbers as the Live page's SNR meter.
    double weak = 2.0, good = 6.0;
    switch (link) {
    case Link::Fy2: snap.link = satelliteName_.isEmpty() ? QStringLiteral("FY-2 S-VISSR") : satelliteName_ + QStringLiteral(" S-VISSR");
        snap.chain = QStringLiteral("BPSK 660 ksym/s · NRZ-M · PN sync · S-VISSR sectors"); weak = 1.5; good = 4.0; break;
    case Link::Gk2aLrit: snap.link = QStringLiteral("GK-2A LRIT");
        snap.chain = QStringLiteral("BPSK 128 ksym/s · Viterbi r=1/2 · ASM · RS(255,223) · CCSDS packets · LRIT files"); weak = 1.0; good = 4.0; break;
    case Link::Gk2aHrit: snap.link = QStringLiteral("GK-2A HRIT");
        snap.chain = QStringLiteral("QPSK 3 Msym/s · Viterbi r=1/2 · ASM · RS(255,223) · CCSDS packets · HRIT files"); weak = 2.0; good = 5.0; break;
    case Link::ElektroLrit: snap.link = QStringLiteral("ELEKTRO-L LRIT");
        snap.chain = QStringLiteral("BPSK 294 ksym/s · Viterbi · ASM · RS · CCSDS packets · xRIT files"); weak = 1.0; good = 4.0; break;
    case Link::ElektroHrit: snap.link = QStringLiteral("ELEKTRO-L HRIT");
        snap.chain = QStringLiteral("OQPSK 1.15 Msym/s · Viterbi · ASM · RS · CCSDS packets · xRIT files"); weak = 2.5; good = 5.5; break;
    case Link::Gvar: snap.link = QStringLiteral("GOES GVAR");
        snap.chain = QStringLiteral("BPSK 2.111 Msym/s · NRZ-S · block sync · header CRC · soft pixels"); weak = 3.0; good = 6.0; break;
    case Link::GoesSd: snap.link = QStringLiteral("GOES SD");
        snap.chain = QStringLiteral("BPSK 2.6208 Mbit/s · NRZ-M · 14-bit block sync · PN · imager scans + sounder"); weak = 4.0; good = 8.0; break;
    case Link::Metop: snap.link = QStringLiteral("MetOp AHRPT");
        snap.chain = QStringLiteral("QPSK 2.33 Msym/s · Viterbi r=3/4 · ASM · RS · AVHRR / MHS / AMSU / IASI"); weak = 4.0; good = 5.5; break;
    case Link::Meteor: snap.link = QStringLiteral("METEOR HRPT");
        snap.chain = QStringLiteral("PM / Manchester 665.4 kbit/s · frame marker · MSU-MR"); weak = 6.0; good = 9.0; break;
    case Link::Ggak: snap.link = QStringLiteral("ELEKTRO-L GGAK");
        snap.chain = QStringLiteral("PM / BPSK 5 kBd · frame sync · checksum · GGAK frames"); weak = 6.0; good = 10.0; break;
    }

    // ---- 1. IQ input ------------------------------------------------------------------------------
    {
        Stage st;
        st.key = QStringLiteral("iq");
        st.title = QStringLiteral("IQ input");
        st.detail = QStringLiteral("Receiver → decoder queue → DSP thread");
        const double load = winSignal > 0.0 ? winDsp / winSignal : std::nan("");
        const double speed = winWall > 0.0 ? winSignal / winWall : std::nan("");
        const std::size_t queued = ingressQueuedSamples_.load();
        const double queueS = sampleRate_ > 0.0 ? double(queued) / sampleRate_ : 0.0;
        const double queueFill = double(queued) / double(MAX_INGRESS_SAMPLES);
        const uint64_t dropped = ingressDroppedSamples_.load();
        const uint64_t wait = ingressWaitMs_.load();
        const uint64_t newDrops = dropped - std::min(dropped, diagDroppedAtLastEmit_);
        const uint64_t newWait = wait - std::min(wait, diagWaitAtLastEmit_);
        diagDroppedAtLastEmit_ = dropped;
        diagWaitAtLastEmit_ = wait;
        if (newDrops > 0) diagLastDropMs_ = now;
        const bool recentDrop = diagLastDropMs_ != 0 && now - diagLastDropMs_ < 10000;

        st.headline = std::isfinite(load) ? QStringLiteral("%1 %").arg(load * 100.0, 0, 'f', 0) : QStringLiteral("—");
        st.caption = QStringLiteral("DSP load (time per signal second)");
        st.spark = std::isfinite(load) ? load * 100.0 : std::nan("");
        st.sparkUnit = QStringLiteral("% DSP");
        st.passed = diagStreamSeconds_ * sampleRate_;
        st.rejected = double(dropped);
        st.unit = QStringLiteral("samples");
        const int loadHealth = !std::isfinite(load) ? Idle : load >= 1.0 ? Error : load >= 0.85 ? Warn : Ok;
        const int queueHealth = queueFill >= 0.5 ? Error : queueFill >= 0.15 ? Warn : Ok;
        st.metrics << metric(QStringLiteral("Sample rate"), QStringLiteral("%1 MS/s").arg(sampleRate_ / 1e6, 0, 'f', 3))
                   << metric(QStringLiteral("DSP load"), st.headline, loadHealth,
                             QStringLiteral("Processing time divided by signal time. Above 100 % the decoder falls behind a live receiver."))
                   << metric(QStringLiteral("Throughput"), QStringLiteral("%1 × real time").arg(speed, 0, 'f', 2), Ok,
                             QStringLiteral("Signal seconds handled per wall-clock second. A file plays faster than real time."))
                   << metric(QStringLiteral("Queue"), QStringLiteral("%1 M samples · %2 s").arg(double(queued) / 1e6, 0, 'f', 2).arg(queueS, 0, 'f', 2),
                             queueHealth, QStringLiteral("IQ waiting for the DSP thread"))
                   << metric(QStringLiteral("Dropped"), newDrops > 0 || dropped > 0 ? QStringLiteral("%1 samples").arg(n0(double(dropped))) : QStringLiteral("none"),
                             recentDrop ? Error : dropped > 0 ? Warn : Ok,
                             QStringLiteral("Oldest queued blocks thrown away because the DSP fell behind (backpressure off)"))
                   << metric(QStringLiteral("Receiver held"), newWait > 0 ? QStringLiteral("%1 ms in the last %2 s").arg(newWait).arg(winWall, 0, 'f', 2)
                                                                          : QStringLiteral("no"),
                             newWait > 50 ? Warn : Ok, QStringLiteral("Backpressure: the capture thread waited for the queue to drain"))
                   << metric(QStringLiteral("Signal processed"), QStringLiteral("%1 s").arg(diagStreamSeconds_, 0, 'f', 1));
        st.health = worst(worst(loadHealth, queueHealth), recentDrop ? Error : Ok);
        st.why = recentDrop ? QStringLiteral("Samples were dropped in the last 10 s: the DSP could not keep up, so every later stage lost data.")
               : loadHealth == Error ? QStringLiteral("DSP takes longer than the signal lasts; with a live receiver the queue grows until samples are lost.")
               : queueHealth == Error ? QStringLiteral("The input queue is more than half full.")
               : st.health == Warn ? QStringLiteral("Little real-time margin (DSP load or queue rising).")
                                   : QStringLiteral("Samples reach the DSP thread in time.");
        snap.stages << st;
    }

    // ---- per-link numbers used by several stages ---------------------------------------------------
    double carrierMeasure = std::nan("");
    int acquisitions = -1;
    double carrierErrDeg = std::nan(""), timingErrRms = std::nan("");
    bool carrierFound = true;
    double cnr = std::nan("");
    switch (link) {
    case Link::Fy2: case Link::Gk2aLrit: case Link::ElektroLrit:
        carrierMeasure = pskDemod_.lockMeasure(); acquisitions = pskDemod_.acquisitions(); break;
    case Link::Gk2aHrit:
        carrierMeasure = hritDemod_.lockMeasure(); acquisitions = hritDemod_.acquisitions();
        carrierErrDeg = hritDemod_.carrierErrorRms() * 57.29578; timingErrRms = hritDemod_.timingErrorRms(); break;
    case Link::ElektroHrit:
        carrierMeasure = elektroOqpsk_.lockMeasure(); acquisitions = elektroOqpsk_.acquisitions();
        carrierErrDeg = elektroOqpsk_.carrierErrorRms() * 57.29578; timingErrRms = elektroOqpsk_.timingErrorRms(); break;
    case Link::Metop:
        carrierMeasure = metopDemod_.lockMeasure(); acquisitions = metopDemod_.acquisitions();
        carrierErrDeg = metopDemod_.carrierErrorRms() * 57.29578; timingErrRms = metopDemod_.timingErrorRms(); break;
    case Link::Meteor:
        carrierMeasure = meteorDemod_.lockMeasure(); acquisitions = meteorDemod_.acquisitions();
        carrierFound = meteorDemod_.acquired(); break;
    case Link::Gvar: carrierMeasure = gvar_ ? gvar_->lockMeasure() : std::nan(""); break;
    case Link::GoesSd: carrierMeasure = goessd_ ? goessd_->lockMeasure() : std::nan(""); break;
    case Link::Ggak: {
        const auto& g = ggakDemod_.stats();
        carrierFound = g.carrierFound;
        cnr = g.carrierToNoiseDb;
        carrierMeasure = g.tracking ? 1.0 : g.carrierFound ? 0.5 : 0.0;
        break;
    }
    }
    const double acqRate = acquisitions >= 0 ? diagRate(QStringLiteral("acq"), acquisitions, winWall) : 0.0;

    // Sync and FEC numbers, per link.
    double syncCount = 0.0;          // frames / blocks / lines with a sync marker, cumulative
    bool syncLocked = lock_ >= 100;
    bool syncPartial = lock_ >= 50;
    QVector<Metric> syncMetrics;
    QString syncUnit = QStringLiteral("frames");
    QString syncDetail;
    double fecPassed = -1.0, fecRejected = -1.0;
    QVector<Metric> fecMetrics;
    QString fecTitle = QStringLiteral("Error correction"), fecDetail, fecUnit = QStringLiteral("frames");
    double fecWarnAt = 0.02, fecErrorAt = 0.15;
    bool fecNone = false;            // link carries no FEC: the stage grades something else
    int fecOverride = -1;
    QString fecWhyOverride;

    const auto ccsds = [&](const nadirstation::gk2a::DecodeStats& d, bool locked) {
        syncCount = double(d.cadusWithSync);
        syncLocked = locked;
        syncPartial = d.cadusWithSync > 0;
        syncDetail = QStringLiteral("CCSDS attached sync marker 1ACFFC1D");
        const double tolRate = diagRate(QStringLiteral("asmTol"), double(d.asmTolerated), winWall);
        syncMetrics << metric(QStringLiteral("CADUs assembled"), n0(double(d.cadusAssembled)))
                    << metric(QStringLiteral("With sync marker"), n0(double(d.cadusWithSync)))
                    << metric(QStringLiteral("Sync rate (all time)"), pct(d.frameSyncRate), Ok,
                              QStringLiteral("CADU windows with a marker among all windows tried, including the search before lock"))
                    << metric(QStringLiteral("Marker bit errors tolerated"), n0(double(d.asmTolerated)), tolRate > 0.5 ? Warn : Ok,
                              QStringLiteral("Frames accepted with 1..N wrong marker bits"))
                    << metric(QStringLiteral("Flywheel frames"), QStringLiteral("%1 (%2 RS ok)").arg(n0(double(d.flywheelFrames)), n0(double(d.flywheelRsOk))),
                              Ok, QStringLiteral("Taken on the frame clock while the marker was unreadable"))
                    << metric(QStringLiteral("Sync losses"), n0(double(d.syncLosses)),
                              diagRate(QStringLiteral("syncLoss"), double(d.syncLosses), winWall) > 0.0 ? Error : d.syncLosses > 0 ? Warn : Ok)
                    << metric(QStringLiteral("Symbol pairing slips"), n0(double(d.pairSlips)))
                    << metric(QStringLiteral("Polarity"), d.polarityInverted ? QStringLiteral("inverted") : QStringLiteral("normal"));
        fecTitle = QStringLiteral("Viterbi + Reed-Solomon");
        fecDetail = QStringLiteral("Convolutional decoding, then RS(255,223) per CADU");
        fecPassed = double(d.cadusWithGoodRs);
        fecRejected = double(d.cadusWithSync - std::min(d.cadusWithSync, d.cadusWithGoodRs));
        const double corrRate = diagRate(QStringLiteral("rsCorr"), double(d.cadusCorrected), winWall);
        const double okRate = diagRate(QStringLiteral("rsOkC"), double(d.cadusWithGoodRs), winWall);
        fecMetrics << metric(QStringLiteral("Needed correction"), QStringLiteral("%1 of %2").arg(n0(double(d.cadusCorrected)), n0(double(d.cadusWithGoodRs))),
                             okRate > 0 && corrRate / okRate > 0.5 ? Warn : Ok, QStringLiteral("CADUs that passed only after RS changed bytes"))
                   << metric(QStringLiteral("Bytes corrected / CADU"), d.cadusWithGoodRs ? n1(double(d.rsBytesCorrected) / double(d.cadusWithGoodRs), 2) : QStringLiteral("—"),
                             d.cadusWithGoodRs && double(d.rsBytesCorrected) / double(d.cadusWithGoodRs) > 8.0 ? Warn : Ok,
                             QStringLiteral("RS(255,223) corrects up to 16 bytes per interleave; the closer to that, the closer to failure"))
                   << metric(QStringLiteral("Iterative rescue"), QStringLiteral("%1 of %2 recovered").arg(n0(double(d.rescueOk)), n0(double(d.rescueTried))))
                   << metric(QStringLiteral("Soft bits used"), n0(double(d.softBitsUsed)));
    };

    switch (link) {
    case Link::Fy2: {
        syncCount = double(goodFrames_ + badFrames_);
        syncUnit = QStringLiteral("lines");
        syncDetail = QStringLiteral("S-VISSR PN sync, one frame per scan line");
        syncLocked = lock_ >= 100;
        syncPartial = lock_ >= 50;
        syncMetrics << metric(QStringLiteral("State"), lock_ >= 100 ? QStringLiteral("LOCK") : lock_ >= 50 ? QStringLiteral("SYNC") : QStringLiteral("SEARCH"),
                              lock_ >= 100 ? Ok : lock_ >= 50 ? Warn : Error)
                    << metric(QStringLiteral("Frame locker"), frameLocker_[0].locked() ? QStringLiteral("collecting a frame") : QStringLiteral("searching"))
                    << metric(QStringLiteral("Lines found"), n0(syncCount))
                    << metric(QStringLiteral("Last line number"), lastReportedLine_ > 0 ? QString::number(lastReportedLine_) : QStringLiteral("—"))
                    << metric(QStringLiteral("Picture"), pictureCycleActive_ ? QStringLiteral("in progress") : QStringLiteral("waiting for Picture Flag"));
        fecTitle = QStringLiteral("Sector checks");
        fecDetail = QStringLiteral("Sector IDs, zero filler, DOC CRC; soft-decision repair");
        fecUnit = QStringLiteral("lines");
        fecPassed = goodFrames_;
        fecRejected = badFrames_;
        fecWarnAt = 0.05; fecErrorAt = 0.25;
        fecMetrics << metric(QStringLiteral("Soft quality"), pct(softQuality_), softQuality_ < 0.3f ? Warn : Ok)
                   << metric(QStringLiteral("Lines accepted"), n0(goodFrames_))
                   << metric(QStringLiteral("Lines rejected"), n0(badFrames_), badFrames_ > 0 ? Warn : Ok);
        break;
    }
    case Link::Gk2aLrit: ccsds(gk2aLrit_.stats(), lock_ >= 100); break;
    case Link::Gk2aHrit: ccsds(hritDecoder_.stats(), hritDecoder_.locked());
        syncMetrics << metric(QStringLiteral("QPSK mapping"), QString::number(hritDecoder_.mapping())); break;
    case Link::ElektroLrit: ccsds(elektroLrit_.stats(), elektroLrit_.frameLocked());
        syncMetrics << metric(QStringLiteral("Re-acquisitions"), QString::number(elektroReacq_), elektroReacq_ > 0 ? Warn : Ok); break;
    case Link::ElektroHrit: ccsds(elektroHrit_.stats(), elektroHrit_.locked());
        syncMetrics << metric(QStringLiteral("Re-acquisitions"), QString::number(elektroReacq_), elektroReacq_ > 0 ? Warn : Ok); break;
    case Link::Metop: ccsds(metopDecoder_.stats(), metopDecoder_.locked()); break;
    case Link::Meteor: {
        syncCount = double(meteorDeframer_.frames());
        syncLocked = meteorDeframer_.locked();
        syncPartial = meteorDeframer_.frames() > 0;
        syncDetail = QStringLiteral("HRPT frame marker");
        const double fly = double(meteorDeframer_.flywheelFrames());
        syncMetrics << metric(QStringLiteral("Frames"), n0(syncCount))
                    << metric(QStringLiteral("Flywheel frames"), n0(fly), syncCount > 0 && fly / syncCount > 0.2 ? Warn : Ok)
                    << metric(QStringLiteral("Marker bit errors"), n0(double(meteorDeframer_.markerErrorsTotal())));
        fecNone = true;
        fecTitle = QStringLiteral("Bit decisions");
        fecDetail = QStringLiteral("Uncoded link: no FEC, errors go straight into the pixels");
        fecPassed = syncCount - fly;
        fecRejected = -1.0;
        const double errPerFrame = syncCount > 0 ? double(meteorDeframer_.markerErrorsTotal()) / syncCount : std::nan("");
        fecMetrics << metric(QStringLiteral("Marker errors / frame"), n1(errPerFrame, 2), errPerFrame > 2.0 ? Warn : Ok,
                             QStringLiteral("Wrong bits in the known frame marker: a sample of the channel bit error rate"));
        fecOverride = !std::isfinite(errPerFrame) ? Idle : errPerFrame > 6.0 ? Error : errPerFrame > 2.0 ? Warn : Ok;
        fecWhyOverride = QStringLiteral("METEOR HRPT has no error correction; bit errors from the marker estimate the damage in the image.");
        break;
    }
    case Link::Gvar: {
        syncUnit = QStringLiteral("blocks");
        syncCount = gvar_ ? double(gvar_->syncs()) : 0.0;
        syncLocked = lock_ >= 100;
        syncPartial = syncCount > 0;
        syncDetail = QStringLiteral("GVAR block sync, soft correlation");
        syncMetrics << metric(QStringLiteral("Sync words"), n0(syncCount))
                    << metric(QStringLiteral("Last sync"), gvarLastSyncMs_ ? QStringLiteral("%1 s ago").arg(double(now - gvarLastSyncMs_) / 1000.0, 0, 'f', 1) : QStringLiteral("never"),
                              gvarLastSyncMs_ && now - gvarLastSyncMs_ < 1500 ? Ok : Error);
        fecTitle = QStringLiteral("Header CRC");
        fecDetail = QStringLiteral("Triple header with CRC, repaired by bit flips and block sequence");
        fecUnit = QStringLiteral("blocks");
        fecPassed = gvar_ ? double(gvar_->blocksGood()) : -1.0;
        fecRejected = gvar_ ? double(gvar_->blocksBad()) : -1.0;
        fecWarnAt = 0.05; fecErrorAt = 0.30;
        if (gvar_) fecMetrics << metric(QStringLiteral("Soft blocks"), n0(double(gvar_->softBlocks())), Ok,
                                        QStringLiteral("Blocks whose pixels came from the soft-decision path"));
        break;
    }
    case Link::GoesSd: {
        syncUnit = QStringLiteral("blocks");
        syncCount = goessd_ ? double(goessd_->blocks()) : 0.0;
        syncLocked = goessd_ && goessd_->imagerLocked() && lock_ >= 100;
        syncPartial = syncCount > 0;
        syncDetail = QStringLiteral("14-bit sync per 480-bit imager block");
        if (goessd_) {
            syncMetrics << metric(QStringLiteral("Imager"), goessd_->imagerLocked() ? QStringLiteral("locked") : QStringLiteral("searching"),
                                  goessd_->imagerLocked() ? Ok : Error)
                        << metric(QStringLiteral("Blocks"), n0(double(goessd_->blocks())))
                        << metric(QStringLiteral("Blocks with perfect sync"), n0(double(goessd_->syncs())))
                        << metric(QStringLiteral("Sounder"), goessd_->sounderLocked() ? QStringLiteral("locked") : QStringLiteral("searching"),
                                  goessd_->sounderLocked() ? Ok : Warn);
            fecNone = true;
            fecTitle = QStringLiteral("Sync-bit errors");
            fecDetail = QStringLiteral("No FEC on SD: the sync bits of each block measure the channel");
            fecUnit = QStringLiteral("blocks");
            fecPassed = double(goessd_->syncs());
            fecRejected = -1.0;
            const double ber = goessd_->syncBitErrorPct();
            fecMetrics << metric(QStringLiteral("Sync bit error rate"), QStringLiteral("%1 %").arg(ber, 0, 'f', 2),
                                 ber >= 5.0 ? Error : ber >= 1.0 ? Warn : Ok,
                                 QStringLiteral("Errors among the sync bits of locked blocks ≈ raw channel BER"))
                       << metric(QStringLiteral("Sounder parity"), QStringLiteral("%1 %").arg(goessd_->sounderParityPct(), 0, 'f', 2),
                                 goessd_->sounderFrames() > 50 && goessd_->sounderParityPct() < 95.0 ? Warn : Ok);
            fecOverride = syncCount <= 0 ? Idle : ber >= 5.0 ? Error : ber >= 1.0 ? Warn : Ok;
            fecWhyOverride = QStringLiteral("GOES SD carries no FEC. Sync bit errors %1 %: above 1 % the picture shows speckle, above 5 % it falls apart.")
                                 .arg(ber, 0, 'f', 2);
        }
        break;
    }
    case Link::Ggak: {
        const int stt = ggakDeframer_.state();
        syncCount = double(ggakGood_ + ggakBad_);
        syncLocked = stt == 2;
        syncPartial = stt >= 1;
        syncDetail = QStringLiteral("GGAK frame sync");
        syncMetrics << metric(QStringLiteral("State"), stt == 2 ? QStringLiteral("locked") : stt == 1 ? QStringLiteral("candidate") : QStringLiteral("searching"),
                              stt == 2 ? Ok : stt == 1 ? Warn : Error);
        fecTitle = QStringLiteral("Checksum");
        fecDetail = QStringLiteral("Frame checksum");
        fecPassed = double(ggakGood_);
        fecRejected = double(ggakBad_);
        fecWarnAt = 0.05; fecErrorAt = 0.3;
        break;
    }
    }

    // ---- 2. carrier ------------------------------------------------------------------------------
    // Graded after sync is known: a link that delivers frames has, by definition, a working carrier
    // loop, whatever its lock measure says (GVAR's falls to 0.3 at 0 dB while blocks still decode).
    const double syncRate = diagRate(QStringLiteral("sync"), syncCount, winWall);
    if (syncCount > diagLastSyncCount_) diagLastSyncProgressMs_ = now;
    diagLastSyncCount_ = syncCount;
    const bool syncFlowing = diagLastSyncProgressMs_ != 0 && now - diagLastSyncProgressMs_ < 2000;
    {
        Stage st;
        st.key = QStringLiteral("carrier");
        st.title = QStringLiteral("Carrier");
        st.detail = QStringLiteral("Acquisition and carrier loop");
        st.headline = hz(carrierOffsetHz_);
        st.caption = QStringLiteral("frequency offset");
        st.spark = carrierOffsetHz_;
        st.sparkUnit = QStringLiteral("Hz");
        int h = !std::isfinite(carrierMeasure) ? Idle
              : carrierMeasure >= 0.6 ? Ok : carrierMeasure >= 0.3 ? Warn : Error;
        if (!carrierFound) h = Error;
        if (acqRate > 0.0) h = worst(h, Warn);
        if (syncFlowing) h = std::min(h, int(Ok));
        if (acquiring && h == Error) h = Warn;
        st.health = h;
        st.metrics << metric(QStringLiteral("Offset"), hz(carrierOffsetHz_))
                   << metric(QStringLiteral("Loop lock measure"), pct(carrierMeasure, 0),
                             !std::isfinite(carrierMeasure) ? Idle : carrierMeasure >= 0.6 ? Ok : carrierMeasure >= 0.3 ? Warn : Error,
                             QStringLiteral("Mean cos(M·phase error): 100 % = locked, ~0 % = not"));
        if (acquisitions >= 0)
            st.metrics << metric(QStringLiteral("Acquisitions"), QString::number(acquisitions), acqRate > 0.0 ? Warn : Ok,
                                 QStringLiteral("Carrier searches so far; one more each time the loop lost lock"));
        if (std::isfinite(carrierErrDeg))
            st.metrics << metric(QStringLiteral("Phase jitter"), QStringLiteral("%1°").arg(carrierErrDeg, 0, 'f', 1), carrierErrDeg > 15.0 ? Warn : Ok,
                                 QStringLiteral("RMS carrier error detector output"));
        if (std::isfinite(cnr))
            st.metrics << metric(QStringLiteral("C/N"), QStringLiteral("%1 dB").arg(cnr, 0, 'f', 1), cnr < 10.0 ? Warn : Ok);
        if (!carrierFound) st.metrics << metric(QStringLiteral("Carrier"), QStringLiteral("not found"), Error);
        st.why = h == Error ? QStringLiteral("The carrier loop is not locked: check the frequency, the antenna pointing and the gain.")
               : acqRate > 0.0 ? QStringLiteral("The carrier was re-acquired just now: the loop lost lock (fade, drift or interference).")
               : h == Warn ? (acquiring ? QStringLiteral("Acquiring the carrier.") : QStringLiteral("The carrier loop is jittery; lock is weak."))
               : QStringLiteral("Carrier loop locked.");
        snap.stages << st;
    }

    // ---- 3. demodulator ----------------------------------------------------------------------------
    {
        Stage st;
        st.key = QStringLiteral("demod");
        st.title = QStringLiteral("Demodulator");
        st.detail = QStringLiteral("Matched filter, symbol clock, soft symbols");
        const float snr = diagSnrDb_.load();
        st.headline = std::isfinite(snr) ? QStringLiteral("%1 dB").arg(snr, 0, 'f', 1) : QStringLiteral("—");
        st.caption = QStringLiteral("symbol SNR");
        st.spark = std::isfinite(snr) ? double(snr) : std::nan("");
        st.sparkUnit = QStringLiteral("dB");
        int h = !std::isfinite(snr) ? Idle : snr < weak ? Error : snr < good ? Warn : Ok;
        if (syncFlowing && h == Error) h = Warn;   // frames still come: marginal, not broken
        if (acquiring && h == Error) h = Warn;
        st.health = h;
        st.metrics << metric(QStringLiteral("Symbol SNR"), st.headline, h,
                             QStringLiteral("Red below %1 dB (frames stop), amber below %2 dB (little margin)").arg(weak, 0, 'f', 1).arg(good, 0, 'f', 1))
                   << metric(QStringLiteral("Symbol rate"), symbolRate_ >= 1e6 ? QStringLiteral("%1 Msym/s").arg(symbolRate_ / 1e6, 0, 'f', 4)
                                                                               : QStringLiteral("%1 ksym/s").arg(symbolRate_ / 1e3, 0, 'f', 1))
                   << metric(QStringLiteral("Samples / symbol"), sampleRate_ > 0 && symbolRate_ > 0 ? n1(sampleRate_ / symbolRate_, 2) : QStringLiteral("—"),
                             sampleRate_ / std::max(1.0, symbolRate_) < 1.5 ? Warn : Ok);
        if (link == Link::Fy2 || link == Link::Gk2aLrit || link == Link::ElektroLrit)
            st.metrics << metric(QStringLiteral("Soft quality"), pct(softQuality_), softQuality_ < 0.2f ? Warn : Ok,
                                 QStringLiteral("Mean soft-decision confidence"));
        if (link == Link::Fy2)
            st.metrics << metric(QStringLiteral("Timing quality"), pct(fy2TimingQuality_), fy2TimingQuality_ < 0.5f ? Warn : Ok);
        if (std::isfinite(timingErrRms))
            st.metrics << metric(QStringLiteral("Timing jitter"), QStringLiteral("%1 samples").arg(timingErrRms, 0, 'f', 3), timingErrRms > 0.25 ? Warn : Ok);
        st.why = h == Error ? QStringLiteral("Symbol SNR %1 dB is below the %2 dB this link needs.").arg(snr, 0, 'f', 1).arg(weak, 0, 'f', 1)
               : h == Warn ? QStringLiteral("Symbol SNR %1 dB: decoding, with little margin (good from %2 dB).").arg(snr, 0, 'f', 1).arg(good, 0, 'f', 1)
               : h == Idle ? QStringLiteral("No symbols yet.")
                           : QStringLiteral("Symbols are clean enough for this link.");
        snap.stages << st;
    }

    // ---- 4. frame sync -----------------------------------------------------------------------------
    {
        Stage st;
        st.key = QStringLiteral("sync");
        st.title = QStringLiteral("Frame sync");
        st.detail = syncDetail;
        st.headline = perS(syncRate);
        st.caption = syncUnit + QStringLiteral(" with sync");
        st.spark = syncRate;
        st.sparkUnit = QStringLiteral("/s");
        st.passed = syncCount;
        st.unit = syncUnit;
        int h = syncLocked && syncFlowing ? Ok : (syncPartial || syncFlowing) ? Warn : Error;
        if (acquiring && h == Error) h = Warn;
        st.health = h;
        st.metrics << metric(QStringLiteral("Rate"), perS(syncRate), syncFlowing ? Ok : Error)
                   << metric(QStringLiteral("Last progress"), diagLastSyncProgressMs_ ? QStringLiteral("%1 s ago").arg(double(now - diagLastSyncProgressMs_) / 1000.0, 0, 'f', 1)
                                                                                  : QStringLiteral("never"),
                             syncFlowing ? Ok : diagLastSyncProgressMs_ ? Error : Warn);
        st.metrics += syncMetrics;
        st.why = h == Ok ? QStringLiteral("Locked; %1 arriving.").arg(syncUnit)
               : h == Warn ? (acquiring ? QStringLiteral("Looking for the first sync marker.")
                                        : QStringLiteral("Sync comes and goes: markers found, lock not held."))
               : diagLastSyncProgressMs_ ? QStringLiteral("No sync for %1 s: lock lost.").arg(double(now - diagLastSyncProgressMs_) / 1000.0, 0, 'f', 0)
                                         : QStringLiteral("No sync marker found yet.");
        snap.stages << st;
    }

    // ---- 5. error correction -----------------------------------------------------------------------
    {
        Stage st;
        st.key = QStringLiteral("fec");
        st.title = fecTitle;
        st.detail = fecDetail;
        st.unit = fecUnit;
        st.passed = fecPassed;
        st.rejected = fecRejected;
        const double passRate = fecPassed >= 0 ? diagRate(QStringLiteral("fecPass"), fecPassed, winWall) : 0.0;
        const double rejRate = fecRejected >= 0 ? diagRate(QStringLiteral("fecRej"), fecRejected, winWall) : 0.0;
        const double rej = rejectFraction(passRate, rejRate);
        int h = fecOverride >= 0 ? fecOverride : gradeReject(rej, fecWarnAt, fecErrorAt);
        if (fecOverride < 0 && syncFlowing && passRate <= 0.0 && rejRate > 0.0) h = Error;   // all failing
        if (!syncFlowing && fecOverride < 0) h = std::min(h, int(Idle));                  // nothing to check
        if (acquiring && h == Error) h = Warn;   // the first frames after lock often fail while the loops settle
        st.health = h;
        if (fecRejected >= 0) {
            st.headline = std::isfinite(rej) ? QStringLiteral("%1 %").arg((1.0 - rej) * 100.0, 0, 'f', 1) : QStringLiteral("—");
            st.caption = QStringLiteral("%1 passing now").arg(fecUnit);
            st.spark = std::isfinite(rej) ? (1.0 - rej) * 100.0 : std::nan("");
            st.sparkUnit = QStringLiteral("% pass");
            st.metrics << metric(QStringLiteral("Passing now"), st.headline, gradeReject(rej, fecWarnAt, fecErrorAt))
                       << metric(QStringLiteral("Passed"), n0(fecPassed))
                       << metric(QStringLiteral("Rejected"), n0(fecRejected), rejRate > 0.0 ? Warn : Ok);
        } else {
            st.headline = perS(passRate);
            st.caption = QStringLiteral("%1 through").arg(fecUnit);
            st.spark = passRate;
            st.sparkUnit = QStringLiteral("/s");
            if (fecPassed >= 0) st.metrics << metric(QStringLiteral("Through"), n0(fecPassed));
        }
        if (fecNone)
            st.metrics << metric(QStringLiteral("Coding"), QStringLiteral("none on this link"), Ok,
                                 QStringLiteral("Bit errors are not corrected; this stage measures them instead"));
        st.metrics += fecMetrics;
        st.why = !fecWhyOverride.isEmpty() ? fecWhyOverride
               : h == Idle ? QStringLiteral("Waiting for synced %1.").arg(fecUnit)
               : h == Error ? QStringLiteral("%1 of the synced %2 fail the check: the errors are more than the code can correct.")
                                  .arg(pct(rej, 0), fecUnit)
               : h == Warn ? QStringLiteral("%1 of the synced %2 fail the check.").arg(pct(rej, 1), fecUnit)
                           : QStringLiteral("Errors corrected; %1 pass.").arg(fecUnit);
        snap.stages << st;
    }

    // ---- 6. data layer ------------------------------------------------------------------------------
    {
        Stage st;
        st.key = QStringLiteral("data");
        st.title = QStringLiteral("Data layer");
        int h = Idle;
        const auto assembler = [&](const nadirstation::gk2a::LritFileAssembler& a, const QString& what) {
            st.detail = QStringLiteral("Virtual channels → CCSDS packets → %1").arg(what);
            st.unit = QStringLiteral("packets");
            st.passed = double(a.packetsSeen);
            st.rejected = double(a.crcFailures);
            const double pRate = diagRate(QStringLiteral("pkt"), double(a.packetsSeen), winWall);
            const double cRate = diagRate(QStringLiteral("pktCrc"), double(a.crcFailures), winWall);
            const double gRate = diagRate(QStringLiteral("vcGap"), double(a.frameDiscontinuities), winWall);
            const double dRate = diagRate(QStringLiteral("limitDrop"), double(a.limitDrops), winWall);
            const double fill = a.cadusFed ? double(a.fillerCadus) / double(a.cadusFed) : std::nan("");
            const double buf = double(a.bufferedBytes()) / double(nadirstation::gk2a::LritFileAssembler::kMaxBufferedBytes);
            st.headline = perS(pRate);
            st.caption = QStringLiteral("packets");
            st.spark = pRate;
            st.sparkUnit = QStringLiteral("/s");
            const double crcFrac = rejectFraction(pRate, cRate);
            h = a.cadusFed == 0 ? Idle : Ok;
            h = worst(h, gradeReject(crcFrac, 0.02, 0.2));
            if (gRate > 0.0) h = worst(h, Warn);
            if (dRate > 0.0 || buf > 0.9) h = Error;
            else if (buf > 0.6) h = worst(h, Warn);
            st.metrics << metric(QStringLiteral("CADUs in"), n0(double(a.cadusFed)))
                       << metric(QStringLiteral("Fill frames"), pct(fill), Ok, QStringLiteral("Idle frames the satellite sends between files"))
                       << metric(QStringLiteral("Packets"), n0(double(a.packetsSeen)))
                       << metric(QStringLiteral("Packet CRC failures"), n0(double(a.crcFailures)), cRate > 0.0 ? Warn : Ok)
                       << metric(QStringLiteral("VC discontinuities"), n0(double(a.frameDiscontinuities)), gRate > 0.0 ? Warn : Ok,
                                 QStringLiteral("Frame counter jumps: CADUs lost between RS and here"))
                       << metric(QStringLiteral("Packets zero-filled"), n0(double(a.packetsZeroFilled)), a.packetsZeroFilled ? Warn : Ok)
                       << metric(QStringLiteral("Files rescued"), n0(double(a.filesRescued)))
                       << metric(QStringLiteral("Assembly buffer"), QStringLiteral("%1 MiB (%2)").arg(double(a.bufferedBytes()) / 1048576.0, 0, 'f', 1).arg(pct(buf, 0)),
                                 buf > 0.9 ? Error : buf > 0.6 ? Warn : Ok)
                       << metric(QStringLiteral("Dropped at size limit"), n0(double(a.limitDrops)), a.limitDrops ? Error : Ok);
            st.why = h == Error ? QStringLiteral("Packets are being dropped in reassembly (buffer full or size limit).")
                   : gRate > 0.0 ? QStringLiteral("CADUs missing between frames (counter jumps): parts of files will be blank.")
                   : h == Warn ? QStringLiteral("%1 of packets fail their CRC.").arg(pct(crcFrac, 1))
                   : h == Idle ? QStringLiteral("No CADU has reached the packet layer yet.")
                               : QStringLiteral("Packets reassembled without gaps.");
        };
        switch (link) {
        case Link::Gk2aLrit: assembler(gk2aLritAssembler_, QStringLiteral("LRIT files")); break;
        case Link::Gk2aHrit: assembler(hritAssembler_, QStringLiteral("HRIT files")); break;
        case Link::ElektroLrit: case Link::ElektroHrit: assembler(elektroAssembler_, QStringLiteral("xRIT files")); break;
        case Link::Fy2: {
            st.detail = QStringLiteral("DOC, calibration and MANAM subcommutation");
            const int cal = calibrationGroupsSeen_, man = manamGroupCount_, orb = orbitAttitudeGroupsSeen_;
            st.headline = QStringLiteral("%1 / 25").arg(cal);
            st.caption = QStringLiteral("calibration groups");
            st.spark = cal;
            st.sparkUnit = QStringLiteral("groups");
            h = goodFrames_ > 0 ? Ok : Idle;
            st.metrics << metric(QStringLiteral("Calibration table"), calibrationLutReady_ ? QStringLiteral("ready (%1)").arg(calibrationSource_.isEmpty() ? QStringLiteral("this image") : calibrationSource_)
                                                                                         : QStringLiteral("%1 / 25 groups").arg(cal),
                                 calibrationLutReady_ ? Ok : goodFrames_ > 200 ? Warn : Ok,
                                 QStringLiteral("Brightness temperatures need all 25 groups"))
                       << metric(QStringLiteral("Orbit / attitude"), QStringLiteral("%1 / 25 groups").arg(orb))
                       << metric(QStringLiteral("MANAM"), QStringLiteral("%1 / 25 groups").arg(man));
            st.why = h == Idle ? QStringLiteral("No scan line has passed yet.")
                   : calibrationLutReady_ ? QStringLiteral("DOC data arriving; calibration table complete.")
                                          : QStringLiteral("DOC data arriving; calibration table still filling.");
            break;
        }
        case Link::Gvar: {
            st.detail = QStringLiteral("Blocks → scan lines → sectors");
            st.unit = QStringLiteral("lines");
            const double lines = gvar_ ? double(gvar_->linesStored()) : 0.0;
            const int queued = gvar_ ? gvar_->queuedBlocks() : 0;
            st.passed = lines;
            const double lRate = diagRate(QStringLiteral("gvarLines"), lines, winWall);
            st.headline = perS(lRate);
            st.caption = QStringLiteral("scan lines");
            st.spark = lRate;
            st.sparkUnit = QStringLiteral("/s");
            h = lines > 0 ? (queued > 400 ? Warn : Ok) : Idle;
            st.metrics << metric(QStringLiteral("Lines stored"), n0(lines))
                       << metric(QStringLiteral("Image queue"), QStringLiteral("%1 blocks").arg(queued), queued > 400 ? Warn : Ok,
                                 QStringLiteral("Blocks waiting for the image assembler thread"));
            st.why = h == Idle ? QStringLiteral("No scan line assembled yet.")
                   : h == Warn ? QStringLiteral("The image assembler is falling behind.") : QStringLiteral("Scan lines assembling.");
            break;
        }
        case Link::GoesSd: {
            st.detail = QStringLiteral("Blocks → imager scans + sounder frames");
            st.unit = QStringLiteral("scans");
            if (goessd_) {
                const double scans = double(goessd_->scans());
                const double outl = double(goessd_->widthOutliers());
                st.passed = scans;
                st.rejected = outl;
                const double sRate = diagRate(QStringLiteral("sdScans"), scans, winWall);
                const double oRate = diagRate(QStringLiteral("sdOutl"), outl, winWall);
                st.headline = perS(sRate);
                st.caption = QStringLiteral("imager scans");
                st.spark = sRate;
                st.sparkUnit = QStringLiteral("/s");
                h = scans > 0 ? Ok : Idle;
                if (oRate > 0.0) h = worst(h, Warn);
                st.metrics << metric(QStringLiteral("Scans"), n0(scans))
                           << metric(QStringLiteral("Scan width"), goessd_->scanWidth() > 0 ? QString::number(goessd_->scanWidth()) : QStringLiteral("—"))
                           << metric(QStringLiteral("Width outliers"), n0(outl), oRate > 0.0 ? Warn : Ok,
                                     QStringLiteral("Scans whose length disagreed with the running width"))
                           << metric(QStringLiteral("Direction fixes"), n0(double(goessd_->directionFixes())))
                           << metric(QStringLiteral("Mode changes"), n0(double(goessd_->modeChanges())))
                           << metric(QStringLiteral("Sounder frames"), n0(double(goessd_->sounderFrames())));
                st.why = h == Idle ? QStringLiteral("No complete scan yet.")
                       : h == Warn ? QStringLiteral("Some scans have the wrong length (bit slips).") : QStringLiteral("Imager scans arriving.");
            }
            break;
        }
        case Link::Metop: case Link::Meteor: {
            st.detail = QStringLiteral("Frames → instrument lines");
            st.unit = QStringLiteral("lines");
            const double lines = polarImager_ ? double(polarImager_->lines()) : 0.0;
            st.passed = lines;
            const double lRate = diagRate(QStringLiteral("polarLines"), lines, winWall);
            st.headline = perS(lRate);
            st.caption = polarImager_ ? polarImager_->instrument() + QStringLiteral(" lines") : QStringLiteral("image lines");
            st.spark = lRate;
            st.sparkUnit = QStringLiteral("/s");
            h = lines > 0 ? Ok : Idle;
            st.metrics << metric(QStringLiteral("Image lines"), n0(lines))
                       << metric(QStringLiteral("Passes this run"), QString::number(polarPasses_));
            st.why = h == Idle ? QStringLiteral("No image line yet.") : QStringLiteral("Instrument lines arriving.");
            break;
        }
        case Link::Ggak: {
            st.detail = QStringLiteral("Frames → telemetry records");
            st.unit = QStringLiteral("frames");
            st.passed = double(ggakGood_);
            const double gRate = diagRate(QStringLiteral("ggakFrames"), double(ggakGood_), winWall);
            st.headline = perS(gRate);
            st.caption = QStringLiteral("frames stored");
            st.spark = gRate;
            st.sparkUnit = QStringLiteral("/s");
            h = ggakGood_ > 0 ? Ok : Idle;
            st.metrics << metric(QStringLiteral("Frames stored"), n0(double(ggakGood_)));
            st.why = h == Idle ? QStringLiteral("No good frame yet.") : QStringLiteral("Telemetry frames stored.");
            break;
        }
        }
        st.health = h;
        snap.stages << st;
    }

    // ---- 7. products --------------------------------------------------------------------------------
    {
        Stage st;
        st.key = QStringLiteral("product");
        st.title = QStringLiteral("Products");
        double files = -1.0;
        QString what = QStringLiteral("files");
        switch (link) {
        case Link::Gk2aLrit: files = double(gk2aFilesWritten_); st.detail = QStringLiteral("LRIT files, full disk, products"); break;
        case Link::Gk2aHrit: files = double(hritFilesCompleted_); st.detail = QStringLiteral("HRIT segments, full disk"); break;
        case Link::ElektroLrit: case Link::ElektroHrit:
            files = double(elektroFiles_); st.detail = QStringLiteral("xRIT files, image segments");
            if (elektroProducts_) st.metrics << metric(QStringLiteral("Image segments"), n0(double(elektroProducts_->segmentsDecoded())));
            break;
        case Link::Fy2: files = lastReportedLine_ > 0 ? double(lastReportedLine_) : 0.0; what = QStringLiteral("lines");
            st.detail = QStringLiteral("Full-disk picture, line by line");
            st.metrics << metric(QStringLiteral("Picture progress"), QStringLiteral("%1 / %2 lines").arg(std::max(0, lastReportedLine_)).arg(SVISSR_SCAN_LINES));
            break;
        case Link::Gvar: files = gvar_ ? double(gvar_->linesStored()) : 0.0; what = QStringLiteral("lines");
            st.detail = QStringLiteral("Imager sectors, live view"); break;
        case Link::GoesSd: files = goessd_ ? double(goessd_->scans()) : 0.0; what = QStringLiteral("scans");
            st.detail = QStringLiteral("Imager pictures, sounder frames"); break;
        case Link::Metop: case Link::Meteor: files = polarImager_ ? double(polarImager_->lines()) : 0.0; what = QStringLiteral("lines");
            st.detail = QStringLiteral("Pass images, written when the pass ends"); break;
        case Link::Ggak: files = double(ggakGood_); what = QStringLiteral("frames"); st.detail = QStringLiteral("GGAK frame files"); break;
        }
        st.headline = n0(files);
        st.caption = what + QStringLiteral(" so far");
        st.spark = diagRate(QStringLiteral("prod"), files, winWall);
        st.sparkUnit = QStringLiteral("/s");
        st.passed = files;
        st.unit = what;
        st.health = files > 0 ? Ok : Idle;
        st.metrics.prepend(metric(what.left(1).toUpper() + what.mid(1), n0(files)));
        st.why = files > 0 ? QStringLiteral("Output is being written.")
                           : QStringLiteral("Nothing written yet; products appear as files or images complete.");
        snap.stages << st;
    }

    // A stage downstream of a red one is not separately at fault: show it as starved (idle) rather
    // than red, so the first red node is where to look.
    bool upstreamDown = false;
    for (Stage& st : snap.stages) {
        if (upstreamDown && st.health == Error && st.key != QLatin1String("iq")) {
            st.health = Idle;
            st.why = QStringLiteral("Starved: an earlier stage is failing. ") + st.why;
        }
        if (st.health == Error && st.key != QLatin1String("iq")) upstreamDown = true;
    }
    return snap;
}
