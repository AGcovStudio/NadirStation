#include "ascatl2.h"

#include "passpredict.h"

#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <map>

namespace nadirstation::polar {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kEarthKm = 6371.0;

// Off-nadir angle (degrees, signed: positive to the right of the track) of a range sample, and the
// beam's turn about the nadir (degrees, clockwise from the track), per beam (APID 208..213).
struct BeamGeometry {
    std::vector<std::pair<int, double>> points;   // sample -> roll
    double yaw = 0.0;
};
const std::array<BeamGeometry, 6>& beams() {
    static const std::array<BeamGeometry, 6> b{{
        {{{0, -10}, {30, -26}, {80, -35}, {130, -43.0}, {194, -50.6}, {255, -55.5}}, 135.0},
        {{{0, 10}, {30, 18}, {80, 30}, {130, 37.5}, {194, 44.1}, {255, 48.5}}, 0.0},
        {{{0, -55.5}, {61, -51.6}, {125, -44.2}, {175, -37.1}, {225, -28.4}, {255, -19}}, -135.0},
        {{{0, -10}, {30, -26}, {80, -35}, {130, -43.0}, {194, -50.6}, {255, -55.5}}, 45.0},
        {{{0, -10}, {30, -18}, {80, -30}, {130, -37.5}, {194, -44.1}, {255, -48.5}}, 0.0},
        {{{0, -55.5}, {61, -51.6}, {125, -44.2}, {175, -37.1}, {225, -28.4}, {255, -19}}, -45.0},
    }};
    return b;
}
double rollOf(const BeamGeometry& g, int s) {
    for (std::size_t i = 1; i < g.points.size(); ++i)
        if (s <= g.points[i].first) {
            const auto& a = g.points[i - 1];
            const auto& b = g.points[i];
            return a.second + (b.second - a.second) * double(s - a.first) / double(b.first - a.first);
        }
    return g.points.back().second;
}

// Point at `distKm` from (lat, lon) on azimuth `azDeg` (great circle).
void destination(double latDeg, double lonDeg, double azDeg, double distKm, double* outLat, double* outLon) {
    const double d = distKm / kEarthKm, la = latDeg * kDeg, lo = lonDeg * kDeg, az = azDeg * kDeg;
    const double la2 = std::asin(std::sin(la) * std::cos(d) + std::cos(la) * std::sin(d) * std::cos(az));
    const double lo2 = lo + std::atan2(std::sin(az) * std::sin(d) * std::cos(la), std::cos(d) - std::sin(la) * std::sin(la2));
    *outLat = la2 / kDeg;
    double l = lo2 / kDeg;
    while (l > 180.0) l -= 360.0;
    while (l < -180.0) l += 360.0;
    *outLon = l;
}
double bearing(double lat1, double lon1, double lat2, double lon2) {
    const double a = lat1 * kDeg, b = lat2 * kDeg, dl = (lon2 - lon1) * kDeg;
    return std::atan2(std::sin(dl) * std::cos(b), std::cos(a) * std::sin(b) - std::sin(a) * std::cos(b) * std::cos(dl)) / kDeg;
}

std::vector<std::vector<QPointF>> loadLines(const QString& mapsDir) {
    std::vector<std::vector<QPointF>> lines;
    QFile f(QDir(mapsDir).filePath(QStringLiteral("ne_10m_admin_0_countries_chn.geojson")));
    if (!f.open(QIODevice::ReadOnly)) return lines;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    auto ring = [&](const QJsonArray& r) {
        std::vector<QPointF> l;
        l.reserve(std::size_t(r.size()));
        for (const auto& v : r) { const QJsonArray p = v.toArray(); if (p.size() >= 2) l.emplace_back(p[0].toDouble(), p[1].toDouble()); }
        if (l.size() >= 2) lines.push_back(std::move(l));
    };
    for (const auto& feat : doc.object().value(QStringLiteral("features")).toArray()) {
        const QJsonObject g = feat.toObject().value(QStringLiteral("geometry")).toObject();
        const QString type = g.value(QStringLiteral("type")).toString();
        const QJsonArray c = g.value(QStringLiteral("coordinates")).toArray();
        if (type == QLatin1String("Polygon")) for (const auto& r : c) ring(r.toArray());
        else if (type == QLatin1String("MultiPolygon")) for (const auto& poly : c) for (const auto& r : poly.toArray()) ring(r.toArray());
    }
    return lines;
}

QRgb colourOf(double t) {   // 0..1: smooth / weak echo (dark blue) .. strong (yellow, white)
    static const std::pair<double, QColor> stops[] = {{0.0, QColor(12, 18, 60)}, {0.25, QColor(30, 80, 170)},
        {0.45, QColor(40, 165, 190)}, {0.6, QColor(110, 200, 120)}, {0.75, QColor(230, 215, 70)},
        {0.9, QColor(235, 120, 40)}, {1.0, QColor(250, 245, 235)}};
    t = std::clamp(t, 0.0, 1.0);
    for (std::size_t i = 1; i < std::size(stops); ++i)
        if (t <= stops[i].first) {
            const double u = (t - stops[i - 1].first) / (stops[i].first - stops[i - 1].first);
            const QColor& a = stops[i - 1].second;
            const QColor& b = stops[i].second;
            return qRgb(int(a.red() + u * (b.red() - a.red())), int(a.green() + u * (b.green() - a.green())),
                        int(a.blue() + u * (b.blue() - a.blue())));
        }
    return stops[std::size(stops) - 1].second.rgb();
}

}  // namespace

QStringList makeAscatL2(const AscatL2Input& in, const QString& dir, QString* log) {
    QStringList out;
    auto say = [&](const QString& s) { if (log) *log = s; };
    if (in.pulses.size() < 30) { say(QStringLiteral("ASCAT L2: too few pulses (%1)").arg(in.pulses.size())); return out; }
    if (in.tleLine1.empty() || in.tleLine2.empty()) {
        say(QStringLiteral("ASCAT L2: no TLE for this MetOp in the TLE cache (download TLEs on the Passes page)"));
        return out;
    }
    pass::TleEntry te;
    te.name = in.tleName;
    te.line1 = in.tleLine1;
    te.line2 = in.tleLine2;
    pass::Tracker tr(te);
    if (!tr.valid()) { say(QStringLiteral("ASCAT L2: TLE does not parse: %1").arg(QString::fromStdString(tr.error()))); return out; }

    // Relative dB: each beam and range sample against its median over the pass.
    std::array<std::array<float, 256>, 6> median{};
    for (int b = 0; b < 6; ++b)
        for (int s = 0; s < 256; ++s) {
            std::vector<float> v;
            for (const auto& p : in.pulses)
                if (p.beam == b && p.power[std::size_t(s)] > 0.0f) v.push_back(10.0f * std::log10(p.power[std::size_t(s)]));
            if (v.empty()) { median[std::size_t(b)][std::size_t(s)] = std::nanf(""); continue; }
            std::nth_element(v.begin(), v.begin() + long(v.size() / 2), v.end());
            median[std::size_t(b)][std::size_t(s)] = v[v.size() / 2];
        }

    // Locate every sample.
    struct Pt { float lat, lon, db; int beam; };
    std::vector<Pt> pts;
    pts.reserve(in.pulses.size() * 256);
    double t0 = 1e300, t1 = -1e300, refLon = 0.0;
    bool refSet = false;
    int systemTimed = 0;
    for (const auto& p : in.pulses) {
        double la = 0, lo = 0, alt = 0, la2 = 0, lo2 = 0;
        if (!tr.subPoint(p.time, &la, &lo, &alt) || !tr.subPoint(p.time + 1.0, &la2, &lo2, nullptr)) continue;
        if (!refSet) { refLon = lo; refSet = true; }
        t0 = std::min(t0, p.time); t1 = std::max(t1, p.time);
        if (p.systemTime) ++systemTimed;
        const double heading = bearing(la, lo, la2, lo2);
        const BeamGeometry& g = beams()[std::size_t(p.beam)];
        for (int s = 0; s < 256; ++s) {
            const float pw = p.power[std::size_t(s)];
            const float m = median[std::size_t(p.beam)][std::size_t(s)];
            if (!(pw > 0.0f) || !std::isfinite(m)) continue;
            const double roll = rollOf(g, s);
            const double th = std::fabs(roll) * kDeg;
            const double sinArg = (kEarthKm + alt) / kEarthKm * std::sin(th);
            if (sinArg >= 1.0) continue;
            const double gamma = std::asin(sinArg) - th;
            // horizontal direction: cross-track to the right for positive roll, turned by the beam yaw
            // The settings' roll and yaw are mirrored with respect to this frame (x along track,
            // y to the right, azimuth clockwise): checked on a real pass, where only the mirrored
            // geometry puts the land / sea contrast of the echo on the coastline.
            const double along = 0.0, cross = roll >= 0 ? -1.0 : 1.0, yaw = -g.yaw * kDeg;
            const double a2 = along * std::cos(yaw) - cross * std::sin(yaw);
            const double c2 = along * std::sin(yaw) + cross * std::cos(yaw);
            const double az = heading + std::atan2(c2, a2) / kDeg;
            double plat = 0, plon = 0;
            destination(la, lo, az, gamma * kEarthKm, &plat, &plon);
            double rel = plon - refLon;           // keep the map continuous across 180
            while (rel > 180.0) rel -= 360.0;
            while (rel < -180.0) rel += 360.0;
            pts.push_back({float(plat), float(refLon + rel), 10.0f * std::log10(pw) - m, p.beam});
        }
    }
    if (pts.size() < 1000) { say(QStringLiteral("ASCAT L2: could not locate the pulses (TLE too old?)")); return out; }

    // Grid, 0.05 degree; mean of the samples in each cell, no interpolation.
    float laMin = 90, laMax = -90, loMin = 1e9f, loMax = -1e9f;
    for (const auto& p : pts) { laMin = std::min(laMin, p.lat); laMax = std::max(laMax, p.lat); loMin = std::min(loMin, p.lon); loMax = std::max(loMax, p.lon); }
    const double res = 0.05;
    const double lat0 = std::floor(laMin / 1.0) * 1.0, lat1 = std::ceil(laMax / 1.0) * 1.0;
    const double lon0 = std::floor(loMin / 1.0) * 1.0, lon1 = std::ceil(loMax / 1.0) * 1.0;
    const int W = int(std::lround((lon1 - lon0) / res)), H = int(std::lround((lat1 - lat0) / res));
    if (W <= 0 || H <= 0 || W > 8000 || H > 8000) { say(QStringLiteral("ASCAT L2: map extent out of range")); return out; }
    std::vector<float> sum(std::size_t(W) * std::size_t(H), 0.0f), sumMid(sum.size(), 0.0f);
    std::vector<int> cnt(sum.size(), 0), cntMid(sum.size(), 0);
    for (const auto& p : pts) {
        const int x = int((p.lon - lon0) / res), y = int((lat1 - p.lat) / res);
        if (x < 0 || y < 0 || x >= W || y >= H) continue;
        const std::size_t i = std::size_t(y) * std::size_t(W) + std::size_t(x);
        sum[i] += p.db; ++cnt[i];
        if (p.beam == 1 || p.beam == 4) { sumMid[i] += p.db; ++cntMid[i]; }
    }

    const auto lines = loadLines(in.mapsDir);
    const int Z = 2;   // drawn at 2 x 2 pixels per cell, nearest (no smoothing)
    const QString when = QDateTime::fromSecsSinceEpoch(qint64(t0), QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))
                       + QStringLiteral(" - ") + QDateTime::fromSecsSinceEpoch(qint64(t1), QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss 'UTC'"));
    const QString timeNote = systemTimed == 0 ? QStringLiteral("times from the packets")
                           : systemTimed == int(in.pulses.size()) ? QStringLiteral("times from the system clock (packets carried none)")
                                                                   : QStringLiteral("times from the packets, %1 pulses from the system clock").arg(systemTimed);
    int filledCells = 0;
    auto render = [&](const std::vector<float>& s, const std::vector<int>& c, const QString& name, const QString& title) {
        const double range = 8.0;   // dB either side of the median
        const int legendH = 84;
        QImage img(W * Z, H * Z + legendH, QImage::Format_RGB32);
        img.fill(qRgb(14, 15, 17));
        QImage raw(W, H, QImage::Format_Grayscale16);
        raw.fill(0);
        // Cell values; then only the empty cells inside the swath (the black dots) are filled with the
        // median of their measured 3 x 3 neighbours. A cell counts as inside when it has data on two
        // opposite sides and at least four neighbours with data, so the swath edges do not grow.
        // Measured cells are never changed.
        std::vector<float> val(std::size_t(W) * std::size_t(H), std::nanf(""));
        for (std::size_t i = 0; i < val.size(); ++i) if (c[i]) val[i] = s[i] / float(c[i]);
        int filled = 0;
        for (int pass = 0; pass < 2; ++pass) {
            std::vector<float> next = val;
            for (int y = 1; y + 1 < H; ++y)
                for (int x = 1; x + 1 < W; ++x) {
                    const std::size_t i = std::size_t(y) * std::size_t(W) + std::size_t(x);
                    if (std::isfinite(val[i])) continue;
                    auto at = [&](int dx, int dy) { return val[std::size_t(y + dy) * std::size_t(W) + std::size_t(x + dx)]; };
                    const bool between = (std::isfinite(at(-1, 0)) && std::isfinite(at(1, 0))) ||
                                         (std::isfinite(at(0, -1)) && std::isfinite(at(0, 1))) ||
                                         (std::isfinite(at(-1, -1)) && std::isfinite(at(1, 1))) ||
                                         (std::isfinite(at(1, -1)) && std::isfinite(at(-1, 1)));
                    if (!between) continue;
                    float nb[8];
                    int n = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if ((dx || dy) && std::isfinite(at(dx, dy))) nb[n++] = at(dx, dy);
                    if (n < 4) continue;
                    std::nth_element(nb, nb + n / 2, nb + n);
                    next[i] = (n % 2) ? nb[n / 2] : 0.5f * (nb[n / 2] + *std::max_element(nb, nb + n / 2));
                    ++filled;
                }
            val.swap(next);
        }
        filledCells += filled;
        for (int y = 0; y < H; ++y) {
            auto* r16 = reinterpret_cast<quint16*>(raw.scanLine(y));
            for (int x = 0; x < W; ++x) {
                const std::size_t i = std::size_t(y) * std::size_t(W) + std::size_t(x);
                if (!std::isfinite(val[i])) continue;
                const double db = val[i];
                r16[x] = quint16(std::clamp(std::lround(db * 100.0) + 32768L, 1L, 65535L));
                const QRgb col = colourOf((db + range) / (2 * range));
                for (int yy = 0; yy < Z; ++yy) {
                    auto* line = reinterpret_cast<QRgb*>(img.scanLine(y * Z + yy));
                    for (int xx = 0; xx < Z; ++xx) line[x * Z + xx] = col;
                }
            }
        }
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        auto toPx = [&](double lon, double lat) { return QPointF((lon - lon0) / res * Z, (lat1 - lat) / res * Z); };
        // graticule
        p.setPen(QPen(QColor(255, 255, 255, 50), 1));
        QFont f = p.font();
        f.setPixelSize(12);
        p.setFont(f);
        for (double la = std::ceil(lat0 / 5) * 5; la <= lat1; la += 5) {
            p.drawLine(toPx(lon0, la), toPx(lon1, la));
            p.drawText(toPx(lon0, la) + QPointF(3, -3), QStringLiteral("%1°%2").arg(std::fabs(la)).arg(la >= 0 ? 'N' : 'S'));
        }
        for (double lo = std::ceil(lon0 / 5) * 5; lo <= lon1; lo += 5) {
            p.drawLine(toPx(lo, lat0), toPx(lo, lat1));
            double show = lo;
            while (show > 180) show -= 360;
            while (show < -180) show += 360;
            p.drawText(toPx(lo, lat1) + QPointF(3, 14), QStringLiteral("%1°%2").arg(std::fabs(show)).arg(show >= 0 ? 'E' : 'W'));
        }
        // coast and borders
        p.setPen(QPen(QColor(255, 255, 255, 190), 1.0));
        for (const auto& l : lines) {
            QPainterPath path;
            bool started = false;
            for (const QPointF& q : l) {
                double lo = q.x();
                double rel = lo - refLon;
                while (rel > 180.0) rel -= 360.0;
                while (rel < -180.0) rel += 360.0;
                lo = refLon + rel;
                const QPointF px = toPx(lo, q.y());
                if (!started || std::fabs(px.x() - path.currentPosition().x()) > W * Z / 2) { path.moveTo(px); started = true; }
                else path.lineTo(px);
            }
            p.drawPath(path);
        }
        // legend
        const int ly = H * Z + 10, lw = std::min(W * Z - 40, 420);
        for (int i = 0; i < lw; ++i) { p.setPen(QColor::fromRgb(colourOf(double(i) / (lw - 1)))); p.drawLine(20 + i, ly, 20 + i, ly + 12); }
        p.setPen(QColor(220, 222, 226));
        p.drawText(20, ly + 28, QStringLiteral("-%1 dB").arg(range));
        p.drawText(20 + lw / 2 - 8, ly + 28, QStringLiteral("0"));
        p.drawText(20 + lw - 40, ly + 28, QStringLiteral("+%1 dB").arg(range));
        p.drawText(20, ly + 46, QStringLiteral("MetOp ASCAT %1 - relative backscatter (echo vs. its pass median per beam and range), uncalibrated").arg(title));
        p.drawText(20, ly + 62, QStringLiteral("%1 · %2 · grid %3° (gaps: neighbour median) · TLE %4").arg(when, timeNote).arg(res).arg(QString::fromStdString(in.tleName)));
        p.end();
        const QString a = QDir(dir).filePath(QStringLiteral("ASCAT_L2_%1_Enhanced.png").arg(name));
        const QString b = QDir(dir).filePath(QStringLiteral("ASCAT_L2_%1_dBx100.png").arg(name));
        if (img.save(a, "PNG")) out << a;
        if (raw.save(b, "PNG")) {
            out << b;
            // world file: the 16-bit grid's pixel size and the centre of its top-left pixel
            QSaveFile wf(QDir(dir).filePath(QStringLiteral("ASCAT_L2_%1_dBx100.pgw").arg(name)));
            if (wf.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream ts(&wf);
                ts << res << "\n0\n0\n" << -res << "\n" << (lon0 + res / 2) << "\n" << (lat1 - res / 2) << "\n";
                ts.flush();
                wf.commit();
            }
        }
    };
    render(sum, cnt, QStringLiteral("AllBeams"), QStringLiteral("all six beams"));
    render(sumMid, cntMid, QStringLiteral("MidBeams"), QStringLiteral("mid beams"));
    say(QStringLiteral("ASCAT L2: %1 pulses located, %2 x %3 grid at %4 deg, %5; %6 empty cells inside the swath filled with the median of their neighbours")
            .arg(in.pulses.size()).arg(W).arg(H).arg(res).arg(timeNote).arg(filledCells));
    return out;
}

}  // namespace nadirstation::polar
