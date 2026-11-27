// Preview the coloured MCIR composite from a full-disk scene.
//
// The plain MCIR draws every cloud in the same neutral grey, which is what its own description
// says is not enough: the product exists to "differentiate some cloud types from land in
// single-band infrared imagery", and one grey cannot do that. This tool exercises
// chinaGeostationaryMcirColour(), which keeps the basemap and the transparency model and colours
// the cloud by family using the channels that can tell the families apart.
//
// Inputs are the full-disk channel products this project writes:
//
//     CH4_IR1_TBB_0p01K.png   anchor: brightness temperature in hundredths of a kelvin
//     CH5_IR2_TBB_0p01K.png   split window, for optical thickness
//     CH3_IR3_TBB_0p01K.png   water vapour, for height
//     CH1_VIS.png             visible, for daylight texture (optional; absent at night)
//
// Usage: NadirStation_MCIRColour <scene-dir> <out.png> <centerLat> <centerLon> <satLon>
//                        [--satellite height-km] [--no-visible]

#include "../src/imageproducts.h"
#include "../src/mcircolour.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>

namespace {

QImage loadChannel(const QString& dir, const QString& name) {
    const QString path = QDir(dir).filePath(name);
    if (!QFileInfo::exists(path)) return {};
    // The visible product is assembled at 9164x9164, which needs more than Qt's default 256 MB
    // image allocation once decoded, so the reader is allowed a larger budget. Decoding it at the
    // infrared grid size directly would be better still, but the target size is only known by the
    // caller, so the resize happens there.
    QImageReader::setAllocationLimit(2048);
    QImage image(path);
    return image;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 6) {
        std::cout << "usage: NadirStation_MCIRColour <scene-dir> <out.png> <centerLat> <centerLon> "
                     "<satLon> [--satellite height-km] [--no-visible]\n";
        return 2;
    }
    const QString sceneDir = QString::fromLocal8Bit(argv[1]);
    const QString outPath = QString::fromLocal8Bit(argv[2]);
    bool latOk = false, lonOk = false, satOk = false;
    const double lat = QString::fromLocal8Bit(argv[3]).toDouble(&latOk);
    const double lon = QString::fromLocal8Bit(argv[4]).toDouble(&lonOk);
    const double satLon = QString::fromLocal8Bit(argv[5]).toDouble(&satOk);
    if (!latOk || !lonOk || !satOk) {
        std::cerr << "centre coordinates and satellite longitude must be numbers\n";
        return 2;
    }
    double heightKm = 35786.0;
    bool useVisible = true;
    for (int i = 6; i < argc; ++i) {
        const QString a = QString::fromLocal8Bit(argv[i]);
        if (a == QStringLiteral("--satellite") && i + 1 < argc)
            heightKm = QString::fromLocal8Bit(argv[++i]).toDouble();
        else if (a == QStringLiteral("--no-visible"))
            useVisible = false;
    }

    const QImage ir1 = loadChannel(sceneDir, QStringLiteral("CH4_IR1_TBB_0p01K.png"));
    const QImage ir2 = loadChannel(sceneDir, QStringLiteral("CH5_IR2_TBB_0p01K.png"));
    const QImage ir3 = loadChannel(sceneDir, QStringLiteral("CH3_IR3_TBB_0p01K.png"));
    QImage visible;
    if (useVisible) {
        visible = loadChannel(sceneDir, QStringLiteral("CH1_VIS.png"));
        // The visible product is assembled four times finer than the infrared grid, so it is
        // brought to the same lattice before it is used as a texture weight.
        if (!visible.isNull() && !ir1.isNull() && visible.size() != ir1.size())
            visible = visible.scaled(ir1.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    if (ir1.isNull()) {
        std::cerr << "CH4_IR1_TBB_0p01K.png not found in " << sceneDir.toLocal8Bit().constData()
                  << '\n';
        return 3;
    }
    std::cout << "channels: IR1 " << ir1.width() << 'x' << ir1.height()
              << (ir2.isNull() ? ", no IR2" : ", IR2 ok")
              << (ir3.isNull() ? ", no IR3" : ", IR3 ok")
              << (visible.isNull() ? ", no visible" : ", visible ok") << '\n';

    // Use the nominal geostationary position and calibrated legacy scan lattice for the preview.
    const QString norad = satLon > 90.0 ? QStringLiteral("40367") : QStringLiteral("43491");
    const nadirstation::orbit::SatellitePosition sat = nadirstation::orbit::nominalGeostationary(satLon, norad);

    const QImage colour = nadirstation::products::chinaGeostationaryMcirColour(
        ir1, ir2, ir3, visible, lat, lon, sat, /*sourceIsBrightnessTemperature=*/true,
        /*sourceFirstLine=*/1, /*projectedLayerOffsetX=*/0, /*projectedLayerOffsetY=*/0);

    if (colour.isNull()) {
        std::cerr << "the composite could not be produced; the basemap is the usual cause, and it "
                     "lives at <app-dir>/resources/maps/world_relief.jpg\n";
        return 4;
    }
    QImage repaired = nadirstation::products::repairMissingLines(colour, 8);
    if (!repaired.save(outPath, "PNG")) {
        std::cerr << "could not write " << outPath.toLocal8Bit().constData() << '\n';
        return 5;
    }
    std::cout << "wrote " << repaired.width() << 'x' << repaired.height() << " to "
              << QFileInfo(outPath).absoluteFilePath().toLocal8Bit().constData() << '\n';
    return 0;
}
