#pragma once
// Helpers shared by the MainWindow source files (mainwindow*.cpp). Internal to the window.
#include "imageproducts.h"
#include "gk2aproducts.h"
#include "gk2ahritimages.h"
#include "lritadd.h"
#include "ggakpage.h"
#include "globeview.h"
#include "passpage.h"
#include "spaceweatherpage.h"
#include "orbit.h"
#include "runtime_logger.h"
#include "uikit.h"
#include <QApplication>
#include <QFileDialog>
#include <QLineEdit>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QFileInfo>
#include <QImageReader>
#include <QCursor>
#include <QLocale>
#include <QDesktopServices>
#include <QFile>
#include <QHash>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QMessageBox>
#include <QPalette>
#include <QPixmap>
#include <QPainter>
#include <QProcess>
#include <QPlainTextEdit>
#include <QSettings>
#include <QCheckBox>
#include <QScrollArea>
#include <QStorageInfo>
#include <QTextCursor>
#include <QSplitter>
#include <QSignalBlocker>
#include <QRegularExpression>
#include <QStatusBar>
#include <QPointer>
#include <QStyle>
#include <QThread>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QUrl>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QHeaderView>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QMap>
#include <QTimeZone>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QTabWidget>
#include <QTimer>
#include <algorithm>
#include <functional>
#include <random>
#include <thread>
#include <array>
#include <limits>

namespace {
inline QString observationManifestPath(const QDir& directory) {
    const QString current = directory.filePath(QStringLiteral("FY2_Observation.json"));
    if (QFileInfo::exists(current)) return current;
    return directory.filePath(QStringLiteral("MGAS_Observation.json")); // legacy observation folders
}

// The decoder stops sending statistics when the input stops, so the badge would keep its last
// colour (often green). Turn it grey and say STOPPED, keeping the last counts on the lines below.
inline void markDecodeBadgeStopped(QLabel* badge) {
    if (!badge) return;
    QStringList lines = badge->text().split(QStringLiteral("<br>"));
    if (lines.isEmpty()) lines << QString();
    lines[0] = QStringLiteral("<span style=\"color:#6f8294; font-weight:600;\">&#9679; STOPPED</span>");
    badge->setText(lines.join(QStringLiteral("<br>")));
    badge->setProperty("sync", QStringLiteral("idle"));
    badge->style()->unpolish(badge);
    badge->style()->polish(badge);
}


inline bool resolveObservationOrbit(const QString& sourcePath, const QString& timestamp,
                             const QString& noradId, bool isFullDiskRaster,
                             int scanLinesPerRasterLine,
                             nadirstation::orbit::SatellitePosition& raw,
                             nadirstation::orbit::SatellitePosition& camera,
                             int& firstRasterLine, QString& error) {
    nadirstation::orbit::TleRecord tle;
    if (!nadirstation::orbit::loadTle(noradId, tle, &error)) return false;
    int scanLineMin = 1;
    QDateTime observationUtc;
    const QString sessionDir = QFileInfo(sourcePath).absolutePath();
    QFile manifest(observationManifestPath(QDir(sessionDir)));
    if (manifest.open(QIODevice::ReadOnly | QIODevice::Text)) {
        const QJsonObject obs = QJsonDocument::fromJson(manifest.readAll()).object();
        scanLineMin = qMax(1, obs.value(QStringLiteral("scan_line_min")).toInt(1));
        observationUtc = QDateTime::fromString(obs.value(QStringLiteral("created_utc")).toString(), Qt::ISODateWithMs);
        if (!observationUtc.isValid())
            observationUtc = QDateTime::fromString(obs.value(QStringLiteral("created_utc")).toString(), Qt::ISODate);
    }
    const QString folderStamp = QFileInfo(sessionDir).fileName();
    const auto match = QRegularExpression(QStringLiteral("^(\\d{4})(\\d{2})(\\d{2})_(\\d{2})(\\d{2})(\\d{2})_(\\d{3})$")).match(folderStamp);
    if (match.hasMatch()) {
        const QString startIso = QStringLiteral("%1-%2-%3T%4:%5:%6.%7Z")
            .arg(match.captured(1), match.captured(2), match.captured(3), match.captured(4),
                 match.captured(5), match.captured(6), match.captured(7));
        const QDateTime startUtc = QDateTime::fromString(startIso, Qt::ISODateWithMs);
        if (startUtc.isValid()) observationUtc = observationUtc.isValid()
            ? startUtc.addMSecs(startUtc.msecsTo(observationUtc) / 2) : startUtc;
    }
    if (!observationUtc.isValid()) observationUtc = QDateTime::fromString(timestamp, Qt::ISODateWithMs);
    if (!observationUtc.isValid()) observationUtc = QDateTime::fromString(timestamp, Qt::ISODate);
    if (!observationUtc.isValid()) observationUtc = QDateTime::currentDateTimeUtc();
    if (!nadirstation::orbit::propagate(tle, observationUtc.toUTC(), raw, &error)) return false;
    camera = noradId == QStringLiteral("40367")
        ? nadirstation::orbit::calibrateFy2gCameraBoresight(raw) : raw;
    firstRasterLine = isFullDiskRaster ? 1 : (scanLineMin - 1) * qMax(1, scanLinesPerRasterLine) + 1;
    return true;
}
}  // namespace
