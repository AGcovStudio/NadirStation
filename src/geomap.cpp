#include "geomap.h"

#include "passpredict.h"

#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPainterPath>
#include <QSaveFile>
#include <QTextStream>
#include <QRegularExpression>
#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <mutex>

namespace nadirstation::polar {
namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kEarthKm = 6371.0;

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
// Earth central angle (rad) to the point seen `offNadir` rad off nadir from altitude h (km).
double centralAngle(double offNadir, double hKm) {
    const double s = (kEarthKm + hKm) / kEarthKm * std::sin(offNadir);
    if (s >= 1.0) return std::nan("");
    return std::asin(s) - offNadir;
}

QRgb rampStops(const std::vector<std::pair<double, QColor>>& stops, double t) {
    t = std::clamp(t, 0.0, 1.0);
    for (std::size_t i = 1; i < stops.size(); ++i)
        if (t <= stops[i].first) {
            const double u = (t - stops[i - 1].first) / (stops[i].first - stops[i - 1].first);
            const QColor& a = stops[i - 1].second;
            const QColor& b = stops[i].second;
            return qRgb(int(a.red() + u * (b.red() - a.red())), int(a.green() + u * (b.green() - a.green())),
                        int(a.blue() + u * (b.blue() - a.blue())));
        }
    return stops.back().second.rgb();
}

std::vector<std::vector<QPointF>> loadLines(const QString& mapsDir) {
    static std::mutex m;
    static std::map<QString, std::vector<std::vector<QPointF>>> cache;
    std::lock_guard<std::mutex> lock(m);
    auto it = cache.find(mapsDir);
    if (it != cache.end()) return it->second;
    std::vector<std::vector<QPointF>> lines;
    QFile f(QDir(mapsDir).filePath(QStringLiteral("ne_10m_admin_0_countries_chn.geojson")));
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        auto ring = [&](const QJsonArray& r) {
            std::vector<QPointF> l;
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
    }
    cache[mapsDir] = lines;
    return lines;
}
}  // namespace

QRgb rampMicrowave(double t) {
    static const std::vector<std::pair<double, QColor>> s{{0.0, QColor(20, 10, 70)}, {0.2, QColor(40, 70, 200)},
        {0.4, QColor(40, 190, 220)}, {0.57, QColor(60, 200, 90)}, {0.75, QColor(240, 220, 50)},
        {0.9, QColor(240, 110, 30)}, {1.0, QColor(190, 20, 20)}};
    return rampStops(s, t);
}
QRgb rampCloudTop(double t) {
    static const std::vector<std::pair<double, QColor>> s{{0.0, QColor(0, 0, 0)}, {0.62, QColor(225, 225, 225)},
        {0.68, QColor(80, 160, 230)}, {0.76, QColor(40, 200, 80)}, {0.84, QColor(240, 220, 40)},
        {0.91, QColor(240, 100, 30)}, {0.97, QColor(200, 20, 20)}, {1.0, QColor(255, 255, 255)}};
    return rampStops(s, t);
}
QRgb rampDiverging(double t) {
    static const std::vector<std::pair<double, QColor>> s{{0.0, QColor(30, 60, 160)}, {0.35, QColor(120, 170, 225)},
        {0.5, QColor(235, 235, 235)}, {0.65, QColor(245, 170, 110)}, {1.0, QColor(180, 30, 30)}};
    return rampStops(s, t);
}

// ---------------------------------------------------------------------------------------------
struct SwathGeo::Impl {
    pass::TleEntry tle;
    std::unique_ptr<pass::Tracker> tr;
    mutable std::mutex m;
    mutable std::map<long long, std::array<double, 4>> cache;   // ms -> lat, lon, alt, heading
    bool sub(double t, double* la, double* lo, double* alt, double* hd) const {
        const long long key = (long long)std::llround(t * 1000.0);
        {
            std::lock_guard<std::mutex> lock(m);
            auto it = cache.find(key);
            if (it != cache.end()) { *la = it->second[0]; *lo = it->second[1]; *alt = it->second[2]; *hd = it->second[3]; return true; }
        }
        double la2 = 0, lo2 = 0;
        if (!tr->subPoint(t, la, lo, alt) || !tr->subPoint(t + 1.0, &la2, &lo2, nullptr)) return false;
        *hd = bearing(*la, *lo, la2, lo2);
        std::lock_guard<std::mutex> lock(m);
        if (cache.size() > 200000) cache.clear();
        cache[key] = {*la, *lo, *alt, *hd};
        return true;
    }
};

SwathGeo::SwathGeo(const std::string& name, const std::string& line1, const std::string& line2) : d_(new Impl) {
    d_->tle.name = name;
    d_->tle.line1 = line1;
    d_->tle.line2 = line2;
    if (!line1.empty() && !line2.empty()) d_->tr = std::make_unique<pass::Tracker>(d_->tle);
}
SwathGeo::~SwathGeo() = default;
bool SwathGeo::valid() const { return d_->tr && d_->tr->valid(); }
QString SwathGeo::tleName() const { return QString::fromStdString(d_->tle.name); }

bool SwathGeo::locate(double t, double crossDeg, double alongDeg, double* lat, double* lon) const {
    if (!valid()) return false;
    double la = 0, lo = 0, alt = 0, hd = 0;
    if (!d_->sub(t, &la, &lo, &alt, &hd)) return false;
    // look direction in the local frame: x forward, y right, z down
    const double cx = std::tan(alongDeg * kDeg), cy = std::tan(crossDeg * kDeg);
    const double off = std::atan(std::hypot(cx, cy));
    const double g = centralAngle(off, alt);
    if (!std::isfinite(g)) return false;
    const double az = hd + std::atan2(cy, cx) / kDeg;
    destination(la, lo, az, g * kEarthKm, lat, lon);
    return true;
}

double SwathGeo::groundSpacingKm(double t, double crossDeg, double stepDeg) const {
    if (!valid()) return 1.0;
    double la = 0, lo = 0, alt = 0, hd = 0;
    if (!d_->sub(t, &la, &lo, &alt, &hd)) return 1.0;
    const double a = std::fabs(crossDeg) * kDeg, s = stepDeg * kDeg / 2.0;
    const double g1 = centralAngle(a + s, alt), g0 = centralAngle(std::max(0.0, a - s), alt);
    if (!std::isfinite(g1) || !std::isfinite(g0)) return 50.0;
    return (g1 - g0) * kEarthKm / 2.0;
}

// ---------------------------------------------------------------------------------------------
void GeoGrid::add(double lat, double lon, const float* values, float radiusKm) {
    if (!refSet_) { refLon_ = lon; refSet_ = true; }
    double rel = lon - refLon_;
    while (rel > 180.0) rel -= 360.0;
    while (rel < -180.0) rel += 360.0;
    lat_.push_back(float(lat));
    lon_.push_back(float(refLon_ + rel));
    rad_.push_back(radiusKm);
    for (int b = 0; b < bands_; ++b) val_.push_back(values[b]);
}

bool GeoGrid::build(double resDeg) {
    if (lat_.empty()) return false;
    float laMin = 90, laMax = -90, loMin = 1e9f, loMax = -1e9f;
    for (std::size_t i = 0; i < lat_.size(); ++i) {
        laMin = std::min(laMin, lat_[i]); laMax = std::max(laMax, lat_[i]);
        loMin = std::min(loMin, lon_[i]); loMax = std::max(loMax, lon_[i]);
    }
    res_ = resDeg;
    const double lat0 = std::floor(laMin), lat1 = std::ceil(laMax), lon0 = std::floor(loMin), lon1 = std::ceil(loMax);
    W_ = int(std::lround((lon1 - lon0) / res_));
    H_ = int(std::lround((lat1 - lat0) / res_));
    if (W_ <= 0 || H_ <= 0 || double(W_) * H_ > 6.0e7) return false;
    lon0_ = lon0;
    lat1_ = lat1;
    const std::size_t n = std::size_t(W_) * std::size_t(H_);
    cell_.assign(n * std::size_t(bands_), std::nanf(""));
    std::vector<float> best(n, 1e30f);
    // nearest sample within its footprint
    for (std::size_t i = 0; i < lat_.size(); ++i) {
        const double cosLat = std::max(0.05, std::cos(lat_[i] * kDeg));
        const double rLat = rad_[i] / 111.2, rLon = rLat / cosLat;
        const int x0 = std::max(0, int(std::floor((lon_[i] - rLon - lon0_) / res_)));
        const int x1 = std::min(W_ - 1, int(std::floor((lon_[i] + rLon - lon0_) / res_)));
        const int y0 = std::max(0, int(std::floor((lat1_ - lat_[i] - rLat) / res_)));
        const int y1 = std::min(H_ - 1, int(std::floor((lat1_ - lat_[i] + rLat) / res_)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double cLat = lat1_ - (y + 0.5) * res_, cLon = lon0_ + (x + 0.5) * res_;
                const double dy = (cLat - lat_[i]) * 111.2, dx = (cLon - lon_[i]) * 111.2 * cosLat;
                const float d2 = float(dx * dx + dy * dy);
                if (d2 > rad_[i] * rad_[i] && !(x == x0 && y == y0 && x0 == x1 && y0 == y1)) continue;
                const std::size_t c = std::size_t(y) * std::size_t(W_) + std::size_t(x);
                if (d2 < best[c]) {
                    best[c] = d2;
                    for (int b = 0; b < bands_; ++b) cell_[c * std::size_t(bands_) + std::size_t(b)] = val_[i * std::size_t(bands_) + std::size_t(b)];
                }
            }
    }
    // only the empty cells inside the swath: median of the measured neighbours
    filled_ = 0;
    for (int pass = 0; pass < 2; ++pass) {
        std::vector<float> next = cell_;
        for (int y = 1; y + 1 < H_; ++y)
            for (int x = 1; x + 1 < W_; ++x) {
                const std::size_t c = std::size_t(y) * std::size_t(W_) + std::size_t(x);
                if (std::isfinite(cell_[c * std::size_t(bands_)])) continue;
                auto has = [&](int dx, int dy) { return std::isfinite(cell_[(std::size_t(y + dy) * std::size_t(W_) + std::size_t(x + dx)) * std::size_t(bands_)]); };
                const bool between = (has(-1, 0) && has(1, 0)) || (has(0, -1) && has(0, 1)) || (has(-1, -1) && has(1, 1)) || (has(1, -1) && has(-1, 1));
                if (!between) continue;
                int count = 0;
                for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) if ((dx || dy) && has(dx, dy)) ++count;
                if (count < 4) continue;
                for (int b = 0; b < bands_; ++b) {
                    float nb[8];
                    int k = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if ((dx || dy) && has(dx, dy)) nb[k++] = cell_[(std::size_t(y + dy) * std::size_t(W_) + std::size_t(x + dx)) * std::size_t(bands_) + std::size_t(b)];
                    std::nth_element(nb, nb + k / 2, nb + k);
                    next[c * std::size_t(bands_) + std::size_t(b)] = nb[k / 2];
                }
                ++filled_;
            }
        cell_.swap(next);
    }
    return true;
}

QImage GeoGrid::render(const std::function<QRgb(const GeoGrid&, int, int)>& colour, int zoom, const QString& mapsDir,
                       const Legend& legend) const {
    const int Z = std::max(1, zoom);
    const int legendH = 92;
    QImage img(W_ * Z, H_ * Z + legendH, QImage::Format_RGB32);
    img.fill(qRgb(14, 15, 17));
    for (int y = 0; y < H_; ++y)
        for (int x = 0; x < W_; ++x) {
            const QRgb c = colour(*this, x, y);
            if (!c) continue;
            for (int yy = 0; yy < Z; ++yy) {
                auto* line = reinterpret_cast<QRgb*>(img.scanLine(y * Z + yy));
                for (int xx = 0; xx < Z; ++xx) line[x * Z + xx] = c;
            }
        }
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    const double lat0 = lat1_ - H_ * res_, lon1 = lon0_ + W_ * res_;
    auto toPx = [&](double lon, double lat) { return QPointF((lon - lon0_) / res_ * Z, (lat1_ - lat) / res_ * Z); };
    QFont f = p.font();
    f.setPixelSize(12);
    p.setFont(f);
    const double step = (lat1_ - lat0) > 30 ? 10.0 : 5.0;
    p.setPen(QPen(QColor(255, 255, 255, 50), 1));
    for (double la = std::ceil(lat0 / step) * step; la <= lat1_; la += step) {
        p.drawLine(toPx(lon0_, la), toPx(lon1, la));
        p.drawText(toPx(lon0_, la) + QPointF(3, -3), QStringLiteral("%1°%2").arg(std::fabs(la)).arg(la >= 0 ? 'N' : 'S'));
    }
    for (double lo = std::ceil(lon0_ / step) * step; lo <= lon1; lo += step) {
        p.drawLine(toPx(lo, lat0), toPx(lo, lat1_));
        double show = lo;
        while (show > 180) show -= 360;
        while (show < -180) show += 360;
        p.drawText(toPx(lo, lat1_) + QPointF(3, 14), QStringLiteral("%1°%2").arg(std::fabs(show)).arg(show >= 0 ? 'E' : 'W'));
    }
    p.setPen(QPen(QColor(255, 255, 255, 190), 1.0));
    for (const auto& l : loadLines(mapsDir)) {
        QPainterPath path;
        bool started = false;
        for (const QPointF& q : l) {
            double rel = q.x() - refLon_;
            while (rel > 180.0) rel -= 360.0;
            while (rel < -180.0) rel += 360.0;
            const QPointF px = toPx(refLon_ + rel, q.y());
            if (!started || std::fabs(px.x() - path.currentPosition().x()) > W_ * Z / 2) { path.moveTo(px); started = true; }
            else path.lineTo(px);
        }
        p.drawPath(path);
    }
    const int ly = H_ * Z + 10;
    p.setPen(QColor(220, 222, 226));
    if (!legend.classes.empty()) {
        int x = 20;
        for (const auto& [c, label] : legend.classes) {
            p.fillRect(x, ly, 14, 14, QColor::fromRgb(c));
            p.drawText(x + 20, ly + 12, label);
            x += 34 + p.fontMetrics().horizontalAdvance(label);
        }
    } else if (legend.ramp) {
        const int lw = std::max(120, std::min(W_ * Z - 40, 420));
        for (int i = 0; i < lw; ++i) { p.setPen(QColor::fromRgb(legend.ramp(double(i) / (lw - 1)))); p.drawLine(20 + i, ly, 20 + i, ly + 12); }
        p.setPen(QColor(220, 222, 226));
        p.drawText(20, ly + 28, legend.lowLabel);
        p.drawText(20 + lw / 2 - p.fontMetrics().horizontalAdvance(legend.midLabel) / 2, ly + 28, legend.midLabel);
        p.drawText(20 + lw - p.fontMetrics().horizontalAdvance(legend.highLabel), ly + 28, legend.highLabel);
    }
    p.drawText(20, ly + 50, legend.title);
    p.drawText(20, ly + 68, legend.detail);
    p.end();
    return img;
}

bool GeoGrid::saveRaw16(int band, double scale, double offset, const QString& pngPath) const {
    QImage raw(W_, H_, QImage::Format_Grayscale16);
    raw.fill(0);
    for (int y = 0; y < H_; ++y) {
        auto* r = reinterpret_cast<quint16*>(raw.scanLine(y));
        for (int x = 0; x < W_; ++x) {
            const float v = at(x, y, band);
            if (std::isfinite(v)) r[x] = quint16(std::clamp(std::lround(v * scale + offset), 1L, 65535L));
        }
    }
    if (!raw.save(pngPath, "PNG")) return false;
    QString pgw = pngPath;
    pgw.replace(QRegularExpression(QStringLiteral("\\.png$")), QStringLiteral(".pgw"));
    QSaveFile wf(pgw);
    if (wf.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream ts(&wf);
        ts << res_ << "\n0\n0\n" << -res_ << "\n" << (lon0_ + res_ / 2) << "\n" << (lat1_ - res_ / 2) << "\n";
        ts.flush();
        wf.commit();
    }
    return true;
}

}  // namespace nadirstation::polar
