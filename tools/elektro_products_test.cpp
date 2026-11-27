#include "elektroxrit.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <iostream>

using nadirstation::elektro::XritProducts;
using nadirstation::gk2a::LritFile;

namespace {
LritFile segment(int minute, int part, int total) {
    const QByteArray name = QStringLiteral("H-000-GOMS4_-GOMS4_4_____-00_7_165E-%1___-2026111500%2-C_")
        .arg(part, 6, 10, QLatin1Char('0')).arg(minute, 2, 10, QLatin1Char('0')).toLatin1();
    LritFile file;
    auto& bytes = file.bytes;
    const auto bigEndian = [&](unsigned value, int count) {
        for (int i = count - 1; i >= 0; --i)
            bytes.push_back(static_cast<std::uint8_t>(value >> (8 * i)));
    };
    const auto header = [&](unsigned type, unsigned length) { bigEndian(type, 1); bigEndian(length, 2); };
    header(0, 16);
    bigEndian(0, 1);
    bigEndian(16 + 9 + 3 + name.size() + 13, 4);
    bigEndian(0, 4);
    bigEndian(8 * 8, 4);
    header(1, 9);
    bigEndian(8, 1);
    bigEndian(4, 2);
    bigEndian(2, 2);
    bigEndian(0, 1);
    header(4, 3 + name.size());
    bytes.insert(bytes.end(), name.begin(), name.end());
    header(128, 13);
    bigEndian(4, 2);
    bigEndian(1, 1);
    bigEndian(part, 2);
    bigEndian(1, 2);
    bigEndian(total, 2);
    bigEndian(0, 1);
    file.dataOffset = bytes.size();
    for (int i = 1; i <= 8; ++i) bytes.push_back(static_cast<std::uint8_t>(i * 20));
    return file;
}
}

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    XritProducts products(QStringLiteral("HRIT"));
    products.submit(segment(0, 1, 2), temp.path());
    products.submit(segment(1, 1, 1), temp.path());
    products.submit(segment(2, 1, 1), temp.path());
    products.submit(segment(0, 2, 2), temp.path());
    products.finalize();

    const auto update = products.take();
    for (int minute = 0; minute < 3; ++minute) {
        const QString stamp = QStringLiteral("20261115_000%1").arg(minute);
        auto it = update.completed.find(stamp);
        if (it == update.completed.end() || it->second.size() != 1 || !QFileInfo::exists(it->second.constFirst()) ||
            it->second.constFirst().endsWith(QStringLiteral("_partial.png")) != (minute == 0)) {
            std::cerr << "Missing or misclassified image for " << stamp.toStdString() << '\n';
            return 1;
        }
    }
    const QDir raw(temp.filePath(QStringLiteral("ELEKTRO-L/HRIT/2026-11-15/xrit")));
    if (products.skippedImages() != 1 || products.archiveFailures() != 0 || raw.entryList(QDir::Files).size() != 4) {
        std::cerr << "Late segment was not archived independently of its skipped image\n";
        return 1;
    }
    // A non-directory root must be reported as an archive failure.
    QFile blocked(temp.filePath(QStringLiteral("file-instead-of-folder")));
    if (!blocked.open(QIODevice::WriteOnly)) return 1;
    blocked.close();
    products.submit(segment(3, 1, 1), blocked.fileName());
    products.finalize();
    return products.archiveFailures() == 1 ? 0 : 1;
}
