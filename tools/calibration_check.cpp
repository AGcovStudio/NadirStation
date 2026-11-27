// NadirStation_CalibrationCheck - judge a Calibration-2 block with the station's rules.
//
// Motivation: a real block received by this station (SHA-256 7C1993A1..., seven
// sessions) decodes to a damaged table -- 26 entries outside 100..500 K, the
// worst at -2.1e6 K, plus 109 non-monotonic steps across the four infrared
// channels -- yet the original acceptance test passed it, because that test only
// counted how many values fell in range (1014 of 1024 against a 768 threshold).
//
// This tool applies the stricter rules and reports each one separately, so the
// verdict is auditable rather than a single opaque pass/fail.
//
// Usage: NadirStation_CalibrationCheck <block.bin> [more blocks...]
//        NadirStation_CalibrationCheck --sessions <session-root>

#include "nadirstationdecoder.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>

#include <cmath>
#include <cstdio>

namespace {

QTextStream& out()
{
    static QTextStream stream(stdout);
    return stream;
}

QString sha256Of(const QByteArray& data)
{
    QCryptographicHash hash(QCryptographicHash::Algorithm::Sha256);
    hash.addData(data);
    return QString::fromLatin1(hash.result().toHex());
}

struct Verdict {
    bool decoded = false;          // passed every check
    QString reason;
    int outOfRange = 0;
    int descending = 0;
    int bigJumps = 0;
    double worstTableDeviationK = 0.0;
    int worstTableDeviationCount = -1;
    bool referenceAvailable = false;
};

// Re-implements the per-channel statistics for reporting only. The accept/reject
// decision itself comes from decodeCalibration2(), so this cannot disagree with
// what the station actually does.
Verdict inspect(const QByteArray& block)
{
    Verdict v;
    const auto* reference = nadirstation::referenceCalibrationTables();
    v.referenceAvailable = reference != nullptr;

    if (block.size() < 1280 + 4 * 4096) {
        v.reason = QStringLiteral("truncated: %1 bytes, need %2")
                       .arg(block.size()).arg(1280 + 4 * 4096);
        return v;
    }

    std::array<QVector<float>, 5> luts;
    nadirstation::parseCalibration2Tables(block, luts);

    for (int channel = 1; channel < 5; ++channel) {
        const QVector<float>& table = luts[static_cast<std::size_t>(channel)];
        for (int i = 0; i < table.size(); ++i) {
            const float value = table[i];
            if (!(std::isfinite(value) && value >= 100.0f && value <= 500.0f))
                ++v.outOfRange;
            if (i > 0) {
                const float delta = value - table[i - 1];
                if (delta < 0.0f) ++v.descending;
                if (std::fabs(delta) > 20.0f) ++v.bigJumps;
            }
        }
        if (reference) {
            const QVector<float>& ref = (*reference)[static_cast<std::size_t>(channel)];
            for (int i = 16; i < 1008; ++i) {
                const float r = ref[i];
                if (!(r >= 100.0f && r <= 500.0f)) continue;
                const double dev = std::fabs(static_cast<double>(table[i]) - r);
                if (dev > v.worstTableDeviationK) {
                    v.worstTableDeviationK = dev;
                    v.worstTableDeviationCount = i;
                }
            }
        }
    }

    std::array<QVector<float>, 5> verified;
    v.decoded = nadirstation::decodeCalibration2(block, verified);
    if (!v.decoded)
        v.reason = QStringLiteral("rejected (see the station log line for which check failed)");
    return v;
}

void report(const QString& label, const QByteArray& block)
{
    const Verdict v = inspect(block);
    out() << "\n=== " << label << " ===\n";
    out() << "  bytes              : " << block.size() << "\n";
    out() << "  sha256             : " << sha256Of(block).left(32) << "...\n";
    out() << "  out-of-range values: " << v.outOfRange
          << "   (100..500 K expected for all 4096)\n";
    out() << "  descending steps   : " << v.descending
          << "   (a genuine table has 0)\n";
    out() << "  jumps over 20 K    : " << v.bigJumps << "\n";
    if (v.referenceAvailable) {
        out() << "  worst deviation vs reference : " << QString::number(v.worstTableDeviationK, 'f', 3)
              << " K at count " << v.worstTableDeviationCount << "\n";
    } else {
        out() << "  worst deviation vs reference : (reference resource not found)\n";
    }
    out() << "  VERDICT            : " << (v.decoded ? "ACCEPTED" : "REJECTED") << "\n";
    if (!v.decoded) out() << "  reason             : " << v.reason << "\n";
    out().flush();
}

} // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QStringList args = app.arguments();
    if (args.size() < 2) {
        fprintf(stderr, "usage: NadirStation_CalibrationCheck <block.bin> [...]\n"
                        "       NadirStation_CalibrationCheck --sessions <session-root>\n");
        return 2;
    }

    const bool perSession = args.contains(QStringLiteral("--sessions"));
    out() << "reference table resource: "
          << (nadirstation::referenceCalibrationTables() ? "loaded" : "NOT FOUND (comparison disabled)")
          << "\n";
    out().flush();

    int rejected = 0, accepted = 0;
    for (int i = 1; i < args.size(); ++i) {
        const QString arg = args.at(i);
        if (arg.startsWith(QStringLiteral("--"))) continue;

        if (perSession) {
            const QDir root(arg);
            const QFileInfoList sessions =
                root.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
            for (const QFileInfo& session : sessions) {
                const QString path = session.absoluteFilePath() +
                    QStringLiteral("/FY2_Calibration2.bin");
                QFile file(path);
                if (!file.open(QIODevice::ReadOnly)) continue;
                const QByteArray block = file.readAll();
                report(session.fileName(), block);
                inspect(block).decoded ? ++accepted : ++rejected;
            }
        } else {
            QFile file(arg);
            if (!file.open(QIODevice::ReadOnly)) {
                fprintf(stderr, "cannot open %s\n", qPrintable(arg));
                continue;
            }
            const QByteArray block = file.readAll();
            report(QFileInfo(arg).fileName(), block);
            inspect(block).decoded ? ++accepted : ++rejected;
        }
    }

    out() << "\nsummary: " << accepted << " accepted, " << rejected << " rejected\n";
    out().flush();
    return rejected == 0 ? 0 : 1;
}
