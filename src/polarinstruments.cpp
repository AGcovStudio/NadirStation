#include "polarinstruments.h"

#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QColor>
#include <algorithm>
#include <limits>
#include <cmath>
#include <QFile>
#include <QTextStream>

#include "iasi_brd_lengths.inc"

namespace nadirstation::polar {
namespace {

// CCSDS secondary header time of a MetOp packet (days since 2000-01-01, ms of day, us); 0 if none.
double packetTime(const Packet& p) {
    if (p.bytes.size() < 14) return 0.0;
    const std::uint8_t* pl = p.bytes.data() + 6;
    const int days = (pl[0] << 8) | pl[1];
    const std::uint32_t ms = (std::uint32_t(pl[2]) << 24) | (std::uint32_t(pl[3]) << 16) | (std::uint32_t(pl[4]) << 8) | pl[5];
    const int us = (pl[6] << 8) | pl[7];
    const double t = (10957.0 + days) * 86400.0 + ms / 1000.0 + us / 1e6;
    return (t > 1262304000.0 && t < 2208988800.0 && ms < 86400000u) ? t : 0.0;
}

bool writePng(const QImage& img, const QString& path) {
    if (img.isNull()) return false;
    QDir().mkpath(QFileInfo(path).absolutePath());
    return img.save(path, "PNG");
}

// Low/high cut of the non-zero values (fraction lo / hi of them below).
std::pair<double, double> cuts(std::vector<double> v, double lo = 0.01, double hi = 0.99) {
    v.erase(std::remove_if(v.begin(), v.end(), [](double x) { return !std::isfinite(x); }), v.end());
    if (v.empty()) return {0.0, 1.0};
    std::sort(v.begin(), v.end());
    const double a = v[std::size_t(lo * double(v.size() - 1))], b = v[std::size_t(hi * double(v.size() - 1))];
    return {a, b > a ? b : a + 1.0};
}

QRgb ramp(const std::vector<std::pair<int, QColor>>& stops, int v) {
    v = std::clamp(v, 0, 255);
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

// Microwave: cold (low) dark blue through cyan, green, yellow to warm red.
const std::vector<std::pair<int, QColor>>& microwaveTable() {
    static const std::vector<std::pair<int, QColor>> t{{0, QColor(20, 10, 70)}, {50, QColor(40, 70, 200)},
        {100, QColor(40, 190, 220)}, {145, QColor(60, 200, 90)}, {190, QColor(240, 220, 50)},
        {230, QColor(240, 110, 30)}, {255, QColor(190, 20, 20)}};
    return t;
}
// Infrared cloud tops: input 0 = warm .. 255 = cold (as the AVHRR 10.8 um enhancement).
const std::vector<std::pair<int, QColor>>& cloudTable() {
    static const std::vector<std::pair<int, QColor>> t{{0, QColor(0, 0, 0)}, {170, QColor(225, 225, 225)},
        {185, QColor(80, 160, 230)}, {205, QColor(40, 200, 80)}, {220, QColor(240, 220, 40)},
        {235, QColor(240, 100, 30)}, {248, QColor(200, 20, 20)}, {255, QColor(255, 255, 255)}};
    return t;
}

// A w x h field of values (NaN = missing) as a 16-bit grey (stretched), an 8-bit grey and an
// enhanced colour picture, the latter two enlarged by `zoom` for viewing.
void writeField(const QString& dir, const QString& name, const std::vector<double>& v, int w, int h, int zoom,
                bool mirror, QStringList& out, const QString& legendNote = {}) {
    Q_UNUSED(legendNote);
    if (w <= 0 || h <= 0) return;
    const auto [lo, hi] = cuts(v);
    QImage g16(w, h, QImage::Format_Grayscale16), g8(w, h, QImage::Format_Grayscale8), col(w, h, QImage::Format_RGB32);
    for (int y = 0; y < h; ++y) {
        auto* p16 = reinterpret_cast<quint16*>(g16.scanLine(y));
        uchar* p8 = g8.scanLine(y);
        QRgb* pc = reinterpret_cast<QRgb*>(col.scanLine(y));
        for (int x = 0; x < w; ++x) {
            const double val = v[std::size_t(y) * std::size_t(w) + std::size_t(mirror ? w - 1 - x : x)];
            if (!std::isfinite(val)) { p16[x] = 0; p8[x] = 0; pc[x] = qRgb(0, 0, 0); continue; }
            const double t = std::clamp((val - lo) / (hi - lo), 0.0, 1.0);
            p16[x] = quint16(std::lround(t * 65535.0));
            p8[x] = uchar(std::lround(t * 255.0));
            pc[x] = ramp(microwaveTable(), int(std::lround(t * 255.0)));
        }
    }
    const QString base = QDir(dir).filePath(name);
    if (writePng(g16, base + QStringLiteral(".png"))) out << base + QStringLiteral(".png");
    const QSize big(w * zoom, h * zoom);
    if (writePng(col.scaled(big, Qt::IgnoreAspectRatio, Qt::FastTransformation), base + QStringLiteral("_Enhanced.png")))
        out << base + QStringLiteral("_Enhanced.png");
}

double planckRadiance(double um, double t) {
    const double c1 = 1.191042e8, c2 = 1.4387752e4;   // W m-2 sr-1 um-1 with um
    return c1 / (std::pow(um, 5.0) * (std::exp(c2 / (um * t)) - 1.0));
}
double planckTemperature(double um, double l) {
    const double c1 = 1.191042e8, c2 = 1.4387752e4;
    if (l <= 0.0) return std::nan("");
    return c2 / (um * std::log(1.0 + c1 / (std::pow(um, 5.0) * l)));
}

const char* kMhsNames[5] = {"MHS_CH1_89GHz", "MHS_CH2_157GHz", "MHS_CH3_183p3pm1GHz", "MHS_CH4_183p3pm3GHz",
                            "MHS_CH5_190p3GHz"};
const char* kAmsuNames[15] = {"AMSU_CH01_23p8GHz", "AMSU_CH02_31p4GHz", "AMSU_CH03_50p3GHz", "AMSU_CH04_52p8GHz",
                              "AMSU_CH05_53p6GHz", "AMSU_CH06_54p4GHz", "AMSU_CH07_54p9GHz", "AMSU_CH08_55p5GHz",
                              "AMSU_CH09_57p29GHz", "AMSU_CH10_57p29GHz", "AMSU_CH11_57p29GHz", "AMSU_CH12_57p29GHz",
                              "AMSU_CH13_57p29GHz", "AMSU_CH14_57p29GHz", "AMSU_CH15_89GHz"};
}  // namespace

// =============================================================================================
// MetOp
void MetopInstruments::reset() {
    vc3_.reset(); vc10_.reset(); vc12_.reset();
    mhs_.clear(); amsu_.clear(); iisScans_.clear(); iisBbt_.clear(); iisCur_.clear();
    iisLastField_ = 0; lastBbt_ = 0.0;
    iisFieldTimes_.clear(); iisCurTimes_.fill(0.0); mapsLog_.clear();
    vc15_.reset(); vc24_.reset();
    iasiRows_.clear(); iasiRowBase_ = 0;
    for (auto& b : ascat6_) b.clear();
    ascatPulses_.clear();
    scid_ = -1;
    ascatLog_.clear();
    gomeScans_.clear(); gomeLastCounter_ = -1;
    semSamples_.clear();
    iasiSpectra_ = ascat_ = gome_ = sem_ = admin_ = 0;
}

void MetopInstruments::feed(const std::uint8_t* cadu) {
    const int vcid = cadu[5] & 0x3F;
    if (vcid != 63) scid_ = ((cadu[4] & 0x3F) << 2) | (cadu[5] >> 6);
    switch (vcid) {
    case 3: for (const auto& p : vc3_.feed(cadu)) { if (p.apid == 37) { ++sem_; semPacket(p); } else amsuPacket(p); } break;
    case 10: for (const auto& p : vc10_.feed(cadu)) iasiPacket(p); break;
    case 12: for (const auto& p : vc12_.feed(cadu)) mhsPacket(p); break;
    case 15: for (const auto& p : vc15_.feed(cadu)) { ++ascat_; ascatPacket(p); } break;
    case 24: for (const auto& p : vc24_.feed(cadu)) { ++gome_; gomePacket(p); } break;
    case 34: ++admin_; break;
    default: break;
    }
}

void MetopInstruments::mhsPacket(const Packet& p) {
    if (p.apid != 34 || p.bytes.size() < 1308) return;
    const std::uint8_t* b = p.bytes.data() + 6 + 14;
    MhsScan s;
    for (int i = 0; i < 90; ++i)
        for (int c = 0; c < 5; ++c) {
            const int at = 49 + i * 12 + (c + 1) * 2;
            s.ch[std::size_t(c)][std::size_t(i)] = std::uint16_t((b[at] << 8) | b[at + 1]);
        }
    // calibration data of the scan: PRTs, housekeeping, space (views 90..93) and warm (94..97) views
    s.time = packetTime(p);
    auto w16 = [&](int at) { return std::uint16_t((b[at] << 8) | b[at + 1]); };
    for (int i = 0; i < 3; ++i) s.prtCal[std::size_t(i)] = w16(1225 + 10 + i * 2);
    for (int i = 0; i < 5; ++i) s.prt[std::size_t(i)] = w16(1225 + i * 2);
    for (int i = 0; i < 39; ++i) s.hk[std::size_t(i)] = b[i];
    s.pie = (b[0] >> 3) & 1;
    static const int lim[2][2] = {{1500, 28000}, {20000, 64881}};
    for (int c = 0; c < 5; ++c)
        for (int j = 0; j < 2; ++j) {
            int v[4], n = 0;
            for (int k = 0; k < 4; ++k) {
                const int x = w16(49 + (90 + j * 4 + k) * 12 + (c + 1) * 2);
                if (x > lim[j][0] && x < lim[j][1]) v[n++] = x;
            }
            if (!n) continue;
            double avg = 0;
            for (int k = 0; k < n; ++k) avg += v[k];
            avg /= n;
            double sum = 0;
            int m = 0;
            for (int k = 0; k < n; ++k) if (std::fabs(v[k] - avg) <= 5000) { sum += v[k]; ++m; }
            s.views[std::size_t(c)][std::size_t(j)] = std::uint16_t(m ? sum / m : avg);
        }
    mhs_.push_back(s);
}

void MetopInstruments::amsuPacket(const Packet& p) {
    const bool a1 = p.apid == 39, a2 = p.apid == 40;
    if (!a1 && !a2) return;
    const std::size_t payload = p.bytes.size() - 6;
    if ((a1 && payload != 2096) || (a2 && payload != 1136)) return;
    const std::uint8_t* pl = p.bytes.data() + 6;
    std::vector<std::uint8_t> f;
    f.reserve(payload);
    for (std::size_t i = 13; i + 2 < payload; i += 2) {
        const int w = (pl[i + 1] << 8) | pl[i + 2];
        if (w != 1) { f.push_back(std::uint8_t(w >> 8)); f.push_back(std::uint8_t(w & 0xFF)); }
    }
    // A1 and A2 of one scan arrive close together: pair them into one record.
    if (amsu_.empty() || (a1 ? amsu_.back().a1 : amsu_.back().a2)) amsu_.emplace_back();
    AmsuScan& s = amsu_.back();
    if (a1) {
        if (f.size() < 1020 + 16 + 26) return;
        for (int v = 0; v < 30; ++v)
            for (int j = 0; j < 13; ++j) {
                const std::size_t at = std::size_t(v * 34 + 16 + 2 * j);
                s.ch[std::size_t(j + 2)][std::size_t(v)] = std::uint16_t((f[at] << 8) | f[at + 1]);
            }
        auto w16 = [&](std::size_t at) { return int((f[at] << 8) | f[at + 1]); };
        if (f.size() >= 1242) {
            for (int c = 0; c < 13; ++c) {
                s.a1Views[std::size_t(c)][0] = std::uint16_t((w16(std::size_t(1188 + 2 * c)) + w16(std::size_t(1214 + 2 * c))) / 2);
                s.a1Views[std::size_t(c)][1] = std::uint16_t((w16(std::size_t(1036 + 2 * c)) + w16(std::size_t(1062 + 2 * c))) / 2);
            }
            for (int n = 0; n < 45; ++n) s.a1Temps[std::size_t(n)] = std::uint16_t(w16(std::size_t(1088 + 2 * n)));
        }
        s.time = packetTime(p);
        s.a1 = true;
    } else {
        if (f.size() < 240 + 16) return;
        for (int v = 0; v < 30; ++v)
            for (int j = 0; j < 2; ++j) {
                const std::size_t at = std::size_t(v * 8 + 12 + 2 * j);
                s.ch[std::size_t(j)][std::size_t(v)] = std::uint16_t((f[at] << 8) | f[at + 1]);
            }
        auto w16 = [&](std::size_t at) { return int((f[at] << 8) | f[at + 1]); };
        if (f.size() >= 312) {
            for (int c = 0; c < 2; ++c) {
                s.a2Views[std::size_t(c)][0] = std::uint16_t((w16(std::size_t(304 + 2 * c)) + w16(std::size_t(308 + 2 * c))) / 2);
                s.a2Views[std::size_t(c)][1] = std::uint16_t((w16(std::size_t(252 + 2 * c)) + w16(std::size_t(256 + 2 * c))) / 2);
            }
            for (int n = 0; n < 19; ++n) s.a2Temps[std::size_t(n)] = std::uint16_t(w16(std::size_t(260 + 2 * n)));
        }
        if (s.time == 0.0) s.time = packetTime(p);
        s.a2 = true;
    }
}

void MetopInstruments::iasiPacket(const Packet& p) {
    if (p.apid == 130 || p.apid == 135 || p.apid == 140 || p.apid == 145) { ++iasiSpectra_; iasiSpectrum(p); return; }
    const std::uint8_t* pl = p.bytes.data() + 6;
    const std::size_t payload = p.bytes.size() - 6;
    if (p.apid == 180) {
        if (payload < 776) return;
        const std::uint8_t* w = pl + 14;
        const std::uint32_t bbt = (std::uint32_t(w[8]) << 24) | (std::uint32_t(w[9]) << 16) | (std::uint32_t(w[10]) << 8) | w[11];
        const double k = double(bbt) / 1000.0;
        if (k > 200.0 && k < 350.0) lastBbt_ = k;
        return;
    }
    if (p.apid != 150 || payload < 6196) return;
    const int field = pl[16];
    if (field < 1 || field > 36) return;
    if (iisCur_.empty() || field <= iisLastField_) {
        if (!iisCur_.empty()) { iisScans_.push_back(std::move(iisCur_)); iisBbt_.push_back(lastBbt_); iisFieldTimes_.push_back(iisCurTimes_); }
        iisCur_.assign(64 * 36 * 64, 0);
        iisCurTimes_.fill(0.0);
    }
    iisLastField_ = field;
    iisCurTimes_[std::size_t(field - 1)] = packetTime(p);
    // 4096 12-bit samples, two per three bytes; sample (y * 64 + i) is row i, column y of the field
    const std::uint8_t* raw = pl + 50;
    for (int k = 0; k < 4096; k += 2) {
        const std::uint8_t a = raw[k / 2 * 3], b = raw[k / 2 * 3 + 1], c = raw[k / 2 * 3 + 2];
        const std::uint16_t s0 = std::uint16_t((a << 4) | (b >> 4)), s1 = std::uint16_t(((b & 0x0F) << 8) | c);
        for (int q = 0; q < 2; ++q) {
            const int idx = k + q, y = idx / 64, i = idx % 64;
            iisCur_[std::size_t(i) * 2304 + std::size_t(2303 - ((field - 1) * 64 + y))] = q ? s1 : s0;
        }
    }
    if (field == 36) { iisScans_.push_back(std::move(iisCur_)); iisBbt_.push_back(lastBbt_); iisFieldTimes_.push_back(iisCurTimes_); iisCur_.clear(); iisLastField_ = 0; }
}

namespace {
// IASI spectral samples kept, and their approximate wavenumber (645 + 0.25 k cm-1).
constexpr int kIasiSamples[6] = {220, 1020, 1580, 2440, 3420, 7420};
const char* kIasiNames[6] = {"IASI_700cm-1_CO2_14p3um", "IASI_900cm-1_Window_11p1um", "IASI_1040cm-1_O3_9p6um",
                             "IASI_1255cm-1_8p0um", "IASI_1500cm-1_WV_6p7um", "IASI_2500cm-1_4p0um"};
double halfFloat(std::uint16_t s) {
    const bool neg = (s >> 15) & 1;
    const int e = (s >> 7) & 0xFF, f = s & 0x7F;
    if (e == 255) return 0.0;
    if (e == 0) return f == 0 ? 0.0 : (neg ? -1.0 : 1.0) * std::ldexp(double(f) / 128.0, -126);
    return (neg ? -1.0 : 1.0) * std::ldexp(double(f) / 128.0 + 1.0, e - 127);
}
}  // namespace

void MetopInstruments::iasiSpectrum(const Packet& p) {
    const std::size_t payload = p.bytes.size() - 6;
    if (payload < 8954) return;
    const std::uint8_t* pl = p.bytes.data() + 6;
    const int counter = pl[16];
    if (counter < 1 || counter > 30) return;
    const int row = (p.apid == 130 || p.apid == 140) ? 1 : 0;
    const int col = (p.apid == 130 || p.apid == 135) ? 1 : 0;
    while (int(iasiRows_.size()) < iasiRowBase_ + 2) iasiRows_.emplace_back();
    // bit offset of each kept sample
    static const auto offsets = [] {
        std::array<std::pair<int, int>, 6> o{};
        int bit = 0, k = 0;
        for (int seg = 0; seg < 523; ++seg)
            for (int s = 0; s < 16; ++s, ++k) {
                for (int i = 0; i < 6; ++i) if (kIasiSamples[i] == k) o[std::size_t(i)] = {bit, kIasiSegmentBits[seg]};
                bit += kIasiSegmentBits[seg];
            }
        return o;
    }();
    const std::uint8_t* w = pl + 314;
    auto bitAt = [&](int pos) { const int word = pos / 16, b = pos % 16; const int v = (w[word * 2] << 8) | w[word * 2 + 1]; return (v >> b) & 1; };
    for (int i = 0; i < kIasiSel; ++i) {
        const auto [start, len] = offsets[std::size_t(i)];
        unsigned v = 0;
        for (int b = 0; b < len; ++b) v |= unsigned(bitAt(start + b)) << b;
        iasiRows_[std::size_t(iasiRowBase_ + row)][std::size_t(i)][std::size_t(59 - ((counter - 1) * 2 + col))] = std::uint16_t(v << (16 - len));
    }
    if (counter == 30 && p.apid == 130) iasiRowBase_ += 2;
}

void MetopInstruments::ascatPacket(const Packet& p) {
    const int beam = p.apid - 208;
    if (beam < 0 || beam > 5 || p.bytes.size() - 6 < 654) return;
    const std::uint8_t* pl = p.bytes.data() + 6;
    std::array<float, 256> r{};
    for (int i = 0; i < 256; ++i) r[std::size_t(i)] = float(halfFloat(std::uint16_t((pl[140 + 2 * i] << 8) | pl[141 + 2 * i])));
    ascat6_[std::size_t(beam)].push_back(r);
    // time: CCSDS secondary header (days since 2000-01-01, ms of day, us); else the system clock
    AscatPulse a;
    a.beam = beam;
    a.power = r;
    const int days = (pl[0] << 8) | pl[1];
    const std::uint32_t ms = (std::uint32_t(pl[2]) << 24) | (std::uint32_t(pl[3]) << 16) | (std::uint32_t(pl[4]) << 8) | pl[5];
    const int us = (pl[6] << 8) | pl[7];
    const double t = (10957.0 + days) * 86400.0 + ms / 1000.0 + us / 1e6;
    if (t > 1262304000.0 && t < 2208988800.0 && ms < 86400000u) a.time = t;   // 2010 .. 2040
    else { a.time = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0; a.systemTime = true; }
    ascatPulses_.push_back(a);
}

void MetopInstruments::gomePacket(const Packet& p) {
    if (p.apid != 384 || p.bytes.size() - 6 < 18732) return;
    const std::uint8_t* h = p.bytes.data() + 6 + 14;
    auto word = [&](int i) { return std::uint16_t((h[2 * i] << 8) | h[2 * i + 1]); };
    const int counter = word(6);
    if (counter > 15) return;
    if (gomeScans_.empty() || counter <= gomeLastCounter_) gomeScans_.emplace_back();
    gomeLastCounter_ = counter;
    auto& px = gomeScans_.back().px;
    for (int r = 0; r < 2; ++r)
        for (int det = 0; det < 4; ++det) {
            const int base = 1158 + (r * 4 + det) * 1025 + 1;
            const int pos = 31 - (counter * 2 + r);
            for (int k = 0; k < 1024; ++k) px[std::size_t((det * 1024 + k) * 32 + pos)] = word(base + k);
        }
}

void MetopInstruments::semPacket(const Packet& p) {
    if (p.bytes.size() - 6 < 656) return;
    const std::uint8_t* pl = p.bytes.data() + 6;
    for (int i = 0; i < 640; i += 40) {
        std::array<std::uint8_t, 40> s{};
        for (int j = 0; j < 40; ++j) s[std::size_t(j)] = pl[15 + i + j] ^ 0xFF;
        semSamples_.push_back(s);
    }
}

QString MetopInstruments::summary() const {
    std::size_t a = 0;
    for (const auto& s : amsu_) if (s.a1 || s.a2) ++a;
    return QStringLiteral("MHS %1 scans, AMSU-A %2 scans, IASI imager %3 scans, IASI %4 rows, ASCAT %5 pulses, GOME-2 %6 scans, SEM %7 samples, admin %8 frames")
        .arg(mhs_.size()).arg(a).arg(iisScans_.size()).arg(iasiRows_.size()).arg(ascat_).arg(gomeScans_.size()).arg(semSamples_.size()).arg(admin_);
}

QStringList MetopInstruments::save(const QString& dir) const {
    QStringList out;
    if (mhs_.size() >= 2) {
        const int h = int(mhs_.size());
        for (int c = 0; c < 5; ++c) {
            std::vector<double> v(std::size_t(h) * 90);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < 90; ++x) {
                    const auto n = mhs_[std::size_t(y)].ch[std::size_t(c)][std::size_t(x)];
                    v[std::size_t(y) * 90 + std::size_t(x)] = n ? double(n) : std::nan("");
                }
            writeField(dir, QString::fromLatin1(kMhsNames[c]), v, 90, h, 6, true, out);
        }
    }
    if (amsu_.size() >= 2) {
        const int h = int(amsu_.size());
        for (int c = 0; c < 15; ++c) {
            std::vector<double> v(std::size_t(h) * 30, std::nan(""));
            bool any = false;
            for (int y = 0; y < h; ++y) {
                const auto& s = amsu_[std::size_t(y)];
                if (c < 2 ? !s.a2 : !s.a1) continue;
                for (int x = 0; x < 30; ++x) {
                    const auto n = s.ch[std::size_t(c)][std::size_t(x)];
                    if (n) { v[std::size_t(y) * 30 + std::size_t(x)] = double(n); any = true; }
                }
            }
            if (any) writeField(dir, QString::fromLatin1(kAmsuNames[c]), v, 30, h, 16, true, out);
        }
    }
    if (!iisScans_.empty()) {
        // Per scan and per detector pixel (x, y of the 64 x 64 field): cold space (fields 1, 2)
        // and blackbody (fields 4, 5), as laid out after the column reversal above.
        const int scans = int(iisScans_.size()), W = 30 * 64, H = scans * 64;
        std::vector<double> bt(std::size_t(W) * std::size_t(H), std::nan(""));
        const double um = 11.4;
        bool nominalBb = false;
        for (int s = 0; s < scans; ++s) {
            const auto& f = iisScans_[std::size_t(s)];
            double tbb = iisBbt_[std::size_t(s)];
            if (!(tbb > 200.0)) { tbb = 290.0; nominalBb = true; }
            const double lbb = planckRadiance(um, tbb);
            for (int y = 0; y < 64; ++y)
                for (int x = 0; x < 64; ++x) {
                    auto px = [&](int fieldCol) { return double(f[std::size_t(y) * 2304 + std::size_t(fieldCol * 64 + x)]); };
                    const double cold = 0.5 * (px(0) + px(1)), warm = 0.5 * (px(3) + px(4));
                    if (cold <= 0.0 || warm <= 0.0 || std::fabs(warm - cold) < 1.0) continue;
                    for (int t = 0; t < 30; ++t) {
                        const double c = px(6 + t);
                        if (c <= 0.0) continue;
                        const double l = (c - cold) / (warm - cold) * lbb;
                        bt[std::size_t(s * 64 + y) * std::size_t(W) + std::size_t(t * 64 + x)] = planckTemperature(um, l);
                    }
                }
        }
        QImage t16(W, H, QImage::Format_Grayscale16), grey(W, H, QImage::Format_Grayscale8), en(W, H, QImage::Format_RGB32);
        for (int y = 0; y < H; ++y) {
            auto* p16 = reinterpret_cast<quint16*>(t16.scanLine(y));
            uchar* pg = grey.scanLine(y);
            QRgb* pe = reinterpret_cast<QRgb*>(en.scanLine(y));
            for (int x = 0; x < W; ++x) {
                const double k = bt[std::size_t(y) * std::size_t(W) + std::size_t(x)];
                if (!std::isfinite(k) || k < 150.0 || k > 345.0) { p16[x] = 0; pg[x] = 0; pe[x] = qRgb(0, 0, 0); continue; }
                p16[x] = quint16(std::lround(k * 100.0));
                // 330 K -> 0 (black) .. 180 K -> 255 (white), cold bright
                const int g = std::clamp(int(std::lround((330.0 - k) / 150.0 * 255.0)), 0, 255);
                pg[x] = uchar(g);
                pe[x] = ramp(cloudTable(), g);
            }
        }
        const QString note = nominalBb ? QStringLiteral("_nominalBB") : QString();
        const QString a = QDir(dir).filePath(QStringLiteral("IASI_IIS_TBB_0p01K%1.png").arg(note));
        const QString b = QDir(dir).filePath(QStringLiteral("IASI_IIS_Grey.png"));
        const QString c = QDir(dir).filePath(QStringLiteral("IASI_IIS_Enhanced.png"));
        if (writePng(t16, a)) out << a;
        if (writePng(grey, b)) out << b;
        if (writePng(en, c)) out << c;
    }
    // IASI spectra: a few channels, counts (uncalibrated), 60 positions per row.
    if (iasiRows_.size() >= 4) {
        const int h = int(iasiRows_.size());
        for (int i = 0; i < kIasiSel; ++i) {
            std::vector<double> v(std::size_t(h) * 60);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < 60; ++x) {
                    const auto n = iasiRows_[std::size_t(y)][std::size_t(i)][std::size_t(x)];
                    v[std::size_t(y) * 60 + std::size_t(x)] = n ? double(n) : std::nan("");
                }
            writeField(dir, QString::fromLatin1(kIasiNames[i]), v, 60, h, 8, false, out);
        }
    }
    // ASCAT: echo power per beam (dB, relative), and the six beams arranged as the antennas sit.
    {
        int h = 1 << 30;
        for (const auto& b : ascat6_) h = std::min(h, int(b.size()));
        if (h >= 4) {
            std::array<std::vector<double>, 6> db;
            for (int b = 0; b < 6; ++b) {
                const auto& rows = ascat6_[std::size_t(b)];
                const int hb = int(rows.size());
                db[std::size_t(b)].assign(std::size_t(hb) * 256, std::nan(""));
                for (int y = 0; y < hb; ++y)
                    for (int x = 0; x < 256; ++x) {
                        const double p = rows[std::size_t(y)][std::size_t(x)];
                        if (p > 0.0) db[std::size_t(b)][std::size_t(y) * 256 + std::size_t(x)] = 10.0 * std::log10(p);
                    }
                writeField(dir, QStringLiteral("ASCAT_Beam%1").arg(b + 1), db[std::size_t(b)], 256, hb, 1, false, out);
            }
            // rows: beams 6 | 3 (mirrored), 5 (mirrored) | 2, 4 (mirrored) | 1
            const int layout[3][2] = {{6, 3}, {5, 2}, {4, 1}};
            const bool mir[3][2] = {{false, true}, {true, false}, {true, false}};
            std::vector<double> all(std::size_t(512) * std::size_t(h * 3), std::nan(""));
            for (int r = 0; r < 3; ++r)
                for (int c = 0; c < 2; ++c) {
                    const auto& src = db[std::size_t(layout[r][c] - 1)];
                    for (int y = 0; y < h; ++y)
                        for (int x = 0; x < 256; ++x)
                            all[std::size_t(r * h + y) * 512 + std::size_t(c * 256 + x)] = src[std::size_t(y) * 256 + std::size_t(mir[r][c] ? 255 - x : x)];
                }
            writeField(dir, QStringLiteral("ASCAT_AllBeams"), all, 512, h * 3, 1, false, out);
        }
    }
    // ASCAT located on the Earth (map products)
    if (!ascatPulses_.empty()) {
        AscatL2Input in;
        in.pulses = ascatPulses_;
        in.tleName = ascatTleName_;
        in.tleLine1 = ascatTle1_;
        in.tleLine2 = ascatTle2_;
        in.mapsDir = mapsDir_;
        out << makeAscatL2(in, dir, &ascatLog_);
    }
    // MHS, AMSU-A and IASI imager on the map (brightness temperature)
    out << saveMaps(dir);
    // GOME-2: quick-look colour (red ~650 nm, green ~550 nm, blue ~470 nm, by pixel position on the
    // detectors, not wavelength-calibrated) and the mean of each detector.
    if (gomeScans_.size() >= 2) {
        const int h = int(gomeScans_.size());
        auto band = [&](int det, int k0, int k1) {
            std::vector<double> v(std::size_t(h) * 32, std::nan(""));
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < 32; ++x) {
                    double s = 0; int n = 0;
                    for (int k = k0; k <= k1; ++k) {
                        const auto c = gomeScans_[std::size_t(y)].px[std::size_t((det * 1024 + k) * 32 + x)];
                        if (c && c != 65535) { s += c; ++n; }
                    }
                    if (n) v[std::size_t(y) * 32 + std::size_t(x)] = s / n;
                }
            return v;
        };
        const auto r = band(3, 299, 315), g = band(2, 749, 765), b = band(2, 353, 369);
        auto stretch = [](const std::vector<double>& v) {
            const auto [lo, hi] = cuts(v);
            std::vector<int> o(v.size());
            for (std::size_t i = 0; i < v.size(); ++i) o[i] = std::isfinite(v[i]) ? std::clamp(int((v[i] - lo) / (hi - lo) * 255.0), 0, 255) : 0;
            return o;
        };
        const auto R = stretch(r), Gc = stretch(g), B = stretch(b);
        QImage tc(32, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < 32; ++x) {
                const std::size_t i = std::size_t(y) * 32 + std::size_t(x);
                tc.setPixel(x, y, qRgb(R[i], Gc[i], B[i]));
            }
        const QString p = QDir(dir).filePath(QStringLiteral("GOME2_QuickLook_Colour.png"));
        if (writePng(tc.scaled(32 * 12, h * 12, Qt::IgnoreAspectRatio, Qt::FastTransformation), p)) out << p;
        const char* names[4] = {"GOME2_Detector1_UV1", "GOME2_Detector2_UV2", "GOME2_Detector3_Vis", "GOME2_Detector4_Red"};
        for (int d = 0; d < 4; ++d) writeField(dir, QString::fromLatin1(names[d]), band(d, 300, 700), 32, h, 12, false, out);
    }
    // SEM: counts of the 40 particle channels over time, as a heat map, and the numbers as CSV.
    if (semSamples_.size() >= 2) {
        const int n = int(semSamples_.size()), rowH = 10;
        QImage img(std::max(n * 2, 200), 40 * rowH, QImage::Format_RGB32);
        img.fill(qRgb(0, 0, 0));
        for (int c = 0; c < 40; ++c) {
            int mx = 1;
            for (const auto& s : semSamples_) mx = std::max(mx, int(s[std::size_t(c)]));
            for (int t = 0; t < n; ++t) {
                const int v = int(std::lround(std::log1p(double(semSamples_[std::size_t(t)][std::size_t(c)])) / std::log1p(double(mx)) * 255.0));
                const QRgb col = ramp(microwaveTable(), v);
                for (int yy = 0; yy < rowH - 1; ++yy)
                    for (int xx = 0; xx < 2; ++xx) img.setPixel(t * 2 + xx, c * rowH + yy, col);
            }
        }
        const QString p = QDir(dir).filePath(QStringLiteral("SEM_Channels_Heatmap.png"));
        if (writePng(img, p)) out << p;
        QFile f(QDir(dir).filePath(QStringLiteral("SEM_Counts.csv")));
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "sample_2s";
            for (int c = 0; c < 40; ++c) ts << ",ch" << (c + 1);
            ts << "\n";
            for (int t = 0; t < n; ++t) {
                ts << t;
                for (int c = 0; c < 40; ++c) ts << ',' << int(semSamples_[std::size_t(t)][std::size_t(c)]);
                ts << "\n";
            }
        }
    }
    return out;
}

// =============================================================================================
// METEOR MTVZA
void MtvzaReader::reset() {
    stream_.clear(); scans_.clear(); scanTimes_.clear(); cur_ = {}; curAny_ = false;
    msuTime_ = 0.0; otherFrames_ = 0; lastCounter_ = 0; mapsLog_.clear();
}

void MtvzaReader::feedFrame(const std::uint8_t* frame, double msuMrTime) {
    msuTime_ = msuMrTime;
    for (int at : {14, 270, 526, 782}) stream_.insert(stream_.end(), frame + at, frame + at + 8);
    std::size_t i = 0;
    while (i + 248 <= stream_.size()) {
        const std::uint8_t* f = stream_.data() + i;
        const bool plain = f[0] == 0xFB && f[1] == 0x38 && f[2] == 0x6A && f[3] == 0x45;
        const bool swapped = f[0] == 0x38 && f[1] == 0xFB && f[2] == 0x45 && f[3] == 0x6A;
        if (plain || swapped) { frame248(f, swapped); i += 248; }
        else ++i;
    }
    stream_.erase(stream_.begin(), stream_.begin() + long(i));
    if (stream_.size() > 4096) stream_.clear();
}

void MtvzaReader::frame248(const std::uint8_t* f, bool swapped) {
    const int counter = swapped ? f[4] : f[5];
    if (counter < 2 || counter > 26) { ++otherFrames_; return; }
    // a scan whose last frame was lost: close it when the scan position starts over
    if (curAny_ && counter <= lastCounter_) { scans_.push_back(cur_); scanTimes_.push_back(msuTime_); cur_ = {}; curAny_ = false; }
    lastCounter_ = counter;
    const int k = counter - 2;
    auto val = [&](int base, int pos) {
        const int hi = f[base + pos * 2 + (swapped ? 0 : 1)], lo = f[base + pos * 2 + (swapped ? 1 : 0)];
        return std::int16_t(((hi << 8) | lo) - 32768);
    };
    auto parse = [&](int ch0, int off, int n, int ns) {
        for (int ch = 0; ch < n; ++ch)
            for (int i = 0; i < 4; ++i) {
                const int pos = ch * ns + off + (ns == 2 ? i / 2 : ns == 4 ? i : 0);
                cur_[std::size_t(ch0 + ch)][std::size_t(k * 8 + i)] = val(8, pos);
                cur_[std::size_t(ch0 + ch)][std::size_t(k * 8 + 4 + i)] = val(128, pos);
            }
    };
    parse(0, 0, 5, 1);
    parse(5, 5, 2, 4);
    parse(7, 13, 23, 2);
    curAny_ = true;
    if (counter == 26) { scans_.push_back(cur_); scanTimes_.push_back(msuTime_); cur_ = {}; curAny_ = false; lastCounter_ = 0; }
}

bool MtvzaReader::hasSignal() const {
    std::size_t nonzero = 0, total = 0;
    for (const auto& s : scans_)
        for (const auto& c : s)
            for (auto v : c) { ++total; if (v != 0 && v != -32768) ++nonzero; }
    return total > 0 && nonzero > total / 10;
}

std::vector<std::array<std::array<float, 200>, 30>> MtvzaReader::cleaned(int* spikes) const {
    const int h = int(scans_.size());
    std::vector<std::array<std::array<float, 200>, 30>> v;
    v.resize(std::size_t(h));
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (int y = 0; y < h; ++y)
        for (int c = 0; c < 30; ++c)
            for (int x = 0; x < 200; ++x) {
                const auto n = scans_[std::size_t(y)][std::size_t(c)][std::size_t(x)];
                // The counts fall as the scene warms (sea reads higher than land in the window
                // channels): flipped, so that higher = warmer as in brightness temperature.
                v[std::size_t(y)][std::size_t(c)][std::size_t(x)] = (n == 0 || n == -32768) ? nan : -float(n);
            }
    // HRPT has no error correction: a flipped high bit makes a lone spike. A value is dropped when it
    // is further from the median of its eight neighbours (in scan / sample, at the channel's own
    // sample spacing) than 8 robust standard deviations of that difference over the pass, and
    // further than half that from every one of them (a sample on a real edge, such as a coast,
    // matches its neighbours on its own side and is kept).
    int dropped = 0;
    for (int c = 0; c < 30; ++c) {
        const int st = sampleStep(c);
        std::vector<float> diff(std::size_t(h) * 200, nan), nearest(std::size_t(h) * 200, nan);
        auto at = [&](int y, int x) { return (y < 0 || y >= h || x < 0 || x >= 200) ? nan : v[std::size_t(y)][std::size_t(c)][std::size_t(x)]; };
        std::vector<float> all;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < 200; x += st) {
                const float a = at(y, x);
                if (!std::isfinite(a)) continue;
                float nb[8]; int m = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dy && !dx) continue;
                        const float b = at(y + dy, x + dx * st);
                        if (std::isfinite(b)) nb[m++] = b;
                    }
                if (m < 3) continue;
                float near = 1e30f;
                for (int q = 0; q < m; ++q) near = std::min(near, std::fabs(a - nb[q]));
                std::nth_element(nb, nb + m / 2, nb + m);
                const float d = a - nb[m / 2];
                diff[std::size_t(y) * 200 + std::size_t(x)] = d;
                nearest[std::size_t(y) * 200 + std::size_t(x)] = near;
                all.push_back(std::fabs(d));
            }
        if (all.size() < 50) continue;
        std::nth_element(all.begin(), all.begin() + long(all.size() / 2), all.end());
        const float mad = std::max(1.0f, all[all.size() / 2]);
        const float lim = 8.0f * 1.4826f * mad;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < 200; x += st) {
                const float d = diff[std::size_t(y) * 200 + std::size_t(x)];
                if (!std::isfinite(d) || std::fabs(d) <= lim || nearest[std::size_t(y) * 200 + std::size_t(x)] <= lim / 2) continue;
                for (int k = 0; k < st && x + k < 200; ++k) v[std::size_t(y)][std::size_t(c)][std::size_t(x + k)] = nan;
                ++dropped;
            }
    }
    if (spikes) *spikes = dropped;
    return v;
}

QStringList MtvzaReader::save(const QString& dir) const {
    QStringList out;
    mapsLog_.clear();
    if (scans_.size() < 2 || !hasSignal()) return out;
    const int h = int(scans_.size());
    int spikes = 0;
    const auto v = cleaned(&spikes);
    for (int c = 0; c < 30; ++c) {
        std::vector<double> f(std::size_t(h) * 200);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < 200; ++x) f[std::size_t(y) * 200 + std::size_t(x)] = v[std::size_t(y)][std::size_t(c)][std::size_t(x)];
        writeField(dir, QStringLiteral("MTVZA_CH%1").arg(c + 1, 2, 10, QLatin1Char('0')), f, 200, h, 3, false, out);
    }
    out << saveMaps(dir, v);
    mapsLog_ = QStringLiteral("MTVZA: %1 bit-error samples dropped; %2 frames outside scan positions 2..26; %3")
                   .arg(spikes).arg(otherFrames_).arg(mapsLog_);
    return out;
}

}  // namespace nadirstation::polar
