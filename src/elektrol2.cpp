#include "elektrol2.h"

#include <QColor>
#include <QFont>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>

namespace nadirstation::elektro {
namespace {
#include "elektro_lut.inc"

constexpr double kBands[10] = {0.6, 0.7, 0.9, 3.8, 6.4, 8.0, 8.7, 9.7, 10.7, 11.9};
constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

struct Stop { float v; QColor c; };
QRgb palette(const std::vector<Stop>& s, float v) {
    if (!(v == v)) return qRgb(0, 0, 0);
    if (v <= s.front().v) return s.front().c.rgb();
    for (std::size_t i = 1; i < s.size(); ++i)
        if (v <= s[i].v) {
            const float t = (v - s[i - 1].v) / std::max(1e-6f, s[i].v - s[i - 1].v);
            const QColor& a = s[i - 1].c;
            const QColor& b = s[i].c;
            return qRgb(int(a.red() + t * (b.red() - a.red())), int(a.green() + t * (b.green() - a.green())),
                        int(a.blue() + t * (b.blue() - a.blue())));
        }
    return s.back().c.rgb();
}

// Title in the top-left corner and a colour bar along the bottom, scaled to the picture.
void annotate(QImage& img, const QString& title, const QString& line2, const std::vector<Stop>* bar,
              const QString& unit, const std::vector<std::pair<QString, QColor>>* classes = nullptr) {
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double s = std::max(1.0, img.width() / 1400.0);
    QFont f(QStringLiteral("Arial"));
    f.setPixelSize(int(22 * s));
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor(240, 240, 240));
    p.drawText(QPointF(16 * s, 34 * s), title);
    f.setBold(false);
    f.setPixelSize(int(17 * s));
    p.setFont(f);
    p.setPen(QColor(200, 205, 210));
    p.drawText(QPointF(16 * s, 58 * s), line2);
    if (bar) {
        const QRectF r(img.width() * 0.2, img.height() - 52 * s, img.width() * 0.6, 16 * s);
        const float lo = bar->front().v, hi = bar->back().v;
        for (int x = 0; x < int(r.width()); ++x) {
            const float v = lo + (hi - lo) * float(x) / float(r.width());
            p.setPen(QColor(palette(*bar, v)));
            p.drawLine(QPointF(r.left() + x, r.top()), QPointF(r.left() + x, r.bottom()));
        }
        p.setPen(QColor(220, 220, 220));
        p.setBrush(Qt::NoBrush);
        p.drawRect(r);
        f.setPixelSize(int(15 * s));
        p.setFont(f);
        for (int k = 0; k <= 4; ++k) {
            const float v = lo + (hi - lo) * k / 4.0f;
            const double x = r.left() + r.width() * k / 4.0;
            p.drawLine(QPointF(x, r.bottom()), QPointF(x, r.bottom() + 5 * s));
            p.drawText(QRectF(x - 60 * s, r.bottom() + 6 * s, 120 * s, 20 * s), Qt::AlignCenter,
                       QString::number(v, 'f', std::fabs(hi - lo) < 20 ? 1 : 0));
        }
        p.drawText(QRectF(r.right() + 10 * s, r.top() - 4 * s, 120 * s, 24 * s), Qt::AlignLeft | Qt::AlignVCenter, unit);
    }
    if (classes) {
        f.setPixelSize(int(16 * s));
        p.setFont(f);
        double x = 16 * s;
        const double y = img.height() - 40 * s;
        for (const auto& [name, colour] : *classes) {
            p.setPen(QColor(220, 220, 220));
            p.setBrush(colour);
            p.drawRect(QRectF(x, y, 18 * s, 18 * s));
            p.drawText(QPointF(x + 24 * s, y + 15 * s), name);
            x += (34 + 9 * name.size()) * s;
        }
    }
}
}  // namespace

int msuBandIndex(double um) {
    int best = -1;
    double d = 0.3;
    for (int i = 0; i < 10; ++i)
        if (std::fabs(kBands[i] - um) < d) { d = std::fabs(kBands[i] - um); best = i; }
    return best;
}

const std::array<std::array<float, 1024>, 10>& nominalCalibration() {
    static const auto table = [] {
        std::array<std::array<float, 1024>, 10> t{};
        for (int b = 0; b < 10; ++b)
            for (int c = 0; c < 1024; ++c) t[std::size_t(b)][std::size_t(c)] = kElektroNominalLut[b][c] / 100.0f;
        return t;
    }();
    return table;
}

bool readPrologueCalibration(const std::vector<std::uint8_t>& file, std::array<std::array<float, 1024>, 10>& lut) {
    // The table sits 612 bytes into the file (after the 80-byte header of the HRIT prologue); look a
    // little around that in case another header length moves it, and accept it only when every band
    // rises with the count and the values are physical.
    if (file.size() < 16) return false;
    const std::uint32_t hl = (std::uint32_t(file[4]) << 24) | (std::uint32_t(file[5]) << 16) | (std::uint32_t(file[6]) << 8) | file[7];
    for (std::size_t off : {std::size_t(612), std::size_t(hl) + 532, std::size_t(hl) + 612}) {
        if (off + 40960 > file.size()) continue;
        bool ok = true;
        std::array<std::array<float, 1024>, 10> t{};
        for (int b = 0; b < 10 && ok; ++b) {
            float prev = -1e9f;
            for (int c = 0; c < 1024; ++c) {
                const std::size_t at = off + std::size_t(b * 1024 + c) * 4;
                const std::uint32_t v = std::uint32_t(file[at]) | (std::uint32_t(file[at + 1]) << 8) |
                                        (std::uint32_t(file[at + 2]) << 16) | (std::uint32_t(file[at + 3]) << 24);
                const float x = float(v) / 1000.0f;
                if (x + 0.5f < prev) { ok = false; break; }
                prev = x;
                t[std::size_t(b)][std::size_t(c)] = x;
            }
            if (!ok) break;
            const float top = t[std::size_t(b)][1023];
            ok = b < 3 ? (top > 20.0f && top < 150.0f) : (top > 250.0f && top < 420.0f);
        }
        if (ok) { lut = t; return true; }
    }
    return false;
}

std::vector<L2Output> makeL2(const std::vector<L2Band>& bands, const std::array<std::array<float, 1024>, 10>& lut,
                             bool nominal, const QString& when, const QString& service) {
    std::vector<L2Output> out;
    if (bands.empty()) return out;
    const int w = bands.front().width, h = bands.front().height;
    const std::size_t n = std::size_t(w) * std::size_t(h);
    // Physical values per band (NaN where there is no data), computed on first use.
    std::array<std::vector<float>, 10> phys;
    std::array<bool, 10> have{};
    for (const auto& b : bands) {
        const int i = msuBandIndex(b.micrometres);
        if (i < 0 || b.width != w || b.height != h || !b.counts) continue;
        auto& v = phys[std::size_t(i)];
        v.resize(n);
        const auto& t = lut[std::size_t(i)];
        for (std::size_t k = 0; k < n; ++k) {
            const std::uint16_t c = b.counts[k];
            v[k] = c == 0 ? kNaN : t[std::min<std::size_t>(c, 1023)];
        }
        have[std::size_t(i)] = true;
    }
    auto band = [&](double um) -> const std::vector<float>* {
        const int i = msuBandIndex(um);
        return i >= 0 && have[std::size_t(i)] ? &phys[std::size_t(i)] : nullptr;
    };
    const QString cal = nominal ? QStringLiteral("nominal calibration (no prologue received)")
                                : QStringLiteral("calibrated with this observation's prologue");
    const QString line2 = QStringLiteral("ELEKTRO-L %1 · %2 · %3").arg(service, when, cal);

    auto colourImage = [&](const std::function<float(std::size_t)>& value, const std::vector<Stop>& pal) {
        QImage img(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h && !img.isNull(); ++y) {
            QRgb* o = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) o[x] = palette(pal, value(std::size_t(y) * w + x));
        }
        return img;
    };
    auto numeric = [&](const std::vector<float>& bt) {
        QImage img(w, h, QImage::Format_Grayscale16);
        for (int y = 0; y < h && !img.isNull(); ++y) {
            auto* o = reinterpret_cast<quint16*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const float v = bt[std::size_t(y) * w + x];
                o[x] = v == v ? quint16(std::clamp(int(v * 100.0f + 0.5f), 0, 65534)) : 0;
            }
        }
        return img;
    };
    auto rgb = [&](const std::function<std::array<float, 3>(std::size_t)>& f) {
        QImage img(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h && !img.isNull(); ++y) {
            QRgb* o = reinterpret_cast<QRgb*>(img.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const auto c = f(std::size_t(y) * w + x);
                if (!(c[0] == c[0]) || !(c[1] == c[1]) || !(c[2] == c[2])) { o[x] = qRgb(0, 0, 0); continue; }
                o[x] = qRgb(int(std::clamp(c[0], 0.0f, 1.0f) * 255.0f + 0.5f), int(std::clamp(c[1], 0.0f, 1.0f) * 255.0f + 0.5f),
                            int(std::clamp(c[2], 0.0f, 1.0f) * 255.0f + 0.5f));
            }
        }
        return img;
    };
    auto scale = [](float v, float lo, float hi, float gamma = 1.0f) {
        const float t = std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f);
        return gamma == 1.0f ? t : std::pow(t, 1.0f / gamma);
    };

    const auto* ir107 = band(10.7);
    const auto* ir119 = band(11.9);
    const auto* wv64 = band(6.4);
    const auto* wv80 = band(8.0);
    const auto* ir87 = band(8.7);
    const auto* o397 = band(9.7);
    const auto* ir38 = band(3.8);
    const auto* vis06 = band(0.6);

    // Cloud-top temperature: grey for warm surfaces and low cloud, colours for the cold tops.
    const std::vector<Stop> cttPal{{180, QColor(255, 255, 255)}, {195, QColor(180, 20, 20)}, {205, QColor(240, 90, 30)},
        {215, QColor(245, 210, 40)}, {225, QColor(40, 190, 80)}, {235, QColor(60, 140, 230)}, {245, QColor(30, 50, 140)},
        {246, QColor(225, 225, 225)}, {310, QColor(20, 20, 20)}};
    if (ir107) {
        QImage img = colourImage([&](std::size_t k) { return (*ir107)[k]; }, cttPal);
        annotate(img, QStringLiteral("Cloud-top temperature · 10.7 µm brightness temperature"), line2, &cttPal, QStringLiteral("K"));
        out.push_back({QStringLiteral("L2_CloudTopTemp_IR107_TBB_Enhanced"), img});
        out.push_back({QStringLiteral("L2_CloudTopTemp_IR107_TBB_0p01K"), numeric(*ir107)});
    }
    if (wv64) {
        const std::vector<Stop> pal{{200, QColor(20, 30, 100)}, {215, QColor(40, 90, 200)}, {228, QColor(120, 190, 240)},
            {236, QColor(235, 235, 235)}, {246, QColor(200, 160, 90)}, {262, QColor(110, 60, 20)}};
        QImage img = colourImage([&](std::size_t k) { return (*wv64)[k]; }, pal);
        annotate(img, QStringLiteral("Water vapour · 6.4 µm brightness temperature (blue moist, brown dry)"), line2, &pal, QStringLiteral("K"));
        out.push_back({QStringLiteral("L2_WaterVapour_WV64_TBB_Enhanced"), img});
        out.push_back({QStringLiteral("L2_WaterVapour_WV64_TBB_0p01K"), numeric(*wv64)});
    }
    if (ir107 && ir119) {
        const std::vector<Stop> pal{{-4, QColor(150, 40, 160)}, {-1, QColor(230, 120, 40)}, {0, QColor(70, 70, 70)},
            {1.5f, QColor(150, 150, 150)}, {3, QColor(120, 200, 240)}, {6, QColor(255, 255, 255)}};
        QImage img = colourImage([&](std::size_t k) { return (*ir107)[k] - (*ir119)[k]; }, pal);
        annotate(img, QStringLiteral("Split window · BTD 10.7 − 11.9 µm (high: thin cirrus / moist air; negative: dust)"), line2, &pal, QStringLiteral("K"));
        out.push_back({QStringLiteral("L2_SplitWindow_BTD_107-119"), img});
    }
    if (ir38 && ir107) {
        const std::vector<Stop> pal{{-8, QColor(250, 250, 250)}, {-3, QColor(120, 200, 240)}, {-1, QColor(60, 60, 60)},
            {2, QColor(110, 110, 110)}, {8, QColor(240, 170, 60)}, {20, QColor(220, 40, 30)}};
        QImage img = colourImage([&](std::size_t k) { return (*ir38)[k] - (*ir107)[k]; }, pal);
        annotate(img, QStringLiteral("Fog / low cloud · BTD 3.8 − 10.7 µm (night: below −2 K is fog or low water cloud)"), line2, &pal, QStringLiteral("K"));
        out.push_back({QStringLiteral("L2_FogLowCloud_BTD_38-107"), img});
    }
    if (wv64 && ir107) {
        const std::vector<Stop> pal{{-40, QColor(20, 20, 20)}, {-15, QColor(80, 80, 80)}, {-5, QColor(160, 160, 160)},
            {-2, QColor(240, 210, 40)}, {0, QColor(240, 90, 30)}, {3, QColor(200, 20, 20)}};
        QImage img = colourImage([&](std::size_t k) { return (*wv64)[k] - (*ir107)[k]; }, pal);
        annotate(img, QStringLiteral("Convective tops · BTD 6.4 − 10.7 µm (near or above 0 K: deep convection, overshooting tops)"), line2, &pal, QStringLiteral("K"));
        out.push_back({QStringLiteral("L2_ConvectiveTops_BTD_64-107"), img});
    }
    if (vis06) {
        const std::vector<Stop> pal{{0, QColor(0, 0, 0)}, {8, QColor(40, 40, 40)}, {25, QColor(130, 130, 130)},
            {50, QColor(210, 210, 210)}, {80, QColor(255, 255, 255)}};
        QImage img = colourImage([&](std::size_t k) { return (*vis06)[k]; }, pal);
        annotate(img, QStringLiteral("Visible albedo · 0.6 µm"), line2, &pal, QStringLiteral("%"));
        out.push_back({QStringLiteral("L2_VisibleAlbedo_VIS06"), img});
    }
    // Cloud mask and cloud level: thresholds on the window channel, helped by the visible albedo by
    // day and by the split window for thin cirrus.
    if (ir107) {
        auto cloudy = [&](std::size_t k) -> int {   // -1 no data, 0 clear, 1 probably cloudy, 2 cloudy
            const float t = (*ir107)[k];
            if (!(t == t)) return -1;
            int score = 0;
            if (t < 255.0f) score += 2;
            else if (t < 270.0f) score += 1;
            if (vis06) { const float a = (*vis06)[k]; if (a == a && a > 35.0f) score += 2; else if (a == a && a > 22.0f) score += 1; }
            if (ir119) { const float d = t - (*ir119)[k]; if (d == d && d > 3.0f && t < 285.0f) score += 1; }
            return score >= 2 ? 2 : score == 1 ? 1 : 0;
        };
        const QColor clear(30, 60, 40), maybe(120, 140, 150), cloud(245, 245, 245), none(0, 0, 0);
        QImage mask(w, h, QImage::Format_RGB32);
        const QColor high(220, 60, 50), mid(240, 200, 60), low(90, 160, 230);
        QImage level(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h && !mask.isNull() && !level.isNull(); ++y) {
            QRgb* m = reinterpret_cast<QRgb*>(mask.scanLine(y));
            QRgb* l = reinterpret_cast<QRgb*>(level.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const std::size_t k = std::size_t(y) * w + x;
                const int c = cloudy(k);
                m[x] = (c < 0 ? none : c == 0 ? clear : c == 1 ? maybe : cloud).rgb();
                const float t = (*ir107)[k];
                l[x] = (c < 0 ? none : c == 0 ? clear : t < 233.0f ? high : t < 253.0f ? mid : low).rgb();
            }
        }
        const std::vector<std::pair<QString, QColor>> mc{{QStringLiteral("clear"), clear}, {QStringLiteral("probably cloudy"), maybe}, {QStringLiteral("cloudy"), cloud}};
        annotate(mask, QStringLiteral("Cloud mask · thresholds on 10.7 µm, 0.6 µm albedo and the split window"), line2, nullptr, QString(), &mc);
        out.push_back({QStringLiteral("L2_CloudMask"), mask});
        const std::vector<std::pair<QString, QColor>> lc{{QStringLiteral("clear"), clear}, {QStringLiteral("low (top > 253 K)"), low},
                                                        {QStringLiteral("middle (233–253 K)"), mid}, {QStringLiteral("high (top < 233 K)"), high}};
        annotate(level, QStringLiteral("Cloud level · from the cloud-top temperature"), line2, nullptr, QString(), &lc);
        out.push_back({QStringLiteral("L2_CloudLevel"), level});
    }
    // EUMETSAT RGB recipes, with MSU-GS's nearest bands (6.4 for 6.2, 8.0 for 7.3).
    if (wv64 && wv80 && o397 && ir107) {
        QImage img = rgb([&](std::size_t k) -> std::array<float, 3> {
            return {scale((*wv64)[k] - (*wv80)[k], -25.0f, 0.0f), scale((*o397)[k] - (*ir107)[k], -40.0f, 5.0f),
                    1.0f - scale((*wv64)[k], 208.0f, 243.0f)};
        });
        annotate(img, QStringLiteral("RGB Airmass · 6.4−8.0, 9.7−10.7, 6.4 µm (jets, dry intrusions, polar / tropical air)"), line2, nullptr, QString());
        out.push_back({QStringLiteral("RGB_Airmass"), img});
    }
    if (ir119 && ir107 && ir87) {
        QImage img = rgb([&](std::size_t k) -> std::array<float, 3> {
            return {scale((*ir119)[k] - (*ir107)[k], -4.0f, 2.0f), scale((*ir107)[k] - (*ir87)[k], 0.0f, 15.0f, 2.5f),
                    scale((*ir107)[k], 261.0f, 289.0f)};
        });
        annotate(img, QStringLiteral("RGB Dust · 11.9−10.7, 10.7−8.7, 10.7 µm (dust magenta, thick ice cloud red)"), line2, nullptr, QString());
        out.push_back({QStringLiteral("RGB_Dust"), img});
    }
    if (ir119 && ir107 && ir38) {
        QImage img = rgb([&](std::size_t k) -> std::array<float, 3> {
            return {scale((*ir119)[k] - (*ir107)[k], -4.0f, 2.0f), scale((*ir107)[k] - (*ir38)[k], 0.0f, 10.0f),
                    scale((*ir107)[k], 243.0f, 293.0f)};
        });
        annotate(img, QStringLiteral("RGB Night microphysics · 11.9−10.7, 10.7−3.8, 10.7 µm (fog / low cloud aqua)"), line2, nullptr, QString());
        out.push_back({QStringLiteral("RGB_NightMicrophysics"), img});
    }
    return out;
}

}  // namespace nadirstation::elektro
