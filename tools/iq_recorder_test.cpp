#include "iqrecorder.h"
#include <QCoreApplication>
#include <QTemporaryDir>
#include <iostream>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QTemporaryDir temp;
    if (!temp.isValid()) return 1;
    if (QStorageInfo(temp.path()).bytesAvailable() < 1024ll * 1024ll * 1024ll) return 77;

    using nadirstation::IqRecorder;
    const auto read = [](const QString& path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray{};
    };
    const auto block = std::make_shared<std::vector<std::complex<float>>>(1024, std::complex<float>(0.25f, -0.5f));
    for (int attempt = 0; attempt < 8; ++attempt) {
        IqRecorder recorder;
        QString error;
        const auto startSecond = QDateTime::currentSecsSinceEpoch();
        if (!recorder.start(temp.path(), 2000000, 1687500000, IqRecorder::Format::WAV16, &error)) {
            std::cerr << error.toStdString() << '\n';
            return 1;
        }
        const QString first = recorder.path();
        recorder.push(block);
        recorder.stop();
        if (!recorder.takeFailure().isEmpty()) return 1;
        const QByteArray original = read(first);
        if (original.size() != 44 + 4096 || original.mid(40, 4).toHex() != "00100000") {
            std::cerr << "First WAV has the wrong data size\n";
            return 1;
        }
        if (!recorder.start(temp.path(), 2000000, 1687500000, IqRecorder::Format::WAV16, &error)) {
            std::cerr << error.toStdString() << '\n';
            return 1;
        }
        const QString second = recorder.path();
        const auto endSecond = QDateTime::currentSecsSinceEpoch();
        recorder.stop();
        if (first == second || read(first) != original || !recorder.takeFailure().isEmpty()) {
            std::cerr << "Second recording overwrote the first WAV\n";
            return 1;
        }
        if (startSecond == endSecond) return 0;
    }
    std::cerr << "Could not exercise a same-second recording restart\n";
    return 1;
}
