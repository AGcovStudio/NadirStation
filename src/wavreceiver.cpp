#include "wavreceiver.h"
#include "dsp.h"
#include "runtime_logger.h"

#include <QByteArray>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QRegularExpression>
#include <QDataStream>
#include <QDateTime>
#include <QThread>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>
#include <chrono>
#include <deque>

namespace {
constexpr quint32 fourcc(char a, char b, char c, char d) {
    return static_cast<quint32>(static_cast<unsigned char>(a)) |
           (static_cast<quint32>(static_cast<unsigned char>(b)) << 8) |
           (static_cast<quint32>(static_cast<unsigned char>(c)) << 16) |
           (static_cast<quint32>(static_cast<unsigned char>(d)) << 24);
}
constexpr quint32 RIFF = fourcc('R','I','F','F');
constexpr quint32 WAVE = fourcc('W','A','V','E');
constexpr quint32 FMT  = fourcc('f','m','t',' ');
constexpr quint32 DATA = fourcc('d','a','t','a');

quint32 readU32LE(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<quint32>(u[0]) |
           (static_cast<quint32>(u[1]) << 8) |
           (static_cast<quint32>(u[2]) << 16) |
           (static_cast<quint32>(u[3]) << 24);
}
quint16 readU16LE(const char* p) {
    const auto* u = reinterpret_cast<const unsigned char*>(p);
    return static_cast<quint16>(u[0]) |
           (static_cast<quint16>(u[1]) << 8);
}
}

WavReceiver::WavReceiver(QObject* parent) : QObject(parent) {}

WavReceiver::Format WavReceiver::formatForPath(const QString& path) {
    const QString ext = QFileInfo(path).suffix().toLower();
    if (ext == QLatin1String("cs16") || ext == QLatin1String("s16") || ext == QLatin1String("c16")) return Format::CS16;
    if (ext == QLatin1String("cs8") || ext == QLatin1String("s8") || ext == QLatin1String("c8")) return Format::CS8;
    if (ext == QLatin1String("cu8") || ext == QLatin1String("u8")) return Format::CU8;
    return Format::Wav;
}

const char* WavReceiver::formatName(Format f) {
    switch (f) {
    case Format::CS16: return "cs16 (I/Q int16)";
    case Format::CS8: return "cs8 (I/Q int8)";
    case Format::CU8: return "cu8 (I/Q uint8)";
    case Format::Wav: break;
    }
    return "WAV 2ch 16-bit";
}

double WavReceiver::rateFromSidecar(const QString& path) {
    const QFileInfo fi(path);
    const QStringList candidates{
        fi.dir().filePath(fi.completeBaseName() + QStringLiteral(".txt")),
        path + QStringLiteral(".txt")};
    static const QRegularExpression rx(QStringLiteral("\\brate\\s*[=:]?\\s*([0-9]+(?:\\.[0-9]+)?)"),
                                       QRegularExpression::CaseInsensitiveOption);
    for (const QString& c : candidates) {
        QFile f(c);
        if (!f.open(QIODevice::ReadOnly)) continue;
        // Only the start of the file: the capture tool writes its settings on the first line.
        const QString head = QString::fromLatin1(f.read(4096));
        const auto m = rx.match(head);
        if (m.hasMatch()) {
            const double hz = m.captured(1).toDouble();
            if (hz >= 1000.0 && hz <= 100e6) return hz;
        }
    }
    return 0.0;
}

double WavReceiver::rateFromName(const QString& path) {
    // Only a number followed by a rate unit counts; a bare number in a name can as well be a
    // frequency or a date.
    static const QRegularExpression rx(QStringLiteral("([0-9]+(?:\\.[0-9]+)?)\\s*([km]?)(?:sps|s/s|sa/s)"),
                                       QRegularExpression::CaseInsensitiveOption);
    const QString name = QFileInfo(path).fileName();
    auto it = rx.globalMatch(name);
    while (it.hasNext()) {
        const auto m = it.next();
        double hz = m.captured(1).toDouble();
        const QString unit = m.captured(2).toLower();
        if (unit == QLatin1String("k")) hz *= 1e3;
        else if (unit == QLatin1String("m")) hz *= 1e6;
        if (hz >= 1000.0 && hz <= 100e6) return hz;
    }
    return 0.0;
}

double WavReceiver::occupiedBandwidthHz(const QString& path, double sampleRate) {
    const Format format = formatForPath(path);
    if (format == Format::Wav || sampleRate <= 0.0) return 0.0;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return 0.0;
    const int bytesPer = format == Format::CS16 ? 4 : 2;
    constexpr std::size_t kFft = 4096;
    constexpr int kBlocks = 48;
    const qint64 frames = f.size() / bytesPer;
    if (frames < qint64(kFft) * 4) return 0.0;
    // Skip the first quarter second (tuner settling), then spread the blocks over two seconds.
    const qint64 start = std::min<qint64>(qint64(sampleRate * 0.25), frames / 4);
    const qint64 span = std::max<qint64>(qint64(kFft), std::min<qint64>(qint64(sampleRate * 2.0), frames - start - qint64(kFft)));
    std::vector<double> power(kFft, 0.0);
    int used = 0;
    std::vector<std::complex<float>> iq(kFft);
    for (int b = 0; b < kBlocks; ++b) {
        const qint64 at = start + span * b / kBlocks;
        if (!f.seek(at * bytesPer)) break;
        const QByteArray raw = f.read(qint64(kFft) * bytesPer);
        if (raw.size() < qint64(kFft) * bytesPer) break;
        const char* d = raw.constData();
        for (std::size_t i = 0; i < kFft; ++i) {
            if (format == Format::CS16) {
                const auto* v = reinterpret_cast<const qint16*>(d) + 2 * i;
                iq[i] = {float(v[0]), float(v[1])};
            } else if (format == Format::CS8) {
                iq[i] = {float(qint8(d[2 * i])), float(qint8(d[2 * i + 1]))};
            } else {
                iq[i] = {float(quint8(d[2 * i])) - 127.5f, float(quint8(d[2 * i + 1])) - 127.5f};
            }
        }
        const auto r = nadirstation::dsp::spectrum(iq, float(sampleRate), kFft);
        if (r.db.size() != kFft) break;
        for (std::size_t i = 0; i < kFft; ++i) power[i] += std::pow(10.0, r.db[i] / 10.0);
        ++used;
    }
    if (used < 8) return 0.0;
    // The width is that of the power standing above the noise floor: from the 10 % to the 90 % point
    // of its cumulative sum. Power, not the peak, decides, so a narrow spur cannot pass for the signal:
    // HRIT spreads 1.15 Msym/s over 1.5 MHz and its spectral density is lower than that of a strong
    // spur, which made a peak-based width read 23 kHz on an HRIT recording.
    const int half = std::max(1, int(50e3 / (sampleRate / kFft)) / 2);
    std::vector<double> smooth(kFft);
    for (int i = 0; i < int(kFft); ++i) {
        double acc = 0.0;
        for (int k = -half; k <= half; ++k) acc += power[std::size_t((i + k + int(kFft)) % int(kFft))];
        smooth[std::size_t(i)] = acc / (2 * half + 1);
    }
    const int from = int(kFft) / 20, to = int(kFft) * 19 / 20;   // leave out the anti-alias roll-off
    std::vector<double> inner(smooth.begin() + from, smooth.begin() + to);
    // Noise floor: the 40th percentile. The SDR's own filter makes the noise sag towards the band
    // edges (about 2 dB on a Pluto at 5 MS/s), and a lower percentile reads that sag as the floor and
    // counts the flat noise in the middle as signal. Widths with it: LRIT 259 kHz; HRIT 1110-1170 kHz
    // (at 3 and 5 MS/s).
    std::nth_element(inner.begin(), inner.begin() + std::ptrdiff_t(inner.size() * 2 / 5), inner.end());
    const double floor = inner[inner.size() * 2 / 5];
    std::vector<double> excess(kFft, 0.0);
    double total = 0.0;
    for (int i = from; i < to; ++i) { excess[std::size_t(i)] = std::max(0.0, smooth[std::size_t(i)] - floor); total += excess[std::size_t(i)]; }
    if (!(floor > 0.0) || total < 0.05 * floor * double(kFft)) return 0.0;   // nothing stands out of the noise
    // A signal worth measuring stands at least 2 dB over the floor somewhere. The SDR's own filter
    // ripple reaches 1.3 dB on a recording with no signal in it (a mis-pointed HRIT capture), and the
    // power test above alone took that ripple for a 1.1 MHz signal.
    const double peak = *std::max_element(smooth.begin() + from, smooth.begin() + to);
    if (peak < floor * std::pow(10.0, 0.2)) return 0.0;
    double acc = 0.0;
    int lo = -1, hi = -1;
    for (int i = from; i < to; ++i) {
        acc += excess[std::size_t(i)];
        if (lo < 0 && acc >= 0.1 * total) lo = i;
        if (hi < 0 && acc >= 0.9 * total) { hi = i; break; }
    }
    if (lo < 0 || hi < lo) return 0.0;
    return double(hi - lo + 1) * sampleRate / double(kFft);
}

bool WavReceiver::openRaw() {
    QFileInfo fi(filePath_);
    if (!fi.exists() || !fi.isFile()) {
        emit statusMessage(QStringLiteral("Cannot open I/Q file: %1").arg(filePath_));
        return false;
    }
    frameBytes_ = format_ == Format::CS16 ? 4 : 2;
    const qint64 size = fi.size();
    totalFrames_ = size / frameBytes_;
    if (totalFrames_ <= 0) {
        emit statusMessage(QStringLiteral("I/Q file is empty: %1").arg(filePath_));
        return false;
    }
    if (size % frameBytes_ != 0)
        nadirstation::log::write(QStringLiteral("I/Q file %1: %2 trailing bytes ignored").arg(filePath_).arg(size % frameBytes_));
    double hz = rateFromSidecar(filePath_);
    rateSource_ = QStringLiteral("sidecar");
    if (hz <= 0.0) { hz = rateFromName(filePath_); rateSource_ = QStringLiteral("file name"); }
    if (hz <= 0.0) { hz = rawFallbackRate_; rateSource_ = QStringLiteral("Sample Rate box"); }
    if (hz <= 0.0) {
        emit statusMessage(QStringLiteral("The I/Q file carries no sample rate; choose one in Sample Rate."));
        return false;
    }
    sampleRate_ = hz;
    dataOffset_ = 0;
    endFrame_ = std::clamp(static_cast<qint64>(std::llround(static_cast<double>(totalFrames_) * fraction_)), 1LL, totalFrames_);
    nadirstation::log::write(QStringLiteral("I/Q file %1: format %2, %3 samples, %4 Hz (from %5)")
        .arg(filePath_).arg(QString::fromLatin1(formatName(format_))).arg(totalFrames_)
        .arg(sampleRate_, 0, 'f', 0).arg(rateSource_));
    return true;
}

WavReceiver::~WavReceiver() { stop(); }

bool WavReceiver::prepare() {
    stop();
    if (!openWav()) {
        prepared_ = false;
        return false;
    }
    prepared_ = true;
    return true;
}

bool WavReceiver::openWav() {
    format_ = formatForPath(filePath_);
    if (format_ != Format::Wav) return openRaw();
    rateSource_ = QStringLiteral("WAV header");
    QFile f(filePath_);
    if (!f.open(QIODevice::ReadOnly)) {
        emit statusMessage(QStringLiteral("Cannot open WAV: %1").arg(filePath_));
        return false;
    }

    const QByteArray header = f.read(12);
    // RF64 is the same layout with 64-bit sizes kept in a 'ds64' chunk; recorders switch to it past 4 GB.
    const bool rf64 = header.size() == 12 && readU32LE(header.constData()) == fourcc('R','F','6','4');
    if (header.size() != 12 || (readU32LE(header.constData()) != RIFF && !rf64) || readU32LE(header.constData() + 8) != WAVE) {
        emit statusMessage(QStringLiteral("Bad WAV: not a RIFF/WAVE file."));
        return false;
    }

    bool haveFmt = false;
    bool haveData = false;
    quint16 audioFormat = 0;
    quint16 channels = 0;
    quint16 bitsPerSample = 0;
    quint32 sampleRate = 0;
    qint64 dataSize = 0;

    while (!f.atEnd()) {
        const QByteArray ch = f.read(8);
        if (ch.size() != 8) break;
        const quint32 id = readU32LE(ch.constData());
        const quint32 size = readU32LE(ch.constData() + 4);
        const qint64 payloadPos = f.pos();
        if (id == FMT) {
            const QByteArray fmt = f.read(static_cast<qint64>(size));
            if (fmt.size() < 16) {
                emit statusMessage(QStringLiteral("WAV fmt chunk is incomplete."));
                return false;
            }
            audioFormat = readU16LE(fmt.constData() + 0);
            channels = readU16LE(fmt.constData() + 2);
            sampleRate = readU32LE(fmt.constData() + 4);
            bitsPerSample = readU16LE(fmt.constData() + 14);
            haveFmt = true;
        } else if (id == DATA) {
            dataOffset_ = payloadPos;
            dataSize = static_cast<qint64>(size);
            haveData = true;
            // The header's size is 32 bits and is written last. A recording past 4 GB wraps it or
            // sets it to 0xFFFFFFFF (RF64), and one whose recorder was closed or crashed mid-file
            // leaves 0 or more than the file holds. In all of those the samples run to the end of
            // the file, so that is what is played, instead of refusing the file.
            const qint64 remaining = f.size() - payloadPos;
            // Wrapped: what lies beyond the stated size is a whole number of 4 GiB (give or take a
            // trailing chunk), which is what a 32-bit counter that overflowed leaves behind.
            const qint64 beyond = remaining - dataSize;
            const bool wrapped = beyond >= (1ll << 32) && (beyond % (1ll << 32)) < (1ll << 20);
            if (rf64 || wrapped || size == 0u || size == 0xFFFFFFFFu || dataSize > remaining) {
                if (!rf64 && size != 0u && size != 0xFFFFFFFFu)
                    nadirstation::log::write(QStringLiteral("WAV %1: data chunk says %2 bytes but the file holds %3; using the file length")
                        .arg(filePath_).arg(dataSize).arg(remaining));
                dataSize = remaining;
                break;   // nothing can follow a chunk that runs to the end
            }
            f.seek(f.pos() + static_cast<qint64>(size));
        } else {
            f.seek(f.pos() + static_cast<qint64>(size));
        }
        if (size & 1u) f.seek(f.pos() + 1);
        if (haveFmt && haveData) break;
    }

    if (!haveFmt || !haveData) {
        emit statusMessage(QStringLiteral("WAV has no fmt or data chunk."));
        return false;
    }
    if (audioFormat != 1 || channels != 2 || bitsPerSample != 16) {
        emit statusMessage(QStringLiteral("WAV input must be 16-bit PCM stereo; this file has format=%1 channels=%2 bits=%3")
            .arg(audioFormat).arg(channels).arg(bitsPerSample));
        return false;
    }
    if (sampleRate == 0) {
        emit statusMessage(QStringLiteral("WAV sample rate is invalid."));
        return false;
    }

    sampleRate_ = static_cast<double>(sampleRate);
    frameBytes_ = static_cast<qint64>(channels) * (static_cast<qint64>(bitsPerSample) / 8);
    if (frameBytes_ <= 0 || dataSize < frameBytes_) {
        emit statusMessage(QStringLiteral("WAV contains no sample data."));
        return false;
    }
    // A trailing partial frame (an interrupted write) is dropped rather than rejecting the file.
    if ((dataSize % frameBytes_) != 0)
        nadirstation::log::write(QStringLiteral("WAV %1: %2 trailing bytes ignored").arg(filePath_).arg(dataSize % frameBytes_));
    totalFrames_ = dataSize / frameBytes_;
    endFrame_ = std::clamp(static_cast<qint64>(std::llround(static_cast<double>(totalFrames_) * fraction_)), 1LL, totalFrames_);
    return true;
}

bool WavReceiver::start() {
    if (running_.exchange(true)) return true;
    if (worker_.joinable()) worker_.join();
    if (!prepared_) {
        if (!openWav()) {
            running_ = false;
            return false;
        }
    }
    prepared_ = false;
    worker_ = std::thread(&WavReceiver::run, this);
    return true;
}

void WavReceiver::stop() {
    const bool wasRunning = running_.exchange(false);
    if (worker_.joinable()) worker_.join();
    if (wasRunning) emit deviceConnected(false);
}

void WavReceiver::run() {
    QFile f(filePath_);
    if (!f.open(QIODevice::ReadOnly) || !f.seek(dataOffset_)) {
        emit statusMessage(QStringLiteral("Cannot locate the WAV data chunk: %1").arg(filePath_));
        running_ = false;
        emit finished();
        return;
    }

    emit deviceConnected(true);
    emit statusMessage(QStringLiteral("File input: %1 · %2 MS/s (%3) · %4 · read %5%")
        .arg(filePath_).arg(sampleRate_ / 1e6, 0, 'f', 6).arg(rateSource_)
        .arg(QString::fromLatin1(formatName(format_))).arg(fraction_ * 100.0, 0, 'f', 0));

    // The demodulator is stateful across blocks, so Max mode must NOT collapse
    // the whole WAV into one giant allocation.  Keep one bounded transport block
    // for every playback mode; this is only an I/O boundary and does not reset DSP.
    constexpr std::size_t BLOCK_FRAMES = 65536u;
    nadirstation::log::write(QStringLiteral("WAV decode mode: bounded streaming frames/block=%1")
        .arg(static_cast<qulonglong>(BLOCK_FRAMES)));

    QByteArray raw;
    raw.resize(static_cast<int>(BLOCK_FRAMES * static_cast<std::size_t>(frameBytes_)));
    qint64 framePos = 0;
    float peakSnr = -120.0f;
    std::deque<std::pair<std::chrono::steady_clock::time_point, float>> history;
    auto nextTick = std::chrono::steady_clock::now();
    auto lastMetricTick = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto lastSpectrumTick = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto lastStatusTick = std::chrono::steady_clock::now() - std::chrono::seconds(1);

    while (running_ && framePos < endFrame_) {
        const qint64 wantFrames = std::min<qint64>(static_cast<qint64>(BLOCK_FRAMES), endFrame_ - framePos);
        const qint64 wantBytes = wantFrames * frameBytes_;
        const qint64 gotBytes = f.read(raw.data(), wantBytes);
        if (gotBytes < wantBytes) {
            emit statusMessage(QStringLiteral("File ended early: %1/%2 frames").arg(framePos).arg(endFrame_));
            break;
        }

        auto samples = std::make_shared<std::vector<std::complex<float>>>();
        samples->resize(static_cast<std::size_t>(wantFrames));
        const auto* p = reinterpret_cast<const unsigned char*>(raw.constData());
        auto& out = *samples;
        const std::size_t n = static_cast<std::size_t>(wantFrames);
        switch (format_) {
        case Format::Wav:
        case Format::CS16:   // little-endian int16 I, Q -- the same layout as the WAV data chunk
            for (std::size_t i = 0; i < n; ++i) {
                const unsigned char* s = p + 4 * i;
                const qint16 I = static_cast<qint16>(static_cast<quint16>(s[0]) | (static_cast<quint16>(s[1]) << 8));
                const qint16 Q = static_cast<qint16>(static_cast<quint16>(s[2]) | (static_cast<quint16>(s[3]) << 8));
                out[i] = {static_cast<float>(I) / 32768.0f, static_cast<float>(Q) / 32768.0f};
            }
            break;
        case Format::CS8:
            for (std::size_t i = 0; i < n; ++i) {
                const auto I = static_cast<signed char>(p[2 * i]), Q = static_cast<signed char>(p[2 * i + 1]);
                out[i] = {static_cast<float>(I) / 128.0f, static_cast<float>(Q) / 128.0f};
            }
            break;
        case Format::CU8:   // offset binary, 127.5 = zero
            for (std::size_t i = 0; i < n; ++i)
                out[i] = {(static_cast<float>(p[2 * i]) - 127.5f) / 128.0f, (static_cast<float>(p[2 * i + 1]) - 127.5f) / 128.0f};
            break;
        }

        // Display FFTs are decoupled from the IQ transport; the decoder still receives every block.
        const auto now = std::chrono::steady_clock::now();
        // Waterfall rows every 20 ms of wall clock (a faster-than-real-time replay must not flood
        // the GUI); SNR metrics every 100 ms.
        if (now - lastSpectrumTick >= std::chrono::milliseconds(20)) {
            const auto sp = nadirstation::dsp::spectrum(*samples, static_cast<float>(sampleRate_), fftSize_.load(), static_cast<nadirstation::dsp::SpectrumWindow>(fftWindow_.load()));
            emit spectrumReady(sp.db, sampleRate_);
            lastSpectrumTick = now;
            if (now - lastMetricTick >= std::chrono::milliseconds(100)) {
                peakSnr = std::max(peakSnr, sp.snrDb);
                history.push_back({now, sp.snrDb});
                while (!history.empty() && now - history.front().first > std::chrono::seconds(1)) history.pop_front();
                double sum = 0.0;
                for (const auto& v : history) sum += v.second;
                const float avg = history.empty() ? sp.snrDb : static_cast<float>(sum / history.size());
                emit metricsReady(sp.snrDb, peakSnr, avg, sampleRate_);
                lastMetricTick = now;
            }
        }

        emit samplesReady(samples);

        framePos += wantFrames;
        if (now - lastStatusTick >= std::chrono::milliseconds(200) || framePos >= endFrame_) {
            const double progress = 100.0 * static_cast<double>(framePos) / static_cast<double>(endFrame_);
            emit statusMessage(QStringLiteral("File playback %1% · %2/%3 frames")
                .arg(progress, 0, 'f', 1).arg(framePos).arg(endFrame_));
            lastStatusTick = now;
        }

        if (playbackFactor_ < 1e6) {
            const double seconds = static_cast<double>(wantFrames) / sampleRate_ / playbackFactor_;
            nextTick += std::chrono::microseconds(static_cast<long long>(seconds * 1e6));
            std::this_thread::sleep_until(nextTick);
        }
    }

    f.close();
    running_ = false;
    emit deviceConnected(false);
    emit statusMessage(QStringLiteral("File input finished: %1/%2 frames sent. Decoding ends automatically.")
        .arg(framePos).arg(endFrame_));
    emit finished();
}
