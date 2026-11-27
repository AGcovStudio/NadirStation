#pragma once
// Records the live receiver's IQ to a raw file that the File input plays back.
//
// The capture thread only hands over the block it already made (a shared pointer, no copy) and
// returns; a writer thread converts and writes. A disk that cannot keep up therefore costs the
// recording samples -- counted and reported -- and never the live decode.
//
// Formats are the ones the File input reads:
//   .cs16  int16 I/Q   lossless for the PlutoSDR (its 12-bit samples stored at 16x)
//   .cu8   uint8 I/Q   lossless for the RTL-SDR (its native samples)
//   .cs8   int8 I/Q    half the size of .cs16; a Pluto recording keeps its top 8 bits
//   .wav   16-bit PCM stereo (I left, Q right), the sample rate in the header
// The name carries the rate and the centre frequency the way SatDump writes them
// (2026-11-10_12-00-00_6000000SPS_1692140000Hz.cs16), which is where the File input looks.
#include <QByteArray>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStorageInfo>
#include <QString>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <complex>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include <utility>

namespace nadirstation {

class IqRecorder {
public:
    enum class Format { CS16, CS8, CU8, WAV16 };
    using Block = std::shared_ptr<std::vector<std::complex<float>>>;

    IqRecorder() = default;
    ~IqRecorder() { stop(); }
    IqRecorder(const IqRecorder&) = delete;
    IqRecorder& operator=(const IqRecorder&) = delete;

    static QString fileName(double sampleRate, double centreHz, Format format) {
        return QStringLiteral("%1_%2SPS_%3Hz.%4")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd_HH-mm-ss")))
            .arg(static_cast<qulonglong>(std::llround(sampleRate)))
            .arg(static_cast<qulonglong>(std::llround(centreHz)))
            .arg(extension(format));
    }
    static QString extension(Format format) {
        switch (format) {
        case Format::CS16: return QStringLiteral("cs16");
        case Format::CS8: return QStringLiteral("cs8");
        case Format::CU8: return QStringLiteral("cu8");
        case Format::WAV16: return QStringLiteral("wav");
        }
        return QStringLiteral("cs16");
    }
    // Bytes one complex sample takes on disk.
    static int bytesPerSample(Format format) { return format == Format::CS16 || format == Format::WAV16 ? 4 : 2; }

    // Opens `directory`/<name> and starts the writer. Returns false with a reason when the file
    // cannot be created or the disk has less than the reserve free.
    bool start(const QString& directory, double sampleRate, double centreHz, Format format, QString* error) {
        stop();
        if (!QDir().mkpath(directory)) {
            if (error) *error = QStringLiteral("cannot create the recording folder %1").arg(directory);
            return false;
        }
        const QStorageInfo disk(directory);
        if (disk.isValid() && disk.bytesAvailable() < kReserveBytes) {
            if (error) *error = QStringLiteral("less than %1 GB free on the disk").arg(kReserveBytes / 1e9, 0, 'f', 1);
            return false;
        }
        const QDir dir(directory);
        const QString name = fileName(sampleRate, centreHz, format);
        for (int attempt = 0; ; ++attempt) {
            path_ = dir.filePath(attempt == 0 ? name : QStringLiteral("%1_%2").arg(attempt).arg(name));
            file_.setFileName(path_);
            if (file_.open(QIODevice::WriteOnly | QIODevice::NewOnly)) break;
            if (attempt < 999 && QFileInfo::exists(path_)) continue;
            if (error) *error = QStringLiteral("cannot create %1 (%2)").arg(path_, file_.errorString());
            return false;
        }
        headerBytes_ = 0;
        if (format == Format::WAV16) {
            // Sizes are left at zero until the file is closed: a recording cut short by a crash or
            // a power cut then still plays, because the File input reads a zero size as "to the end".
            const QByteArray header = wavHeader(sampleRate, 0);
            if (file_.write(header) != header.size()) {
                if (error) *error = QStringLiteral("cannot write to %1 (%2)").arg(path_, file_.errorString());
                file_.close();
                return false;
            }
            headerBytes_ = header.size();
            sampleRate_ = sampleRate;
        }
        format_ = format;
        directory_ = directory;
        bytesWritten_.store(0);
        droppedSamples_.store(0);
        failed_.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.clear();
            queuedSamples_ = 0;
            failure_.clear();
            quit_ = false;
        }
        active_.store(true);
        writer_ = std::thread(&IqRecorder::run, this);
        return true;
    }

    // Any thread; never waits for the disk.
    void push(const Block& block) {
        if (!active_.load(std::memory_order_relaxed) || !block || block->empty()) return;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (quit_) return;
            if (queuedSamples_ + block->size() > kMaxQueuedSamples) {
                droppedSamples_.fetch_add(block->size());
                return;
            }
            queuedSamples_ += block->size();
            queue_.push_back(block);
        }
        wake_.notify_one();
    }

    // Writes what is queued, closes the file. Safe to call when not recording.
    void stop() {
        active_.store(false);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            quit_ = true;
        }
        wake_.notify_one();
        if (writer_.joinable()) writer_.join();
        if (file_.isOpen()) file_.close();
    }

    bool active() const { return active_.load(); }
    bool failed() const { return failed_.load(); }
    quint64 bytesWritten() const { return bytesWritten_.load(); }   // sample data, without the WAV header
    quint64 droppedSamples() const { return droppedSamples_.load(); }
    QString path() const { return path_; }   // GUI thread, after start()
    // GUI thread, after stop() has joined the writer. Report each failure once.
    QString takeFailure() {
        std::lock_guard<std::mutex> lock(mutex_);
        failed_.store(false);
        return std::exchange(failure_, QString());
    }

private:
    static constexpr qint64 kReserveBytes = 1024ll * 1024ll * 1024ll;   // stop before the disk is full
    static constexpr std::size_t kMaxQueuedSamples = 48u * 1024u * 1024u;   // 384 MB of floats waiting

    void fail(const QString& why) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (failure_.isEmpty()) failure_ = why;
            quit_ = true;
            queue_.clear();
            queuedSamples_ = 0;
        }
        failed_.store(true);
        active_.store(false);
    }

    void run() {
        std::vector<char> out;
        quint64 sinceSpaceCheck = 0;
        for (;;) {
            Block block;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                wake_.wait(lock, [this] { return quit_ || !queue_.empty(); });
                if (queue_.empty()) break;   // quit requested and nothing left to write
                block = std::move(queue_.front());
                queue_.pop_front();
                queuedSamples_ -= block->size();
            }
            const auto& in = *block;
            if (format_ == Format::CS16 || format_ == Format::WAV16) {
                out.resize(in.size() * 4);
                auto* p = reinterpret_cast<unsigned char*>(out.data());
                for (std::size_t i = 0; i < in.size(); ++i) {
                    const auto I = static_cast<std::int16_t>(std::clamp(std::lround(in[i].real() * 32768.0f), -32768L, 32767L));
                    const auto Q = static_cast<std::int16_t>(std::clamp(std::lround(in[i].imag() * 32768.0f), -32768L, 32767L));
                    p[4 * i] = static_cast<unsigned char>(I & 0xff);
                    p[4 * i + 1] = static_cast<unsigned char>((I >> 8) & 0xff);
                    p[4 * i + 2] = static_cast<unsigned char>(Q & 0xff);
                    p[4 * i + 3] = static_cast<unsigned char>((Q >> 8) & 0xff);
                }
            } else if (format_ == Format::CS8) {
                out.resize(in.size() * 2);
                auto* p = reinterpret_cast<signed char*>(out.data());
                for (std::size_t i = 0; i < in.size(); ++i) {
                    p[2 * i] = static_cast<signed char>(std::clamp(std::lround(in[i].real() * 128.0f), -128L, 127L));
                    p[2 * i + 1] = static_cast<signed char>(std::clamp(std::lround(in[i].imag() * 128.0f), -128L, 127L));
                }
            } else {
                out.resize(in.size() * 2);
                auto* p = reinterpret_cast<unsigned char*>(out.data());
                for (std::size_t i = 0; i < in.size(); ++i) {
                    p[2 * i] = static_cast<unsigned char>(std::clamp(std::lround((in[i].real() + 1.0f) * 127.5f), 0L, 255L));
                    p[2 * i + 1] = static_cast<unsigned char>(std::clamp(std::lround((in[i].imag() + 1.0f) * 127.5f), 0L, 255L));
                }
            }
            if (file_.write(out.data(), static_cast<qint64>(out.size())) != static_cast<qint64>(out.size())) {
                fail(QStringLiteral("write failed: %1").arg(file_.errorString()));
                break;
            }
            bytesWritten_.fetch_add(out.size());
            sinceSpaceCheck += out.size();
            if (sinceSpaceCheck >= 256u * 1024u * 1024u) {
                sinceSpaceCheck = 0;
                const QStorageInfo disk(directory_);
                if (disk.isValid() && disk.bytesAvailable() < kReserveBytes) {
                    fail(QStringLiteral("less than %1 GB left on the disk").arg(kReserveBytes / 1e9, 0, 'f', 1));
                    break;
                }
            }
        }
        if (headerBytes_ > 0) {
            // Now the length is known. Past 4 GB the 32-bit fields cannot hold it; they are set to
            // all ones, which readers (the File input among them) take as "to the end of the file".
            const QByteArray header = wavHeader(sampleRate_, bytesWritten_.load());
            if (!file_.seek(0) || file_.write(header) != header.size())
                fail(QStringLiteral("cannot finalize WAV header: %1").arg(file_.errorString()));
        }
        if (!file_.flush()) fail(QStringLiteral("cannot flush recording: %1").arg(file_.errorString()));
    }

    static QByteArray wavHeader(double sampleRate, quint64 dataBytes) {
        const auto u32 = [](QByteArray& b, quint32 v) { for (int i = 0; i < 4; ++i) b.append(static_cast<char>((v >> (8 * i)) & 0xff)); };
        const auto u16 = [](QByteArray& b, quint16 v) { b.append(static_cast<char>(v & 0xff)); b.append(static_cast<char>((v >> 8) & 0xff)); };
        const quint32 rate = static_cast<quint32>(std::llround(sampleRate));
        const bool fits = dataBytes + 36u <= 0xFFFFFFFFull;
        QByteArray h;
        h.append("RIFF", 4);
        u32(h, dataBytes == 0 ? 0u : (fits ? static_cast<quint32>(dataBytes + 36u) : 0xFFFFFFFFu));
        h.append("WAVEfmt ", 8);
        u32(h, 16);          // fmt chunk length
        u16(h, 1);           // PCM
        u16(h, 2);           // channels: I, Q
        u32(h, rate);
        u32(h, rate * 4u);   // bytes per second
        u16(h, 4);           // bytes per frame
        u16(h, 16);          // bits per sample
        h.append("data", 4);
        u32(h, dataBytes == 0 ? 0u : (fits ? static_cast<quint32>(dataBytes) : 0xFFFFFFFFu));
        return h;
    }

    QFile file_;
    QString path_, directory_;
    Format format_ = Format::CS16;
    qint64 headerBytes_ = 0;      // WAV only
    double sampleRate_ = 0.0;
    std::thread writer_;
    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Block> queue_;
    std::size_t queuedSamples_ = 0;
    bool quit_ = false;
    QString failure_;
    std::atomic<bool> active_{false}, failed_{false};
    std::atomic<quint64> bytesWritten_{0}, droppedSamples_{0};
};

}  // namespace nadirstation
