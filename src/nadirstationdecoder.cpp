#include "nadirstationdecoder.h"
#include <future>
#include <QImageWriter>
#include <cstring>
#include <QFileInfo>
#include "des.h"
#include "lritadd.h"
#include "dsp.h"
#include "imageproducts.h"
#include "svissrsoft.h"
#include "gvarengine.h"
#include "goessd.h"
#include "runtime_logger.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QTimeZone>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QtGlobal>
#include <algorithm>
#include <limits>
#include <map>
#include <bit>
#include <exception>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>
#include <numeric>
#include <functional>
#include <iterator>
#include <unordered_map>
#include <atomic>
#include <chrono>
#include <QScopeGuard>

// Wall-clock seconds spent in the main stages of the FY-2 chain, for measuring the real-time margin.
// 0 demodulator, 1 frame locker, 2 decodeFrame (sector checks, soft stage, line assembly),
// 3 soft-decision stage alone (inside 2). Read by the tools; a few clock reads per block, nothing more.
namespace nadirstation {
std::atomic<double> g_stageSeconds[4] = {0.0, 0.0, 0.0, 0.0};
}
namespace {
inline void addStageTime(int slot, std::chrono::steady_clock::time_point since) {
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - since).count();
    nadirstation::g_stageSeconds[slot].fetch_add(s, std::memory_order_relaxed);
}
}

namespace {
constexpr std::size_t SVISSR_SYNC_BITS = 10000;
constexpr std::size_t SVISSR_DATA_BITS = 354848;
constexpr int SVISSR_SCAN_LINES = 2291;
constexpr int SVISSR_IR_SAMPLES = 2291;
constexpr int SVISSR_VIS_SAMPLES = 9164;
// The sync marker is still exact. Inside an acquired frame, permit more local
// noise in the known zero-fill and sector ID so usable scan lines survive weak
// fades. The DOC sector remains mandatory; other sectors are blanked separately
// when they fail these checks.
constexpr int MAX_SECTOR_FILLER_ERRORS = 1500;
constexpr int MIN_TRUSTED_SECTORS = 4;
constexpr int MAX_SECTOR_ID_BIT_ERRORS = 2;
constexpr int MAX_SYNC_MARKER_BIT_ERRORS = 8;
// Soft-decision path (only when the demodulator's symbol values come with the frame).
// See svissrsoft.h: the sector's known-zero filler, read from the symbols, tells an aligned sector
// from anything else long after the 16-bit IDs stopped being readable.
// The soft path switches on only when the average filler is dirtier than kSoftEngageFiller
// (about two per cent differential bit errors); cleaner frames go through the unchanged hard path.
constexpr int SOFT_ENGAGE_MEAN_FILLER = 40;

constexpr uint64_t SYNC_MARKER = 0b0100101110111011101110011001100110010101010101010111111111111111ULL;

struct SectorSpec { int idBits; int dataBits; int totalBits; uint32_t expectedId; const char* name; };

constexpr std::array<SectorSpec, 13> makeSectorSpecs() {
    return {{
        {16, 18328, 20408, 0x0000, "DOC"},
        {16, 18328, 20408, 0x1111, "IR1_H"},
        {16, 18328, 20408, 0x2222, "IR2_H"},
        {16, 18328, 20408, 0x4444, "IR3_H"},
        {12, 54984, 57060, 0x06DB, "VIS1"},
        {12, 54984, 57060, 0x0B6D, "VIS2"},
        {12, 54984, 57060, 0x0DB6, "VIS3"},
        {12, 54984, 57060, 0x0FFF, "VIS4"},
        {16, 4582, 6662, 0x8888, "IR1_L"},
        {16, 4582, 6662, 0x9999, "IR2_L"},
        {16, 4582, 6662, 0xAAAA, "IR3_L"},
        {16, 22910, 24990, 0xBBBB, "IR4"},
        {0, 0, 0, 0, "END"}
    }};
}
constexpr auto SECTORS = makeSectorSpecs();

QString observationManifestPath(const QDir& directory) {
    const QString current = directory.filePath(QStringLiteral("FY2_Observation.json"));
    if (QFile::exists(current)) return current;
    return directory.filePath(QStringLiteral("MGAS_Observation.json")); // backward compatibility
}

bool readBcdByte(const std::vector<uint8_t>& bytes, std::size_t offset, int& value) {
    if (offset >= bytes.size()) return false;
    const uint8_t byte = bytes[offset];
    const int hi = (byte >> 4) & 0x0F;
    const int lo = byte & 0x0F;
    if (hi > 9 || lo > 9) return false;
    value = hi * 10 + lo;
    return true;
}

bool readBcd2(const std::vector<uint8_t>& bytes, std::size_t offset, int& value) {
    int high = 0;
    int low = 0;
    if (!readBcdByte(bytes, offset, high) || !readBcdByte(bytes, offset + 1, low)) return false;
    value = high * 100 + low;
    return true;
}

const std::vector<uint16_t>& crcFlipSyndromes(int length) {
    static std::unordered_map<int, std::vector<uint16_t>> cache;
    const auto it = cache.find(length);
    if (it != cache.end()) return it->second;

    std::vector<uint16_t> out(static_cast<std::size_t>(length));
    auto update = [](uint16_t crc, uint8_t bit) {
        const bool msb = (crc & 0x8000u) != 0;
        crc = static_cast<uint16_t>(crc << 1);
        if (msb ^ (bit != 0)) crc ^= 0x1021u;
        return crc;
    };
    uint16_t c = update(0, 1);
    out[static_cast<std::size_t>(length - 1)] = c;
    for (int pos = length - 2; pos >= 0; --pos) {
        c = update(c, 0);
        out[static_cast<std::size_t>(pos)] = c;
    }
    return cache.emplace(length, std::move(out)).first->second;
}

uint16_t crc16Local(const std::vector<uint8_t>& bits) {
    uint16_t crc = 0;
    for (const uint8_t bit : bits) {
        const bool msb = (crc & 0x8000u) != 0;
        crc = static_cast<uint16_t>(crc << 1);
        if (msb ^ (bit != 0)) crc ^= 0x1021u;
    }
    return crc;
}

bool correctSectorCrc(std::vector<uint8_t>& segment, int idBits, int dataBits,
                      const std::vector<uint8_t>& reliability, std::size_t dataStart,
                      uint16_t received, int& correctedBits) {
    correctedBits = 0;
    uint16_t calc = crc16Local(segment);
    if (calc == received) return true;
    if (segment.size() <= static_cast<std::size_t>(idBits)) return false;

    const auto& synd = crcFlipSyndromes(static_cast<int>(segment.size()));
    const uint16_t target = static_cast<uint16_t>(calc ^ received);

    // One-bit correction, restricted to data bits so a valid sector ID is never changed.
    // CRC-16 syndromes are not a unique locator over these long S-VISSR
    // sectors. Correct one bit only when its syndrome identifies exactly one
    // data position.
    int candidate = -1;
    int singleMatches = 0;
    const int dataEnd = idBits + dataBits;
    for (int pos = idBits; pos < dataEnd; ++pos) {
        if (synd[static_cast<std::size_t>(pos)] != target) continue;
        candidate = pos;
        ++singleMatches;
    }
    if (singleMatches == 1) {
        segment[static_cast<std::size_t>(candidate)] ^= 1u;
        if (crc16Local(segment) == received) {
            correctedBits = 1;
            return true;
        }
        segment[static_cast<std::size_t>(candidate)] ^= 1u;
    }

    // A single wrong hard decision in the BPSK stream flips two adjacent
    // differential (NRZM) bits. Only try that physically constrained pattern;
    // searching arbitrary low-confidence pairs can CRC-match unrelated data.
    //
    // Both positions must lie inside the data field. The CRC segment continues
    // past it with the zero pad bits that byte-align a non-multiple-of-eight
    // payload, and those bits were transmitted as zero: "correcting" one of them
    // is not a correction at all, it rewrites a bit the link defined. It also
    // changes the frame the CRC is verified over, so the match that follows
    // proves nothing. The old bound (dataPos + 1 < dataBits) let p2 land on the
    // first pad bit; measured on a four-bit payload with one pad bit, a three-bit
    // error pattern written {data0, data1, pad} is reported as a clean two-bit
    // correction because single-bit syndromes for positions 0, 1 and 2 are
    // distinct while the pair (0, pad) is the only pair that reproduces the
    // target. Sectors with a two-bit pad -- IR1_L, IR2_L, IR3_L, IR4 and all four
    // VIS sectors -- are the ones this reached.
    constexpr uint8_t MAX_RELIABILITY = 248; // 0.973 on a 0..255 scale.
    int first = -1;
    int second = -1;
    int pairMatches = 0;
    for (int dataPos = 0; dataPos + 2 <= dataBits; ++dataPos) {
        const std::size_t relPos = dataStart + static_cast<std::size_t>(dataPos);
        if (relPos + 1 >= reliability.size() ||
            reliability[relPos] >= MAX_RELIABILITY ||
            reliability[relPos + 1] >= MAX_RELIABILITY)
            continue;
        const int p1 = idBits + dataPos;
        const int p2 = p1 + 1;
        if ((synd[static_cast<std::size_t>(p1)] ^ synd[static_cast<std::size_t>(p2)]) != target)
            continue;
        first = p1;
        second = p2;
        if (++pairMatches > 1) return false;
    }
    if (pairMatches == 1) {
        segment[static_cast<std::size_t>(first)] ^= 1u;
        segment[static_cast<std::size_t>(second)] ^= 1u;
        if (crc16Local(segment) == received) {
            correctedBits = 2;
            return true;
        }
        segment[static_cast<std::size_t>(first)] ^= 1u;
        segment[static_cast<std::size_t>(second)] ^= 1u;
    }
    return false;
}

quint32 readBigEndian32(const QByteArray& bytes, int offset) {
    return (quint32(static_cast<quint8>(bytes[offset])) << 24) |
           (quint32(static_cast<quint8>(bytes[offset + 1])) << 16) |
           (quint32(static_cast<quint8>(bytes[offset + 2])) << 8) |
           quint32(static_cast<quint8>(bytes[offset + 3]));
}

} // namespace

namespace nadirstation {

// Parses a Calibration-2 block into the five lookup tables, with no opinion
// about whether the result is usable. The integrity checks live in
// decodeCalibration2() so that loading the reference baseline can reuse this
// without recursively validating itself.
void parseCalibration2Tables(const QByteArray& block, std::array<QVector<float>, 5>& luts) {
    // Layout of the block, in bytes: the VIS table is 64 big-endian knots from offset 256, one every
    // 16 counts; each IR channel is a 1024-entry big-endian table, the four channels 4096 bytes apart
    // from offset 1280.
    constexpr int kVisKnotsAt = 256, kVisKnots = 64, kVisSpacing = 16;
    constexpr int kIrTablesAt = 1280, kIrTableStride = 4096, kTableSize = 1024;

    for (auto& lut : luts) lut.fill(0.0f, kTableSize);

    // VIS: read the knots, then interpolate linearly inside each segment. The first segment and the
    // counts from the last knot upwards are reserved invalids and stay zero.
    std::array<float, kVisKnots> knot{};
    for (int k = 0; k < kVisKnots; ++k)
        knot[static_cast<std::size_t>(k)] = static_cast<float>(readBigEndian32(block, kVisKnotsAt + k * 4)) * 1.0e-8f;
    QVector<float>& vis = luts[0];
    for (int segment = 1; segment < kVisKnots - 1; ++segment) {
        const float from = knot[static_cast<std::size_t>(segment)];
        const float rise = knot[static_cast<std::size_t>(segment) + 1] - from;
        const int base = segment * kVisSpacing;
        vis[base] = from;
        for (int offset = 1; offset < kVisSpacing; ++offset)
            vis[base + offset] = from + rise * (static_cast<float>(offset) / 15.0f);
    }

    // IR channels, wire order IR1..IR4. The tables are sent in reverse count order, so entry n of the
    // wire table is the value for count 1023 - n.
    for (int channel = 0; channel < 4; ++channel) {
        QVector<float>& table = luts[static_cast<std::size_t>(channel) + 1];
        const int start = kIrTablesAt + kIrTableStride * channel;
        for (int count = 0; count < kTableSize; ++count) {
            const int wireIndex = kTableSize - 1 - count;
            table[count] = static_cast<float>(readBigEndian32(block, start + wireIndex * 4)) * 1.0e-3f;
        }
    }
}

// The reference table shipped with the station: a complete, already-verified
// Calibration-2 block (its decoded tables agree with the official NSMC
// CALChannelIR1..IR4 to 7e-06 K). Loaded once and used to check blocks that
// actually arrive over the air.
const std::array<QVector<float>, 5>* referenceCalibrationTables() {
    static std::array<QVector<float>, 5> reference;
    static bool loaded = false;
    static bool usable = false;
    if (!loaded) {
        loaded = true;
        const QString path = QCoreApplication::applicationDirPath() +
            QStringLiteral("/resources/orbits/fy2_calibration2_backup.bin");
        QFile file(path);
        if (file.open(QIODevice::ReadOnly)) {
            const QByteArray block = file.readAll();
            if (block.size() >= 1280 + 4 * 4096) {
                parseCalibration2Tables(block, reference);
                usable = true;
                nadirstation::log::write(QStringLiteral(
                    "FY-2 Calibration-2 reference table loaded from %1").arg(path));
            }
        }
        if (!usable) {
            nadirstation::log::write(QStringLiteral(
                "FY-2 Calibration-2 reference table not available; live tables will "
                "be accepted on their internal consistency alone"));
        }
    }
    return usable ? &reference : nullptr;
}

// Why this is stricter than a range count.
//
// The original test counted how many of the 1024 values fell in 100..500 K and
// rejected the block below 768. A real block received here (hash 7C1993A1,
// seven sessions) carries 26 extreme values -- down to -2.1e6 K -- yet keeps
// 1014..1020 of 1024 in range, so it passed and its damaged table reached the
// products. A count cannot see damage that is sparse and scattered; these tests
// look at the shape of the table and, decisively, at how far it departs from
// the verified reference.
bool decodeCalibration2(const QByteArray& block, std::array<QVector<float>, 5>& luts) {
    constexpr int requiredBytes = 1280 + 4 * 4096;
    if (block.size() < requiredBytes) return false;
    parseCalibration2Tables(block, luts);

    bool ok = true;
    for (int channel = 1; channel < 5 && ok; ++channel) {
        const QVector<float>& table = luts[static_cast<std::size_t>(channel)];
        int valid = 0;
        int outOfRange = 0;
        int descending = 0;
        int bigJumps = 0;
        for (int i = 0; i < table.size(); ++i) {
            const float value = table[i];
            if (!std::isfinite(value)) { ++outOfRange; continue; }
            if (value >= 100.0f && value <= 500.0f) ++valid;
            else ++outOfRange;
            if (i > 0) {
                const float delta = value - table[i - 1];
                if (delta < 0.0f) ++descending;
                if (std::fabs(delta) > 20.0f) ++bigJumps;
            }
        }
        if (valid < 768) {
            nadirstation::log::write(QStringLiteral(
                "Calibration-2 rejected: IR%1 has only %2/1024 values in 100..500 K")
                .arg(channel).arg(valid));
            ok = false;
            break;
        }
        // A genuine table is monotonic in count and smooth. Measured on the
        // healthy reference: 0 descending steps and 0 jumps above 20 K; on the
        // damaged block: 11/54/19/25 descending steps.
        if (descending > 12 || bigJumps > 12) {
            nadirstation::log::write(QStringLiteral(
                "Calibration-2 rejected: IR%1 is not a plausible table "
                "(%2 descending step(s), %3 jump(s) over 20 K, %4 out of range)")
                .arg(channel).arg(descending).arg(bigJumps).arg(outOfRange));
            ok = false;
            break;
        }
        // The decisive test: compare against the verified local reference. The
        // reference's reserved end entries are not temperatures, so they are
        // excluded; everything else must agree closely. This is what makes a
        // low signal-to-noise live capture safe to use.
        if (const auto* reference = referenceCalibrationTables()) {
            const QVector<float>& ref = (*reference)[static_cast<std::size_t>(channel)];
            float worst = 0.0f;
            int worstIndex = -1;
            for (int i = 16; i < 1008; ++i) {
                const float r = ref[i];
                if (!(r >= 100.0f && r <= 500.0f)) continue;
                const float deviation = std::fabs(table[i] - r);
                if (deviation > worst) { worst = deviation; worstIndex = i; }
            }
            if (worstIndex >= 0 && worst > 20.0f) {
                nadirstation::log::write(QStringLiteral(
                    "Calibration-2 rejected: IR%1 deviates from the verified reference "
                    "by %2 K at count %3 (limit 20 K)")
                    .arg(channel).arg(double(worst), 0, 'f', 3).arg(worstIndex));
                ok = false;
                break;
            }
        }
    }
    return ok;
}

} // namespace nadirstation

namespace {
// Lossless PNG at a light zlib level (Qt quality 80): a 9164 x 5076 VIS image is written in 0.76 s
// instead of 2.2 s, for files about 14 % larger. The end-of-image product set is a dozen such
// files, written on the decoder thread.
bool savePng(const QImage& image, const QString& path) {
    QImageWriter writer(path, "png");
    writer.setQuality(80);
    return writer.write(image);
}
}  // namespace

NadirStationDecoder::NadirStationDecoder(QObject* parent) : QObject(parent) {
    recentLines_.reserve(SVISSR_SCAN_LINES + 32);
    qRegisterMetaType<nadirstation::diag::Snapshot>("nadirstation::diag::Snapshot");
    // Every link reports its symbol SNR through this signal; the Decoder page's demodulator stage
    // reads the last value from here instead of each link keeping a copy for it.
    connect(this, &NadirStationDecoder::demodSnrMetricsReady, this,
            [this](float snr, float, float) { diagSnrDb_.store(snr, std::memory_order_relaxed); }, Qt::DirectConnection);
    ingressTimer_ = new QTimer(this);
    ingressTimer_->setInterval(5);
    ingressTimer_->setTimerType(Qt::PreciseTimer);
    connect(ingressTimer_, &QTimer::timeout, this, [this]{
        if(!((isHrit()||isGvar()||isGoesSd()||isElektroHrit()||isPolar()) && hritDspRunningNow())) drainQueuedSamplesLimited(8);
        if(isHrit())publishHritPreview();
        if(isElektro())publishElektro();
        if(isGvar())publishGvarUpdates();
        if(isGoesSd())publishGoesSdUpdates();
    });
}

void NadirStationDecoder::setOutputRoot(const QString& root) {
    outputRoot_ = root;
    QDir().mkpath(outputRoot_);
}

void NadirStationDecoder::configurePsk() {
    pskDemod_.setQpsk(isHrit());
    // HRIT: the link decoder (Viterbi, sync, Reed-Solomon) runs on its own thread so a busy machine
    // does not overflow the IQ queue and drop blocks of samples.
    hritDecoder_.setAsync(isHrit());
    hritDecoder_.reset(); hritAssembler_.reset(); hritAssembler_.setStrict(true); hritAssembler_.setLossTolerant(true);
    hritFilesCompleted_=0;
    hritIqSamples_=0;
    lastHritProgressStatusMs_=0;
    if(hritImages_)hritImages_->reset();
    if(isGvar()){
        // GVAR has its own BPSK chain, block deframer and image assembler (gvarengine.h).
        if(!gvar_)gvar_=std::make_unique<nadirstation::gvar::Engine>();
        gvar_->setOutputRoot(outputRoot_);
        gvar_->configure(sampleRate_,symbolRate_,rrcAlpha_,pllBandwidth_);
        gvar_->reset();
    }
    if(isGoesSd()){
        if(!goessd_)goessd_=std::make_unique<nadirstation::goessd::Engine>();
        goessd_->setOutputRoot(outputRoot_);
        goessd_->configure(sampleRate_);
        goessd_->reset();
    }
    // HRIT runs on its own QPSK demodulator (polyphase matched filter, works from 1.5 samples a
    // symbol); see processHritSamples().
    if (isHrit() && sampleRate_ >= 1.05 * symbolRate_)
        hritDemod_.configure(static_cast<float>(sampleRate_), static_cast<float>(symbolRate_),
                             rrcAlpha_, kHritCarrierLoopBw, kHritTimingLoopBw);
    // PskDemod upsamples rates below three samples/symbol before timing recovery. Reject only
    // rates that cannot contain the configured RRC signal bandwidth, rather than requiring 2 sps.
    const double minimumRate = symbolRate_ * (1.0 + static_cast<double>(rrcAlpha_));
    // FY-2 S-VISSR is unfiltered rectangular NRZ: integrate-and-dump is its matched filter (about
    // 0.4 dB better than an RRC in simulation). Other BPSK links are RRC-shaped.
    // ELEKTRO-L HRIT: offset-QPSK demodulator and the 8-hypothesis link decoder on its own thread.
    elektroHrit_.setAsync(false);
    elektroHrit_.setOffsetPairing(isElektroHrit());
    elektroHrit_.reset();
    if (isElektroHrit()) {
        elektroHrit_.setAsync(true);
        elektroOqpsk_.setOffsetQ(true);
        elektroOqpsk_.setAcquisition(kElektroAcqBlocks, kElektroAcqMaxHz);
        if (sampleRate_ >= 1.6 * symbolRate_)
            elektroOqpsk_.configure(static_cast<float>(sampleRate_), static_cast<float>(symbolRate_), rrcAlpha_,
                                    kElektroCarrierLoopBw, kElektroTimingLoopBw);
    }
    configurePolar();
    pskDemod_.setRectangularPulse(!isHrit() && !isGk2a() && !isGgak() && !isElektro() && fy2RectMatched_);
    if (sampleRate_ >= minimumRate)
        pskDemod_.configure(static_cast<float>(sampleRate_), static_cast<float>(symbolRate_), rrcAlpha_, pllBandwidth_);
}

void NadirStationDecoder::setSampleRate(double sampleRate) {
    std::unique_lock<std::mutex> dspGuard(hritDspMutex_, std::defer_lock);
    if (hritDspRunningNow()) dspGuard.lock();
    // Any rate from 1 kS/s. This used to ignore everything up to 100 kS/s, so a 60 kS/s recording
    // was decoded as if it were still at the previous rate (2.5 MS/s) and nothing locked.
    if (sampleRate > 1000.0) {
        const bool changed = std::fabs(sampleRate_ - sampleRate) > 0.5;
        sampleRate_ = sampleRate;
        if (changed) lowRateReported_ = false;
        // Metrics are emitted periodically while receiving.  Do NOT rebuild the
        // demodulator (filters, AGC, carrier and timing loops) when the rate is unchanged: that would
        // reset the carrier/timing loops every 100 ms and destroy lock.
        if (changed) {
            configurePsk();
            if (isGk2a()) resetGk2aLinkForRetune();
        }
    }
}

void NadirStationDecoder::setSymbolRate(double symbolRate) {
    std::unique_lock<std::mutex> dspGuard(hritDspMutex_, std::defer_lock);
    if (hritDspRunningNow()) dspGuard.lock();
    if (!(symbolRate > 1000.0)) return;
    const bool wasGk2a = isGk2a();
    const bool changed = std::fabs(symbolRate_ - symbolRate) > 0.5;
    symbolRate_ = symbolRate;
    if (!changed) return;
    // Reconfigure and drop anything already accumulated: the RRC filter, the input decimation and
    // the timing loop all scale with the symbol rate, so samples buffered at the old rate would
    // be interpreted with the new filters and produce a constellation that means nothing.
    lowRateReported_ = false;
    configurePsk();
    pskDemod_.reset();
    dspAccumulator_.clear();
    displayAccumulator_.clear();
    displaySamplesSinceUpdate_ = 0;
    if (wasGk2a || isGk2a()) resetGk2aLinkForRetune();
}

void NadirStationDecoder::setDemodParameters(float rrcAlpha, float pllBandwidth) {
    std::unique_lock<std::mutex> dspGuard(hritDspMutex_, std::defer_lock);
    if (hritDspRunningNow()) dspGuard.lock();
    rrcAlpha_ = std::clamp(rrcAlpha, 0.05f, 1.0f);
    pllBandwidth_ = std::clamp(pllBandwidth, 0.0005f, 0.25f);
    configurePsk();
    if (isGk2a()) resetGk2aLinkForRetune();
}

void NadirStationDecoder::setIngressBackpressureEnabled(bool enabled) {
    ++ingressBackpressureEpoch_;
    ingressBackpressureEnabled_.store(enabled, std::memory_order_release);
    if (!enabled) ingressCv_.notify_all();
}

void NadirStationDecoder::resetGk2aLinkForRetune() {
    // A rate or loop change invalidates recovered symbols and assembled frames.
    gk2aLrit_.reset();
    hritDecoder_.reset(); hritAssembler_.reset(); hritAssembler_.setStrict(true); hritAssembler_.setLossTolerant(true);
    if(hritImages_)hritImages_->reset();
    gk2aLritAssembler_.reset();
    gk2aEverHadFrame_ = false;
    lastGk2aSyncMs_ = 0;
    lastGk2aRsOkMs_ = 0;
    lastGk2aParityRejectLogMs_ = 0;
    gk2aLastFrameMs_ = gk2aStreamMs_;
    gk2aRecoveryAttempt_ = 0;
    gk2aRecoveryReferenceCfoHz_ = 0.0f;
    gk2aLastGoodCfoHz_ = 0.0f;
    lock_ = 0;
}

QString NadirStationDecoder::spacecraftFolder() const {
    // The folder name is the spacecraft, so a directory listing says what produced its contents.
    // UNKNOWN rather than an empty string when nothing is selected: a session that starts before
    // the operator picks a satellite must still land somewhere findable.
    const QString name = satelliteName_.trimmed();
    if (name.isEmpty()) return QStringLiteral("UNKNOWN");
    if (name.compare(QStringLiteral("GK-2A"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("GK-2A");
    if (name.compare(QStringLiteral("FY-2G"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("FY-2G");
    if (name.compare(QStringLiteral("FY-2H"), Qt::CaseInsensitive) == 0)
        return QStringLiteral("FY-2H");
    if (name.contains(QStringLiteral("ELEKTRO"), Qt::CaseInsensitive))
        return QStringLiteral("ELEKTRO-L");
    if (name.startsWith(QStringLiteral("MetOp"), Qt::CaseInsensitive)) return QStringLiteral("MetOp");
    if (name.startsWith(QStringLiteral("METEOR"), Qt::CaseInsensitive)) return QStringLiteral("METEOR");
    return name;
}

void NadirStationDecoder::beginObservation() {
    const QString stamp = QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss_zzz");
    // <root>/<spacecraft>/<timestamp>/...
    const QString spacecraftDir = QDir(outputRoot_).filePath(spacecraftFolder());
    sessionDir_ = QDir(spacecraftDir).filePath(stamp);
    // GK-2A HRIT, GOES GVAR and the polar passes write their own products, straight into <root>/<spacecraft>/<observation time>/
    // (the engines create that folder when something is saved). A session folder made here would only stay empty.
    const QString folder = spacecraftFolder();
    // MetOp / METEOR passes go to <root>/<MetOp|METEOR>/<AHRPT|HRPT>_<first line time>/ (finalizePolarPass).
    const bool ownsItsFolders = folder == QStringLiteral("GK-2A HRIT") || folder == QStringLiteral("GOES GVAR") || folder == QStringLiteral("GOES SD") || isPolar();
    if (!ownsItsFolders) {
        QDir().mkpath(spacecraftDir);
        QDir().mkpath(sessionDir_);
    }
    observationFinalized_ = false;
    finalizedIrCounts10_.fill(QImage());
    pictureCycleActive_ = false;
    pictureOffFrameCount_ = 0;
}

void NadirStationDecoder::resetDecoderState() {
    resetPipelineDiag();
    frameLocker_[0].reset();
    frameLocker_[1].reset();
    goodFrames_ = badFrames_ = 0;
    lock_ = 0;
    softQuality_ = 0.0f;
    carrierOffsetHz_ = 0.0f;
    polarityHint_ = -1;
    nrzmLastBit_ = 0;
    nrzmLastReliability_ = 255;
    configurePsk();
    dspAccumulator_.clear();
    displayAccumulator_.clear();
    displaySamplesSinceUpdate_ = 0;
    firstDspWindow_ = true;
    {
        std::lock_guard<std::mutex> lock(ingressMutex_);
        ingressQueue_.clear();
        ingressQueuedSamples_ = 0;
    }
    ingressCv_.notify_all();
    recentLines_.clear();
    sequenceLineNumber_ = 1;
    lastReportedLine_ = 0;
    lineNumberOffset_ = 0;
    docCounterSeen_ = false;
    docLineContinuityWindow_.clear();
    lastReliableDocSyncSymbolPosition_ = 0;
    lastReliableDocLineNumber_ = -1;
    lastAcceptedSyncSymbolPosition_ = 0;
    lineSymbolPeriodEstimate_ = 401408.0;
    lineSymbolPeriodHistory_.clear();
    lineSymbolPeriodObservations_ = 0;
    manamRevision_ = -1;
    satelliteIdentityCandidateId_ = -1;
    satelliteIdentityCandidateLongitude_ = 0.0;
    satelliteIdentityConfirmations_ = 0;
    manamGroupCount_ = 0;
    manamGroupMask_ = 0;
    lastCompleteManam_.clear();
    manamChunkSeen_.fill(false);
    manamChunkScores_.fill(-1);
    for (auto& chunk : manamChunks_) chunk.clear();
    orbitAttitudeChunkSeen_.fill(false);
    orbitAttitudeGroupsSeen_ = 0;
    for (auto& chunk : orbitAttitudeChunks_) chunk.clear();
    resetCalibrationAssembly();
    lastGoodFrameMs_ = 0;
    lastGoodSyncSymbol_ = 0;
    lastBuiltLine_ = 0;
    lastConstellationEmitMs_ = 0;
    lastHritNoFrameStatusMs_ = 0;
    lastStatsEmitMs_ = 0;
    lastDspLogMs_ = 0;
    lastSnrMetricEmitMs_ = 0;
    peakDemodSnr_ = 0.0f;
    hritSnrY1_ = hritSnrY2_ = 0.0;
    demodSnrHistory_.clear();
    observationFinalized_ = false;
    // Start every session with fresh streaming demodulator and decoder state.
    gk2aReacquired_ = false;
    gk2aEverHadFrame_ = false;
    gk2aLastFrameMs_ = 0;
    gk2aStreamMs_ = 0;
    gk2aStreamMsExact_ = 0.0;
    lastGk2aSyncMs_ = 0;
    lastGk2aRsOkMs_ = 0;
    lastGk2aParityRejectLogMs_ = 0;
    gk2aRecoveryAttempt_ = 0;
    gk2aRecoveryReferenceCfoHz_ = 0.0f;
    gk2aLastGoodCfoHz_ = 0.0f;
    gk2aLrit_.reset();
    hritDecoder_.reset(); hritAssembler_.reset(); hritAssembler_.setStrict(true); hritAssembler_.setLossTolerant(true);
    if(hritImages_)hritImages_->reset();
    gk2aLritAssembler_.reset();
    pskDemod_.reset();
    hritDemod_.reset();
    if(gvar_&&isGvar())gvar_->reset();
    if(goessd_&&isGoesSd())goessd_->reset();
    resetGgak();
    elektroLrit_.reset();
    elektroLrit_.setRescueEnabled(true);
    elektroHrit_.reset();
    elektroOqpsk_.reset();
    resetPolar();
    elektroAssembler_.reset();
    elektroAssembler_.setStrict(true);
    elektroAssembler_.setLossTolerant(true);
    elektroSnrY1_ = elektroSnrY2_ = 0.0;
    elektroStreamMs_ = elektroLastGoodMs_ = elektroLastReacqMs_ = 0.0;
    elektroGoodSeen_ = 0;
    elektroReacq_ = 0;
    elektroEverLocked_ = false;
    elektroProgressMs_ = 0;
    elektroFiles_ = 0;
    if (isElektro()) {
        const QString service = isElektroHrit() ? QStringLiteral("HRIT") : QStringLiteral("LRIT");
        elektroProducts_ = std::make_unique<nadirstation::elektro::XritProducts>(service);
    }
}

void NadirStationDecoder::setEnabled(bool enabled) {
    try {
        setEnabledImpl(enabled);
    } catch (const std::exception& e) {
        // An exception leaving a slot on the decoder thread would end the process (std::terminate)
        // with nothing in the log; it is recorded here and the decoder is left stopped.
        enabled_ = false;
        stopping_ = false;
        nadirstation::log::write(QStringLiteral("decoder setEnabled(%1) failed: %2")
            .arg(enabled ? QStringLiteral("true") : QStringLiteral("false"), QString::fromLocal8Bit(e.what())));
        nadirstation::log::flush();
        emit statusMessage(QStringLiteral("Error while starting / stopping the decoder: %1").arg(QString::fromLocal8Bit(e.what())));
    } catch (...) {
        enabled_ = false;
        stopping_ = false;
        nadirstation::log::write(QStringLiteral("decoder setEnabled(%1) failed with a non-standard exception")
            .arg(enabled ? QStringLiteral("true") : QStringLiteral("false")));
        nadirstation::log::flush();
    }
}

void NadirStationDecoder::setEnabledImpl(bool enabled) {
    if (enabled) {
        stopping_ = false;
        nadirstation::log::setArmed(1, true);
        resetDecoderState();
        beginObservation();
            enabled_ = true;
        if (isHrit() || isGvar() || isGoesSd() || isElektroHrit() || isPolar()) startHritDsp();
        if (ingressTimer_ && !ingressTimer_->isActive()) ingressTimer_->start();
        nadirstation::log::write(QStringLiteral(
            "Decoder enabled; inputRate=%1 decimation=%2 demodRate=%3 samples/symbol=%4")
            .arg(sampleRate_, 0, 'f', 0)
            .arg(pskDemod_.inputDecimation())
            .arg(pskDemod_.sampleRate(), 0, 'f', 0)
            .arg(pskDemod_.sampleRate() / pskDemod_.symbolRate(), 0, 'f', 3));
        if (isPolar())
            emit statusMessage(isMetop()
                ? QStringLiteral("MetOp AHRPT: QPSK 2.33 Msym/s, soft-decision Viterbi + RS, waiting for AVHRR lines…")
                : QStringLiteral("METEOR HRPT: PM / Manchester 665.4 kbit/s, soft-decision matched filter, waiting for MSU-MR lines…"));
        else if (isElektro())
            emit statusMessage(isElektroHrit()
                ? QStringLiteral("ELEKTRO-L HRIT: OQPSK 1.15 Msym/s, soft-decision Viterbi + RS, waiting for CADUs…")
                : QStringLiteral("ELEKTRO-L LRIT: BPSK 294 ksym/s, soft-decision Viterbi + RS, waiting for CADUs…"));
        else
        emit statusMessage(isGoesSd() ? QStringLiteral("GOES SD: imager 2.6208 Mbit/s + sounder 40 kbit/s, soft-decision, waiting for sync…")
                           : isGvar() ? QStringLiteral("GOES GVAR: BPSK 2.111 Msym/s, soft-decision block sync + soft pixel recovery, waiting for GVAR sync…")
                           : isHrit() ? QStringLiteral("GK-2A HRIT: QPSK 3 Msym/s, soft-decision decoding, waiting for CADUs / VIS segments…") : QStringLiteral("FY-2G S-VISSR: 660 kbit/s BPSK→NRZ-M, waiting for S-VISSR PN sync…"));
        return;
    }

    if (enabled_.load()) {
        nadirstation::log::write(QStringLiteral("Decoder stopping: refusing new IQ, draining %1 queued samples")
            .arg(static_cast<qulonglong>(ingressQueuedSamples_.load())));
        // Refuse new IQ first, so the drain below empties what is queued and then ends even while
        // a receiver keeps delivering; wake a capture thread that is waiting on backpressure.
        stopping_ = true;
        ingressCv_.notify_all();
        if (ingressTimer_) ingressTimer_->stop();
        stopHritDsp();
        drainQueuedSamples();
        enabled_ = false;
        ingressCv_.notify_all();
        nadirstation::log::setArmed(1, false);   // building the final products can take a while; not a hang
        finishDecoding();
        stopping_ = false;
        nadirstation::log::write(QStringLiteral("Decoder stopped"));
        nadirstation::log::flush();
    } else {
        enabled_ = false;
        emit statusMessage(QStringLiteral("FY-2G decoder stopped."));
    }
}

void NadirStationDecoder::processSamples(const std::shared_ptr<std::vector<std::complex<float>>>& samples) {
    if (!samples || samples->empty() || samples->size()>MAX_INGRESS_SAMPLES || !enabled_.load() || stopping_.load()) return;

    std::unique_lock<std::mutex> lock(ingressMutex_);
    if (ingressBackpressureEnabled_.load(std::memory_order_acquire)) {
        constexpr std::size_t HIGH_WATER = 4u * 1024u * 1024u;
        const bool wasFull=ingressQueuedSamples_.load(std::memory_order_relaxed)>HIGH_WATER;
        const auto waitStarted=std::chrono::steady_clock::now();
        ingressCv_.wait(lock, [this] {
            return !enabled_.load() || stopping_.load() ||
                   !ingressBackpressureEnabled_.load(std::memory_order_acquire) ||
                   ingressQueuedSamples_.load(std::memory_order_relaxed) <= HIGH_WATER;
        });
        if(wasFull){
            const auto now=std::chrono::steady_clock::now();
            static thread_local auto lastWarning=std::chrono::steady_clock::time_point{};
            const auto waitMs=std::chrono::duration_cast<std::chrono::milliseconds>(now-waitStarted).count();
            if(waitMs>0) ingressWaitMs_.fetch_add(uint64_t(waitMs), std::memory_order_relaxed);
            if(waitMs>=20 && (lastWarning.time_since_epoch().count()==0 || now-lastWarning>std::chrono::seconds(5))){
                nadirstation::log::write(QStringLiteral("IQ ingress backpressure: receiver waited %1 ms; live SDR buffers may overflow if DSP falls behind").arg(waitMs));
                lastWarning=now;
            }
        }
    }
    if (!enabled_.load() || stopping_.load()) return;

    // With backpressure the capture thread is held at the high-water mark, so this cannot be reached.
    // Without it -- a harness that has not enabled it, or an operator who has turned it off -- the
    // queue is the only thing standing between a DSP that has fallen behind and unbounded growth.
    // A nine-minute run in exactly that state reached 4.9 GB, at about 9 MB a second, and stopped
    // producing files. Samples that cannot be processed in time are worth less than the memory they
    // are held in: the newest ones describe the present and the oldest describe a past that has
    // already been missed.
    while (ingressQueuedSamples_ + samples->size() > MAX_INGRESS_SAMPLES && !ingressQueue_.empty()) {
        const std::size_t dropped = ingressQueue_.front() ? ingressQueue_.front()->size() : 0;
        ingressQueue_.pop_front();
        ingressDroppedSamples_.fetch_add(dropped, std::memory_order_relaxed);
        ingressQueuedSamples_ -= std::min<std::size_t>(ingressQueuedSamples_, dropped);
        if (!ingressDropReported_) {
            ingressDropReported_ = true;
            nadirstation::log::write(QStringLiteral(
                "decoder ingress over %1 samples with backpressure off; the oldest blocks are being "
                "dropped rather than held").arg(static_cast<qulonglong>(MAX_INGRESS_SAMPLES)));
            nadirstation::log::flush();
        }
    }
    ingressQueue_.push_back(samples);
    ingressQueuedSamples_ += samples->size();
    if (hritDspRunning_) dspCv_.notify_one();
}


void NadirStationDecoder::drainQueuedSamples() {
    drainQueuedSamplesLimited(std::numeric_limits<std::size_t>::max());
}

void NadirStationDecoder::drainQueuedSamplesLimited(std::size_t maxBlocks) {
    // Drain all currently available capture blocks. The capture thread can keep
    // appending while the DSP worker is busy; it never touches dspAccumulator_.
    nadirstation::log::beat(1);
    for (std::size_t blockIndex=0;blockIndex<maxBlocks;++blockIndex) {
        nadirstation::log::beat(1);
        std::shared_ptr<std::vector<std::complex<float>>> block;
        {
            std::lock_guard<std::mutex> lock(ingressMutex_);
            if (ingressQueue_.empty()) break;
            block = std::move(ingressQueue_.front());
            ingressQueue_.pop_front();
            ingressQueuedSamples_ -= std::min<std::size_t>(ingressQueuedSamples_, block ? block->size() : 0);
        }
        ingressCv_.notify_one();
        if (block && !block->empty()) {
            // With the dedicated DSP thread running (HRIT, GVAR, GOES SD, ELEKTRO HRIT, polar), that
            // thread processes blocks under hritDspMutex_. A caller draining here at the same time
            // would run processSamplesCore concurrently with it, so take the same lock.
            std::unique_lock<std::mutex> dspGuard(hritDspMutex_, std::defer_lock);
            if (hritDspRunningNow()) dspGuard.lock();
            processSamplesCore(block);
        }
    }
}

void NadirStationDecoder::processSamplesCore(const std::shared_ptr<std::vector<std::complex<float>>>& samples) {
    if (!samples || samples->empty()) return;
    // Decoder page: signal time, DSP time and a snapshot of every stage, whichever branch returns below.
    const auto tDiag = std::chrono::steady_clock::now();
    const auto diagGuard = qScopeGuard([this, n = samples->size(), tDiag] {
        pipelineTick(n, std::chrono::duration<double>(std::chrono::steady_clock::now() - tDiag).count());
    });
    static quint64 blockCounter = 0;
    const quint64 ingressId = ++blockCounter;

    // Advance signal time before the GK-2A carrier-search early return below. Reacquisition runs
    // through that branch; advancing the clock only after a winner existed froze time at the loss
    // point, so the reopened search could never age or publish fresh link state.
    if (isGk2a() && sampleRate_ > 0.0) {
        gk2aStreamMsExact_ += static_cast<double>(samples->size()) * 1000.0 / sampleRate_;
        gk2aStreamMs_ = static_cast<qint64>(std::llround(gk2aStreamMsExact_));
    }

    if(isGoesSd()) {
        try { processGoesSdSamples(*samples); }
        catch(const std::exception& e){ emit statusMessage(QStringLiteral("GOES SD error: %1").arg(QString::fromUtf8(e.what()))); }
        return;
    }
    if(isGvar()) {
        try { processGvarSamples(*samples); }
        catch(const std::exception& e){ emit statusMessage(QStringLiteral("GVAR error: %1").arg(QString::fromUtf8(e.what()))); }
        return;
    }
    if(isPolar()) {
        try { processPolarSamples(*samples); }
        catch(const std::exception& e){ emit statusMessage(QStringLiteral("HRPT error: %1").arg(QString::fromUtf8(e.what()))); }
        return;
    }
    if(isElektro()) {
        try { processElektroSamples(*samples); }
        catch(const std::exception& e){ emit statusMessage(QStringLiteral("ELEKTRO-L error: %1").arg(QString::fromUtf8(e.what()))); }
        return;
    }
    if(isHrit()) {
        try { processHritSamples(*samples); }
        catch(const std::exception& e){ emit statusMessage(QStringLiteral("HRIT error: %1").arg(QString::fromUtf8(e.what()))); }
        return;
    }
    if (isGgak()) {
        try {
            processGgakSamples(*samples);
        } catch (const std::exception& e) {
            nadirstation::log::write(QStringLiteral("GGAK exception: %1").arg(QString::fromUtf8(e.what())));
        }
        return;
    }

    const double minimumPskRate = symbolRate_ * (1.0 + static_cast<double>(rrcAlpha_));
    if (sampleRate_ < minimumPskRate) {
        if (!lowRateReported_) {
            lowRateReported_ = true;
            const QString msg = QStringLiteral("Sample rate %1 kS/s is below the %2 kS/s the RRC signal bandwidth needs; cannot demodulate. Use a recording with a higher sample rate.")
                .arg(sampleRate_ / 1e3, 0, 'f', 1).arg(minimumPskRate / 1e3, 0, 'f', 1);
            nadirstation::log::write(msg);
            emit statusMessage(msg);
        }
        return;
    }

    try {
        // IMPORTANT: this is a streaming demodulator. Its carrier measurement, AGC, matched
        // filter, carrier and timing loops live in pskDemod_ and keep their state across every
        // IQ block. We do NOT restart acquisition at a block boundary.
        const auto tDemod = std::chrono::steady_clock::now();
        const auto demod = pskDemod_.process(*samples);
        addStageTime(0, tDemod);
        carrierOffsetHz_ = demod.carrierOffsetHz;
        if (!demod.symbols.empty()) fy2TimingQuality_ = demod.timingQuality;
        if (!demod.symbols.empty()) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            if (lastSnrMetricEmitMs_ == 0 || nowMs - lastSnrMetricEmitMs_ >= 100) {
                // From the constellation's covariance, not from its fourth moment.
                //
                // The M2M4 estimator that used to be here assumes independent samples. These come
                // out of a matched filter, which correlates the noise: correlation leaves the
                // second moment alone and raises the fourth, and M2M4 reads that rise as more
                // noise. On a test recording it reported 13.25 dB where the noise floor puts
                // the answer at 17.73 -- the noise overestimated by 2.7 times, the signal
                // underestimated at the same time, so the error nearly doubles instead of partly
                // cancelling.
                //
                // A BPSK constellation occupies one real axis whatever the carrier phase, and the
                // noise is circular, so the covariance is S*u*u^T + sigma^2*I and its eigenvalues
                // are S + sigma^2 and sigma^2. Both quantities are then second moments, which the
                // filter does not bias the way it biases a fourth, and neither needs the signal
                // axis to be known -- so this also survives a carrier phase that is still settling.
                // An average over a few dozen symbols is dominated by which few they were, and a
                // rank-one signal can look smeared in one. Below this the estimate would say more
                // about the block length than about the signal, so nothing is reported and the
                // interface keeps its previous value.
                constexpr std::size_t kMinSymbolsForSnr = 256;
                if (demod.symbols.size() >= kMinSymbolsForSnr) {
                double c11 = 0.0, c22 = 0.0, c12 = 0.0;
                for (const auto& symbol : demod.symbols) {
                    c11 += static_cast<double>(symbol.real()) * symbol.real();
                    c22 += static_cast<double>(symbol.imag()) * symbol.imag();
                    c12 += static_cast<double>(symbol.real()) * symbol.imag();
                }
                const double n = static_cast<double>(demod.symbols.size());
                c11 /= n;
                c22 /= n;
                c12 /= n;
                const double half = 0.5 * (c11 + c22);
                const double spread = std::sqrt(0.25 * (c11 - c22) * (c11 - c22) + c12 * c12);
                const double lambdaMin = std::max(0.0, half - spread);
                const double lambdaMax = half + spread;
                const double signal = std::max(0.0, lambdaMax - lambdaMin);
                const double noise = 2.0 * lambdaMin;
                const float snr = signal > 0.0 && noise > 0.0
                    ? static_cast<float>(std::max(0.0, 10.0 * std::log10(signal / noise))) : 0.0f;
                peakDemodSnr_ = std::max(peakDemodSnr_, snr);
                demodSnrHistory_.push_back({nowMs, snr});
                while (!demodSnrHistory_.empty() && nowMs - demodSnrHistory_.front().first > 1000)
                    demodSnrHistory_.pop_front();
                double sum = 0.0;
                for (const auto& item : demodSnrHistory_) sum += item.second;
                const float average = demodSnrHistory_.empty() ? snr
                    : static_cast<float>(sum / demodSnrHistory_.size());
                emit demodSnrMetricsReady(snr, peakDemodSnr_, average);
                }
                lastSnrMetricEmitMs_ = nowMs;
            }
        }
        softQuality_ = demod.soft.empty()
            ? 0.0f
            : std::accumulate(demod.soft.begin(), demod.soft.end(), 0.0f)
                / static_cast<float>(demod.soft.size());

        publishConstellation(demod.symbols);

        // Which service this is decides everything downstream. GK-2A LRIT and FY-2 S-VISSR are
        // not two settings of one chain; they are different codes, and feeding one to the other's
        // deframer is why GK-2A never reported a lock.
        if (isGk2a()) {
            processGk2aSymbols(demod.symbols);
            const qint64 nowGk = QDateTime::currentMSecsSinceEpoch();
            if (lastStatsEmitMs_ == 0 || nowGk - lastStatsEmitMs_ >= 100) {
                const auto s0 = gk2aLrit_.stats();
emit decodeStats(lock_, static_cast<int>(s0.cadusWithSync),
                                 static_cast<int>(s0.cadusAssembled - s0.cadusWithGoodRs),
                                 softQuality_, carrierOffsetHz_);
                lastStatsEmitMs_ = nowGk;
            }
            return;
        }

        // The frame locker is continuous across blocks; feed it
        // the newly recovered differential bits without any window-boundary reset.
        std::vector<uint8_t> bits;
        std::vector<uint8_t> bitReliability;
        std::vector<float> softSymbols;
        bits.reserve(demod.symbols.size());
        bitReliability.reserve(demod.symbols.size());
        softSymbols.reserve(demod.symbols.size());
        for (std::size_t i = 0; i < demod.symbols.size(); ++i) {
            const auto& sym = demod.symbols[i];
            const uint8_t hard = sym.real() < 0.0f ? 1u : 0u;
            const float soft = i < demod.soft.size() ? demod.soft[i] : 1.0f;
            const uint8_t currentReliability = static_cast<uint8_t>(std::clamp(
                static_cast<int>(std::lround(soft * 255.0f)), 0, 255));
            softSymbols.push_back(sym.real());
            bits.push_back(static_cast<uint8_t>(hard ^ nrzmLastBit_));
            bitReliability.push_back(std::min(currentReliability, nrzmLastReliability_));
            nrzmLastBit_ = hard;
            nrzmLastReliability_ = currentReliability;
        }
        if (!bits.empty()) processBitsWithDeframer(bits, bitReliability, frameLocker_[0], &softSymbols);

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (lastDspLogMs_ == 0 || nowMs - lastDspLogMs_ >= 1000) {
            nadirstation::log::write(QStringLiteral(
                "FY2-PSK block=%1 n=%2 symbols=%3 bits=%4 CFO=%5 SNRQ=%6 TIMQ=%7")
                .arg(ingressId)
                .arg(static_cast<qulonglong>(samples->size()))
                .arg(static_cast<qulonglong>(demod.symbols.size()))
                .arg(static_cast<qulonglong>(bits.size()))
                .arg(carrierOffsetHz_, 0, 'f', 1)
                .arg(softQuality_, 0, 'f', 3)
                .arg(demod.timingQuality, 0, 'f', 3));
            lastDspLogMs_ = nowMs;
        }

        // Decoder statistics are GUI telemetry, not data-plane output. Cap
        // cross-thread notifications at 10 Hz instead of queueing one event per
        // IQ block (which can exceed 100 Hz on higher-rate Pluto streams).
        if (lastStatsEmitMs_ == 0 || nowMs - lastStatsEmitMs_ >= 100) {
            emit decodeStats(lock_, goodFrames_, badFrames_, softQuality_, carrierOffsetHz_);
            lastStatsEmitMs_ = nowMs;
        }
    } catch (const std::exception& e) {
        nadirstation::log::write(QStringLiteral("decoder exception: %1").arg(QString::fromUtf8(e.what())));
        emit statusMessage(QStringLiteral("FY-2G DSP exception: %1").arg(QString::fromUtf8(e.what())));
        emit decodeStats(lock_, goodFrames_, badFrames_, softQuality_, carrierOffsetHz_);
    } catch (...) {
        nadirstation::log::write(QStringLiteral("decoder unknown exception"));
        emit statusMessage(QStringLiteral("FY-2G DSP: unknown exception."));
        emit decodeStats(lock_, goodFrames_, badFrames_, softQuality_, carrierOffsetHz_);
    }
}

namespace {
// Tuning of the frame locker. These are the values the decoder has always run with.
constexpr int kScoreCeiling = 5000;         // the score never climbs above this
constexpr int kScoreMissPenalty = 2;        // lost for every received bit the generator did not predict
constexpr int kScoreTrusted = 1000;         // the marker is only believed above this score
constexpr int kEarlyMarkerMinBits = 10000;  // a marker closer to the frame start than this is not the next frame's
}  // namespace

namespace {
// ---- Acquisition from the symbols ------------------------------------------------------------------
//
// The preamble is the generator's own sequence (period 32767). After the receiver's differential
// decoding it is what the locker sees; before it, the symbols are the running XOR of that sequence,
// which is again periodic with the same period. Correlating the last kProbeSymbols symbols against
// that periodic sequence finds the generator's phase from the single symbol error rate, where the
// generator-follower has to see 15 clean differential bits in a row. The correlation is only a way
// of starting the generator; the marker is still recognised in the generator's register, exactly as
// before.
constexpr int kPnPeriod = 32767;
constexpr int kProbeSymbols = 512;       // symbols correlated at a time
constexpr int kProbeWords = kProbeSymbols / 64;
constexpr int kProbeStride = 512;        // one search per this many symbols, while not locked
constexpr int kProbePrefilter = 24;      // 3 sigma over the first four words: worth finishing
constexpr int kProbeDeviation = 80;      // 7 sigma over 512 symbols: a false peak in 6e9 searches
// The marker is believed from the correlation alone only after the preamble has been tracked for this
// long. The scrambler also appears inside a frame wherever the data is zero (the 2048-bit fill fields
// above all), and a generator started there reaches the marker's phase 32767 symbols after it;
// those stretches are far shorter than the 10000-symbol preamble.
constexpr int kAssistMinRun = 6000;
constexpr int kAssistBitsMax = 40000;    // far longer than a preamble; only a backstop, the phase check ends it first
constexpr int kAssistCheckEvery = 512;
constexpr int kAssistWeakDeviation = 40; // below this the correlation has gone

struct PnTables {
    static constexpr int kShiftWords = (kPnPeriod + kProbeSymbols + 64) / 64 + 2;
    std::array<uint8_t, kPnPeriod> step{};    // the generator's output bit
    std::array<uint8_t, kPnPeriod> level{};   // its running XOR: what the symbols are in the preamble
    std::vector<uint64_t> shifted;            // level packed 64 at a time, at each of the 64 bit offsets

    PnTables() {
        uint32_t reg = 1;
        for (int j = 0; j < kPnPeriod; ++j) {
            const uint32_t bit = ((reg >> 14) ^ (reg >> 13)) & 1u;
            reg = ((reg << 1) | bit) & 0x7FFFu;
            step[static_cast<std::size_t>(j)] = static_cast<uint8_t>(bit);
        }
        uint8_t run = 0;
        for (int j = 0; j < kPnPeriod; ++j) {
            run ^= step[static_cast<std::size_t>(j)];
            level[static_cast<std::size_t>(j)] = run;
        }
        const std::size_t span = static_cast<std::size_t>(kShiftWords) * 64 + 128;
        std::vector<uint8_t> extended(span);
        for (std::size_t j = 0; j < span; ++j) extended[j] = level[j % kPnPeriod];
        shifted.assign(static_cast<std::size_t>(64) * kShiftWords, 0);
        for (int s = 0; s < 64; ++s)
            for (int w = 0; w < kShiftWords; ++w) {
                uint64_t v = 0;
                for (int b = 0; b < 64; ++b)
                    v |= static_cast<uint64_t>(extended[static_cast<std::size_t>(64 * w + s + b)]) << b;
                shifted[static_cast<std::size_t>(s) * kShiftWords + static_cast<std::size_t>(w)] = v;
            }
    }
};

const PnTables& pnTables() {
    static const PnTables tables;
    return tables;
}

struct ProbeResult { int offset = -1; int deviation = 0; };

// Offset into the periodic sequence whose next kProbeSymbols values differ most (or least) from
// the window. Only the size of the deviation matters: a carrier phase of 180 degrees inverts the
// symbols, and the sign of the deviation says which.
template <typename Pop>
inline ProbeResult probeBest(const std::array<uint64_t, kProbeWords>& window, const PnTables& tables, Pop pop) {
    ProbeResult best;
    for (int k = 0; k < kPnPeriod; ++k) {
        const uint64_t* row = &tables.shifted[static_cast<std::size_t>(k & 63) * PnTables::kShiftWords
                                              + static_cast<std::size_t>(k >> 6)];
        int differ = pop(window[0] ^ row[0]) + pop(window[1] ^ row[1])
                   + pop(window[2] ^ row[2]) + pop(window[3] ^ row[3]);
        if (std::abs(differ - 128) < kProbePrefilter) continue;
        for (int w = 4; w < kProbeWords; ++w) differ += pop(window[static_cast<std::size_t>(w)] ^ row[w]);
        const int deviation = std::abs(differ - kProbeSymbols / 2);
        if (deviation > best.deviation) { best.deviation = deviation; best.offset = k; }
    }
    return best;
}

inline int popSoftware(uint64_t x) {
    x = x - ((x >> 1) & 0x5555555555555555ULL);
    x = (x & 0x3333333333333333ULL) + ((x >> 2) & 0x3333333333333333ULL);
    x = (x + (x >> 4)) & 0x0F0F0F0F0F0F0F0FULL;
    return static_cast<int>((x * 0x0101010101010101ULL) >> 56);
}

#if defined(__GNUC__) && (defined(__x86_64__) || defined(__i386__))
__attribute__((target("popcnt"))) ProbeResult probeBestHardware(
        const std::array<uint64_t, kProbeWords>& window, const PnTables& tables) {
    return probeBest(window, tables, [](uint64_t x) { return static_cast<int>(__builtin_popcountll(x)); });
}
bool hardwarePopcount() {
    static const bool has = __builtin_cpu_supports("popcnt") != 0;
    return has;
}
#else
ProbeResult probeBestHardware(const std::array<uint64_t, kProbeWords>& window, const PnTables& tables) {
    return probeBest(window, tables, popSoftware);
}
bool hardwarePopcount() { return false; }
#endif

ProbeResult probeSymbols(const std::array<uint64_t, kProbeWords>& window) {
    const PnTables& tables = pnTables();
    if (hardwarePopcount()) return probeBestHardware(window, tables);
    return probeBest(window, tables, popSoftware);
}
}  // namespace

void NadirStationDecoder::SvissrFrameLocker::reset() {
    received_ = 0;
    generator_ = 0;
    score_ = 1;
    collecting_ = false;
    dropFrame();
    symbolsSeen_ = 0;
    frameStart_ = 0;
    symbolRing_.fill(0);
    symbolCount_ = 0;
    symbolState_ = 0;
    sinceSearch_ = 0;
    assisted_ = false;
    assistedBitsLeft_ = 0;
    assistedRun_ = 0;
    assistedWeakChecks_ = 0;
    assistedSinceCheck_ = 0;
    assistedIndex_ = 0;
}

// Advances the local generator by one bit and updates the score.
void NadirStationDecoder::SvissrFrameLocker::followGenerator(uint8_t bit) {
    received_ = (received_ << 1) | bit;
    const uint64_t predicted = ((generator_ >> 14) ^ (generator_ >> 13)) & 1u;
    generator_ = (generator_ << 1) | predicted;
    if (bit == predicted) {
        if (score_ < kScoreCeiling) ++score_;
        return;
    }
    score_ -= kScoreMissPenalty;
    if (score_ <= 0) {
        score_ = 1;
        // A generator whose phase came from a correlation is not reloaded from the last received
        // bits when it misses: those are the noisy ones.
        if (!assisted_) generator_ = received_;
    }
}

// Keeps the symbol history and, while the generator is not trusted, looks for the preamble in it.
void NadirStationDecoder::SvissrFrameLocker::trackSymbol(uint8_t bit) {
    symbolState_ = static_cast<uint8_t>(symbolState_ ^ bit);
    const std::size_t pos = static_cast<std::size_t>(symbolCount_ & (kProbeSymbols - 1));
    const uint64_t mask = uint64_t{1} << (pos & 63);
    if (symbolState_) symbolRing_[pos >> 6] |= mask; else symbolRing_[pos >> 6] &= ~mask;
    ++symbolCount_;

    if (assisted_) {
        assistedIndex_ = (assistedIndex_ + 1) % kPnPeriod;
        if (++assistedRun_ >= kAssistMinRun && score_ <= kScoreTrusted) score_ = kScoreTrusted + 1;
        if (--assistedBitsLeft_ <= 0 || !assistedPhaseHolds()) {
            assisted_ = false;
            score_ = 1;
            generator_ = received_;
        }
        return;
    }
    if (collecting_ || score_ > kScoreTrusted || symbolCount_ < kProbeSymbols) return;
    if (++sinceSearch_ < kProbeStride) return;
    sinceSearch_ = 0;
    acquireFromSymbols();
}

bool NadirStationDecoder::SvissrFrameLocker::acquireFromSymbols() {
    std::array<uint64_t, kProbeWords> window{};
    for (int i = 0; i < kProbeSymbols; ++i) {
        const std::size_t pos = static_cast<std::size_t>((symbolCount_ - kProbeSymbols + static_cast<uint64_t>(i))
                                                         & (kProbeSymbols - 1));
        const uint64_t bit = (symbolRing_[pos >> 6] >> (pos & 63)) & 1u;
        window[static_cast<std::size_t>(i >> 6)] |= bit << (i & 63);
    }
    const ProbeResult found = probeSymbols(window);
    if (found.offset < 0 || found.deviation < kProbeDeviation) return false;

    // The newest symbol sits kProbeSymbols - 1 past the start of the matched stretch. The
    // generator's register holds the last 64 output bits, newest in bit 0.
    const PnTables& tables = pnTables();
    const int newest = (found.offset + kProbeSymbols - 1) % kPnPeriod;
    uint64_t reg = 0;
    for (int m = 0; m < 64; ++m) {
        const int j = ((newest - m) % kPnPeriod + kPnPeriod) % kPnPeriod;
        reg |= static_cast<uint64_t>(tables.step[static_cast<std::size_t>(j)]) << m;
    }
    const bool alreadyInPhase = std::popcount(generator_ ^ reg) <= 4;
    generator_ = reg;
    if (!alreadyInPhase) score_ = 1;   // otherwise the follower's own score stands
    assisted_ = true;
    assistedRun_ = kProbeSymbols;
    assistedBitsLeft_ = kAssistBitsMax;
    assistedWeakChecks_ = 0;
    assistedSinceCheck_ = 0;
    assistedIndex_ = static_cast<uint32_t>(newest);
    return true;
}

// While assisted, the last kProbeSymbols symbols are compared with the sequence at the expected
// phase every kAssistCheckEvery symbols; two weak checks in a row end the assistance.
bool NadirStationDecoder::SvissrFrameLocker::assistedPhaseHolds() {
    if (++assistedSinceCheck_ < kAssistCheckEvery) return true;
    assistedSinceCheck_ = 0;
    const PnTables& tables = pnTables();
    int differ = 0;
    const int start = static_cast<int>(((static_cast<int64_t>(assistedIndex_) - (kProbeSymbols - 1)) % kPnPeriod + kPnPeriod) % kPnPeriod);
    for (int i = 0; i < kProbeSymbols; ++i) {
        const std::size_t pos = static_cast<std::size_t>((symbolCount_ - kProbeSymbols + static_cast<uint64_t>(i))
                                                         & (kProbeSymbols - 1));
        const uint8_t sym = static_cast<uint8_t>((symbolRing_[pos >> 6] >> (pos & 63)) & 1u);
        differ += sym ^ tables.level[static_cast<std::size_t>((start + i) % kPnPeriod)];
    }
    if (std::abs(differ - kProbeSymbols / 2) >= kAssistWeakDeviation) {
        assistedWeakChecks_ = 0;
        return true;
    }
    return ++assistedWeakChecks_ < 2;
}

bool NadirStationDecoder::SvissrFrameLocker::markerInGenerator() const {
    return std::popcount(generator_ ^ SYNC_MARKER) <= MAX_SYNC_MARKER_BIT_ERRORS && score_ > kScoreTrusted;
}

void NadirStationDecoder::SvissrFrameLocker::takeBit(uint8_t bit, uint8_t trust, const float* soft) {
    if (soft) frameSoft_.push_back(*soft);
    partialByte_ = static_cast<uint8_t>((partialByte_ << 1) | (bit & 1u));
    frameTrust_.push_back(trust);
    if (++bitsInByte_ == 8) {
        frameBytes_.append(static_cast<char>(partialByte_));
        bitsInByte_ = 0;
    }
    ++bitsTaken_;
}

bool NadirStationDecoder::SvissrFrameLocker::frameIsWhole() const {
    return bitsInByte_ == 0 && frameBytes_.size() == static_cast<int>(SVISSR_DATA_BITS / 8);
}

void NadirStationDecoder::SvissrFrameLocker::dropFrame() {
    frameBytes_.clear();
    frameTrust_.clear();
    frameSoft_.clear();
    bitsTaken_ = 0;
    bitsInByte_ = 0;
    partialByte_ = 0;
}

void NadirStationDecoder::SvissrFrameLocker::beginFrame(uint64_t position) {
    assisted_ = false;
    dropFrame();
    frameTrust_.reserve(SVISSR_DATA_BITS);
    frameSoft_.reserve(SVISSR_DATA_BITS);
    frameStart_ = position;
    collecting_ = true;
    score_ = 0;
}

NadirStationDecoder::DeframedFrame NadirStationDecoder::SvissrFrameLocker::releaseFrame() {
    DeframedFrame frame{std::move(frameBytes_), std::move(frameTrust_), frameStart_, std::move(frameSoft_)};
    dropFrame();
    return frame;
}

std::vector<NadirStationDecoder::DeframedFrame> NadirStationDecoder::SvissrFrameLocker::consume(
        const std::vector<uint8_t>& bits, const std::vector<uint8_t>& reliability,
        const std::vector<float>* soft) {
    std::vector<DeframedFrame> frames;
    const float zeroSoft = 0.0f;
    if (soft && soft->size() != bits.size()) soft = nullptr;
    constexpr int kFrameBits = static_cast<int>(SVISSR_DATA_BITS);
    for (std::size_t i = 0; i < bits.size(); ++i) {
        const uint8_t bit = bits[i] & 1u;
        const uint8_t trust = i < reliability.size() ? reliability[i] : 255;
        followGenerator(bit);
        trackSymbol(bit);
        const uint64_t here = symbolsSeen_ + i;

        if (!collecting_) {
            if (markerInGenerator()) beginFrame(here);
            continue;
        }

        // A short fade or a clock slip can corrupt a few bits without changing the nominal frame
        // length. If the next marker shows up after the DOC region, close this row early with the
        // missing bits marked untrusted and start on the new frame at once.
        if (bitsTaken_ > kEarlyMarkerMinBits && markerInGenerator()) {
            while (bitsTaken_ < kFrameBits) takeBit(0, 0, soft ? &zeroSoft : nullptr);
            if (frameIsWhole()) frames.push_back(releaseFrame());
            beginFrame(here);   // the matching bit belongs to the marker
            continue;
        }

        takeBit(bit, trust, soft ? &(*soft)[i] : nullptr);
        if (bitsTaken_ == kFrameBits) {
            collecting_ = false;
            if (frameIsWhole()) frames.push_back(releaseFrame());
            else dropFrame();
        }
    }
    symbolsSeen_ += bits.size();
    return frames;
}

uint8_t NadirStationDecoder::getBit(const QByteArray& data, std::size_t bitPos) {
    const std::size_t bytePos = bitPos / 8;
    const int shift = 7 - static_cast<int>(bitPos % 8);
    return static_cast<uint8_t>((static_cast<unsigned char>(data[static_cast<qsizetype>(bytePos)]) >> shift) & 1u);
}

uint32_t NadirStationDecoder::readBits(const QByteArray& data, std::size_t bitPos, int bitCount) {
    uint32_t v = 0;
    for (int i = 0; i < bitCount; ++i) v = (v << 1) | getBit(data, bitPos + static_cast<std::size_t>(i));
    return v;
}

uint16_t NadirStationDecoder::crc16_1021(const std::vector<uint8_t>& bits) {
    uint16_t crc = 0;
    for (const uint8_t bit : bits) {
        const bool msb = (crc & 0x8000u) != 0;
        crc = static_cast<uint16_t>(crc << 1);
        if (msb ^ (bit != 0)) crc ^= 0x1021u;
    }
    return crc;
}

QVector<quint16> NadirStationDecoder::unpackSamples(const std::vector<uint8_t>& bits, int count, int bitsPerSample) {
    QVector<quint16> out;
    out.resize(count);
    std::size_t p = 0;
    const uint32_t mask = (1u << bitsPerSample) - 1u;
    for (int i = 0; i < count; ++i) {
        uint32_t v = 0;
        for (int b = 0; b < bitsPerSample && p < bits.size(); ++b, ++p) v = (v << 1) | bits[p];
        out[i] = static_cast<quint16>(v & mask);
    }
    return out;
}

void NadirStationDecoder::appendBitsToByteArray(const std::vector<uint8_t>& bits, QByteArray& out) {
    uint8_t cur = 0;
    int n = 0;
    for (const auto b : bits) {
        cur = static_cast<uint8_t>((cur << 1) | (b & 1u));
        if (++n == 8) {
            out.append(static_cast<char>(cur));
            cur = 0;
            n = 0;
        }
    }
}

QDateTime NadirStationDecoder::parseDocTime(const std::vector<uint8_t>& doc) {
    // DOC payload begins with the 126-byte S/C + CDAS status block. Per the
    // S-VISSR2.0 Table 8, year is bytes 18-19 (1-based), followed by month,
    // day, hour, minute and second in bytes 20-24.
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
    if (readBcd2(doc, 17, year) && readBcdByte(doc, 19, month) &&
        readBcdByte(doc, 20, day) && readBcdByte(doc, 21, hour) &&
        readBcdByte(doc, 22, minute) && readBcdByte(doc, 23, second)) {
        if (year < 100) year += 2000;
        if (year >= 1970 && year <= 2199 && month >= 1 && month <= 12 &&
            day >= 1 && day <= 31 && hour < 24 && minute < 60 && second < 60) {
            const QDate date(year, month, day);
            const QTime time(hour, minute, second);
            if (date.isValid() && time.isValid())
                return QDateTime(date, time, QTimeZone::UTC);
        }
    }
    return {};
}

int NadirStationDecoder::parseReportedLineNumber(const std::vector<uint8_t>& doc) {
    // DOC Table 8 uses BCD bytes 5-6 for Picture-Flag Set Line Number and
    // bytes 9-10 for Scan Count (1-based byte numbers within the DOC payload).
    // Image position is Scan Count - Set Line Number; expose it as 1-based.
    int setLine = 0;
    int scanCount = 0;
    if (readBcd2(doc, 4, setLine) && readBcd2(doc, 8, scanCount)) {
        const int line = scanCount - setLine + 1;
        if (line >= 1 && line <= SVISSR_SCAN_LINES) return line;
        // During scans with the picture flag already active, a zero set-line
        // marker can occur in legacy DOC records; the raw scan count is useful.
        if (setLine == 0 && scanCount >= 1 && scanCount <= SVISSR_SCAN_LINES) return scanCount;
    }
    return -1;
}

namespace {
// The S-VISSR downlink is scrambled with a 15-bit shift-register sequence (x^15 + x^14 + 1, seeded
// with the documented start value). The first SVISSR_SYNC_BITS bits belong to the synchronisation
// field; the mask for the data part of a frame is what follows, packed most significant bit first.
const std::array<uint8_t, SVISSR_DATA_BITS / 8>& svissrScrambleMask() {
    static const std::array<uint8_t, SVISSR_DATA_BITS / 8> mask = [] {
        std::array<uint8_t, SVISSR_DATA_BITS / 8> m{};
        uint32_t reg = 0x339Fu;   // 0b011001110011111
        const auto step = [&reg]() {
            const uint32_t feedback = ((reg >> 14) ^ (reg >> 13)) & 1u;
            reg = ((reg << 1) | feedback) & 0x7FFFu;
            return feedback;
        };
        for (std::size_t i = 0; i < SVISSR_SYNC_BITS; ++i) step();
        for (auto& byte : m) {
            uint32_t v = 0;
            for (int k = 0; k < 8; ++k) v = (v << 1) | step();
            byte = static_cast<uint8_t>(v);
        }
        return m;
    }();
    return mask;
}

// Cumulative parity of the descrambling mask: the data bits of a frame are the first difference of
// (symbol xor this table), so (symbol xor table) is the running parity the soft decoder works on.
const std::vector<uint8_t>& svissrRunningMask() {
    static const std::vector<uint8_t> table = [] {
        const auto& mask = svissrScrambleMask();
        std::vector<uint8_t> t(SVISSR_DATA_BITS);
        uint8_t acc = 0;
        for (std::size_t k = 0; k < SVISSR_DATA_BITS; ++k) {
            const uint8_t byte = static_cast<uint8_t>(mask[k / 8] ^ (((k / 8) & 1) ? 0xFFu : 0x00u));
            acc ^= static_cast<uint8_t>((byte >> (7 - (k % 8))) & 1u);
            t[k] = acc;
        }
        return t;
    }();
    return table;
}
}  // namespace

QByteArray NadirStationDecoder::derandomize(const QByteArray& frame) const {
    // XOR with the scrambling mask; every odd-numbered byte is also complemented.
    const auto& mask = svissrScrambleMask();
    QByteArray result = frame;
    const int n = std::min(static_cast<int>(result.size()), static_cast<int>(mask.size()));
    for (int i = 0; i < n; ++i) {
        const uint8_t flip = (i & 1) ? 0xFFu : 0x00u;
        result[i] = static_cast<char>(static_cast<uint8_t>(result[i]) ^ mask[static_cast<std::size_t>(i)] ^ flip);
    }
    return result;
}

bool NadirStationDecoder::parseFrame(const QByteArray& raw, const std::vector<uint8_t>& reliability,
                            uint64_t syncSymbolPosition, Line& line, QString* detail,
                            const std::vector<float>* soft) {
    if (raw.size() * 8 != static_cast<int>(SVISSR_DATA_BITS) ||
        reliability.size() != SVISSR_DATA_BITS) return false;
    std::size_t pos = 0;
    std::array<DecodedSector, 12> sectors{};
    int goodCrc = 0;
    int totalCrc = 0;
    int correctedTotal = 0;
    QStringList sectorReport;
    std::array<std::size_t, 12> sectorStart{};

    for (int si = 0; si < 12; ++si) {
        const auto& spec = SECTORS[static_cast<std::size_t>(si)];
        sectorStart[static_cast<std::size_t>(si)] = pos;
        sectors[static_cast<std::size_t>(si)].id = readBits(raw, pos, spec.idBits);
        const uint32_t receivedId = sectors[static_cast<std::size_t>(si)].id;
        const int idBitErrors = std::popcount(receivedId ^ spec.expectedId);
        sectors[static_cast<std::size_t>(si)].idOk = idBitErrors <= MAX_SECTOR_ID_BIT_ERRORS;
        pos += static_cast<std::size_t>(spec.idBits);
        auto& dst = sectors[static_cast<std::size_t>(si)].dataBits;
        dst.resize(static_cast<std::size_t>(spec.dataBits));
        for (int b = 0; b < spec.dataBits; ++b) dst[static_cast<std::size_t>(b)] = getBit(raw, pos + static_cast<std::size_t>(b));
        pos += static_cast<std::size_t>(spec.dataBits);

        sectors[static_cast<std::size_t>(si)].receivedCrc = static_cast<uint16_t>(readBits(raw, pos, 16));

        // The 2048-bit sector filler is defined as all zeroes. It is a second,
        // local quality check that remains useful when a few payload bit errors
        // make the long sector CRC fail. At 2048 bits, tolerate up to 160 bad
        // filler bits (7.8% BER); a larger count indicates a false frame/poor
        // demodulation and the sector is not safe to render.
        auto& sector = sectors[static_cast<std::size_t>(si)];
        const std::size_t fillerStart = pos + 16;
        for (std::size_t b = 0; b < 2048; ++b)
            sector.fillerErrors += getBit(raw, fillerStart + b);
        sector.fillerOk = sector.fillerErrors <= MAX_SECTOR_FILLER_ERRORS;

        // S-VISSR CRC is over the sector ID + data, not the data field alone.
        // IMPORTANT: pos currently points immediately after the data field and
        // before the 16-bit received CRC. The sector ID therefore starts at
        //   pos - dataBits - idBits
        // not at pos - dataBits - idBits - 16. The latter underflowed on the
        // first sector (DOC) and caused an access violation exactly when the
        // first valid frame was decoded.
        std::vector<uint8_t> crcSegment;
        const int dataPaddingBits = (8 - (spec.dataBits % 8)) % 8;
        crcSegment.reserve(static_cast<std::size_t>(spec.idBits + spec.dataBits + dataPaddingBits));
        const std::size_t sectorIdStart = pos - static_cast<std::size_t>(spec.dataBits) - static_cast<std::size_t>(spec.idBits);
        if (sectorIdStart + static_cast<std::size_t>(spec.idBits + spec.dataBits) > raw.size() * 8ULL) {
            if (detail) *detail = QStringLiteral("sector %1 CRC data range out of bounds").arg(spec.name);
            return false;
        }
        for (int b = 0; b < spec.idBits; ++b)
            crcSegment.push_back(static_cast<uint8_t>((spec.expectedId >> (spec.idBits - 1 - b)) & 1u));
        crcSegment.insert(crcSegment.end(), dst.begin(), dst.end());
        // The short IR low-bit planes and IR4 are stored in a byte-packed
        // sample array. Their final sample leaves six payload bits in the last
        // byte, so two zero pad bits participate in the transmitted CRC.
        crcSegment.insert(crcSegment.end(), static_cast<std::size_t>(dataPaddingBits), 0u);

        int corrected = 0;
        int dataCorrected = 0;
        const uint16_t rawCalculatedCrc = crc16Local(crcSegment);
        sector.crcOk = sector.idOk && sector.fillerOk &&
            correctSectorCrc(crcSegment, spec.idBits, spec.dataBits,
                             reliability, sectorIdStart + static_cast<std::size_t>(spec.idBits),
                             sectors[static_cast<std::size_t>(si)].receivedCrc, dataCorrected);
        corrected += dataCorrected;
        sectors[static_cast<std::size_t>(si)].calculatedCrc = crc16_1021(crcSegment);
        sectorReport.push_back(QStringLiteral("%1[id=%2/%3 crc=%4/%5 fill=%6]")
            .arg(QString::fromLatin1(spec.name))
            .arg(sector.idOk ? QStringLiteral("ok") : QStringLiteral("bad"))
            .arg(QString::number(receivedId, 16).rightJustified(spec.idBits / 4, QLatin1Char('0')))
            .arg(QString::number(rawCalculatedCrc, 16).rightJustified(4, QLatin1Char('0')))
            .arg(QString::number(sector.receivedCrc, 16).rightJustified(4, QLatin1Char('0')))
            .arg(sector.fillerErrors));
        if (corrected > 0) {
            dst.assign(crcSegment.begin() + spec.idBits,
                       crcSegment.begin() + spec.idBits + spec.dataBits);
            correctedTotal += corrected;
        }
        ++totalCrc;
        if (sectors[static_cast<std::size_t>(si)].crcOk) ++goodCrc;
        pos += 16 + 2048; // CRC + documented fill field.
    }

    if (pos != SVISSR_DATA_BITS) return false;

    // DOC telemetry is useful when its sector ID and filler agree. Row-number
    // authority is stricter below: the DOC CRC must also pass, otherwise the
    // row is inferred from PN-sync spacing instead of dropping image data.
    const bool docTrusted = sectors[0].idOk && sectors[0].fillerOk;
    QByteArray telemetryPacked;
    if (docTrusted) {
        appendBitsToByteArray(sectors[0].dataBits, telemetryPacked);
        emit telemetryUpdated(telemetryPacked.left(126));
        emit docPayloadUpdated(telemetryPacked, sectors[0].crcOk);
        // The DOC S/C status block carries an 8-bit satellite ID at word 90.
        // The following constants block carries the subsatellite longitude
        // (DOC bytes 147-150, signed 32-bit millidegrees). These are independent
        // identity indicators. Only CRC-accepted DOCs may advance confirmation.
        if (sectors[0].crcOk && telemetryPacked.size() >= 150) {
            const int id = static_cast<quint8>(telemetryPacked[89]);
            const quint32 longitudeWord =
                (quint32(static_cast<quint8>(telemetryPacked[146])) << 24) |
                (quint32(static_cast<quint8>(telemetryPacked[147])) << 16) |
                (quint32(static_cast<quint8>(telemetryPacked[148])) << 8) |
                quint32(static_cast<quint8>(telemetryPacked[149]));
            const auto signedLongitude = static_cast<int32_t>(longitudeWord);
            const double longitude = static_cast<double>(signedLongitude) / 1000.0;
            // S/C IDs are the FY-2 flight-unit numbers: 08=G and 09=H.
            const bool gMatch = id == 8 && std::abs(longitude - 99.2) <= 1.0;
            const bool hMatch = id == 9 && std::abs(longitude - 79.0) <= 1.0;
            if (gMatch || hMatch) {
                if (satelliteIdentityCandidateId_ == id &&
                    std::abs(satelliteIdentityCandidateLongitude_ - longitude) <= 0.05) {
                    ++satelliteIdentityConfirmations_;
                } else {
                    satelliteIdentityCandidateId_ = id;
                    satelliteIdentityCandidateLongitude_ = longitude;
                    satelliteIdentityConfirmations_ = 1;
                }
                emit satelliteIdentityUpdated(id, longitude, satelliteIdentityConfirmations_,
                                               satelliteIdentityConfirmations_ >= 5);
            } else {
                satelliteIdentityCandidateId_ = -1;
                satelliteIdentityConfirmations_ = 0;
                emit satelliteIdentityUpdated(id, longitude, 0, false);
            }
        }
    }

    // ---- soft-decision stage ------------------------------------------------------------------
    // Below about 6 dB the link makes several errors in every 16-bit sector ID, so the hard path
    // throws away frames that are well aligned, and it passes the damaged payload of every sector
    // on as it is. The 2048-bit known-zero filler of a sector says whether the sector is aligned
    // far more reliably than 16 ID bits can, and the symbols behind the bits let the payload be
    // estimated instead of copied. Both steps are skipped for clean frames.
    int softRecoveredSectors = 0;
    bool softEngaged = false;
    if (soft && soft->size() == SVISSR_DATA_BITS) {
        int fillerSum = 0;
        for (int si = 1; si < 12; ++si) fillerSum += sectors[static_cast<std::size_t>(si)].fillerErrors;
        softEngaged = fillerSum > SOFT_ENGAGE_MEAN_FILLER * 11;
    }
    if (softEngaged) {
        std::array<nadirstation::svissrsoft::SectorGeometry, 12> geometry{};
        std::array<nadirstation::svissrsoft::SectorIo, 12> io{};
        for (std::size_t si = 0; si < 12; ++si) {
            geometry[si] = {SECTORS[si].idBits, SECTORS[si].dataBits, SECTORS[si].expectedId};
            io[si].start = sectorStart[si];
            io[si].crcOk = sectors[si].crcOk;
            io[si].dataBits = &sectors[si].dataBits;
        }
        const auto tSoft = std::chrono::steady_clock::now();
        const auto outcome = nadirstation::svissrsoft::run(soft->data(), svissrRunningMask().data(),
                                                           geometry.data(), io);
        addStageTime(3, tSoft);
        softRecoveredSectors = outcome.recoveredSectors;
        if (outcome.accepted) {
            // Relax the ID check where the filler vouches for the sector. The DOC stays strict:
            // its CRC decides the row number, and a damaged counter is worse than none.
            for (std::size_t si = 1; si < 12; ++si) {
                sectors[si].strongFiller = io[si].strong;
                sectors[si].softRecovered = io[si].recovered;
                if (io[si].strong) { sectors[si].idOk = true; sectors[si].fillerOk = true; }
            }
        }
    }

    int trustedSectors = 0;
    for (const auto& sector : sectors)
        if (sector.idOk && sector.fillerOk) ++trustedSectors;

    // The frame includes the 2-byte DOC sector ID before this payload:
    // full-frame byte 193 is the group ID, and full-frame
    // byte 196 is the start of the subcommunication data.
    // Assemble products using that framing after DOC ID/filler checks.
    if (docTrusted && telemetryPacked.size() >= 832) {
        // The orbit/attitude block occupies 128 bytes in each of the 25
        // subcommutation groups. Only CRC-accepted DOC sectors contribute;
        // this prevents a damaged transmission from being presented as live
        // navigation telemetry.
        constexpr int ORBIT_ATTITUDE_OFFSET = 194 + 100;
        constexpr int ORBIT_ATTITUDE_CHUNK_BYTES = 128;
        const int orbitGroup = static_cast<quint8>(telemetryPacked[191]) & 0x1f;
        if (sectors[0].crcOk && orbitGroup < 25 &&
            telemetryPacked.size() >= ORBIT_ATTITUDE_OFFSET + ORBIT_ATTITUDE_CHUNK_BYTES) {
            if (!orbitAttitudeChunkSeen_[static_cast<std::size_t>(orbitGroup)]) {
                orbitAttitudeChunkSeen_[static_cast<std::size_t>(orbitGroup)] = true;
                orbitAttitudeChunks_[static_cast<std::size_t>(orbitGroup)] =
                    telemetryPacked.mid(ORBIT_ATTITUDE_OFFSET, ORBIT_ATTITUDE_CHUNK_BYTES);
                ++orbitAttitudeGroupsSeen_;
            }
            if (orbitAttitudeGroupsSeen_ == 25) {
                QByteArray block;
                block.reserve(3200);
                for (const auto& chunk : orbitAttitudeChunks_) block.append(chunk);
                emit orbitAttitudeUpdated(block);
                orbitAttitudeChunkSeen_.fill(false);
                orbitAttitudeGroupsSeen_ = 0;
            }
        }

        const int revision = (static_cast<quint8>(telemetryPacked[26]) << 8) | static_cast<quint8>(telemetryPacked[27]);
        if (revision != manamRevision_) {
            manamRevision_ = revision;
            manamGroupCount_ = 0;
            manamGroupMask_ = 0;
            lastCompleteManam_.clear();
            manamChunkSeen_.fill(false);
            manamChunkScores_.fill(-1);
            for (auto& chunk : manamChunks_) chunk.clear();
        }
        const int group = static_cast<quint8>(telemetryPacked[191]) & 0x1f;
        constexpr int MANAM_OFFSET = 194 + 228;
        constexpr int MANAM_BYTES = 410;
        if (group < 25 && telemetryPacked.size() >= MANAM_OFFSET + MANAM_BYTES) {
            const QByteArray chunk = telemetryPacked.mid(MANAM_OFFSET, MANAM_BYTES);
            int score = 0;
            for (int line = 0; line < 5; ++line) {
                const int base = line * 82;
                if (static_cast<quint8>(chunk[base + 80]) == '\r' && static_cast<quint8>(chunk[base + 81]) == '\n') score += 4;
                for (int c = 0; c < 80; ++c) {
                    const auto ch = static_cast<quint8>(chunk[base + c]);
                    if (ch == 0 || ch == '\t' || (ch >= 0x20 && ch <= 0x7e)) ++score;
                }
            }
            if (!manamChunkSeen_[static_cast<std::size_t>(group)]) {
                manamChunkSeen_[static_cast<std::size_t>(group)] = true;
                ++manamGroupCount_;
            }
            manamGroupMask_ |= (quint32(1) << group);
            if (score > manamChunkScores_[static_cast<std::size_t>(group)]) {
                manamChunkScores_[static_cast<std::size_t>(group)] = score;
                manamChunks_[static_cast<std::size_t>(group)] = chunk;
            }
            emit manamProgress(revision, static_cast<int>(manamGroupMask_));
            if (manamGroupCount_ == 25) {
                QByteArray complete; complete.reserve(10250);
                for (const auto& block : manamChunks_) complete.append(block);
                if (complete != lastCompleteManam_) {
                    lastCompleteManam_ = complete;
                    emit manamUpdated(revision, complete);
                }
            }
        }
    }

    // Calibration-2 is part of the subcommunication group beginning
    // at full-frame byte 196, or DOC payload byte 194 after its 2-byte sector
    // ID is removed above. Collect its 1024-byte region from up to eight
    // sequential transmissions per group; repeat_id is not used.
    constexpr int CALIBRATION_GROUP_OFFSET = 194 + 894;
    constexpr int CALIBRATION_GROUP_BYTES = 1024;
    if (docTrusted && telemetryPacked.size() >=
            CALIBRATION_GROUP_OFFSET + CALIBRATION_GROUP_BYTES) {
        const int group = static_cast<quint8>(telemetryPacked[191]) & 0x1f;
        if (group < 25 && calibrationCopyCounts_[static_cast<std::size_t>(group)] < 8) {
            bool groupWasPresent = false;
            for (const bool seen : calibrationCopySeen_[static_cast<std::size_t>(group)])
                groupWasPresent = groupWasPresent || seen;
            const int copyIndex = calibrationCopyCounts_[static_cast<std::size_t>(group)]++;
            calibrationCopySeen_[static_cast<std::size_t>(group)][static_cast<std::size_t>(copyIndex)] = true;
            calibrationCopies_[static_cast<std::size_t>(group)][static_cast<std::size_t>(copyIndex)] =
                telemetryPacked.mid(CALIBRATION_GROUP_OFFSET, CALIBRATION_GROUP_BYTES);
            if (!groupWasPresent) ++calibrationGroupsSeen_;

            if (calibrationGroupsSeen_ == 25) {
                calibrationBlock_.resize(25 * CALIBRATION_GROUP_BYTES);
                for (int g = 0; g < 25; ++g) {
                    const auto& seen = calibrationCopySeen_[static_cast<std::size_t>(g)];
                    const auto& copies = calibrationCopies_[static_cast<std::size_t>(g)];
                    int copyCount = 0;
                    int firstCopy = -1;
                    for (int r = 0; r < 8; ++r) if (seen[static_cast<std::size_t>(r)]) {
                        if (firstCopy < 0) firstCopy = r;
                        ++copyCount;
                    }
                    for (int b = 0; b < CALIBRATION_GROUP_BYTES; ++b) {
                        quint8 result = static_cast<quint8>(copies[static_cast<std::size_t>(firstCopy)][b]);
                        for (int bit = 0; bit < 8; ++bit) {
                            int ones = 0;
                            for (int r = 0; r < 8; ++r)
                                if (seen[static_cast<std::size_t>(r)] &&
                                    (static_cast<quint8>(copies[static_cast<std::size_t>(r)][b]) & (1u << bit))) ++ones;
                            if (ones * 2 > copyCount) result |= static_cast<quint8>(1u << bit);
                            else if (ones * 2 < copyCount) result &= static_cast<quint8>(~(1u << bit));
                        }
                        calibrationBlock_[g * CALIBRATION_GROUP_BYTES + b] = static_cast<char>(result);
                    }
                }
                std::array<QVector<float>, 5> decodedLuts;
                if (nadirstation::decodeCalibration2(calibrationBlock_, decodedLuts)) {
                    const bool newlyReady = !calibrationLutReady_;
                    calibrationLuts_ = std::move(decodedLuts);
                    calibrationLutReady_ = true;
                    calibrationSource_ = QStringLiteral("live");
                    // Persist this block so a later low-SNR pass that loses some
                    // of the 25 groups can still calibrate.
                    rememberValidatedCalibrationBlock(calibrationBlock_);
                    if (newlyReady) {
                        emit calibration2Updated(calibrationBlock_);
                        nadirstation::log::write(QStringLiteral("FY-2 Calibration-2 LUT ready from all 25 DOC groups; up to eight sequential copies per group majority-merged"));
                        emit statusMessage(QStringLiteral("FY-2 IR calibration table received (25/25 groups); brightness-temperature products can be made for this image."));
                        // Calibration may arrive after the image's Picture Flag
                        // has ended. In that case, convert the preserved native
                        // count rasters and publish a refreshed imageReady event.
                        writeCalibrationProductsToSession();
                    }
                } else {
                    calibrationLutReady_ = false;
                    calibrationSource_.clear();
                    nadirstation::log::write(QStringLiteral("FY-2 Calibration-2 block failed physical-range validation; falling back to a validated or bundled table"));
                    // Do not leave the observation without a table: drop to the
                    // validated copy or the bundled one.
                    loadFallbackCalibration();
                }
            }
        }
    }
    if (trustedSectors < MIN_TRUSTED_SECTORS) {
        if (detail) *detail = QStringLiteral("Too few trusted sectors: %1/12 (at least %2/12 needed): %3")
            .arg(trustedSectors).arg(MIN_TRUSTED_SECTORS)
            .arg(sectorReport.join(QLatin1Char(' ')));
        return false;
    }
    // Sector IDs are short headers and can be damaged while the following
    // image payload is still usable. Preserve that payload when its known-zero
    // filler confirms alignment; only mask the sector when the filler itself
    // indicates that the payload boundary may be corrupted.
    for (std::size_t si = 1; si < sectors.size(); ++si) {
        if (!sectors[si].fillerOk)
            std::fill(sectors[si].dataBits.begin(), sectors[si].dataBits.end(), 0u);
    }

    std::vector<uint8_t> doc;
    if (docTrusted) {
        QByteArray docPacked;
        appendBitsToByteArray(sectors[0].dataBits, docPacked);
        doc.assign(docPacked.begin(), docPacked.end());
    }
    int reported = docTrusted ? parseReportedLineNumber(doc) : -1;
    bool estimatedLine = false;
    // A plausible BCD value is not enough to steer row placement at weak SNR:
    // the DOC header/filler can survive while the counter bits are damaged.
    // Only CRC-accepted DOC payloads are allowed to vote on the line counter.
    const bool documentCounterValid = sectors[0].crcOk && reported >= 1 && reported <= SVISSR_SCAN_LINES;
    const int reportedDocLineNumber = documentCounterValid ? reported : -1;
    if (documentCounterValid) {
        docLineContinuityWindow_.push_back(reportedDocLineNumber);
        while (docLineContinuityWindow_.size() > 8) docLineContinuityWindow_.pop_front();
    }
    const auto addPeriodEstimate = [&](double measuredPeriod) {
        constexpr double nominal = 401408.0;
        if (!(measuredPeriod >= nominal * 0.98 && measuredPeriod <= nominal * 1.02)) return;
        lineSymbolPeriodHistory_.push_back(measuredPeriod);
        while (lineSymbolPeriodHistory_.size() > 31) lineSymbolPeriodHistory_.pop_front();
        std::vector<double> periods(lineSymbolPeriodHistory_.begin(), lineSymbolPeriodHistory_.end());
        const auto mid = periods.begin() + static_cast<std::ptrdiff_t>(periods.size() / 2);
        std::nth_element(periods.begin(), mid, periods.end());
        lineSymbolPeriodEstimate_ = *mid;
        lineSymbolPeriodObservations_ = static_cast<int>(lineSymbolPeriodHistory_.size());
    };
    // Use counter differences only after two CRC-accepted DOC records. This
    // gives an independent multi-frame check of the PN-derived line period,
    // without ever treating one readable DOC counter as the image origin.
    if (documentCounterValid && lastReliableDocSyncSymbolPosition_ > 0 &&
        syncSymbolPosition > lastReliableDocSyncSymbolPosition_ &&
        reportedDocLineNumber > lastReliableDocLineNumber_) {
        const int docSteps = reportedDocLineNumber - lastReliableDocLineNumber_;
        if (docSteps <= 100) {
            const double docPeriod = static_cast<double>(
                syncSymbolPosition - lastReliableDocSyncSymbolPosition_) / static_cast<double>(docSteps);
            addPeriodEstimate(docPeriod);
        }
    }
    if (documentCounterValid) {
        lastReliableDocSyncSymbolPosition_ = syncSymbolPosition;
        lastReliableDocLineNumber_ = reportedDocLineNumber;
    }
    int intervalLine = -1;
    if (lastAcceptedSyncSymbolPosition_ > 0 && syncSymbolPosition > lastAcceptedSyncSymbolPosition_) {
        // fengyun_svissr_fix's symbol_gap is one physical scan-line period;
        // time_gap is its symbol-count tolerance. Compute the elapsed line
        // count from PN sync positions, including frames lost during fades.
        constexpr double FY2_LINE_SYMBOL_GAP = 401408.0;
        constexpr double FY2_LINE_SYMBOL_TOLERANCE = 600.0;
        const uint64_t delta = syncSymbolPosition - lastAcceptedSyncSymbolPosition_;
        const uint64_t steps = std::max<uint64_t>(1, static_cast<uint64_t>(
            std::llround(static_cast<double>(delta) / lineSymbolPeriodEstimate_)));
        const double measuredPeriod = static_cast<double>(delta) / static_cast<double>(steps);
        const double nominal = static_cast<double>(steps) * lineSymbolPeriodEstimate_;
        const double residual = std::fabs(static_cast<double>(delta) - nominal);
        const double tolerance = lineSymbolPeriodObservations_ < 3
            ? lineSymbolPeriodEstimate_ * 0.02 * static_cast<double>(steps)
            : FY2_LINE_SYMBOL_TOLERANCE * static_cast<double>(steps);
        if (steps <= 100 && residual <= tolerance &&
            std::fabs(measuredPeriod - lineSymbolPeriodEstimate_) <= FY2_LINE_SYMBOL_GAP * 0.02) {
            // Keep a rolling robust estimate. An EMA initialized from the first
            // accepted interval let one noisy early interval bias every later
            // row calculation; the median lets a run of good PN intervals win.
            addPeriodEstimate(measuredPeriod);
            const int candidate = lastReportedLine_ + static_cast<int>(steps);
            if (candidate >= 1 && candidate <= SVISSR_SCAN_LINES)
                intervalLine = candidate;
        }
    }
    const bool hadDocAnchor = docCounterSeen_;
    if (reported < 1 || reported > SVISSR_SCAN_LINES || !documentCounterValid) {
        // A DOC sector can pass its sector ID/filler checks while a few status
        // bits still corrupt the BCD counter. Keep the scan continuous when we
        // have an immediately preceding valid counter; the next good DOC value
        // resynchronizes this estimate.
        // Before the visible Earth scan begins, valid S-VISSR frames can carry
        // black-space samples while the DOC line counter is unset. Keep every
        // structurally valid frame in stream order until the counter appears.
        reported = sequenceLineNumber_;
        // The FY2X line-fix pipeline uses the nominal 401408-symbol scan
        // interval after PN synchronization. Use the measured interval to
        // account for whole frames lost during a fade; otherwise a damaged
        // DOC counter would compress subsequent scan rows upward.
        if (intervalLine > 0) reported = intervalLine;
        estimatedLine = true;
    } else if (pictureCycleActive_ && intervalLine > 0) {
        // Inside a Picture Flag cycle, keep rows relative to the cycle start.
        // Using the first readable DOC value as the image origin makes one bad
        // low-SNR frame shift the whole quicklook. PN-sync spacing accumulates
        // across accepted frames and accounts for any missed lines.
        reported = intervalLine;
        estimatedLine = reportedDocLineNumber != reported;
    } else {
        // If the DOC counter becomes valid only after black-space scan frames,
        // it may restart at line 1 while our sequential image already contains
        // those leading frames. Offset the valid DOC counter to keep the Earth
        // edge after the preceding black rows instead of sorting it to the top.
        if (!pictureCycleActive_ && !docCounterSeen_ && sequenceLineNumber_ > reported)
            lineNumberOffset_ = sequenceLineNumber_ - reported;
        docCounterSeen_ = true;
        if (lineNumberOffset_ != 0) {
            reported = std::clamp(reported + lineNumberOffset_, 1, SVISSR_SCAN_LINES);
            estimatedLine = true;
        }
    }
    // DOC's BCD fields can occasionally decode to a different but plausible
    // line (for example 79 followed by 70). Once anchored, prefer the measured
    // PN interval when it is within the configured symbol tolerance.
    if ((pictureCycleActive_ || hadDocAnchor) && intervalLine > 0 && reported != intervalLine) {
        nadirstation::log::write(QStringLiteral("line interval correction DOC=%1 -> %2 sync_delta=%3")
            .arg(reported).arg(intervalLine)
            .arg(static_cast<qulonglong>(syncSymbolPosition - lastAcceptedSyncSymbolPosition_)));
        reported = intervalLine;
        estimatedLine = true;
    }
    // A valid DOC counter may become stuck on an old lock or jump to a new
    // numbering origin. Re-anchor it only when the current value ends an
    // uninterrupted +1 run of at least six samples in the eight-sample window.
    // A run earlier in the window is stale evidence if the newest value jumped.
    // The target row comes from PN-sync timing when available, otherwise from
    // the already-locked sequential row. This makes the correction auditable
    // and prevents a single plausible but corrupt BCD value from moving rows.
    if (!pictureCycleActive_ && hadDocAnchor && documentCounterValid && docLineContinuityWindow_.size() == 8) {
        int trailingIncrementRun = 1;
        for (std::size_t i = 1; i < docLineContinuityWindow_.size(); ++i) {
            if (docLineContinuityWindow_[i] == docLineContinuityWindow_[i - 1] + 1) {
                ++trailingIncrementRun;
            } else {
                trailingIncrementRun = 1;
            }
        }
        if (trailingIncrementRun >= 6) {
            const int targetLine = intervalLine > 0 ? intervalLine : lastReportedLine_ + 1;
            if (targetLine >= 1 && targetLine <= SVISSR_SCAN_LINES) {
                const int candidateOffset = targetLine - reportedDocLineNumber;
                if (candidateOffset != lineNumberOffset_) {
                    nadirstation::log::write(QStringLiteral(
                        "line counter recalibrated after 8-sample continuity: increments=%1 old_offset=%2 new_offset=%3 raw=%4 target=%5")
                        .arg(trailingIncrementRun - 1).arg(lineNumberOffset_).arg(candidateOffset)
                        .arg(reportedDocLineNumber).arg(targetLine));
                    lineNumberOffset_ = candidateOffset;
                    reported = targetLine;
                    estimatedLine = true;
                }
            }
        }
    }
    line.lineNumber = reported;
    line.syncSymbolPosition = syncSymbolPosition;
    line.reportedDocLineNumber = reportedDocLineNumber;
    line.lineNumberEstimated = estimatedLine;
    line.documentCounterValid = documentCounterValid;
    line.scanMode = docTrusted && doc.size() > 0 ? doc[0] : -1;
    line.scanStatus = docTrusted && doc.size() > 1 ? doc[1] : -1;
    line.frameFlag = docTrusted && doc.size() > 2 ? doc[2] : -1;
    line.pictureFlag = docTrusted && doc.size() > 3 ? doc[3] : -1;
    readBcd2(doc, 4, line.setLine);
    readBcd2(doc, 6, line.resetLine);
    readBcd2(doc, 8, line.scanCount);
    sequenceLineNumber_ = reported + 1;
    lastReportedLine_ = reported;
    if (syncSymbolPosition > lastAcceptedSyncSymbolPosition_)
        lastAcceptedSyncSymbolPosition_ = syncSymbolPosition;
    // BCD-valid time can still contain a flipped bit. Only a CRC-accepted DOC
    // may supply an observation timestamp or influence the manifest time span.
    line.time = docTrusted && sectors[0].crcOk ? parseDocTime(doc) : QDateTime{};
    line.valid = true;
    line.crcGood = goodCrc;
    line.crcTotal = totalCrc;
    line.crcCorrected = correctedTotal;

    const auto& ir1h = sectors[1].dataBits;
    const auto& ir2h = sectors[2].dataBits;
    const auto& ir3h = sectors[3].dataBits;
    const auto& vis1 = sectors[4].dataBits;
    const auto& vis2 = sectors[5].dataBits;
    const auto& vis3 = sectors[6].dataBits;
    const auto& vis4 = sectors[7].dataBits;
    const auto& ir1l = sectors[8].dataBits;
    const auto& ir2l = sectors[9].dataBits;
    const auto& ir3l = sectors[10].dataBits;
    const auto& ir4 = sectors[11].dataBits;
    line.irValid = {{sectors[1].idOk && sectors[1].fillerOk && sectors[8].idOk && sectors[8].fillerOk,
                     sectors[2].idOk && sectors[2].fillerOk && sectors[9].idOk && sectors[9].fillerOk,
                     sectors[3].idOk && sectors[3].fillerOk && sectors[10].idOk && sectors[10].fillerOk,
                     sectors[11].idOk && sectors[11].fillerOk}};

    const auto ir1H = unpackSamples(ir1h, SVISSR_IR_SAMPLES, 8);
    const auto ir2H = unpackSamples(ir2h, SVISSR_IR_SAMPLES, 8);
    const auto ir3H = unpackSamples(ir3h, SVISSR_IR_SAMPLES, 8);
    const auto ir1L = unpackSamples(ir1l, SVISSR_IR_SAMPLES, 2);
    const auto ir2L = unpackSamples(ir2l, SVISSR_IR_SAMPLES, 2);
    const auto ir3L = unpackSamples(ir3l, SVISSR_IR_SAMPLES, 2);
    line.ir[0].resize(SVISSR_IR_SAMPLES);
    line.ir[1].resize(SVISSR_IR_SAMPLES);
    line.ir[2].resize(SVISSR_IR_SAMPLES);
    const auto ir4Raw = unpackSamples(ir4, SVISSR_IR_SAMPLES, 10);
    for (int i = 0; i < SVISSR_IR_SAMPLES; ++i) {
        // FY-2 infrared samples are transmitted inverted. They are un-inverted
        // before storing the channel and the calibration LUT is reversed to match,
        // so the 10-bit samples stay in the calibrated count domain.
        line.ir[0][i] = static_cast<quint16>(1023u - ((ir1H[i] << 2) | ir1L[i]));
        line.ir[1][i] = static_cast<quint16>(1023u - ((ir2H[i] << 2) | ir2L[i]));
        line.ir[2][i] = static_cast<quint16>(1023u - ((ir3H[i] << 2) | ir3L[i]));
    }
    line.ir[3].resize(SVISSR_IR_SAMPLES);
    for (int i = 0; i < SVISSR_IR_SAMPLES; ++i)
        line.ir[3][i] = static_cast<quint16>(1023u - ir4Raw[i]);
    line.vis[0] = unpackSamples(vis1, SVISSR_VIS_SAMPLES, 6);
    line.vis[1] = unpackSamples(vis2, SVISSR_VIS_SAMPLES, 6);
    line.vis[2] = unpackSamples(vis3, SVISSR_VIS_SAMPLES, 6);
    line.vis[3] = unpackSamples(vis4, SVISSR_VIS_SAMPLES, 6);

    int goodIds = 0;
    for (const auto& sector : sectors) if (sector.idOk) ++goodIds;
    if (detail) *detail = QStringLiteral("line=%1%2 ID=%3/12 trusted=%4/12 CRC=%5/%6 · corrected bits=%7%8")
        .arg(line.lineNumber).arg(estimatedLine ? QStringLiteral("(estimated)") : QString())
        .arg(goodIds).arg(trustedSectors).arg(goodCrc).arg(totalCrc).arg(correctedTotal)
        .arg(softEngaged ? QStringLiteral(" · soft sectors=%1").arg(softRecoveredSectors) : QString());
    return true;
}

bool NadirStationDecoder::decodeFrame(const DeframedFrame& frame, Line& line, QString* detail) {
    // No per-frame log lines here: at 100 lines a minute they pushed the useful history out of the
    // 30 MiB runtime log within hours (the ACCEPT/REJECT line below says what happened to the frame).
    const QByteArray raw = derandomize(frame.bits);
    return parseFrame(raw, frame.reliability, frame.syncSymbolPosition, line, detail,
                      frame.soft.empty() ? nullptr : &frame.soft);
}

void NadirStationDecoder::processBits(const std::vector<uint8_t>& bits) {
    std::vector<uint8_t> reliability(bits.size(), 255);
    processBitsWithDeframer(bits, reliability, frameLocker_[0]);
}

// The link state, evaluated whether or not the block produced symbols.
//
// This used to live inside processGk2aSymbols below its early return for an empty symbol vector, so
// a block with no symbols updated nothing: the timers did not advance and the lock kept its last
// value. The receiver could be silent for ten seconds with the lock still reading its best value,
// which is why re-acquisition never triggered. The lock is derived from the timestamps here rather
// than assigned when a frame is seen, because those differ precisely when frames have stopped.
void NadirStationDecoder::refreshGk2aLinkState(qint64 now) {
    constexpr qint64 kRsHoldMs = 5000;
    constexpr qint64 kSyncHoldMs = 3000;
    // The concatenated decoder gives up sync after 20 CADUs. One GK-2A CADU
    // takes 128 ms, so first resynchronise FEC/frame state after a short margin.
    constexpr qint64 kFecResyncMs = 3000;
    constexpr qint64 kCarrierReacquireMs = 6000;
    constexpr qint64 kCarrierRetryMs = 4000;

    if (!gk2aEverHadFrame_ || lastGk2aSyncMs_ == 0) {
        lock_ = 0;
        gk2aReacquired_ = false;
        gk2aRecoveryAttempt_ = 0;
        return;
    }

    const qint64 silenceMs = std::max<qint64>(0, now - lastGk2aSyncMs_);
    if (silenceMs <= kSyncHoldMs) {
        lock_ = lastGk2aRsOkMs_ != 0 && now - lastGk2aRsOkMs_ < kRsHoldMs ? 100 : 50;
        if (gk2aReacquired_) {
            nadirstation::log::write(QStringLiteral(
                "GK-2A: CADU sync reacquired; recovery attempts=%1 CFO=%2 Hz")
                .arg(gk2aRecoveryAttempt_).arg(carrierOffsetHz_, 0, 'f', 1));
        }
        gk2aReacquired_ = false;
        gk2aRecoveryAttempt_ = 0;
        return;
    }

    // Keep the visible lock through a short CADU-sync gap while the most recent frame
    // still has fresh Reed-Solomon validation. The FEC/ASM state is still reset below at
    // the 20-CADU timeout; this only prevents the status badge flickering to zero
    // for the brief gaps that the live log shows recovering on the very next CADU.
    lock_ = lastGk2aRsOkMs_ != 0 && now - lastGk2aRsOkMs_ < kRsHoldMs ? 100 : 0;
    if (silenceMs <= kFecResyncMs) return;

    if (gk2aRecoveryAttempt_ == 0) {
        // Out-of-sync recovery: discard stale trellis/ASM history, but keep the demodulator's
        // filters and loops streaming continuously across the reset.
        flushGk2aObservation(false);
        gk2aLritAssembler_.reset();
        gk2aLrit_.reset();
    hritDecoder_.reset(); hritAssembler_.reset(); hritAssembler_.setStrict(true); hritAssembler_.setLossTolerant(true);
    if(hritImages_)hritImages_->reset();
        gk2aRecoveryReferenceCfoHz_ = gk2aLastGoodCfoHz_;
        gk2aRecoveryAttempt_ = 1;
        gk2aReacquired_ = true;
        nadirstation::log::write(QStringLiteral(
            "GK-2A: no CADU for %1 ms; reset Viterbi/ASM, keeping carrier/timing loops "
            "(CFO=%2 Hz, last RS=%3 ms ago)")
                .arg(silenceMs).arg(carrierOffsetHz_, 0, 'f', 1)
                .arg(lastGk2aRsOkMs_ == 0 ? -1 : now - lastGk2aRsOkMs_));
        return;
    }

    if (silenceMs < kCarrierReacquireMs) return;

    // If frame sync still has not returned, the carrier/timing loops may have slipped.
    // Reset the demodulator, which measures the carrier again before its loops start (the
    // offsets below only seed the loop until that measurement is made).
    static constexpr float kOffsetStepsHz[] = {0.0f, -250.0f, 250.0f, -500.0f,
                                                500.0f, -1000.0f, 1000.0f};
    const qint64 nextAttemptAt = kCarrierReacquireMs
        + static_cast<qint64>(std::max(0, gk2aRecoveryAttempt_ - 1)) * kCarrierRetryMs;
    if (silenceMs < nextAttemptAt) return;

    const int offsetIndex = (gk2aRecoveryAttempt_ - 1)
        % static_cast<int>(std::size(kOffsetStepsHz));
    const float offsetHz = gk2aRecoveryReferenceCfoHz_ + kOffsetStepsHz[offsetIndex];
    const float demodRate = pskDemod_.sampleRate();
    const float radiansPerSample = demodRate > 0.0f
        ? (2.0f * 3.14159265358979323846f * offsetHz / demodRate) : 0.0f;
    pskDemod_.setInitialCarrierOffset(radiansPerSample);
    pskDemod_.reset();
    gk2aLrit_.reset();
    hritDecoder_.reset(); hritAssembler_.reset(); hritAssembler_.setStrict(true); hritAssembler_.setLossTolerant(true);
    if(hritImages_)hritImages_->reset();
    ++gk2aRecoveryAttempt_;
    gk2aReacquired_ = true;
    nadirstation::log::write(QStringLiteral(
        "GK-2A: no CADU for %1 ms; carrier re-measured, loops and Viterbi reset near CFO %2 Hz (attempt %3)")
            .arg(silenceMs).arg(offsetHz, 0, 'f', 1).arg(gk2aRecoveryAttempt_ - 1));
}
// Publishes a symbol set to the constellation display, on the same cadence whether it comes from the
// ordinary path or from the carrier search. While the search is running there is no winner, and the
// constellation is the most useful thing it can show -- an attempt at the wrong offset draws a
// rotating constellation and an empty band draws noise.
void NadirStationDecoder::publishConstellation(const std::vector<std::complex<float>>& symbols) {
    if (symbols.empty()) return;
    constellationBuffer_ = symbols;
    if (constellationBuffer_.size() > 1200)
        constellationBuffer_.erase(constellationBuffer_.begin(),
                                   constellationBuffer_.end() - static_cast<std::ptrdiff_t>(1200));
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastConstellationEmitMs_ == 0 || nowMs - lastConstellationEmitMs_ >= 25) {
        lastConstellationEmitMs_ = nowMs;
        emit symbolConstellation(std::make_shared<std::vector<std::complex<float>>>(
            constellationBuffer_));
    }
}

void NadirStationDecoder::processGk2aSymbols(const std::vector<std::complex<float>>& symbols) {
    // Stream time, not wall clock: a file is processed far faster than it played, so wall-clock
    // timeouts expire about seven times too slowly and a lost signal is never noticed.
    const qint64 now = gk2aStreamMs_;
    if (symbols.empty()) {
        // No symbols still says something about the link: that nothing arrived. Evaluating it here
        // is what the early return used to skip, leaving the lock frozen at its last value.
        refreshGk2aLinkState(now);
        return;
    }

    // The demodulator emits one signed soft stream from the carrier-corrected BPSK real axis.
    // The concatenated decoder then performs Viterbi, ASM deframing, derandomization and RS(223)
    // interleave-depth-4 checking. Keep one continuous chain so symbol/trellis state is never
    // split across polarity hypotheses or carrier-search candidates.
    // PskDemod normalises the symbols to a mean |Re| of 1. Half that keeps noisy symbols inside
    // the Viterbi decoder's soft range instead of clipping them: on a GK-2A recording with noise
    // added, 131 good frames against 91 at full scale (scratch A/B, 2026-10-18).
    constexpr float kViterbiSoftScale = 0.5f;
    std::vector<float> soft;
    soft.reserve(symbols.size());
    for (const auto& z : symbols) soft.push_back(-kViterbiSoftScale * z.real());

    gk2aLrit_.feedSoftBits(soft, false);
    bool synced = false;
    bool checked = false;
    std::size_t parityRejected = 0;
    for (const nadirstation::gk2a::Cadu& cadu : gk2aLrit_.takeCadus()) {
        if (!cadu.syncFound) continue;
        synced = true;
        if (!cadu.rsOk) {
            // A 32-bit sync marker is the only thing holding a frame that failed its parity
            // together, and a false one arrives with a first-header pointer and a file type that
            // look like fields but are not. Feeding it to the reassembler splices its bytes into
            // whatever file is open on that channel. The same step is gated on the check,
            // so frames that fail it stop here and are only counted.
            ++parityRejected;
            continue;
        }
        checked = true;
        for (const nadirstation::gk2a::LritFile& file : gk2aLritAssembler_.feed(cadu.bytes))
            writeGk2aLritFile(file);
    }
    if (parityRejected > 0 &&
        (lastGk2aParityRejectLogMs_ == 0 || now - lastGk2aParityRejectLogMs_ >= 10000)) {
        lastGk2aParityRejectLogMs_ = now;
        const nadirstation::gk2a::DecodeStats& s = gk2aLrit_.stats();
        nadirstation::log::write(QStringLiteral(
            "GK-2A: %1 CADU(s) in this block carried the sync marker but failed Reed-Solomon and "
            "were dropped before reassembly (%2 of %3 frames have passed the check so far)")
            .arg(static_cast<qulonglong>(parityRejected))
            .arg(static_cast<qulonglong>(s.cadusWithGoodRs))
            .arg(static_cast<qulonglong>(s.cadusWithSync)));
    }
    if (checked) lastGk2aRsOkMs_ = now;
    if (synced) lastGk2aSyncMs_ = now;
    if (synced) {
        // Save the loop estimate only while a frame proves that the carrier/timing
        // solution is actually usable. Once sync is gone, the live estimate may be
        // the carrier loop winding away from lock and is not a safe reacquisition seed.
        gk2aLastGoodCfoHz_ = carrierOffsetHz_;
        gk2aLastFrameMs_ = now;
        gk2aEverHadFrame_ = true;
    }
    refreshGk2aLinkState(now);
}

namespace {

// The operator's key file: a two-byte big-endian count, then ten-byte entries of a two-byte
// big-endian index and eight key bytes. Same layout the streaming tool reads.
std::map<int, std::array<std::uint8_t, 8>> loadGk2aKeys(const QString& path) {
    std::map<int, std::array<std::uint8_t, 8>> keys;
    QFile in(path);
    if (!in.open(QIODevice::ReadOnly)) return keys;
    const QByteArray bytes = in.readAll();
    in.close();
    if (bytes.size() < 2) return keys;
    const int count = (static_cast<std::uint8_t>(bytes[0]) << 8)
                      | static_cast<std::uint8_t>(bytes[1]);
    for (int i = 0; i < count; ++i) {
        const int at = 2 + i * 10;
        if (at + 10 > bytes.size()) break;
        std::array<std::uint8_t, 8> key{};
        std::memcpy(key.data(), bytes.constData() + at + 2, 8);
        keys[(static_cast<std::uint8_t>(bytes[at]) << 8)
             | static_cast<std::uint8_t>(bytes[at + 1])] = key;
    }
    return keys;
}

// The strip number, from the last field of the file name: IMG_<mode>_<seq>_<channel>_<date>_<time>_<NN>.
// Returns -1 when the name does not carry one, because a strip placed at a guessed row would be
// worse than one not placed.
int stripIndexFromName(const QString& fileName) {
    const QString stem = QFileInfo(fileName).completeBaseName();
    const QStringList parts = stem.split('_');
    if (parts.size() < 2) return -1;
    bool ok = false;
    const int value = parts.last().toInt(&ok);
    return ok && value > 0 ? value - 1 : -1;
}

}  // namespace

// The first JPEG start marker in a payload, or the end when there is none.
std::size_t jpegStartOf(const std::vector<std::uint8_t>& payload) {
    for (std::size_t i = 0; i + 3 <= payload.size() && i < 4096; ++i)
        if (payload[i] == 0xFF && payload[i + 1] == 0xD8 && payload[i + 2] == 0xFF) return i;
    return payload.size();
}

namespace {

// The number of segments the disk is split into, from the segmentation record.
//
// Type 128 carries the image sequence number, the total segment count and the line number; the
// files received read 01 0a 00 01, so sequence 1 of 10. Reading it rather than assuming ten means a
// service that changed its segmentation still assembles, and the assumption that used to be here
// was right only by coincidence of the present geometry.
int totalSegmentsOf(const std::vector<std::uint8_t>& file) {
    std::uint32_t totalHeader = 0;
    if (file.size() >= 8)
        totalHeader = (static_cast<std::uint32_t>(file[4]) << 24)
                      | (static_cast<std::uint32_t>(file[5]) << 16)
                      | (static_cast<std::uint32_t>(file[6]) << 8)
                      | static_cast<std::uint32_t>(file[7]);
    if (totalHeader == 0 || totalHeader > file.size()) totalHeader = file.size();
    std::size_t pos = 0;
    while (pos + 3 <= totalHeader) {
        const std::uint8_t type = file[pos];
        const std::uint16_t len = static_cast<std::uint16_t>((file[pos + 1] << 8) | file[pos + 2]);
        if (len < 3 || pos + len > totalHeader) break;
        if (type == 128 && pos + 5 <= file.size()) {
            // type, length, image sequence number, total segment number, line number -- four
            // bytes, each a byte. Reading the last two as one sixteen-bit number gives 0x010A for
            // the first strip: the sequence number shifted up eight, with the real count below it.
            return static_cast<int>(file[pos + 4]);
        }
        pos += len;
    }
    return 0;
}

}  // namespace

void NadirStationDecoder::writeGk2aAdditionalContent(const nadirstation::gk2a::LritFile& file,
                                             const std::vector<std::uint8_t>& payload,
                                             const QString& rawPath,
                                             const QString& keyName) {
    // What the payload is decides the file it is written as, because the point of writing it at all
    // is that it can be read. The raw file beside it stays untouched -- it is what the satellite
    // sent, and the content is derived from it rather than replacing it.
    // JPEG, PNG and GIF. The reference checks PNG and GIF by name and this project checked JPEG and
    // PNG, so a chart sent as a GIF would have been filed as .bin and shown as hexadecimal -- the
    // content arriving, being written, and being unreadable.
    auto looksLikeImage = [](const std::vector<std::uint8_t>& b) {
        if (b.size() < 5) return false;
        const bool jpeg = b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF;
        const bool png = b[0] == 0x89 && b[1] == 0x50 && b[2] == 0x4E && b[3] == 0x47;
        const bool gif = b[0] == 'G' && b[1] == 'I' && b[2] == 'F';
        return jpeg || png || gif;
    };
    const bool image = looksLikeImage(payload);

    std::size_t end = payload.size();
    while (end > 0 && (payload[end - 1] == 0x00 || payload[end - 1] == 0x20)) --end;
    std::size_t printable = 0;
    for (std::size_t i = 0; i < end; ++i) {
        const std::uint8_t ch = payload[i];
        if (ch == 0x0A || ch == 0x0D || ch == 0x09 || (ch >= 32 && ch < 127)) ++printable;
    }
    // Readable content is text whatever its category. Content in ANT is text because the category
    // says what ANT carries -- the reference writes it as text without looking, and that is
    // knowledge about the service rather than a guess about the bytes.
    const bool text = (end > 0 && printable * 10 >= end * 9) ||
                      nadirstation::gk2a::lritAddCategory(file.fileName) == "ANT";

    QString contentPath;
    if (text) {
        contentPath = rawPath + QStringLiteral(".txt");
        QFile out(contentPath);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(reinterpret_cast<const char*>(payload.data()), static_cast<qint64>(end));
            out.close();
        }
    } else if (image) {
        // Written in the format it arrived in, not re-encoded. The bytes are what the satellite sent,
        // and converting them would lose the difference between what came in and what was drawn from
        // it -- as well as turning a GIF into a PNG, which is not what a chart sent as a GIF should
        // be filed as.
        const bool jpeg = payload.size() > 3 && payload[0] == 0xFF && payload[1] == 0xD8;
        const bool png = payload.size() > 4 && payload[0] == 0x89 && payload[1] == 0x50;
        contentPath = rawPath
                      + (jpeg ? QStringLiteral(".jpg")
                              : (png ? QStringLiteral(".png") : QStringLiteral(".gif")));

        std::vector<std::uint8_t> bytes = payload;
        // Only a JPEG gets an end marker put back, and only a JPEG has one to miss. The last
        // segment of a reception ends mid-scan as a matter of course; a GIF or PNG that is short is
        // short, and appending someone else's terminator would be inventing data.
        if (jpeg && !(bytes.size() >= 2 && bytes[bytes.size() - 2] == 0xFF
                      && bytes[bytes.size() - 1] == 0xD9)) {
            bytes.push_back(0xFF);
            bytes.push_back(0xD9);
        }
        QFile out(contentPath);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size()));
            out.close();
        } else {
            contentPath.clear();
        }
    }
    if (contentPath.isEmpty()) {
        contentPath = rawPath + QStringLiteral(".bin");
        QFile out(contentPath);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(reinterpret_cast<const char*>(payload.data()),
                      static_cast<qint64>(payload.size()));
            out.close();
        }
    }

    nadirstation::log::write(QStringLiteral("GK-2A additional data %1: %2 bytes, key %3, written as %4")
        .arg(QFileInfo(rawPath).fileName())
        .arg(payload.size())
        .arg(keyName)
        .arg(text ? QStringLiteral("text")
                  : (image ? QStringLiteral("an image") : QStringLiteral("bytes"))));
}

void NadirStationDecoder::flushGk2aObservation(bool final) {
    if (gk2aPendingReady_.isEmpty()) return;

    if (!final) return;

    // Keep partial reception data in memory, but only publish the calibrated enhanced image.
    QStringList products;
    if (!gk2aDisk_.isNull() && !gk2aObservationDir_.isEmpty()) {
        const nadirstation::gk2a::DiskGeometry geometry =
            nadirstation::gk2a::measureDisk(gk2aDisk_, gk2aNavigation_);
        if (!gk2aNavigation_.valid)
            nadirstation::log::write(QStringLiteral(
                "GK-2A: missing/invalid LRIT GEOS navigation record; raw disk only, no geolocated products"));
        products = nadirstation::gk2a::renderObservationProducts(gk2aDisk_, gk2aCalibration_, geometry,
                                                         gk2aObservationDir_,
                                                         QStringLiteral("GK2A"));
    }

    if (products.isEmpty()) {
        nadirstation::log::write(QStringLiteral(
            "GK-2A: nothing to hand over for %1; no product could be rendered")
            .arg(gk2aPendingReady_));
    } else {
        emit imageReady(gk2aPendingReady_, products);
        nadirstation::log::write(QStringLiteral("GK-2A: handing over %1, %2 product(s), %3 of %4 strips")
            .arg(gk2aPendingReady_).arg(products.size())
            .arg(gk2aStrips_.size()).arg(gk2aPendingExpected_));
    }
    gk2aPendingReady_.clear();
    gk2aPendingExpected_ = 0;
}

void NadirStationDecoder::writeGk2aLritFile(const nadirstation::gk2a::LritFile& file) {
    if (outputRoot_.isEmpty() || file.bytes.empty()) return;

    const nadirstation::gk2a::LritKind kind = nadirstation::gk2a::lritKindOf(file.primary.fileTypeCode);
    if (kind == nadirstation::gk2a::LritKind::Unknown) {
        nadirstation::log::write(QStringLiteral("GK-2A: ignoring unsupported LRIT file type %1 (%2)")
            .arg(file.primary.fileTypeCode).arg(QString::fromStdString(file.fileName)));
        return;
    }

    const std::string name = file.fileName.empty() ? std::string("unnamed.lrit") : file.fileName;

    // ADD/GTS/text/key records belong to their own service tree, not the image pipeline.
    // Preserve the transmitted LRIT first, then decrypt and decode its data field for the ADD
    // reader. This path used to be unreachable because all non-image file types returned above.
    if (kind != nadirstation::gk2a::LritKind::Image) {
        if (!gk2aKeysLoaded_) {
            gk2aKeysLoaded_ = true;
            gk2aKeys_ = loadGk2aKeys(gk2aKeyPath_);
            nadirstation::log::write(QStringLiteral("GK-2A: %1 decryption keys from %2")
                .arg(gk2aKeys_.size()).arg(gk2aKeyPath_));
        }

        QString safeName = QFileInfo(QString::fromStdString(name)).fileName();
        if (safeName.isEmpty() || safeName == QStringLiteral(".") || safeName == QStringLiteral(".."))
            safeName = QStringLiteral("unnamed.lrit");
        const std::string relative = nadirstation::gk2a::lritRelativePath(
            safeName.toStdString(), kind, spacecraftFolder().toStdString(), {});
        const QString rawPath = QDir(outputRoot_).filePath(QString::fromStdString(relative));
        if (!QDir().mkpath(QFileInfo(rawPath).absolutePath())) {
            nadirstation::log::write(QStringLiteral("GK-2A ADD: cannot create folder for %1").arg(rawPath));
            return;
        }
        QFile rawFile(rawPath);
        if (!rawFile.open(QIODevice::WriteOnly) ||
            rawFile.write(reinterpret_cast<const char*>(file.bytes.data()),
                          static_cast<qint64>(file.bytes.size())) !=
                static_cast<qint64>(file.bytes.size())) {
            nadirstation::log::write(QStringLiteral("GK-2A ADD: cannot save raw LRIT file %1").arg(rawPath));
            return;
        }
        rawFile.close();

        if (file.dataOffset > file.bytes.size()) {
            nadirstation::log::write(QStringLiteral("GK-2A ADD: invalid data offset %1 in %2-byte file %3")
                .arg(file.dataOffset).arg(file.bytes.size()).arg(rawPath));
            return;
        }
        std::vector<std::uint8_t> payload(
            file.bytes.begin() + static_cast<std::ptrdiff_t>(file.dataOffset), file.bytes.end());
        if (payload.empty()) {
            nadirstation::log::write(QStringLiteral("GK-2A ADD: empty data field in %1").arg(rawPath));
            return;
        }
        QString keyName = QStringLiteral("(not encrypted)");
        if (file.encrypted) {
            const auto found = gk2aKeys_.find(static_cast<int>(file.encryptionKey));
            if (found == gk2aKeys_.end()) {
                nadirstation::log::write(QStringLiteral(
                    "GK-2A ADD: saved raw %1, but no decryption key %2 is available")
                    .arg(rawPath).arg(file.encryptionKey));
                return;
            }
            keyName = QStringLiteral("index %1").arg(file.encryptionKey);
            const std::size_t pad = (8 - payload.size() % 8) % 8;
            payload.resize(payload.size() + pad, 0x00);
            std::vector<std::uint8_t> plain(payload.size(), 0);
            const auto subkeys = nadirstation::crypto::desKeySchedule(found->second.data());
            nadirstation::crypto::desEcbDecrypt(payload.data(), plain.data(), payload.size(), subkeys);
            payload.swap(plain);
        }
        writeGk2aAdditionalContent(file, payload, rawPath, keyName);
        nadirstation::log::write(QStringLiteral("GK-2A ADD: received type %1 %2 -> %3")
            .arg(file.primary.fileTypeCode).arg(QString::fromStdString(name)).arg(rawPath));
        return;
    }

    // Keys are loaded once, on first use, because the decoder has no other reason to touch the
    // filesystem at construction and a station that never sees this service should not read them.
    if (!gk2aKeysLoaded_) {
        gk2aKeysLoaded_ = true;
        gk2aKeys_ = loadGk2aKeys(gk2aKeyPath_);
        nadirstation::log::write(QStringLiteral("GK-2A: %1 decryption keys from %2")
            .arg(gk2aKeys_.size()).arg(gk2aKeyPath_));
    }

    // The layout is the one the service defines: imagery under a folder per
    // observation, additional data under ADD/<category>/. One function decides it so the two
    // cannot drift apart.
    // The folder is named after the observation, taken from the file name, and only falls back to
    // the current time when the name carries no date. A folder named after when the receiver
    // happened to write the file is not the same thing and is not what a reader expects.
    std::string stamp;
    {
        const QString qname = QString::fromStdString(name);
        const QStringList parts = qname.split('_');
        for (int i = 0; i + 1 < parts.size(); ++i) {
            const QString& a = parts[i];
            const QString& b = parts[i + 1];
            if (a.size() == 8 && b.size() == 6
                && a.toLongLong() > 0 && b.toLongLong() >= 0) {
                stamp = (a + "_" + b).toStdString();
                break;
            }
        }
    }
    if (stamp.empty())
        stamp = QDateTime::currentDateTimeUtc()
                    .toString(QStringLiteral("yyyyMMdd_hhmmss")).toStdString();
    const QString qstamp = QString::fromStdString(stamp);
    // Start a clean assembly as soon as the first strip of a new observation arrives.
    // Previously the strip was copied into the previous disk before the timestamp switch
    // was handled, and the strip-index set survived the handoff. That made the next preview
    // include stale pixels and could prevent its first packet from being emitted as new.
    if (gk2aActiveStamp_ != qstamp) {
        if (!gk2aActiveStamp_.isEmpty()) flushGk2aObservation(true);
        gk2aDisk_ = QImage();
        gk2aStrips_.clear();
        gk2aCalibration_ = nadirstation::gk2a::Calibration{};
        gk2aNavigation_ = nadirstation::gk2a::LritNavigation{};
        gk2aObservationDir_.clear();
        gk2aPendingExpected_ = 0;
        gk2aActiveStamp_ = qstamp;
    }
    ++gk2aFilesWritten_;
    nadirstation::log::write(QStringLiteral("GK-2A received image LRIT segment %1 (%2 bytes; kept in memory)")
        .arg(QString::fromStdString(name)).arg(file.bytes.size()));

    // The payload from the announced header length onwards, decrypted if the file says so. The
    // specification puts the padding in for the cipher's block alignment, which is why it is
    // applied here and not treated as part of the data.
    //
    // This sits above the file type rather than below it. It used to be under a return that skipped
    // every non-image file, so additional data was written encrypted and the page that reads it
    // showed ciphertext: for a reader asking for the observation schedule, a block of random
    // bytes. The encryption belongs to the service, not to what the payload contains.
    // The offset comes from the file's own header, and the file comes off the air, so it is
    // checked against the buffer before it is used to index it. A corrupt or mis-assembled file
    // gives an offset past the end, and slicing there is an access violation -- which is what the
    // crash log recorded, twice, after nine strips and while the tenth was being handled.
    if (file.dataOffset > file.bytes.size()) {
        nadirstation::log::write(QStringLiteral("GK-2A: %1 announces its data at %2 but is only %3 bytes")
            .arg(name.c_str()).arg(file.dataOffset).arg(file.bytes.size()));
        return;
    }
    std::vector<std::uint8_t> payload(
        file.bytes.begin() + static_cast<std::ptrdiff_t>(file.dataOffset), file.bytes.end());
    if (payload.empty()) return;
    QString keyName = QStringLiteral("(not encrypted)");
    if (file.encrypted) {
        const auto found = gk2aKeys_.find(static_cast<int>(file.encryptionKey));
        if (found == gk2aKeys_.end()) {
            nadirstation::log::write(QStringLiteral("GK-2A: no key for index %1, so %2 stays encrypted")
                .arg(file.encryptionKey).arg(name.c_str()));
            return;
        }
        keyName = QStringLiteral("index %1").arg(file.encryptionKey);
        const std::size_t pad = (8 - payload.size() % 8) % 8;
        for (std::size_t i = 0; i < pad; ++i) payload.push_back(0x00);
        std::vector<std::uint8_t> plain(payload.size(), 0);
        const auto subkeys = nadirstation::crypto::desKeySchedule(found->second.data());
        nadirstation::crypto::desEcbDecrypt(payload.data(), plain.data(), payload.size(), subkeys);
        payload.swap(plain);
    }

    // The first image file of an observation carries the count-to-kelvin table, and every image
    // file of the same observation carries the same one, so the first is kept. Taken here rather
    // than from whichever file arrived first, because it arrives in an image's annotation record
    // and a file that is not an image does not carry one.
    if (!gk2aCalibration_.usable())
        gk2aCalibration_ = nadirstation::gk2a::calibrationFromLrit(file.bytes);
    const nadirstation::gk2a::LritNavigation navigation = nadirstation::gk2a::navigationFromLrit(file.bytes);
    if (navigation.valid) {
        if (!gk2aNavigation_.valid) {
            gk2aNavigation_ = navigation;
            nadirstation::log::write(QStringLiteral("GK-2A navigation record: GEOS(%1) CFAC %2 LFAC %3 COFF %4 LOFF %5")
                .arg(navigation.subLonDeg).arg(navigation.cfac).arg(navigation.lfac).arg(navigation.coff).arg(navigation.loff));
        } else if (gk2aNavigation_.cfac != navigation.cfac ||
                   gk2aNavigation_.lfac != navigation.lfac ||
                   gk2aNavigation_.coff != navigation.coff ||
                   gk2aNavigation_.loff != navigation.loff ||
                   std::fabs(gk2aNavigation_.subLonDeg - navigation.subLonDeg) > 1e-6) {
            nadirstation::log::write(QStringLiteral(
                "GK-2A: conflicting LRIT navigation records within one disk; suppressing geolocation"));
            gk2aNavigation_.valid = false;
        }
    }

    // The JPEG, and its end marker if the reception did not capture it. A file that ends mid-scan
    // is the normal case rather than a fault, and Qt refuses such a file outright.
    const std::size_t at = jpegStartOf(payload);
    if (at >= payload.size()) {
        nadirstation::log::write(QStringLiteral("GK-2A: no JPEG in %1").arg(name.c_str()));
        return;
    }
    if (payload.size() < 2 || payload[payload.size() - 2] != 0xFF
        || payload[payload.size() - 1] != 0xD9) {
        payload.push_back(0xFF);
        payload.push_back(0xD9);
    }
    QImage strip;
    strip.loadFromData(payload.data() + static_cast<std::ptrdiff_t>(at),
                       static_cast<int>(payload.size() - at), "JPEG");
    if (strip.isNull()) {
        // The four numbers that separate a wrong key from a short file. The index comes from a
        // header record, and a record assembled across a loss can yield a real key that is the
        // wrong one -- which decrypts to noise that is not a JPEG and looks exactly like
        // truncation from here.
        nadirstation::log::write(QStringLiteral(
            "GK-2A: %1 did not decode as JPEG (key index %2, encrypted %3, header %4, payload %5)")
            .arg(name.c_str()).arg(file.encryptionKey)
            .arg(file.encrypted ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(file.dataOffset).arg(payload.size()));
        return;
    }
    strip = strip.convertToFormat(QImage::Format_Grayscale8);

    // Assemble strips in memory. Incomplete fragments are never written as LRIT or PNG files.
    // The disk height comes from the file's own segmentation record rather than from a constant,
    // but a count that cannot be right is not used: the specification fixes the full disk at 2200
    // lines, so a count that does not divide into that is a misread, and taking it would produce a
    // disk of the wrong shape -- which is how a correct strip came out as a preview nobody could
    // see.
    constexpr int kFullDiskLines = 2200;
    const int segments = totalSegmentsOf(file.bytes);
    int stripCount = 10;
    if (segments > 0 && strip.height() > 0
        && kFullDiskLines % strip.height() == 0
        && segments == kFullDiskLines / strip.height()) {
        stripCount = segments;
    } else if (segments > 0) {
        nadirstation::log::write(QStringLiteral("GK-2A: segment record says %1, which does not fit a %2 "
                                        "line disk in strips of %3; using %4")
            .arg(segments).arg(kFullDiskLines).arg(strip.height())
            .arg(strip.height() > 0 ? kFullDiskLines / strip.height() : 10));
    }
    // Allocated once, and cleared only at that moment. The clear used to run on every strip, so
    // each arriving strip wiped the ones already placed and then wrote itself: the log counted
    // eight strips assembled while the picture held one, because the count and the pixels were
    // kept in different places and only the count was right.
    if (gk2aDisk_.isNull() || gk2aDisk_.width() != strip.width()
        || gk2aDisk_.height() != strip.height() * stripCount) {
        gk2aDisk_ = QImage(strip.width(), strip.height() * stripCount, QImage::Format_Grayscale8);
        gk2aDisk_.fill(0);
    }
    if (gk2aDisk_.isNull()) return;
    const int index = stripIndexFromName(QString::fromStdString(name));
    bool newStrip = false;
    if (index >= 0 && index * strip.height() < gk2aDisk_.height()) {
        newStrip = gk2aStrips_.insert(index).second;
        for (int row = 0; row < strip.height(); ++row) {
            const int target = index * strip.height() + row;
            if (target >= gk2aDisk_.height()) break;
            std::copy(strip.constScanLine(row), strip.constScanLine(row) + strip.width(),
                      gk2aDisk_.scanLine(target));
        }
    }

    gk2aObservationDir_ = QDir(outputRoot_).filePath(QStringLiteral("GK-2A/")
        + QString::fromStdString(stamp));

    // The list entry is not cheap -- it loads the products and generates the regional ones -- so
    // it is made once per observation. A different stamp means the previous observation is done
    // and this strip starts a new one, which is the only end a partial reception ever sees:
    // waiting for a complete disk would leave the list empty, because eight strips of ten is the
    // normal case rather than an edge one.
    gk2aPendingReady_ = qstamp;
    gk2aPendingExpected_ = stripCount;

    // Keep the small live preview separate from the product/image list. The latter only receives
    // the completed enhanced product through imageReady; this temporary file exists solely because
    // imagePreviewReady carries paths for the UI to load. Show only the contiguous prefix from
    // the top of the disk, so the live image visibly grows downward. Out-of-order strips stay
    // assembled in memory and become visible when the gap above them arrives.
    if (newStrip && !gk2aStrips_.empty()) {
        int contiguousStrips = 0;
        while (gk2aStrips_.find(contiguousStrips) != gk2aStrips_.end())
            ++contiguousStrips;
        const int height = std::min(contiguousStrips * strip.height(), gk2aDisk_.height());
        if (height > 0) {
            const QImage preview = gk2aDisk_.copy(0, 0, gk2aDisk_.width(), height)
                .scaled(480, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            const QString previewDir = QDir(outputRoot_).filePath(QStringLiteral("live_preview"));
            if (QDir().mkpath(previewDir)) {
                const QString previewPath = QDir(previewDir).filePath(QStringLiteral("GK2A_IR.png"));
                if (preview.save(previewPath))
                    emit imagePreviewReady(qstamp, QList<QString>{previewPath});
            }
        }
    }
    // And when the disk is complete, it is handed over straight away rather than waiting for the
    // next observation to push it out.
    if (stripCount > 0 && static_cast<int>(gk2aStrips_.size()) >= stripCount)
        flushGk2aObservation(true);
    nadirstation::log::write(QStringLiteral("GK-2A: strip %1 of %2, %3 strips assembled")
        .arg(index + 1).arg(stripCount).arg(gk2aStrips_.size()));
}

void NadirStationDecoder::processBitsWithDeframer(const std::vector<uint8_t>& bits,
                                         const std::vector<uint8_t>& reliability,
                                         SvissrFrameLocker& locker,
                                         const std::vector<float>* soft) {
    if (bits.empty()) return;

    const auto tLock = std::chrono::steady_clock::now();
    const auto frames = locker.consume(bits, reliability, soft);
    addStageTime(1, tLock);

    bool anyAccepted = false;
    for (const auto& f : frames) {
        Line l;
        QString d;
        const auto tFrame = std::chrono::steady_clock::now();
        const bool frameOk = decodeFrame(f, l, &d);
        addStageTime(2, tFrame);
        if (frameOk) {
            anyAccepted = true;
            lastGoodSyncSymbol_ = std::max(lastGoodSyncSymbol_, f.syncSymbolPosition);
            polarityHint_ = 0;
            if (l.pictureFlag == 0xff && !pictureCycleActive_) {
                startPictureCycle(l);
                pictureCycleActive_ = true;
            }
            if (l.pictureFlag == 0 && pictureCycleActive_) {
                ++pictureOffFrameCount_;
                if (pictureOffFrameCount_ <= 3) {
                    nadirstation::log::write(QStringLiteral("picture OFF candidate %1/3 line=%2%3 scan=%4 reset=%5")
                        .arg(pictureOffFrameCount_).arg(l.lineNumber)
                        .arg(l.lineNumberEstimated ? QStringLiteral("(estimated)") : QString())
                        .arg(l.scanCount < 0 ? QStringLiteral("unavailable") : QString::number(l.scanCount))
                        .arg(l.resetLine < 0 ? QStringLiteral("unavailable") : QString::number(l.resetLine)));
                }
            } else {
                pictureOffFrameCount_ = 0;
            }
            // Some valid DOC blocks carry FFFF in the BCD scan counter. A
            // stable OFF flag after an ON cycle is enough to finalize; require
            // three consecutive accepted DOC frames to reject a one-frame glitch.
            const bool pictureEnded = pictureCycleActive_ && l.pictureFlag == 0 &&
                pictureOffFrameCount_ >= 3;
            if (pictureEnded) {
                ++goodFrames_;
                lastGoodFrameMs_ = QDateTime::currentMSecsSinceEpoch();
                nadirstation::log::write(QStringLiteral("picture OFF confirmed; finalizing at line=%1 scan=%2 reset=%3")
                    .arg(l.lineNumber)
                    .arg(l.scanCount < 0 ? QStringLiteral("unavailable") : QString::number(l.scanCount))
                    .arg(l.resetLine < 0 ? QStringLiteral("unavailable") : QString::number(l.resetLine)));
                finishPictureCycle();
                continue; // The OFF/reset line belongs to the gap between images.
            }
            // A repeat is the same frame seen twice, which the sync position identifies. The line
            // number is no good for this since renumberLinesBySync(): the stored lines carry the
            // renumbered value and this one still the raw DOC reading, so real new lines were
            // counted as repeats (6 of 72 frames reported at 2.5 dB although 71 decoded).
            const bool duplicateLine = std::any_of(
                recentLines_.begin(), recentLines_.end(),
                [&](const Line& oldLine) {
                    return l.syncSymbolPosition != 0
                        ? oldLine.syncSymbolPosition == l.syncSymbolPosition
                        : oldLine.lineNumber == l.lineNumber;
                });
            if (!duplicateLine) ++goodFrames_;
            feedLine(std::move(l));
            emit statusMessage(QStringLiteral("S-VISSR lock: %1").arg(d));
            nadirstation::log::write(QStringLiteral("ACCEPT %1").arg(d));
        } else {
            ++badFrames_;
            nadirstation::log::write(QStringLiteral("REJECT frame: %1").arg(d));
        }
    }

    // Sync state for the badge: 100 = a line passed its checks in the last few seconds (green),
    // 50 = the deframer holds the sync marker but lines are failing (yellow), 0 = searching (red).
    // It used to be raised and never lowered, so after a fade the badge kept saying LOCK.
    // The hold is counted in the signal's own symbols, five scan lines, not in wall-clock time:
    // a file played back faster than real time covers many lines in three seconds.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const double holdSymbols = 5.0 * lineSymbolPeriodEstimate_;
    const uint64_t here = locker.symbolsSeen();
    if (anyAccepted) {
        lock_ = 100;
        lastGoodFrameMs_ = nowMs;
    } else if (lastGoodSyncSymbol_ != 0 && here >= lastGoodSyncSymbol_ &&
               static_cast<double>(here - lastGoodSyncSymbol_) <= holdSymbols) {
        lock_ = 100;
    } else if (locker.locked()) {
        lock_ = 50;
    } else {
        lock_ = 0;
    }
}

void NadirStationDecoder::startPictureCycle(Line& firstLine) {
    // The DOC Picture Flag marks the meaningful image interval. Drop any
    // leading off-picture scan rows while retaining the live demod and frame locker.
    const bool newSession = sessionDir_.isEmpty() || observationFinalized_;
    recentLines_.clear();
    sequenceLineNumber_ = 1;
    lastBuiltLine_ = 0;
    lastReportedLine_ = 0;
    lineNumberOffset_ = 0;
    docCounterSeen_ = false;
    docLineContinuityWindow_.clear();
    lastReliableDocSyncSymbolPosition_ = 0;
    lastReliableDocLineNumber_ = -1;
    // decodeFrame() has already assigned a line number before we discover the
    // Picture Flag. That number can belong to the preceding off-picture gap
    // (e.g. ~1930), while the next frame starts from 1 after this reset. Put
    // this boundary frame at row 1; subsequent rows advance from the measured
    // PN-sync intervals, so an early DOC count cannot shift the image origin.
    firstLine.lineNumber = 1;
    firstLine.lineNumberEstimated = true;
    sequenceLineNumber_ = 2;
    lastReportedLine_ = 1;
    // The boundary frame is a local row-1 anchor only. Do not derive the DOC
    // offset from it; low-SNR damage to this single counter must not shift the
    // rest of the image. Later rows are numbered by accumulated PN-sync gaps.
    lastAcceptedSyncSymbolPosition_ = firstLine.syncSymbolPosition;
    lineSymbolPeriodEstimate_ = 401408.0;
    lineSymbolPeriodHistory_.clear();
    lineSymbolPeriodObservations_ = 0;
    pictureOffFrameCount_ = 0;
    if (newSession) beginObservation();
    observationFinalized_ = false;
    nadirstation::log::write(QStringLiteral("picture flag ON; started image cycle in %1").arg(sessionDir_));
    emit statusMessage(QStringLiteral("New image started: DOC Picture Flag ON; reception continues."));
}

void NadirStationDecoder::finishPictureCycle() {
    // This closes the image/session only. Pluto capture and the continuous
    // demodulator stay active so the following scan cycle can be acquired.
    //
    // Building the final products (line repair, calibration, colour tables, regional products, a
    // dozen large PNGs) takes 30-60 s on the decoder thread. With backpressure on, the receiver was
    // held for all of it (44 s in the 2026-11-15 FY-2H log), the Pluto's buffers overflowed and the
    // hang watchdog wrote a dump. The picture has ended, so what arrives meanwhile is the retrace gap:
    // the receiver is let run (the queue keeps the newest 16 M samples and drops older ones) and the
    // watchdog is told this is not a hang.
    const unsigned epoch = ingressBackpressureEpoch_.load();
    const bool heldReceiver = ingressBackpressureEnabled_.exchange(false, std::memory_order_acq_rel);
    if (heldReceiver) ingressCv_.notify_all();
    nadirstation::log::setArmed(1, false);
    const auto finalStart = std::chrono::steady_clock::now();
    tryCompleteImage(true);
    const auto finalMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - finalStart).count();
    nadirstation::log::write(QStringLiteral("image products built in %1 s (receiver kept running)").arg(finalMs / 1000.0, 0, 'f', 1));
    if (enabled_.load() && !stopping_.load()) {
        nadirstation::log::setArmed(1, true);
        // Restore the hold only if nobody set it meanwhile (the GUI turns it off on Stop).
        if (heldReceiver && ingressBackpressureEpoch_.load() == epoch) ingressBackpressureEnabled_.store(true, std::memory_order_release);
    }
    observationFinalized_ = true;
    pictureCycleActive_ = false;
    recentLines_.clear();
    sequenceLineNumber_ = 1;
    lastBuiltLine_ = 0;
    lastReportedLine_ = 0;
    lineNumberOffset_ = 0;
    docCounterSeen_ = false;
    docLineContinuityWindow_.clear();
    lastReliableDocSyncSymbolPosition_ = 0;
    lastReliableDocLineNumber_ = -1;
    lastAcceptedSyncSymbolPosition_ = 0;
    lineSymbolPeriodEstimate_ = 401408.0;
    lineSymbolPeriodHistory_.clear();
    lineSymbolPeriodObservations_ = 0;
    emit statusMessage(QStringLiteral("Picture Flag ended: the image was saved; receiving and waiting for the next one."));
    resetManamAssembly();
    // Calibration-2 is a repeated telemetry table and can finish after the
    // image cycle. Keep its assembled groups for the rest of this receive run.
}

void NadirStationDecoder::resetManamAssembly() {
    manamRevision_ = -1;
    manamGroupCount_ = 0;
    manamGroupMask_ = 0;
    lastCompleteManam_.clear();
    manamChunkSeen_.fill(false);
    manamChunkScores_.fill(-1);
    for (auto& chunk : manamChunks_) chunk.clear();
    emit manamReset();
    nadirstation::log::write(QStringLiteral("MANAM assembly reset after image cycle finalization"));
}

void NadirStationDecoder::resetCalibrationAssembly() {
    for (auto& group : calibrationCopies_)
        for (auto& repeat : group) repeat.clear();
    for (auto& group : calibrationCopySeen_) group.fill(false);
    calibrationCopyCounts_.fill(0);
    for (auto& lut : calibrationLuts_) lut.clear();
    calibrationBlock_.clear();
    calibrationGroupsSeen_ = 0;
    calibrationLutReady_ = false;
    calibrationSource_.clear();
    emit calibration2Reset();
    // A new observation starts by trying to receive the live table, but a table
    // is always made available so a low-SNR pass can still produce brightness
    // temperatures. loadFallbackCalibration() prefers a block that was validated
    // in an earlier run and falls back to the copy bundled with the station.
    loadFallbackCalibration();
}

QString NadirStationDecoder::calibrationBackupPath() const {
    if (outputRoot_.isEmpty()) return {};
    return QDir(outputRoot_).filePath(QStringLiteral("FY2_Calibration2_validated.bin"));
}

void NadirStationDecoder::rememberValidatedCalibrationBlock(const QByteArray& block) {
    constexpr int expectedBytes = 25 * 1024;
    if (block.size() != expectedBytes) return;
    validatedCalibrationBlock_ = block;
    const QString path = calibrationBackupPath();
    if (path.isEmpty()) return;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(block);
    file.close();
}

bool NadirStationDecoder::loadFallbackCalibration() {
    constexpr int expectedBytes = 25 * 1024;
    QStringList candidates;
    const QString userBackup = calibrationBackupPath();
    if (!userBackup.isEmpty()) candidates << userBackup;
    // Factory copy shipped with the station, used when the user has no validated
    // block yet and the live table could not be received.
    candidates << QCoreApplication::applicationDirPath() +
        QStringLiteral("/resources/orbits/fy2_calibration2_backup.bin");

    for (int index = 0; index < candidates.size(); ++index) {
        QFile file(candidates.at(index));
        if (!file.open(QIODevice::ReadOnly)) continue;
        const QByteArray block = file.readAll();
        file.close();
        if (block.size() != expectedBytes) continue;
        std::array<QVector<float>, 5> decodedLuts;
        if (!nadirstation::decodeCalibration2(block, decodedLuts)) continue;
        calibrationLuts_ = std::move(decodedLuts);
        calibrationBlock_ = block;
        validatedCalibrationBlock_ = block;
        // calibrationGroupsSeen_ is the count actually received this observation
        // and is reported as such; the fallback marks readiness, not reception,
        // so the reported provenance stays honest.
        calibrationLutReady_ = true;
        calibrationSource_ = index == 0 ? QStringLiteral("backup") : QStringLiteral("factory");
        nadirstation::log::write(QStringLiteral(
            "FY-2 Calibration-2 LUT loaded from %1 (%2); brightness temperatures available "
            "even if the live 25-group table is not received")
            .arg(calibrationSource_, candidates.at(index)));
        emit statusMessage(calibrationSource_ == QStringLiteral("backup")
            ? QStringLiteral("FY-2 IR calibration table: using this station's validated backup.")
            : QStringLiteral("FY-2 IR calibration table: using the built-in copy (25/25 groups not yet received)."));
        return true;
    }
    return false;
}

void NadirStationDecoder::writeCalibrationProductsToSession() {
    if (!calibrationLutReady_ || !observationFinalized_ || sessionDir_.isEmpty()) return;
    QDir dir(sessionDir_);
    const std::array<QString, 4> thermalNames{
        QStringLiteral("CH4_IR1_TBB_0p01K.png"), QStringLiteral("CH5_IR2_TBB_0p01K.png"),
        QStringLiteral("CH3_IR3_TBB_0p01K.png"), QStringLiteral("CH2_IR4_TBB_0p01K.png")};
    const std::array<QString, 4> displayNames{
        QStringLiteral("CH4_IR1_TBB_Enhanced.png"), QStringLiteral("CH5_IR2_TBB_Enhanced.png"),
        QStringLiteral("CH3_IR3_TBB_Enhanced.png"), QStringLiteral("CH2_IR4_TBB_Enhanced.png")};
    QStringList paths;
    QFile manifestFile(observationManifestPath(dir));
    QJsonObject manifest;
    if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text))
        manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    manifestFile.close();
    for (const QJsonValue& value : manifest.value(QStringLiteral("products")).toArray()) {
        const QString path = value.toObject().value(QStringLiteral("absolute_path")).toString();
        if (!path.isEmpty() && QFileInfo::exists(path)) paths.append(path);
    }

    auto addProduct = [&](const QString& name) {
        const QString path = dir.filePath(name);
        if (!paths.contains(path)) paths.append(path);
        QJsonObject item;
        item[QStringLiteral("path")] = name;
        item[QStringLiteral("absolute_path")] = QFileInfo(path).absoluteFilePath();
        QJsonArray products = manifest.value(QStringLiteral("products")).toArray();
        bool present = false;
        for (const QJsonValue& value : products)
            if (value.toObject().value(QStringLiteral("path")).toString() == name) present = true;
        if (!present) products.append(item);
        manifest[QStringLiteral("products")] = products;
    };

    int generated = 0;
    for (int channel = 0; channel < 4; ++channel) {
        QImage counts = finalizedIrCounts10_[static_cast<std::size_t>(channel)];
        if (counts.isNull()) continue;
        counts = counts.convertToFormat(QImage::Format_Grayscale16);
        QImage tbb(counts.size(), QImage::Format_Grayscale16);
        QImage display(counts.size(), QImage::Format_Grayscale8);
        tbb.fill(0);
        display.fill(0);
        const auto& lut = calibrationLuts_[static_cast<std::size_t>(channel + 1)];
        for (int y = 0; y < counts.height(); ++y) {
            const auto* source = reinterpret_cast<const quint16*>(counts.constScanLine(y));
            auto* temperatures = reinterpret_cast<quint16*>(tbb.scanLine(y));
            uchar* shown = display.scanLine(y);
            for (int x = 0; x < counts.width(); ++x) {
                const int dn = source[x];
                if (dn >= lut.size()) continue;
                const float kelvin = lut[dn];
                if (!std::isfinite(kelvin) || kelvin < 100.0f || kelvin > 500.0f) continue;
                temperatures[x] = static_cast<quint16>(qRound(kelvin * 100.0f));
                shown[x] = static_cast<uchar>(qBound(0,
                    qRound((330.0f - kelvin) * 255.0f / 150.0f), 255));
            }
        }
        tbb = nadirstation::products::repairMissingLinesMedian(tbb, 2);
        // This in-memory calibration raster uses 0 as its unfilled-pixel
        // sentinel; unlike published regional rasters it does not use 65535.
        tbb = nadirstation::products::medianBlurPreservingFormat(tbb, 3, nullptr, false, 0);
        display = nadirstation::products::repairMissingLines(display, 8);
        display = nadirstation::products::medianBlurPreservingFormat(display, 3);
        if (savePng(tbb, dir.filePath(thermalNames[static_cast<std::size_t>(channel)]))) {
            addProduct(thermalNames[static_cast<std::size_t>(channel)]);
            ++generated;
        }
        if (savePng(display, dir.filePath(displayNames[static_cast<std::size_t>(channel)]))) {
            addProduct(displayNames[static_cast<std::size_t>(channel)]);
            ++generated;
        }
        if (channel == 0) {
            const QString colorName = QStringLiteral("CH4_IR1_Longwave_ColorEnhanced.png");
            const QImage color = nadirstation::products::longwaveIrColorEnhancement(tbb);
            if (savePng(color, dir.filePath(colorName))) addProduct(colorName);
        }
    }
    if (generated == 0) return;

    QFile rawCalibration(dir.filePath(QStringLiteral("FY2_Calibration2.bin")));
    if (rawCalibration.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        rawCalibration.write(calibrationBlock_);
        rawCalibration.close();
    }
    QJsonObject calibration;
    calibration[QStringLiteral("ready")] = true;
    calibration[QStringLiteral("groups_received")] = calibrationGroupsSeen_;
    calibration[QStringLiteral("table_source")] = calibrationSource_;
    calibration[QStringLiteral("groups_expected")] = 25;
    calibration[QStringLiteral("infrared_unit")] = QStringLiteral("K");
    calibration[QStringLiteral("temperature_encoding")] = QStringLiteral("uint16 grayscale; value in 0.01 K, zero is no data");
    manifest[QStringLiteral("radiometric_calibration")] = calibration;
    manifestFile.setFileName(dir.filePath(QStringLiteral("FY2_Observation.json")));
    if (manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        manifestFile.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
        manifestFile.close();
        QFile::remove(dir.filePath(QStringLiteral("MGAS_Observation.json")));
    }
    QFile calibrationJson(dir.filePath(QStringLiteral("FY2_RadiometricCalibration.json")));
    if (calibrationJson.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QJsonObject lutDocument;
        lutDocument[QStringLiteral("groups_received")] = calibrationGroupsSeen_;
        lutDocument[QStringLiteral("table_source")] = calibrationSource_;
        lutDocument[QStringLiteral("groups_expected")] = 25;
        lutDocument[QStringLiteral("infrared_unit")] = QStringLiteral("K");
        QJsonArray allLuts;
        for (const auto& lut : calibrationLuts_) {
            QJsonArray values;
            for (float value : lut) values.append(value);
            allLuts.append(values);
        }
        lutDocument[QStringLiteral("luts_vis_ir1_ir2_ir3_ir4")] = allLuts;
        calibrationJson.write(QJsonDocument(lutDocument).toJson(QJsonDocument::Indented));
        calibrationJson.close();
    }
    nadirstation::log::write(QStringLiteral("late Calibration-2 arrived; wrote %1 TBB/display rasters from preserved counts").arg(generated));
    emit imageReady(QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs), paths);
}

void NadirStationDecoder::feedLine(Line&& line) {
    if (!line.valid || line.lineNumber < 1 || line.lineNumber > SVISSR_SCAN_LINES) return;
    const bool logLine = (line.lineNumber % 25 == 0);
    if (logLine) {
        nadirstation::log::write(QStringLiteral("feedLine line=%1 recentBefore=%2").arg(line.lineNumber).arg(recentLines_.size()));
    }
    recentLines_.push_back(std::move(line));
    // Every line is placed by its PN-sync time against a line number agreed by the DOC counters
    // (renumberLinesBySync), then the list is sorted and one line kept per number.
    renumberLinesBySync();
    if (recentLines_.size() > SVISSR_SCAN_LINES + 16)
        recentLines_.erase(recentLines_.begin(), recentLines_.begin() + static_cast<std::ptrdiff_t>(recentLines_.size() - SVISSR_SCAN_LINES));

    // Keep real-time work tiny: only a downscaled preview is built while receiving.
    // Full-size CH products are generated once, when decoding is stopped or the
    // complete 2291-line scan has arrived.
    tryCompleteImage(false);
}

void NadirStationDecoder::renumberLinesBySync() {
    // Reconcile accumulated row numbers against a rolling PN-sync grid. Each
    // accepted frame contributes a vote for the grid origin; a single bad
    // early interval cannot shift the entire image, and CRC-valid DOC counter
    // deltas help refine the rolling line-period estimate in parseFrame().
    const auto quality = [](const Line& l) {
        // lineNumber is cycle-relative after Picture Flag ON; reportedDocLineNumber
        // remains the original telemetry counter and can therefore differ by design.
        const bool hasDocCounter = l.documentCounterValid && l.lineNumber >= 1;
        return (hasDocCounter ? 100000 : 0) + l.crcGood * 100 + (l.lineNumberEstimated ? 0 : 1);
    };
    const auto sortAndDedupe = [&] {
        std::stable_sort(recentLines_.begin(), recentLines_.end(), [&](const Line& a, const Line& b) {
            if (a.lineNumber != b.lineNumber) return a.lineNumber < b.lineNumber;
            return quality(a) > quality(b);
        });
        recentLines_.erase(std::unique(recentLines_.begin(), recentLines_.end(), [](const Line& a, const Line& b) {
            return a.lineNumber == b.lineNumber;
        }), recentLines_.end());
    };
    const double period = lineSymbolPeriodEstimate_;
    if (recentLines_.size() < 2 || lineSymbolPeriodObservations_ < 3 || !(period > 300000.0 && period < 500000.0)) {
        sortAndDedupe();
        return;
    }
    uint64_t ref = std::numeric_limits<uint64_t>::max();
    for (const auto& l : recentLines_)
        if (l.syncSymbolPosition > 0) ref = std::min(ref, l.syncSymbolPosition);
    if (ref == std::numeric_limits<uint64_t>::max()) { sortAndDedupe(); return; }

    std::vector<long long> steps(recentLines_.size(), -1);
    std::map<int, int> votes;
    for (std::size_t i = 0; i < recentLines_.size(); ++i) {
        const auto& l = recentLines_[i];
        if (l.syncSymbolPosition < ref) continue;
        const double d = static_cast<double>(l.syncSymbolPosition - ref) / period;
        const long long k = std::llround(d);
        if (std::fabs(d - static_cast<double>(k)) > 0.05) continue;   // not on the line grid
        steps[i] = k;
        if (l.lineNumber >= 1 && l.lineNumber <= SVISSR_SCAN_LINES)
            ++votes[l.lineNumber - static_cast<int>(k)];
    }
    int n0 = 0, best = 0, total = 0;
    for (const auto& [value, count] : votes) {
        total += count;
        if (count > best) { best = count; n0 = value; }
    }
    // Require a multi-frame consensus before re-numbering a row on the grid.
    if (best < 3) { sortAndDedupe(); return; }

    long long minNumber = std::numeric_limits<long long>::max();
    for (std::size_t i = 0; i < recentLines_.size(); ++i)
        if (steps[i] >= 0) minNumber = std::min(minNumber, n0 + steps[i]);
    // Frames before the counter starts (black space ahead of the Earth scan) keep their order
    // ahead of line 1, as the sequential numbering did.
    const long long offset = minNumber < 1 ? 1 - minNumber : 0;
    for (std::size_t i = 0; i < recentLines_.size(); ++i) {
        if (steps[i] < 0) continue;
        auto& l = recentLines_[i];
        const long long number = n0 + steps[i] + offset;
        l.lineNumber = static_cast<int>(std::clamp<long long>(number, 1, SVISSR_SCAN_LINES + 1));
        l.lineNumberEstimated = offset != 0 || !(l.documentCounterValid && l.reportedDocLineNumber == l.lineNumber);
    }
    recentLines_.erase(std::remove_if(recentLines_.begin(), recentLines_.end(), [](const Line& l) {
        return l.lineNumber < 1 || l.lineNumber > SVISSR_SCAN_LINES;
    }), recentLines_.end());
    sortAndDedupe();
}

bool NadirStationDecoder::hasCompleteDiskCoverage() const {
    if (recentLines_.empty()) return false;
    std::vector<bool> docLines(static_cast<std::size_t>(SVISSR_SCAN_LINES + 1), false);
    for (const auto& line : recentLines_) {
        if (line.documentCounterValid && line.reportedDocLineNumber >= 1 &&
            line.reportedDocLineNumber <= SVISSR_SCAN_LINES)
            docLines[static_cast<std::size_t>(line.reportedDocLineNumber)] = true;
    }
    int docLineCount = 0;
    for (int line = 1; line <= SVISSR_SCAN_LINES; ++line)
        if (docLines[static_cast<std::size_t>(line)]) ++docLineCount;
    return docLines[1] && docLines[static_cast<std::size_t>(SVISSR_SCAN_LINES)] &&
        docLineCount >= (SVISSR_SCAN_LINES * 9) / 10;
}

QString NadirStationDecoder::writeSessionManifest(bool fullDisk, const QList<QString>& paths, int minLine, int maxLine,
                                         int restoredScanLines, int repairedBlankRows,
                                         int repairedImpulsePixels) const {
    if (sessionDir_.isEmpty()) return {};
    int crcGoodCount = 0;
    int crcTotalCount = 0;
    int correctedBitCount = 0;
    int estimatedLineCount = 0;
    for (const auto& line : recentLines_) {
        crcGoodCount += line.crcGood;
        crcTotalCount += line.crcTotal;
        correctedBitCount += line.crcCorrected;
        estimatedLineCount += line.lineNumberEstimated ? 1 : 0;
    }
    QJsonObject root;
    root["format"] = QStringLiteral("NadirStation-Session-1");
    const QString spacecraft = spacecraftFolder();
    root["satellite"] = spacecraft;
    root["instrument"] = spacecraft == QStringLiteral("FY-2H")
        ? QStringLiteral("FY-2H S-VISSR")
        : (spacecraft == QStringLiteral("GK-2A")
            ? QStringLiteral("GK-2A AMI") : QStringLiteral("FY-2G S-VISSR"));
    root["created_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    if (spacecraft != QStringLiteral("GK-2A")) {
        QJsonArray scanLineTimes;
        QDateTime firstLineTime, lastLineTime;
        for (const auto& line : recentLines_) {
            if (!line.time.isValid()) continue;
            if (!firstLineTime.isValid() || line.time < firstLineTime) firstLineTime = line.time;
            if (!lastLineTime.isValid() || line.time > lastLineTime) lastLineTime = line.time;
            QJsonObject row;
            row["scan_line_number"] = line.lineNumber;
            row["source_row_zero_based"] = line.lineNumber - 1;
            row["line_time_utc"] = line.time.toUTC().toString(Qt::ISODateWithMs);
            row["line_number_estimated"] = line.lineNumberEstimated;
            if (line.documentCounterValid)
                row["reported_doc_line_number"] = line.reportedDocLineNumber;
            else
                row["reported_doc_line_number"] = QJsonValue(QJsonValue::Null);
            scanLineTimes.append(row);
        }
        root["observation_start_utc"] = firstLineTime.isValid()
            ? QJsonValue(firstLineTime.toUTC().toString(Qt::ISODateWithMs))
            : QJsonValue(QJsonValue::Null);
        root["observation_end_utc"] = lastLineTime.isValid()
            ? QJsonValue(lastLineTime.toUTC().toString(Qt::ISODateWithMs))
            : QJsonValue(QJsonValue::Null);
        root["scan_line_time_basis"] = QStringLiteral(
            "Per-scan-line UTC from the DOC S/C status block; each record is keyed by the finalized source scan-line number.");
        root["scan_line_times"] = scanLineTimes;
    }
    root["source"] = sourceLabel_;
    root["sample_rate_hz"] = sampleRate_;
    root["complete_disk"] = fullDisk;
    root["scan_lines_expected"] = SVISSR_SCAN_LINES;
    root["scan_line_min"] = minLine;
    root["scan_line_max"] = maxLine;
    root["scan_lines_received"] = static_cast<qint64>(recentLines_.size());
    root["doc_lines_estimated"] = estimatedLineCount;
    root["sector_crc_good"] = crcGoodCount;
    root["sector_crc_total"] = crcTotalCount;
    root["sector_crc_corrected_bits"] = correctedBitCount;
    root["sector_crc_policy"] = QStringLiteral("CRC-16/0x1021 over sector ID and byte-packed payload; non-byte-aligned payloads are zero-padded before CRC. CRC is an integrity metric. Payload acceptance allows sector-ID Hamming distance <= %1 and <= %2 errors in the 2048-bit zero filler; at least %3/12 sectors, including DOC, must meet these checks.")
        .arg(MAX_SECTOR_ID_BIT_ERRORS).arg(MAX_SECTOR_FILLER_ERRORS).arg(MIN_TRUSTED_SECTORS);
    QJsonObject radiometricCalibration;
    radiometricCalibration["ready"] = calibrationLutReady_;
    radiometricCalibration["groups_received"] = calibrationGroupsSeen_;
    radiometricCalibration["groups_expected"] = 25;
    radiometricCalibration["table_source"] = calibrationSource_.isEmpty()
        ? QStringLiteral("none") : calibrationSource_;
    radiometricCalibration["table_source_meaning"] = QStringLiteral(
        "live = all 25 DOC Calibration-2 groups received this observation; "
        "backup = a block validated in an earlier run and stored beside the session output; "
        "factory = the copy bundled with the station. The tables are instrument constants, so a "
        "fallback gives identical radiometry; it exists so a low-SNR pass that loses part of the "
        "telemetry stream can still produce brightness temperatures.");
    radiometricCalibration["source"] = QStringLiteral("DOC Calibration-2 groups with accepted ID/filler checks; repeated copies bit-majority combined and LUTs physically validated");
    radiometricCalibration["infrared_unit"] = QStringLiteral("K");
    radiometricCalibration["raw_counts_storage"] = QStringLiteral("decoded 10-bit counts are retained in memory only for TBB calibration; no Counts10 PNG products are written");
    radiometricCalibration["temperature_encoding"] = QStringLiteral("16-bit grayscale PNG, unsigned value = brightness temperature in 0.01 K; 0 means no data");
    if (calibrationLutReady_) {
        radiometricCalibration["raw_block_file"] = QStringLiteral("FY2_Calibration2.bin");
        radiometricCalibration["lut_file"] = QStringLiteral("FY2_RadiometricCalibration.json");
        radiometricCalibration["temperature_products"] = QStringLiteral("CH2/CH3/CH4/CH5 TBB, numeric and display-enhanced");
    } else {
        radiometricCalibration["note"] = QStringLiteral("No TBB is written unless all 25 Calibration-2 groups are collected and the decoded thermal LUTs pass physical-range checks.");
    }
    root["radiometric_calibration"] = radiometricCalibration;
    QJsonObject qualityRepair;
    qualityRepair["missing_scan_lines_interpolated"] = restoredScanLines;
    qualityRepair["additional_scanline_anomalies_interpolated"] = repairedBlankRows;
    qualityRepair["impulse_pixels_repaired"] = repairedImpulsePixels;
    qualityRepair["line_method"] = QStringLiteral("Interpolate near-blank rows (>=98% sampled zeros), clear row-wide anomalies (adjacent rows agree within 8 DN for >=90% of samples, median residual >=18 DN, >=75% same-sign residuals), and isolated stripes checked against rows two scans away (>=85% agreement, >=10 DN median residual); maximum gap 8 rows.");
    qualityRepair["impulse_method"] = QStringLiteral("3x3 Median Blur: replace every sample with the median of the center-inclusive 3x3 neighborhood (replicated borders).");
    qualityRepair["saved_channels"] = QStringLiteral("CH1_VIS and CH2-CH5 are saved after line and impulse repair; source telemetry bits remain unchanged in decoder memory.");
    qualityRepair["native_ir_counts"] = QStringLiteral("decoded 10-bit, inversion-corrected CH2-CH5 counts are retained in memory for calibration and are not emitted as PNG products.");
    root["quality_repair"] = qualityRepair;

    QJsonArray products;
    const QString base = QDir(sessionDir_).absolutePath();
    for (const QString& p : paths) {
        QJsonObject item;
        item["path"] = QDir(base).relativeFilePath(QFileInfo(p).absoluteFilePath());
        item["absolute_path"] = QFileInfo(p).absoluteFilePath();
        products.append(item);
    }
    root["products"] = products;

    QJsonObject dims;
    dims["ir_width"] = SVISSR_IR_SAMPLES;
    dims["vis_width"] = SVISSR_VIS_SAMPLES;
    dims["ir_height"] = (fullDisk ? SVISSR_SCAN_LINES : std::max(1, maxLine - minLine + 1));
    dims["vis_height"] = dims["ir_height"].toInt() * 4;
    root["dimensions"] = dims;

    const QString manifest = QDir(sessionDir_).filePath(QStringLiteral("FY2_Observation.json"));
    QFile out(manifest);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) return {};
    out.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    out.close();
    return manifest;
}

void NadirStationDecoder::tryCompleteImage(bool finalPass) {
    if (recentLines_.empty()) return;
    if (finalPass && observationFinalized_) return;
    // Quicklook images are only meaningful after DOC Picture Flag ON begins
    // an observation. Decoded scan lines in the intervening OFF state must not
    // create or publish an image preview.
    if (!finalPass && !pictureCycleActive_) return;

    const int minLine = recentLines_.front().lineNumber;
    const int maxLine = recentLines_.back().lineNumber;
    constexpr int PREVIEW_WIDTH = 480;
    constexpr int QUICKLOOK_STEP = 1;

    if (!finalPass) {
        if (maxLine <= lastBuiltLine_) return;
        if (maxLine < SVISSR_SCAN_LINES && (maxLine - lastBuiltLine_) < QUICKLOOK_STEP && lastBuiltLine_ > 0) return;

        // Keep a full-disk square canvas and place every quicklook row at its
        // actual scan position. Since FY-2 scans north-to-south, the circular
        // Earth disk appears from its top limb and fills downward over time.
        QImage preview(PREVIEW_WIDTH, PREVIEW_WIDTH, QImage::Format_Grayscale8);
        preview.fill(0);
        const double diskCenter = (PREVIEW_WIDTH - 1) * 0.5;
        const double diskRadius = PREVIEW_WIDTH * 0.5;
        for (int py = 0; py < PREVIEW_WIDTH; ++py) {
            const double normalizedY = (py - diskCenter) / diskRadius;
            if (std::abs(normalizedY) > 1.0) continue;
            const int halfWidth = static_cast<int>(std::floor(diskRadius *
                std::sqrt(std::max(0.0, 1.0 - normalizedY * normalizedY))));
            const int left = std::max(0, static_cast<int>(std::ceil(diskCenter - halfWidth)));
            const int right = std::min(PREVIEW_WIDTH - 1, static_cast<int>(std::floor(diskCenter + halfWidth)));

            // Map the square preview to the complete 4x VIS scan-line grid.
            // Unreceived future rows stay black instead of being stretched over
            // the current image, which creates the familiar slowly filling disk.
            const int visLine = static_cast<int>(std::lround(
                static_cast<double>(py) * (SVISSR_VIS_SAMPLES - 1) / (PREVIEW_WIDTH - 1)));
            const int wantedLine = visLine / 4 + 1;
            const int sensor = visLine % 4;
            auto it = std::lower_bound(recentLines_.begin(), recentLines_.end(), wantedLine,
                [](const Line& line, int number) { return line.lineNumber < number; });
            if (it == recentLines_.end()) {
                if (recentLines_.empty()) continue;
                --it;
            } else if (it != recentLines_.begin()) {
                const auto previous = std::prev(it);
                if (std::abs(previous->lineNumber - wantedLine) <= std::abs(it->lineNumber - wantedLine))
                    it = previous;
            }
            if (std::abs(it->lineNumber - wantedLine) > 8) continue;
            const auto& vis = it->vis[static_cast<std::size_t>(sensor)];
            const int visSize = static_cast<int>(vis.size());
            if (visSize <= 0) continue;
            uchar* row = preview.scanLine(py);
            for (int x = left; x <= right; ++x) {
                const int srcX = std::clamp(static_cast<int>((static_cast<double>(x) / PREVIEW_WIDTH) * visSize), 0, visSize - 1);
                row[x] = static_cast<uchar>((vis[static_cast<std::size_t>(srcX)] * 255u) / 63u);
            }
        }

        preview = nadirstation::products::repairMissingLines(preview, 8);
        preview = nadirstation::products::medianBlurPreservingFormat(preview, 3);
        if (sessionDir_.isEmpty()) beginObservation();
        QDir dir(sessionDir_);
        dir.mkpath(".");
        const QString p = dir.filePath("Preview_CH1_VIS.png");
        if (!savePng(preview, p)) {
            emit statusMessage(QStringLiteral("Cannot save the FY-2G quick-look: %1").arg(p));
            return;
        }
        lastBuiltLine_ = maxLine;
        const QString timestamp = QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss");
        emit imagePreviewReady(timestamp, QList<QString>{p});
        if (hasCompleteDiskCoverage()) {
            nadirstation::log::write(QStringLiteral("valid DOC line coverage detected; finalizing full disk"));
            tryCompleteImage(true);
        }
        return;
    }

    try {
        // For a partial observation, crop vertically to the received line span.
        // For a complete scan the output remains the full 2291-line product.
        const int irWidth = SVISSR_IR_SAMPLES;
        const int irHeight = (maxLine >= SVISSR_SCAN_LINES && minLine == 1)
            ? SVISSR_SCAN_LINES : std::max(1, maxLine - minLine + 1);
        constexpr int calibrationImageHeight = SVISSR_SCAN_LINES;
        const int visWidth = SVISSR_VIS_SAMPLES;
        const int visHeight = irHeight * 4;
        int restoredScanLines = 0;

        QImage vis(visWidth, visHeight, QImage::Format_Grayscale8);
        QImage ir1(irWidth, irHeight, QImage::Format_Grayscale8);
        QImage ir2(irWidth, irHeight, QImage::Format_Grayscale8);
        QImage ir3(irWidth, irHeight, QImage::Format_Grayscale8);
        QImage ir4(irWidth, irHeight, QImage::Format_Grayscale8);
        std::array<QImage, 4> irCounts10;
        for (auto& counts : irCounts10) {
            // Radiometric count rasters always retain the native scan-line
            // coordinate system, including for partial captures. This keeps
            // later geostationary/equirectangular projection from treating a
            // cropped line N as if it were full-disk line 1.
            counts = QImage(irWidth, calibrationImageHeight, QImage::Format_Grayscale16);
            counts.fill(65535);
        }
        vis.fill(0); ir1.fill(0); ir2.fill(0); ir3.fill(0); ir4.fill(0);

        for (const auto& line : recentLines_) {
            const int y = (irHeight == SVISSR_SCAN_LINES) ? line.lineNumber - 1 : line.lineNumber - minLine;
            if (y < 0 || y >= irHeight) continue;
            // Writing grayscale pixels through QImage::setPixel performs format
            // checks and conversion for every sample. A full-disk finalize does
            // that millions of times and can make End Decode look hung. These
            // images are Format_Grayscale8, so fill their row buffers directly.
            uchar* irRows[4] = {ir1.scanLine(y), ir2.scanLine(y), ir3.scanLine(y), ir4.scanLine(y)};
            const int countY = line.lineNumber - 1;
            if (countY < 0 || countY >= calibrationImageHeight) continue;
            for (int channel = 0; channel < 4; ++channel) {
                const auto& samples = line.ir[channel];
                const int count = std::min(irWidth, static_cast<int>(samples.size()));
                auto* rawCounts = reinterpret_cast<quint16*>(irCounts10[static_cast<std::size_t>(channel)].scanLine(countY));
                for (int x = 0; x < count; ++x) {
                    if (line.irValid[static_cast<std::size_t>(channel)]) {
                        rawCounts[x] = samples[x];
                        irRows[channel][x] = static_cast<uchar>((samples[x] * 255u) / 1023u);
                    } else {
                        rawCounts[x] = 65535;
                        irRows[channel][x] = 0;
                    }
                }
            }
            const int visY = y * 4;
            for (int sensor = 0; sensor < 4; ++sensor) {
                if (visY + sensor >= visHeight) continue;
                uchar* row = vis.scanLine(visY + sensor);
                const auto& samples = line.vis[sensor];
                const int count = std::min(visWidth, static_cast<int>(samples.size()));
                for (int x = 0; x < count; ++x)
                    row[x] = static_cast<uchar>((samples[x] * 255u) / 63u);
            }
        }
        finalizedIrCounts10_ = irCounts10;

        // Row repair runs first, on the lines as received: a scan line that arrived blank or full of
        // impulse noise is rebuilt from good neighbours, and the lines that never arrived (still
        // blank here) are rebuilt in the same pass when the gap is short. Only then does the longer
        // gap fill below look for the nearest rows that are actually good.
        std::atomic<int> repairedBlankRows{0};
        std::atomic<int> repairedImpulsePixels{0};
        // The 10-bit count rasters feed every temperature product, so the rows the repair rebuilt in
        // the 8-bit picture are rebuilt in the counts too (linear blend of the nearest good rows).
        const int countRowOffset = (irHeight == SVISSR_SCAN_LINES) ? 0 : minLine - 1;
        auto repairSavedChannel = [&](QImage& channel, int maxGapRows, QImage* counts = nullptr) {
            int blankRows = 0;
            int impulsePixels = 0;
            int noisyRows = 0;
            const QImage before = counts ? channel : QImage();
            channel = nadirstation::products::repairMissingLines(channel, maxGapRows, &blankRows, true, &noisyRows);
            if (counts && !before.isNull() && before.size() == channel.size() && counts->width() == channel.width()) {
                const int h = channel.height(), w = channel.width();
                std::vector<uint8_t> bad(static_cast<std::size_t>(h), 0);
                for (int y = 0; y < h; ++y) {
                    const uchar* a = before.constScanLine(y);
                    const uchar* b = channel.constScanLine(y);
                    int changed = 0;
                    for (int x = 0; x < w; x += 4) changed += std::abs(int(a[x]) - int(b[x])) > 12;
                    bad[static_cast<std::size_t>(y)] = changed * 4 * 100 >= w * 25;
                }
                const int reach = 3 * maxGapRows;
                int rebuiltCounts = 0;
                for (int y = 0; y < h;) {
                    if (!bad[static_cast<std::size_t>(y)]) { ++y; continue; }
                    int y2 = y;
                    while (y2 + 1 < h && bad[static_cast<std::size_t>(y2 + 1)]) ++y2;
                    const int cy0 = y - 1 + countRowOffset, cy1 = y2 + 1 + countRowOffset;
                    if (y2 - y + 1 <= reach && y >= 1 && y2 + 1 < h && cy0 >= 0 && cy1 < counts->height()) {
                        const auto* top = reinterpret_cast<const quint16*>(counts->constScanLine(cy0));
                        const auto* bot = reinterpret_cast<const quint16*>(counts->constScanLine(cy1));
                        const uchar* chTop = channel.constScanLine(y - 1);
                        const uchar* chBot = channel.constScanLine(y2 + 1);
                        for (int yy = y; yy <= y2; ++yy) {
                            const float t = float(yy - (y - 1)) / float(cy1 - cy0);
                            auto* dst = reinterpret_cast<quint16*>(counts->scanLine(yy + countRowOffset));
                            const uchar* chRow = channel.constScanLine(yy);
                            for (int x = 0; x < w; ++x) {
                                if (top[x] > 1023 || bot[x] > 1023) { dst[x] = 65535; continue; }
                                // Linear blend of the good rows, plus what the 8-bit rebuild added to its own
                                // linear blend (edge shifts, mirrored texture, grain), in counts: the 8-bit
                                // value is counts * 255 / 1023.
                                const float linear8 = (1.0f - t) * chTop[x] + t * chBot[x];
                                const float v = (1.0f - t) * top[x] + t * bot[x] + (chRow[x] - linear8) * (1023.0f / 255.0f);
                                dst[x] = static_cast<quint16>(std::clamp(static_cast<int>(std::lround(v)), 0, 1023));
                            }
                            ++rebuiltCounts;
                        }
                    }
                    y = y2 + 1;
                }
                if (rebuiltCounts > 0)
                    nadirstation::log::write(QStringLiteral("line repair: %1 rows of 10-bit counts rebuilt").arg(rebuiltCounts));
            }
            if (noisyRows > 0)
                nadirstation::log::write(QStringLiteral("line repair: %1 noisy rows removed and rebuilt (%2 rows rebuilt in all)")
                                             .arg(noisyRows).arg(blankRows));
            channel = nadirstation::products::medianBlur(channel, 3, &impulsePixels);
            repairedBlankRows += blankRows;
            repairedImpulsePixels += impulsePixels;
        };
        // VIS has four rows a scan line: 16 rows is the same four-line limit as the IR channels.
        // NADIR_RAW_ROWS=1 leaves every row as received, for looking at what the repair is working on.
        const bool rawRows = qEnvironmentVariableIsSet("NADIR_RAW_ROWS");
        if (!rawRows) {
            // The five channels are independent (each its own picture and count raster; the
            // totals are atomic, the log is locked), so they are repaired at the same time: this
            // step alone took about 12 s at the end of an FY-2H image, most of it VIS.
            auto visJob = std::async(std::launch::async, [&] { repairSavedChannel(vis, 16); });
            auto ir1Job = std::async(std::launch::async, [&] { repairSavedChannel(ir1, 4, &irCounts10[0]); });
            auto ir2Job = std::async(std::launch::async, [&] { repairSavedChannel(ir2, 4, &irCounts10[1]); });
            repairSavedChannel(ir3, 4, &irCounts10[2]);
            repairSavedChannel(ir4, 4, &irCounts10[3]);
            visJob.get(); ir1Job.get(); ir2Job.get();
            finalizedIrCounts10_ = irCounts10;
        }

        // Missing scan lines are filled at image assembly time
        // rather than discarding the whole observation. For a
        // complete 1..2291 span, reconstruct every absent line by averaging the
        // nearest valid line above and below (or copying the single available
        // neighbour at an edge). This affects only the final image, not raw data.
        // A partial observation gets the same treatment for short gaps (up to four lines) inside
        // its received span; longer gaps stay black rather than being smeared over.
        const bool fullSpan = irHeight == SVISSR_SCAN_LINES;
        constexpr int kPartialFillMaxLines = 4;
        {
            std::vector<const Line*> byLine(static_cast<std::size_t>(SVISSR_SCAN_LINES + 1), nullptr);
            for (const auto& line : recentLines_) {
                if (line.lineNumber >= 1 && line.lineNumber <= SVISSR_SCAN_LINES)
                    byLine[static_cast<std::size_t>(line.lineNumber)] = &line;
            }
            // A lost scan line is rebuilt from the nearest good lines above and below, weighted by
            // distance, with matching structure along the row and the grain of the lines beside it
            // (see rebuildRowEdgeAware): a plain average of the two is what made the filled lines
            // read as smooth bands.
            auto blendGray = [](QImage& img, int y, const QImage& a, int ya, const QImage& b, int yb, int h, float t) {
                Q_UNUSED(a); Q_UNUSED(b);
                if (y < 0 || y >= h || ya < 0 || yb < 0 || ya >= h || yb >= h) return;
                nadirstation::products::rebuildRowEdgeAware(img, y, ya, yb, t);
            };

            const int firstLine = fullSpan ? 1 : minLine;
            const int lastLine = fullSpan ? SVISSR_SCAN_LINES : maxLine;
            const int rowOf0 = fullSpan ? 1 : minLine;   // image row = line - rowOf0
            for (int lineNo = firstLine; lineNo <= lastLine; ++lineNo) {
                if (rawRows || byLine[static_cast<std::size_t>(lineNo)] != nullptr) continue;
                int up = lineNo - 1;
                while (up >= firstLine && byLine[static_cast<std::size_t>(up)] == nullptr) --up;
                int dn = lineNo + 1;
                while (dn <= lastLine && byLine[static_cast<std::size_t>(dn)] == nullptr) ++dn;
                if (up < firstLine && dn > lastLine) continue;
                if (!fullSpan && (up < firstLine || dn > lastLine || dn - up - 1 > kPartialFillMaxLines)) continue;

                const int y = lineNo - rowOf0;
                if (up >= firstLine && dn <= lastLine) {
                    const int ya = up - rowOf0, yb = dn - rowOf0;
                    const float t = static_cast<float>(lineNo - up) / static_cast<float>(dn - up);
                    blendGray(ir1, y, ir1, ya, ir1, yb, irHeight, t);
                    blendGray(ir2, y, ir2, ya, ir2, yb, irHeight, t);
                    blendGray(ir3, y, ir3, ya, ir3, yb, irHeight, t);
                    blendGray(ir4, y, ir4, ya, ir4, yb, irHeight, t);
                    for (int sidx = 0; sidx < 4; ++sidx) {
                        const int vy = y * 4 + sidx;
                        const int va = ya * 4 + sidx, vb = yb * 4 + sidx;
                        blendGray(vis, vy, vis, va, vis, vb, visHeight, t);
                    }
                } else {
                    const int srcY = (up >= firstLine ? up : dn) - rowOf0;
                    auto copyRow = [](QImage& img, int dstY, int srcY) {
                        if (dstY == srcY || dstY < 0 || srcY < 0 || dstY >= img.height() || srcY >= img.height()) return;
                        const int bytes = static_cast<int>(std::min(img.bytesPerLine(), static_cast<qsizetype>(img.width())));
                        std::copy(img.constScanLine(srcY), img.constScanLine(srcY) + bytes, img.scanLine(dstY));
                    };
                    copyRow(ir1, y, srcY);
                    copyRow(ir2, y, srcY);
                    copyRow(ir3, y, srcY);
                    copyRow(ir4, y, srcY);
                    const int dstV = y * 4;
                    const int srcV = srcY * 4;
                    for (int sidx = 0; sidx < 4; ++sidx) copyRow(vis, dstV + sidx, srcV + sidx);
                }
                ++restoredScanLines;
            }
        }

        if (sessionDir_.isEmpty()) beginObservation();
        QDir dir(sessionDir_);
        dir.mkpath(".");
        const QString p1 = dir.filePath("CH1_VIS.png");
        const QString p2 = dir.filePath("CH2_IR4.png");
        const QString p3 = dir.filePath("CH3_IR3.png");
        const QString p4 = dir.filePath("CH4_IR1.png");
        const QString p5 = dir.filePath("CH5_IR2.png");
        if (!savePng(vis, p1) || !savePng(ir4, p2) || !savePng(ir3, p3) ||
            !savePng(ir1, p4) || !savePng(ir2, p5)) {
            emit statusMessage(QStringLiteral("Cannot save the FY-2G products: %1").arg(dir.absolutePath()));
            return;
        }

        QImage ie = nadirstation::products::irEnhancement(ir1, ir3);
        ie = nadirstation::products::repairMissingLines(ie, 8);
        ie = nadirstation::products::medianBlurPreservingFormat(ie, 3);
        const QString p7 = dir.filePath("IR_Enhanced.png");
        savePng(ie, p7);

        // A full FY-2G disk is complete only when the actual reported line
        // numbering spans the documented 1..2291 scan range.  The normal
        // products above are still written for partial observations, while
        // a second set with GLOBAL_ prefixes is emitted only for a complete
        // disk so it is unambiguous in downstream processing.
        const bool fullDisk = hasCompleteDiskCoverage();

        QList<QString> paths{p1, p2, p3, p4, p5, p7};
        const std::array<QString, 4> thermalNames{
            QStringLiteral("CH4_IR1_TBB_0p01K.png"), QStringLiteral("CH5_IR2_TBB_0p01K.png"),
            QStringLiteral("CH3_IR3_TBB_0p01K.png"), QStringLiteral("CH2_IR4_TBB_0p01K.png")};
        const std::array<QString, 4> thermalDisplayNames{
            QStringLiteral("CH4_IR1_TBB_Enhanced.png"), QStringLiteral("CH5_IR2_TBB_Enhanced.png"),
            QStringLiteral("CH3_IR3_TBB_Enhanced.png"), QStringLiteral("CH2_IR4_TBB_Enhanced.png")};
        if (calibrationLutReady_) {
            for (int channel = 0; channel < 4; ++channel) {
                QImage tbb(irWidth, calibrationImageHeight, QImage::Format_Grayscale16);
                QImage display(irWidth, calibrationImageHeight, QImage::Format_Grayscale8);
                tbb.fill(0);
                display.fill(0);
                const auto& lut = calibrationLuts_[static_cast<std::size_t>(channel + 1)];
                for (int y = 0; y < calibrationImageHeight; ++y) {
                    const auto* counts = reinterpret_cast<const quint16*>(irCounts10[static_cast<std::size_t>(channel)].constScanLine(y));
                    auto* kelvinHundredths = reinterpret_cast<quint16*>(tbb.scanLine(y));
                    uchar* shown = display.scanLine(y);
                    for (int x = 0; x < irWidth; ++x) {
                        const int dn = counts[x];
                        if (dn >= lut.size()) continue;
                        const float temperatureK = lut[dn];
                        if (!std::isfinite(temperatureK) || temperatureK < 100.0f || temperatureK > 500.0f) continue;
                        kelvinHundredths[x] = static_cast<quint16>(qRound(temperatureK * 100.0f));
                        // Cold cloud tops render bright; warm surfaces render dark.
                        shown[x] = static_cast<uchar>(qBound(0,
                            qRound((330.0f - temperatureK) * 255.0f / 150.0f), 255));
                    }
                }
                tbb = nadirstation::products::repairMissingLinesMedian(tbb, 2);
                // These in-memory calibration rasters use 0 as the
                // unfilled-pixel sentinel; keep that convention explicit.
                tbb = nadirstation::products::medianBlurPreservingFormat(tbb, 3, nullptr, false, 0);
                display = nadirstation::products::repairMissingLines(display, 8);
                display = nadirstation::products::medianBlurPreservingFormat(display, 3);
                const QString numericPath = dir.filePath(thermalNames[static_cast<std::size_t>(channel)]);
                const QString displayPath = dir.filePath(thermalDisplayNames[static_cast<std::size_t>(channel)]);
                if (savePng(tbb, numericPath)) paths.append(numericPath);
                if (savePng(display, displayPath)) paths.append(displayPath);
                if (channel == 0) {
                    const QString colorPath = dir.filePath(QStringLiteral("CH4_IR1_Longwave_ColorEnhanced.png"));
                    const QImage color = nadirstation::products::longwaveIrColorEnhancement(tbb);
                    if (savePng(color, colorPath)) paths.append(colorPath);
                }
            }

            // Preserve the majority-assembled calibration payload and the LUT
            // values used to generate the temperature rasters.
            QFile rawCalibration(dir.filePath(QStringLiteral("FY2_Calibration2.bin")));
            if (rawCalibration.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                rawCalibration.write(calibrationBlock_);
                rawCalibration.close();
            }
            QJsonObject calibrationJson;
            calibrationJson[QStringLiteral("format")] = QStringLiteral("FY-2 S-VISSR Calibration-2 layout");
            calibrationJson[QStringLiteral("groups_received")] = calibrationGroupsSeen_;
            calibrationJson[QStringLiteral("table_source")] = calibrationSource_;
            calibrationJson[QStringLiteral("groups_expected")] = 25;
            calibrationJson[QStringLiteral("repeat_policy")] = QStringLiteral("DOC ID/filler-accepted groups; bitwise majority across observed repeat IDs");
            calibrationJson[QStringLiteral("infrared_unit")] = QStringLiteral("K");
            calibrationJson[QStringLiteral("infrared_lut_order")] = QJsonArray{
                QStringLiteral("IR1 / CH4"), QStringLiteral("IR2 / CH5"),
                QStringLiteral("IR3 / CH3"), QStringLiteral("IR4 / CH2")};
            calibrationJson[QStringLiteral("raw_block_file")] = QStringLiteral("FY2_Calibration2.bin");
            QJsonArray lutJson;
            for (const auto& lut : calibrationLuts_) {
                QJsonArray channelLut;
                for (const float value : lut) channelLut.append(value);
                lutJson.append(channelLut);
            }
            calibrationJson[QStringLiteral("luts_vis_ir1_ir2_ir3_ir4")] = lutJson;
            QFile calibrationSidecar(dir.filePath(QStringLiteral("FY2_RadiometricCalibration.json")));
            if (calibrationSidecar.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                calibrationSidecar.write(QJsonDocument(calibrationJson).toJson(QJsonDocument::Indented));
                calibrationSidecar.close();
            }
        }
        QImage falseColour;
        QString falseColourPath;
        // FalseColor: SatDump's legacy S-VISSR false colour. The visible channel and IR4 (3.75 um) go
        // through the S-VISSR colour table, lut(ch2, 1-ch1), and the table colour is written as it is.
        // Like SatDump it is made for every scene, night ones included.
        {
            QString geographyReport;
            falseColour = nadirstation::products::falseColor(vis, ir4, ir1, nullptr, &geographyReport);
            if (!falseColour.isNull()) {
                nadirstation::log::write(QStringLiteral("FalseColor: %1").arg(geographyReport));
                const QString pf = dir.filePath("FalseColor.png");
                if (savePng(falseColour, pf)) falseColourPath = pf;
            } else {
                nadirstation::log::write(QStringLiteral("FalseColor skipped: colour table resources/fy2/svissr/lut.png not found"));
            }
        }

        if (!falseColourPath.isEmpty()) paths.append(falseColourPath);
        if (fullDisk) {
            const QString g2 = dir.filePath("GLOBAL_CH2_IR4.png");
            const QString g3 = dir.filePath("GLOBAL_CH3_IR3.png");
            const QString g4 = dir.filePath("GLOBAL_CH4_IR1.png");
            const QString g5 = dir.filePath("GLOBAL_CH5_IR2.png");
            const QString g7 = dir.filePath("GLOBAL_IR_Enhanced.png");
            savePng(ir4, g2);
            savePng(ir3, g3);
            savePng(ir1, g4);
            savePng(ir2, g5);
            savePng(ie, g7);
            paths.append(g2); paths.append(g3); paths.append(g4);
            paths.append(g5); paths.append(g7);
            if (!falseColour.isNull()) {
                const QString gf = dir.filePath("GLOBAL_FalseColor.png");
                if (savePng(falseColour, gf)) paths.append(gf);
            }
            QFile marker(dir.filePath("FULL_DISK_READY.txt"));
            if (marker.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                marker.write("FY-2G S-VISSR full-disk assembly complete\n");
                marker.write("Reported scan lines: 1..2291\n");
                marker.write("GLOBAL_* non-VIS products contain the complete assembled disk; CH1_VIS.png is already full size.\n");
                marker.close();
            }
            nadirstation::log::write(QStringLiteral("FULL DISK COMPLETE: 1..%1, GLOBAL products saved").arg(SVISSR_SCAN_LINES));
        }

        lastBuiltLine_ = maxLine;
        const QString timestamp = QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss");
        const QString manifest = writeSessionManifest(fullDisk, paths, minLine, maxLine,
            restoredScanLines, repairedBlankRows, repairedImpulsePixels);
        if (!manifest.isEmpty()) nadirstation::log::write(QStringLiteral("session manifest saved=%1").arg(manifest));
        observationFinalized_ = true;
        nadirstation::log::write(QStringLiteral("final products saved, emitting imageReady fullDisk=%1").arg(fullDisk));
        emit imageReady(timestamp, paths);
        emit statusMessage(fullDisk
            ? QStringLiteral("FY-2G full disk complete: %1 → GLOBAL_* products, loaded into the viewer").arg(dir.absolutePath())
            : QStringLiteral("FY-2G image saved and loaded into the viewer: %1 (lines %2-%3)")
                .arg(dir.absolutePath()).arg(minLine).arg(maxLine));
    } catch (const std::exception& e) {
        nadirstation::log::write(QStringLiteral("decoder exception: %1").arg(QString::fromUtf8(e.what())));
        emit statusMessage(QStringLiteral("FY-2G final image failed: %1").arg(QString::fromUtf8(e.what())));
    } catch (...) {
        emit statusMessage(QStringLiteral("FY-2G final image: unknown exception."));
    }
}

void NadirStationDecoder::finishDecoding() {
    if(isPolar()) {
        drainQueuedSamples();
        metopDecoder_.drain([this](nadirstation::gk2a::Cadu&& c) {
            if (c.rsOk && polarImager_) { avhrr_.feed(c.bytes.data(), *polarImager_); metopInstr_.feed(c.bytes.data()); }
        });
        if (isMetop()) {
            const auto& st = metopDecoder_.stats();
            nadirstation::log::write(QStringLiteral("MetOp AHRPT finished: RS ok %1 / sync %2, corrected %3, AVHRR lines %4, VCID 9 gaps %5")
                .arg(st.cadusWithGoodRs).arg(st.cadusWithSync).arg(st.cadusCorrected).arg(avhrr_.lines()).arg(0));
        } else {
            nadirstation::log::write(QStringLiteral("METEOR HRPT finished: frames %1 (flywheel %2), MSU-MR lines %3, carrier %4 Hz, SNR %5 dB")
                .arg(meteorDeframer_.frames()).arg(meteorDeframer_.flywheelFrames()).arg(msumr_.lines())
                .arg(meteorDemod_.carrierOffsetHz(), 0, 'f', 0).arg(meteorDemod_.snrDb(), 0, 'f', 1));
        }
        finalizePolarPass(QStringLiteral("reception ended"));
        return;
    }
    if(isElektro()) {
        drainQueuedSamples();
        finalizeElektro();
        return;
    }
    if(isGoesSd()) {
        drainQueuedSamples();
        finalizeGoesSdObservation();
        return;
    }
    if(isGvar()) {
        drainQueuedSamples();
        // Reception ended: the observation in progress is written (false colour, five channels, JSON)
        // and handed to the image page.
        finalizeGvarObservation();
        return;
    }
    if(isHrit()) {
        drainQueuedSamples();
        hritDecoder_.drain([this](nadirstation::gk2a::Cadu&& frame){handleHritFrame(std::move(frame));});
        {
            const auto& st = hritDecoder_.stats();
            nadirstation::log::write(QStringLiteral("GK-2A HRIT finished: RS ok %1 / sync %2, corrected %3")
                .arg(st.cadusWithGoodRs).arg(st.cadusWithSync).arg(st.cadusCorrected));
        }
        // Reception or decoding has ended: whatever of the observation has arrived is written out
        // (five channels as received, L2 when all five are present, GK2A_Observation.json) and
        // imported into the image page, instead of waiting for a full disk that will not come.
        finalizeHritObservation();
        return;
    }
    drainQueuedSamples();
    // All complete input blocks have already gone through the persistent
    // demodulator. There is deliberately no second "offline" demodulation pass here.
    dspAccumulator_.clear();
    if (isGk2a()) {
        const auto& st = gk2aLrit_.stats();
        nadirstation::log::write(QStringLiteral("GK-2A LRIT finished: RS ok %1 / sync %2, corrected %3")
            .arg(st.cadusWithGoodRs).arg(st.cadusWithSync).arg(st.cadusCorrected));
    }
    // Decoding has stopped, so the observation in progress is handed over. A reception that
    // captured eight strips of ten never completes and has no successor to push it out, so this is
    // the only end it has.
    flushGk2aObservation(true);
    tryCompleteImage(true);
}

// ---------------------------------------------------------------------------------------------
// ELEKTRO-L GGAK
// ---------------------------------------------------------------------------------------------

void NadirStationDecoder::resetGgak() {
    ggakDemod_ = nadirstation::ggak::Demodulator{};
    ggakRateScaleLogged_ = 1.0;
    ggakDeframer_.reset();
    ggakGood_ = ggakBad_ = 0;
    ggakLastGoodMs_ = 0;
    ggakLastStatusMs_ = 0;
    ggakStreamMs_ = 0;
    ggakStreamMsExact_ = 0.0;
    ggakLoggedSync_ = -1;
    ggakLoggedTracking_ = false;
    ggakUnit_.reset();
    ggakUnitLogged_.clear();
}

void NadirStationDecoder::storeGgakFrame(const nadirstation::ggak::Frame& frame) {
    // <root>/ELEKTRO-L/GGAK/<UTC date of reception>/: the raw frames, a header log and the two
    // decoded instruments as CSV. Appended per frame; at 2.8 frames a second that is cheap.
    // The folder follows the reception date: filler frames carry 0x33 in every byte, including
    // the time field, which reads as March 2027 and split a live session over two folders.
    const QDateTime t = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(frame.unixTime), QTimeZone::UTC);
    const QString day = QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd"));
    const QString dir = QDir(outputRoot_).filePath(QStringLiteral("ELEKTRO-L/GGAK/") + day);
    if (ggakDir_ != dir) {
        QDir().mkpath(dir);
        ggakDir_ = dir;
    }
    auto append = [&dir](const QString& name, const QByteArray& data, const QByteArray& header) {
        QFile f(QDir(dir).filePath(name));
        const bool fresh = !f.exists();
        if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) return;
        if (fresh && !header.isEmpty()) f.write(header);
        f.write(data);
    };
    const QString received = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    const QString frameTime = frame.id == nadirstation::ggak::kIdFiller ? QString() : t.toString(Qt::ISODate);
    append(QStringLiteral("frames.csv"),
           QStringLiteral("%1,%2,%3,%4,%5,%6,%7\n").arg(received, frameTime).arg(frame.id)
               .arg(QString::fromLatin1(nadirstation::ggak::frameIdName(frame.id)).replace(',', ';'))
               .arg(frame.masterCounter).arg(frame.channelCounter).arg(frame.checksumOk ? 1 : 0).toUtf8(),
           QByteArrayLiteral("received_utc,frame_time_utc,id,instrument,master_counter,channel_counter,checksum_ok\n"));
    if (!frame.checksumOk) return;
    ggakUnit_.note(frame.id, true);
    {
        const std::string label = ggakUnit_.label();
        if (label != ggakUnitLogged_ && ggakUnit_.confidence() > 0) {
            ggakUnitLogged_ = label;
            nadirstation::log::write(QStringLiteral("GGAK: unit %1 (confidence %2 percent); %3")
                                 .arg(QString::fromStdString(label)).arg(ggakUnit_.confidence())
                                 .arg(QString::fromStdString(ggakUnit_.reason())));
        }
    }
    // ELEKTRO-L N4 instrument records (layouts measured, instrument names inferred): one CSV per
    // frame ID with the named fields and the record's raw bytes, for later analysis.
    if (const nadirstation::ggak::L4Layout* layout = nadirstation::ggak::l4Layout(frame.id)) {
        QByteArray rows;
        QByteArray header = "time_utc,record_index,counter";
        for (const auto& f : nadirstation::ggak::l4Fields())
            if (f.id == frame.id) header += QByteArray(",") + QByteArray(f.name).replace(' ', '_').replace('(', '_').replace(')', '_');
        header += ",record_hex\n";
        for (const auto& r : nadirstation::ggak::splitL4Records(frame)) {
            QString line = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(r.unixTime * 1000.0), QTimeZone::UTC).toString(Qt::ISODateWithMs);
            line += QStringLiteral(",%1,%2").arg(r.index).arg(r.counter);
            for (const auto& f : nadirstation::ggak::l4Fields()) {
                if (f.id != frame.id) continue;
                const double v = nadirstation::ggak::l4FieldValue(r, f);
                line += QLatin1Char(',') + (std::isnan(v) ? QString() : QString::number(v, 'f', 0));
            }
            line += QLatin1Char(',') + QString::fromLatin1(QByteArray(reinterpret_cast<const char*>(r.bytes.data()),
                                                                      static_cast<qsizetype>(r.bytes.size())).toHex());
            rows += line.toUtf8() + '\n';
        }
        Q_UNUSED(layout);
        if (!rows.isEmpty()) append(QStringLiteral("l4_id%1.csv").arg(frame.id), rows, header);
    }
    append(QStringLiteral("ggak.cadu"),
           QByteArray(reinterpret_cast<const char*>(frame.bytes.data()), nadirstation::ggak::kFrameBytes), {});
    if (frame.id == nadirstation::ggak::kIdSkl) {
        QByteArray rows;
        for (const auto& r : nadirstation::ggak::parseSkl(frame)) {
            QString line = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(r.unixTime), QTimeZone::UTC).toString(Qt::ISODate);
            for (int c = 0; c < nadirstation::ggak::kSklChannels; ++c)
                line += QLatin1Char(',') + (std::isnan(r.value[static_cast<std::size_t>(c)]) ? QString()
                                                : QString::number(r.value[static_cast<std::size_t>(c)], 'g', 8));
            for (int c = 0; c < 11; ++c) line += QLatin1Char(',') + QString::number(r.raw[static_cast<std::size_t>(c)]);
            rows += line.toUtf8() + '\n';
        }
        QByteArray header = "time_utc";
        for (int c = 0; c < nadirstation::ggak::kSklChannels; ++c) header += ",skl" + QByteArray::number(c + 1);
        for (int c = 0; c < 11; ++c) header += ",raw" + QByteArray::number(c + 3);
        append(QStringLiteral("skl.csv"), rows, header + '\n');
    } else if (frame.id == nadirstation::ggak::kIdSkif) {
        QByteArray rows;
        for (const auto& r : nadirstation::ggak::parseSkif(frame)) {
            rows += QStringLiteral("%1,%2,%3,%4,%5,%6,%7\n")
                        .arg(QDateTime::fromSecsSinceEpoch(static_cast<qint64>(r.unixTime), QTimeZone::UTC).toString(Qt::ISODate))
                        .arg(r.recordId).arg(r.position)
                        .arg(QString::fromLatin1(nadirstation::ggak::skifChannelName(r.channel)))
                        .arg(r.raw).arg(r.value, 0, 'g', 8)
                        .arg(nadirstation::ggak::skifScalingUncertain(r.channel) ? 1 : 0).toUtf8();
        }
        append(QStringLiteral("skif.csv"), rows,
               QByteArrayLiteral("time_utc,record_id,position,channel,raw,value,scaling_uncertain\n"));
    }
}

void NadirStationDecoder::processGgakSamples(const std::vector<std::complex<float>>& iq) {
    if (iq.empty() || sampleRate_ <= 0.0) return;
    ggakDemod_.configure(sampleRate_);
    // The demodulator's own rate, which differs from sampleRate_ when the source declared the wrong
    // one (see Demodulator::coarseSearch); stream time follows the samples as they really are.
    ggakStreamMsExact_ += static_cast<double>(iq.size()) * 1000.0 / ggakDemod_.sampleRate();
    ggakStreamMs_ = static_cast<qint64>(std::llround(ggakStreamMsExact_));
    const std::vector<float> soft = ggakDemod_.process(iq);
    if (ggakDemod_.stats().rateScale != ggakRateScaleLogged_) {
        ggakRateScaleLogged_ = ggakDemod_.stats().rateScale;
        nadirstation::log::write(QStringLiteral("GGAK: the source says %1 MS/s but the signal fits %2 MS/s "
                                        "(the file header is probably wrong); decoding at %2 MS/s")
                             .arg(sampleRate_ / 1e6, 0, 'f', 3).arg(ggakDemod_.sampleRate() / 1e6, 0, 'f', 3));
        emit statusMessage(QStringLiteral("GGAK: the file's stated sample rate %1 MS/s does not match the signal; demodulated at %2 MS/s")
                               .arg(sampleRate_ / 1e6, 0, 'f', 3).arg(ggakDemod_.sampleRate() / 1e6, 0, 'f', 3));
    }

    std::vector<std::uint8_t> bits(soft.size());
    for (std::size_t i = 0; i < soft.size(); ++i) bits[i] = soft[i] > 0.0f ? 1u : 0u;
    std::vector<std::array<std::uint8_t, nadirstation::ggak::kFrameBytes>> frames;
    ggakDeframer_.push(bits, frames);

    QByteArray block;
    for (const auto& raw : frames) {
        const nadirstation::ggak::Frame frame = nadirstation::ggak::parseFrame(raw.data());
        if (frame.checksumOk) {
            ++ggakGood_;
            ggakLastGoodMs_ = ggakStreamMs_;
        } else {
            ++ggakBad_;
        }
        storeGgakFrame(frame);
        block.append(reinterpret_cast<const char*>(raw.data()), nadirstation::ggak::kFrameBytes);
    }
    if (!block.isEmpty()) emit ggakFrames(block);

    // No good frame for 20 s of signal while a carrier is being tracked: look for the carrier again.
    if (ggakDemod_.stats().carrierFound && ggakStreamMs_ - ggakLastGoodMs_ > 20000) {
        ggakDemod_.requestSearch();
        ggakLastGoodMs_ = ggakStreamMs_;
        nadirstation::log::write(QStringLiteral("GGAK: no valid frame for 20 s; searching for the carrier again"));
    }

    publishConstellation(ggakDemod_.lastSymbols());
    {
        // Acquisition and sync changes go to the runtime log, so a session can be reviewed later.
        const auto& st = ggakDemod_.stats();
        if (st.tracking != ggakLoggedTracking_) {
            ggakLoggedTracking_ = st.tracking;
            nadirstation::log::write(st.tracking
                ? QStringLiteral("GGAK: carrier acquired at %1 Hz (signal %2 dB above noise); tracking")
                      .arg(st.coarseOffsetHz, 0, 'f', 1).arg(st.carrierToNoiseDb, 0, 'f', 1)
                : QStringLiteral("GGAK: searching for the carrier"));
        }
        const int sync = ggakDeframer_.state();
        if (sync != ggakLoggedSync_) {
            ggakLoggedSync_ = sync;
            nadirstation::log::write(QStringLiteral("GGAK: frame sync %1; carrier %2 Hz, Eb/N0 %3 dB, modulation index %4 deg, "
                                            "%5 sym/s, frames ok %6, checksum errors %7")
                                 .arg(sync == 2 ? QStringLiteral("locked") : sync == 1 ? QStringLiteral("candidate") : QStringLiteral("searching"))
                                 .arg(st.coarseOffsetHz + st.pllOffsetHz, 0, 'f', 1).arg(st.snrDb, 0, 'f', 1)
                                 .arg(st.modulationIndexDeg, 0, 'f', 0).arg(st.symbolsPerSecond, 0, 'f', 1)
                                 .arg(ggakGood_).arg(ggakBad_));
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (ggakLastStatusMs_ == 0 || now - ggakLastStatusMs_ >= 200) {
        ggakLastStatusMs_ = now;
        const auto& st = ggakDemod_.stats();
        const int state = ggakDeframer_.state();
        lock_ = state == 2 ? 100 : (state == 1 ? 50 : 0);
        carrierOffsetHz_ = static_cast<float>(st.coarseOffsetHz + st.pllOffsetHz);
        emit ggakStatus(state, st.coarseOffsetHz + st.pllOffsetHz, st.carrierToNoiseDb, st.carrierFound,
                        st.snrDb, ggakGood_, ggakBad_, ggakDir_);
        emit decodeStats(lock_, static_cast<int>(ggakGood_), static_cast<int>(ggakBad_),
                         static_cast<float>(std::clamp(st.snrDb / 20.0, 0.0, 1.0)), carrierOffsetHz_);
        emit demodSnrMetricsReady(static_cast<float>(st.snrDb), static_cast<float>(st.snrDb), static_cast<float>(st.snrDb));
    }
}

NadirStationDecoder::~NadirStationDecoder() { stopHritDsp(); }

bool NadirStationDecoder::hritDspRunningNow() {
    std::lock_guard<std::mutex> lock(ingressMutex_);
    return hritDspRunning_;
}

void NadirStationDecoder::startHritDsp() {
    stopHritDsp();
    // Created before the thread exists, so the event-loop timer never reads a pointer being built.
    if (isHrit() && !hritImages_) hritImages_ = std::make_unique<nadirstation::gk2a::HritImages>();
    {
        std::lock_guard<std::mutex> lock(ingressMutex_);
        dspStop_ = false;
        hritDspRunning_ = true;
    }
    hritDspThread_ = std::thread([this] { hritDspLoop(); });
}

void NadirStationDecoder::stopHritDsp() {
    if (!hritDspThread_.joinable()) return;
    {
        std::lock_guard<std::mutex> lock(ingressMutex_);
        dspStop_ = true;
        hritDspRunning_ = false;
    }
    dspCv_.notify_all();
    hritDspThread_.join();
}

void NadirStationDecoder::hritDspLoop() {
    for (;;) {
        std::shared_ptr<std::vector<std::complex<float>>> block;
        {
            std::unique_lock<std::mutex> lock(ingressMutex_);
            // Heartbeat for the hang watchdog. The DSP thread replaces the timer drain for HRIT,
            // so without this beat the watchdog reports a false 'decoder thread not responding'.
            nadirstation::log::beat(1);
            dspCv_.wait_for(lock, std::chrono::milliseconds(500), [this] { return !ingressQueue_.empty() || dspStop_; });
            // Leave only once everything queued before the stop has been processed.
            if (ingressQueue_.empty()) { if (dspStop_) return; continue; }
            block = std::move(ingressQueue_.front());
            ingressQueue_.pop_front();
            ingressQueuedSamples_ -= std::min<std::size_t>(ingressQueuedSamples_, block ? block->size() : 0);
        }
        ingressCv_.notify_one();
        if (block && !block->empty()) {
            std::lock_guard<std::mutex> guard(hritDspMutex_);
            processSamplesCore(block);
        }
    }
}
