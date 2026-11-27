#include "../src/gk2aproducts.h"

#include <QCoreApplication>
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QImageReader>
#include <QStringList>
#include <cstdint>
#include <iostream>
#include <vector>

int main(int argc, char** argv) {
    // A GUI application (no window is opened) so the colour keys on the products can carry text.
    QGuiApplication app(argc, argv);
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath() + QStringLiteral("/imageformats"));
#ifdef NADIRSTATION_QT_PLUGIN_DIR
    QCoreApplication::addLibraryPath(QStringLiteral(NADIRSTATION_QT_PLUGIN_DIR));
#endif
    const QStringList args = app.arguments();
    if (args.size() < 4) {
        std::cerr << "usage: NadirStation_GK2AMcir <full-disk-counts.png> <segment.lrit> <out-dir> "
                     "[--center-lat 20] [--center-lon 128.2] [--span 40]\n";
        return 2;
    }
    double centreLat = 20.0, centreLon = 128.2, spanDeg = 40.0;
    for (int i = 4; i + 1 < args.size(); ++i) {
        if (args[i] == QStringLiteral("--center-lat")) centreLat = args[++i].toDouble();
        else if (args[i] == QStringLiteral("--center-lon")) centreLon = args[++i].toDouble();
        else if (args[i] == QStringLiteral("--span")) spanDeg = args[++i].toDouble();
    }
    QImageReader imageReader(args[1]);
    const QImage disk = imageReader.read().convertToFormat(QImage::Format_Grayscale8);
    if (disk.isNull()) {
        std::cerr << "could not read full-disk count image: " << args[1].toStdString() << "\n";
        return 3;
    }
    QFile lrit(args[2]);
    if (!lrit.open(QIODevice::ReadOnly)) {
        std::cerr << "could not read LRIT navigation/calibration header: " << args[2].toStdString() << "\n";
        return 4;
    }
    const QByteArray bytes = lrit.readAll();
    const std::vector<std::uint8_t> raw(bytes.begin(), bytes.end());
    const auto navigation = nadirstation::gk2a::navigationFromLrit(raw);
    const auto calibration = nadirstation::gk2a::calibrationFromLrit(raw);
    if (!navigation.valid) {
        std::cerr << "no valid GEOS navigation record in LRIT; refusing limb-derived geolocation\n";
        return 5;
    }
    if (!calibration.usable()) {
        std::cerr << "no usable LRIT count-to-temperature table\n";
        return 6;
    }
    const auto geometry = nadirstation::gk2a::measureDisk(disk, navigation);
    nadirstation::gk2a::Region region;
    region.centreLat = centreLat;
    region.centreLon = centreLon;
    region.spanDeg = spanDeg;
    QDir().mkpath(args[3]);
    QStringList products = nadirstation::gk2a::renderObservationProducts(
        disk, calibration, geometry, args[3], QStringLiteral("GK2A"), region);
    const QString tbbPath = QDir(args[3]).filePath(QStringLiteral("GK2A_IR_0p01K.png"));
    QImageReader tbbReader(tbbPath);
    const QImage tbb01K = tbbReader.read().convertToFormat(QImage::Format_Grayscale16);
    if (tbb01K.isNull()) {
        std::cerr << "could not read the calibrated temperature raster needed for regional MCIR\n";
        return 8;
    }
    const QStringList regionalProducts = nadirstation::gk2a::renderRegionalProductsFromTbb(
        disk, tbb01K, geometry, args[3], QStringLiteral("GK2A"), region);
    if (regionalProducts.isEmpty()) {
        std::cerr << "could not generate georeferenced regional products\n";
        return 9;
    }
    products.append(regionalProducts);
    std::cout << "GEOS(" << navigation.subLonDeg << ") from LRIT header; CFAC="
              << navigation.cfac << " LFAC=" << navigation.lfac << " COFF="
              << navigation.coff << " LOFF=" << navigation.loff << "\n";
    for (const QString& product : products)
        std::cout << "wrote " << product.toStdString() << "\n";
    return products.isEmpty() ? 7 : 0;
}
