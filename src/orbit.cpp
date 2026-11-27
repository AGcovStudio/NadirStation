#include "orbit.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QTextStream>
#include <QtGlobal>
#include <cmath>
#include <exception>

#include "SGP4.h"
#include "Tle.h"

namespace nadirstation::orbit {
namespace {
constexpr double kPi = 3.14159265358979323846;

// Age of a TLE in days at `utc`, from the epoch in line 1 (columns 19-32: two-digit year and
// fractional day of year). Plain arithmetic on seconds since the Unix epoch, so it does not depend
// on time-zone APIs. Returns 0 when the line cannot be read.
double tleAgeDays(const QString& line1, const QDateTime& utc) {
    if (line1.size() < 32) return 0.0;
    bool yearOk = false, dayOk = false;
    const int yy = line1.mid(18, 2).toInt(&yearOk);
    const double dayOfYear = line1.mid(20, 12).trimmed().toDouble(&dayOk);
    if (!yearOk || !dayOk || dayOfYear < 1.0) return 0.0;
    const int year = yy < 57 ? 2000 + yy : 1900 + yy;
    const qint64 daysToYearStart = QDate(1970, 1, 1).daysTo(QDate(year, 1, 1));
    const double epochSecs = (static_cast<double>(daysToYearStart) + (dayOfYear - 1.0)) * 86400.0;
    return (utc.toMSecsSinceEpoch() / 1000.0 - epochSecs) / 86400.0;
}

QString satelliteLabel(const QString& noradId) {
    return noradId == QStringLiteral("40367") ? QStringLiteral("FENGYUN 2G")
        : noradId == QStringLiteral("43491") ? QStringLiteral("FENGYUN 2H")
                                              : QString();
}
}

QString preferredTleFilePath() {
    const QString writable = QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation))
        .filePath(QStringLiteral("fy2.tle"));
    if (QFileInfo::exists(writable)) return writable;
    return QCoreApplication::applicationDirPath() + QStringLiteral("/resources/orbits/fy2.tle");
}

bool loadTle(const QString& noradId, TleRecord& record, QString* error, const QString& explicitPath) {
    const QString path = explicitPath.isEmpty() ? preferredTleFilePath() : explicitPath;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        if (error) *error = QStringLiteral("Cannot open TLE file: %1").arg(path);
        return false;
    }
    QTextStream in(&file);
    QString name, line1, line2;
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty()) continue;
        if (line.startsWith(QLatin1Char('1')) && line1.isEmpty()) {
            line1 = line;
        } else if (line.startsWith(QLatin1Char('2')) && !line1.isEmpty()) {
            line2 = line;
            if (line1.size() >= 7 && line1.mid(2, 5).trimmed() == noradId) {
                record = {name.isEmpty() ? satelliteLabel(noradId) : name, line1, line2, noradId};
                return true;
            }
            name.clear(); line1.clear(); line2.clear();
        } else if (line.startsWith(QLatin1Char('0'))) {
            name = line.mid(1).trimmed();
        } else if (!line.startsWith(QLatin1Char('1')) && !line.startsWith(QLatin1Char('2'))) {
            name = line;
        }
    }
    if (error) *error = QStringLiteral("TLE for NORAD %1 was not found in %2").arg(noradId, path);
    return false;
}

bool propagate(const TleRecord& record, const QDateTime& utc,
               SatellitePosition& result, QString* error) {
    if (!utc.isValid() || record.line1.isEmpty() || record.line2.isEmpty()) {
        if (error) *error = QStringLiteral("Invalid UTC time or empty TLE");
        return false;
    }
    try {
        const QDateTime time = utc.toUTC();
        const QDate date = time.date();
        const double dayOfYear = date.dayOfYear() +
            (time.time().hour() * 3600.0 + time.time().minute() * 60.0 +
             time.time().second() + time.time().msec() / 1000.0) / 86400.0;
        const libsgp4::DateTime epochTime(static_cast<unsigned int>(date.year()), dayOfYear);
        const libsgp4::Tle tle(record.name.toStdString(), record.line1.toStdString(), record.line2.toStdString());
        const libsgp4::SGP4 propagator(tle);
        const libsgp4::Eci eci = propagator.FindPosition(epochTime);
        const libsgp4::Vector inertial = eci.Position();

        // Vallado/SGP4 position is TEME. Rotate it into Earth-fixed coordinates
        // at this exact observation epoch using the library's GST implementation.
        const double gst = eci.GetDateTime().ToGreenwichSiderealTime();
        const double c = std::cos(gst), s = std::sin(gst);
        result.xKm = inertial.x * c + inertial.y * s;
        result.yKm = -inertial.x * s + inertial.y * c;
        result.zKm = inertial.z;
        result.distanceKm = std::sqrt(result.xKm * result.xKm + result.yKm * result.yKm + result.zKm * result.zKm);
        // Orbit normal r x v in TEME, rotated with the same GST. TEME velocity (not the Earth-fixed
        // one) is used so the normal is the inertial angular-momentum direction, i.e. the spin axis.
        const libsgp4::Vector velocity = eci.Velocity();
        double hx = inertial.y * velocity.z - inertial.z * velocity.y;
        double hy = inertial.z * velocity.x - inertial.x * velocity.z;
        double hz = inertial.x * velocity.y - inertial.y * velocity.x;
        const double hn = std::sqrt(hx * hx + hy * hy + hz * hz);
        if (hn > 0.0 && std::isfinite(hn)) {
            hx /= hn; hy /= hn; hz /= hn;
            result.orbitNormalX = hx * c + hy * s;
            result.orbitNormalY = -hx * s + hy * c;
            result.orbitNormalZ = hz;
            result.hasOrbitNormal = true;
        }
        if (!std::isfinite(result.distanceKm) || result.distanceKm < 10000.0) {
            if (error) *error = QStringLiteral("SGP4 returned an invalid satellite position");
            return false;
        }
        result.subpointLongitudeDeg = std::atan2(result.yKm, result.xKm) * 180.0 / kPi;
        result.subpointLatitudeDeg = std::atan2(result.zKm, std::hypot(result.xKm, result.yKm)) * 180.0 / kPi;
        result.satelliteName = record.name;
        result.noradId = record.noradId;
        result.tleEpochUtc = QString::fromStdString(tle.Epoch().ToString());
        result.tleLine1 = record.line1;
        result.tleLine2 = record.line2;
        result.propagatedAtUtc = time;
        result.tleAgeDays = tleAgeDays(record.line1, time);
        result.valid = true;
        result.fromTle = true;
        return true;
    } catch (const std::exception& ex) {
        if (error) *error = QString::fromLocal8Bit(ex.what());
        return false;
    }
}

namespace {

QVector<ScanCalibration> loadScanCalibrations() {
    QVector<ScanCalibration> out;
    const QString path = QCoreApplication::applicationDirPath() +
        QStringLiteral("/resources/orbits/nadirstation_scan_calibration.json");
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) return out;
    const QJsonObject root = QJsonDocument::fromJson(file.readAll()).object();
    const QJsonObject satellites = root.value(QStringLiteral("satellites")).toObject();
    // The scale is relative to the nominal lattice, so its default is exactly 1. (This line used to
    // read the nominal step and then choose 1.0 on both branches of a conditional.)
    const double defaultStepScale = 1.0;
    for (auto it = satellites.constBegin(); it != satellites.constEnd(); ++it) {
        const QJsonObject entry = it.value().toObject();
        ScanCalibration calibration;
        calibration.valid = true;
        calibration.noradId = it.key();
        calibration.satelliteName = entry.value(QStringLiteral("name")).toString();
        calibration.nominalSlotLongitudeDeg =
            entry.value(QStringLiteral("nominal_slot_longitude_deg")).toDouble();
        calibration.tleSubpointLongitudeDeg =
            entry.value(QStringLiteral("tle_subpoint_longitude_deg")).toDouble();
        calibration.driftDegPerDay = entry.value(QStringLiteral("drift_deg_per_day")).toDouble();
        calibration.angularStepScale =
            entry.value(QStringLiteral("fitted_angular_step_scale_k")).toDouble(defaultStepScale);
        calibration.residualRowPx =
            entry.value(QStringLiteral("fitted_residual_row_px")).toDouble(0.0);
        calibration.residualColPx =
            entry.value(QStringLiteral("fitted_residual_col_px")).toDouble(0.0);
        calibration.status = entry.value(QStringLiteral("status")).toString();
        calibration.fitReference = entry.value(QStringLiteral("fit_reference")).toString();
        // Residuals are applied only when the resource records a validated fit, or when it opts in
        // explicitly with residual_applied_pending_validation (a measured but not yet validated
        // offset). Either way the choice is written in the resource, never implied.
        calibration.residualValidated =
            entry.value(QStringLiteral("residual_validated")).toBool(false);
        calibration.residualAppliedPendingValidation =
            entry.value(QStringLiteral("residual_applied_pending_validation")).toBool(false);
        out.append(calibration);
    }
    return out;
}

} // namespace

QVector<ScanCalibration> allScanCalibrations() {
    return loadScanCalibrations();
}

ScanCalibration scanCalibrationFor(const QString& noradId) {
    const QVector<ScanCalibration> all = loadScanCalibrations();
    for (const ScanCalibration& calibration : all) {
        if (calibration.noradId == noradId) return calibration;
    }
    return {};
}

bool tleIsStale(const SatellitePosition& position, double maxAgeDays) {
    return position.fromTle && std::fabs(position.tleAgeDays) > maxAgeDays;
}

SatellitePosition nominalGeostationary(double longitudeDeg, const QString& satelliteName) {    SatellitePosition position;
    constexpr double radiusKm = 42164.0;
    const double longitude = longitudeDeg * kPi / 180.0;
    position.xKm = radiusKm * std::cos(longitude);
    position.yKm = radiusKm * std::sin(longitude);
    position.zKm = 0.0;
    position.distanceKm = radiusKm;
    position.subpointLongitudeDeg = longitudeDeg;
    position.subpointLatitudeDeg = 0.0;
    position.satelliteName = satelliteName;
    position.valid = true;
    position.fromTle = false;
    return position;
}

SatellitePosition calibrateFy2gCameraBoresight(const SatellitePosition& rawPosition) {
    if (!rawPosition.valid || rawPosition.distanceKm <= 10000.0) return rawPosition;
    // Keep the TLE-derived east/west drift, but model the S-VISSR camera as
    // Earth-pointing at the equatorial geostationary scan axis. Orbit latitude
    // is not itself the imager boresight for a station-kept/attitude-controlled
    // GEO spacecraft. Avoid scene-specific manual offsets here.
    constexpr double pi = 3.14159265358979323846;
    const double lon = rawPosition.subpointLongitudeDeg * pi / 180.0;
    SatellitePosition calibrated = rawPosition;
    calibrated.xKm = rawPosition.distanceKm * std::cos(lon);
    calibrated.yKm = rawPosition.distanceKm * std::sin(lon);
    calibrated.zKm = 0.0;
    calibrated.subpointLongitudeDeg = rawPosition.subpointLongitudeDeg;
    calibrated.subpointLatitudeDeg = 0.0;
    return calibrated;
}

} // namespace nadirstation::orbit
