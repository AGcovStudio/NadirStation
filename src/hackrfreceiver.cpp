#include "hackrfreceiver.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLibrary>
#include <QProcessEnvironment>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// The parts of libhackrf's API this file uses (hackrf.h, libhackrf 2017.02 and later).
struct hackrf_device;
struct HackRfTransfer {             // hackrf_transfer: only the leading fields are read
    hackrf_device* device;
    std::uint8_t* buffer;
    int buffer_length;
    int valid_length;
    void* rx_ctx;
    void* tx_ctx;
};
struct HackRfDeviceList {           // hackrf_device_list_t: only devicecount is read
    char** serial_numbers;
    int* usb_board_ids;
    int* usb_device_index;
    int devicecount;
};
using Callback = int (*)(HackRfTransfer*);
using Init = int (*)();
using Exit = int (*)();
using DeviceList = HackRfDeviceList* (*)();
using DeviceListOpen = int (*)(HackRfDeviceList*, int, hackrf_device**);
using DeviceListFree = void (*)(HackRfDeviceList*);
using Open = int (*)(hackrf_device**);
using Close = int (*)(hackrf_device*);
using SetFreq = int (*)(hackrf_device*, std::uint64_t);
using SetSampleRate = int (*)(hackrf_device*, double);
using SetBasebandFilter = int (*)(hackrf_device*, std::uint32_t);
using ComputeBasebandFilter = std::uint32_t (*)(std::uint32_t);
using SetU32 = int (*)(hackrf_device*, std::uint32_t);
using SetU8 = int (*)(hackrf_device*, std::uint8_t);
using StartRx = int (*)(hackrf_device*, Callback, void*);
using StopRx = int (*)(hackrf_device*);
using IsStreaming = int (*)(hackrf_device*);
using ErrorName = const char* (*)(int);
constexpr int kHackRfTrue = 1;

int rxCallback(HackRfTransfer* transfer) {
    auto* self = transfer ? static_cast<HackRfReceiver*>(transfer->rx_ctx) : nullptr;
    if (!self) return -1;
    return self->onTransfer(reinterpret_cast<const std::int8_t*>(transfer->buffer), transfer->valid_length);
}

// Panel gain (dB) -> amplifier on/off, LNA and VGA settings.
void splitGain(double gainDb, bool& amp, std::uint32_t& lna, std::uint32_t& vga) {
    double g = std::clamp(gainDb, 0.0, 116.0);
    amp = g >= 56.0;
    if (amp) g -= 14.0;
    const double lnaDb = std::clamp(std::round(g * 0.45 / 8.0) * 8.0, 0.0, 40.0);
    const double vgaDb = std::clamp(std::round((g - lnaDb) / 2.0) * 2.0, 0.0, 62.0);
    lna = static_cast<std::uint32_t>(lnaDb);
    vga = static_cast<std::uint32_t>(vgaDb);
}
}

HackRfReceiver::HackRfReceiver(QObject* parent) : QObject(parent) {}
HackRfReceiver::~HackRfReceiver() { stop(); }

void HackRfReceiver::setParameters(double frequencyHz, double sampleRate, double bandwidthHz, double gainDb,
                                   unsigned deviceIndex, bool antennaPower) {
    frequencyHz_ = frequencyHz;
    sampleRate_ = sampleRate;
    bandwidthHz_ = bandwidthHz;
    gainDb_ = gainDb;
    deviceIndex_ = deviceIndex;
    antennaPower_ = antennaPower;
}

void HackRfReceiver::setSpectrumSettings(std::size_t fftSize, nadirstation::dsp::SpectrumWindow window) {
    fftSize_.store(fftSize);
    fftWindow_.store(static_cast<int>(window));
}

bool HackRfReceiver::start() {
    if (running_.exchange(true)) return true;
    if (worker_.joinable()) worker_.join();
    worker_ = std::thread(&HackRfReceiver::run, this);
    return true;
}

void HackRfReceiver::stop() {
    running_.store(false);
    if (worker_.joinable()) worker_.join();
}

int HackRfReceiver::onTransfer(const std::int8_t* bytes, int length) {
    if (!running_.load()) return -1;            // non-zero asks libhackrf to end the stream
    if (!bytes || length < 2) return 0;
    gotSamples_.store(true);
    const std::size_t count = static_cast<std::size_t>(length) / 2;
    auto samples = std::make_shared<std::vector<std::complex<float>>>(count);
    for (std::size_t i = 0; i < count; ++i)
        (*samples)[i] = {static_cast<float>(bytes[2 * i]) / 128.0f, static_cast<float>(bytes[2 * i + 1]) / 128.0f};
    shifter_.process(samples->data(), count);
    float latest = 0.0f;
    bool haveSpectrum = false;
    spectrumTap_.feed(*samples, sampleRate_, fftSize_.load(),
                      static_cast<nadirstation::dsp::SpectrumWindow>(fftWindow_.load()),
                      [&](const nadirstation::dsp::SpectrumResult& sp) {
                          emit spectrumReady(sp.db, sampleRate_);
                          latest = sp.snrDb;
                          haveSpectrum = true;
                      });
    const auto now = std::chrono::steady_clock::now();
    if (haveSpectrum && now - lastMetric_ >= std::chrono::milliseconds(100)) {
        peak_ = std::max(peak_, latest);
        history_.emplace_back(now, latest);
        while (!history_.empty() && now - history_.front().first > std::chrono::seconds(1)) history_.pop_front();
        double sum = 0.0;
        for (const auto& v : history_) sum += v.second;
        emit metricsReady(latest, peak_, history_.empty() ? latest : static_cast<float>(sum / history_.size()), sampleRate_);
        lastMetric_ = now;
    }
    emit samplesReady(samples);
    return 0;
}

void HackRfReceiver::run() {
    auto quit = [this](const QString& message) {
        if (!message.isEmpty()) emit statusMessage(message);
        emit deviceConnected(false);
        running_.store(false);
        emit finished();
    };

    QLibrary library;
    QStringList candidates;
    const QString configured = QProcessEnvironment::systemEnvironment().value(QStringLiteral("NADIRSTATION_HACKRF_DLL"));
    if (!configured.isEmpty()) candidates << configured;
    const QString appDir = QCoreApplication::applicationDirPath();
    candidates << QDir(appDir).filePath(QStringLiteral("hackrf.dll")) << QDir(appDir).filePath(QStringLiteral("libhackrf.dll"));
    const QString programFiles = qEnvironmentVariable("ProgramFiles");
    if (!programFiles.isEmpty()) {
        candidates << QDir(programFiles).filePath(QStringLiteral("PothosSDR/bin/hackrf.dll"))
                   << QDir(programFiles).filePath(QStringLiteral("SDRangel/hackrf.dll"));
    }
    candidates << QStringLiteral("hackrf") << QStringLiteral("libhackrf.so.0") << QStringLiteral("libhackrf");
    for (const QString& candidate : candidates) {
#ifdef Q_OS_WIN
        // hackrf.dll needs libusb-1.0.dll and pthread from its own folder.
        if (QFileInfo(candidate).isAbsolute()) {
            if (!QFileInfo::exists(candidate)) continue;
            SetDllDirectoryW(QFileInfo(candidate).absolutePath().toStdWString().c_str());
        }
#endif
        library.setFileName(candidate);
        if (library.load()) break;
    }
#ifdef Q_OS_WIN
    SetDllDirectoryW(nullptr);
#endif
    if (!library.isLoaded()) {
        quit(QStringLiteral("HackRF driver not found. Put hackrf.dll (with libusb-1.0.dll and pthreadVC2.dll) in the program folder, "
                            "or install libhackrf (Linux: libhackrf0)."));
        return;
    }
    const auto init = reinterpret_cast<Init>(library.resolve("hackrf_init"));
    const auto exitLib = reinterpret_cast<Exit>(library.resolve("hackrf_exit"));
    const auto deviceList = reinterpret_cast<DeviceList>(library.resolve("hackrf_device_list"));
    const auto deviceListOpen = reinterpret_cast<DeviceListOpen>(library.resolve("hackrf_device_list_open"));
    const auto deviceListFree = reinterpret_cast<DeviceListFree>(library.resolve("hackrf_device_list_free"));
    const auto open = reinterpret_cast<Open>(library.resolve("hackrf_open"));
    const auto close = reinterpret_cast<Close>(library.resolve("hackrf_close"));
    const auto setFreq = reinterpret_cast<SetFreq>(library.resolve("hackrf_set_freq"));
    const auto setRate = reinterpret_cast<SetSampleRate>(library.resolve("hackrf_set_sample_rate"));
    const auto setFilter = reinterpret_cast<SetBasebandFilter>(library.resolve("hackrf_set_baseband_filter_bandwidth"));
    const auto computeFilter = reinterpret_cast<ComputeBasebandFilter>(library.resolve("hackrf_compute_baseband_filter_bw"));
    const auto setLna = reinterpret_cast<SetU32>(library.resolve("hackrf_set_lna_gain"));
    const auto setVga = reinterpret_cast<SetU32>(library.resolve("hackrf_set_vga_gain"));
    const auto setAmp = reinterpret_cast<SetU8>(library.resolve("hackrf_set_amp_enable"));
    const auto setAntennaPower = reinterpret_cast<SetU8>(library.resolve("hackrf_set_antenna_enable"));
    const auto startRx = reinterpret_cast<StartRx>(library.resolve("hackrf_start_rx"));
    const auto stopRx = reinterpret_cast<StopRx>(library.resolve("hackrf_stop_rx"));
    const auto isStreaming = reinterpret_cast<IsStreaming>(library.resolve("hackrf_is_streaming"));
    const auto errorName = reinterpret_cast<ErrorName>(library.resolve("hackrf_error_name"));
    if (!init || !exitLib || !open || !close || !setFreq || !setRate || !setLna || !setVga || !setAmp ||
        !startRx || !stopRx || !isStreaming) {
        quit(QStringLiteral("The HackRF driver lacks the receive API; use libhackrf 2017.02 or later."));
        return;
    }
    const auto err = [errorName](int rc) {
        return errorName ? QStringLiteral("%1 (%2)").arg(QString::fromLatin1(errorName(rc))).arg(rc) : QString::number(rc);
    };
    int rc = init();
    if (rc != 0) { quit(QStringLiteral("HackRF library init failed: %1").arg(err(rc))); return; }

    hackrf_device* device = nullptr;
    if (deviceList && deviceListOpen && deviceListFree) {
        HackRfDeviceList* list = deviceList();
        const int count = list ? list->devicecount : 0;
        if (static_cast<int>(deviceIndex_) >= count) {
            if (list) deviceListFree(list);
            exitLib();
            quit(QStringLiteral("HackRF device index %1 is out of range; %2 device(s) detected.").arg(deviceIndex_).arg(count));
            return;
        }
        rc = deviceListOpen(list, static_cast<int>(deviceIndex_), &device);
        deviceListFree(list);
    } else {
        rc = open(&device);
    }
    if (rc != 0 || !device) {
        exitLib();
        quit(QStringLiteral("Cannot open the HackRF: %1. Check the USB driver (WinUSB via Zadig on Windows) "
                            "and whether another program is using it.").arg(err(rc)));
        return;
    }

    bool amp = false;
    std::uint32_t lna = 0, vga = 0;
    splitGain(gainDb_, amp, lna, vga);
    const double tuneHz = frequencyHz_ + frequencyShiftHz_;
    const double filterRequest = bandwidthHz_ > 0.0 ? bandwidthHz_ : 0.75 * sampleRate_;
    const std::uint32_t filter = computeFilter ? computeFilter(static_cast<std::uint32_t>(filterRequest))
                                               : static_cast<std::uint32_t>(filterRequest);
    QString failed;
    if ((rc = setRate(device, sampleRate_)) != 0) failed = QStringLiteral("sample rate: %1").arg(err(rc));
    else if (setFilter && (rc = setFilter(device, filter)) != 0) failed = QStringLiteral("baseband filter: %1").arg(err(rc));
    else if ((rc = setFreq(device, static_cast<std::uint64_t>(std::llround(tuneHz)))) != 0) failed = QStringLiteral("frequency: %1").arg(err(rc));
    else if ((rc = setAmp(device, amp ? 1 : 0)) != 0) failed = QStringLiteral("RF amplifier: %1").arg(err(rc));
    else if ((rc = setLna(device, lna)) != 0) failed = QStringLiteral("LNA gain: %1").arg(err(rc));
    else if ((rc = setVga(device, vga)) != 0) failed = QStringLiteral("VGA gain: %1").arg(err(rc));
    else if (setAntennaPower && (rc = setAntennaPower(device, antennaPower_ ? 1 : 0)) != 0)
        failed = QStringLiteral("antenna port power: %1").arg(err(rc));
    if (!failed.isEmpty()) {
        close(device);
        exitLib();
        quit(QStringLiteral("HackRF setup failed: %1").arg(failed));
        return;
    }

    spectrumTap_.reset();
    history_.clear();
    peak_ = -120.0f;
    lastMetric_ = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    shifter_.configure(frequencyShiftHz_, sampleRate_);
    gotSamples_.store(false);
    emit statusMessage(QStringLiteral("HackRF connected: device %1 · %2 MS/s · filter %3 MHz · amp %4, LNA %5 dB, VGA %6 dB%7%8")
                           .arg(deviceIndex_).arg(sampleRate_ / 1e6, 0, 'f', 3).arg(filter / 1e6, 0, 'f', 2)
                           .arg(amp ? QStringLiteral("on") : QStringLiteral("off")).arg(lna).arg(vga)
                           .arg(antennaPower_ ? QStringLiteral(" · antenna power on") : QString())
                           .arg(shifter_.active() ? QStringLiteral(" · tuned %1 kHz off").arg(frequencyShiftHz_ / 1e3, 0, 'f', 1) : QString()));
    emit deviceConnected(true);

    rc = startRx(device, &rxCallback, this);
    if (rc != 0) {
        emit statusMessage(QStringLiteral("HackRF could not start receiving: %1").arg(err(rc)));
    } else {
        const auto started = std::chrono::steady_clock::now();
        bool warned = false;
        while (running_.load() && isStreaming(device) == kHackRfTrue) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            if (!warned && !gotSamples_.load() && std::chrono::steady_clock::now() - started > std::chrono::seconds(5)) {
                emit statusMessage(QStringLiteral("HackRF opened, but no IQ arrived within 5 s; check the USB cable and port."));
                warned = true;
            }
        }
        if (running_.load()) emit statusMessage(QStringLiteral("HackRF stream ended (device unplugged or USB error)."));
        stopRx(device);
    }
    if (setAntennaPower && antennaPower_) setAntennaPower(device, 0);
    close(device);
    exitLib();
    emit deviceConnected(false);
    running_.store(false);
    emit finished();
}
