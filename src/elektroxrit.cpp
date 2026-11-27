#include "elektroxrit.h"
#include "elektrol2.h"
#include "runtime_logger.h"

#include "CDataField.h"
#include "CompressJPEG.h"
#include "CompressT4.h"
#include "CompressWT.h"

#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QPainter>
#include <QRegularExpression>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace nadirstation::elektro {

namespace {
std::uint32_t be16(const std::uint8_t* p) { return (std::uint32_t(p[0]) << 8) | p[1]; }
std::uint32_t be32(const std::uint8_t* p) { return (be16(p) << 16) | be16(p + 2); }
std::uint64_t be64(const std::uint8_t* p) { return (std::uint64_t(be32(p)) << 32) | be32(p + 4); }

// A name safe to use as a file name on Windows.
QString safeName(const std::string& s) {
    QString out;
    for (char ch : s) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 32 || c > 126) continue;
        out += QString(QChar::fromLatin1(ch)).contains(QRegularExpression(QStringLiteral("[\\\\/:*?\"<>|]")))
            ? QStringLiteral("_") : QString(QChar::fromLatin1(ch));
    }
    return out.trimmed();
}
}  // namespace

XritHeader parseXrit(const std::vector<std::uint8_t>& b) {
    XritHeader h;
    if (b.size() < 16 || b[0] != 0 || be16(&b[1]) != 16) return h;
    h.fileType = b[3];
    h.headerLength = be32(&b[4]);
    h.dataBits = be64(&b[8]);
    if (h.headerLength < 16 || h.headerLength > b.size()) return h;
    std::size_t at = 0;
    while (at + 3 <= h.headerLength) {
        const int type = b[at];
        const std::size_t len = be16(&b[at + 1]);
        if (len < 3 || at + len > h.headerLength) break;
        const std::uint8_t* d = &b[at + 3];
        const std::size_t n = len - 3;
        h.recordTypes.push_back(type);
        switch (type) {
        case 1:
            if (n >= 6) { h.nb = d[0]; h.nc = int(be16(d + 1)); h.nl = int(be16(d + 3)); h.compression = d[5]; }
            break;
        case 2:
            if (n >= 48) {
                h.projection.assign(reinterpret_cast<const char*>(d), 32);
                h.projection.erase(h.projection.find_last_not_of(" \0", std::string::npos, 2) + 1);
                h.cfac = std::int32_t(be32(d + 32)); h.lfac = std::int32_t(be32(d + 36));
                h.coff = std::int32_t(be32(d + 40)); h.loff = std::int32_t(be32(d + 44));
            }
            break;
        case 4:
            h.annotation.assign(reinterpret_cast<const char*>(d), n);
            break;
        case 5:
            if (n >= 7) {
                // CCSDS day segmented time: days since 1958-01-01 and milliseconds of the day.
                const QDateTime epoch(QDate(1958, 1, 1), QTime(0, 0), QTimeZone::UTC);
                h.time = epoch.addDays(be16(d + 1)).addMSecs(be32(d + 3));
            }
            break;
        case 128:
            if (n >= 10) {
                h.hasSegment = true;
                h.spacecraft = int(be16(d));
                h.channel = d[2];
                h.segment = int(be16(d + 3));
                h.firstSegment = int(be16(d + 5));
                h.lastSegment = int(be16(d + 7));
                h.representation = d[9];
            }
            break;
        default:
            break;
        }
        at += len;
    }
    h.valid = true;
    return h;
}

bool decompressImage(const XritHeader& h, const std::uint8_t* data, std::size_t size,
                     std::vector<std::uint16_t>& pixels, std::string& error) {
    if (h.nc <= 0 || h.nl <= 0 || h.nb <= 0 || h.nb > 16) { error = "no image structure"; return false; }
    const std::size_t count = std::size_t(h.nc) * std::size_t(h.nl);
    pixels.assign(count, 0);
    // Unpack an uncompressed field: nb bits a pixel, most significant first.
    auto unpack = [&](const std::uint8_t* p, std::size_t bytes, int bits) {
        std::uint64_t acc = 0; int have = 0; std::size_t k = 0;
        for (std::size_t i = 0; i < bytes && k < count; ++i) {
            acc = (acc << 8) | p[i]; have += 8;
            while (have >= bits && k < count) {
                pixels[k++] = std::uint16_t((acc >> (have - bits)) & ((1u << bits) - 1));
                have -= bits;
            }
        }
    };
    if (h.compression == 0) { unpack(data, size, h.nb); return true; }
    try {
        Util::CDataField raw(static_cast<unsigned __int64>(size) * 8, false);
        std::copy(data, data + size, raw.Data());
        Util::CDataFieldCompressedImage in(raw, static_cast<unsigned char>(h.nb),
                                           static_cast<unsigned short>(h.nc), static_cast<unsigned short>(h.nl));
        Util::CDataFieldUncompressedImage out;
        std::vector<short> quality;
        const unsigned char nr = static_cast<unsigned char>(h.nb);
        // Representation from the segment header (MSG convention: 1 JPEG, 2 T.4, 3 wavelet); a JPEG
        // stream announces itself with FF D8, so that is recognised without the header too.
        int rep = h.representation;
        if (size >= 2 && data[0] == 0xFF && data[1] == 0xD8) rep = 1;
        if (rep == 1) COMP::DecompressJPEG(in, nr, out, quality);
        else if (rep == 2) COMP::DecompressT4(in, out, quality);
        else COMP::DecompressWT(in, nr, out, quality);
        const int bits = out.GetNR() > 0 ? out.GetNR() : h.nb;
        if (out.GetNC() == h.nc && out.GetNL() == h.nl) {
            unpack(out.Data(), static_cast<std::size_t>((out.Size() + 7) / 8), bits);
        } else {
            // Decoded, but to another size: keep the part that overlaps the announced one.
            std::vector<std::uint16_t> wanted;
            wanted.swap(pixels);
            const int oc = out.GetNC(), ol = out.GetNL();
            pixels.assign(std::size_t(oc) * ol, 0);
            const std::size_t saveCount = count;
            (void)saveCount;
            std::vector<std::uint16_t> decoded;
            {
                std::uint64_t acc = 0; int have = 0; std::size_t k = 0; const std::size_t total = pixels.size();
                const std::uint8_t* p = out.Data();
                const std::size_t bytes = static_cast<std::size_t>((out.Size() + 7) / 8);
                decoded.assign(total, 0);
                for (std::size_t i = 0; i < bytes && k < total; ++i) {
                    acc = (acc << 8) | p[i]; have += 8;
                    while (have >= bits && k < total) { decoded[k++] = std::uint16_t((acc >> (have - bits)) & ((1u << bits) - 1)); have -= bits; }
                }
            }
            wanted.assign(count, 0);
            for (int y = 0; y < std::min(ol, h.nl); ++y)
                for (int x = 0; x < std::min(oc, h.nc); ++x)
                    wanted[std::size_t(y) * h.nc + x] = decoded[std::size_t(y) * oc + x];
            pixels.swap(wanted);
        }
        // The decoder reports, per line, how many pixels from the left are good. Where a segment's
        // tail was lost (packets zero-filled by the assembler) the rest of the line is garbage: it
        // is cleared, so a damaged segment shows black instead of stripes.
        for (int y = 0; y < h.nl && y < int(quality.size()); ++y) {
            if (quality[std::size_t(y)] < 0) continue;   // negative: "errors somewhere", not per line
            const int good = std::clamp(int(quality[std::size_t(y)]), 0, h.nc);
            if (good < h.nc)
                std::fill(pixels.begin() + std::ptrdiff_t(std::size_t(y) * h.nc + good),
                          pixels.begin() + std::ptrdiff_t(std::size_t(y + 1) * h.nc), std::uint16_t(0));
        }
        return true;
    } catch (...) {
        error = "PublicDecompWT could not decode the data field";
        return false;
    }
}

// ---------------------------------------------------------------------------------------------

XritProducts::XritProducts(QString service) : service_(std::move(service)) {
    worker_ = std::thread([this] { run(); });
}

XritProducts::~XritProducts() {
    {
        std::lock_guard<std::mutex> lock(m_);
        stop_ = true;
    }
    cv_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void XritProducts::run() {
    for (;;) {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_);
            cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            queuedBytes_ -= job.file.bytes.capacity();
            busy_ = true;
        }
        try { process(job); }
        catch (const std::exception& e) { post(QStringLiteral("ELEKTRO-L %1: file skipped (%2)").arg(service_, QString::fromLocal8Bit(e.what()))); }
        catch (...) { post(QStringLiteral("ELEKTRO-L %1: file skipped").arg(service_)); }
        {
            std::lock_guard<std::mutex> lock(m_);
            busy_ = false;
        }
        idle_.notify_all();
    }
}

void XritProducts::post(const QString& message) {
    std::lock_guard<std::mutex> lock(m_);
    pending_.message = message;
    nadirstation::log::write(message);
}

void XritProducts::submit(gk2a::LritFile&& file, const QString& root) {
    XritHeader h = parseXrit(file.bytes);
    const auto sequence = ++filesSeen_;
    // ELEKTRO-L file names follow the MSG convention,
    //   H-000-GOMS4_-GOMS4_4_____-00_7_165E-000004___-202611150000-C_
    // (service, spacecraft, channel in micrometres with '_' for the point and the longitude, segment,
    // nominal time, compressed). The files carry no time-stamp record, so the time is read from here.
    const QStringList fields = QString::fromStdString(h.annotation).split(QLatin1Char('-'));
    QDateTime when = h.time;
    QString band;
    if (fields.size() >= 7) {
        const QDateTime t = QDateTime::fromString(fields.at(6).left(12), QStringLiteral("yyyyMMddHHmm"));
        if (t.isValid()) when = QDateTime(t.date(), t.time(), QTimeZone::UTC);
        const QString ch = fields.at(4).section(QLatin1Char('_'), 0, 1);   // "00_7"
        bool ok1 = false, ok2 = false;
        const int whole = ch.section(QLatin1Char('_'), 0, 0).toInt(&ok1);
        const int frac = ch.section(QLatin1Char('_'), 1, 1).toInt(&ok2);
        if (ok1 && ok2) band = QStringLiteral("%1.%2um").arg(whole).arg(frac);
    }
    if (!when.isValid()) when = QDateTime::currentDateTimeUtc();
    const QString day = when.toString(QStringLiteral("yyyy-MM-dd"));
    const QString base = QDir(root).filePath(QStringLiteral("ELEKTRO-L/%1/%2").arg(service_, day));
    QString name = safeName(h.annotation);
    if (name.isEmpty())
        name = QStringLiteral("apid%1_%2_%3").arg(file.apid).arg(when.toString(QStringLiteral("HHmmss"))).arg(sequence);

    // Archive on the decoder thread before handing off optional image processing. An overloaded
    // image worker can skip an image, but cannot discard the received xRIT file.
    const QString rawDir = QDir(base).filePath(QStringLiteral("xrit"));
    QSaveFile raw(QDir(rawDir).filePath(name));
    const auto size = static_cast<qint64>(file.bytes.size());
    if (!QDir().mkpath(rawDir) || !raw.open(QIODevice::WriteOnly) ||
        raw.write(reinterpret_cast<const char*>(file.bytes.data()), size) != size || !raw.commit()) {
        ++archiveFailures_;
        post(QStringLiteral("ELEKTRO-L %1: cannot archive %2 (%3)").arg(service_, raw.fileName(), raw.errorString()));
    }
    // The prologue carries the calibration tables the L2 products use.
    if (fields.size() >= 6 && fields.at(5).startsWith(QStringLiteral("PRO"))) {
        std::array<std::array<float, 1024>, 10> lut{};
        const QString stamp = when.toString(QStringLiteral("yyyyMMdd_HHmm"));
        if (readPrologueCalibration(file.bytes, lut)) {
            std::lock_guard<std::mutex> lock(m_);
            prologue_[stamp] = lut;
            nadirstation::log::write(QStringLiteral("ELEKTRO-L %1 %2: calibration tables read from the prologue (10.7 um: count 512 = %3 K)")
                .arg(service_, stamp).arg(lut[8][512], 0, 'f', 2));
        } else {
            nadirstation::log::write(QStringLiteral("ELEKTRO-L %1 %2: the prologue (%3 bytes) has no calibration tables; L2 products will use the nominal ones")
                .arg(service_, stamp).arg(file.bytes.size()));
        }
    }
    QStringList types;
    for (int t : h.recordTypes) types << QString::number(t);
    nadirstation::log::write(QStringLiteral("ELEKTRO-L %1 file %2: type %3, headers [%4], %5 x %6 x %7 bit, compression %8, segment %9/%10..%11 ch %12 rep %13, %14 bytes, %15 lost packets")
        .arg(service_, name).arg(h.fileType).arg(types.join(QLatin1Char(','))).arg(h.nc).arg(h.nl).arg(h.nb)
        .arg(h.compression).arg(h.segment).arg(h.firstSegment).arg(h.lastSegment).arg(h.channel).arg(h.representation)
        .arg(file.bytes.size()).arg(file.lostPackets));

    if (!h.valid || h.fileType != 0 || h.nc <= 0 || h.nl <= 0) {
        if (h.valid && (h.fileType == 1 || h.fileType == 2) && file.dataOffset < file.bytes.size()) {
            QSaveFile text(QDir(rawDir).filePath(name + QStringLiteral(".txt")));
            const auto length = static_cast<qint64>(file.bytes.size() - file.dataOffset);
            if (!text.open(QIODevice::WriteOnly) ||
                text.write(reinterpret_cast<const char*>(file.bytes.data() + file.dataOffset), length) != length ||
                !text.commit()) {
                post(QStringLiteral("ELEKTRO-L %1: cannot export %2 as text (%3)").arg(service_, name, text.errorString()));
            }
        }
        return;
    }

    const auto bytes = file.bytes.capacity();
    {
        std::lock_guard lock(m_);
        if (queue_.size() < 64 && bytes <= kQueueBytes - queuedBytes_) {
            queue_.push_back({std::move(file), std::move(h), when, name, base, band});
            queuedBytes_ += bytes;
            cv_.notify_one();
            return;
        }
    }
    skipImage(QStringLiteral("%1: image queue is full").arg(name));
}

void XritProducts::skipImage(const QString& reason) {
    const auto count = ++skippedImages_;
    post(QStringLiteral("ELEKTRO-L %1: %2 (images skipped: %3)").arg(service_, reason).arg(count));
}

bool XritProducts::makeRoomForCanvas(const QString& stamp, std::size_t bytes) {
    if (bytes > kCanvasBytes || (!evictedThrough_.isEmpty() && stamp <= evictedThrough_)) return false;
    while (!canvases_.empty()) {
        // The sorted keys begin with the observation timestamp, so the first key is the oldest.
        int observations = 0;
        QString last;
        bool present = false;
        for (const auto& [key, c] : canvases_) {
            if (c.stamp != last) { ++observations; last = c.stamp; }
            present = present || c.stamp == stamp;
        }
        if (bytes <= kCanvasBytes - canvasBytes_ && (present || observations < 2)) return true;
        const QString oldest = canvases_.begin()->second.stamp;
        if (oldest >= stamp) return false;
        closeObservation(oldest);
    }
    return true;
}

void XritProducts::process(const Job& job) {
    const auto& file = job.file;
    const auto& h = job.header;
    const auto& when = job.when;
    const auto& name = job.name;
    const auto& band = job.band;

    const int first = h.hasSegment ? std::max(1, h.firstSegment) : 1;
    const int last = h.hasSegment ? std::max(first, h.lastSegment) : 1;
    const int seg = h.hasSegment ? h.segment : 1;
    const int segments = last - first + 1;
    const QString stamp = when.toString(QStringLiteral("yyyyMMdd_HHmm"));
    const QString channel = !band.isEmpty() ? band
                          : h.hasSegment ? QStringLiteral("CH%1").arg(h.channel, 2, 10, QLatin1Char('0'))
                                         : name.left(40);
    const QString key = stamp + QLatin1Char('|') + channel;
    const auto count = std::uint64_t(h.nc) * h.nl * segments;
    auto existing = canvases_.find(key);
    if (seg < first || seg > last || count > kSingleCanvasBytes / sizeof(std::uint16_t) ||
        (existing != canvases_.end() && (h.nc != existing->second.nc || h.nl != existing->second.nl ||
         h.nb != existing->second.nb || first != existing->second.first || last != existing->second.last))) {
        skipImage(QStringLiteral("%1: inconsistent image or segment geometry").arg(name));
        return;
    }
    const auto bytes = static_cast<std::size_t>(count) * sizeof(std::uint16_t);
    if (!evictedThrough_.isEmpty() && stamp <= evictedThrough_) {
        skipImage(QStringLiteral("%1: observation already closed").arg(name));
        return;
    }
    std::vector<std::uint16_t> pixels;
    std::string error;
    const std::size_t off = std::min<std::size_t>(h.headerLength, file.bytes.size());
    if (!decompressImage(h, file.bytes.data() + off, file.bytes.size() - off, pixels, error)) {
        skipImage(QStringLiteral("%1 not decoded (%2)").arg(name, QString::fromStdString(error)));
        return;
    }
    ++segments_;
    if (file.lostPackets > 0) {
        // The tail of this segment was lost and zero-filled. Past the point where the data stops, a
        // JPEG decoder produces full-width saturated stripes; find the first such line (a line that
        // is almost entirely at full scale, which no real line of an Earth disk is) and clear the
        // segment from the MCU row before it.
        const std::uint16_t top = std::uint16_t((1u << h.nb) - 1);
        for (int y = 0; y < h.nl; ++y) {
            const std::uint16_t* line = pixels.data() + std::size_t(y) * h.nc;
            // Garbage lines are two-level: runs at full scale and runs at zero, nothing between.
            int sat = 0, zero = 0;
            for (int x = 0; x < h.nc; ++x) { sat += line[x] >= top - 2; zero += line[x] <= 2; }
            if (sat * 10 >= h.nc && (sat + zero) * 20 >= h.nc * 19) {
                const int from = std::max(0, (y / 8) * 8 - 8);
                std::fill(pixels.begin() + std::ptrdiff_t(std::size_t(from) * h.nc), pixels.end(), std::uint16_t(0));
                nadirstation::log::write(QStringLiteral("ELEKTRO-L %1: %2 lost its tail; kept lines 0..%3 of %4")
                    .arg(service_, name).arg(from).arg(h.nl));
                break;
            }
        }
    }

    // Allocate a canvas only after the segment has been decoded successfully.
    if (existing == canvases_.end()) {
        if (!makeRoomForCanvas(stamp, bytes)) {
            skipImage(QStringLiteral("%1: image cache limit reached").arg(name));
            return;
        }
        Canvas c;
        c.stamp = stamp;
        c.channelName = channel;
        c.dir = QDir(job.base).filePath(stamp);
        c.nc = h.nc;
        c.nl = h.nl;
        c.first = first;
        c.last = last;
        c.segments = segments;
        c.nb = h.nb;
        c.micrometres = band.isEmpty() ? 0.0 : band.chopped(2).toDouble();
        c.pixels.assign(static_cast<std::size_t>(count), 0);
        c.have.assign(static_cast<std::size_t>(segments), false);
        existing = canvases_.emplace(key, std::move(c)).first;
        canvasBytes_ += bytes;
    }
    Canvas& c = existing->second;
    const auto index = static_cast<std::size_t>(seg - first);
    const auto segmentPixels = static_cast<std::size_t>(c.nc) * c.nl;
    std::copy_n(pixels.begin(), std::min(pixels.size(), segmentPixels),
                c.pixels.begin() + std::ptrdiff_t(index * segmentPixels));
    c.have[index] = true;
    const int got = int(std::count(c.have.begin(), c.have.end(), true));
    const QImage preview = channelImage(c);
    {
        std::lock_guard<std::mutex> lock(m_);
        pending_.stamp = stamp;
        pending_.preview = preview.scaled(std::min(preview.width(), 1200), std::min(preview.height(), 1200),
                                          Qt::KeepAspectRatio, Qt::SmoothTransformation);
        pending_.previewInfo = QStringLiteral("ELEKTRO-L %1 %2 · %3 · segment %4 of %5")
            .arg(service_, channel, when.toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"))).arg(got).arg(c.segments);
        pending_.message = pending_.previewInfo;
    }
    nadirstation::log::write(QStringLiteral("ELEKTRO-L %1: %2 segment %3 of %4 placed").arg(service_, channel).arg(got).arg(c.segments));
    if (got == c.segments) {
        writeCanvas(c, false);
        makeProducts(stamp, false);
    }
}

QImage XritProducts::channelImage(const Canvas& c) const {
    QImage img = toImage(c);
    if (c.micrometres >= 3.4) {
        // Infrared: cold is bright. Pixels with no data (count 0: space, segments not received,
        // lines cleared after a loss) stay black.
        for (int y = 0; y < img.height(); ++y) {
            uchar* line = img.scanLine(y);
            const std::uint16_t* src = c.pixels.data() + std::size_t(y) * c.nc;
            for (int x = 0; x < img.width(); ++x) line[x] = src[x] == 0 ? 0 : uchar(255 - line[x]);
        }
    }
    return img;
}

const XritProducts::Canvas* XritProducts::band(const QString& stamp, double lo, double hi) const {
    for (const auto& [key, c] : canvases_)
        if (c.stamp == stamp && c.micrometres >= lo && c.micrometres <= hi && c.any()) return &c;
    return nullptr;
}

namespace {
// Piecewise-linear colour table over 0..255.
QRgb ramp(const std::vector<std::pair<int, QColor>>& stops, int v) {
    for (std::size_t i = 1; i < stops.size(); ++i) {
        if (v <= stops[i].first) {
            const auto& a = stops[i - 1];
            const auto& b = stops[i];
            const double t = b.first == a.first ? 0.0 : double(v - a.first) / double(b.first - a.first);
            return qRgb(int(a.second.red() + t * (b.second.red() - a.second.red())),
                        int(a.second.green() + t * (b.second.green() - a.second.green())),
                        int(a.second.blue() + t * (b.second.blue() - a.second.blue())));
        }
    }
    return stops.back().second.rgb();
}
}  // namespace

void XritProducts::makeProducts(const QString& stamp, bool final) {
    struct Product { QString name; std::vector<std::pair<double, double>> inputs; };
    // Band windows in micrometres (MSU-GS: 0.6, 0.7, 0.9, 3.8, 6.4, 8.0, 8.7, 9.7, 10.7, 11.9).
    const std::vector<Product> products = {
        {QStringLiteral("VIS_false_colour"), {{0.85, 1.0}, {0.65, 0.8}, {0.5, 0.65}}},
        {QStringLiteral("IR_enhanced"), {{10.2, 11.2}}},
        {QStringLiteral("WV_enhanced"), {{5.7, 7.0}}},
        {QStringLiteral("IR_split_window"), {{10.2, 11.2}, {11.2, 12.5}}},
        {QStringLiteral("Day_cloud_phase"), {{0.65, 0.8}, {3.5, 4.0}, {10.2, 11.2}}},
    };
    for (const Product& p : products) {
        const QString key = stamp + QLatin1Char('|') + p.name;
        if (productsWritten_[key]) continue;
        std::vector<const Canvas*> in;
        bool complete = true;
        for (const auto& [lo, hi] : p.inputs) {
            const Canvas* c = band(stamp, lo, hi);
            if (!c) break;
            complete = complete && c->complete();
            in.push_back(c);
        }
        if (in.size() != p.inputs.size()) continue;
        if (!complete && !final) continue;
        const int w = in[0]->nc, hgt = in[0]->nl * in[0]->segments;
        bool same = true;
        for (const Canvas* c : in) same = same && c->nc == w && c->nl * c->segments == hgt;
        if (!same) continue;
        std::vector<QImage> ch;
        for (const Canvas* c : in) ch.push_back(channelImage(*c).convertToFormat(QImage::Format_Grayscale8));
        QImage out(w, hgt, QImage::Format_RGB32);
        if (out.isNull() || std::any_of(ch.begin(), ch.end(), [](const QImage& image) { return image.isNull(); })) continue;
        if (p.name == QLatin1String("VIS_false_colour") || p.name == QLatin1String("Day_cloud_phase")) {
            for (int y = 0; y < hgt; ++y) {
                const uchar* r = ch[0].constScanLine(y);
                const uchar* g = ch[1].constScanLine(y);
                const uchar* b = ch[2].constScanLine(y);
                QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
                for (int x = 0; x < w; ++x) o[x] = qRgb(r[x], g[x], b[x]);
            }
        } else if (p.name == QLatin1String("IR_split_window")) {
            // 10.7 minus 11.9 (as inverted counts): thin cirrus and dust stand out against thick cloud.
            for (int y = 0; y < hgt; ++y) {
                const uchar* a = ch[0].constScanLine(y);
                const uchar* b = ch[1].constScanLine(y);
                QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
                for (int x = 0; x < w; ++x) {
                    const int d = std::clamp(128 + 4 * (int(b[x]) - int(a[x])), 0, 255);
                    o[x] = ramp({{0, QColor(20, 30, 90)}, {110, QColor(60, 60, 60)}, {128, QColor(128, 128, 128)},
                                 {160, QColor(230, 200, 60)}, {255, QColor(230, 40, 30)}}, d);
                }
            }
        } else {
            // Grey for the warm end, colours for the coldest tops (IR) or the driest / moistest air (WV).
            const bool wv = p.name == QLatin1String("WV_enhanced");
            const std::vector<std::pair<int, QColor>> table = wv
                ? std::vector<std::pair<int, QColor>>{{0, QColor(110, 70, 30)}, {90, QColor(200, 160, 90)}, {140, QColor(230, 230, 230)},
                                                      {190, QColor(80, 160, 220)}, {230, QColor(30, 70, 170)}, {255, QColor(20, 30, 90)}}
                : std::vector<std::pair<int, QColor>>{{0, QColor(0, 0, 0)}, {170, QColor(225, 225, 225)}, {185, QColor(80, 160, 230)},
                                                      {205, QColor(40, 200, 80)}, {220, QColor(240, 220, 40)}, {235, QColor(240, 100, 30)},
                                                      {248, QColor(200, 20, 20)}, {255, QColor(255, 255, 255)}};
            for (int y = 0; y < hgt; ++y) {
                const uchar* a = ch[0].constScanLine(y);
                QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
                for (int x = 0; x < w; ++x) o[x] = ramp(table, a[x]);
            }
        }
        const QString dir = in[0]->dir;
        QDir().mkpath(dir);
        const QString path = QDir(dir).filePath(QStringLiteral("ELEKTRO-L_%1_%2_%3%4.png")
            .arg(service_, p.name, stamp, complete ? QString() : QStringLiteral("_partial")));
        if (!out.save(path)) continue;
        if (complete) productsWritten_[key] = true;
        std::lock_guard<std::mutex> lock(m_);
        addOutputLocked(stamp, path);
        pending_.message = QStringLiteral("ELEKTRO-L %1 %2 %3 written").arg(service_, p.name, stamp);
        nadirstation::log::write(pending_.message + QStringLiteral(": ") + path);
    }
    makeL2Products(stamp, final);
}

// Calibrated L2 products (elektrol2.h): once every band of the observation is in (all ten), or at the
// end with what arrived.
void XritProducts::makeL2Products(const QString& stamp, bool final) {
    const QString key = stamp + QStringLiteral("|L2");
    if (productsWritten_[key]) return;
    std::vector<const Canvas*> in;
    int complete = 0;
    for (const auto& [k, c] : canvases_)
        if (c.stamp == stamp && c.any() && c.micrometres > 0.0) { in.push_back(&c); complete += c.complete(); }
    if (in.empty() || (!final && complete < 10)) return;
    // Every band at the size of the window channel (10.7 um), or of the first one.
    const Canvas* ref = in.front();
    for (const Canvas* c : in) if (msuBandIndex(c->micrometres) == 8) ref = c;
    std::vector<L2Band> bands;
    for (const Canvas* c : in)
        if (c->nc == ref->nc && c->nl * c->segments == ref->nl * ref->segments)
            bands.push_back({c->micrometres, c->nc, c->nl * c->segments, c->pixels.data()});
    std::array<std::array<float, 1024>, 10> lut = nominalCalibration();
    bool nominal = true;
    {
        std::lock_guard<std::mutex> lock(m_);
        auto it = prologue_.find(stamp);
        if (it != prologue_.end()) { lut = it->second; nominal = false; }
    }
    const QDateTime t = QDateTime::fromString(stamp, QStringLiteral("yyyyMMdd_HHmm"));
    const QString when = t.isValid() ? t.toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'")) : stamp;
    const auto products = makeL2(bands, lut, nominal, when, service_);
    QDir().mkpath(ref->dir);
    int written = 0;
    for (const auto& p : products) {
        const QString path = QDir(ref->dir).filePath(QStringLiteral("ELEKTRO-L_%1_%2_%3.png").arg(service_, stamp, p.name));
        if (p.image.isNull() || !p.image.save(path)) continue;
        ++written;
        std::lock_guard<std::mutex> lock(m_);
        addOutputLocked(stamp, path);
    }
    productsWritten_[key] = true;
    const QString msg = QStringLiteral("ELEKTRO-L %1 %2: %3 L2 / RGB products from %4 bands (%5)")
        .arg(service_, stamp).arg(written).arg(bands.size()).arg(nominal ? QStringLiteral("nominal calibration") : QStringLiteral("prologue calibration"));
    post(msg);
}

QImage XritProducts::toImage(const Canvas& c) const {
    const int w = c.nc, hgt = c.nl * c.segments;
    QImage img(w, hgt, QImage::Format_Grayscale8);
    if (img.isNull()) return img;
    // Stretch from the 0.5 to 99.5 percentiles of the received part, so a 10-bit channel whose
    // signal uses a few hundred counts is not drawn almost black.
    std::vector<std::uint32_t> hist(1u << std::min(c.nb, 16), 0);
    std::size_t n = 0;
    for (int s = 0; s < c.segments; ++s) {
        if (!c.have[std::size_t(s)]) continue;
        const std::size_t from = std::size_t(s) * c.nl * c.nc, to = from + std::size_t(c.nl) * c.nc;
        for (std::size_t i = from; i < to; i += 7) { if (c.pixels[i] == 0) continue; ++hist[std::min<std::size_t>(c.pixels[i], hist.size() - 1)]; ++n; }
    }
    int lo = 0, hi = int(hist.size()) - 1;
    if (n > 0) {
        std::size_t acc = 0;
        for (int v = 0; v < int(hist.size()); ++v) { acc += hist[std::size_t(v)]; if (acc >= n / 200) { lo = v; break; } }
        acc = 0;
        for (int v = int(hist.size()) - 1; v >= 0; --v) { acc += hist[std::size_t(v)]; if (acc >= n / 200) { hi = v; break; } }
        if (hi <= lo) hi = lo + 1;
    }
    const double scale = 255.0 / double(hi - lo);
    for (int y = 0; y < hgt; ++y) {
        uchar* line = img.scanLine(y);
        const bool have = c.have[std::size_t(y / c.nl)];
        const std::uint16_t* src = c.pixels.data() + std::size_t(y) * w;
        for (int x = 0; x < w; ++x)
            line[x] = have ? uchar(std::clamp(int((src[x] - lo) * scale + 0.5), 0, 255)) : 0;
    }
    return img;
}

void XritProducts::writeCanvas(Canvas& c, bool partial) {
    if (c.written) return;
    QDir().mkpath(c.dir);
    const QString path = QDir(c.dir).filePath(QStringLiteral("ELEKTRO-L_%1_%2_%3%4.png")
        .arg(service_, c.channelName, c.stamp, partial ? QStringLiteral("_partial") : QString()));
    if (!channelImage(c).save(path)) {
        post(QStringLiteral("ELEKTRO-L %1: cannot save %2").arg(service_, path));
        return;
    }
    c.written = !partial;
    std::lock_guard<std::mutex> lock(m_);
    addOutputLocked(c.stamp, path);
    pending_.message = QStringLiteral("ELEKTRO-L %1 %2 %3 written%4")
        .arg(service_, c.channelName, c.stamp, partial ? QStringLiteral(" (partial)") : QString());
    nadirstation::log::write(pending_.message + QStringLiteral(": ") + path);
}

void XritProducts::closeObservation(const QString& stamp) {
    for (auto& [key, c] : canvases_) {
        if (c.stamp == stamp && !c.written && c.any()) writeCanvas(c, true);
    }
    makeProducts(stamp, true);
    const QString prefix = stamp + QLatin1Char('|');
    for (auto it = canvases_.lower_bound(prefix); it != canvases_.end() && it->first.startsWith(prefix); ) {
        canvasBytes_ -= it->second.pixels.size() * sizeof(std::uint16_t);
        it = canvases_.erase(it);
    }
    for (auto it = productsWritten_.lower_bound(prefix);
         it != productsWritten_.end() && it->first.startsWith(prefix); )
        it = productsWritten_.erase(it);
    outputs_.erase(stamp);
    evictedThrough_ = stamp;
}

void XritProducts::finalize() {
    {
        std::unique_lock<std::mutex> lock(m_);
        idle_.wait(lock, [this] { return queue_.empty() && !busy_; });
    }
    while (!canvases_.empty()) {
        const QString oldest = canvases_.begin()->second.stamp;
        closeObservation(oldest);
    }
}

void XritProducts::reset() {
    std::unique_lock<std::mutex> lock(m_);
    queue_.clear();
    queuedBytes_ = 0;
    idle_.wait(lock, [this] { return !busy_; });
    canvases_.clear();
    canvasBytes_ = 0;
    evictedThrough_.clear();
    productsWritten_.clear();
    outputs_.clear();
    pending_ = Update{};
    filesSeen_ = segments_ = skippedImages_ = archiveFailures_ = 0;
}

XritProducts::Update XritProducts::take() {
    std::lock_guard<std::mutex> lock(m_);
    Update u = std::move(pending_);
    pending_ = Update{};
    return u;
}

}  // namespace nadirstation::elektro

namespace nadirstation::elektro {
// Every picture of one observation so far, so the image page always gets the whole set (it replaces
// its list with what it is handed). A complete picture replaces its partial version.
void XritProducts::addOutputLocked(const QString& stamp, const QString& path) {
    QStringList& list = outputs_[stamp];
    QString base = path;
    base.remove(QStringLiteral("_partial"));
    for (int i = list.size() - 1; i >= 0; --i) {
        QString other = list.at(i);
        other.remove(QStringLiteral("_partial"));
        if (other == base) list.removeAt(i);
    }
    list << path;
    // Channels by wavelength first (the file names sort that way within a service), then products.
    std::sort(list.begin(), list.end(), [](const QString& a, const QString& b) {
        const bool pa = a.contains(QStringLiteral("um_")), pb = b.contains(QStringLiteral("um_"));
        if (pa != pb) return pa;
        if (pa) {
            const auto um = [](const QString& s) {
                const QString name = QFileInfo(s).fileName();
                return name.section(QLatin1Char('_'), 2, 2).chopped(2).toDouble();
            };
            return um(a) < um(b);
        }
        return a < b;
    });
    pending_.completed[stamp] = list;
}
}  // namespace nadirstation::elektro
