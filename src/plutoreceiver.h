#pragma once
#include <QObject>
#include <QString>
#include <complex>
#include <cstdint>
#include <memory>
#include <thread>
#include <atomic>
#include <vector>
#include <deque>
#include <chrono>
#include "dsp.h"
#include "spectrumtap.h"

class PlutoReceiver : public QObject {
    Q_OBJECT
public:
    explicit PlutoReceiver(QObject* parent = nullptr);
    ~PlutoReceiver() override;

    void setParameters(double frequencyHz, double sampleRate, double bandwidthHz, double gainDb);
    void setIp(const QString& ip);
    void setSpectrumSettings(std::size_t fftSize, nadirstation::dsp::SpectrumWindow window);
    // Change the RX gain while receiving: applied by the worker between two buffer refills. The
    // value passed to setParameters only takes effect on the next start().
    void setGainLive(double gainDb) { pendingGain_.store(gainDb); }

    bool start();
    void stop();
    bool isRunning() const { return running_.load(); }
    bool readyToStart() const { return !workerBusy_.load(); }
    // As last given to setParameters (GUI thread).
    double sampleRate() const { return sampleRate_; }
    double frequencyHz() const { return frequencyHz_; }
    // Seconds in which the Pluto reported a buffer overrun (lost samples) since the last start.
    std::uint64_t overflowCount() const { return overflowCount_.load(); }

signals:
    void samplesReady(const std::shared_ptr<std::vector<std::complex<float>>>& samples);
    void metricsReady(float snr, float peakSnr, float avgSnr1s, double sampleRate);
    void spectrumReady(const std::vector<float>& db, double sampleRate);
    void statusMessage(const QString& text);
    void deviceConnected(bool connected);

private:
    void run();
    bool configurePluto();
    void releaseDevice();

    QString ip_ = QStringLiteral(NADIRSTATION_DEFAULT_PLUTO_IP);
    double frequencyHz_ = 1687500000.0;
    double sampleRate_ = 6000000.0;
    double actualSampleRate_ = 6000000.0;
    double bandwidthHz_ = 2000000.0;
    double gainDb_ = 35.0;

    std::deque<std::pair<std::chrono::steady_clock::time_point, float>> snrHistory_;
    float peakSnr_ = -120.0f;
    std::chrono::steady_clock::time_point lastMetricEmit_{};

    std::atomic<bool> running_{false}, workerBusy_{false};
    std::atomic<std::uint64_t> overflowCount_{0};
    std::atomic<double> pendingGain_{-1000.0};   // below -999: nothing pending
    std::atomic<std::size_t> fftSize_{2048};
    std::atomic<int> fftWindow_{static_cast<int>(nadirstation::dsp::SpectrumWindow::Hann)};
    std::thread worker_;
    std::thread retired_;   // a stopped worker still finishing a libiio call
    nadirstation::dsp::SpectrumTap spectrumTap_;   // used only on the worker thread

#ifdef NADIRSTATION_HAS_LIBIIO
    struct iio_context* ctx_ = nullptr;
    struct iio_device* phy_ = nullptr;
    struct iio_device* rxdev_ = nullptr;
    struct iio_channel* rx0_ = nullptr;
    struct iio_channel* rx1_ = nullptr;
    struct iio_channel* lo_ = nullptr;
    struct iio_channel* rxPhy_ = nullptr;   // ad9361-phy voltage0 input: gain lives here
    struct iio_buffer* buffer_ = nullptr;
#endif
};
