#pragma once

#include <QObject>
#include <QString>
#include <atomic>
#include <chrono>
#include <complex>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>
#include <vector>
#include "dsp.h"
#include "iqmetatypes.h"
#include "spectrumtap.h"

// HackRF One receive path (issue #2). libhackrf is loaded at run time, the same way the RTL-SDR
// driver is, so the program builds and runs without it and only this input needs it: Windows looks
// for hackrf.dll next to the program (or in the PothosSDR / SDRangel folders, or the path in
// NADIRSTATION_HACKRF_DLL), Linux for libhackrf.so.0.
class HackRfReceiver : public QObject {
    Q_OBJECT
public:
    explicit HackRfReceiver(QObject* parent = nullptr);
    ~HackRfReceiver() override;

    // gainDb is the panel's single gain figure, spread over the RF amplifier (+14 dB), the LNA
    // (0-40 dB, 8 dB steps) and the baseband VGA (0-62 dB, 2 dB steps).
    void setParameters(double frequencyHz, double sampleRate, double bandwidthHz, double gainDb,
                       unsigned deviceIndex, bool antennaPower);
    void setFrequencyShift(double hz) { frequencyShiftHz_ = hz; }
    void setSpectrumSettings(std::size_t fftSize, nadirstation::dsp::SpectrumWindow window);
    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }
    double sampleRate() const { return sampleRate_; }
    double frequencyHz() const { return frequencyHz_; }

    // Exposed for the stream callback, which is a plain C function.
    int onTransfer(const std::int8_t* bytes, int length);

signals:
    void samplesReady(const std::shared_ptr<std::vector<std::complex<float>>>& samples);
    void metricsReady(float snr, float peakSnr, float avgSnr1s, double sampleRate);
    void spectrumReady(const std::vector<float>& db, double sampleRate);
    void statusMessage(const QString& text);
    void deviceConnected(bool connected);
    void finished();

private:
    void run();

    double frequencyHz_ = 1687500000.0;
    double sampleRate_ = 8000000.0;
    double bandwidthHz_ = 0.0;
    double gainDb_ = 35.0;
    double frequencyShiftHz_ = 0.0;
    unsigned deviceIndex_ = 0;
    bool antennaPower_ = false;

    std::atomic<bool> running_{false};
    std::atomic<bool> gotSamples_{false};
    std::atomic<std::size_t> fftSize_{2048};
    std::atomic<int> fftWindow_{static_cast<int>(nadirstation::dsp::SpectrumWindow::Hann)};
    std::thread worker_;

    // Stream callback state (libhackrf's transfer thread only).
    nadirstation::dsp::SpectrumTap spectrumTap_;
    nadirstation::dsp::FrequencyShifter shifter_;
    std::deque<std::pair<std::chrono::steady_clock::time_point, float>> history_;
    float peak_ = -120.0f;
    std::chrono::steady_clock::time_point lastMetric_{};
};
