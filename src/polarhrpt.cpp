#include "polarhrpt.h"

#include <QDir>
#include <QImageWriter>
#include <QRegularExpression>
#include <QTimeZone>
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <functional>
#include <numeric>
#include <cstdlib>

namespace nadirstation::polar {
namespace {
constexpr double kTwoPi = 6.283185307179586;
constexpr double kPi = 3.141592653589793;

bool savePng(const QImage& image, const QString& path) {
    QImageWriter writer(path, "png");
    writer.setQuality(80);
    return writer.write(image);
}

// In-place radix-2 FFT (n a power of two).
void fft(std::vector<std::complex<double>>& a) {
    const std::size_t n = a.size();
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) std::swap(a[i], a[j]);
    }
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const double ang = -kTwoPi / double(len);
        const std::complex<double> wl(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const auto u = a[i + k], v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wl;
            }
        }
    }
}

// Low/high percentile of a sample of values (every `step`-th, zeros skipped).
std::pair<double, double> percentiles(const std::vector<std::uint16_t>& v, std::size_t from, std::size_t to,
                                      double lo, double hi) {
    std::vector<std::uint32_t> hist(1024, 0);
    std::size_t n = 0;
    const std::size_t step = std::max<std::size_t>(1, (to - from) / 400000);
    for (std::size_t i = from; i < to; i += step) {
        if (v[i] == 0) continue;
        ++hist[std::min<std::size_t>(v[i], 1023)];
        ++n;
    }
    if (n == 0) return {0.0, 1023.0};
    auto at = [&](double q) {
        const std::size_t target = std::size_t(q * double(n));
        std::size_t acc = 0;
        for (int i = 0; i < 1024; ++i) { acc += hist[std::size_t(i)]; if (acc > target) return double(i); }
        return 1023.0;
    };
    double a = at(lo), b = at(hi);
    if (b <= a) b = a + 1.0;
    return {a, b};
}

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

// =============================================================================================
// VcidDemux
std::vector<Packet> VcidDemux::feed(const std::uint8_t* cadu) {
    std::vector<Packet> out;
    const std::uint32_t count = (std::uint32_t(cadu[6]) << 16) | (std::uint32_t(cadu[7]) << 8) | cadu[8];
    const bool continuous = haveCount_ && count == ((lastCount_ + 1) & 0xFFFFFFu);
    if (haveCount_ && !continuous) ++gaps_;
    haveCount_ = true;
    lastCount_ = count;
    const int fhp = ((cadu[off_ - 2] & 0x07) << 8) | cadu[off_ - 1];
    const std::uint8_t* data = cadu + off_;
    if (!continuous) { buf_.clear(); synced_ = false; }
    if (!synced_) {
        if (fhp == 2047 || fhp >= size_) return out;   // no packet starts here
        buf_.assign(data + fhp, data + size_);
        synced_ = true;
    } else {
        buf_.insert(buf_.end(), data, data + size_);
    }
    std::size_t at = 0;
    while (buf_.size() - at >= 6) {
        const std::size_t len = ((std::size_t(buf_[at + 4]) << 8) | buf_[at + 5]) + 7;
        if (buf_.size() - at < len) break;
        Packet p;
        p.apid = ((buf_[at] & 0x07) << 8) | buf_[at + 1];
        p.bytes.assign(buf_.begin() + long(at), buf_.begin() + long(at + len));
        if (p.apid != 2047) out.push_back(std::move(p));
        at += len;
    }
    buf_.erase(buf_.begin(), buf_.begin() + long(at));
    // A header that does not parse (length beyond any real packet) means we lost alignment.
    if (buf_.size() > 65536) { buf_.clear(); synced_ = false; }
    return out;
}

// =============================================================================================
// Imager
Imager::Imager(QString instrument, int width) : instrument_(std::move(instrument)), width_(width) {}

QStringList Imager::slotNames(const QString& instrument) {
    if (instrument == QLatin1String("AVHRR"))
        return {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3A"), QStringLiteral("3B"),
                QStringLiteral("4"), QStringLiteral("5")};
    return {QStringLiteral("1"), QStringLiteral("2"), QStringLiteral("3"), QStringLiteral("4"),
            QStringLiteral("5"), QStringLiteral("6")};
}

void Imager::addLine(const std::array<const std::uint16_t*, 6>& channels, double unixTime) {
    std::lock_guard<std::mutex> lock(m_);
    for (int c = 0; c < 6; ++c) {
        auto& d = data_[std::size_t(c)];
        const std::size_t at = d.size();
        d.resize(at + std::size_t(width_), 0);
        if (channels[std::size_t(c)]) {
            std::memcpy(d.data() + at, channels[std::size_t(c)], std::size_t(width_) * sizeof(std::uint16_t));
            ++present_[std::size_t(c)];
        }
    }
    times_.push_back(unixTime);
}

int Imager::lines() const {
    std::lock_guard<std::mutex> lock(m_);
    return int(times_.size());
}

bool Imager::slotPresent(int slot) const {
    std::lock_guard<std::mutex> lock(m_);
    // A slot counts when it was sent on a fair share of lines (AVHRR switches 3A / 3B).
    return slot >= 0 && slot < 6 && present_[std::size_t(slot)] > std::max<std::size_t>(10, times_.size() / 20);
}

std::vector<double> Imager::lineTimes() const {
    std::lock_guard<std::mutex> lock(m_);
    return times_;
}

std::vector<std::uint16_t> Imager::slotCounts(int slot) const {
    std::lock_guard<std::mutex> lock(m_);
    if (slot < 0 || slot >= 6) return {};
    std::vector<std::uint16_t> v = data_[std::size_t(slot)];
    v.resize(times_.size() * std::size_t(width_), 0);
    return v;
}

QDateTime Imager::firstTime() const {
    std::lock_guard<std::mutex> lock(m_);
    for (double t : times_)
        if (t > 1e9) return QDateTime::fromMSecsSinceEpoch(qint64(t * 1000.0), QTimeZone::UTC);
    return {};
}

QImage Imager::preview(int maxLines, bool half, QString* what) const {
    std::lock_guard<std::mutex> lock(m_);
    const int total = int(times_.size());
    if (total == 0) return {};
    const int first = maxLines > 0 ? std::max(0, total - maxLines) : 0;
    const int rows = total - first;
    const int step = half ? 2 : 1;
    const int w = width_ / step, h = (rows + step - 1) / step;
    const std::size_t from = std::size_t(first) * std::size_t(width_), to = std::size_t(total) * std::size_t(width_);
    // Day or night: the near-infrared channel is almost empty at night.
    const auto nir = percentiles(data_[1], from, to, 0.5, 0.97);
    const bool day = present_[1] > 0 && nir.second > 160.0;
    QImage img(w, h, QImage::Format_RGB32);
    if (img.isNull()) return img;
    if (day) {
        const auto r = percentiles(data_[1], from, to, 0.005, 0.995);
        const auto b = percentiles(data_[0], from, to, 0.005, 0.995);
        for (int y = 0; y < h; ++y) {
            QRgb* o = reinterpret_cast<QRgb*>(img.scanLine(y));
            const std::size_t row = std::size_t(first + y * step) * std::size_t(width_);
            for (int x = 0; x < w; ++x) {
                const std::size_t i = row + std::size_t(x * step);
                const int rr = std::clamp(int((data_[1][i] - r.first) * 255.0 / (r.second - r.first)), 0, 255);
                const int bb = std::clamp(int((data_[0][i] - b.first) * 255.0 / (b.second - b.first)), 0, 255);
                o[x] = qRgb(rr, rr, bb);
            }
        }
        if (what) *what = QStringLiteral("221 false colour");
    } else {
        const auto t = percentiles(data_[4], from, to, 0.005, 0.995);
        // AVHRR's infrared counts already rise as the scene gets colder; MSU-MR's fall.
        const bool irInvert = instrument_ != QLatin1String("AVHRR");
        for (int y = 0; y < h; ++y) {
            QRgb* o = reinterpret_cast<QRgb*>(img.scanLine(y));
            const std::size_t row = std::size_t(first + y * step) * std::size_t(width_);
            for (int x = 0; x < w; ++x) {
                const int c = std::clamp(int((data_[4][row + std::size_t(x * step)] - t.first) * 255.0 / (t.second - t.first)), 0, 255);
                const int v = irInvert ? 255 - c : c;
                o[x] = qRgb(v, v, v);
            }
        }
        if (what) *what = QStringLiteral("10.8 µm infrared (cold bright)");
    }
    return img;
}

QImage Imager::channel16(int slot) const {
    const int h = int(times_.size());
    QImage img(width_, h, QImage::Format_Grayscale16);
    if (img.isNull()) return img;
    for (int y = 0; y < h; ++y) {
        auto* o = reinterpret_cast<quint16*>(img.scanLine(y));
        const std::uint16_t* s = data_[std::size_t(slot)].data() + std::size_t(y) * std::size_t(width_);
        for (int x = 0; x < width_; ++x) o[x] = quint16(std::min<int>(s[x], 1023) << 6);
    }
    return img;
}

QImage Imager::channel8(int slot, bool invert) const {
    const int h = int(times_.size());
    QImage img(width_, h, QImage::Format_Grayscale8);
    if (img.isNull()) return img;
    const auto& d = data_[std::size_t(slot)];
    const auto p = percentiles(d, 0, d.size(), 0.005, 0.995);
    for (int y = 0; y < h; ++y) {
        uchar* o = img.scanLine(y);
        const std::uint16_t* s = d.data() + std::size_t(y) * std::size_t(width_);
        for (int x = 0; x < width_; ++x) {
            int v = std::clamp(int((s[x] - p.first) * 255.0 / (p.second - p.first) + 0.5), 0, 255);
            o[x] = uchar(invert ? 255 - v : v);
        }
    }
    return img;
}

QStringList Imager::save(const QString& dir, const QString& stem) const {
    std::lock_guard<std::mutex> lock(m_);
    QStringList written;
    if (times_.empty()) return written;
    QDir().mkpath(dir);
    const QStringList names = slotNames(instrument_);
    const std::size_t minLines = std::max<std::size_t>(10, times_.size() / 20);
    auto present = [&](int s) { return present_[std::size_t(s)] > minLines; };
    for (int s = 0; s < 6; ++s) {
        if (!present(s)) continue;
        const QString path = QDir(dir).filePath(QStringLiteral("%1_CH%2.png").arg(stem, names[s]));
        if (savePng(channel16(s), path)) written << path;
    }
    const int w = width_, h = int(times_.size());
    auto rgb = [&](int r, int g, int b, const QString& name) {
        if (!present(r) || !present(g) || !present(b)) return;
        const QImage R = channel8(r, false), G = channel8(g, false), B = channel8(b, false);
        QImage out(w, h, QImage::Format_RGB32);
        if (out.isNull() || R.isNull()) return;
        for (int y = 0; y < h; ++y) {
            const uchar *pr = R.constScanLine(y), *pg = G.constScanLine(y), *pb = B.constScanLine(y);
            QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) o[x] = qRgb(pr[x], pg[x], pb[x]);
        }
        const QString path = QDir(dir).filePath(QStringLiteral("%1_RGB_%2.png").arg(stem, name));
        if (savePng(out, path)) written << path;
    };
    rgb(1, 1, 0, QStringLiteral("221_FalseColour"));
    rgb(2, 1, 0, QStringLiteral("321_NaturalColour"));
    // 10.8 um, cold tops coloured.
    if (present(4)) {
        const QImage t = channel8(4, instrument_ != QLatin1String("AVHRR"));
        QImage out(w, h, QImage::Format_RGB32);
        const std::vector<std::pair<int, QColor>> table{{0, QColor(0, 0, 0)}, {170, QColor(225, 225, 225)},
            {185, QColor(80, 160, 230)}, {205, QColor(40, 200, 80)}, {220, QColor(240, 220, 40)},
            {235, QColor(240, 100, 30)}, {248, QColor(200, 20, 20)}, {255, QColor(255, 255, 255)}};
        for (int y = 0; y < h && !out.isNull(); ++y) {
            const uchar* a = t.constScanLine(y);
            QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) o[x] = ramp(table, a[x]);
        }
        const QString pIr = QDir(dir).filePath(QStringLiteral("%1_IR108_Grey.png").arg(stem));
        if (savePng(t, pIr)) written << pIr;
        const QString pEn = QDir(dir).filePath(QStringLiteral("%1_IR108_Enhanced.png").arg(stem));
        if (!out.isNull() && savePng(out, pEn)) written << pEn;
    }
    // Night / any time: 3.7 um, 10.8 um, 12 um (inverted) -- fog and low cloud stand out.
    if (present(3) && present(4) && present(5)) {
        const QImage a = channel8(3, instrument_ != QLatin1String("AVHRR")), b = channel8(4, instrument_ != QLatin1String("AVHRR")), c = channel8(5, instrument_ != QLatin1String("AVHRR"));
        QImage out(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h && !out.isNull(); ++y) {
            const uchar *pa = a.constScanLine(y), *pb = b.constScanLine(y), *pc = c.constScanLine(y);
            QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) o[x] = qRgb(pa[x], pb[x], pc[x]);
        }
        const QString p = QDir(dir).filePath(QStringLiteral("%1_RGB_IR_FalseColour.png").arg(stem));
        if (!out.isNull() && savePng(out, p)) written << p;
    }
    return written;
}

// =============================================================================================
// AvhrrReader
void AvhrrReader::feed(const std::uint8_t* cadu, Imager& imager) {
    const int vcid = cadu[5] & 0x3F;
    scid_ = ((cadu[4] & 0x3F) << 2) | (cadu[5] >> 6);
    if (vcid != 9) return;
    for (auto& p : vc9_.feed(cadu)) {
        if (p.apid != 103 && p.apid != 104) continue;
        if (p.bytes.size() < 6 + 12960) continue;
        const std::uint8_t* pl = p.bytes.data() + 6;
        // 10-bit words from data field byte 14 on
        static thread_local std::vector<std::uint16_t> words;
        const std::size_t nWords = 12944 * 8 / 10;
        words.resize(nWords);
        for (std::size_t w = 0; w < nWords; ++w) {
            const std::size_t bit = w * 10;
            const std::size_t byte = 14 + bit / 8;
            const int shift = int(bit % 8);
            const std::uint32_t v = (std::uint32_t(pl[byte]) << 16) | (std::uint32_t(pl[byte + 1]) << 8) | pl[byte + 2];
            words[w] = std::uint16_t((v >> (14 - shift)) & 0x3FF);
        }
        static thread_local std::array<std::vector<std::uint16_t>, 5> ch;
        for (int c = 0; c < 5; ++c) {
            ch[std::size_t(c)].resize(2048);
            for (int i = 0; i < 2048; ++i) ch[std::size_t(c)][std::size_t(i)] = words[std::size_t(55 + c + i * 5)];
        }
        std::array<const std::uint16_t*, 6> slotPtr{};
        const bool ch3a = p.apid == 103;
        slotPtr[0] = ch[0].data();
        slotPtr[1] = ch[1].data();
        slotPtr[ch3a ? 2 : 3] = ch[2].data();
        slotPtr[4] = ch[3].data();
        slotPtr[5] = ch[4].data();
        const int days = (pl[0] << 8) | pl[1];
        const std::uint32_t ms = (std::uint32_t(pl[2]) << 24) | (std::uint32_t(pl[3]) << 16) | (std::uint32_t(pl[4]) << 8) | pl[5];
        const double t = (10957.0 + days) * 86400.0 + ms / 1000.0;
        imager.addLine(slotPtr, t);
        ++lines_;
    }
}

// =============================================================================================
// MeteorDemod
void MeteorDemod::configure(double sampleRate, double bitRate) {
    fs_ = sampleRate;
    rb_ = bitRate;
    T_ = fs_ / rb_;
    // Carrier loop: noise bandwidth about 3.8 kHz -- wide enough for the Doppler rate of a low pass,
    // two decades under the bit rate so the Manchester data does not leak into it. (Simulated: 1.2 to
    // 3.8 kHz make no measurable difference to the bit error rate.)
    const double zeta = 0.707, bn = 3800.0 / fs_;
    const double wn = 2.0 * bn / (zeta + 1.0 / (4.0 * zeta));
    kp_ = 2.0 * zeta * wn;
    ki_ = wn * wn;
    // Bit clock loop (Mueller & Muller on the Manchester matched filter), per bit.
    const double tb = 0.004, tz = 1.0;
    const double twn = tb / (tz + 1.0 / (4.0 * tz));
    tKp_ = 2.0 * tz * twn;
    tKi_ = twn * twn;
    acqLength_ = std::size_t(1) << 16;
    while (double(acqLength_) / fs_ < 0.02) acqLength_ <<= 1;
    reset();
}

void MeteorDemod::reset() {
    phase_ = freq_ = 0.0;
    amp_ = 0.0;
    pow_ = 0.0;
    cum_.clear();
    base_ = 0.0;
    next_ = 0.0;
    period_ = T_;
    prevY_ = 0.0;
    yAvg_ = 1.0;
    yVar_ = 1.0;
    lock_ = 0.0;
    snrDb_ = 0.0;
    sum_ = 0.0;
    acquiring_ = true;
    acqBuf_.clear();
    sampleIndex_ = 0;
    pending_.clear();
}

void MeteorDemod::acquire() {
    // The residual carrier is a clean line (Manchester leaves nothing at DC): average a few FFTs and
    // take the strongest bin within the search span, refined by a parabola through its neighbours.
    const std::size_t n = 1u << 15;
    std::vector<double> mag(n, 0.0);
    std::vector<std::complex<double>> a(n);
    int blocks = 0;
    for (std::size_t at = 0; at + n <= acqBuf_.size(); at += n) {
        for (std::size_t i = 0; i < n; ++i) {
            const double w = 0.5 - 0.5 * std::cos(kTwoPi * double(i) / double(n));
            a[i] = std::complex<double>(acqBuf_[at + i]) * w;
        }
        fft(a);
        for (std::size_t i = 0; i < n; ++i) mag[i] += std::norm(a[i]);
        ++blocks;
    }
    if (blocks == 0) return;
    const long span = long(std::min(searchHz_, 0.45 * fs_) / fs_ * double(n));
    long best = 0;
    double bestV = -1.0;
    for (long k = -span; k <= span; ++k) {
        const std::size_t i = std::size_t((k + long(n)) % long(n));
        if (mag[i] > bestV) { bestV = mag[i]; best = k; }
    }
    auto m = [&](long k) { return mag[std::size_t((k + long(n)) % long(n))]; };
    const double l = m(best - 1), c = m(best), r = m(best + 1);
    const double den = l - 2.0 * c + r;
    const double frac = std::fabs(den) > 1e-30 ? 0.5 * (l - r) / den : 0.0;
    freq_ = kTwoPi * (double(best) + std::clamp(frac, -0.5, 0.5)) / double(n);
    phase_ = 0.0;
    double p = 0.0;
    for (const auto& z : acqBuf_) p += std::norm(z);
    pow_ = p / double(acqBuf_.size());
    amp_ = std::sqrt(pow_) * 0.5;
    acquiring_ = false;
    ++acquisitions_;
}

void MeteorDemod::process(const std::vector<std::complex<float>>& iq, std::vector<float>& soft,
                          std::vector<std::complex<float>>* constellation) {
    if (acquiring_) {
        acqBuf_.insert(acqBuf_.end(), iq.begin(), iq.end());
        if (acqBuf_.size() < acqLength_) return;
        acquire();
        std::vector<std::complex<float>> buf;
        buf.swap(acqBuf_);
        // Bit-clock phase: the one that gives the most energy out of the matched filter on the
        // acquisition block (the carrier loop is run over it first).
        runSamples(buf.data(), buf.size(), soft, constellation);
        return;
    }
    runSamples(iq.data(), iq.size(), soft, constellation);
}

void MeteorDemod::runSamples(const std::complex<float>* x, std::size_t n, std::vector<float>& soft,
                             std::vector<std::complex<float>>* constellation) {
    // 1. carrier loop and detection: the quadrature part, normalised by the carrier amplitude.
    std::vector<double> d(n);
    double ph = phase_, fr = freq_, amp = amp_, pw = pow_;
    for (std::size_t i = 0; i < n; ++i) {
        const std::complex<double> z = std::complex<double>(x[i]) * std::complex<double>(std::cos(-ph), std::sin(-ph));
        pw += 1e-4 * (std::norm(z) - pw);
        amp += 2e-4 * (z.real() - amp);
        const double a = std::max(std::fabs(amp), 1e-3 * std::sqrt(pw) + 1e-12);
        const double err = std::clamp(z.imag() / a, -1.5, 1.5);
        fr += ki_ * err;
        ph += fr + kp_ * err;
        if (ph > kPi) ph -= kTwoPi; else if (ph < -kPi) ph += kTwoPi;
        d[i] = z.imag() / a;
    }
    phase_ = ph; freq_ = fr; amp_ = amp; pow_ = pw;
    // Carrier on the wrong side (amplitude negative means a pi slip): flip it back.
    if (amp_ < 0.0) { phase_ += kPi; amp_ = -amp_; }

    // 2. cumulative sum, sample k covers [k, k+1).
    if (cum_.empty()) { cum_.push_back(0.0); base_ = double(sampleIndex_); sum_ = 0.0; }
    const bool first = next_ == 0.0;
    cum_.reserve(cum_.size() + n);
    for (std::size_t i = 0; i < n; ++i) { sum_ += d[i]; cum_.push_back(sum_); }
    sampleIndex_ += n;
    const double end = base_ + double(cum_.size() - 1);   // time up to which the integral is known
    auto C = [&](double t) {
        const double rel = t - base_;
        if (rel <= 0.0) return cum_.front();
        const std::size_t k = std::size_t(rel);
        if (k + 1 >= cum_.size()) return cum_.back();
        const double f = rel - double(k);
        return cum_[k] + f * (cum_[k + 1] - cum_[k]);
    };
    auto mf = [&](double t) { return 2.0 * C(t - 0.5 * T_) - C(t - T_) - C(t); };
    if (first) {
        // Choose the bit-clock phase: 32 candidates over one bit, energy over the available bits.
        const double span = std::min(end - base_ - 2.0 * T_, 4000.0 * T_);
        double bestE = -1.0, bestP = 0.0;
        for (int k = 0; k < 32; ++k) {
            const double p = base_ + T_ + T_ * k / 32.0;
            double e = 0.0;
            for (double t = p; t < base_ + span; t += T_) e += std::fabs(mf(t));
            if (e > bestE) { bestE = e; bestP = p; }
        }
        next_ = bestP;
        period_ = T_;
    }
    // 3. bits: matched filter at the clock, Mueller & Muller timing error, soft value out.
    while (next_ + 1.0 <= end) {
        const double y = mf(next_);
        // Mueller & Muller, sign chosen so that a late clock is pulled back (checked in simulation).
        const double e = (y >= 0.0 ? prevY_ : -prevY_) - (prevY_ >= 0.0 ? y : -y);
        prevY_ = y;
        const double ay = std::fabs(y);
        yAvg_ += 0.002 * (ay - yAvg_);
        yVar_ += 0.002 * ((ay - yAvg_) * (ay - yAvg_) - yVar_);
        const double en = std::clamp(e / std::max(yAvg_, 1e-9), -2.0, 2.0);
        period_ += tKi_ * en * T_ * 0.25;
        period_ = std::clamp(period_, T_ * 0.995, T_ * 1.005);
        next_ += period_ + tKp_ * en * T_ * 0.25;
        const float s = float(y / std::max(yAvg_, 1e-9));
        soft.push_back(s);
        if (constellation && constellation->size() < 4096) constellation->emplace_back(s, 0.0f);
    }
    const double snr = yAvg_ * yAvg_ / std::max(yVar_, 1e-12);
    snrDb_ = 10.0 * std::log10(std::max(snr, 1e-6));
    lock_ = std::clamp((snrDb_ - 3.0) / 10.0, 0.0, 1.0);
    // 4. drop what the clock has passed (keep two bits of history).
    const double keepFrom = next_ - 3.0 * T_ - 2.0;
    if (keepFrom - base_ > 65536.0) {
        const std::size_t drop = std::size_t(keepFrom - base_);
        cum_.erase(cum_.begin(), cum_.begin() + long(drop));
        base_ += double(drop);
    }
}

// =============================================================================================
// MeteorDeframer
namespace {
constexpr std::uint32_t kAsm = 0x1ACFFC1Du;
constexpr int kFrameBits = 8192;
}

void MeteorDeframer::reset() {
    win_.clear();
    bits_.clear();
    state_ = 0;
    inverted_ = false;
    misses_ = good_ = 0;
    untilMarker_ = 0;
    collecting_ = false;
    reg_ = 0;
}

void MeteorDeframer::startFrame() {
    bits_.clear();
    bits_.reserve(kFrameBits);
    collecting_ = true;
    untilMarker_ = kFrameBits;
}

std::vector<std::vector<std::uint8_t>> MeteorDeframer::feed(const std::vector<float>& soft) {
    std::vector<std::vector<std::uint8_t>> out;
    for (float b : soft) {
        reg_ = (reg_ << 1) | (b > 0.0f ? 1u : 0u);
        if (collecting_) {
            bits_.push_back(b);
            if (--untilMarker_ > 0) continue;
            // 8192 bits since the last marker: the last 32 are the next marker.
            const int err = std::popcount(reg_ ^ (inverted_ ? ~kAsm : kAsm));
            markerErrors_ += std::size_t(err);
            const bool markerOk = err <= (state_ == 1 ? 5 : 8);
            if (markerOk || (state_ == 2 && misses_ < 4)) {
                if (state_ == 1 && !markerOk) { state_ = 0; collecting_ = false; continue; }
                state_ = 2;
                if (markerOk) misses_ = 0; else { ++misses_; ++flywheel_; }
                // The frame: marker as sent plus the 8160 bits that followed it.
                std::vector<std::uint8_t> f(1024, 0);
                f[0] = 0x1A; f[1] = 0xCF; f[2] = 0xFC; f[3] = 0x1D;
                for (int i = 0; i < kFrameBits - 32; ++i) {
                    const bool bit = (bits_[std::size_t(i)] > 0.0f) != inverted_;
                    if (bit) f[std::size_t(4 + i / 8)] |= std::uint8_t(0x80u >> (i % 8));
                }
                out.push_back(std::move(f));
                ++frames_;
                startFrame();
            } else {
                state_ = 0;
                collecting_ = false;
            }
            continue;
        }
        // Search: the marker in either polarity, at most 3 bits wrong.
        const int e0 = std::popcount(reg_ ^ kAsm), e1 = std::popcount(reg_ ^ ~kAsm);
        if (std::min(e0, e1) <= 3) {
            inverted_ = e1 < e0;
            state_ = 1;
            misses_ = 0;
            startFrame();
        }
    }
    return out;
}

// =============================================================================================
// MsuMrReader
namespace {
constexpr std::uint64_t kMsuSync = 0x0218A7A392DD9ABFull;
constexpr std::size_t kLineBits = 11850 * 8;
}

void MsuMrReader::reset() {
    bits_.clear();
    pos_ = 0;
    locked_ = false;
    misses_ = 0;
    serialVotes_ = {};
    lastTime_ = 0.0;
}

int MsuMrReader::serial() const {
    int best = -1, votes = 0;
    for (int i = 0; i < 16; ++i)
        if (serialVotes_[std::size_t(i)] > votes) { votes = serialVotes_[std::size_t(i)]; best = i; }
    return best;
}

void MsuMrReader::feedFrame(const std::uint8_t* frame, Imager& imager) {
    static const int kStart[4] = {22, 278, 534, 790};
    static const int kLen[4] = {238, 238, 238, 234};
    for (int part = 0; part < 4; ++part)
        for (int i = 0; i < kLen[part]; ++i) {
            const std::uint8_t v = frame[kStart[part] + i];
            for (int b = 7; b >= 0; --b) bits_.push_back((v >> b) & 1u);
        }
    auto syncErrors = [&](std::size_t at) {
        std::uint64_t r = 0;
        for (int i = 0; i < 64; ++i) r = (r << 1) | bits_[at + std::size_t(i)];
        return std::popcount(r ^ kMsuSync);
    };
    for (;;) {
        if (!locked_) {
            if (bits_.size() < pos_ + 64) break;
            std::uint64_t r = 0;
            std::size_t i = pos_;
            for (int k = 0; k < 64; ++k) r = (r << 1) | bits_[i + std::size_t(k)];
            bool found = false;
            for (;;) {
                if (std::popcount(r ^ kMsuSync) <= 6) { found = true; break; }
                if (i + 64 >= bits_.size()) break;
                r = (r << 1) | bits_[i + 64];
                ++i;
            }
            pos_ = i;
            if (!found) break;
            locked_ = true;
            misses_ = 0;
        }
        if (bits_.size() < pos_ + kLineBits + 64) break;
        if (syncErrors(pos_) > 10) {
            // Marker damaged: keep the line only when the next marker is where it should be;
            // otherwise the stream slipped (frames lost upstream) and the line is not whole.
            if (++misses_ > 3 || syncErrors(pos_ + kLineBits) > 10) { locked_ = false; ++pos_; continue; }
        } else {
            misses_ = 0;
        }
        std::vector<std::uint8_t> line(11850, 0);
        for (std::size_t k = 0; k < kLineBits; ++k)
            if (bits_[pos_ + k]) line[k / 8] |= std::uint8_t(0x80u >> (k % 8));
        takeLine(line, imager);
        pos_ += kLineBits;
    }
    if (pos_ > 4 * kLineBits) {
        const std::size_t drop = pos_ - kLineBits / 4;
        bits_.erase(bits_.begin(), bits_.begin() + long(drop));
        pos_ -= drop;
    }
}

void MsuMrReader::takeLine(const std::vector<std::uint8_t>& b, Imager& imager) {
    static thread_local std::array<std::vector<std::uint16_t>, 6> ch;
    std::array<const std::uint16_t*, 6> slotPtr{};
    for (int c = 0; c < 6; ++c) {
        auto& v = ch[std::size_t(c)];
        v.resize(1572);
        for (int g = 0; g < 393; ++g) {
            const std::size_t p = std::size_t(50 + g * 30 + c * 5);
            const std::uint64_t w = (std::uint64_t(b[p]) << 32) | (std::uint64_t(b[p + 1]) << 24) |
                                    (std::uint64_t(b[p + 2]) << 16) | (std::uint64_t(b[p + 3]) << 8) | b[p + 4];
            v[std::size_t(g * 4 + 0)] = std::uint16_t((w >> 30) & 0x3FF);
            v[std::size_t(g * 4 + 1)] = std::uint16_t((w >> 20) & 0x3FF);
            v[std::size_t(g * 4 + 2)] = std::uint16_t((w >> 10) & 0x3FF);
            v[std::size_t(g * 4 + 3)] = std::uint16_t(w & 0x3FF);
        }
        slotPtr[std::size_t(c)] = v.data();
    }
    // Moscow time of day; the date is the reception's (UTC).
    if (!day_.isValid()) day_ = QDateTime::currentDateTimeUtc().date();
    double t = 0.0;
    if (b[8] < 24 && b[9] < 60 && b[10] < 60) {
        const double sod = b[8] * 3600.0 + b[9] * 60.0 + b[10] + b[11] / 255.0 - 3 * 3600.0;
        t = double(QDateTime(day_, QTime(0, 0), QTimeZone::UTC).toSecsSinceEpoch()) + sod;
    }
    imager.addLine(slotPtr, t);
    ++serialVotes_[std::size_t(b[12] >> 4)];
    lastTime_ = t;
    ++lines_;
}

// =============================================================================================
// composeRgb
namespace {
struct Expr {
    // A small recursive-descent evaluator over whole images (each node yields one float per pixel).
    const QString& s;
    int i = 0;
    const std::map<QString, std::vector<float>>& ch;
    std::size_t n;
    QString error;
    QStringList known;
    Expr(const QString& src, const std::map<QString, std::vector<float>>& c, std::size_t count) : s(src), ch(c), n(count) {}
    void ws() { while (i < s.size() && s[i].isSpace()) ++i; }
    bool eat(QChar c) { ws(); if (i < s.size() && s[i] == c) { ++i; return true; } return false; }
    std::vector<float> fail(const QString& e) { if (error.isEmpty()) error = e; return std::vector<float>(n, 0.0f); }
    std::vector<float> expr() {
        auto a = term();
        for (;;) {
            if (eat('+')) { auto b = term(); for (std::size_t k = 0; k < n; ++k) a[k] += b[k]; }
            else if (eat('-')) { auto b = term(); for (std::size_t k = 0; k < n; ++k) a[k] -= b[k]; }
            else return a;
        }
    }
    std::vector<float> term() {
        auto a = unary();
        for (;;) {
            if (eat('*')) { auto b = unary(); for (std::size_t k = 0; k < n; ++k) a[k] *= b[k]; }
            else if (eat('/')) { auto b = unary(); for (std::size_t k = 0; k < n; ++k) a[k] = std::fabs(b[k]) > 1e-9f ? a[k] / b[k] : 0.0f; }
            else return a;
        }
    }
    std::vector<float> unary() {
        if (eat('-')) { auto a = unary(); for (auto& v : a) v = -v; return a; }
        if (eat('+')) return unary();
        return primary();
    }
    std::vector<std::vector<float>> args() {
        std::vector<std::vector<float>> a;
        if (!eat('(')) { fail(QStringLiteral("“(” expected after a function name")); return a; }
        if (eat(')')) return a;
        do { a.push_back(expr()); } while (eat(','));
        if (!eat(')')) fail(QStringLiteral("“)” missing"));
        return a;
    }
    std::vector<float> primary() {
        ws();
        if (eat('(')) { auto a = expr(); if (!eat(')')) return fail(QStringLiteral("“)” missing")); return a; }
        if (i < s.size() && (s[i].isDigit() || s[i] == '.')) {
            int j = i;
            while (j < s.size() && (s[j].isDigit() || s[j] == '.')) ++j;
            bool ok = false;
            const float v = s.mid(i, j - i).toFloat(&ok);
            i = j;
            if (!ok) return fail(QStringLiteral("bad number"));
            return std::vector<float>(n, v);
        }
        int j = i;
        while (j < s.size() && (s[j].isLetterOrNumber() || s[j] == '_')) ++j;
        if (j == i) return fail(QStringLiteral("unexpected “%1”").arg(i < s.size() ? QString(s[i]) : QStringLiteral("end")));
        const QString name = s.mid(i, j - i).toLower();
        i = j;
        auto it = ch.find(name);
        if (it != ch.end()) return it->second;
        if (name == QLatin1String("inv") || name == QLatin1String("abs") || name == QLatin1String("min") ||
            name == QLatin1String("max") || name == QLatin1String("clamp") || name == QLatin1String("pow")) {
            auto a = args();
            auto need = [&](std::size_t k) { return a.size() == k; };
            std::vector<float> r(n, 0.0f);
            if (name == QLatin1String("inv") && need(1)) for (std::size_t k = 0; k < n; ++k) r[k] = 1.0f - a[0][k];
            else if (name == QLatin1String("abs") && need(1)) for (std::size_t k = 0; k < n; ++k) r[k] = std::fabs(a[0][k]);
            else if (name == QLatin1String("min") && need(2)) for (std::size_t k = 0; k < n; ++k) r[k] = std::min(a[0][k], a[1][k]);
            else if (name == QLatin1String("max") && need(2)) for (std::size_t k = 0; k < n; ++k) r[k] = std::max(a[0][k], a[1][k]);
            else if (name == QLatin1String("pow") && need(2)) for (std::size_t k = 0; k < n; ++k) r[k] = std::pow(std::max(a[0][k], 0.0f), a[1][k]);
            else if (name == QLatin1String("clamp") && need(3)) for (std::size_t k = 0; k < n; ++k) r[k] = std::clamp(a[0][k], a[1][k], std::max(a[1][k], a[2][k]));
            else return fail(QStringLiteral("wrong number of arguments to %1()").arg(name));
            return r;
        }
        return fail(QStringLiteral("unknown channel “%1” (available: %2)").arg(name, known.join(QStringLiteral(", "))));
    }
};

std::vector<float> normalised(const QImage& src, const QSize& size) {
    QImage img = src.size() == size ? src : src.scaled(size, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    std::vector<float> out(std::size_t(size.width()) * std::size_t(size.height()));
    if (img.format() == QImage::Format_Grayscale16) {
        for (int y = 0; y < size.height(); ++y) {
            const auto* r = reinterpret_cast<const quint16*>(img.constScanLine(y));
            for (int x = 0; x < size.width(); ++x) out[std::size_t(y) * size.width() + x] = r[x] / 65535.0f;
        }
        return out;
    }
    img = img.convertToFormat(QImage::Format_Grayscale8);
    for (int y = 0; y < size.height(); ++y) {
        const uchar* r = img.constScanLine(y);
        for (int x = 0; x < size.width(); ++x) out[std::size_t(y) * size.width() + x] = r[x] / 255.0f;
    }
    return out;
}
}  // namespace

QImage composeRgb(const QString& formula, const std::map<QString, QImage>& channels, bool stretch, QString* error) {
    auto setError = [&](const QString& e) { if (error) *error = e; return QImage(); };
    const QStringList parts = formula.split(QLatin1Char(','));
    // Split on top-level commas only (function arguments contain commas too).
    QStringList comps;
    {
        int depth = 0;
        QString cur;
        for (QChar c : formula) {
            if (c == '(') ++depth;
            if (c == ')') --depth;
            if (c == ',' && depth == 0) { comps << cur; cur.clear(); continue; }
            cur += c;
        }
        comps << cur;
    }
    (void)parts;
    if (comps.size() != 1 && comps.size() != 3)
        return setError(QStringLiteral("Write three expressions separated by commas (R, G, B), or one for a grey picture."));
    if (channels.empty()) return setError(QStringLiteral("This observation has no channel images."));
    // Only the channels the formula names are loaded.
    QSize size;
    std::map<QString, std::vector<float>> used;
    std::vector<std::pair<QString, const QImage*>> named;
    for (const auto& [name, img] : channels)
        if (formula.contains(QRegularExpression(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(name)),
                                                QRegularExpression::CaseInsensitiveOption))) {
            named.emplace_back(name.toLower(), &img);
            if (img.width() * img.height() > size.width() * size.height()) size = img.size();
        }
    if (named.empty()) {
        QStringList known;
        for (const auto& [k, v] : channels) known << k;
        return setError(QStringLiteral("The formula names no channel of this observation (available: %1).").arg(known.join(QStringLiteral(", "))));
    }
    for (const auto& [name, img] : named) used[name] = normalised(*img, size);
    const std::size_t n = std::size_t(size.width()) * std::size_t(size.height());
    std::array<std::vector<float>, 3> rgb;
    for (int c = 0; c < int(comps.size()); ++c) {
        Expr e(comps[c], used, n);
        for (const auto& [k, v] : channels) e.known << k.toLower();
        rgb[std::size_t(c)] = e.expr();
        e.ws();
        if (e.error.isEmpty() && e.i < comps[c].size()) e.error = QStringLiteral("unexpected “%1”").arg(comps[c].mid(e.i).trimmed());
        if (!e.error.isEmpty()) return setError(QStringLiteral("Component %1: %2").arg(c + 1).arg(e.error));
    }
    if (comps.size() == 1) rgb[1] = rgb[2] = rgb[0];
    for (auto& v : rgb) {
        double lo = 0.0, hi = 1.0;
        if (stretch) {
            std::vector<float> sample;
            const std::size_t step = std::max<std::size_t>(1, n / 200000);
            for (std::size_t k = 0; k < n; k += step) sample.push_back(v[k]);
            std::sort(sample.begin(), sample.end());
            lo = sample[std::size_t(0.005 * double(sample.size() - 1))];
            hi = sample[std::size_t(0.995 * double(sample.size() - 1))];
            if (hi - lo < 1e-6) hi = lo + 1e-6;
        }
        const float a = float(lo), sc = float(1.0 / (hi - lo));
        for (auto& x : v) x = std::clamp((x - a) * sc, 0.0f, 1.0f);
    }
    QImage out(size, QImage::Format_RGB32);
    if (out.isNull()) return setError(QStringLiteral("Out of memory for a %1 × %2 picture.").arg(size.width()).arg(size.height()));
    for (int y = 0; y < size.height(); ++y) {
        QRgb* o = reinterpret_cast<QRgb*>(out.scanLine(y));
        const std::size_t row = std::size_t(y) * size.width();
        for (int x = 0; x < size.width(); ++x)
            o[x] = qRgb(int(rgb[0][row + x] * 255.0f + 0.5f), int(rgb[1][row + x] * 255.0f + 0.5f), int(rgb[2][row + x] * 255.0f + 0.5f));
    }
    if (error) error->clear();
    return out;
}

}  // namespace nadirstation::polar
