// Coloured MCIR composite. See mcircolour.h for why the cloud is coloured by type.
//
// The projection step in imageproducts.cpp converts its input to Grayscale8 before resampling.
// For a brightness-temperature product stored in hundredths of a kelvin that is a truncation, not
// a scaling: 28000 becomes 109. The existing MCIR routine avoids the problem by reducing the TBB to
// an eight-bit field BEFORE projecting, and this does the same. Each channel is therefore turned
// into a small feature image first, in the units the colouring needs, and only then resampled.

#include "mcircolour.h"

#include <QCoreApplication>
#include <QImageReader>
#include <algorithm>
#include <cmath>
#include <functional>

namespace nadirstation::products {
namespace {

constexpr int kWidth = 1360, kHeight = 840;
constexpr double kLonHalfSpan = 34.0, kLatHalfSpan = 21.0;
constexpr QRgb kOutsideDisk = qRgb(4, 12, 24);

// Brightness temperature in kelvin from a TBB image, or NaN where the pixel carries no
// measurement. Handling both the sixteen-bit products this project writes and an eight-bit input
// keeps the function usable from the viewer, which may hand over either.
double kelvinAt(const QImage& image, int x, int y) {
    if (image.format() == QImage::Format_Grayscale16) {
        const auto* row = reinterpret_cast<const quint16*>(image.constScanLine(y));
        const quint16 v = row[x];
        // 0 is the unfilled sentinel of the full-disk TBB rasters and 65535 the one the regional
        // rasters use; neither is a temperature (65535 would read as 655 K).
        return (v == 0 || v == 65535) ? std::nan("") : v / 100.0;
    }
    const int g = qGray(image.pixel(x, y));
    return g == 0 ? std::nan("") : g;
}

// Reduce an image to an eight-bit field using a caller-supplied mapping, so the projection that
// follows resamples a value that is already in its final scale.
QImage reduceTo8(const QImage& source, const std::function<int(int, int)>& value) {
    QImage out(source.size(), QImage::Format_Grayscale8);
    for (int y = 0; y < source.height(); ++y) {
        auto* dst = out.scanLine(y);
        for (int x = 0; x < source.width(); ++x)
            dst[x] = static_cast<uchar>(std::clamp(value(x, y), 0, 255));
    }
    return out;
}

QImage loadBasemap(double centerLatitudeDeg, double centerLongitudeDeg) {
    const double west = std::clamp(centerLongitudeDeg - kLonHalfSpan, -180.0, 180.0);
    const double east = std::clamp(centerLongitudeDeg + kLonHalfSpan, -180.0, 180.0);
    const double south = std::clamp(centerLatitudeDeg - kLatHalfSpan, -90.0, 90.0);
    const double north = std::clamp(centerLatitudeDeg + kLatHalfSpan, -90.0, 90.0);
    const QString path = QCoreApplication::applicationDirPath() +
                         QStringLiteral("/resources/maps/world_relief.jpg");
    constexpr QSize world(7200, 3600);
    const QRect clip(
        qBound(0, qRound((west + 180.0) / 360.0 * world.width()), world.width() - 1),
        qBound(0, qRound((90.0 - north) / 180.0 * world.height()), world.height() - 1),
        qMax(1, qRound((east - west) / 360.0 * world.width())),
        qMax(1, qRound((north - south) / 180.0 * world.height())));
    QImageReader reader(path);
    reader.setScaledSize(world);
    reader.setScaledClipRect(clip);
    QImage map = reader.read();
    if (map.size() == world) map = map.copy(clip);
    if (map.isNull()) {
        QImageReader fb(path);
        const QSize original = fb.size();
        if (!original.isEmpty()) {
            fb.setClipRect(QRect(
                qBound(0, qRound((west + 180.0) / 360.0 * original.width()), original.width() - 1),
                qBound(0, qRound((90.0 - north) / 180.0 * original.height()), original.height() - 1),
                qMax(1, qRound((east - west) / 360.0 * original.width())),
                qMax(1, qRound((north - south) / 180.0 * original.height()))));
            fb.setScaledSize(QSize(kWidth, kHeight));
            map = fb.read();
        }
    }
    if (map.isNull()) return {};
    if (map.size() != QSize(kWidth, kHeight))
        map = map.scaled(kWidth, kHeight, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    return map.convertToFormat(QImage::Format_RGB32);
}

}  // namespace

QImage chinaGeostationaryMcirColour(const QImage& ir1,
                                    const QImage& ir2,
                                    const QImage& ir3,
                                    const QImage& visible,
                                    double centerLatitudeDeg,
                                    double centerLongitudeDeg,
                                    const nadirstation::orbit::SatellitePosition& satellite,
                                    bool sourceIsBrightnessTemperature,
                                    int sourceFirstLine,
                                    int projectedLayerOffsetX,
                                    int projectedLayerOffsetY) {
    if (ir1.isNull() || !satellite.valid) return {};

    // Fixed thermal scale in kelvin. A per-scene stretch would move the colour that means "deep
    // convection" from one day to the next and make two images incomparable.
    constexpr double kTbbWarm = 305.0;
    constexpr double kTbbCold = 200.0;

    const auto kelvinOf = [&](const QImage& img) {
        return [&img](int x, int y) { return kelvinAt(img, x, y); };
    };
    auto k1 = kelvinOf(ir1);
    auto k2 = kelvinOf(ir2);
    auto k3 = kelvinOf(ir3);

    // Coverage: 0 at the warm end, 1 at the cold end.
    const QImage coverage = reduceTo8(ir1, [&](int x, int y) {
        const double t = k1(x, y);
        if (std::isnan(t)) return 0;
        const double c = (kTbbWarm - t) / (kTbbWarm - kTbbCold);
        return static_cast<int>(std::lround(std::clamp(c, 0.0, 1.0) * 255.0));
    });

    // Thickness proxy: the split-window difference. Both channels go cold over thick cloud, but the
    // difference between them grows with the optical depth, which is what separates a convective
    // tower from a thin cirrus shield that is equally cold in the window channel alone.
    QImage thickness;
    if (!ir2.isNull()) {
        thickness = reduceTo8(ir1, [&](int x, int y) {
            const double a = k1(x, y), b = k2(x, y);
            if (std::isnan(a)) return 0;
            if (std::isnan(b)) return 128;      // no second channel: sit in the middle
            // Only a genuinely deep cloud has a large split-window difference. A lower threshold
            // here, as an earlier version used, painted most mid-level cloud warm and the image
            // read as if the whole scene were convective.
            const double d = std::clamp((a - b - 1.2) / 2.6, 0.0, 1.0);
            return static_cast<int>(std::lround(d * 255.0));
        });
    }

    // Highness proxy: the water-vapour channel responds to the upper troposphere, so a large
    // IR1-IR3 difference means the cloud sits up where IR3 is also cold.
    QImage highness;
    if (!ir3.isNull()) {
        highness = reduceTo8(ir1, [&](int x, int y) {
            const double a = k1(x, y), c = k3(x, y);
            if (std::isnan(a)) return 0;
            if (std::isnan(c)) return 0;
            const double h = std::clamp((a - c - 6.0) / 26.0, 0.0, 1.0);
            return static_cast<int>(std::lround(h * 255.0));
        });
    }

    const auto project = [&](const QImage& src) {
        if (src.isNull()) return QImage{};
        return chinaGeostationaryProjection(src, centerLatitudeDeg, centerLongitudeDeg, satellite,
                                            sourceFirstLine, projectedLayerOffsetX,
                                            projectedLayerOffsetY);
    };

    const QImage covP = project(coverage);
    if (covP.isNull()) return {};
    const QImage thickP = project(thickness);
    const QImage highP = project(highness);
    const QImage visP = visible.isNull() ? QImage{} : project(visible);

    const QImage map = loadBasemap(centerLatitudeDeg, centerLongitudeDeg);
    if (map.isNull()) return {};

    QImage out(kWidth, kHeight, QImage::Format_RGB32);
    for (int y = 0; y < kHeight; ++y) {
        const auto* mapRow = reinterpret_cast<const QRgb*>(map.constScanLine(y));
        auto* dst = reinterpret_cast<QRgb*>(out.scanLine(y));
        for (int x = 0; x < kWidth; ++x) {
            const QRgb base = mapRow[x];
            const QRgb cov = covP.pixel(x, y);
            if (cov == kOutsideDisk) { dst[x] = base; continue; }

            const double c = qGray(cov) / 255.0;
            if (c <= 0.0) { dst[x] = base; continue; }
            const double alpha = std::pow(c, 1.45) * 0.97;

            const double thick = thickP.isNull() ? 0.0 : qGray(thickP.pixel(x, y)) / 255.0;
            const double high = highP.isNull() ? 0.0 : qGray(highP.pixel(x, y)) / 255.0;
            double bright = 0.0;
            if (!visP.isNull()) {
                const QRgb v = visP.pixel(x, y);
                // The visible product stores hundredths of a percent reflectance across the whole
                // sixteen-bit range, and the projection has already taken its low byte, so the
                // value here is a display figure rather than a physical one. It is used only as a
                // texture weight, which is what daylight offers and what the product needs.
                if (v != kOutsideDisk) bright = std::clamp(qGray(v) / 180.0, 0.0, 1.0);
            }

            // Colour by cloud family. The axes are independent enough to combine additively: each
            // shifts the hue toward what it identifies without cancelling the others.
            // Cloud family colours as targets to interpolate between, not offsets from white.
            //
            // Two earlier versions added and subtracted channel offsets instead, which reaches a
            // brown at full strength: subtracting from two of three channels lowers luminance, and
            // a colour only reads as bright warm cloud while its luminance stays high. The result
            // looked like terrain under haze. Every target below is bright, so no blend of them
            // can produce mud.
            //
            //   low cloud          near-white, very slightly cool
            //   deep convection    bright warm amber -- the signal a forecaster looks for first
            //   cirrus             pale cyan: high, thin, cold
            constexpr double kLowR = 0xE8, kLowG = 0xEC, kLowB = 0xF2;
            constexpr double kConvR = 0xFF, kConvG = 0xC2, kConvB = 0x6E;
            constexpr double kCirrR = 0xC6, kCirrG = 0xE6, kCirrB = 0xF0;

            double cr = kLowR, cg = kLowG, cb = kLowB;
            // Deep convection first: it is the strongest statement the image makes.
            if (thick > 0.0) {
                cr = cr * (1.0 - thick) + kConvR * thick;
                cg = cg * (1.0 - thick) + kConvG * thick;
                cb = cb * (1.0 - thick) + kConvB * thick;
            }
            // Then cirrus, weighted by how thin it is: a thick high cloud is convection with an
            // anvil and has already been coloured warm, so it must not be pulled back to cyan.
            const double cirrus = high * (1.0 - 0.70 * thick);
            if (cirrus > 0.0) {
                cr = cr * (1.0 - cirrus) + kCirrR * cirrus;
                cg = cg * (1.0 - cirrus) + kCirrG * cirrus;
                cb = cb * (1.0 - cirrus) + kCirrB * cirrus;
            }
            // Daylight texture brightens the cloud without changing its family.
            if (bright > 0.0) {
                const double boost = 0.10 * bright * (1.0 - high);
                cr += (255.0 - cr) * boost;
                cg += (255.0 - cg) * boost;
                cb += (255.0 - cb) * boost;
            }
            // The coldest tops desaturate toward pure white, so the strongest signal is also the
            // brightest and the image still reads the way a visible picture of it would.
            const double whiten = std::clamp((c - 0.80) / 0.20, 0.0, 1.0);
            cr += (255.0 - cr) * whiten;
            cg += (255.0 - cg) * whiten;
            cb += (255.0 - cb) * whiten;

            const double w = std::clamp(alpha, 0.0, 1.0);
            dst[x] = qRgb(
                qBound(0, qRound(qRed(base) * (1.0 - w) + std::clamp(cr, 0.0, 255.0) * w), 255),
                qBound(0, qRound(qGreen(base) * (1.0 - w) + std::clamp(cg, 0.0, 255.0) * w), 255),
                qBound(0, qRound(qBlue(base) * (1.0 - w) + std::clamp(cb, 0.0, 255.0) * w), 255));
        }
    }
    return out;
}

}  // namespace nadirstation::products
