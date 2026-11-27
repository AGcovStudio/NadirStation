#pragma once
// ELEKTRO-L MSU-GS level-2 style products from calibrated channels.
//
// Calibration: the HRIT/LRIT prologue (the "PRO" file of each observation) carries a 1024-entry table
// per band, count -> albedo (%) for 0.6 / 0.7 / 0.9 um and brightness temperature (K) for the seven
// infrared bands. A prologue without the table (the short LRIT one) or a reception that missed it falls
// back to the table of 2026-11-15 00:00 UTC, and the products say "nominal calibration".
//
// What the products are: threshold and band-difference products on brightness temperature and albedo,
// made from what the broadcast carries (no NWP, no surface data), in the way quick-look products are
// usually done; the legends say so. Each needs its bands; one that is missing leaves the product out.
//
//   L2 Cloud-top temperature (10.7)          L2 Water vapour BT (6.4)
//   L2 Split-window BTD 10.7 - 11.9          L2 Fog / low cloud BTD 3.8 - 10.7
//   L2 Convective tops BTD 6.4 - 10.7        L2 Visible albedo (0.6)
//   L2 Cloud mask                            L2 Cloud level (high / mid / low)
//   RGB Airmass (6.4-8.0, 9.7-10.7, 6.4)     RGB Dust (11.9-10.7, 10.7-8.7, 10.7)
//   RGB Night microphysics (11.9-10.7, 10.7-3.8, 10.7)
//
// Brightness-temperature products also write a 16-bit image in 0.01 K beside them (name ending
// _TBB_0p01K) so the Images page shows the temperature under the cursor.

#include <QDateTime>
#include <QImage>
#include <QString>
#include <array>
#include <cstdint>
#include <vector>

namespace nadirstation::elektro {

struct L2Band {
    double micrometres = 0.0;
    int width = 0, height = 0;
    const std::uint16_t* counts = nullptr;   // 10-bit counts, 0 = no data
};

struct L2Output {
    QString name;    // file name part, e.g. "L2_CloudTopTemp_IR107_TBB_Enhanced"
    QImage image;
};

// The ten MSU-GS bands in order; index of the band nearest `um`, or -1 when none is within 0.3 um.
int msuBandIndex(double um);
// Calibration tables (10 x 1024), from a prologue file's bytes; false when it has none.
bool readPrologueCalibration(const std::vector<std::uint8_t>& file, std::array<std::array<float, 1024>, 10>& lut);
const std::array<std::array<float, 1024>, 10>& nominalCalibration();

std::vector<L2Output> makeL2(const std::vector<L2Band>& bands, const std::array<std::array<float, 1024>, 10>& lut,
                             bool nominal, const QString& when, const QString& service);

}  // namespace nadirstation::elektro
