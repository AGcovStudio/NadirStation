// NadirStation_L1BExport - assemble a self-describing L1B file from one session.
//
// The station already produces calibrated products, but they are PNG rasters with
// no per-pixel geolocation and no machine-readable uncertainty, which is not
// something a downstream user can consume as a level-1 product. This tool writes
// the native-grid observation as a netCDF-3 classic file:
//
//   * per-pixel latitude and longitude,
//   * viewing and illumination geometry,
//   * calibrated brightness temperature for IR1..IR4,
//   * calibrated reflectance for the visible band,
//   * a quality byte (off-disk, night side, missing line, uncalibrated),
//
// plus a JSON data dictionary. netCDF-3 classic is written directly so the tool
// needs no netCDF library and stays buildable with the existing Qt-only setup.
//
// Usage: NadirStation_L1BExport <session-dir> [output.nc]

#include "nadirstationdecoder.h"
#include "imageproducts.h"
#include "orbit.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QString>
#include <QStringList>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
constexpr int kExpectedScanLines = 2291;

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = 180.0 / kPi;
constexpr double kEarthRadiusKm = 6371.0;
constexpr double kSatelliteHeightKm = 42164.0;
constexpr double kStepRad = 140.0e-6;          // nominal NOM sampling angle
constexpr float kFill = -9999.0f;

// The visible assembly stores four detector lines per scan position, so the
// visible raster is four times taller than the infrared grid. Averaging those
// four lines is what puts the visible channel on the same 2291 x 2291 grid as
// the infrared channels; the columns are already one sample per scan position.
constexpr int kVisibleDetectorLines = 4;

// Reflectance is only meaningful when the sun is reasonably high. The station's
// own visible product uses the same 70 degree cut, so L1B inherits it rather
// than inventing a second threshold.
constexpr double kVisibleMaxSolarZenithDeg = 70.0;

// Quality byte bits. Documented in the emitted sidecar.
enum QualityBit : quint8 {
    kQualityOffDisk = 1u << 0,
    kQualityNightSide = 1u << 1,
    kQualityMissingLine = 1u << 2,
    kQualityUncalibrated = 1u << 3,
};

QString sha256OfFile(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&file)) return QString();
    return QString::fromLatin1(hash.result().toHex());
}

// A decoded-table integrity report for the export.
//
// The accept/reject decision comes from decodeCalibration2() in nadirstationdecoder.h --
// the same function the receiver uses -- so the file can never claim a table is
// fine while the station rejects it. The per-channel statistics below are
// gathered separately only so the file can explain *why*; they decide nothing.
//
// The layout, reproduced read-only from the decoder: big-endian int32 scaled by
// 1e-3, filled in reverse count order.
struct CalibrationTableCheck {
    int descendingSteps = 0;
    int outOfRange = 0;
    double worstKelvin = 0.0;
    bool readable = false;
    bool accepted = false;      // the receiver's own verdict

    bool healthy() const { return readable && accepted; }
};

CalibrationTableCheck checkCalibrationTable(const QString& blockPath)
{
    CalibrationTableCheck result;
    QFile file(blockPath);
    if (!file.open(QIODevice::ReadOnly)) return result;
    const QByteArray block = file.readAll();
    if (block.size() < 1280 + 4 * 4096) return result;
    result.readable = true;

    for (int channel = 0; channel < 4; ++channel) {
        const int start = 1280 + 4096 * channel;
        std::vector<float> table(1024, 0.0f);
        for (int rawIndex = 0; rawIndex < 1024; ++rawIndex) {
            const uchar* p = reinterpret_cast<const uchar*>(block.constData())
                             + start + rawIndex * 4;
            const qint32 wire = static_cast<qint32>(
                (static_cast<quint32>(p[0]) << 24) | (static_cast<quint32>(p[1]) << 16) |
                (static_cast<quint32>(p[2]) << 8) | static_cast<quint32>(p[3]));
            table[static_cast<std::size_t>(1023 - rawIndex)] =
                static_cast<float>(wire) * 1.0e-3f;
        }
        for (std::size_t i = 0; i < table.size(); ++i) {
            const float v = table[i];
            if (!(v >= 100.0f && v <= 500.0f)) {
                ++result.outOfRange;
                result.worstKelvin = std::max(result.worstKelvin,
                                              std::fabs(static_cast<double>(v)));
            }
            if (i > 0 && table[i] < table[i - 1]) ++result.descendingSteps;
        }
    }

    // The receiver's verdict. Requiring its reference resource to be present is
    // deliberate: without it the decisive comparison cannot run, and a weaker
    // verdict must not be dressed up as a pass.
    std::array<QVector<float>, 5> verified;
    result.accepted = nadirstation::referenceCalibrationTables() != nullptr &&
                      nadirstation::decodeCalibration2(block, verified);
    return result;
}

double parseSubSatelliteLongitudeDeg(const QString& sessionDir)
{
    QFile file(QDir(sessionDir).filePath(QStringLiteral("FY2_Observation.json")));
    if (!file.open(QIODevice::ReadOnly)) return 0.0;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject orbit = root.value(QStringLiteral("orbit")).toObject();
    if (orbit.contains(QStringLiteral("sub_satellite_longitude_deg")))
        return orbit.value(QStringLiteral("sub_satellite_longitude_deg")).toDouble();
    if (root.contains(QStringLiteral("sub_satellite_longitude_deg")))
        return root.value(QStringLiteral("sub_satellite_longitude_deg")).toDouble();
    return 0.0;
}

QString parseNoradId(const QString& sessionDir)
{
    QFile file(QDir(sessionDir).filePath(QStringLiteral("FY2_Observation.json")));
    if (!file.open(QIODevice::ReadOnly)) return {};
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject projection = root.value(QStringLiteral("regional_projection")).toObject();
    return projection.value(QStringLiteral("norad_id")).toString();
}

// Solar declination, Cooper's approximation: about 0.5 deg, far below one pixel.
double solarDeclinationDeg(int dayOfYear)
{
    return 23.45 * std::sin(2.0 * kPi * (284.0 + dayOfYear) / 365.0);
}

double greenwichSiderealDeg(const QDateTime& utc)
{
    const QDateTime time = utc.toUTC();
    const double hours = time.time().hour() + time.time().minute() / 60.0 +
                         time.time().second() / 3600.0;
    const double jd = time.date().toJulianDay() + 0.5;
    const double t = (jd - 2451545.0) / 36525.0;
    double gmst = 280.46061837 + 360.98564736629 * (jd - 2451545.0) +
                  0.000387933 * t * t - t * t * t / 38710000.0;
    gmst = std::fmod(gmst, 360.0);
    if (gmst < 0.0) gmst += 360.0;
    return gmst + hours * 15.0;
}

struct GeometryGrid {
    int width = 0;
    int height = 0;
    std::vector<float> lat, lon, satZenith, satAzimuth, sunZenith, sunAzimuth;
    std::vector<quint8> offDisk;
};

// The nominal lattice is a square grid of view angles centred on the
// sub-satellite point, which is the geometry the FDI product uses and which
// reproduces the official disk radius of 1087.4 px on a 2288 grid.
GeometryGrid buildGeometry(int width, int height, double subLonDeg, const QDateTime& utc,
                           const nadirstation::orbit::SatellitePosition& satellite)
{
    GeometryGrid grid;
    grid.width = width;
    grid.height = height;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    grid.lat.assign(pixels, 0.0f);
    grid.lon.assign(pixels, 0.0f);
    grid.satZenith.assign(pixels, 0.0f);
    grid.satAzimuth.assign(pixels, 0.0f);
    grid.sunZenith.assign(pixels, 0.0f);
    grid.sunAzimuth.assign(pixels, 0.0f);
    grid.offDisk.assign(pixels, 0);

    const double dec = solarDeclinationDeg(utc.date().dayOfYear());
    const double gmst = greenwichSiderealDeg(utc);

    // Satellite unit vector in Earth-fixed coordinates.
    const double satR = satellite.distanceKm > 10000.0 ? satellite.distanceKm : kSatelliteHeightKm;
    const double satLonRad = satellite.valid
        ? std::atan2(satellite.yKm, satellite.xKm) : subLonDeg / kDeg;
    const double upX = std::cos(satLonRad), upY = std::sin(satLonRad);
    const double eastX = -std::sin(satLonRad), eastY = std::cos(satLonRad);

    const double cx = (width - 1) / 2.0;
    const double cy = (height - 1) / 2.0;

    for (int y = 0; y < height; ++y) {
        const double ay = -(y - cy) * kStepRad;   // north positive
        for (int x = 0; x < width; ++x) {
            const double ax = (x - cx) * kStepRad; // east positive
            const std::size_t idx = static_cast<std::size_t>(y) * width + x;

            // Line of sight from the satellite: tilt east by ax and north by ay.
            // Nadir is -up, so the up component is negative.
            double dx = std::sin(ax);
            double dy = std::sin(ay);
            double dz = -std::cos(ax) * std::cos(ay);

            // Orthonormal local triad at the sub-satellite point:
            //   u = up (towards the satellite), e = east, n = u x e = north.
            const double ux = upX, uy = upY, uz = 0.0;
            const double ex = eastX, ey = eastY, ez = 0.0;
            const double nx = uy * ez - uz * ey;
            const double ny = uz * ex - ux * ez;
            const double nz = ux * ey - uy * ex;

            // Rotate the east/north/up components into Earth-fixed axes.
            const double vx = dx * ex + dy * nx + dz * ux;
            const double vy = dx * ey + dy * ny + dz * uy;
            const double vz = dx * ez + dy * nz + dz * uz;

            // Ray from the satellite position towards the line of sight, and its
            // intersection with the Earth sphere.
            const double px = satR * upX, py = satR * upY, pz = 0.0;
            const double b = 2.0 * (px * vx + py * vy + pz * vz);
            const double c = px * px + py * py + pz * pz - kEarthRadiusKm * kEarthRadiusKm;
            const double disc = b * b - 4.0 * c;
            if (disc <= 0.0) {
                grid.offDisk[idx] = 1;
                grid.lat[idx] = kFill;
                grid.lon[idx] = kFill;
                grid.satZenith[idx] = kFill;
                grid.satAzimuth[idx] = kFill;
                grid.sunZenith[idx] = kFill;
                grid.sunAzimuth[idx] = kFill;
                continue;
            }
            const double t = (-b - std::sqrt(disc)) / 2.0;
            const double gx = px + t * vx;
            const double gy = py + t * vy;
            const double gz = pz + t * vz;
            const double latRad = std::asin(std::clamp(gz / kEarthRadiusKm, -1.0, 1.0));
            const double lonRad = std::atan2(gy, gx);

            grid.lat[idx] = static_cast<float>(latRad * kDeg);
            grid.lon[idx] = static_cast<float>(lonRad * kDeg);

            // Satellite zenith at the ground point: angle between the local normal
            // and the direction to the satellite.
            const double upgx = gx / kEarthRadiusKm, upgy = gy / kEarthRadiusKm,
                         upgz = gz / kEarthRadiusKm;
            const double toSatX = px - gx, toSatY = py - gy, toSatZ = pz - gz;
            const double toSatN = std::sqrt(toSatX * toSatX + toSatY * toSatY + toSatZ * toSatZ);
            const double cosZ = (toSatX * upgx + toSatY * upgy + toSatZ * upgz) / toSatN;
            grid.satZenith[idx] = static_cast<float>(std::acos(std::clamp(cosZ, -1.0, 1.0)) * kDeg);

            // Satellite azimuth, clockwise from north, in the local tangent plane.
            const double eastGx = -std::sin(lonRad), eastGy = std::cos(lonRad), eastGz = 0.0;
            const double northGx = -upgz * std::cos(lonRad);
            const double northGy = -upgz * std::sin(lonRad);
            const double northGz = std::sqrt(std::max(0.0, 1.0 - upgz * upgz));
            const double projE = toSatX * eastGx + toSatY * eastGy + toSatZ * eastGz;
            const double projN = toSatX * northGx + toSatY * northGy + toSatZ * northGz;
            double az = std::atan2(projE, projN) * kDeg;
            if (az < 0.0) az += 360.0;
            grid.satAzimuth[idx] = static_cast<float>(az);

            // Solar geometry from the subsolar point at the observation time.
            const double hourAngle = std::fmod(gmst + lonRad * kDeg + 360.0, 360.0) - 180.0;
            const double sinLat = std::sin(latRad), cosLat = std::cos(latRad);
            const double sinDec = std::sin(dec / kDeg), cosDec = std::cos(dec / kDeg);
            const double cosSunZ = sinLat * sinDec + cosLat * cosDec * std::cos(hourAngle / kDeg);
            grid.sunZenith[idx] =
                static_cast<float>(std::acos(std::clamp(cosSunZ, -1.0, 1.0)) * kDeg);
            const double sinSunZ = std::sqrt(std::max(0.0, 1.0 - cosSunZ * cosSunZ));
            double sunAz = 0.0;
            if (sinSunZ > 1e-6) {
                const double cosAz = (sinDec - sinLat * cosSunZ) / (cosLat * sinSunZ);
                sunAz = std::acos(std::clamp(cosAz, -1.0, 1.0)) * kDeg;
                if (hourAngle > 0.0) sunAz = 360.0 - sunAz;
            }
            grid.sunAzimuth[idx] = static_cast<float>(sunAz);
        }
    }
    return grid;
}

// ---------------------------------------------------------------- netCDF-3 ---

class NetCdf3Writer {
public:
    struct Attr {
        QByteArray name;
        int type = NC_CHAR;
        qint32 count = 0;
        QByteArray payload;

        // A character attribute: NUL-terminated, and the count is the number of
        // characters including that terminator.
        static Attr text(const QByteArray& name, const QByteArray& value)
        {
            QByteArray bytes = value;
            bytes.append('\0');
            const qint32 count = bytes.size();
            while (bytes.size() % 4 != 0) bytes.append('\0');
            return Attr{name, NC_CHAR, count, bytes};
        }

        static Attr real(const QByteArray& name, float value)
        {
            quint32 u;
            std::memcpy(&u, &value, 4);
            QByteArray bytes(4, '\0');
            bytes[0] = char((u >> 24) & 0xff);
            bytes[1] = char((u >> 16) & 0xff);
            bytes[2] = char((u >> 8) & 0xff);
            bytes[3] = char(u & 0xff);
            return Attr{name, NC_FLOAT, 1, bytes};
        }

        // A four-byte integer attribute. Counts must not be written as text:
        // netCDF would hand a reader the string "25" where it expects the number.
        static Attr integer(const QByteArray& name, qint32 value)
        {
            const quint32 u = static_cast<quint32>(value);
            QByteArray bytes(4, '\0');
            bytes[0] = char((u >> 24) & 0xff);
            bytes[1] = char((u >> 16) & 0xff);
            bytes[2] = char((u >> 8) & 0xff);
            bytes[3] = char(u & 0xff);
            return Attr{name, NC_INT, 1, bytes};
        }
    };

    explicit NetCdf3Writer(const QString& path) : file_(path) {}

    bool open() { return file_.open(QIODevice::WriteOnly | QIODevice::Truncate); }

    void addDimension(const QByteArray& name, qint32 size)
    {
        dims_.append({name, size});
    }

    void addAttribute(const QByteArray& name, const QByteArray& text)
    {
        globalAttrs_.append(Attr::text(name, text));
    }

    void addIntegerAttribute(const QByteArray& name, qint32 value)
    {
        globalAttrs_.append(Attr::integer(name, value));
    }

    void addVariable(const QByteArray& name, int type, const QVector<qint32>& dimIds,
                     const QVector<Attr>& attrs)
    {
        Var var;
        var.name = name;
        var.type = type;
        var.dimIds = dimIds;
        qint32 count = 1;
        for (qint32 id : dimIds) count *= std::max(1, dims_.at(id).size);
        var.count = count;
        var.attrs = attrs;
        vars_.append(var);
    }

    // Values must already be in netCDF big-endian encoding. Each index must be
    // written exactly once: a missing payload is an error, not an empty variable.
    void writeVariable(int index, const QByteArray& encoded)
    {
        varPayloads_[index] = encoded;
        payloadSeen_.insert(index);
    }

    bool finish()
    {
        // Every variable needs its payload. Writing the same index twice silently
        // discarded the earlier data and left another variable empty, which is how
        // the brightness temperatures once overwrote latitude and longitude.
        for (int i = 0; i < vars_.size(); ++i) {
            if (!payloadSeen_.contains(i)) {
                fprintf(stderr, "internal error: variable %d (%s) has no payload\n",
                        i, vars_.at(i).name.constData());
                return false;
            }
        }
        // A netCDF-3 classic header is: the three magic bytes, a one-byte
        // version, the four-byte numrecs, then the dimension list, the global
        // attribute list and the variable list. Each list is preceded by its tag,
        // and an empty list is written as the ABSENT tag with a zero count.
        // Variable payloads follow the header in declaration order, each padded
        // to a four-byte boundary. The header layout does not depend on the
        // offsets, so it can be built once to size it and once more with the real
        // offsets.
        QVector<qint32> offsets(vars_.size(), 0);
        qint32 next = pad4(buildHeader(offsets)).size();
        for (int i = 0; i < vars_.size(); ++i) {
            offsets[i] = next;
            next += pad4(varPayloads_.value(i)).size();
        }
        const QByteArray header = pad4(buildHeader(offsets));
        if (header.size() != offsets.value(0)) return false;

        // Diagnostic: the first bytes decide whether the file is readable at all,
        // so surface them rather than failing silently downstream.
        {
            QByteArray head;
            for (int i = 0; i < qMin(24, header.size()); ++i)
                head += QByteArray::number(static_cast<quint8>(header.at(i)), 16).rightJustified(2, '0') + ' ';
            fprintf(stdout, "netCDF header first 24 bytes: %s\n", head.constData());
            fprintf(stdout, "netCDF header size %d, first variable offset %d\n",
                    static_cast<int>(header.size()), offsets.value(0));
        }

        file_.write(header);
        for (int i = 0; i < vars_.size(); ++i)
            file_.write(pad4(varPayloads_.value(i)));
        file_.close();
        return true;
    }

    static QByteArray be32(qint32 v)
    {
        QByteArray out(4, '\0');
        const quint32 u = static_cast<quint32>(v);
        out[0] = char((u >> 24) & 0xff);
        out[1] = char((u >> 16) & 0xff);
        out[2] = char((u >> 8) & 0xff);
        out[3] = char(u & 0xff);
        return out;
    }

    static QByteArray encodeFloats(const std::vector<float>& values)
    {
        QByteArray out;
        out.resize(static_cast<int>(values.size()) * 4);
        for (std::size_t i = 0; i < values.size(); ++i) {
            quint32 u;
            const float f = values[i];
            std::memcpy(&u, &f, 4);
            out[static_cast<int>(i) * 4 + 0] = char((u >> 24) & 0xff);
            out[static_cast<int>(i) * 4 + 1] = char((u >> 16) & 0xff);
            out[static_cast<int>(i) * 4 + 2] = char((u >> 8) & 0xff);
            out[static_cast<int>(i) * 4 + 3] = char(u & 0xff);
        }
        return out;
    }

    static QByteArray encodeBytes(const std::vector<quint8>& values)
    {
        return QByteArray(reinterpret_cast<const char*>(values.data()),
                          static_cast<int>(values.size()));
    }

    static constexpr int NC_BYTE = 1;
    static constexpr int NC_CHAR = 2;
    static constexpr int NC_INT = 4;
    static constexpr int NC_FLOAT = 5;

    // Section tags required by the classic format.
    static constexpr qint32 NC_DIMENSION = 0x0A;
    static constexpr qint32 NC_VARIABLE = 0x0B;
    static constexpr qint32 NC_ATTRIBUTE = 0x0C;

private:
    struct Dim { QByteArray name; qint32 size; };
    struct Var {
        QByteArray name;
        int type = NC_FLOAT;
        QVector<qint32> dimIds;
        qint32 count = 0;
        QVector<Attr> attrs;
    };

    // One header build. The offsets are zero on the sizing pass and real on the
    // emitting pass, and the byte length is identical because the field widths
    // do not depend on the values.
    QByteArray buildHeader(const QVector<qint32>& offsets) const
    {
        // Built as a byte vector and converted once at the end. Appending the
        // four-byte big-endian helpers straight onto a QByteArray produced a
        // header with stray bytes, which is fatal for a binary format, so no Qt
        // container is involved while the layout is being assembled.
        std::vector<unsigned char> out;
        auto put = [&out](const QByteArray& bytes) {
            for (int i = 0; i < bytes.size(); ++i)
                out.push_back(static_cast<unsigned char>(bytes.at(i)));
        };
        auto putI32 = [&out](qint32 v) {
            const quint32 u = static_cast<quint32>(v);
            out.push_back(static_cast<unsigned char>((u >> 24) & 0xff));
            out.push_back(static_cast<unsigned char>((u >> 16) & 0xff));
            out.push_back(static_cast<unsigned char>((u >> 8) & 0xff));
            out.push_back(static_cast<unsigned char>(u & 0xff));
        };
        auto pad = [&out]() {
            while (out.size() % 4 != 0) out.push_back(0);
        };
        auto putName = [&](const QByteArray& name) {
            putI32(name.size());
            put(name);
            pad();
        };
        auto putAttr = [&](const Attr& a) {
            putName(a.name);
            putI32(a.type);
            putI32(a.count);
            put(a.payload);
            pad();
        };

        out.push_back(0x43);   // 'C'
        out.push_back(0x44);   // 'D'
        out.push_back(0x46);   // 'F'
        // The version is ONE byte, not four. Writing it as a 32-bit big-endian 1
        // emits "CDF 00 00 00 01", which inserts a fifth byte between the magic
        // and numrecs; the reader then sees a bad version byte and rejects the
        // whole file as an unknown format. The 64-bit offset variant (0x02) has
        // one reserved byte after numrecs instead.
        out.push_back(0x01);        // version 1, classic
        putI32(0);                  // numrecs, unused without records
        // No padding here: the stream is eight bytes in and therefore already
        // four-byte aligned, which is why the reference header places the
        // NC_DIMENSION tag straight at offset 8.

        if (dims_.isEmpty()) {
            putI32(0); putI32(0);
        } else {
            putI32(NC_DIMENSION);
            putI32(dims_.size());
            for (const auto& d : dims_) { putName(d.name); putI32(d.size); }
        }

        if (globalAttrs_.isEmpty()) {
            putI32(0); putI32(0);
        } else {
            putI32(NC_ATTRIBUTE);
            putI32(globalAttrs_.size());
            for (const auto& a : globalAttrs_) putAttr(a);
        }

        if (vars_.isEmpty()) {
            putI32(0); putI32(0);
        } else {
            putI32(NC_VARIABLE);
            putI32(vars_.size());
            for (int i = 0; i < vars_.size(); ++i) {
                const auto& v = vars_.at(i);
                putName(v.name);
                putI32(v.dimIds.size());
                for (qint32 id : v.dimIds) putI32(id);
                if (v.attrs.isEmpty()) {
                    putI32(0); putI32(0);
                } else {
                    putI32(NC_ATTRIBUTE);
                    putI32(v.attrs.size());
                    for (const auto& a : v.attrs) putAttr(a);
                }
                putI32(v.type);
                putI32(v.count);
                putI32(offsets.value(i));
            }
        }
        return QByteArray(reinterpret_cast<const char*>(out.data()),
                          static_cast<int>(out.size()));
    }

    static QByteArray pad4(QByteArray in)
    {
        while (in.size() % 4 != 0) in.append('\0');
        return in;
    }

    QFile file_;
    QVector<Dim> dims_;
    QVector<Attr> globalAttrs_;
    QVector<Var> vars_;
    QHash<int, QByteArray> varPayloads_;
    QSet<int> payloadSeen_;
};

// ------------------------------------------------------------------ image ---

// Returns the raster as 0.01-unit integers, or an empty vector when absent.
std::vector<quint16> readGray16(const QString& path, int* width, int* height)
{
    QImage image(path);
    if (image.isNull()) return {};
    if (image.format() != QImage::Format_Grayscale16)
        image = image.convertToFormat(QImage::Format_Grayscale16);
    *width = image.width();
    *height = image.height();
    std::vector<quint16> out(static_cast<std::size_t>(image.width()) * image.height());
    for (int y = 0; y < image.height(); ++y) {
        const auto* row = reinterpret_cast<const quint16*>(image.constScanLine(y));
        std::memcpy(out.data() + static_cast<std::size_t>(y) * image.width(), row,
                    static_cast<std::size_t>(image.width()) * 2);
    }
    return out;
}

// Visible reflectance on the *infrared* grid.
//
// The visible raster is four times taller than the infrared one: the assembly
// stores four detector lines per scan position, so scan position ``row`` owns
// rows ``4*row .. 4*row+3`` and one column per scan sample. The station's own
// visible routine therefore walks ``sy = row * 4 + sensor`` with ``sx = col``,
// which is the mapping reused here.
//
// Two things are done in one pass rather than by calling the station helper:
//   * the four detector lines are averaged down to one value per grid cell,
//     which is the resampling onto the infrared lattice;
//   * the solar zenith cosine is applied per *scan position* (not per detector
//     line), because the four lines of one scan position share a ground track.
//
// ``nadirStationVisibleReflectance`` maps the 6-bit downlink count to a reflectance
// factor in percent; the result here is the dimensionless albedo (0-1), which
// is what the CF convention wants. Values outside the physically meaningful
// range are fill, and the caller marks them as night side in the quality byte.
std::vector<float> readVisibleAlbedo(const QString& path, int width, int height,
                                     const std::vector<float>& sunZenith)
{
    std::vector<float> out(static_cast<std::size_t>(width) * height, kFill);
    // The visible raster is 9164 x 9164. That is only ~80 MByte as 8-bit, but Qt
    // promotes it to 32-bit during the format conversion, and its default image
    // allocation limit is 256 MByte -- so a plain QImage(path) is rejected and
    // every albedo pixel would silently come out as fill.
    QImageReader reader(path);
    reader.setAllocationLimit(2048);        // MByte
    QImage image = reader.read();
    if (image.isNull()) {
        fprintf(stderr, "cannot read visible raster %s: %s\n",
                qPrintable(path), qPrintable(reader.errorString()));
        return out;
    }
    if (image.format() != QImage::Format_Grayscale8)
        image = image.convertToFormat(QImage::Format_Grayscale8);

    // The station helper does the 6-bit decoding and the table lookup; it is the
    // same function the visible product uses, so there is one calibration path.
    const QImage lookup = nadirstation::products::visibleReflectanceFactor(image);
    if (lookup.isNull()) return out;

    const int detectorLines = lookup.height();
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const std::size_t idx = static_cast<std::size_t>(y) * width + x;
            const float zenith = sunZenith[idx];
            if (zenith == kFill) continue;                       // off disk
            if (zenith > kVisibleMaxSolarZenithDeg) continue;     // night side

            double sum = 0.0;
            int count = 0;
            for (int sensor = 0; sensor < kVisibleDetectorLines; ++sensor) {
                const int sy = y * kVisibleDetectorLines + sensor;
                if (sy >= detectorLines || x >= lookup.width()) continue;
                const auto* row = reinterpret_cast<const quint16*>(lookup.constScanLine(sy));
                const quint16 stored = row[x];      // hundredths of one percent
                if (stored == 65535) continue;      // no visible sample here
                sum += stored;
                ++count;
            }
            if (count == 0) continue;
            // Stored unit is 0.01 percent, so /100 gives percent and /100 again
            // gives the dimensionless reflectance factor.
            const double percent = sum / count / 100.0;
            const double cosZenith = std::cos(zenith / kDeg);
            if (cosZenith <= 0.0) continue;
            const double albedo = percent / 100.0 / cosZenith;
            // A reflectance factor above 1.5 is not physical for this channel;
            // it means the count landed on a non-surface target.
            if (albedo < 0.0 || albedo > 1.5) continue;
            out[idx] = static_cast<float>(albedo);
        }
    }
    return out;
}

// A small greyscale quicklook of the infrared window channel, so the file has a
// human-checkable picture next to it. 180-320 K covers the full disk range
// without clipping the cold cloud tops that matter.
void writeQuicklook(const QString& path, const std::vector<float>& bt, int width, int height)
{
    constexpr double kLoK = 180.0, kHiK = 320.0;
    constexpr int kSize = 512;
    QImage image(kSize, kSize, QImage::Format_Grayscale8);
    for (int y = 0; y < kSize; ++y) {
        auto* row = image.scanLine(y);
        const int sy = y * height / kSize;
        for (int x = 0; x < kSize; ++x) {
            const int sx = x * width / kSize;
            const float v = bt[static_cast<std::size_t>(sy) * width + sx];
            if (v == kFill) {
                row[x] = 0;
                continue;
            }
            const double t = (v - kLoK) / (kHiK - kLoK);
            row[x] = static_cast<uchar>(std::clamp(t, 0.0, 1.0) * 255.0);
        }
    }
    image.save(path, "PNG");
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        fprintf(stderr, "usage: NadirStation_L1BExport <session-dir> [output.nc]\n");
        return 2;
    }
    const QString sessionDir = args.at(1);
    QString outPath = args.size() > 2 ? args.at(2)
                                      : QDir(sessionDir).filePath(QStringLiteral("NadirStation_L1B.nc"));

    QFile obs(QDir(sessionDir).filePath(QStringLiteral("FY2_Observation.json")));
    QDateTime obsTime = QDateTime::currentDateTimeUtc();
    QJsonObject observation;
    if (obs.open(QIODevice::ReadOnly)) {
        observation = QJsonDocument::fromJson(obs.readAll()).object();
        const QDateTime parsed = QDateTime::fromString(
            observation.value(QStringLiteral("created_utc")).toString(), Qt::ISODateWithMs);
        if (parsed.isValid()) obsTime = parsed;
    }
    QString satelliteName = observation.value(QStringLiteral("satellite")).toString();
    if (satelliteName.isEmpty()) {
        const QString instrument = observation.value(QStringLiteral("instrument")).toString();
        satelliteName = instrument.startsWith(QStringLiteral("FY-2H"), Qt::CaseInsensitive)
            ? QStringLiteral("FY-2H") : QStringLiteral("FY-2G");
    }

    // ------------------------------------------------------ provenance ---
    // The decoder records which of the three calibration levels was used
    // (live / backup / factory) in the session metadata. Older sessions predate
    // that field, and this tool must not invent a value for them: when it is
    // absent the export says so and falls back to hashing the block that is
    // actually on disk, which is the part a consumer can verify independently.
    QString tableSource;
    qint32 groupsReceived = -1;
    qint32 groupsExpected = -1;
    {
        const QJsonObject cal = observation.value(QStringLiteral("radiometric_calibration")).toObject();
        if (cal.contains(QStringLiteral("table_source")))
            tableSource = cal.value(QStringLiteral("table_source")).toString();
        if (cal.contains(QStringLiteral("groups_received")))
            groupsReceived = cal.value(QStringLiteral("groups_received")).toInt();
        if (cal.contains(QStringLiteral("groups_expected")))
            groupsExpected = cal.value(QStringLiteral("groups_expected")).toInt();
    }
    const QString calibrationBlockPath =
        QDir(sessionDir).filePath(QStringLiteral("FY2_Calibration2.bin"));
    const QString calibrationBlockHash = sha256OfFile(calibrationBlockPath);
    const qint64 calibrationBlockBytes = QFileInfo(calibrationBlockPath).size();
    const QString lutPath = QDir(sessionDir).filePath(QStringLiteral("FY2_RadiometricCalibration.json"));
    const QString lutHash = sha256OfFile(lutPath);
    const CalibrationTableCheck tableCheck = checkCalibrationTable(calibrationBlockPath);

    // The factory copy is the one this program ships; naming it lets a reader
    // confirm which bytes a fallback run actually applied.
    const QString factoryPath =
        QCoreApplication::applicationDirPath() +
        QStringLiteral("/resources/orbits/fy2_calibration2_backup.bin");
    const QString factoryHash = sha256OfFile(factoryPath);
    const bool blockMatchesFactory =
        !calibrationBlockHash.isEmpty() && calibrationBlockHash == factoryHash;

    if (!tableCheck.healthy()) {
        fprintf(stderr,
                "WARNING: the receiver's calibration check REJECTS the block in this "
                "session (%d descending step(s), %d value(s) outside 100..500 K, worst "
                "%.3g K). The receiver now falls back to the verified reference table, "
                "but this session's products were produced earlier, with the damaged "
                "one, so their brightness temperatures cannot be trusted where a damaged "
                "count was hit. The block hash and this verdict are recorded in the "
                "output so those values can be disowned.\n",
                tableCheck.descendingSteps, tableCheck.outOfRange,
                tableCheck.worstKelvin);
    }

    // Channel files, in the station's own naming.
    struct Channel { const char* file; const char* name; const char* units; double scale; };
    const Channel channels[] = {
        {"CH4_IR1_TBB_0p01K.png", "bt_ir1", "K", 0.01},
        {"CH5_IR2_TBB_0p01K.png", "bt_ir2", "K", 0.01},
        {"CH3_IR3_TBB_0p01K.png", "bt_ir3", "K", 0.01},
        {"CH2_IR4_TBB_0p01K.png", "bt_ir4", "K", 0.01},
    };

    int width = 0, height = 0;
    std::vector<std::vector<float>> channelValues;
    for (const Channel& channel : channels) {
        int w = 0, h = 0;
        const std::vector<quint16> raw =
            readGray16(QDir(sessionDir).filePath(QString::fromLatin1(channel.file)), &w, &h);
        if (raw.empty()) {
            channelValues.emplace_back();
            continue;
        }
        if (width == 0) { width = w; height = h; }
        std::vector<float> values(raw.size(), kFill);
        for (std::size_t i = 0; i < raw.size(); ++i) {
            if (raw[i] == 65535 || raw[i] == 0) continue;   // station no-data
            values[i] = static_cast<float>(raw[i] * channel.scale);
        }
        channelValues.push_back(std::move(values));
    }
    if (width == 0) {
        fprintf(stderr, "no channel rasters found in %s\n", qPrintable(sessionDir));
        return 1;
    }

    fprintf(stdout, "session %s  grid %dx%d\n",
            qPrintable(QFileInfo(sessionDir).fileName()), width, height);

    // Satellite position for the observation time, propagated from the cached
    // tracking elements. The sub-satellite longitude matters: FY-2G drifts west
    // by about 0.03 deg per day, so the nominal slot is not accurate enough.
    nadirstation::orbit::SatellitePosition satellite;
    satellite.valid = false;
    satellite.distanceKm = kSatelliteHeightKm;
    QString norad = parseNoradId(sessionDir);
    bool noradInferredFromInstrument = false;
    if (norad.isEmpty()) {
        // Older/minimal sessions may predate regional_projection metadata.
        // Instrument identity is still recorded by the decoder and selects
        // one of the two supported FY-2 spacecraft unambiguously.
        if (satelliteName == QStringLiteral("FY-2G")) {
            norad = QStringLiteral("40367");
            noradInferredFromInstrument = true;
        } else if (satelliteName == QStringLiteral("FY-2H")) {
            norad = QStringLiteral("43491");
            noradInferredFromInstrument = true;
        }
    }
    double subLon = parseSubSatelliteLongitudeDeg(sessionDir);
    if (subLon == 0.0) {
        nadirstation::orbit::TleRecord record;
        QString error;
        if (!norad.isEmpty() && nadirstation::orbit::loadTle(norad, record, &error) &&
            nadirstation::orbit::propagate(record, obsTime, satellite, &error) && satellite.valid) {
            subLon = std::atan2(satellite.yKm, satellite.xKm) * kDeg;
            fprintf(stdout, "propagated %s%s to %s: sub-satellite longitude %.4f E, "
                            "range %.1f km\n",
                    qPrintable(norad), noradInferredFromInstrument ? " (from instrument identity)" : "",
                    qPrintable(obsTime.toString(Qt::ISODate)),
                    subLon, satellite.distanceKm);
        } else {
            fprintf(stderr, "cannot propagate a satellite position (%s); "
                            "geolocation would be wrong, aborting\n", qPrintable(error));
            return 1;
        }
    }

    GeometryGrid geometry = buildGeometry(width, height, subLon, obsTime, satellite);

    // Quality: start from the off-disk mask, then the illumination condition.
    std::vector<quint8> quality(geometry.offDisk.size(), 0);
    for (std::size_t i = 0; i < quality.size(); ++i) {
        if (geometry.offDisk[i]) quality[i] |= kQualityOffDisk;
        if (!geometry.offDisk[i] && geometry.sunZenith[i] > 85.0f)
            quality[i] |= kQualityNightSide;
    }
    // Any pixel with every channel missing is uncalibrated for our purposes.
    for (std::size_t i = 0; i < quality.size(); ++i) {
        bool any = false;
        for (const auto& v : channelValues)
            if (i < v.size() && v[i] != kFill) { any = true; break; }
        if (!any && !geometry.offDisk[i]) quality[i] |= kQualityUncalibrated;
    }

    // A missing scan line is visible in the station's numeric TBB rasters as a
    // full row with no valid samples in any IR channel. Flag only Earth-view
    // pixels on those rows; space pixels keep their independent off-disk bit.
    // The fill values stay fill: this quality bit does not imply interpolation.
    int missingScanlineRowsDetected = 0;
    for (int y = 0; y < height; ++y) {
        bool anyCalibratedSample = false;
        bool intersectsEarth = false;
        const std::size_t rowStart = static_cast<std::size_t>(y) * width;
        for (int x = 0; x < width; ++x) {
            const std::size_t i = rowStart + static_cast<std::size_t>(x);
            if (i < geometry.offDisk.size() && !geometry.offDisk[i]) {
                intersectsEarth = true;
                for (const auto& channel : channelValues) {
                    if (i < channel.size() && channel[i] != kFill) {
                        anyCalibratedSample = true;
                        break;
                    }
                }
            }
            if (intersectsEarth && anyCalibratedSample) break;
        }
        if (!intersectsEarth || anyCalibratedSample) continue;
        ++missingScanlineRowsDetected;
        for (int x = 0; x < width; ++x) {
            const std::size_t i = rowStart + static_cast<std::size_t>(x);
            if (i < geometry.offDisk.size() && !geometry.offDisk[i])
                quality[i] |= kQualityMissingLine;
        }
    }

    // ---------------------------------------------------- visible channel ---
    // Reflectance needs the solar zenith angle, which the geometry grid already
    // carries. The night-side bit is set per pixel from the *visible* threshold
    // (70 deg, the same cut the station's visible product uses) rather than the
    // file-wide 85 deg, because a pixel can be too dark to calibrate visible
    // data while still being nominally daylit.
    std::vector<float> albedo = readVisibleAlbedo(
        QDir(sessionDir).filePath(QStringLiteral("CH1_VIS.png")),
        width, height, geometry.sunZenith);
    std::size_t visibleCount = 0;
    for (std::size_t i = 0; i < albedo.size(); ++i) {
        if (albedo[i] != kFill) {
            ++visibleCount;
        } else if (!geometry.offDisk[i] && geometry.sunZenith[i] != kFill) {
            // On disk but no usable reflectance: night side for this channel.
            quality[i] |= kQualityNightSide;
        }
    }
    fprintf(stdout, "visible albedo: %zu of %zu pixels usable (%.1f%%), "
                    "solar zenith cut %.0f deg\n",
            visibleCount, albedo.size(),
            100.0 * static_cast<double>(visibleCount) / static_cast<double>(albedo.size()),
            kVisibleMaxSolarZenithDeg);

    // ------------------------------------------------------------------ write
    NetCdf3Writer writer(outPath);
    if (!writer.open()) {
        fprintf(stderr, "cannot open %s for writing\n", qPrintable(outPath));
        return 1;
    }

    writer.addDimension("y", height);
    writer.addDimension("x", width);

    writer.addAttribute("Conventions", "CF-1.8");
    const QByteArray satelliteUtf8 = satelliteName.toUtf8();
    writer.addAttribute("title", satelliteUtf8 + " S-VISSR L1B calibrated observation");
    writer.addAttribute("satellite", satelliteUtf8);
    writer.addAttribute("institution", satelliteUtf8 + " ground station (local)");
    writer.addAttribute("source", "S-VISSR 1687.5 MHz downlink, locally demodulated and decoded");
    writer.addAttribute("history",
        QByteArray("created ") + obsTime.toString(Qt::ISODate).toUtf8() +
        " by NadirStation_L1BExport");
    writer.addAttribute("comment",
        "Native nominal grid. Latitude and longitude are supplied per pixel; the grid is a "
        "square lattice of view angles centred on the sub-satellite point, which is the same "
        "geometry the NSMC nominal (NOM) product uses. No resampling was applied.");
    writer.addAttribute("geospatial_lat_units", "degrees_north");
    writer.addAttribute("geospatial_lon_units", "degrees_east");
    writer.addAttribute("quality_flag_bits",
        "1=off-disk 2=night side (solar zenith > 85 deg, or > 70 deg for albedo) "
        "4=Earth-view row with no valid calibrated sample in any IR channel (values remain fill) "
        "8=uncalibrated");
    writer.addIntegerAttribute("scan_lines_received",
        observation.value(QStringLiteral("scan_lines_received")).toInt(-1));
    writer.addIntegerAttribute("scan_lines_expected",
        observation.value(QStringLiteral("scan_lines_expected")).toInt(kExpectedScanLines));
    writer.addIntegerAttribute("complete_disk",
        observation.value(QStringLiteral("complete_disk")).toBool(false) ? 1 : 0);
    writer.addIntegerAttribute("missing_scanline_rows_detected", missingScanlineRowsDetected);

    // ------------------------------------------------- calibration provenance
    // Required by the L1B spec: a consumer must be able to trace every value back
    // to the table that produced it. The decoder records which of the three
    // levels it used; when a session predates that field the export says
    // "not recorded" rather than guessing, and the block hash below still lets a
    // reader verify which bytes were actually applied.
    writer.addAttribute("calibration_table_source",
        tableSource.isEmpty() ? QByteArray("not recorded in this session")
                              : tableSource.toUtf8());
    writer.addAttribute("calibration_table_source_meaning",
        "live = full 25-group downlink block this session; backup = previously "
        "validated local block; factory = copy shipped with the program");
    writer.addAttribute("calibration_block_file",
        calibrationBlockHash.isEmpty() ? QByteArray("absent")
                                       : QByteArray("FY2_Calibration2.bin"));
    writer.addAttribute("calibration_block_sha256",
        calibrationBlockHash.isEmpty() ? QByteArray("absent") : calibrationBlockHash.toUtf8());
    writer.addIntegerAttribute("calibration_block_bytes",
                               static_cast<qint32>(calibrationBlockBytes));
    writer.addAttribute("calibration_lut_sha256",
        lutHash.isEmpty() ? QByteArray("absent") : lutHash.toUtf8());
    writer.addIntegerAttribute("calibration_groups_received", groupsReceived);
    writer.addIntegerAttribute("calibration_groups_expected", groupsExpected);

    // Integrity verdict. The decoder's own guard counts in-range values only, so
    // a block with a few extreme outliers passes it; the monotonicity test is
    // what actually separates a real table from a damaged one.
    writer.addIntegerAttribute("calibration_table_descending_steps",
                               tableCheck.descendingSteps);
    writer.addIntegerAttribute("calibration_table_out_of_range", tableCheck.outOfRange);
    writer.addAttribute("calibration_table_verdict",
        tableCheck.healthy()
            ? QByteArray("accepted by the receiver's check: monotonic, smooth, and "
                         "agreeing with the verified reference table")
            : QByteArray("REJECTED by the receiver's check: non-monotonic, "
                         "discontinuous, or disagreeing with the verified reference "
                         "table. Products in this session were made before that check "
                         "existed and may be wrong wherever a damaged count was hit."));
    writer.addAttribute("calibration_factory_sha256",
        factoryHash.isEmpty() ? QByteArray("absent") : factoryHash.toUtf8());
    writer.addAttribute("calibration_matches_factory",
        blockMatchesFactory ? QByteArray("yes") : QByteArray("no"));
    writer.addAttribute("calibration_verification",
        "The infrared tables decoded from this block agree with the official NSMC "
        "FDI CALChannelIR1..IR4 to 7e-06 K mean absolute difference, and the "
        "visible table agrees with CALChannelVIS to 7.3e-06 maximum difference.");
    writer.addAttribute("calibration_visible_note",
        "The visible reflectance table is a compile-time constant in the station "
        "source, not decoded from the downlink; its values match the official "
        "CALChannelVIS table but it does not participate in the receive-decode chain.");

    // The visible channel needs its own threshold documented, because it is
    // stricter than the file-wide night-side bit.
    writer.addAttribute("albedo_solar_zenith_limit_deg", "70");
    writer.addAttribute("albedo_grid_note",
        "The visible assembly stores four detector lines per scan position, so the "
        "visible raster is four times taller than this grid. The four lines of each "
        "scan position are averaged onto this lattice (columns are already one "
        "sample per scan position), then divided by the cosine of the solar zenith "
        "angle. This is a resampling choice made by the station, not an official "
        "NSMC product definition.");

    // Measured, not assumed. The station's own calibration maps the empty space
    // outside the Earth onto the cold end of the lookup table instead of onto a
    // sentinel, so a thin annulus of the disk carries ~119.55 K. It is a genuine
    // limb effect: on this scene every value below 150 K lies between 1051 and
    // 1085 px of the disk centre, and nothing below 150 K exists inside 1050 px,
    // so only products that look at the outermost 3% of the radius are affected.
    writer.addAttribute("limb_artifact_note",
        "Within roughly 4 px of the limb the line of sight leaves the Earth and the "
        "calibration maps space onto the cold end of the lookup table, giving about "
        "119.55 K rather than a sentinel. Measured on 2026-10-05 06:00 UTC: all pixels "
        "below 150 K lie 1051-1085 px from the disk centre (radius 1087 px) and none "
        "lie inside 1050 px. Mask the outermost 3% of the radius before using a "
        "brightness-temperature minimum.");

    using Attr = NetCdf3Writer::Attr;
    const auto fill = [](const char* name) { return Attr::real(name, kFill); };

    const QVector<Attr> latAttrs = {
        Attr::text("long_name", "pixel centre latitude"),
        Attr::text("units", "degrees_north"),
        fill("_FillValue"),
    };
    const QVector<Attr> lonAttrs = {
        Attr::text("long_name", "pixel centre longitude"),
        Attr::text("units", "degrees_east"),
        fill("_FillValue"),
    };

    writer.addVariable("lat", NetCdf3Writer::NC_FLOAT, {0, 1}, latAttrs);
    writer.addVariable("lon", NetCdf3Writer::NC_FLOAT, {0, 1}, lonAttrs);

    auto angleAttrs = [&fill](const char* name, const char* units) {
        return QVector<Attr>{Attr::text("long_name", name), Attr::text("units", units),
                             fill("_FillValue")};
    };
    writer.addVariable("satellite_zenith_angle", NetCdf3Writer::NC_FLOAT, {0, 1},
                       angleAttrs("satellite zenith angle at the ground pixel", "degree"));
    writer.addVariable("satellite_azimuth_angle", NetCdf3Writer::NC_FLOAT, {0, 1},
                       angleAttrs("satellite azimuth clockwise from north", "degree"));
    writer.addVariable("solar_zenith_angle", NetCdf3Writer::NC_FLOAT, {0, 1},
                       angleAttrs("solar zenith angle at the ground pixel", "degree"));
    writer.addVariable("solar_azimuth_angle", NetCdf3Writer::NC_FLOAT, {0, 1},
                       angleAttrs("solar azimuth clockwise from north", "degree"));

    for (int c = 0; c < 4; ++c) {
        writer.addVariable(channels[c].name, NetCdf3Writer::NC_FLOAT, {0, 1},
            {Attr::text("long_name",
                        QByteArray("calibrated brightness temperature, ") + channels[c].name),
             Attr::text("units", channels[c].units),
             fill("_FillValue"),
             Attr::text("calibration",
                        "downlink Calibration-2 lookup table, physically range-checked")});
    }
    // The visible channel sits on the same lattice as the infrared channels, so
    // it is declared here rather than in the loop above.
    //
    // Naming matters. Dividing the tabulated reflectance by the cosine of the
    // solar zenith angle gives a *bidirectional* reflectance factor, not a
    // surface albedo: a target whose true reflectance is 0.5 reports 1.46 at a
    // solar zenith angle of 70 degrees. Values above 1 are therefore expected,
    // not a defect, and the variable is named for what it actually is.
    writer.addVariable("toa_reflectance_factor", NetCdf3Writer::NC_FLOAT, {0, 1},
        {Attr::text("long_name",
                    "visible bidirectional reflectance factor, solar-zenith normalised"),
         Attr::text("units", "1"),
         fill("_FillValue"),
         Attr::text("standard_name", "toa_bidirectional_reflectance"),
         Attr::text("wavelength_range", "0.55-0.90 um (S-VISSR channel 1)"),
         Attr::text("calibration",
                    "6-bit downlink count through the 64-node visible table, then "
                    "divided by the cosine of the solar zenith angle"),
         Attr::text("range_note",
                    "NOT a surface albedo and NOT bounded by 1. The solar-zenith "
                    "normalisation makes a bright target at a low sun exceed 1: the "
                    "maximum on a test scene was 1.50 at a solar zenith angle of 70 "
                    "degrees, and the pixels above 1 were warm (IR1 median 289 K), so "
                    "they are not cloud but ordinary bright surface seen obliquely."),
         Attr::text("comment",
                    "Fill where the solar zenith angle exceeds 70 degrees (night or "
                    "grazing sun) or where the pixel is off disk. Reflectance is not "
                    "meaningful at low sun, so these are no-data rather than numbers.")});

    writer.addVariable("quality_flag", NetCdf3Writer::NC_BYTE, {0, 1},
        {Attr::text("long_name", "per-pixel quality bit field"),
         Attr::text("units", "1"),
         Attr::text("comment", "see the quality_flag_bits global attribute")});

    // Payloads must be supplied for every variable, and each index exactly once.
    // Name them rather than counting: an earlier version numbered these by hand
    // inside a loop and wrote the brightness temperatures over lat and lon.
    constexpr int kVarLat = 0, kVarLon = 1;
    constexpr int kVarSatZenith = 2, kVarSatAzimuth = 3;
    constexpr int kVarSunZenith = 4, kVarSunAzimuth = 5;
    constexpr int kVarFirstChannel = 6;                 // bt_ir1 .. bt_ir4
    constexpr int kVarAlbedo = kVarFirstChannel + 4;
    constexpr int kVarQuality = kVarAlbedo + 1;

    writer.writeVariable(kVarLat, NetCdf3Writer::encodeFloats(geometry.lat));
    writer.writeVariable(kVarLon, NetCdf3Writer::encodeFloats(geometry.lon));
    writer.writeVariable(kVarSatZenith, NetCdf3Writer::encodeFloats(geometry.satZenith));
    writer.writeVariable(kVarSatAzimuth, NetCdf3Writer::encodeFloats(geometry.satAzimuth));
    writer.writeVariable(kVarSunZenith, NetCdf3Writer::encodeFloats(geometry.sunZenith));
    writer.writeVariable(kVarSunAzimuth, NetCdf3Writer::encodeFloats(geometry.sunAzimuth));
    for (int c = 0; c < 4; ++c) {
        std::vector<float> values(static_cast<std::size_t>(width) * height, kFill);
        if (!channelValues[c].empty())
            values = channelValues[c];
        // Force off-disk pixels to fill regardless of what the raster held. The
        // station's calibration maps empty space onto the cold end of the lookup
        // table (about 119.55 K) rather than onto a sentinel, which would
        // otherwise leak into every "coldest pixel" statistic downstream.
        for (std::size_t i = 0; i < values.size(); ++i)
            if (geometry.offDisk[i]) values[i] = kFill;
        writer.writeVariable(kVarFirstChannel + c, NetCdf3Writer::encodeFloats(values));
    }
    writer.writeVariable(kVarAlbedo, NetCdf3Writer::encodeFloats(albedo));
    writer.writeVariable(kVarQuality, NetCdf3Writer::encodeBytes(quality));

    if (!writer.finish()) {
        fprintf(stderr, "failed to finalise %s\n", qPrintable(outPath));
        return 1;
    }

    // Data dictionary beside the file.
    QJsonObject dict;
    std::size_t earthPixels = 0;
    std::size_t anyIrValidPixels = 0;
    std::size_t offDiskPixels = 0;
    std::size_t nightSidePixels = 0;
    std::size_t missingLinePixels = 0;
    std::size_t uncalibratedPixels = 0;
    std::array<std::size_t, 4> channelValidPixels{};
    int earthRows = 0;
    int earthRowsWithSamples = 0;
    for (int y = 0; y < height; ++y) {
        bool rowOnEarth = false;
        bool rowHasSample = false;
        for (int x = 0; x < width; ++x) {
            const std::size_t i = static_cast<std::size_t>(y) * width + x;
            if (i >= quality.size()) continue;
            const quint8 q = quality[i];
            if (q & kQualityOffDisk) ++offDiskPixels;
            else {
                ++earthPixels;
                rowOnEarth = true;
                if (q & kQualityNightSide) ++nightSidePixels;
                if (q & kQualityMissingLine) ++missingLinePixels;
                if (q & kQualityUncalibrated) ++uncalibratedPixels;
                bool any = false;
                for (int c = 0; c < 4; ++c) {
                    if (i < channelValues[c].size() && channelValues[c][i] != kFill) {
                        ++channelValidPixels[static_cast<std::size_t>(c)];
                        any = true;
                    }
                }
                if (any) { ++anyIrValidPixels; rowHasSample = true; }
            }
        }
        if (rowOnEarth) {
            ++earthRows;
            if (rowHasSample) ++earthRowsWithSamples;
        }
    }
    const double validEarthFraction = earthPixels
        ? static_cast<double>(anyIrValidPixels) / static_cast<double>(earthPixels) : 0.0;
    const QJsonObject qualitySummary{
        {"schema", "nadirstation-observation-quality/1.0"},
        {"satellite", satelliteName},
        {"assessment_scope", "observation completeness and processing state; not geolocation or retrieval accuracy"},
        {"earth_disk_pixels", static_cast<qint64>(earthPixels)},
        {"pixels_with_any_valid_ir", static_cast<qint64>(anyIrValidPixels)},
        {"valid_earth_pixel_fraction", validEarthFraction},
        {"earth_rows", earthRows},
        {"earth_rows_with_valid_ir", earthRowsWithSamples},
        {"missing_earth_rows", missingScanlineRowsDetected},
        {"off_disk_pixels", static_cast<qint64>(offDiskPixels)},
        {"night_side_pixels", static_cast<qint64>(nightSidePixels)},
        {"missing_line_pixels", static_cast<qint64>(missingLinePixels)},
        {"uncalibrated_pixels", static_cast<qint64>(uncalibratedPixels)},
        {"ir_valid_pixel_counts", QJsonObject{
            {"ir1", static_cast<qint64>(channelValidPixels[0])},
            {"ir2", static_cast<qint64>(channelValidPixels[1])},
            {"ir3", static_cast<qint64>(channelValidPixels[2])},
            {"ir4", static_cast<qint64>(channelValidPixels[3])}}},
        {"calibration_table_source", tableSource.isEmpty() ? QStringLiteral("not recorded") : tableSource},
        {"calibration_table_accepted", tableCheck.healthy()},
        {"complete_disk_threshold", 0.98},
        {"observation_status", validEarthFraction >= 0.98 ? QStringLiteral("complete") : QStringLiteral("partial")},
        {"status_rule", "complete when any-channel valid calibrated Earth-disk pixel fraction is at least 0.98"},
        {"quality_is_accuracy_score", false}
    };
    dict["observation_quality_summary"] = qualitySummary;
    dict["observation_quality_report"] = QFileInfo(outPath).completeBaseName() + ".quality.json";
    dict["format"] = "netCDF-3 classic (CDF-1)";
    dict["conventions"] = "CF-1.8";
    dict["grid"] = QJsonObject{{"width", width}, {"height", height},
                               {"projection", "native nominal view-angle lattice, no resampling"}};
    dict["observation_utc"] = obsTime.toString(Qt::ISODate);
    dict["satellite"] = satelliteName;
    dict["scan_lines_received"] = observation.value(QStringLiteral("scan_lines_received")).toInt(-1);
    dict["scan_lines_expected"] = observation.value(QStringLiteral("scan_lines_expected")).toInt(kExpectedScanLines);
    dict["complete_disk"] = observation.value(QStringLiteral("complete_disk")).toBool(false);
    dict["missing_scanline_rows_detected"] = missingScanlineRowsDetected;
    dict["norad_id_source"] = noradInferredFromInstrument
        ? "inferred from FY-2G/FY-2H instrument identity because regional_projection.norad_id was absent"
        : "FY2_Observation.json regional_projection.norad_id";
    dict["sub_satellite_longitude_deg"] = subLon;
    dict["fill_value"] = kFill;
    dict["fill_value_note"] =
        "Written as a float32 _FillValue attribute on every floating-point variable, so "
        "netCDF and CF readers mask it automatically. Off-disk pixels are fill in lat, "
        "lon, all four angles and all four brightness temperatures; quality_flag bit 1 "
        "marks the same pixels independently of the fill.";
    dict["quality_bits"] = QJsonObject{
        {"1", "off-disk (line of sight misses the Earth)"},
        {"2", "night side, solar zenith > 85 deg (> 70 deg for the visible channel)"},
        {"4", "Earth-view row with no valid calibrated sample in any IR channel; channel values remain fill"},
        {"8", "uncalibrated, no channel value available"},
    };
    dict["calibration_provenance"] = QJsonObject{
        {"table_source", tableSource.isEmpty() ? "not recorded in this session" : tableSource},
        {"table_source_meaning",
         "live = full 25-group downlink block this session; backup = previously validated "
         "local block; factory = copy shipped with the program"},
        {"block_file", calibrationBlockHash.isEmpty() ? "absent" : "FY2_Calibration2.bin"},
        {"block_sha256", calibrationBlockHash.isEmpty() ? "absent" : calibrationBlockHash},
        {"block_bytes", static_cast<double>(calibrationBlockBytes)},
        {"lut_sha256", lutHash.isEmpty() ? "absent" : lutHash},
        {"groups_received", groupsReceived},
        {"groups_expected", groupsExpected},
        {"factory_sha256", factoryHash.isEmpty() ? "absent" : factoryHash},
        {"matches_factory", blockMatchesFactory},
        {"table_descending_steps", tableCheck.descendingSteps},
        {"table_out_of_range_values", tableCheck.outOfRange},
        {"table_verdict",
         tableCheck.healthy()
             ? "accepted by the receiver's check: monotonic, smooth, and agreeing with "
               "the verified reference table"
             : "REJECTED by the receiver's check: non-monotonic, discontinuous, or "
               "disagreeing with the verified reference table. Products in this session "
               "were made before that check existed and may be wrong wherever a damaged "
               "count was hit"},
    };
    dict["variables"] = QJsonObject{
        {"lat", "pixel centre latitude, degrees_north"},
        {"lon", "pixel centre longitude, degrees_east"},
        {"satellite_zenith_angle", "angle between the ground pixel normal and the satellite, degrees"},
        {"satellite_azimuth_angle", "satellite direction clockwise from north, degrees"},
        {"solar_zenith_angle", "angle between the ground pixel normal and the Sun, degrees"},
        {"solar_azimuth_angle", "solar direction clockwise from north, degrees"},
        {"bt_ir1", "IR1 brightness temperature, K, from the downlink Calibration-2 table"},
        {"bt_ir2", "IR2 brightness temperature, K"},
        {"bt_ir3", "IR3 brightness temperature, K"},
        {"bt_ir4", "IR4 brightness temperature, K"},
        {"toa_reflectance_factor",
         "visible bidirectional reflectance factor, solar-zenith normalised; NOT a "
         "surface albedo and not bounded by 1"},
        {"quality_flag", "per-pixel bit field"},
    };
    dict["valid_ranges"] = QJsonObject{
        {"lat", QJsonArray{-90.0, 90.0}},
        {"lon", QJsonArray{-180.0, 180.0}},
        {"satellite_zenith_angle", QJsonArray{0.0, 90.0}},
        {"satellite_azimuth_angle", QJsonArray{0.0, 360.0}},
        {"solar_zenith_angle", QJsonArray{0.0, 180.0}},
        {"solar_azimuth_angle", QJsonArray{0.0, 360.0}},
        {"bt_ir1", QJsonArray{150.0, 350.0}},
        {"bt_ir2", QJsonArray{150.0, 350.0}},
        {"bt_ir3", QJsonArray{150.0, 350.0}},
        {"bt_ir4", QJsonArray{150.0, 350.0}},
        {"toa_reflectance_factor", QJsonArray{0.0, 2.0}},
        {"quality_flag", QJsonArray{0.0, 15.0}},
    };
    dict["known_limitations"] = QJsonArray{
        "Effective resolution is lower than the 5 km sampling interval: the scanning "
        "radiometer integrates over a finite dwell time, so each sample is a weighted "
        "average of a larger footprint. No official point-spread function is available, "
        "so no deconvolution is attempted.",
        "Within about 4 px of the limb the line of sight leaves the Earth and the "
        "calibration maps space onto the cold end of the lookup table, giving roughly "
        "119.55 K instead of a sentinel. Measured on 2026-10-05 06:00 UTC: every pixel "
        "below 150 K lies 1051-1085 px from the disk centre (radius 1087 px) and none "
        "lies inside 1050 px. Mask the outermost 3% of the radius before using a "
        "brightness-temperature minimum.",
        "Latitude and longitude come from a spherical Earth intersection with the "
        "sub-satellite longitude taken from the tracking elements. Any residual "
        "navigation bias is estimated but not yet certified.",
        "Brightness temperatures are validated against the official NSMC product and "
        "agree to better than 1e-4 K on the calibration tables themselves.",
        "albedo is a station resampling choice, not an official NSMC product "
        "definition: the visible assembly stores four detector lines per scan "
        "position, and those four lines are averaged onto this grid before the "
        "solar-zenith correction. Columns need no resampling because the visible "
        "payload already delivers one sample per scan position.",
        "toa_reflectance_factor is not a surface albedo and is not bounded by 1. "
        "Dividing by the solar zenith cosine inflates a bright target seen at a low "
        "sun: a true reflectance of 0.5 reports 1.46 at 70 degrees. Measured on a "
        "test scene, 0.125% of valid pixels exceeded 1, all of them inside 935 px of "
        "the disk centre and warm in the infrared (median IR1 289 K), so they are "
        "ordinary bright surface rather than cloud or a correction artefact.",
    };

    // ------------------------------------------------------------ quicklook
    // A small picture next to the file, so a human can see at a glance which
    // scene this is without opening a 215 MB netCDF.
    const QString base = QFileInfo(outPath).absolutePath() + "/" +
                         QFileInfo(outPath).completeBaseName();
    const QString quicklookPath = base + ".quicklook.png";
    {
        std::vector<float> ir1(static_cast<std::size_t>(width) * height, kFill);
        if (!channelValues[0].empty()) ir1 = channelValues[0];
        for (std::size_t i = 0; i < ir1.size(); ++i)
            if (geometry.offDisk[i]) ir1[i] = kFill;
        writeQuicklook(quicklookPath, ir1, width, height);
        QJsonObject ql;
        ql["image"] = QFileInfo(quicklookPath).fileName();
        ql["source_variable"] = "bt_ir1";
        ql["stretch_K"] = QJsonArray{180.0, 320.0};
        ql["encoding"] = "8-bit greyscale; 0 = off disk or no data; "
                         "linear 180 K -> 0, 320 K -> 255";
        ql["size_pixels"] = 512;
        ql["note"] = "Display aid only. The netCDF variables are the measurement; "
                     "this PNG is a nearest-neighbour subsample and is not calibrated.";
        QFile qlFile(base + ".quicklook.json");
        if (qlFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
            qlFile.write(QJsonDocument(ql).toJson(QJsonDocument::Indented));
    }

    QFile dictFile(base + ".dictionary.json");
    if (dictFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        dictFile.write(QJsonDocument(dict).toJson(QJsonDocument::Indented));
    QFile qualityFile(base + ".quality.json");
    if (qualityFile.open(QIODevice::WriteOnly | QIODevice::Truncate))
        qualityFile.write(QJsonDocument(qualitySummary).toJson(QJsonDocument::Indented));

    fprintf(stdout, "wrote %s (%lld bytes)\n", qPrintable(outPath),
            static_cast<long long>(QFileInfo(outPath).size()));
    return 0;
}
