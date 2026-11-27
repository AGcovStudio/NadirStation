#pragma once
#include <array>
#include <QDateTime>
#include <QImage>
#include <QString>
#include <QVector>
#include "orbit.h"

namespace nadirstation::products {

// Runtime switch for the 3x3 median filters below (Settings page). On by default at every start;
// NADIR_NO_MEDIAN=1 in the environment also turns it off (used by the SNR comparison runs).
void setMedianFilterEnabled(bool enabled);
bool medianFilterEnabled();

// Rebuilds row y of a grey image from the good rows `top` (above) and `bot` (below), t = 0..1 being how
// far y lies from top towards bot: matching structure along the row, grain restored.
void rebuildRowEdgeAware(QImage& img, int y, int top, int bot, float t);

// detectNoisyRows also removes scan lines that are not blank but full of impulse noise (a sector that
// failed badly), counted in noisyRows; it is meant for the decoded channels, not for colour products.
QImage repairMissingLines(const QImage& input, int maxGap = 8, int* repairedRows = nullptr,
                          bool detectNoisyRows = false, int* noisyRows = nullptr);
// Detect short internal blank or strongly anomalous scan rows in calibrated
// TBB / categorical L2 rasters and fill from adjacent valid rows.
QImage repairMissingLinesMedian(const QImage& input, int maxGap = 2,
                                int* repairedRows = nullptr,
                                int gray16ValidMinimum = 15000,
                                int gray16ValidMaximum = 35000);
QImage medianBlur(const QImage& input, int kernelSize = 3, int* filteredPixels = nullptr);
// Format-preserving 3x3 median filter for calibrated, categorical and RGB products.
QImage medianBlurPreservingFormat(const QImage& input, int kernelSize = 3,
                                  int* filteredPixels = nullptr,
                                  bool preserveWhiteNoData = false,
                                  quint16 gray16NoData = 65535);
QImage repairImpulseNoise(const QImage& input, int radius = 1, int threshold = 22,
                          int* repairedPixels = nullptr);
QImage destripeConservative(const QImage& input, float z = 4.5f);
struct ScanNavigation;
// `navigation` (optional, valid only for a full 2291 x 2291 infrared lattice) places the sea/land mask from
// the TLE; `geographyReport` receives a one-line note on what was used.
QImage falseColor(const QImage& visible, const QImage& ir4, const QImage& ir1 = QImage(),
                  const ScanNavigation* navigation = nullptr, QString* geographyReport = nullptr);
// Calibrated FY-2G visible reflectance factor (%): accepts the native 6-bit
// DN in Grayscale16, or reverses this application's legacy 8-bit scaling.
QImage visibleReflectanceFactor(const QImage& visible);
// Reflectance factor corrected for the solar zenith angle, so a scene keeps the
// same value at a different local solar time. out = reflectance / cos(sza),
// clamped at a 70 degree zenith angle; night pixels are no-data (65535).
QImage visibleReflectanceNormalised(const QImage& visible, double subSatelliteLongitudeDeg,
                                    const QDateTime& observationUtc);
QImage visibleReflectanceEnhancement(const QImage& visible);
QImage visibleReflectanceColor(const QImage& visible);
// Convenience overloads that fall back to the bundled per-spacecraft scan
// calibration when the caller does not pass a TLE-derived satellite position.
// FY-2G and FY-2H both drift (about -0.03 and -0.014 deg/day), so the slot
// longitude is only a fallback; callers that know the observation time should
// pass a propagated SatellitePosition instead.
QImage chinaGeostationaryReflectanceFactor(const QImage& visible, double centerLatitudeDeg,
                                           double centerLongitudeDeg,
                                           double subSatelliteLongitudeDeg = 99.2);
// Calibrated S-VISSR scan geometry for the geostationary reprojection.
//
// The nominal instrument lattice is 2,291 scan positions across a view angle of
// asin(R_earth / R_orbit), i.e. about 1.4e-4 rad per sample, and its centre sits
// on nadir. Fitting the assembled 2,291 x 2,291 infrared grid against official
// NSMC nominal products shows the true angular step is a few tenths of a percent
// larger and the scan origin is offset by a small number of pixels, so both are
// carried explicitly instead of being assumed.
// Earth disk measured on the 2,291 x 2,291 IR1 brightness-temperature raster (infrared pixels).
// The limb is the sub-pixel crossing of `thresholdKelvin` walking in from space along every other
// row and column; an ellipse with a horizontal shear is fitted with iterative 3-sigma rejection:
//     ((col - cx - shear*(row - cy)) / rx)^2 + ((row - cy) / ry)^2 = 1
// The shear is real: every S-VISSR line starts a little further west than the one before
// (about -0.013 px per line on FY-2G, varying smoothly through the day), so a pure centre offset
// misplaces the north and south of the disk by up to +-14 px.
struct DiskGeometry {
    bool valid = false;
    double cx = 0.0, cy = 0.0;      // disk centre = Earth-centre direction (infrared pixels)
    double rx = 0.0, ry = 0.0;      // limb semi-axes (infrared pixels)
    double shear = 0.0;             // column shift per line
    double rmsPx = 0.0;             // RMS limb residual of the accepted points
    int points = 0;                 // accepted limb points
    double thresholdKelvin = 190.0;
};
DiskGeometry measureDiskGeometry(const QImage& ir1Tbb01K, double thresholdKelvin = 190.0);

// Per-observation S-VISSR navigation (the "spin-scan" model). Built by navigateObservation().
//   * satellite position for every scan line from the TLE (line times from the end of the image);
//   * scan frame: up = satellite direction, spin axis = orbit normal, east = north x up;
//   * scan angles -> infrared pixel through the measured disk (centre, shear, per-axis step
//     asin(a/R)/rx and asin(b/R)/ry);
//   * a small affine residual in infrared pixels, fitted to coastlines (offsets, step scale of the
//     190 K limb against the surface, ~0.14 deg spin-axis misalignment).
// Validation (9 daytime FY-2G scenes, visible channel, Natural-Earth land mask): held-out-scene
// RMS 1.36 px, max 3.2 px; independent infrared check within about 1 px except 1-3 px (row) in the
// far north where the infrared land/sea contrast is weak.
struct ScanNavigation {
    bool valid = false;
    DiskGeometry disk;
    // Satellite state at evenly spaced knots over the scan (linear interpolation in between).
    QVector<nadirstation::orbit::SatellitePosition> knots;
    double knotFirstLine = 1.0;     // 1-based infrared scan line of knots.first()
    double knotLineStep = 1.0;
    int scanLineMin = 1;
    int scanLineMax = 2291;
    // observed pixel = model pixel + k, k = c0 + c1*(col-1145) + c2*(row-1145)   (infrared px)
    std::array<double, 3> residualRow{{0.0, 0.0, 0.0}};
    std::array<double, 3> residualCol{{0.0, 0.0, 0.0}};
    QDateTime scanStartUtc;
    QDateTime scanEndUtc;           // time of scanLineMax
    double linePeriodS = 0.6;
    QString summary;                // human-readable provenance for sidecars/logs
};

struct ScanGeometry {
    // angular step = nominalStep * stepScale
    double stepScale = 1.0;
    // Scan-origin offset in source-product pixels (row positive is south).
    double residualRowPx = 0.0;
    double residualColPx = 0.0;
    // When valid, every projection uses this per-observation navigation instead of the legacy
    // lattice above and ignores the SatellitePosition argument except for its validity.
    ScanNavigation navigation;
};

// Default scan geometry from resources/orbits/nadirstation_scan_calibration.json; the
// nominal lattice when the resource is absent or the satellite is unknown.
ScanGeometry scanGeometryFor(const QString& noradId);

// Full navigation of one decoded S-VISSR observation folder (FY2_Observation.json +
// CH4_IR1_TBB_0p01K.png). Falls back to scanGeometryFor() -- and says so in `report` -- when the
// manifest, the infrared raster, the TLE or the disk fit is unusable.
ScanGeometry navigateObservation(const QString& noradId, const QString& observationDirectory,
                                 QString* report = nullptr);

// Forward navigation: geodetic latitude/longitude (degrees) -> infrared-grid pixel (0-based,
// fractional; row counted from the first line of a full disk). False when the point is not visible.
bool navigationLatLonToPixel(const ScanNavigation& navigation, double latitudeDeg,
                             double longitudeDeg, double& row, double& col);
// Inverse navigation: infrared-grid pixel -> geodetic latitude/longitude. False off the disk.
bool navigationPixelToLatLon(const ScanNavigation& navigation, double row, double col,
                             double& latitudeDeg, double& longitudeDeg);
// Satellite state interpolated to an infrared scan line (1-based).
nadirstation::orbit::SatellitePosition navigationSatelliteAtLine(const ScanNavigation& navigation, double line);
// UTC time at which an infrared scan line (1-based) was observed.
QDateTime navigationLineTime(const ScanNavigation& navigation, double line);

// Sea-surface temperature on the full-disk infrared grid (MCSST form).
//     SST[K] = a0 + a1*T11 + a2*(T11-T12) + a3*(T11-T12)*(sec(satz)-1)
// T11/T12 are 3x3 means. Cloud screening uses satellite data only, with thresholds fixed before any
// comparison with buoys: T11 >= 271.15 K; T11 within 3 K of the warmest pixel in 11x11; 5x5 T11
// standard deviation <= 0.6 K (day) / 0.4 K (night); -0.5 <= T11-T12 <= 5 K; satellite zenith <= 60
// deg; by day (solar zenith < 80 deg) visible reflectance / cos(sza) < 8 % and its 3x3 standard
// deviation < 1 %. Pixels that pass the infrared tests but cannot be checked in the visible (night,
// twilight, no VIS) are returned as candidates only: they must also pass a reference test against
// recent daytime retrievals (done by the regional generator) before they are published.
// Validation against NOAA iQuam quality-level-5 buoys/drifters, 2026-10-04..06, leave-one-day-out:
// day n=357 bias +0.06 K RMSE 0.81 K (78 % within 1 K); night with the reference test n=26 bias
// +0.01 K RMSE 0.69 K (small sample).
struct SstCoefficients {
    double a0 = 3.475257953406574;
    double a1 = 0.966693049211565;
    double a2 = 3.1847775066182233;
    double a3 = 0.07933343443216492;
};
struct SstRetrieval {
    QImage sst01K;   // Grayscale16, kelvin*100; 65535 = not retrieved
    QImage flags;    // Grayscale8: 0 not retrieved, 1 day (visible-screened), 2 night/twilight candidate
    qint64 dayPixels = 0;
    qint64 nightCandidates = 0;
};
// Daytime cloud mask on the full-disk infrared grid: a depth-6 decision tree (src/fy2g_cloudtree_day.inc)
// trained against the official NSMC NOMCloudClassification of five matching hourly FDI files. Uses 3x3
// means of IR1..IR4, 5x5 IR1 uniformity, 11x11/25x25 warmest-pixel differences, visible reflectance
// / cos(sza), land/sea (resources/maps/landmask.jpg) and the sun/satellite zenith angles.
// Grayscale8: 0 clear, 1 cloud, 255 not decided (solar zenith >= 80 deg, no visible channel,
// satellite zenith > 70 deg, invalid). Leave-one-scene-out agreement with the official product on
// daytime pixels: 82.1 % (HSS 0.64) against 78.6 % (HSS 0.57) for cloudDetectionFromTbb.
QImage daytimeCloudMaskFullDisk(const QImage& ir1Tbb01K, const QImage& ir2Tbb01K,
                                const QImage& ir3Tbb01K, const QImage& ir4Tbb01K,
                                const QImage& visible, const ScanNavigation& navigation);
SstRetrieval seaSurfaceTemperatureFullDisk(const QImage& ir1Tbb01K, const QImage& ir2Tbb01K,
                                           const QImage& visible, const ScanNavigation& navigation,
                                           const SstCoefficients& coefficients = {});

QImage chinaGeostationaryReflectanceFactor(const QImage& visible, double centerLatitudeDeg,
                                           double centerLongitudeDeg,
                                           const nadirstation::orbit::SatellitePosition& satellite,
                                           int sourceFirstLine = 1,
                                           const ScanGeometry& scan = {},
                                           const QDateTime& observationUtc = {},
                                           int projectedOffsetX = 0,
                                           int projectedOffsetY = 0);
QImage chinaGeostationaryProjection(const QImage& source, double centerLatitudeDeg,
                                   double centerLongitudeDeg,
                                   const nadirstation::orbit::SatellitePosition& satellite,
                                   int sourceFirstLine = 1,
                                   int projectedOffsetX = 0,
                                   int projectedOffsetY = 0,
                                   const ScanGeometry& scan = {});
// Nearest-neighbour geographic reprojection that preserves source values and
// pixel format (for numeric TBB, cloud masks/classes, and other retrievals).
QImage chinaGeostationaryDataProjection(const QImage& source, double centerLatitudeDeg,
                                        double centerLongitudeDeg,
                                        const nadirstation::orbit::SatellitePosition& satellite,
                                        int sourceFirstLine = 1,
                                        int projectedOffsetX = 0,
                                        int projectedOffsetY = 0,
                                        const ScanGeometry& scan = {});
QImage chinaGeostationaryMcirProjection(const QImage& ir1, double centerLatitudeDeg,
                                        double centerLongitudeDeg,
                                        double subSatelliteLongitudeDeg,
                                        bool sourceIsBrightnessTemperature = false);
QImage chinaGeostationaryMcirProjection(const QImage& ir1, double centerLatitudeDeg,
                                        double centerLongitudeDeg,
                                        const nadirstation::orbit::SatellitePosition& satellite,
                                        bool sourceIsBrightnessTemperature = false,
                                        int sourceFirstLine = 1,
                                        int projectedLayerOffsetX = 0,
                                        int projectedLayerOffsetY = 0);
QImage irEnhancement(const QImage& ir1, const QImage& ir2);
QImage longwaveIrColorEnhancement(const QImage& brightnessTemperature01K);
QImage cloudDetectionFromTbb(const QImage& tbb, double thresholdKelvin = 273.15);
QImage cloudDetectionFromTbb(const QImage& ir1, const QImage& ir2,
                             const QImage& ir3, const QImage& ir4);
QImage cloudTopTemperatureFromTbb(const QImage& tbb, double cloudThresholdKelvin = 273.15);
QImage cloudTopTemperatureFromTbb(const QImage& tbb, const QImage& cloudMask);
// Cloud-top pressure (hPa, 16-bit, 65535 = no data) and cloud-top height (m)
// inverted from a brightness-temperature raster through a standard-atmosphere
// temperature profile. Only pixels colder than the given threshold are treated
// as cloud; everything else is no-data because a clear-sky brightness
// temperature carries no cloud-top information.
QImage cloudTopPressureFromTbb(const QImage& brightnessTemperature01K,
                               double cloudThresholdKelvin = 273.15);
QImage cloudTopHeightFromTbb(const QImage& brightnessTemperature01K,
                             double cloudThresholdKelvin = 273.15);
// Fog / low-stratus candidate from the IR1-IR2 split-window difference, which is
// only diagnostic where cloud tops are low (warm) and the surface is dark.
// `visChannel` must be a calibrated reflectance raster (Format_Grayscale16, hundredths of a percent,
// as written by visibleReflectanceFactor); any other format is ignored, because a display image has
// no known scale. Sunlit pixels need reflectance >= 0.20 to count as fog; unlit ones use IR only.
QImage fogLowCloudFromTbb(const QImage& ir1Tbb01K, const QImage& ir2Tbb01K,
                          const QImage& visChannel = QImage());
// Land surface temperature candidate for clear-sky land pixels (> 0.05
// reflectance in the visible channel) using an IR1/IR2 split-window estimate.
// Same rule for `visChannel` as fogLowCloudFromTbb. A candidate field, not a retrieval: no land mask,
// no emissivity or view-angle term.
QImage landSurfaceTemperatureProxy(const QImage& ir1Tbb01K, const QImage& ir2Tbb01K,
                                   const QImage& visChannel = QImage());
QImage cloudClassificationFromTbb(const QImage& tbb);
QImage cloudClassificationFromTbb(const QImage& ir1, const QImage& ir2,
                                  const QImage& ir3, const QImage& ir4);
// Overload that accepts a visible reflectance raster. The raster currently has NO effect: a sea/land
// split of the clear class by a visible threshold (Youden J = 0.42 against the official product) was
// described here but is not implemented, and cannot be done reliably without a land/sea mask.
QImage cloudClassificationFromTbb(const QImage& ir1, const QImage& ir2,
                                  const QImage& ir3, const QImage& ir4,
                                  const QImage& visibleReflectance);
QImage cloudFractionFromTbb(const QImage& tbb, int cellSize = 32,
                            double cloudThresholdKelvin = 273.15);
QImage cloudFractionFromMask(const QImage& cloudMask, int cellSize = 2);
// Published FY-2G CFR definition: 3x3 sliding-window cloudy-pixel fraction.
// Output is aligned to input pixels; only full-valid windows are emitted.
QImage cloudFractionSlidingFromMask(const QImage& cloudMask, int windowSize = 3);
QImage applyProcessing(const QImage& input, bool lineRepair, bool dotRepair, bool destripe);

// ---------------------------------------------------------------------------------------------
// Additional diagnostic products. All inputs are calibrated brightness temperatures in hundredths
// of a kelvin (Format_Grayscale16) on the same grid. These are CANDIDATE products built from
// conventional multispectral tests; their thresholds are collected as constants in the .cpp and are
// NOT fitted to reference data, so none is a retrieval.
// ---------------------------------------------------------------------------------------------

// Stored brightness-temperature differences are offset so they fit an unsigned raster:
//     stored = (difference_K + kBtdOffsetKelvin) * 100,   65535 = no data.
constexpr double kBtdOffsetKelvin = 100.0;

// minuend - subtrahend, e.g. IR1 - IR2 (split window) or IR3 - IR1 (water vapour minus window).
QImage brightnessTemperatureDifference(const QImage& minuend, const QImage& subtrahend);
// Diverging colour picture of a stored difference: blue below zero, white at zero, red above.
// `minKelvin` must be negative and `maxKelvin` positive; they set where the colours saturate.
QImage btdColourEnhancement(const QImage& btdRaster, double minKelvin, double maxKelvin);
// Indexed8: 0 other, 1 cold cloud top (<=233.15 K), 2 deep-convection candidate (IR1 <=218.15 K and
// WV-IRW >= -5 K), 3 overshooting-top candidate (IR1 <=208.15 K and WV-IRW >= 0 K), 255 invalid.
QImage convectiveCloudCandidates(const QImage& ir1Tbb01K, const QImage& ir3Tbb01K);
// Indexed8 cloud level from a standard-atmosphere cloud-top pressure with the ISCCP boundaries:
// 0 clear, 1 low (>680 hPa), 2 mid (440-680 hPa), 3 high (<440 hPa), 255 invalid. `cloudMask` is the
// Indexed8 mask of cloudDetectionFromTbb(ir1..ir4) (0 clear, 1 cloud, 255 invalid); without one a
// pixel is cloud when IR1 <=273.15 K, which leaves the low class nearly empty. Semi-transparent
// cloud is placed too low, as with every window-channel cloud-top height here.
QImage cloudLevelFromTbb(const QImage& tbb01K, const QImage& cloudMask = QImage());
// Grayscale16 confidence 20..100 for night-time fog / low stratus from IR1 - IR4 (3.7 um); 65535 where
// there is no candidate. Sunlit pixels are skipped when a calibrated reflectance raster
// (Grayscale16, hundredths of a percent) is given; without one the result is only meaningful at night.
QImage nightFogCandidatesFromTbb(const QImage& ir1Tbb01K, const QImage& ir4Tbb01K,
                                 const QImage& visibleReflectance = QImage());
// Colour picture of the water-vapour channel (cold / moist white-blue, warm / dry orange-brown).
QImage waterVapourColourEnhancement(const QImage& ir3Tbb01K);

}
