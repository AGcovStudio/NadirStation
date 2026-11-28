#include "plutoreceiver.h"
#include "dsp.h"
#include <QByteArray>
#include <cmath>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <cstdint>
#include <deque>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#ifdef NADIRSTATION_HAS_LIBIIO
#include <iio.h>
#endif

PlutoReceiver::PlutoReceiver(QObject* parent) : QObject(parent) {}
PlutoReceiver::~PlutoReceiver() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
    if (retired_.joinable()) retired_.join();
}

#ifdef NADIRSTATION_HAS_LIBIIO
namespace {
// The AD9361 refuses a hardwaregain above the ceiling of its gain table, which depends on the LO frequency
// (about 71 dB around 1.7 GHz, 73 dB below 1.3 GHz). A request above it must not stop the receiver from
// starting: step down by half a dB until the board accepts it. Returns the gain that was set, or NaN if
// even the lowest step failed (a real error, such as a lost connection).
double writeRxGain(iio_channel* rx, double wanted, bool* limited) {
    double g = wanted;
    for (int step = 0; step < 24; ++step) {
        if (iio_channel_attr_write_double(rx, "hardwaregain", g) >= 0) {
            *limited = step > 0;
            return g;
        }
        if (wanted < 40.0) break;   // not a ceiling problem
        g -= 0.5;
    }
    *limited = false;
    return std::nan("");
}
}  // namespace
#endif

void PlutoReceiver::setParameters(double frequencyHz, double sampleRate, double bandwidthHz, double gainDb) {
    frequencyHz_ = frequencyHz;
    sampleRate_ = sampleRate;
    bandwidthHz_ = bandwidthHz;
    gainDb_ = gainDb;
}

void PlutoReceiver::setIp(const QString& ip) { ip_ = ip.trimmed(); }
void PlutoReceiver::setSpectrumSettings(std::size_t fftSize, nadirstation::dsp::SpectrumWindow window) {
    fftSize_.store(fftSize); fftWindow_.store(static_cast<int>(window));
}

bool PlutoReceiver::configurePluto() {
#ifndef NADIRSTATION_HAS_LIBIIO
    emit statusMessage(QStringLiteral("This build has no libiio: staying in offline simulation mode."));
    emit deviceConnected(false);
    return false;
#else
    const QByteArray uri = QStringLiteral("ip:%1").arg(ip_).toUtf8();
    ctx_ = iio_create_context_from_uri(uri.constData());
    if (!ctx_) {
        emit statusMessage(QStringLiteral("Cannot connect to the PlutoSDR at ip:%1").arg(ip_));
        emit deviceConnected(false);
        return false;
    }

    phy_ = iio_context_find_device(ctx_, "ad9361-phy");
    rxdev_ = iio_context_find_device(ctx_, "cf-ad9361-lpc");
    if (!phy_ || !rxdev_) {
        emit statusMessage(QStringLiteral("Pluto IIO devices incomplete: ad9361-phy / cf-ad9361-lpc missing."));
        emit deviceConnected(false);
        return false;
    }

    lo_ = iio_device_find_channel(phy_, "altvoltage0", true);
    auto* rx = iio_device_find_channel(phy_, "voltage0", false);
    if (!lo_ || !rx) {
        emit statusMessage(QStringLiteral("Pluto RX LO / voltage0 channel not found."));
        emit deviceConnected(false);
        return false;
    }

    auto failAttr = [this](int rc, const QString& what) {
        if (rc < 0) emit statusMessage(QStringLiteral("Pluto setting failed: %1 (rc=%2)").arg(what).arg(rc));
        return rc < 0;
    };
    if (failAttr(iio_channel_attr_write_longlong(lo_, "frequency", static_cast<long long>(std::llround(frequencyHz_ + frequencyShiftHz_))), "LO frequency")) return false;
    if (failAttr(iio_channel_attr_write(rx, "gain_control_mode", "manual"), "gain_control_mode")) return false;
    rxPhy_ = rx;
    pendingGain_.store(-1000.0);   // start() applies gainDb_ below
    {
        bool limited = false;
        const double applied = writeRxGain(rx, gainDb_, &limited);
        if (std::isnan(applied)) { failAttr(-1, "hardwaregain"); return false; }
        if (limited) {
            emit statusMessage(QStringLiteral("Pluto gain limited to %1 dB (the AD9361 accepts no more at this frequency)")
                                   .arg(applied, 0, 'f', 1));
            gainDb_ = applied;
        }
    }
    if (bandwidthHz_ > 0.0) {
        // The AD9361 filter runs from 0.2 to 56 MHz; a manual value outside that is clamped, not refused.
        const long long chosen = std::min(std::max(static_cast<long long>(bandwidthHz_), 200000LL), 56000000LL);
        if (failAttr(iio_channel_attr_write_longlong(rx, "rf_bandwidth", chosen), "rf_bandwidth")) return false;
    } else {
        // "No bandwidth limit": open the analog filter as wide as the sample rate needs. This is written
        // on every start because the Pluto keeps whatever an earlier session left in rf_bandwidth.
        // A rejected value is not fatal; the board then keeps its current filter.
        const long long wide = std::min(std::max(static_cast<long long>(sampleRate_), 200000LL), 56000000LL);
        if (iio_channel_attr_write_longlong(rx, "rf_bandwidth", wide) < 0)
            emit statusMessage(QStringLiteral("Pluto rf_bandwidth could not be opened to %1 MHz; keeping the board's current filter.")
                                   .arg(wide / 1e6, 0, 'f', 3));
    }
    if (failAttr(iio_channel_attr_write_longlong(rx, "sampling_frequency", static_cast<long long>(sampleRate_)), "sampling_frequency")) return false;

    long long readBack = 0;
    if (iio_channel_attr_read_longlong(rx, "sampling_frequency", &readBack) >= 0 && readBack > 0) {
        actualSampleRate_ = static_cast<double>(readBack);
        if (std::fabs(actualSampleRate_ - sampleRate_) > 500.0)
            emit statusMessage(QStringLiteral("Pluto adjusted the sample rate to %1 MS/s (requested %2 MS/s)")
                .arg(actualSampleRate_ / 1e6, 0, 'f', 6).arg(sampleRate_ / 1e6, 0, 'f', 6));
    } else {
        actualSampleRate_ = sampleRate_;
    }

    rx0_ = iio_device_find_channel(rxdev_, "voltage0", false);
    rx1_ = iio_device_find_channel(rxdev_, "voltage1", false);
    if (!rx0_ || !rx1_) {
        emit statusMessage(QStringLiteral("Pluto I/Q RX data channels not found."));
        emit deviceConnected(false);
        return false;
    }
    iio_channel_enable(rx0_);
    iio_channel_enable(rx1_);
    // A 65536-sample buffer is 11 ms at 6 MS/s. When the host is busy for longer than a few of
    // those the Pluto overwrites samples, and the demodulator sees a timing slip and loses sync.
    // At high rates use a larger read and a deeper kernel ring; low rates (FY-2) keep the old size.
    const bool wideBuffer = actualSampleRate_ >= 4.0e6;
    if (wideBuffer) iio_device_set_kernel_buffers_count(rxdev_, 16);
    buffer_ = iio_device_create_buffer(rxdev_, wideBuffer ? (1u << 17) : (1u << 16), false);
    if (!buffer_) {
        emit statusMessage(QStringLiteral("Could not create the Pluto RX IIO buffer."));
        emit deviceConnected(false);
        return false;
    }
    const QString bandwidthText = bandwidthHz_ > 0.0 ? QStringLiteral("%1 MHz BW").arg(bandwidthHz_ / 1e6, 0, 'f', 3)
                                                     : QStringLiteral("no bandwidth limit");
    emit statusMessage(QStringLiteral("PlutoSDR connected: %1 · %2 MS/s · %3")
        .arg(ip_).arg(actualSampleRate_ / 1e6, 0, 'f', 3).arg(bandwidthText));
    emit deviceConnected(true);
    return true;
#endif
}

bool PlutoReceiver::start() {
    if (running_.load()) return true;
    if (!readyToStart()) return false;
    // The previous worker may have ended on its own (no device, a refill error) without anyone
    // joining it. Assigning a new std::thread over a joinable one calls std::terminate, which ended
    // the program with no crash log the second time Start was pressed after a failed connect.
    // A worker retired by stop() may still be leaving libiio; it owns the device until it returns.
    if (retired_.joinable()) retired_.join();
    if (worker_.joinable()) worker_.join();
    running_ = true;
    workerBusy_ = true;
    try {
        worker_ = std::thread([this] {
            run();
            workerBusy_ = false;
        });
    } catch (...) {
        running_ = false;
        workerBusy_ = false;
        throw;
    }
    return true;
}

void PlutoReceiver::stop() {
    // Does not wait for the worker. It can be inside a libiio call that only returns after a
    // network timeout (iio_create_context on an unreachable address, a refill on a dropped link),
    // and joining it here held the GUI for seconds -- long enough for Windows to mark the window as
    // not responding. The worker sees running_ false, releases the device itself and reports
    // deviceConnected(false); the thread is joined on the next start() or in the destructor.
    running_.store(false);
    if (worker_.joinable()) {
        if (retired_.joinable()) retired_.join();
        retired_ = std::move(worker_);
    }
}

void PlutoReceiver::releaseDevice() {
#ifdef NADIRSTATION_HAS_LIBIIO
    if (buffer_) { iio_buffer_destroy(buffer_); buffer_ = nullptr; }
    if (rxdev_) {
        if (rx0_) iio_channel_disable(rx0_);
        if (rx1_) iio_channel_disable(rx1_);
    }
    rx0_ = rx1_ = nullptr;
    if (ctx_) { iio_context_destroy(ctx_); ctx_ = nullptr; }
    phy_ = rxdev_ = nullptr;
    lo_ = nullptr;
    rxPhy_ = nullptr;
#endif
}

void PlutoReceiver::run() {
    const bool hardware = configurePluto();
#ifndef NADIRSTATION_HAS_LIBIIO
    if (!hardware) {
        // Explicit build-time simulation mode only.
        std::vector<std::complex<float>> block(65536);
        float ph = 0.0f;
        auto next = std::chrono::steady_clock::now();
        while (running_) {
            for (std::size_t i = 0; i < block.size(); ++i) {
                const float t = static_cast<float>(i) / static_cast<float>(actualSampleRate_);
                const float n1 = std::sin(2.0f * 3.14159265358979323846f * 17000.0f * t + ph);
                const float n2 = 0.15f * std::sin(2.0f * 3.14159265358979323846f * 230000.0f * t);
                block[i] = {n1 + n2, 0.9f * n1 + 0.1f * std::cos(2.0f * 3.14159265358979323846f * 17000.0f * t)};
            }
            ph += 0.02f;
            auto p = std::make_shared<std::vector<std::complex<float>>>(block);
            emit samplesReady(p);
            const auto now = std::chrono::steady_clock::now();
            const auto sp = nadirstation::dsp::spectrum(*p, static_cast<float>(actualSampleRate_), fftSize_.load(), static_cast<nadirstation::dsp::SpectrumWindow>(fftWindow_.load()));
            emit spectrumReady(sp.db, actualSampleRate_);
            if (lastMetricEmit_.time_since_epoch().count() == 0 || now - lastMetricEmit_ >= std::chrono::milliseconds(100)) {
                emit metricsReady(sp.snrDb, sp.snrDb + 2.0f, sp.snrDb, actualSampleRate_);
                lastMetricEmit_ = now;
            }
            next += std::chrono::milliseconds(20);
            std::this_thread::sleep_until(next);
        }
    }
    return;
#else
    if (!hardware) {
        // A partial configuration leaves a context open; release it here rather than on the next
        // connect, which used to overwrite the pointers and leak the old one.
        releaseDevice();
        emit statusMessage(QStringLiteral("Pluto not connected, stopped. Simulation mode is not entered automatically, so simulated data cannot be mistaken for real IQ."));
        running_ = false;
        // Not every failed step above reports the state itself (a rejected attribute does not), and
        // the window waits for this to put its Start button back.
        emit deviceConnected(false);
        return;
    }
    spectrumTap_.reset();
    shifter_.configure(frequencyShiftHz_, actualSampleRate_);
    if (shifter_.active())
        emit statusMessage(QStringLiteral("Pluto tuned %1 kHz off the downlink; the signal is moved back digitally")
                               .arg(frequencyShiftHz_ / 1e3, 0, 'f', 1));
    const bool earlyEmit = actualSampleRate_ >= 4.0e6;
#ifdef _WIN32
    // The capture thread must not wait behind the JPEG decoder or the GUI for the CPU.
    if (earlyEmit) SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
#endif
    std::vector<std::complex<float>> block(earlyEmit ? (1u << 17) : 65536u);
    lastMetricEmit_ = std::chrono::steady_clock::now();
    snrHistory_.clear();
    peakSnr_ = -120.0f;
    const float sr = static_cast<float>(actualSampleRate_);
    // Overrun check. When the host does not fetch buffers fast enough the Pluto's DMA overwrites
    // samples it has not sent, and nothing in the data says so: the demodulator just sees a timing
    // jump and loses sync. The ADC core latches that event in its status register (bit 2, write 1
    // to clear) -- the same register libiio's own overflow monitor reads.
    constexpr std::uint32_t kDmaStatus = 0x80000088u, kOverflowBit = 4u;
    bool overflowCheck = iio_device_reg_write(rxdev_, kDmaStatus, 0x6) >= 0;
    auto lastOverflowCheck = std::chrono::steady_clock::now();
    auto lastOverflowReport = lastOverflowCheck - std::chrono::seconds(60);
    overflowCount_.store(0);
    std::uint64_t overflowsSinceReport = 0;
    while (running_) {
        const double newGain = pendingGain_.exchange(-1000.0);
        if (newGain > -999.0 && rxPhy_) {
            bool limited = false;
            const double applied = writeRxGain(rxPhy_, newGain, &limited);
            if (!std::isnan(applied)) {
                gainDb_ = applied;
                emit statusMessage(limited ? QStringLiteral("Pluto gain limited to %1 dB (the AD9361 accepts no more at this frequency)").arg(applied, 0, 'f', 1)
                                           : QStringLiteral("Pluto gain set to %1 dB").arg(applied, 0, 'f', 1));
            } else {
                emit statusMessage(QStringLiteral("Setting the Pluto gain to %1 dB failed").arg(newGain, 0, 'f', 1));
            }
        }
        const ssize_t nbytes = iio_buffer_refill(buffer_);
        if (nbytes < 0) {
            emit statusMessage(QStringLiteral("Pluto buffer refill error: %1").arg(nbytes));
            // Leave running_ honest: the worker is gone, so the UI must not keep showing "receiving".
            running_ = false;
            break;
        }
        if (overflowCheck) {
            const auto t = std::chrono::steady_clock::now();
            if (t - lastOverflowCheck >= std::chrono::seconds(1)) {
                lastOverflowCheck = t;
                std::uint32_t status = 0;
                if (iio_device_reg_read(rxdev_, kDmaStatus, &status) < 0) {
                    overflowCheck = false;   // this firmware does not expose the register
                } else if (status & kOverflowBit) {
                    iio_device_reg_write(rxdev_, kDmaStatus, kOverflowBit);
                    overflowCount_.fetch_add(1);
                    ++overflowsSinceReport;
                }
                if (overflowsSinceReport > 0 && t - lastOverflowReport >= std::chrono::seconds(5)) {
                    emit statusMessage(QStringLiteral("Pluto buffer overflow (samples lost): %1 recently, %2 in total. The computer cannot keep up: lower the sample rate, use wired gigabit Ethernet or a direct USB connection, and close programs that load the CPU.")
                                           .arg(static_cast<qulonglong>(overflowsSinceReport))
                                           .arg(static_cast<qulonglong>(overflowCount_.load())));
                    overflowsSinceReport = 0;
                    lastOverflowReport = t;
                }
            }
        }
        const ptrdiff_t step = iio_buffer_step(buffer_);
        char* p0 = static_cast<char*>(iio_buffer_first(buffer_, rx0_));
        char* end = static_cast<char*>(iio_buffer_end(buffer_));
        std::size_t n = 0;
        for (char* p = p0; p < end && n < block.size(); p += step) {
            const auto* iq = reinterpret_cast<const int16_t*>(p);
            const int16_t I = iq[0];
            const int16_t Q = iq[1];
            block[n++] = {static_cast<float>(I) / 2048.0f, static_cast<float>(Q) / 2048.0f};
        }
        if (n == 0) continue;
        shifter_.process(block.data(), n);
        auto samples = std::make_shared<std::vector<std::complex<float>>>(block.begin(), block.begin() + static_cast<std::ptrdiff_t>(n));
        // Hand the samples to the decoder before the spectrum work, so the next refill is not
        // delayed by an FFT. The decoder only reads the block.
        if (earlyEmit) emit samplesReady(samples);
        float latestSnr = 0.0f;
        bool haveSpectrum = false;
        spectrumTap_.feed(*samples, actualSampleRate_, fftSize_.load(),
                          static_cast<nadirstation::dsp::SpectrumWindow>(fftWindow_.load()),
                          [&](const nadirstation::dsp::SpectrumResult& sp) {
                              emit spectrumReady(sp.db, actualSampleRate_);
                              latestSnr = sp.snrDb;
                              haveSpectrum = true;
                          });
        const auto now = std::chrono::steady_clock::now();
        if (haveSpectrum && now - lastMetricEmit_ >= std::chrono::milliseconds(100)) {
            const nadirstation::dsp::SpectrumResult sp{{}, 0.0f, 0.0f, latestSnr};
            peakSnr_ = std::max(peakSnr_, sp.snrDb);
            snrHistory_.push_back({now, sp.snrDb});
            while (!snrHistory_.empty() && now - snrHistory_.front().first > std::chrono::seconds(1))
                snrHistory_.pop_front();
            float avg = 0.0f;
            if (!snrHistory_.empty()) {
                double sum = 0.0;
                for (const auto& v : snrHistory_) sum += v.second;
                avg = static_cast<float>(sum / static_cast<double>(snrHistory_.size()));
            }
            emit metricsReady(sp.snrDb, peakSnr_, avg, actualSampleRate_);
            lastMetricEmit_ = now;
        }
        if (!earlyEmit) emit samplesReady(samples);
    }
    (void)sr;
    releaseDevice();
    emit deviceConnected(false);
#endif
}
