#pragma once
// GOES-N/O/P Sensor Data (SD) at 1676.0 MHz: the instruments' raw data before ground processing.
//
// One carrier, two BPSK components in quadrature:
//   Imager   2.6208 Mbit/s on one axis: NRZ-M, 480-bit blocks (48 10-bit words) starting with a
//            14-bit sync (0x2B50 after differential decoding), each block XORed with a fixed 60-byte
//            sequence. A scan line (one east-west sweep, 8 visible detectors + 2 per IR channel)
//            is ~5200 image blocks between long all-zero fills at the scan turnarounds (~1.15 s).
//            The header's direction flag is a single bit, so the direction of each scan is decided
//            from the image itself (the new scan is matched against the previous one, as it is and
//            mirrored).
//   Sounder  40 kbit/s on the other axis: NRZ-M, 4224-bit frames after the marker 0x3683A2E6,
//            whitened with a 13-stage sequence that restarts 68 bytes after the marker; 264 16-bit
//            words per frame, words 4..248 with odd parity in the least significant bit.
//
// No error correction on either. The imager is usually received near its threshold, so every bit
// keeps its soft value (matched-filter output; an NRZ-M bit is as reliable as the weaker of its two
// symbols), and the pictures are made with a reliability-weighted vote of each bit plane over a
// small neighbourhood (3 rows x 5 samples), after which only isolated dots far from their 3 x 3
// median are replaced by that median.
//
// Format as worked out from SatDump (goesn_sd), the GOES-N databook and two recordings of
// 2026-11-22; this is an independent implementation.
#include "dsp.h"
#include <QImage>
#include <QString>
#include <QStringList>
#include <array>
#include <atomic>
#include <complex>
#include <cstdint>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <vector>

namespace nadirstation::goessd {

constexpr double kImagerRate = 2620800.0;
constexpr double kSounderRate = 40000.0;

struct Block {
    std::array<uint16_t, 48> words{};
    std::array<int8_t, 480> soft{};      // de-whitened soft bits, positive = 1
    int ones = 0;                        // ones among the 480 de-whitened bits (fill blocks are near 0)
};

class Engine {
public:
    struct Update {
        QString stamp, message;
        QImage live;
        QStringList paths;
    };
    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool configure(double sampleRate);
    void setOutputRoot(const QString& root) { outputRoot_ = root; }
    void setSoftDecisions(bool on) { soft_ = on; }                          // false: plain bit majority (for comparison)
    void reset();
    dsp::DPSKResult process(const std::vector<std::complex<float>>& iq);   // DSP thread
    void finish();                                                         // end of reception: write what there is

    Update takeLive();
    std::vector<Update> takeCompleted();

    bool configured() const { return configured_; }
    float lockMeasure() const { return lock_.load(); }
    float carrierOffsetHz() const { return carrierHz_.load(); }
    double snrDb() const { return snr_.load(); }
    uint64_t syncs() const { return syncs_.load(); }            // blocks whose 14 sync bits were all right
    uint64_t blocks() const { return blocks_.load(); }
    uint64_t scans() const { return scans_.load(); }
    uint64_t directionFixes() const { return flips_.load(); }
    uint64_t sounderFrames() const { return sndFrames_.load(); }
    double syncBitErrorPct() const { return syncBer_.load(); }   // errors among the sync bits of the locked blocks
    double sounderParityPct() const { return sndParity_.load(); }
    bool imagerLocked() const { return locked_.load(); }
    bool sounderLocked() const { return sndLocked_.load(); }
    int imagerPhase() const { return phaseOut_.load(); }

    struct Scan;
    struct Raw;
    struct Observation;
    uint64_t widthOutliers() const { return outliers_.load(); }
    uint64_t modeChanges() const { return modes_.load(); }
    int scanWidth() const { return widthOut_.load(); }

private:
    void handle(std::vector<std::complex<float>> imager, std::vector<std::complex<float>> sounder);
    void imagerSymbols(const std::vector<std::complex<float>>& s);
    void takeBlock(Block&& b);
    void findScans();
    void addScan(int start, int stop, float headerSoft, int64_t endBlock);
    void commitScan(std::shared_ptr<Raw> r);
    void sounderSymbols(const std::vector<std::complex<float>>& s);
    void sounderRecord(uint64_t start);
    void closeObservation(const QString& why);
    void pushLive(bool force);
    void writeObservation(std::shared_ptr<Observation> o);
    void evaluatePhase();
    void sounderDecimate(const std::vector<std::complex<float>>& in, std::vector<std::complex<float>>& out);

    bool configured_ = false;
    bool soft_ = true;
    double sampleRate_ = 0.0;
    QString outputRoot_;
    dsp::PskDemod imager_, sounder_;
    std::vector<std::complex<float>> pendingFront_;
    std::future<void> job_;
    std::deque<std::future<void>> writes_;
    // sounder pre-decimation (fractional boxcar to 1.2 MS/s, then 5-sample boxcar to 240 kS/s)
    double decA_ = 5.0, decPos_ = 0.0;
    std::complex<float> accA_{}, accB_{};
    int nB_ = 0;

    // imager bit sync
    float prevSym_ = 0.0f;
    float level_ = 0.0f;
    uint16_t reg_ = 0;
    uint64_t bitCount_ = 0;
    std::array<float, 480> hist_{};
    int phase_ = -1;
    int pendingPhase_ = -1, pendingVotes_ = 0;
    uint64_t nextStart_ = 0;
    bool inBlock_ = false;
    std::vector<uint8_t> blkBits_;
    std::vector<float> blkSoft_;
    uint64_t syncErrBits_ = 0, syncBitsSeen_ = 0, blocksSinceEval_ = 0;
    uint64_t lastGoodBlock_ = 0;

    // scan assembly: recent blocks
    std::vector<Block> buf_;
    int64_t base_ = 0;                 // global index of buf_[0]
    int64_t lastEnd_ = -100000;
    int64_t prevHeader_ = -1;
    float prevHdrSoft_ = 0.0f;         // soft value of the direction flag in the header that opened the scan being collected
    int64_t sinceCheck_ = 0;
    double period_ = 6286.0;
    int64_t lastScanEnd_ = -1;
    std::vector<float> lastProfile_, lastProfile4_;
    int lastProfileNo_ = -1000;
    // scan width tolerance and direction decisions (all on the worker thread)
    std::vector<std::shared_ptr<Raw>> pending_;
    int refWidth_ = 0;                 // visible samples per scan of the current picture
    int scanNo_ = 0;                   // scans (blank ones included) in the current picture
    bool lastFlip_ = false;
    double altVote_ = 1.0;             // > 0: consecutive scans run in opposite directions
    int lowRun_ = 0, emptyRun_ = 0, structured_ = 0;

    // sounder frames
    float sPrev_ = 0.0f;
    uint32_t sReg_ = 0;
    uint64_t sCount_ = 0, sBase_ = 0;
    std::vector<uint8_t> sBits_;
    std::deque<uint64_t> sPending_;    // marker starts waiting for their 4224 bits
    int64_t sLastMarker_ = -1;
    uint64_t sndParityOk_ = 0, sndParityAll_ = 0;
    double sndLastSec_ = -100.0;

    std::shared_ptr<Observation> obs_;
    double streamSec_ = 0.0;
    double snrY1_ = 0.0, snrY2_ = 0.0;
    int64_t lastLiveMs_ = 0;

    std::mutex uMutex_;
    Update live_;
    std::vector<Update> completed_;

    std::atomic<float> lock_{0.f}, carrierHz_{0.f};
    std::atomic<double> snr_{0.0}, syncBer_{0.0}, sndParity_{0.0};
    std::atomic<uint64_t> syncs_{0}, blocks_{0}, scans_{0}, sndFrames_{0}, flips_{0};
    std::atomic<bool> locked_{false}, sndLocked_{false};
    std::atomic<int> phaseOut_{-1}, widthOut_{0};
    std::atomic<uint64_t> outliers_{0}, modes_{0};
};

}  // namespace nadirstation::goessd
