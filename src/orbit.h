#pragma once

#include <QDateTime>
#include <QString>
#include <QVector>

namespace nadirstation::orbit {

struct TleRecord {
    QString name;
    QString line1;
    QString line2;
    QString noradId;
};

struct SatellitePosition {
    // Earth-fixed, Earth-centred coordinates in kilometres.
    double xKm = 0.0;
    double yKm = 0.0;
    double zKm = 0.0;
    double distanceKm = 42164.0;
    double subpointLatitudeDeg = 0.0;
    double subpointLongitudeDeg = 0.0;
    QString satelliteName;
    QString noradId;
    QString tleEpochUtc;
    QString tleLine1;
    QString tleLine2;
    QDateTime propagatedAtUtc;
    // Age of the TLE at the propagated time, in days (positive when the TLE is older than the
    // observation). Set by propagate(); 0 for positions that did not come from a TLE.
    double tleAgeDays = 0.0;
    // Unit orbit normal (direction of r x v in the inertial frame), expressed in the same
    // Earth-fixed frame as the position. Set by propagate(). FY-2 is spin-stabilised and its spin
    // axis lies along the orbit normal, so this is the S-VISSR scan axis: with FY-2G's ~6 degree
    // inclination the image is rotated by up to that angle against geographic north, depending on
    // the orbital phase. Coastline registration of 9 scenes confirmed this (see
    // nadirstation_scan_calibration.json, "spin_scan_navigation").
    double orbitNormalX = 0.0;
    double orbitNormalY = 0.0;
    double orbitNormalZ = 1.0;
    bool hasOrbitNormal = false;
    bool valid = false;
    bool fromTle = false;
};

QString preferredTleFilePath();
bool loadTle(const QString& noradId, TleRecord& record, QString* error = nullptr,
             const QString& explicitPath = QString());
bool propagate(const TleRecord& record, const QDateTime& utc,
               SatellitePosition& result, QString* error = nullptr);
// True when the position came from a TLE older than `maxAgeDays` at the propagated time. Both FY-2
// spacecraft drift and are re-manoeuvred, so an old TLE moves every pixel of the reprojection.
bool tleIsStale(const SatellitePosition& position, double maxAgeDays = 7.0);
SatellitePosition nominalGeostationary(double longitudeDeg, const QString& satelliteName = QString());

// Per-spacecraft S-VISSR scan geometry from resources/orbits/nadirstation_scan_calibration.json.
//
// Both FY-2 spacecraft drift in longitude (FY-2G at about -0.03 deg/day, FY-2H at
// about -0.014 deg/day, i.e. several degrees per year), so the sub-satellite
// longitude used for the geostationary reprojection must come from a current TLE.
// This record carries the calibrated longitude together with the scan-origin
// residual measured against an official NOM product.
struct ScanCalibration {
    bool valid = false;
    QString noradId;
    QString satelliteName;
    double nominalSlotLongitudeDeg = 0.0;
    double tleSubpointLongitudeDeg = 0.0;
    double driftDegPerDay = 0.0;
    double angularStepScale = 1.0;
    double residualRowPx = 0.0;
    double residualColPx = 0.0;
    // True when the resource explicitly records a validated residual fit, i.e.
    // when the measured offsets may be applied to production reprojection.
    bool residualValidated = false;
    // True when the resource asks for the residual to be applied although it has NOT been validated
    // against an independent reference (for example a single-scene limb measurement). Kept apart from
    // residualValidated so the validation status in the resource stays truthful.
    bool residualAppliedPendingValidation = false;
    QString status;
    QString fitReference;
};

// Looks up the calibration for a NORAD id, reading the bundled resource on first
// use. Returns an invalid record when the resource is missing or the satellite is
// unknown, in which case callers fall back to the nominal slot longitude.
ScanCalibration scanCalibrationFor(const QString& noradId);

// Every calibration known to the bundled resource, for status reporting.
QVector<ScanCalibration> allScanCalibrations();

// LEGACY. Moves the satellite onto the equator at its TLE longitude. Coastline registration showed
// this is wrong for FY-2G (inclination ~6 deg): the real position and the orbit-normal scan axis are
// both needed (SatellitePosition::orbitNormal*, products::navigateObservation). Kept only for the
// fallback path used when an observation cannot be navigated from its own infrared disk.
SatellitePosition calibrateFy2gCameraBoresight(const SatellitePosition& rawPosition);

} // namespace nadirstation::orbit
