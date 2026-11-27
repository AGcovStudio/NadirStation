#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <complex>
#include <memory>
#include <thread>
#include <vector>
#include <algorithm>
#include <QtGlobal>
#include "dsp.h"

// Plays a recording into the decoder: a 2-channel 16-bit PCM WAV, or a headerless interleaved I/Q
// file (.cs16 signed 16-bit, .cs8 signed 8-bit, .cu8 unsigned 8-bit as written by rtl_sdr).
class WavReceiver : public QObject {
    Q_OBJECT
public:
    enum class Format { Wav, CS16, CS8, CU8 };
    // Format from the file name; a file that is not one of the raw I/Q extensions is read as WAV.
    static Format formatForPath(const QString& path);
    static bool isRawIq(const QString& path) { return formatForPath(path) != Format::Wav; }
    static const char* formatName(Format f);
    // Sample rate a raw file states about itself: a "<name>.txt" (or "<name>.cs16.txt") written
    // next to it with a "rate <Hz>" field, or a rate with its unit in the file name
    // ("..._2500000SPS_...", "2.5MSPS", "2500ksps"). 0 when neither says.
    static double rateFromSidecar(const QString& path);
    static double rateFromName(const QString& path);
    // Width of the strongest signal in a raw I/Q recording, in Hz: the run of spectrum bins around
    // the peak that stand more than half-way (in dB) between the noise floor and the peak, averaged
    // over a few hundred milliseconds near the start of the file. 0 when it cannot be measured (WAV,
    // unknown rate, no clear signal). Used to tell ELEKTRO-L LRIT (about 0.3 MHz) from HRIT
    // (about 1.15 MHz), which share 1691 MHz.
    static double occupiedBandwidthHz(const QString& path, double sampleRate);

    explicit WavReceiver(QObject* parent = nullptr);
    ~WavReceiver() override;

    void setFilePath(const QString& path) { filePath_ = path; }
    void setPlaybackFactor(double factor) { playbackFactor_ = factor > 0.0 ? factor : 1.0; }
    // 1.0 = full file, 0.5 = first half, 0.25 = first quarter.
    void setFraction(double fraction) { fraction_ = std::clamp(fraction, 0.01, 1.0); }
    // Rate used for a raw I/Q file that does not state its own (the Sample Rate box).
    void setRawFallbackRate(double hz) { rawFallbackRate_ = hz; }
    void setSpectrumSettings(std::size_t fftSize, nadirstation::dsp::SpectrumWindow window) {
        fftSize_.store(fftSize); fftWindow_.store(static_cast<int>(window));
    }

    bool prepare();
    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }
    double sampleRate() const { return sampleRate_; }
    Format format() const { return format_; }
    // Where the sample rate came from: "WAV header", "sidecar", "file name" or "Sample Rate box".
    QString rateSource() const { return rateSource_; }
    qint64 totalFrames() const { return totalFrames_; }

signals:
    void samplesReady(const std::shared_ptr<std::vector<std::complex<float>>>& samples);
    void metricsReady(float snr, float peakSnr, float avgSnr1s, double sampleRate);
    void spectrumReady(const std::vector<float>& db, double sampleRate);
    void statusMessage(const QString& text);
    void deviceConnected(bool connected);
    void finished();

private:
    bool openWav();
    bool openRaw();
    void run();

    QString filePath_;
    double playbackFactor_ = 1.0;
    double fraction_ = 1.0;
    double sampleRate_ = 0.0;
    double rawFallbackRate_ = 2500000.0;
    Format format_ = Format::Wav;
    QString rateSource_;
    qint64 dataOffset_ = 0;
    qint64 totalFrames_ = 0;
    qint64 frameBytes_ = 0;
    qint64 endFrame_ = 0;
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> fftSize_{2048};
    std::atomic<int> fftWindow_{static_cast<int>(nadirstation::dsp::SpectrumWindow::Hann)};
    bool prepared_ = false;
    std::thread worker_;
};
