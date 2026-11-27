#include "imageproducts.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QImage>
#include <QString>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc < 4) {
        std::cerr << "Usage: NadirStation_DerivedProduct input-tbb.png CODE output.png\n"
                     "CODE: CLD | CTT | CLC | CFR | LWIR\n";
        return 2;
    }
    const QImage tbb(QString::fromLocal8Bit(argv[1]));
    const QString code = QString::fromLocal8Bit(argv[2]).toUpper();
    const QString output = QString::fromLocal8Bit(argv[3]);
    if (tbb.isNull()) {
        std::cerr << "Cannot read TBB input.\n";
        return 3;
    }
    QImage result;
    if (code == "CLD") result = nadirstation::products::cloudDetectionFromTbb(tbb);
    else if (code == "CTT") result = nadirstation::products::cloudTopTemperatureFromTbb(tbb);
    else if (code == "CLC") result = nadirstation::products::cloudClassificationFromTbb(tbb);
    else if (code == "CFR") result = nadirstation::products::cloudFractionFromTbb(tbb);
    else if (code == "LWIR") result = nadirstation::products::longwaveIrColorEnhancement(tbb);
    else {
        std::cerr << "Unknown product code.\n";
        return 4;
    }
    if (result.format() == QImage::Format_Grayscale16 || result.format() == QImage::Format_Indexed8 ||
        code == QStringLiteral("CFR") || code == QStringLiteral("CLC") || code == QStringLiteral("CLD"))
        result = nadirstation::products::repairMissingLinesMedian(result, 2);
    else
        result = nadirstation::products::repairMissingLines(result, 8);
    result = nadirstation::products::medianBlurPreservingFormat(result, 3,
        nullptr, code == QStringLiteral("CFR"));
    if (result.isNull() || !result.save(output, "PNG")) {
        std::cerr << "Could not create requested derived product.\n";
        return 5;
    }
    std::cout << code.toLocal8Bit().constData() << ' ' << result.width() << 'x' << result.height()
              << " saved: " << QFileInfo(output).absoluteFilePath().toLocal8Bit().constData() << '\n';
    return 0;
}
