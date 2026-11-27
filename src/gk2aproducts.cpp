#include <QImageWriter>
#include "gk2aproducts.h"
#include <QColor>
#include <QFont>
#include <QPainter>
#include <QGuiApplication>

#include "runtime_logger.h"

#include <QCoreApplication>
#include <QDir>
#include <QImageReader>
#include <QFileInfo>
#include <QFile>
#include <QtEndian>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QFile>
#include <algorithm>
#include <iterator>
#include <vector>
#include <cmath>

namespace {
// Lossless PNG at a light zlib level (Qt quality 80): about three times faster than the default
// level on full-disk images, for files about 14 % larger.
bool savePngFast(const QImage& image, const QString& path) {
    QImageWriter writer(path, "png");
    writer.setQuality(80);
    return writer.write(image);
}
}  // namespace

namespace nadirstation::gk2a {
namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusKm = 6378.137;
constexpr double kSatelliteHeightKm = 35786.0;
constexpr double kSatelliteRadiusKm = kEarthRadiusKm + kSatelliteHeightKm;
constexpr double kFullDiskLines = 2200.0;

bool writeGeographicWorldFiles(const QString& imagePath, int width, int height,
                               double west, double east, double south, double north) {
    if (width <= 0 || height <= 0 || !(east > west) || !(north > south)) return false;
    const double dx = (east - west) / width;
    const double dy = (north - south) / height;
    const auto number = [](double value) { return QString::number(value, 'g', 16); };
    QFile world(imagePath + QStringLiteral(".pgw"));
    QFile projection(imagePath + QStringLiteral(".prj"));
    if (!world.open(QIODevice::WriteOnly | QIODevice::Text) ||
        !projection.open(QIODevice::WriteOnly | QIODevice::Text)) return false;
    const QByteArray contents = (number(dx) + QStringLiteral("\n0\n0\n") + number(-dy) +
        QLatin1Char('\n') + number(west + dx * 0.5) + QLatin1Char('\n') +
        number(north - dy * 0.5) + QLatin1Char('\n')).toUtf8();
    const QByteArray wkt = "GEOGCS[\"WGS 84\",DATUM[\"WGS_1984\",SPHEROID[\"WGS 84\",6378137,298.257223563]],PRIMEM[\"Greenwich\",0],UNIT[\"degree\",0.0174532925199433],AUTHORITY[\"EPSG\",\"4326\"]]";
    const bool ok = world.write(contents) == contents.size() &&
                    projection.write(wkt) == wkt.size();
    world.close();
    projection.close();
    return ok && world.error() == QFileDevice::NoError &&
           projection.error() == QFileDevice::NoError;
}

// The angular radius of the Earth as seen from the satellite. Geometry, not a parameter.
double limbAngleDegrees() {
    return std::asin(kEarthRadiusKm / kSatelliteRadiusKm) * 180.0 / kPi;
}

// Where a latitude and longitude lands on the disk, or false when it is over the limb.
bool geosPixel(double latDeg, double lonDeg, const DiskGeometry& g, double* col, double* row) {
    if (g.navigation.valid) {
        const double lat = latDeg * kPi / 180.0;
        const double dlon = (lonDeg - g.navigation.subLonDeg) * kPi / 180.0;
        const double geocentricLat = std::atan(0.993243 * std::tan(lat));
        const double cosLat = std::cos(geocentricLat);
        const double rl = 6356.7523 / std::sqrt(1.0 - 0.00669438444 * cosLat * cosLat);
        const double r1 = 42164.0 - rl * cosLat * std::cos(dlon);
        const double r2 = -rl * cosLat * std::sin(dlon);
        const double r3 = rl * std::sin(geocentricLat);
        const double rn = std::sqrt(r1 * r1 + r2 * r2 + r3 * r3);
        const double xDeg = std::atan2(-r2, r1) * 180.0 / kPi;
        const double yDeg = std::asin(std::clamp(-r3 / rn, -1.0, 1.0)) * 180.0 / kPi;
        // Magnitudes only: the disks are assembled north up and east right whatever sign the
        // record gives its factors. GK-2A's LRIT records carry a negative LFAC, and using it signed
        // put every navigated product upside down in latitude (Australia north of the equator on the
        // MCIR disk, the 2026-10-18 03:40 typhoon missing from the regional map). The limb-measured
        // path below never had this, which is why it went unnoticed until navigation records were used.
        *col = g.navigation.coff + xDeg * std::fabs(double(g.navigation.cfac)) / 65536.0;
        *row = g.navigation.loff + yDeg * std::fabs(double(g.navigation.lfac)) / 65536.0;
        return std::isfinite(*col) && std::isfinite(*row);
    }
    const double lat = latDeg * kPi / 180.0;
    const double dlon = (lonDeg - g.subLonDeg) * kPi / 180.0;
    const double x = std::cos(lat) * std::cos(dlon);
    const double y = std::cos(lat) * std::sin(dlon);
    const double z = std::sin(lat);
    const double h = kSatelliteRadiusKm / kEarthRadiusKm;
    const double vx = h - x;
    if (vx <= 0.0) return false;
    const double range = std::sqrt(vx * vx + y * y + z * z);
    *col = g.centre + std::atan2(y, vx) / g.radPerPixel;
    *row = g.centre - std::asin(z / range) / g.radPerPixel;
    return true;
}

// A disk pixel to latitude and longitude.
bool diskToLatLon(double col, double row, const DiskGeometry& g, double* latDeg, double* lonDeg) {
    if (g.navigation.valid) {
        // Magnitudes, as in geosPixel(): north up, east right.
        const double x = (col - g.navigation.coff) * 65536.0 / std::fabs(double(g.navigation.cfac));
        const double y = (row - g.navigation.loff) * 65536.0 / std::fabs(double(g.navigation.lfac));
        const double xRad = x * kPi / 180.0;
        const double yRad = y * kPi / 180.0;
        const double cosX = std::cos(xRad), sinX = std::sin(xRad);
        const double cosY = std::cos(yRad), sinY = std::sin(yRad);
        constexpr double kEarthEccentricityFactor = 1.006739501;
        const double denominator = cosY * cosY + kEarthEccentricityFactor * sinY * sinY;
        const double term = 42164.0 * cosX * cosY;
        const double discriminant = term * term - denominator * (42164.0 * 42164.0 - 6378.137 * 6378.137);
        if (discriminant < 0.0) return false;
        const double sn = (term - std::sqrt(discriminant)) / denominator;
        const double s1 = 42164.0 - sn * cosX * cosY;
        const double s2 = sn * sinX * cosY;
        const double s3 = -sn * sinY;
        const double sxy = std::sqrt(s1 * s1 + s2 * s2);
        *lonDeg = g.navigation.subLonDeg + std::atan2(s2, s1) * 180.0 / kPi;
        *latDeg = std::atan(kEarthEccentricityFactor * s3 / sxy) * 180.0 / kPi;
        return std::isfinite(*latDeg) && std::isfinite(*lonDeg);
    }
    // The scanning angles at this pixel, positive east and positive north.
    const double xn = (col - g.centre) * g.radPerPixel;
    const double yn = (g.centre - row) * g.radPerPixel;
    const double h = kSatelliteRadiusKm / kEarthRadiusKm;
    const double cosXn = std::cos(xn), sinXn = std::sin(xn);
    const double cosYn = std::cos(yn), sinYn = std::sin(yn);

    // Where the line of sight meets the Earth. The satellite is at (h, 0, 0) in Earth radii with +Y
    // east and +Z north, so the direction of the line is
    //
    //     n = (-cos y * cos x,  cos y * sin x,  sin y)
    //
    // and the point is S + t*n where t solves |S + t*n| = 1:
    //
    //     t = h*cos y*cos x - sqrt(h^2*cos^2 y*cos^2 x - h^2 + 1)
    //
    // The distance was always computed correctly here. What was wrong was using it as the position:
    // P = t*n instead of P = S + t*n, which drops the whole Earth radius and makes
    // lat = atan2(t*sin y, t*cos y) = y -- the latitude came out as the scanning angle. A full disk
    // spans 8.7 degrees of scanning angle and 81.3 degrees of Earth, so every product was drawn
    // into a ninth of the area it occupies.
    const double along = h * cosYn * cosXn;
    const double disc = along * along - (h * h - 1.0);
    if (disc < 0.0) return false;                       // the line of sight misses the Earth
    const double t = along - std::sqrt(disc);

    const double px = h - t * cosYn * cosXn;
    const double py = t * cosYn * sinXn;
    const double pz = t * sinYn;

    *latDeg = std::atan2(pz, std::sqrt(px * px + py * py)) * 180.0 / kPi;
    *lonDeg = g.subLonDeg + std::atan2(py, px) * 180.0 / kPi;
    return true;
}

// The real global relief image, decoded once and sampled by geographic coordinates below.
const QImage& cachedSurface() {
    static QImage map = [] {
        const QString path = QDir(QCoreApplication::applicationDirPath())
                                 .filePath(QStringLiteral("resources/maps/world_relief.jpg"));
        QImageReader reader(path);
        reader.setScaledSize(QSize(7200, 3600));
        return reader.read().convertToFormat(QImage::Format_RGB32);
    }();
    return map;
}

// Where each disk pixel lands on the Earth, computed once per geometry.
//
// It does not depend on the image, only on the geometry, so recomputing it for every observation
// was spending 4.84 million trigonometry calls on an answer that had not changed. The map is keyed
// on the three numbers that decide it.
struct ProjectionCache {
    double centre = -1.0;
    double radPerPixel = -1.0;
    double subLon = 0.0;
    LritNavigation navigation;
    int width = 0;
    int height = 0;
    std::vector<float> lat;
    std::vector<float> lon;
    std::vector<std::uint8_t> onEarth;
};
const ProjectionCache& cachedProjection(const DiskGeometry& g, int w, int h) {
    static ProjectionCache cache;
    if (cache.width == w && cache.height == h && cache.centre == g.centre
        && cache.radPerPixel == g.radPerPixel && cache.subLon == g.subLonDeg &&
        cache.navigation.valid == g.navigation.valid &&
        cache.navigation.cfac == g.navigation.cfac && cache.navigation.lfac == g.navigation.lfac &&
        cache.navigation.coff == g.navigation.coff && cache.navigation.loff == g.navigation.loff &&
        cache.navigation.subLonDeg == g.navigation.subLonDeg) {
        return cache;
    }
    cache = ProjectionCache{};
    cache.centre = g.centre;
    cache.radPerPixel = g.radPerPixel;
    cache.subLon = g.subLonDeg;
    cache.navigation = g.navigation;
    cache.width = w;
    cache.height = h;
    const std::size_t n = static_cast<std::size_t>(w) * static_cast<std::size_t>(h);
    cache.lat.assign(n, 0.0f);
    cache.lon.assign(n, 0.0f);
    cache.onEarth.assign(n, 0);
    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            double lat = 0.0, lon = 0.0;
            if (!diskToLatLon(x, y, g, &lat, &lon)) continue;
            const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w)
                                   + static_cast<std::size_t>(x);
            cache.lat[at] = static_cast<float>(lat);
            cache.lon[at] = static_cast<float>(lon);
            cache.onEarth[at] = 1;
        }
    }
    return cache;
}

// Cloud colour by temperature, matching the FY-2 MCIR so the two products read the same way.
void cloudColour(double kelvin, double* r, double* g, double* b) {
    const double coverage = std::clamp((305.0 - kelvin) / (305.0 - 200.0), 0.0, 1.0);
    double cr = 0xE8, cg = 0xEC, cb = 0xF2;
    const double thick = std::clamp((coverage - 0.45) / 0.5, 0.0, 1.0);
    cr = cr * (1.0 - thick) + 0xFF * thick;
    cg = cg * (1.0 - thick) + 0xC2 * thick;
    cb = cb * (1.0 - thick) + 0x6E * thick;
    const double cirrus = std::clamp((coverage - 0.15) / 0.5, 0.0, 1.0) * (1.0 - thick);
    cr = cr * (1.0 - cirrus) + 0xC6 * cirrus;
    cg = cg * (1.0 - cirrus) + 0xE6 * cirrus;
    cb = cb * (1.0 - cirrus) + 0xF0 * cirrus;
    const double whiten = std::clamp((coverage - 0.80) / 0.20, 0.0, 1.0);
    *r = cr + (255.0 - cr) * whiten;
    *g = cg + (255.0 - cg) * whiten;
    *b = cb + (255.0 - cb) * whiten;
}

// The brightness-temperature ramp, for the projected product where colour carries the value.
void tbbColour(double kelvin, double* r, double* g, double* b) {
    // Cold is white through amber, warm is the surface ramp, in the order the eye expects: the
    // coldest tops read as the brightest thing on the map.
    const double t = std::clamp((kelvin - 190.0) / (320.0 - 190.0), 0.0, 1.0);
    struct Stop { double t, r, g, b; };
    static const Stop stops[] = {
        {0.00, 255, 255, 255},   // 190 K, deepest convection
        {0.18, 255, 214, 140},
        {0.35, 236, 236, 240},
        {0.55, 150, 176, 200},
        {0.75, 60,  96,  130},
        {1.00, 12,  26,  46},    // 320 K, warm surface
    };
    for (int i = 0; i + 1 < static_cast<int>(sizeof(stops) / sizeof(stops[0])); ++i) {
        if (t <= stops[i + 1].t) {
            const double span = stops[i + 1].t - stops[i].t;
            const double f = span > 0.0 ? (t - stops[i].t) / span : 0.0;
            *r = stops[i].r + (stops[i + 1].r - stops[i].r) * f;
            *g = stops[i].g + (stops[i + 1].g - stops[i].g) * f;
            *b = stops[i].b + (stops[i + 1].b - stops[i].b) * f;
            return;
        }
    }
    *r = stops[3].r;
    *g = stops[3].g;
    *b = stops[3].b;
}


struct LegendEntry { double celsius; QColor colour; QString label; };

// A colour key in the lower-left corner: one swatch per entry with its temperature, and a title.
void drawLegend(QImage& image, const QString& title, const std::vector<LegendEntry>& entries) {
    if (image.isNull() || entries.empty()) return;
    // Text needs a font database, which exists only in a GUI application. The command-line tools
    // that render these products run a QCoreApplication, where touching QFont is a Qt fatal error
    // (abort); there the swatches are drawn without their labels.
    const bool canWriteText = qobject_cast<QGuiApplication*>(QCoreApplication::instance()) != nullptr;
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing, true);
    const int scale = std::max(1, image.width() / 1100);
    if (canWriteText) {
        QFont font(QStringLiteral("Segoe UI"));
        font.setPixelSize(13 * scale);
        p.setFont(font);
    }
    const int sw = 56 * scale, sh = 16 * scale, pad = 8 * scale;
    const int boxW = static_cast<int>(entries.size()) * sw + 2 * pad;
    const int boxH = sh + 46 * scale;
    const QRect box(pad, image.height() - boxH - pad, std::max(boxW, 330 * scale), boxH);
    p.fillRect(box, QColor(0, 0, 0, 170));
    p.setPen(QColor(235, 235, 235));
    if (canWriteText)
        p.drawText(QRect(box.left() + pad, box.top() + 2 * scale, box.width() - 2 * pad, 18 * scale),
                   Qt::AlignLeft | Qt::AlignVCenter, title);
    for (std::size_t i = 0; i < entries.size(); ++i) {
        const QRect r(box.left() + pad + static_cast<int>(i) * sw, box.top() + 22 * scale, sw - 2 * scale, sh);
        p.fillRect(r, entries[i].colour);
        p.setPen(QColor(120, 120, 120));
        p.drawRect(r);
        p.setPen(QColor(235, 235, 235));
        const QString text = entries[i].label.isEmpty()
            ? QString::number(entries[i].celsius, 'f', 0)
            : QStringLiteral("%1 %2").arg(entries[i].label).arg(entries[i].celsius, 0, 'f', 0);
        if (canWriteText)
            p.drawText(QRect(r.left(), r.bottom() + 2 * scale, sw, 16 * scale), Qt::AlignHCenter | Qt::AlignTop, text);
    }
}
}  // namespace

Calibration calibrationFromLrit(const std::vector<std::uint8_t>& bytes) {
    Calibration cal;
    if (bytes.size() < 16) return cal;
    std::uint32_t total = (static_cast<std::uint32_t>(bytes[4]) << 24)
                          | (static_cast<std::uint32_t>(bytes[5]) << 16)
                          | (static_cast<std::uint32_t>(bytes[6]) << 8)
                          | static_cast<std::uint32_t>(bytes[7]);
    if (total == 0 || total > bytes.size()) total = static_cast<std::uint32_t>(bytes.size());

    std::size_t pos = 0;
    while (pos + 3 <= total) {
        const std::uint8_t type = bytes[pos];
        const std::uint16_t len =
            static_cast<std::uint16_t>((bytes[pos + 1] << 8) | bytes[pos + 2]);
        if (len < 3 || pos + len > total) break;
        if (type == 3) {                                  // annotation
            std::string text;
            for (std::size_t i = pos + 3; i < pos + len; ++i) {
                const std::uint8_t c = bytes[i];
                if (c == '\n') text.push_back('\n');
                else if (c >= 32 && c < 127) text.push_back(static_cast<char>(c));
            }
            std::size_t start = 0;
            while (start <= text.size()) {
                std::size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                const std::string line = text.substr(start, end - start);
                const std::size_t sep = line.find(":=");
                if (sep != std::string::npos && sep > 0) {
                    const std::string key = line.substr(0, sep);
                    const std::string value = line.substr(sep + 2);
                    if (key == "_NAME") cal.channel = value;
                    else if (key == "_UNIT") cal.unit = value;
                    else {
                        bool numeric = !key.empty();
                        for (char c : key)
                            if (c < '0' || c > '9') numeric = false;
                        if (numeric) {
                            const int index = std::atoi(key.c_str());
                            if (index >= 0 && index < 256) {
                                cal.kelvin[static_cast<std::size_t>(index)] = std::atof(value.c_str());
                                cal.valid[static_cast<std::size_t>(index)] = true;
                                ++cal.entries;
                            }
                        }
                    }
                }
                if (end == text.size()) break;
                start = end + 1;
            }
            break;
        }
        pos += len;
    }
    return cal;
}

LritNavigation navigationFromLrit(const std::vector<std::uint8_t>& bytes) {
    LritNavigation nav;
    if (bytes.size() < 16) return nav;
    const auto* raw = reinterpret_cast<const uchar*>(bytes.data());
    const std::uint32_t headerLength = qFromBigEndian<quint32>(raw + 4);
    if (headerLength < 16 || headerLength > bytes.size()) return nav;
    std::size_t pos = 0;
    while (pos + 3 <= headerLength) {
        const std::uint8_t type = bytes[pos];
        const std::uint16_t length = static_cast<std::uint16_t>(
            (std::uint16_t(bytes[pos + 1]) << 8) | bytes[pos + 2]);
        if (length < 3 || pos + length > headerLength) break;
        if (type == 2 && length >= 51) {
            const QString name = QString::fromLatin1(
                reinterpret_cast<const char*>(bytes.data() + pos + 3), 32).trimmed();
            const int open = name.indexOf(QLatin1Char('('));
            const int close = name.indexOf(QLatin1Char(')'), open + 1);
            if (!name.startsWith(QStringLiteral("GEOS"), Qt::CaseInsensitive) ||
                open < 0 || close <= open + 1) return nav;
            bool longitudeOk = false;
            const double longitude = name.mid(open + 1, close - open - 1).toDouble(&longitudeOk);
            const auto* fields = raw + pos + 35;
            nav.cfac = qFromBigEndian<qint32>(fields);
            nav.lfac = qFromBigEndian<qint32>(fields + 4);
            nav.coff = qFromBigEndian<qint32>(fields + 8);
            nav.loff = qFromBigEndian<qint32>(fields + 12);
            nav.subLonDeg = longitude;
            nav.valid = longitudeOk && std::isfinite(longitude) &&
                std::fabs(longitude) <= 180.0 && nav.cfac != 0 && nav.lfac != 0;
            return nav;
        }
        pos += length;
    }
    return nav;
}

DiskGeometry measureDisk(const QImage& disk, double subLonDeg) {
    DiskGeometry g;
    g.size = kFullDiskLines;
    g.centre = kFullDiskLines / 2.0;
    g.subLonDeg = subLonDeg;
    if (disk.isNull() || disk.height() < 40) return g;

    // Sample rows through the middle, because the widest part of a disk is its equator and a row
    // near the top or bottom would measure a chord rather than a diameter.
    int minX = disk.width(), maxX = -1;
    const int mid = disk.height() / 2;
    for (int row = std::max(0, mid - 40); row <= std::min(disk.height() - 1, mid + 40); ++row) {
        const uchar* line = disk.constScanLine(row);
        for (int x = 0; x < disk.width(); ++x) {
            if (line[x] != 0) {
                minX = std::min(minX, x);
                maxX = std::max(maxX, x);
            }
        }
    }
    if (maxX <= minX) return g;

    // A partially received disk gives a chord, not a diameter, and a chord would be read as a
    // smaller Earth. The strips that did arrive are contiguous from the top, so the disk is only
    // measured when enough of it is present for the middle rows to be real.
    g.limbPixels = (maxX - minX + 1) / 2.0;
    g.radPerPixel = (limbAngleDegrees() * kPi / 180.0) / g.limbPixels;
    g.measured = true;
    return g;
}

DiskGeometry measureDisk(const QImage& disk, const LritNavigation& navigation) {
    DiskGeometry g;
    g.size = disk.height();
    g.centre = navigation.loff;
    g.subLonDeg = navigation.subLonDeg;
    g.navigation = navigation;
    if (disk.isNull() || !navigation.valid || navigation.cfac == 0 || navigation.lfac == 0)
        return g;
    // The image navigation record provides the exact GEOS scaling and offsets. Do not
    // infer them from the visible limb: a partial/thresholded disk changes that diameter.
    g.radPerPixel = (65536.0 / std::fabs(double(navigation.cfac))) * kPi / 180.0;
    g.limbPixels = 0.0;
    g.measured = std::isfinite(g.radPerPixel) && g.radPerPixel > 0.0;
    return g;
}

QStringList renderObservationProducts(const QImage& disk,
                                      const Calibration& calibration,
                                      const DiskGeometry& geometry,
                                      const QString& directory,
                                      const QString& stem,
                                      const Region& region) {
    QStringList written;
    if (disk.isNull()) return written;
    QDir().mkpath(directory);

    // The assembled sensor counts are useful for checking the decode even when the LRIT
    // annotation/calibration table was not recovered. Keep this separate from the calibrated
    // products below.
    {
        const QString path = QDir(directory).filePath(stem + QStringLiteral("_IR_raw.png"));
        if (savePngFast(disk, path)) written.append(path);
    }

    if (!calibration.usable()) {
        nadirstation::log::write(QStringLiteral(
            "GK-2A: no usable calibration (%1 table entries); saved raw IR only")
            .arg(calibration.entries));
        return written;
    }

    const int w = disk.width();
    const int h = disk.height();

    // IR enhanced: the calibrated temperature with the range that matters spread across the
    // display. A raw IR disk spends most of its range on cold tops and warm sea and looks flat
    // until that range is stretched.
    {
        QImage out(w, h, QImage::Format_Grayscale8);
        out.fill(0);
        constexpr double kCold = 190.0, kWarm = 310.0;
        for (int y = 0; y < h; ++y) {
            const uchar* src = disk.constScanLine(y);
            uchar* dst = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int c = src[x];
                if (c == 0 || !calibration.valid[static_cast<std::size_t>(c)]) {
                    dst[x] = 0;
                    continue;
                }
                const double k = calibration.kelvin[static_cast<std::size_t>(c)];
                const double t = std::clamp((kWarm - k) / (kWarm - kCold), 0.0, 1.0);
                dst[x] = static_cast<uchar>(std::lround(t * 255.0));
            }
        }
        const QString path = QDir(directory).filePath(stem + QStringLiteral("_IR_enhanced.png"));
        if (savePngFast(out, path)) written.append(path);
    }

    // Dvorak BD-curve enhancement: the standard step table used for tropical-cyclone intensity
    // analysis. Warm scenes keep a grey ramp; cold cloud tops fall into the fixed bands DG, MG, LG,
    // B, W, CMG and CDG, so a band edge on the image is a known temperature.
    {
        auto bdGrey = [](double celsius) -> int {
            if (celsius >= 9.0) return static_cast<int>(std::lround(std::clamp((30.0 - celsius) / 21.0, 0.0, 1.0) * 60.0));
            if (celsius > -30.0) return static_cast<int>(std::lround(60.0 + (9.0 - celsius) / 39.0 * 100.0));   // 60..160
            if (celsius > -42.0) return 92;    // DG  -31..-41
            if (celsius > -54.0) return 138;   // MG  -42..-53
            if (celsius > -64.0) return 184;   // LG  -54..-63
            if (celsius > -70.0) return 0;     // B   -64..-69
            if (celsius > -76.0) return 255;   // W   -70..-75
            if (celsius > -81.0) return 138;   // CMG -76..-80
            return 92;                         // CDG <= -81
        };
        QImage out(w, h, QImage::Format_Grayscale8);
        out.fill(0);
        for (int y = 0; y < h; ++y) {
            const uchar* src = disk.constScanLine(y);
            uchar* dst = out.scanLine(y);
            for (int x = 0; x < w; ++x) {
                const int c = src[x];
                if (c == 0 || !calibration.valid[static_cast<std::size_t>(c)]) continue;
                dst[x] = static_cast<uchar>(bdGrey(calibration.kelvin[static_cast<std::size_t>(c)] - 273.15));
            }
        }
        QImage rgb = out.convertToFormat(QImage::Format_RGB32);
        drawLegend(rgb, QStringLiteral("GK-2A IR105 · Dvorak BD curve"),
                   {{-81, QColor(92, 92, 92), QStringLiteral("CDG")}, {-76, QColor(138, 138, 138), QStringLiteral("CMG")},
                    {-70, QColor(255, 255, 255), QStringLiteral("W")}, {-64, QColor(0, 0, 0), QStringLiteral("B")},
                    {-54, QColor(184, 184, 184), QStringLiteral("LG")}, {-42, QColor(138, 138, 138), QStringLiteral("MG")},
                    {-31, QColor(92, 92, 92), QStringLiteral("DG")}});
        const QString path = QDir(directory).filePath(stem + QStringLiteral("_IR_BD.png"));
        if (savePngFast(rgb, path)) written.append(path);
    }

    // Cloud-top temperature colour enhancement: grey above -20 C, then colour steps every 10 C so
    // convective cores and their cold overshooting tops stand out.
    {
        struct Stop { double c; QColor colour; };
        static const Stop stops[] = {
            {-20.0, QColor(110, 200, 230)}, {-30.0, QColor(40, 110, 220)}, {-40.0, QColor(40, 180, 70)},
            {-50.0, QColor(240, 230, 60)},  {-60.0, QColor(245, 140, 30)}, {-70.0, QColor(220, 30, 30)},
            {-80.0, QColor(150, 20, 160)},  {-90.0, QColor(255, 255, 255)},
        };
        auto colourOf = [](double celsius) -> QRgb {
            if (celsius > -20.0) {
                const int g = static_cast<int>(std::lround(std::clamp((40.0 - celsius) / 60.0, 0.0, 1.0) * 200.0));
                return qRgb(g, g, g);
            }
            for (std::size_t i = 0; i + 1 < std::size(stops); ++i) {
                if (celsius > stops[i + 1].c) {
                    const double f = (stops[i].c - celsius) / (stops[i].c - stops[i + 1].c);
                    const QColor& a = stops[i].colour;
                    const QColor& b = stops[i + 1].colour;
                    return qRgb(static_cast<int>(a.red() + (b.red() - a.red()) * f),
                                static_cast<int>(a.green() + (b.green() - a.green()) * f),
                                static_cast<int>(a.blue() + (b.blue() - a.blue()) * f));
                }
            }
            return stops[std::size(stops) - 1].colour.rgb();
        };
        QImage out(w, h, QImage::Format_RGB32);
        out.fill(Qt::black);
        for (int y = 0; y < h; ++y) {
            const uchar* src = disk.constScanLine(y);
            auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const int c = src[x];
                if (c == 0 || !calibration.valid[static_cast<std::size_t>(c)]) continue;
                dst[x] = colourOf(calibration.kelvin[static_cast<std::size_t>(c)] - 273.15);
            }
        }
        std::vector<LegendEntry> entries;
        for (const Stop& s : stops) entries.push_back({s.c, s.colour, QString()});
        drawLegend(out, QStringLiteral("GK-2A IR105 · cloud-top temperature (°C)"), entries);
        const QString path = QDir(directory).filePath(stem + QStringLiteral("_IR_Rainbow.png"));
        if (savePngFast(out, path)) written.append(path);
    }

    // The brightness temperature as numbers, in the same pixel space as the pictures above.
    // Without this the hover has nothing to read on GK-2A, because every product here is named
    // differently from the FY-2 ones the hover was written against. Zero means no temperature:
    // outside the disk, in a strip that did not arrive, or where the calibration does not reach.
    if (geometry.measured) {
        QImage numeric(w, h, QImage::Format_Grayscale16);
        numeric.fill(0);
        for (int y = 0; y < h; ++y) {
            const uchar* src = disk.constScanLine(y);
            auto* dst = reinterpret_cast<quint16*>(numeric.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const int c = src[x];
                if (c == 0 || !calibration.valid[static_cast<std::size_t>(c)]) continue;
                const double k = calibration.kelvin[static_cast<std::size_t>(c)];
                if (k < 150.0 || k > 350.0) continue;
                dst[x] = static_cast<quint16>(std::lround(k * 100.0));
            }
        }
        // Written, and deliberately not appended to the product list. It is the same data as the
        // picture beside it, in numbers, and it is here for the hover to read -- offering it as a
        // product would put a grey rectangle next to the coloured map and invite the reader to
        // work out which one they wanted.
        const QString numericPath =
            QDir(directory).filePath(stem + QStringLiteral("_IR_0p01K.png"));
        savePngFast(numeric, numericPath);
    }

    // A grayscale, calibrated enhancement of the selected geographic region, paired with a
    // 0.01 K raster so the UI can report the actual temperature under the cursor.
    if (geometry.measured) {
        const double lonHalf = region.spanDeg / 2.0;
        const double latHalf = lonHalf * 0.62;
        const double west = region.centreLon - lonHalf, east = region.centreLon + lonHalf;
        const double north = region.centreLat + latHalf, south = region.centreLat - latHalf;
        const int rw = static_cast<int>(std::lround(region.spanDeg * 20.0));
        const int rh = static_cast<int>(std::lround((north - south) * 20.0));
        QImage enhanced(rw, rh, QImage::Format_Grayscale8);
        enhanced.fill(0);
        QImage numeric(rw, rh, QImage::Format_Grayscale16);
        numeric.fill(0);
        constexpr double kCold = 190.0, kWarm = 310.0;
        for (int y = 0; y < rh; ++y) {
            uchar* dst = enhanced.scanLine(y);
            auto* ndst = reinterpret_cast<quint16*>(numeric.scanLine(y));
            const double lat = north - (y + 0.5) * (north - south) / rh;
            for (int x = 0; x < rw; ++x) {
                const double lon = west + (x + 0.5) * (east - west) / rw;
                double col = 0.0, row = 0.0;
                if (!geosPixel(lat, lon, geometry, &col, &row)) continue;
                const int ix = static_cast<int>(std::lround(col));
                const int iy = static_cast<int>(std::lround(row));
                if (ix < 0 || iy < 0 || ix >= w || iy >= h) continue;
                const int c = disk.constScanLine(iy)[ix];
                if (c == 0 || !calibration.valid[static_cast<std::size_t>(c)]) continue;
                const double k = calibration.kelvin[static_cast<std::size_t>(c)];
                if (k < 150.0 || k > 350.0) continue;
                ndst[x] = static_cast<quint16>(std::lround(k * 100.0));
                dst[x] = static_cast<uchar>(std::lround(
                    std::clamp((kWarm - k) / (kWarm - kCold), 0.0, 1.0) * 255.0));
            }
        }
        const QString path = QDir(directory).filePath(stem + QStringLiteral("_IR_Region_enhanced.png"));
        if (savePngFast(enhanced, path)) {
            written.append(path);
            writeGeographicWorldFiles(path, rw, rh, west, east, south, north);
        }
        const QString numericPath = QDir(directory).filePath(stem + QStringLiteral("_IR_Region_0p01K.png"));
        if (savePngFast(numeric, numericPath))
            writeGeographicWorldFiles(numericPath, rw, rh, west, east, south, north);
    }

    const QImage& basemap = cachedSurface();

    // MCIR: cloud coloured by height over the surface, on the disk as received.
    if (!basemap.isNull() && geometry.measured) {
        // The per-pixel geography comes from the cache: it is the same answer for every observation
        // at this geometry, and recomputing it was 4.84 million trigonometry calls each time.
        const ProjectionCache& proj = cachedProjection(geometry, w, h);
        QImage out(w, h, QImage::Format_RGB32);
        for (int y = 0; y < h; ++y) {
            const uchar* src = disk.constScanLine(y);
            auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
            for (int x = 0; x < w; ++x) {
                const std::size_t at = static_cast<std::size_t>(y) * static_cast<std::size_t>(w)
                                       + static_cast<std::size_t>(x);
                if (!proj.onEarth[at]) {
                    dst[x] = qRgb(0, 0, 0);
                    continue;
                }
                const double lat = proj.lat[at];
                const double lon = proj.lon[at];
                const int bx = std::clamp(static_cast<int>((lon + 180.0) / 360.0 * basemap.width()),
                                          0, basemap.width() - 1);
                const int by = std::clamp(static_cast<int>((90.0 - lat) / 180.0 * basemap.height()),
                                          0, basemap.height() - 1);
                const QRgb base = reinterpret_cast<const QRgb*>(basemap.constScanLine(by))[bx];

                const int c = src[x];
                if (c == 0 || !calibration.valid[static_cast<std::size_t>(c)]) {
                    dst[x] = base;
                    continue;
                }
                const double k = calibration.kelvin[static_cast<std::size_t>(c)];
                double cr = 0.0, cg = 0.0, cb = 0.0;
                cloudColour(k, &cr, &cg, &cb);
                const double coverage = std::clamp((305.0 - k) / (305.0 - 200.0), 0.0, 1.0);
                const double weight = std::pow(coverage, 1.45) * 0.97;
                dst[x] = qRgb(qBound(0, qRound(qRed(base) * (1.0 - weight) + cr * weight), 255),
                              qBound(0, qRound(qGreen(base) * (1.0 - weight) + cg * weight), 255),
                              qBound(0, qRound(qBlue(base) * (1.0 - weight) + cb * weight), 255));
            }
        }
        const QString path = QDir(directory).filePath(stem + QStringLiteral("_MCIR.png"));
        if (savePngFast(out, path)) written.append(path);
    }

    // TBB projected: the same temperature on a latitude and longitude grid, which is a map rather
    // than a view, and the form most uses of this data take.
    if (geometry.measured) {
        const double kCentreLon = region.centreLon, kCentreLat = region.centreLat;
        const double kSpan = region.spanDeg;
        const double lonHalf = kSpan / 2.0;
        const double latHalf = lonHalf * 0.62;
        const double west = kCentreLon - lonHalf, east = kCentreLon + lonHalf;
        const double south = kCentreLat - latHalf, north = kCentreLat + latHalf;
        const int pw = static_cast<int>(std::lround(kSpan * 20.0));
        const int ph = static_cast<int>(std::lround((north - south) * 20.0));

        QImage out(pw, ph, QImage::Format_RGB32);
        // Written beside the colour image and in the same grid, so a reading is the value of the
        // pixel actually under the cursor rather than of a neighbour in a different projection.
        QImage numeric(pw, ph, QImage::Format_Grayscale16);
        numeric.fill(0);
        for (int y = 0; y < ph; ++y) {
            auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
            auto* ndst = reinterpret_cast<quint16*>(numeric.scanLine(y));
            const double lat = north - (y + 0.5) * (north - south) / ph;
            for (int x = 0; x < pw; ++x) {
                const double lon = west + (x + 0.5) * (east - west) / pw;
                double col = 0.0, row = 0.0;
                if (!geosPixel(lat, lon, geometry, &col, &row)) {
                    dst[x] = qRgb(12, 26, 46);      // beyond the limb: nothing is seen here
                    continue;
                }
                const int ix = static_cast<int>(std::lround(col));
                const int iy = static_cast<int>(std::lround(row));
                if (ix < 0 || iy < 0 || ix >= w || iy >= h) {
                    dst[x] = qRgb(12, 26, 46);
                    continue;
                }
                const int c = disk.constScanLine(iy)[ix];
                if (c == 0 || !calibration.valid[static_cast<std::size_t>(c)]) {
                    dst[x] = qRgb(12, 26, 46);
                    continue;
                }
                const double k = calibration.kelvin[static_cast<std::size_t>(c)];
                if (k >= 150.0 && k <= 350.0)
                    ndst[x] = static_cast<quint16>(std::lround(k * 100.0));
                double cr = 0.0, cg = 0.0, cb = 0.0;
                tbbColour(k, &cr, &cg, &cb);
                dst[x] = qRgb(qBound(0, qRound(cr), 255), qBound(0, qRound(cg), 255),
                              qBound(0, qRound(cb), 255));
            }
        }
        const QString path = QDir(directory).filePath(stem + QStringLiteral("_TBB_projected.png"));
        if (savePngFast(out, path)) {
            written.append(path);
            writeGeographicWorldFiles(path, pw, ph, west, east, south, north);
        }
        // Written, and not listed, for the same reason as the disk-space one above.
        const QString numericPath =
            QDir(directory).filePath(stem + QStringLiteral("_TBB_0p01K.png"));
        if (savePngFast(numeric, numericPath))
            writeGeographicWorldFiles(numericPath, pw, ph, west, east, south, north);
    }

    // The same composite over the chosen region, which is the form this product is actually read
    // in: a typhoon is looked at as a typhoon, not as a fraction of a disk. Drawn through the same
    // code as the disk, so a fix to one is a fix to the other.
    if (geometry.measured && !basemap.isNull()) {
        const double lonHalf = region.spanDeg / 2.0;
        const double latHalf = lonHalf * 0.62;
        const double west = region.centreLon - lonHalf, east = region.centreLon + lonHalf;
        const double north = region.centreLat + latHalf, south = region.centreLat - latHalf;
        // Keep the standalone exporter on the same 20 px/degree grid as
        // renderRegionalProductsFromTbb; both write the same *_Region files.
        const int rw = static_cast<int>(std::lround(region.spanDeg * 20.0));
        const int rh = static_cast<int>(std::lround((north - south) * 20.0));

        QImage out(rw, rh, QImage::Format_RGB32);
        for (int y = 0; y < rh; ++y) {
            auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
            const double lat = north - (y + 0.5) * (north - south) / rh;
            const int by = std::clamp(static_cast<int>((90.0 - lat) / 180.0 * basemap.height()),
                                      0, basemap.height() - 1);
            const QRgb* baseRow = reinterpret_cast<const QRgb*>(basemap.constScanLine(by));
            for (int x = 0; x < rw; ++x) {
                const double lon = west + (x + 0.5) * (east - west) / rw;
                const int bx = std::clamp(static_cast<int>((lon + 180.0) / 360.0 * basemap.width()),
                                          0, basemap.width() - 1);
                const QRgb base = baseRow[bx];
                double col = 0.0, row = 0.0;
                if (!geosPixel(lat, lon, geometry, &col, &row)) { dst[x] = base; continue; }
                const int ix = static_cast<int>(std::lround(col));
                const int iy = static_cast<int>(std::lround(row));
                if (ix < 0 || iy < 0 || ix >= w || iy >= h) { dst[x] = base; continue; }
                const int c = disk.constScanLine(iy)[ix];
                if (c == 0 || !calibration.valid[static_cast<std::size_t>(c)]) { dst[x] = base; continue; }
                const double k = calibration.kelvin[static_cast<std::size_t>(c)];
                double cr = 0.0, cg = 0.0, cb = 0.0;
                cloudColour(k, &cr, &cg, &cb);
                const double coverage = std::clamp((305.0 - k) / (305.0 - 200.0), 0.0, 1.0);
                const double weight = std::pow(coverage, 1.45) * 0.97;
                dst[x] = qRgb(qBound(0, qRound(qRed(base) * (1.0 - weight) + cr * weight), 255),
                              qBound(0, qRound(qGreen(base) * (1.0 - weight) + cg * weight), 255),
                              qBound(0, qRound(qBlue(base) * (1.0 - weight) + cb * weight), 255));
            }
        }
        // This is the geographic (equirectangular) MCIR map. The surface beneath it is made
        // from a binary land mask, so internal political/terrain lines in the source mask never
        // become border annotations in the product.
        const QString path = QDir(directory).filePath(stem + QStringLiteral("_MCIR_projected.png"));
        if (savePngFast(out, path)) {
            written.append(path);
            writeGeographicWorldFiles(path, rw, rh, west, east, south, north);
        }

        QJsonObject metadata;
        metadata[QStringLiteral("format")] = QStringLiteral("GK2A-Regional-Products-1");
        metadata[QStringLiteral("center_latitude_deg")] = region.centreLat;
        metadata[QStringLiteral("center_longitude_deg")] = region.centreLon;
        metadata[QStringLiteral("span_deg")] = region.spanDeg;
        metadata[QStringLiteral("pixels_per_degree")] = 20;
        metadata[QStringLiteral("crs")] = QStringLiteral("EPSG:4326");
        metadata[QStringLiteral("grid_registration")] = QStringLiteral("pixel-centre");
        metadata[QStringLiteral("pixel_size_degrees")] =
            QJsonArray{(east - west) / rw, -((north - south) / rh)};
        metadata[QStringLiteral("upper_left_pixel_center_lonlat_deg")] =
            QJsonArray{west + (east - west) / (2.0 * rw),
                       north - (north - south) / (2.0 * rh)};
        metadata[QStringLiteral("products")] =
            QJsonArray{stem + QStringLiteral("_IR_Region_enhanced.png"),
                       stem + QStringLiteral("_IR_Region_0p01K.png"),
                       stem + QStringLiteral("_TBB_projected.png"),
                       stem + QStringLiteral("_TBB_0p01K.png"),
                       stem + QStringLiteral("_MCIR_projected.png")};
        metadata[QStringLiteral("background_source")] =
            QStringLiteral("resources/maps/world_relief.jpg");
        metadata[QStringLiteral("mcir_interpretation")] =
            QStringLiteral("single-channel IR105 cloud-coloured visualization; not a multispectral cloud retrieval");
        QFile sidecar(QDir(directory).filePath(stem + QStringLiteral("_Region.json")));
        if (sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate))
            sidecar.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    }

    nadirstation::log::write(QStringLiteral("GK-2A: %1 product(s) rendered from channel %2")
        .arg(written.size())
        .arg(calibration.channel.empty() ? QStringLiteral("(unnamed)")
                                         : QString::fromStdString(calibration.channel)));
    return written;
}

QStringList renderRegionalProductsFromTbb(const QImage& disk,
                                          const QImage& tbb01K,
                                          const DiskGeometry& geometry,
                                          const QString& directory,
                                          const QString& stem,
                                          const Region& region) {
    QStringList written;
    if (disk.isNull() || tbb01K.isNull() || !geometry.measured
        || disk.size() != tbb01K.size() || region.spanDeg <= 0.0) return written;
    QDir().mkpath(directory);
    const QImage& basemap = cachedSurface();
    if (basemap.isNull()) return written;

    const double lonHalf = region.spanDeg / 2.0;
    const double latHalf = lonHalf * 0.62;
    const double west = region.centreLon - lonHalf, east = region.centreLon + lonHalf;
    const double north = region.centreLat + latHalf, south = region.centreLat - latHalf;
    const int w = static_cast<int>(std::lround(region.spanDeg * 20.0));
    const int h = static_cast<int>(std::lround((north - south) * 20.0));
    if (w <= 0 || h <= 0) return written;

    QImage enhanced(w, h, QImage::Format_Grayscale8);
    enhanced.fill(0);
    QImage numeric(w, h, QImage::Format_Grayscale16);
    numeric.fill(0);
    QImage mcir(w, h, QImage::Format_RGB32);
    constexpr double kCold = 190.0, kWarm = 310.0;
    for (int y = 0; y < h; ++y) {
        uchar* e = enhanced.scanLine(y);
        auto* n = reinterpret_cast<quint16*>(numeric.scanLine(y));
        auto* rgb = reinterpret_cast<QRgb*>(mcir.scanLine(y));
        const double lat = north - (y + 0.5) * (north - south) / h;
        const int by = std::clamp(static_cast<int>((90.0 - lat) / 180.0 * basemap.height()),
                                  0, basemap.height() - 1);
        const QRgb* baseRow = reinterpret_cast<const QRgb*>(basemap.constScanLine(by));
        for (int x = 0; x < w; ++x) {
            const double lon = west + (x + 0.5) * (east - west) / w;
            const int bx = std::clamp(static_cast<int>((lon + 180.0) / 360.0 * basemap.width()),
                                      0, basemap.width() - 1);
            const QRgb base = baseRow[bx];
            rgb[x] = base;
            double col = 0.0, row = 0.0;
            if (!geosPixel(lat, lon, geometry, &col, &row)) continue;
            const int ix = static_cast<int>(std::lround(col));
            const int iy = static_cast<int>(std::lround(row));
            if (ix < 0 || iy < 0 || ix >= disk.width() || iy >= disk.height()) continue;
            const int count = disk.constScanLine(iy)[ix];
            const quint16 rawTbb = reinterpret_cast<const quint16*>(tbb01K.constScanLine(iy))[ix];
            if (count == 0 || rawTbb == 0) continue;
            const double kelvin = rawTbb / 100.0;
            if (kelvin < 150.0 || kelvin > 350.0) continue;
            n[x] = rawTbb;
            e[x] = static_cast<uchar>(std::lround(
                std::clamp((kWarm - kelvin) / (kWarm - kCold), 0.0, 1.0) * 255.0));
            double cr = 0.0, cg = 0.0, cb = 0.0;
            cloudColour(kelvin, &cr, &cg, &cb);
            const double coverage = std::clamp((305.0 - kelvin) / (305.0 - 200.0), 0.0, 1.0);
            const double weight = std::pow(coverage, 1.45) * 0.97;
            rgb[x] = qRgb(qBound(0, qRound(qRed(base) * (1.0 - weight) + cr * weight), 255),
                          qBound(0, qRound(qGreen(base) * (1.0 - weight) + cg * weight), 255),
                          qBound(0, qRound(qBlue(base) * (1.0 - weight) + cb * weight), 255));
        }
    }

    const QString enhancedPath = QDir(directory).filePath(stem + QStringLiteral("_IR_Region_enhanced.png"));
    const QString mcirPath = QDir(directory).filePath(stem + QStringLiteral("_MCIR_Region.png"));
    const QString numericPath = QDir(directory).filePath(stem + QStringLiteral("_IR_Region_0p01K.png"));
    if (savePngFast(enhanced, enhancedPath)) {
        written.append(enhancedPath);
        writeGeographicWorldFiles(enhancedPath, w, h, west, east, south, north);
    }
    if (savePngFast(mcir, mcirPath)) {
        written.append(mcirPath);
        writeGeographicWorldFiles(mcirPath, w, h, west, east, south, north);
    }
    if (savePngFast(numeric, numericPath))
        writeGeographicWorldFiles(numericPath, w, h, west, east, south, north);

    QJsonObject metadata;
    metadata[QStringLiteral("format")] = QStringLiteral("GK2A-Regional-Products-1");
    metadata[QStringLiteral("center_latitude_deg")] = region.centreLat;
    metadata[QStringLiteral("center_longitude_deg")] = region.centreLon;
    metadata[QStringLiteral("span_deg")] = region.spanDeg;
    metadata[QStringLiteral("pixels_per_degree")] = 20;
    metadata[QStringLiteral("crs")] = QStringLiteral("EPSG:4326");
    metadata[QStringLiteral("grid_registration")] = QStringLiteral("pixel-centre");
    metadata[QStringLiteral("pixel_size_degrees")] = QJsonArray{region.spanDeg / w, -((north - south) / h)};
    metadata[QStringLiteral("upper_left_pixel_center_lonlat_deg")] =
        QJsonArray{west + (east - west) / (2.0 * w), north - (north - south) / (2.0 * h)};
    metadata[QStringLiteral("products")] = QJsonArray{QFileInfo(enhancedPath).fileName(),
        QFileInfo(numericPath).fileName(), QFileInfo(mcirPath).fileName()};
    metadata[QStringLiteral("background_source")] =
        QStringLiteral("resources/maps/world_relief.jpg");
    metadata[QStringLiteral("mcir_interpretation")] =
        QStringLiteral("single-channel IR105 cloud-coloured visualization; not a multispectral cloud retrieval");
    QFile sidecar(QDir(directory).filePath(stem + QStringLiteral("_Region.json")));
    if (sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate))
        sidecar.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
    return written;
}

}  // namespace nadirstation::gk2a
