// The observation products rendered from the assembled GK-2A disk.
//
// An observation arrives as ten strips, each its own LRIT file and its own JPEG. Those strips are
// how the service transmits, not what a reader wants to look at: a list of ten files where each is
// one horizontal band of one picture is a list of the transmission rather than of the product. The
// strips are the raw material. What is worth showing is four pictures:
//
//     IR raw          the counts as received, unchanged. This is the measurement, and having it
//                     available unchanged is what makes the other three checkable against it.
//
//     IR enhanced     the same data with a contrast stretch. A raw IR disk spends most of its range
//                     on two narrow bands -- cold cloud tops and warm sea -- and looks flat until
//                     the range that matters is spread across the display.
//
//     MCIR            cloud coloured by height, over the surface. The multi-colour composite, which
//                     is the one that reads as weather rather than as an instrument's output.
//
//     TBB projected   the same brightness temperature on a latitude and longitude grid, which is a
//                     map. The disk is what the satellite sees; a map is what can be compared with
//                     another map, and it is the form most of the uses for this data take.
//
//     MCIR projected  the cloud-height composite on that same geographic grid. Its land mask is
//                     binary, with no country borders or labels drawn over the image.
//
// Counts to temperature is the file's own business: the annotation record carries a table of
// 255 entries, count to kelvin. Without it the pixels are instrument counts and the enhanced, MCIR
// and projected products would all be drawings of nothing. So the calibration is required rather
// than assumed, and its absence is reported.
//
// The geometry is the one the navigation record and the limb give. The record names the projection
// and the centre -- "GEOS(128.2)", column and line 1100 of 2200 -- and the scale comes from the
// limb, whose angular radius at geostationary altitude is asin(Re/(Re+h)), a fixed 8.7005 degrees.
// Measured on this data that is 1092 pixels, which puts the centre within two pixels of what the
// record says, and that agreement is the check that the reading is right.

#pragma once

#include "../src/gk2adatalink.h"

#include <QImage>
#include <QString>
#include <QStringList>
#include <array>
#include <cstdint>
#include <string>

namespace nadirstation::gk2a {

// The count-to-kelvin table from an LRIT file's annotation record, which is the calibration and the
// reason the record is transmitted at all.
struct Calibration {
    std::array<double, 256> kelvin{};
    std::array<bool, 256> valid{};
    std::string channel;
    std::string unit;
    int entries = 0;

    bool usable() const { return entries >= 16; }
};

// Reads the annotation record out of a complete LRIT file.
Calibration calibrationFromLrit(const std::vector<std::uint8_t>& bytes);

// Navigation values carried by LRIT Image Navigation (type 2) records. The scaling
// factors and offsets are signed, big-endian values as specified by KMA/CGMS.
struct LritNavigation {
    double subLonDeg = 128.2;
    std::int32_t cfac = 0;
    std::int32_t lfac = 0;
    std::int32_t coff = 0;
    std::int32_t loff = 0;
    bool valid = false;
};
LritNavigation navigationFromLrit(const std::vector<std::uint8_t>& bytes);

// Where the disk sits and how big it is, in radians per pixel.
struct DiskGeometry {
    double size = 2200.0;        // the full disk is 2200 lines by construction
    double centre = 1100.0;      // column and line of the sub-satellite point
    double radPerPixel = 0.0;    // from the limb, not from CFAC
    double subLonDeg = 128.2;    // GK-2A's station
    double limbPixels = 0.0;     // what was measured
    LritNavigation navigation;
    bool measured = false;
};

// Measures the limb on an assembled disk and derives the scale from it.
DiskGeometry measureDisk(const QImage& disk, double subLonDeg);
DiskGeometry measureDisk(const QImage& disk, const LritNavigation& navigation);

// An area to draw, as a centre and a span. The regional product is the same composite over a
// chosen area rather than the whole visible Earth, which is the form it is read in.
struct Region {
    double centreLat = 20.0;
    double centreLon = 128.2;
    double spanDeg = 40.0;
    QString name = QStringLiteral("Region");   // part of the file name, so the two do not collide
};

// Writes the display products beside each other and returns their paths.
//
// raw, enhanced, mcir and projected. Any that cannot be made is left out of the list rather than
// written as something misleading, and the reason goes to the log.
QStringList renderObservationProducts(const QImage& disk,
                                      const Calibration& calibration,
                                      const DiskGeometry& geometry,
                                      const QString& directory,
                                      const QString& stem,
                                      const Region& region = Region{});

// Rebuild selected-area products from the saved raw disk and its 0.01 K raster. This allows a
// region JSON to be applied again without receiving or decoding the satellite pass a second time.
QStringList renderRegionalProductsFromTbb(const QImage& disk,
                                          const QImage& tbb01K,
                                          const DiskGeometry& geometry,
                                          const QString& directory,
                                          const QString& stem,
                                          const Region& region);

}  // namespace nadirstation::gk2a
