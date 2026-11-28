#pragma once

#include <cmath>

#include "gk2adecoder.h"
#include "gk2ahrit.h"
#include "gk2ahritimages.h"
#include "gk2adatalink.h"
#include "gk2aproducts.h"
#include "elektroxrit.h"
#include "polarhrpt.h"
#include "polarinstruments.h"
#include <QObject>
#include <QImage>
#include <QString>
#include <atomic>
#include <memory>
#include <vector>
#include <complex>
#include <array>
#include <QByteArray>
#include <QDateTime>
#include <QList>
#include <QVector>
#include <cstdint>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <utility>
#include <QTimer>
#include <QHash>
#include <limits>
#include "dsp.h"
#include "ggak.h"
#include "pipelinediag.h"

#include "iqmetatypes.h"

namespace nadirstation::gvar { class Engine; }
namespace nadirstation::goessd { class Engine; }
class NadirStationDecoder : public QObject {
    Q_OBJECT
public:
    // Runs the ingress queue to completion. The application feeds this from another thread
    // and relies on the queue; a caller driving the decoder itself -- a test harness, or
    // anything reading a file rather than a receiver -- has to be able to drain it, or the
    // whole input piles up and is processed at the end.
    void drainQueuedSamples();
    explicit NadirStationDecoder(QObject* parent = nullptr);
    ~NadirStationDecoder() override;

    void setOutputRoot(const QString& root);
    // Read back the folder products are written under, so a viewer can look for them.
    QString outputRoot() const { return outputRoot_; }
    void setEnabled(bool enabled);
    void setSampleRate(double sampleRate);
    void setGk2aKeyPath(const QString& path) { gk2aKeyPath_=path; gk2aKeysLoaded_=false; }
    void setDemodParameters(float rrcAlpha, float pllBandwidth);

    // The symbol rate belongs to the spacecraft, not to the modem: FY-2 S-VISSR runs at 660 kbaud
    // and GK-2A LRIT at 128 kbaud (Table 9.1 gives 64 kbps with a rate-1/2 code). Selecting a
    // satellite therefore has to retune this chain, and until it does the constellation shows a
    // signal that looks locked while the frames never sync.
    void setSymbolRate(double symbolRate);
    double symbolRate() const { return symbolRate_; }
    void setIngressBackpressureEnabled(bool enabled);
    void setSourceLabel(const QString& label) { sourceLabel_ = label; }
    // Which spacecraft the operator selected. The decoder needs it to place its
    // products under a per-spacecraft folder: a timestamp alone does not say
    // whether a session came from GK-2A or from FY-2, and mixing them in one
    // directory makes the provenance of every file a guess.
    void setSatelliteName(const QString& name) { satelliteName_ = name; }
    bool enabled() const { return enabled_.load(); }

public slots:
    // Fast ingress: this slot only enqueues IQ and returns immediately. It is
    // intentionally safe for DirectConnection from the Pluto/WAV worker thread.
    void processSamples(const std::shared_ptr<std::vector<std::complex<float>>>& samples);
    // Runs the ingress queue to completion. The application feeds this from another thread and
    // relies on the queue; a caller driving the decoder itself -- a test harness, or anything
    // reading a file rather than a receiver -- has to be able to drain it, or the whole input
    // piles up and is processed at the end.
    void finishDecoding();

private slots:

signals:
    void statusMessage(const QString& text);
    void decodeStats(int lock, int frames, int badFrames, float softQuality, float carrierOffsetHz);
    // ELEKTRO-L GGAK: every frame received in this block (224 bytes each, ASM included, checksum
    // not yet filtered), and the receiver state for the GGAK page.
    void ggakFrames(const QByteArray& frames);
    void ggakStatus(int syncState, double carrierOffsetHz, double carrierToNoiseDb, bool carrierFound,
                    double snrDb, qulonglong goodFrames, qulonglong badFrames, const QString& outputDir);
    void demodSnrMetricsReady(float snr, float peak, float average);
    void symbolConstellation(const std::shared_ptr<std::vector<std::complex<float>>>& symbols);
    void imagePreviewReady(const QString& timestamp, const QList<QString>& paths);
    // Live GK-2A HRIT visible-channel quicklook (8-bit, rows received so far); in memory only.
    void hritLivePreviewReady(const QString& timestamp, const QImage& image, const QString& info);
    void imageReady(const QString& timestamp, const QList<QString>& paths);
    void telemetryUpdated(const QByteArray& statusBlock);
    // GOES GVAR block 0 and the other non-imager blocks. block0: the newest block 0 information field; consensus: the 278
    // documentation bytes assembled from the per-field plurality of the last blocks; votes: per field, blocks that agree
    // with it; stats: {window, blocks0 received, parity ok, bit errors vs consensus, bits compared}; productInfo: five ints
    // per product (block id, product id, word size, words, blocks received) with its latest payload in productPayloads.
    void gvarTelemetryUpdated(const QByteArray& block0, const QByteArray& consensus, const QVector<int>& votes,
                              const QVector<int>& stats, const QVector<int>& productInfo, const QList<QByteArray>& productPayloads);
    // Full DOC payload as received from the current downlink frame. The payload
    // is accompanied by the sector CRC result so viewers can distinguish raw
    // bytes from integrity-accepted fields.
    void docPayloadUpdated(const QByteArray& payload, bool crcPassed);
    // Complete 3,200-byte orbit/attitude telemetry block reconstructed from
    // CRC-accepted live DOC subcommutation groups 0..24.
    void orbitAttitudeUpdated(const QByteArray& block);
    // Emitted only for CRC-accepted DOC records whose satellite ID and
    // subsatellite longitude agree. Five consecutive records are required.
    void satelliteIdentityUpdated(int satelliteId, double subSatelliteLongitudeDeg,
                                  int confirmations, bool confirmed);
    // Emitted only after a complete Calibration-2 block received in this run
    // passes the decoder's physical-range/reference checks.
    void calibration2Updated(const QByteArray& block);
    void calibration2Reset();
    void manamProgress(int revision, int receivedGroups);
    void manamUpdated(int revision, const QByteArray& scheduleText);
    void manamReset();
    // The whole receive chain, stage by stage, about four times a second while samples flow
    // (built in nadirstationdiag.cpp, shown on the Decoder page).
    void pipelineSnapshot(const nadirstation::diag::Snapshot& snapshot);

private:
    struct DeframedFrame {
        QByteArray bits;
        std::vector<uint8_t> reliability;
        uint64_t syncSymbolPosition = 0;
        // The demodulated symbols of the frame's data part (positive = 0, scaled so a clean
        // symbol is about 1). Empty when the caller had only hard decisions.
        std::vector<float> soft;
    };

    struct DecodedSector {
        uint32_t id = 0;
        std::vector<uint8_t> dataBits;
        uint16_t receivedCrc = 0;
        uint16_t calculatedCrc = 0;
        bool crcOk = false;
        bool idOk = false;
        bool fillerOk = false;
        int fillerErrors = 0;
        bool strongFiller = false;   // the known-zero filler is far from random: the sector is aligned
        bool softRecovered = false;  // payload replaced by the soft-decision estimate
    };

    struct Line {
        int lineNumber = -1;
        int reportedDocLineNumber = -1;
        uint64_t syncSymbolPosition = 0;   // PN sync position in the symbol stream, for numbering
        QDateTime time;
        std::array<QVector<quint16>, 4> vis;
        std::array<QVector<quint16>, 4> ir;
        std::array<bool, 4> irValid{{false, false, false, false}};
        bool valid = false;
        bool lineNumberEstimated = false;
        bool documentCounterValid = false;
        int crcGood = 0;
        int crcTotal = 0;
        int crcCorrected = 0;
        int scanMode = -1, scanStatus = -1, frameFlag = -1, pictureFlag = -1;
        int setLine = -1, resetLine = -1, scanCount = -1, westHorizon = -1, eastHorizon = -1;
        int syncStatus = -1, bitErrors = -1, satelliteId = -1, calibrationId = -1;
        int sensorMask = -1;
    };

    // Finds the frame boundaries of the S-VISSR bit stream and cuts the frames out of it.
    //
    // The stream is scrambled by a free-running 15-bit sequence (x^15 + x^14 + 1), and the sync
    // marker is only readable in the scrambler's own register. The locker therefore runs a local
    // generator beside the stream and keeps a score of how well that generator predicts the
    // received bits: the score rises by one for every bit it predicted, falls by two for every bit
    // it missed, and when it reaches zero the generator is reloaded from the last received bits.
    // The marker is looked for in the generator's register only, and only believed while the score
    // is high.
    class SvissrFrameLocker {
    public:
        std::vector<DeframedFrame> consume(const std::vector<uint8_t>& bits,
                                           const std::vector<uint8_t>& reliability,
                                           const std::vector<float>* soft = nullptr);
        void reset();
        bool locked() const { return collecting_; }
        uint64_t symbolsSeen() const { return symbolsSeen_; }
    private:
        void followGenerator(uint8_t bit);
        void trackSymbol(uint8_t bit);
        bool acquireFromSymbols();
        bool assistedPhaseHolds();
        bool markerInGenerator() const;
        void takeBit(uint8_t bit, uint8_t trust, const float* soft = nullptr);
        bool frameIsWhole() const;
        void beginFrame(uint64_t position);
        DeframedFrame releaseFrame();
        void dropFrame();

        uint64_t received_ = 0;      // the last 64 received bits
        uint64_t generator_ = 0;     // register of the local generator
        int score_ = 1;
        bool collecting_ = false;    // a frame is being cut out
        int bitsTaken_ = 0;          // bits of the current frame so far
        int bitsInByte_ = 0;
        uint8_t partialByte_ = 0;
        QByteArray frameBytes_;
        std::vector<uint8_t> frameTrust_;
        std::vector<float> frameSoft_;
        uint64_t symbolsSeen_ = 0;
        uint64_t frameStart_ = 0;    // stream position of the marker that opened the frame

        // Acquisition from the symbols themselves. The stream the locker is fed has been
        // differentially decoded, which turns every symbol error into two bit errors, so the
        // generator is hard to start at low signal-to-noise ratio even though the preamble is
        // long and exactly known. Integrating the bits again gives back the symbols, and in the
        // preamble those are a fixed periodic sequence: a correlation against it finds the
        // generator's phase with the single error rate instead of the doubled one.
        std::array<uint64_t, 8> symbolRing_{};   // the last 512 symbols, indexed by position mod 512
        uint64_t symbolCount_ = 0;
        uint8_t symbolState_ = 0;                // the running integral of the bits
        int sinceSearch_ = 0;
        bool assisted_ = false;                  // the generator was set from a correlation peak
        int assistedBitsLeft_ = 0;
        int assistedRun_ = 0;                    // symbols the correlation has held for, counted from the first peak
        int assistedWeakChecks_ = 0;
        int assistedSinceCheck_ = 0;
        uint32_t assistedIndex_ = 0;             // position of the newest symbol in the periodic sequence
    };

    void resetDecoderState();
    void configurePsk();
    void resetGk2aLinkForRetune();
    void setEnabledImpl(bool enabled);
    void processSamplesCore(const std::shared_ptr<std::vector<std::complex<float>>>& samples);
    void beginObservation();

    // The per-spacecraft folder products land under.
    QString spacecraftFolder() const;
    void processBits(const std::vector<uint8_t>& bits);
    void processBitsWithDeframer(const std::vector<uint8_t>& bits,
                                 const std::vector<uint8_t>& reliability,
                                 SvissrFrameLocker& locker,
                                 const std::vector<float>* soft = nullptr);
    bool decodeFrame(const DeframedFrame& frame, Line& line, QString* detail);
    bool parseFrame(const QByteArray& raw, const std::vector<uint8_t>& reliability,
                    uint64_t syncSymbolPosition, Line& line, QString* detail,
                    const std::vector<float>* soft = nullptr);
    void feedLine(Line&& line);
    void renumberLinesBySync();
    void startPictureCycle(Line& firstLine);
    void finishPictureCycle();
    void resetManamAssembly();
    void resetCalibrationAssembly();
    void writeCalibrationProductsToSession();
    bool hasCompleteDiskCoverage() const;
    void tryCompleteImage(bool finalPass);
    QString writeSessionManifest(bool fullDisk, const QList<QString>& paths, int minLine, int maxLine,
                                 int restoredScanLines, int repairedBlankRows,
                                 int repairedImpulsePixels) const;

    static uint8_t getBit(const QByteArray& data, std::size_t bitPos);
    static uint32_t readBits(const QByteArray& data, std::size_t bitPos, int bitCount);
    static void appendBitsToByteArray(const std::vector<uint8_t>& bits, QByteArray& out);
    static uint16_t crc16_1021(const std::vector<uint8_t>& bits);
    static QVector<quint16> unpackSamples(const std::vector<uint8_t>& bits, int count, int bitsPerSample);
    static QDateTime parseDocTime(const std::vector<uint8_t>& doc);
    static int parseReportedLineNumber(const std::vector<uint8_t>& doc);

    QByteArray derandomize(const QByteArray& frame) const;

    // Continuous demodulation state across IQ blocks.
    uint8_t nrzmLastBit_ = 0;
    uint8_t nrzmLastReliability_ = 255;
    nadirstation::dsp::PskDemod pskDemod_;
    bool fy2RectMatched_ = true;   // FY-2: integrate-and-dump matched filter instead of RRC
    double symbolRate_ = 660000.0;   // FY-2 S-VISSR; GK-2A LRIT needs 128000
    std::vector<std::complex<float>> dspAccumulator_;
    bool firstDspWindow_ = true;

    // Lightweight display-only demodulator. The low-SNR decoder below keeps its
    // 4 MiS acquisition windows; this path exists only to keep the constellation
    // visually smooth while receiving. It never feeds the deframer.
    std::vector<std::complex<float>> displayAccumulator_;
    quint64 displaySamplesSinceUpdate_ = 0;

    // Bounded ingress queue. Capture threads never perform DSP; they only push
    // shared_ptr blocks here. The decoder QThread drains the queue separately.
    mutable std::mutex ingressMutex_;
    std::condition_variable ingressCv_;
    std::deque<std::shared_ptr<std::vector<std::complex<float>>>> ingressQueue_;
    std::atomic<std::size_t> ingressQueuedSamples_{0};
    // One line of log the first time a block is dropped, rather than one per block.
    bool ingressDropReported_ = false;
    std::atomic<bool> ingressBackpressureEnabled_{false};
    std::atomic<unsigned> ingressBackpressureEpoch_{0};   // bumped by every setIngressBackpressureEnabled()
    static constexpr std::size_t MAX_INGRESS_SAMPLES = 16u * 1024u * 1024u;
    QTimer* ingressTimer_ = nullptr;
    // HRIT demodulation runs on its own thread, taking blocks straight off ingressQueue_, so neither
    // the capture path nor the Qt event loop does the 3 Msym/s filtering. Started when a HRIT
    // reception starts, joined (after draining what is queued) when it stops.
    void startHritDsp();
    bool hritDspRunningNow();
    void stopHritDsp();
    void hritDspLoop();
    std::thread hritDspThread_;
    std::condition_variable dspCv_;
    std::mutex hritDspMutex_;     // held while a block is processed; the rate/parameter setters take it too
    bool dspStop_ = true;
    bool hritDspRunning_ = false;   // guarded by ingressMutex_; read by the capture path

    QString outputRoot_ = QStringLiteral("NadirStation_DATA");
    QString sessionDir_;
    std::atomic<bool> enabled_{false};
    // Set while setEnabled(false) runs: new IQ is refused at once, so the drain only has to empty
    // what was already queued. Without it a receiver that keeps running refilled the queue as fast
    // as it was drained, and Stop Decode never returned -- the decoder thread was stuck, and the
    // next blocking call from the GUI (start of a WAV test, closing the window) froze with it.
    std::atomic<bool> stopping_{false};
    double sampleRate_ = 6000000.0;
    float rrcAlpha_ = 0.5f;
    float pllBandwidth_ = 0.02f;
    QString sourceLabel_ = QStringLiteral("Unknown");
    // Which spacecraft the operator selected, so products land under a folder naming it.
    QString satelliteName_;

    SvissrFrameLocker frameLocker_[2];
    std::array<QByteArray, 25> manamChunks_{};
    std::array<QByteArray, 25> orbitAttitudeChunks_{};
    std::array<bool, 25> orbitAttitudeChunkSeen_{};
    int orbitAttitudeGroupsSeen_ = 0;
    std::array<int, 25> manamChunkScores_{};
    std::array<bool, 25> manamChunkSeen_{};
    std::array<std::array<QByteArray, 8>, 25> calibrationCopies_{};
    std::array<std::array<bool, 8>, 25> calibrationCopySeen_{};
    std::array<int, 25> calibrationCopyCounts_{};
    std::array<QVector<float>, 5> calibrationLuts_{}; // VIS, IR1, IR2, IR3, IR4
    std::array<QImage, 4> finalizedIrCounts10_{}; // retained in memory for late LUT arrival; never emitted as PNG
    QByteArray calibrationBlock_;
    int calibrationGroupsSeen_ = 0;
    bool calibrationLutReady_ = false;
    // Where the active table came from: "live" (all 25 DOC groups this run),
    // "backup" (a previously validated block), or "factory" (the bundled copy).
    // Low SNR can lose Calibration-2 groups, and without a table no brightness
    // temperature can be produced at all, so a fallback is always available.
    QString calibrationSource_;
    QByteArray validatedCalibrationBlock_;
    bool loadFallbackCalibration();
    void rememberValidatedCalibrationBlock(const QByteArray& block);
    QString calibrationBackupPath() const;
    int manamRevision_ = -1;
    int satelliteIdentityCandidateId_ = -1;
    double satelliteIdentityCandidateLongitude_ = 0.0;
    int satelliteIdentityConfirmations_ = 0;
    int manamGroupCount_ = 0;
    quint32 manamGroupMask_ = 0;
    QByteArray lastCompleteManam_;
    std::vector<std::complex<float>> constellationBuffer_;
    int goodFrames_ = 0;
    int badFrames_ = 0;
    int lock_ = 0;

    // The GK-2A LRIT chain. The application had none: every block went to the S-VISSR deframer
    // regardless of which spacecraft was selected, so lock_ was never assigned for GK-2A and the
    // badge read NOSYNC forever. This is that path.
    nadirstation::gk2a::LritDecoder gk2aLrit_;
    nadirstation::gk2a::LritFileAssembler gk2aLritAssembler_;

    // ELEKTRO-L GGAK service (selected by satellite name). Independent of the FY-2 / GK-2A chain.
    bool isGgak() const { return satelliteName_.contains(QStringLiteral("GGAK"), Qt::CaseInsensitive); }

    // ELEKTRO-L LRIT (BPSK 294 ksym/s) and HRIT (OQPSK 1.15 Msym/s), both at 1691 MHz. Link as
    // SatDump's Elektro_Arktika pipelines (rate-1/2 K=7, RS(255,223) depth 4), except the HRIT
    // matched filter: RRC 0.85 instead of 0.35, about 0.4 dB better on the recordings (mainwindow.cpp);
    // 1024-byte CADUs; the link layers are GK-2A's, the xRIT and image layer is elektroxrit.*.
    bool isElektro() const { return satelliteName_.startsWith(QStringLiteral("ELEKTRO-L")) && !isGgak(); }
    bool isElektroHrit() const { return isElektro() && satelliteName_.contains(QStringLiteral("HRIT")); }
    void processElektroSamples(const std::vector<std::complex<float>>& iq);
    void handleElektroFrame(nadirstation::gk2a::Cadu&& frame);
    void publishElektro();
    void finalizeElektro();
    nadirstation::gk2a::LritDecoder elektroLrit_;          // LRIT: soft BPSK symbols from pskDemod_
    nadirstation::gk2a::HritDecoder elektroHrit_;          // HRIT: OQPSK pairs, on its own thread
    nadirstation::dsp::QpskDemod elektroOqpsk_;
    nadirstation::gk2a::LritFileAssembler elektroAssembler_;
    std::unique_ptr<nadirstation::elektro::XritProducts> elektroProducts_;
    double elektroSnrY1_ = 0.0, elektroSnrY2_ = 0.0;
    // Loss of lock: stream time (from the sample count, so a file played fast behaves like the live
    // signal), when a CADU last passed Reed-Solomon, and the re-acquisition attempts since then.
    double elektroStreamMs_ = 0.0;
    double elektroLastGoodMs_ = 0.0;
    double elektroLastReacqMs_ = 0.0;
    std::size_t elektroGoodSeen_ = 0;
    int elektroReacq_ = 0;
    bool elektroEverLocked_ = false;
    void elektroCheckLock(std::size_t goodNow);
    qint64 elektroProgressMs_ = 0;
    std::size_t elektroFiles_ = 0;
    // Loops and acquisition settled with added-noise tests on the 2026-11-15 HRIT recording
    // (simulated threshold test): every CADU passes RS down to Es/N0 2.95 dB, 99 % at 2.5 dB.
    // The averaged, range-limited carrier search is what moved the threshold: with a single
    // fourth-power FFT over the whole band a spur won the search below Es/N0 5 dB.
    static constexpr float kElektroCarrierLoopBw = 0.0003f;
    static constexpr float kElektroTimingLoopBw = 0.0003f;
    static constexpr int kElektroAcqBlocks = 8;
    static constexpr float kElektroAcqMaxHz = 80000.0f;
    // Polar HRPT (MetOp AHRPT, METEOR HRPT): nadirstationpolar.cpp
    bool isMetop() const { return satelliteName_.startsWith(QStringLiteral("MetOp"), Qt::CaseInsensitive); }
    bool isMeteorHrpt() const { return satelliteName_.startsWith(QStringLiteral("METEOR"), Qt::CaseInsensitive); }
    bool isPolar() const { return isMetop() || isMeteorHrpt(); }
    void configurePolar();
    void resetPolar();
    void processPolarSamples(const std::vector<std::complex<float>>& iq);
    void finalizePolarPass(const QString& why);
    void publishPolarPreview(bool force);
    std::unique_ptr<nadirstation::polar::Imager> polarImager_;
    nadirstation::polar::AvhrrReader avhrr_;
    nadirstation::polar::MetopInstruments metopInstr_;   // MHS, AMSU-A, IASI imager
    nadirstation::polar::MtvzaReader mtvza_;
    nadirstation::polar::MeteorDemod meteorDemod_;
    nadirstation::polar::MeteorDeframer meteorDeframer_;
    nadirstation::polar::MsuMrReader msumr_;
    nadirstation::dsp::QpskDemod metopDemod_;
    nadirstation::gk2a::HritDecoder metopDecoder_;
    double polarStreamMs_ = 0.0;
    double polarLastLineMs_ = 0.0;
    int polarLinesSeen_ = 0;
    qint64 polarPreviewMs_ = 0;
    qint64 polarProgressMs_ = 0;
    double polarSnrY1_ = 0.0, polarSnrY2_ = 0.0;
    int polarPasses_ = 0;
    std::size_t polarFramesBefore_ = 0;
    void processGgakSamples(const std::vector<std::complex<float>>& iq);
    void resetGgak();
    void storeGgakFrame(const nadirstation::ggak::Frame& frame);
    nadirstation::ggak::Demodulator ggakDemod_;
    double ggakRateScaleLogged_ = 1.0;   // last sample-rate correction reported to the log
    bool lowRateReported_ = false;       // "sample rate below the configured PSK bandwidth" already said
    nadirstation::ggak::Deframer ggakDeframer_;
    qulonglong ggakGood_ = 0, ggakBad_ = 0;
    qint64 ggakLastGoodMs_ = 0;
    qint64 ggakLastStatusMs_ = 0;
    qint64 ggakStreamMs_ = 0;
    double ggakStreamMsExact_ = 0.0;
    QString ggakDir_;
    int ggakLoggedSync_ = -1;          // last sync state written to the runtime log
    nadirstation::ggak::UnitIdentifier ggakUnit_;   // which ELEKTRO-L unit, from the frame-ID fingerprint
    std::string ggakUnitLogged_;            // last unit label written to the runtime log
    bool ggakLoggedTracking_ = false;  // last acquisition state written to the runtime log
    qint64 lastGk2aSyncMs_ = 0;        // when a CADU last carried its sync marker
    qint64 lastGk2aRsOkMs_ = 0;        // when one last passed its parity check
    // When the "dropped before reassembly" line was last written. A frame that fails its parity
    // check is not fed to the packet layer; the count is worth having, once every ten seconds of
    // stream time rather than once per block.
    qint64 lastGk2aParityRejectLogMs_ = 0;
    qint64 gk2aLastFrameMs_ = 0;       // when a frame was last found, for the re-acquire timer
    // The link timers run on stream time, not wall-clock time. Processing a file takes far less
    // than the signal it contains -- this recording is 134.9 seconds of signal in about 19 seconds
    // of wall clock -- so timers measured against the wall clock expire about seven times too
    // slowly, and a receiver that had lost the signal reported that it had not.
    qint64 gk2aStreamMs_ = 0;
    double gk2aStreamMsExact_ = 0.0;
    double gk2aSampleRate_ = 0.0;
    bool gk2aEverHadFrame_ = false;
    bool gk2aReacquired_ = false;      // reported once, so the log says when it happened
    int gk2aRecoveryAttempt_ = 0;      // FEC resync, then bounded carrier reacquisition
    float gk2aRecoveryReferenceCfoHz_ = 0.0f;
    float gk2aLastGoodCfoHz_ = 0.0f;   // carrier estimate captured while CADU sync was healthy
    std::size_t gk2aFilesWritten_ = 0;
    // The decryption keys, loaded on first use, and the disk being assembled strip by strip. The
    // service sends the full disk as ten horizontal strips of 220 lines, each its own file, so a
    // picture only exists once they are stacked.
    std::map<int, std::array<std::uint8_t, 8>> gk2aKeys_;
    bool gk2aKeysLoaded_ = false;
    QString gk2aKeyPath_ =
        QStringLiteral("resources/gk2a/EncryptionKeyMessage.bin");   // not shipped: see README (GK-2A keys)
    QImage gk2aDisk_;
    // The observation's count-to-kelvin table, taken from the first file of the observation. Every
    // file of one observation carries the same one, and it is what turns the pixels into
    // temperatures -- without it the calibrated products would be instrument counts coloured as if
    // they were weather.
    nadirstation::gk2a::Calibration gk2aCalibration_;
    nadirstation::gk2a::LritNavigation gk2aNavigation_;
    QString gk2aObservationDir_;   // where an observation's products belong
    QString gk2aActiveStamp_;     // stamp owning the in-memory disk and strip index
    std::set<int> gk2aStrips_;
    // The observation a strip belongs to, so the finished one can be handed to the image list
    // exactly once. Firing per strip would run the product generation ten times for one result.
    QString gk2aPendingReady_;      // stamp of the observation not yet imported
    int gk2aPendingExpected_ = 0;   // strips it announced, so completeness is knowable

    // The service is chosen by symbol rate, which is what the satellite selection sets.
    bool isHrit() const { return std::fabs(symbolRate_-3000000.0)<1.0; }
    // GOES-N/O/P GVAR: BPSK 2.11136 Msym/s. Its own receive chain (gvarengine.h) runs on the same DSP thread as HRIT.
    bool isGvar() const { return std::fabs(symbolRate_-2111360.0)<1.0; }
    std::unique_ptr<nadirstation::gvar::Engine> gvar_;
    void processGvarSamples(const std::vector<std::complex<float>>& iq);
    void publishGvarUpdates();
    void publishGvarTelemetry(bool force);
    qint64 lastGvarTelemetryMs_ = 0;
    void finalizeGvarObservation();
    // GOES-N/O/P Sensor Data (1676 MHz): imager 2.6208 Mbit/s BPSK + sounder, goessd.h
    bool isGoesSd() const { return std::fabs(symbolRate_-2620800.0)<1.0; }
    std::unique_ptr<nadirstation::goessd::Engine> goessd_;
    void processGoesSdSamples(const std::vector<std::complex<float>>& iq);
    void publishGoesSdUpdates();
    void finalizeGoesSdObservation();
    uint64_t goesSdLastSyncs_ = 0;
    qint64 goesSdLastSyncMs_ = 0;
    nadirstation::gk2a::HritDecoder hritDecoder_;
    // HRIT has its own QPSK demodulator; pskDemod_ stays the BPSK one for FY-2 / LRIT. Loop
    // bandwidths are the ones the HRIT simulation settled on (2026-11-01): a carrier loop at
    // 0.004 of the symbol rate slipped cycles below Es/N0 2.5 dB, 0.001 holds to the code's limit.
    nadirstation::dsp::QpskDemod hritDemod_;
    static constexpr float kHritCarrierLoopBw = 0.0003f;
    static constexpr float kHritTimingLoopBw = 0.0002f;
    nadirstation::gk2a::LritFileAssembler hritAssembler_;
    std::unique_ptr<nadirstation::gk2a::HritImages> hritImages_;
    std::size_t hritFilesCompleted_ = 0;
    std::uint64_t hritIqSamples_ = 0;
    qint64 lastHritProgressStatusMs_ = 0;
    void processHritSamples(const std::vector<std::complex<float>>& iq);
    void handleHritFrame(nadirstation::gk2a::Cadu&& frame);
    void publishHritPreview();
    // Close the HRIT observation in progress (channels, L2, JSON) and hand it to the image page.
    void finalizeHritObservation();
    void drainQueuedSamplesLimited(std::size_t maxBlocks);
public:
    void setFy2RectMatched(bool on) { fy2RectMatched_ = on; }
private:
    bool isGk2a() const { return std::fabs(symbolRate_ - 128000.0) < 1.0; }
    void processGk2aSymbols(const std::vector<std::complex<float>>& symbols);
    void writeGk2aLritFile(const nadirstation::gk2a::LritFile& file);
    // Hands over the observation in progress, if any. Called when a new observation starts and
    // when decoding stops, because a partial reception has no other end: eight strips of ten never
    // completes and has no successor to push it out.
    // Hands over the observation in progress. `final` says whether the observation is finished:
    // on a re-acquisition it is not, and the products are not rendered then, because rendering
    // them on the decode thread is what stopped the receiver re-acquiring at all.
    void flushGk2aObservation(bool final);
    // Writes the decrypted payload of an additional-data file beside the file itself: text as
    // text, a chart as an image, anything else as bytes.
    void writeGk2aAdditionalContent(const nadirstation::gk2a::LritFile& file,
                                    const std::vector<std::uint8_t>& payload,
                                    const QString& rawPath,
                                    const QString& keyName);
    // Evaluates the link state from the frame timestamps. Called for every block, including those
    // that produced no symbols, because a silent receiver is the case this has to notice.
    void refreshGk2aLinkState(qint64 now);
    // Publishes a symbol set to the constellation display.
    void publishConstellation(const std::vector<std::complex<float>>& symbols);
    float softQuality_ = 0.0f;
    float carrierOffsetHz_ = 0.0f;
    int polarityHint_ = -1;
    qint64 lastConstellationEmitMs_ = 0;
    qint64 lastStatsEmitMs_ = 0;
    qint64 lastHritNoFrameStatusMs_ = 0;
    qint64 lastDspLogMs_ = 0;
    qint64 lastSnrMetricEmitMs_ = 0;
    float peakDemodSnr_ = 0.0f;
    // GOES GVAR lock indication: the sync count and when it last went up (a sync word is the proof of lock)
    uint64_t gvarLastSyncs_ = 0;
    // ---- pipeline diagnostics (nadirstationdiag.cpp) ----
    void pipelineTick(std::size_t samples, double dspSeconds) noexcept;
    void resetPipelineDiag();
    nadirstation::diag::Snapshot buildPipelineSnapshot(double windowSignalSeconds, double windowWallSeconds,
                                                      double windowDspSeconds);
    double diagRate(const QString& key, double value, double seconds);
    std::atomic<uint64_t> ingressDroppedSamples_{0};   // oldest blocks thrown away with backpressure off
    std::atomic<uint64_t> ingressWaitMs_{0};           // time the capture thread was held by backpressure
    std::atomic<float> diagSnrDb_{std::numeric_limits<float>::quiet_NaN()};
    float fy2TimingQuality_ = 0.0f;
    qint64 diagStartMs_ = 0;
    qint64 diagLastEmitMs_ = 0;
    double diagWinSignal_ = 0.0, diagWinDsp_ = 0.0;
    double diagStreamSeconds_ = 0.0;
    uint64_t diagDroppedAtLastEmit_ = 0, diagWaitAtLastEmit_ = 0;
    qint64 diagLastDropMs_ = 0;
    qint64 diagLastSyncProgressMs_ = 0;
    double diagLastSyncCount_ = -1.0;
    struct DiagCounter { double value = 0.0; double rate = 0.0; };
    QHash<QString, DiagCounter> diagCounters_;
    qint64 gvarLastSyncMs_ = 0;
    // GK-2A HRIT SNR, same estimator as SatDump: M2M4 moments of the demodulated symbols, running
    // averages with alpha = 0.001 per symbol.
    double hritSnrY1_ = 0.0, hritSnrY2_ = 0.0;
    std::deque<std::pair<qint64, float>> demodSnrHistory_;

    std::vector<Line> recentLines_;
    int sequenceLineNumber_ = 1;
    int lastBuiltLine_ = 0;
    int lastReportedLine_ = 0;
    int lineNumberOffset_ = 0;
    bool docCounterSeen_ = false;
    std::deque<int> docLineContinuityWindow_;
    uint64_t lastReliableDocSyncSymbolPosition_ = 0;
    int lastReliableDocLineNumber_ = -1;
    uint64_t lastAcceptedSyncSymbolPosition_ = 0;
    double lineSymbolPeriodEstimate_ = 401408.0;
    std::deque<double> lineSymbolPeriodHistory_;
    int lineSymbolPeriodObservations_ = 0;
    qint64 lastGoodFrameMs_ = 0;
    uint64_t lastGoodSyncSymbol_ = 0;   // sync position of the last line that passed, for the badge
    bool observationFinalized_ = false;
    bool pictureCycleActive_ = false;
    int pictureOffFrameCount_ = 0;
};

// Calibration-2 block decoding and validation. These sit in namespace nadirstation at
// file scope rather than inside the class because the L1B export tool and the
// verification tool both need to judge a block, and a second copy of the rules
// would drift apart from the receiver's.
//
// decodeCalibration2() rejects a block that is malformed, truncated, implausible
// as a table (non-monotonic, or with implausibly large jumps), or that disagrees
// with the verified reference shipped in resources/orbits. The last test is what
// makes a low signal-to-noise live capture safe to use: sparse damage that a
// range count cannot see shows up immediately as a deviation from the reference.
namespace nadirstation {
bool decodeCalibration2(const QByteArray& block, std::array<QVector<float>, 5>& luts);

// The same parse without the checks, used to load the reference itself.
void parseCalibration2Tables(const QByteArray& block, std::array<QVector<float>, 5>& luts);

// The reference tables, or nullptr when the resource is missing. Loaded once.
const std::array<QVector<float>, 5>* referenceCalibrationTables();
} // namespace nadirstation
