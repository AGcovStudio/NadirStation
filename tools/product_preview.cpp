#include "imageproducts.h"
#include "orbit.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QString>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QImageReader::setAllocationLimit(512);
    if (argc < 6) {
        std::cerr << "Usage: NadirStation_ProductPreview ir1.png output.png center-lat center-lon satellite-longitude [norad-id [source-first-line]]\n";
        return 2;
    }
    const QImage ir1(QString::fromLocal8Bit(argv[1]));
    bool latOk = false, lonOk = false, satLonOk = false;
    const double lat = QString::fromLocal8Bit(argv[3]).toDouble(&latOk);
    const double lon = QString::fromLocal8Bit(argv[4]).toDouble(&lonOk);
    const double satLon = QString::fromLocal8Bit(argv[5]).toDouble(&satLonOk);
    if (ir1.isNull() || !latOk || !lonOk || !satLonOk) {
        std::cerr << "Invalid input channel, center coordinates, or satellite longitude.\n";
        return 3;
    }
    const bool sourceIsTbb = QFileInfo(QString::fromLocal8Bit(argv[1]))
        .completeBaseName().contains(QStringLiteral("TBB_0p01K"), Qt::CaseInsensitive);
    QImage mcir;
    const QString norad = argc >= 7 ? QString::fromLocal8Bit(argv[6])
        : (satLon > 90.0 ? QStringLiteral("40367") : QStringLiteral("43491"));
    const nadirstation::orbit::SatellitePosition sat = nadirstation::orbit::nominalGeostationary(satLon, norad);
    const int firstLine = argc >= 8 ? QString::fromLocal8Bit(argv[7]).toInt() : 1;
    mcir = nadirstation::products::chinaGeostationaryMcirProjection(
        ir1, lat, lon, sat, sourceIsTbb, sourceIsTbb ? 1 : firstLine, 0, 0);
    mcir = nadirstation::products::repairMissingLines(mcir, 8);
    mcir = nadirstation::products::medianBlurPreservingFormat(mcir, 3);
    if (mcir.isNull() || !mcir.save(QString::fromLocal8Bit(argv[2]), "PNG")) {
        std::cerr << "Unable to generate MCIR projection.\n";
        return 4;
    }
    std::cout << "MCIR " << mcir.width() << 'x' << mcir.height() << "; camera subpoint "
              << (sat.valid ? sat.subpointLongitudeDeg : satLon) << " E, latitude "
              << (sat.valid ? sat.subpointLatitudeDeg : 0.0) << " N; saved "
              << QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath().toLocal8Bit().constData() << '\n';
    return 0;
}
