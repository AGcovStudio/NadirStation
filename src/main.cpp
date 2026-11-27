// NADIR Station - meteorological satellite reception and processing
// Copyright (C) 2026 AGcov (BI2QYN)
//
// This program is free software: you can redistribute it and/or modify it under the terms of the
// GNU Affero General Public License as published by the Free Software Foundation, either version 3
// of the License, or (at your option) any later version.
//
// This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
// even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
// Affero General Public License for more details: <https://www.gnu.org/licenses/>.

#include "mainwindow.h"
#include <QApplication>
#include <QFont>
#include <QStringList>
#include "runtime_logger.h"
#include "uitheme.h"
#include "loadingsplash.h"
#include <QDir>
#include <QTimer>
#include <QPixmap>
#include <algorithm>
#include <QIcon>
#include <QImage>
#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace {
// The window icon, from the IDI_ICON1 resource the executable carries (resources/icon), in the
// sizes Windows uses for the title bar, taskbar and Alt-Tab.
QIcon applicationIcon() {
    QIcon icon;
#ifdef Q_OS_WIN
    for (int size : {16, 24, 32, 48, 64, 128, 256}) {
        HICON h = static_cast<HICON>(LoadImageW(GetModuleHandleW(nullptr), L"IDI_ICON1", IMAGE_ICON, size, size, 0));
        if (!h) continue;
        const QImage image = QImage::fromHICON(h);
        DestroyIcon(h);
        if (!image.isNull()) icon.addPixmap(QPixmap::fromImage(image));
    }
#endif
    return icon;
}
}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    nadirstation::log::init();
    nadirstation::log::installCrashHandlers();
    nadirstation::log::startWatchdog();
    nadirstation::log::write(QStringLiteral("Application started"));
    QApplication::setApplicationName("NADIR_Station");
    QApplication::setOrganizationName("NADIR");
    QApplication::setApplicationVersion("2.0.0");

    nadirstation::ui::apply(app);
    if (const QIcon icon = applicationIcon(); !icon.isNull()) QApplication::setWindowIcon(icon);

    // Loading animation while the pages are built (skipped for the batch / snapshot modes).
    const bool batch = app.arguments().contains(QStringLiteral("--snapshot"))
                    || app.arguments().contains(QStringLiteral("--ggak-png"))
                    || app.arguments().contains(QStringLiteral("--space-weather-png"));
    LoadingSplash* splash = batch ? nullptr : new LoadingSplash;
    if (splash) {
        splash->show();
        splash->step(QStringLiteral("Starting…"), 0.03);
        MainWindow::startupProgress = [splash](const QString& text, double f) { splash->step(text, f); };
    }
    MainWindow w;
    MainWindow::startupProgress = nullptr;
    w.show();
    if (splash) splash->finish(&w);
    const QStringList args = app.arguments();
    const int satArg = args.indexOf(QStringLiteral("--satellite"));
    if (satArg >= 0 && satArg + 1 < args.size())
        w.selectSatelliteByName(args.at(satArg + 1));
    const int wavArg = args.indexOf(QStringLiteral("--decode-wav"));
    if (wavArg >= 0 && wavArg + 1 < args.size())
        w.runWavFileForDiagnosis(args.at(wavArg + 1), satArg < 0);   // an explicit --satellite wins
    // Developer aid: --globe-lead N shows the Globe page's N-step (N x 10 min) AI forecast.
    const int leadArg = args.indexOf(QStringLiteral("--globe-lead"));
    if (leadArg >= 0 && leadArg + 1 < args.size())
        w.setGlobeLead(args.at(leadArg + 1).toInt());
    // Developer aid: --globe-query LAT LON marks and queries a point on the Globe page.
    const int queryArg = args.indexOf(QStringLiteral("--globe-query"));
    if (queryArg >= 0 && queryArg + 2 < args.size())
        w.setGlobeQuery(args.at(queryArg + 1).toDouble(), args.at(queryArg + 2).toDouble());
    // Developer aid: --globe-image NAME shows the scanned image whose path ends with NAME.
    const int imageArg = args.indexOf(QStringLiteral("--globe-image"));
    if (imageArg >= 0 && imageArg + 1 < args.size())
        w.setGlobeImage(args.at(imageArg + 1));
    const int pageArg = args.indexOf(QStringLiteral("--page"));
    if (pageArg >= 0 && pageArg + 1 < args.size())
        w.showPageByName(args.at(pageArg + 1));
    const int addDirArg = args.indexOf(QStringLiteral("--add-dir"));
    if (addDirArg >= 0 && addDirArg + 1 < args.size())
        w.setAddFolder(args.at(addDirArg + 1));
    // Developer aid: --open-images DIR loads a folder of products as one observation.
    const int openImagesArg = args.indexOf(QStringLiteral("--open-images"));
    if (openImagesArg >= 0 && openImagesArg + 1 < args.size()) {
        const QDir d(args.at(openImagesArg + 1));
        QList<QString> files;
        for (const QString& f : d.entryList({QStringLiteral("*.png")}, QDir::Files, QDir::Name)) files << d.filePath(f);
        QTimer::singleShot(500, &w, [&w, files, d] { QMetaObject::invokeMethod(&w, "onImageReady", Q_ARG(QString, d.dirName()), Q_ARG(QList<QString>, files)); });
    }
    const int addFileArg = args.indexOf(QStringLiteral("--add-file"));
    if (addFileArg >= 0 && addFileArg + 1 < args.size())
        w.showAddFileByName(args.at(addFileArg + 1));
    // Developer aid: --ggak-cadu FILE [TAB] loads saved GGAK frames into the GGAK page.
    const int ggakArg = args.indexOf(QStringLiteral("--ggak-cadu"));
    if (ggakArg >= 0 && ggakArg + 1 < args.size()) {
        const QString tab = ggakArg + 2 < args.size() && !args.at(ggakArg + 2).startsWith(QStringLiteral("--"))
            ? args.at(ggakArg + 2) : QString();
        w.reviewGgakFrames(args.at(ggakArg + 1), tab);
    }
    // Batch export of the Products or Physical data tab, matching their PNG buttons.
    const int ggakPngArg = args.indexOf(QStringLiteral("--ggak-png"));
    if (ggakPngArg >= 0 && ggakPngArg + 2 < args.size()) {
        const QString tab = args.at(ggakPngArg + 1);
        const QString path = args.at(ggakPngArg + 2);
        QTimer::singleShot(500, &w, [&w, tab, path] {
            const bool ok = w.exportGgakPng(tab, path);
            nadirstation::log::write(QStringLiteral("GGAK %1 PNG export %2: %3")
                .arg(tab, ok ? QStringLiteral("ok") : QStringLiteral("failed"), path));
            QCoreApplication::exit(ok ? 0 : 2);
        });
    }
    // Batch export of the same report as the Space Weather tab's PNG button.
    const int weatherPngArg = args.indexOf(QStringLiteral("--space-weather-png"));
    if (weatherPngArg >= 0 && weatherPngArg + 1 < args.size()) {
        const QString path = args.at(weatherPngArg + 1);
        QTimer::singleShot(500, &w, [&w, path] {
            const bool ok = w.exportSpaceWeatherPng(path);
            nadirstation::log::write(QStringLiteral("Space weather PNG export %1: %2")
                .arg(ok ? QStringLiteral("ok") : QStringLiteral("failed"), path));
            QCoreApplication::exit(ok ? 0 : 2);
        });
    }
    const int captureArg = args.indexOf(QStringLiteral("--capture-window"));
    if (captureArg >= 0 && captureArg + 2 < args.size())
        w.scheduleCaptureWindow(args.at(captureArg + 1), args.at(captureArg + 2));
    // Developer aid: --snapshot DIR renders every page to DIR/<n>_<page>.png and exits, so the look
    // can be checked without driving the window by hand.
    const int snapArg = args.indexOf(QStringLiteral("--snapshot"));
    if (snapArg >= 0 && snapArg + 1 < args.size()) {
        const QString dir = args.at(snapArg + 1);
        const int delayArg = args.indexOf(QStringLiteral("--snapshot-delay"));
        const int delayMs = delayArg >= 0 && delayArg + 1 < args.size() ? args.at(delayArg + 1).toInt() : 2500;
        QTimer::singleShot(std::max(500, delayMs), &w, [&w, dir] {
            QDir().mkpath(dir);
            const QStringList pages{QStringLiteral("processing"), QStringLiteral("image"), QStringLiteral("add"),
                                    QStringLiteral("system"), QStringLiteral("telemetry"), QStringLiteral("settings"),
                                    QStringLiteral("ggak"), QStringLiteral("globe"), QStringLiteral("passes")};
            for (int i = 0; i < pages.size(); ++i) {
                w.showPageByName(pages.at(i));
                for (int k = 0; k < 10; ++k) QCoreApplication::processEvents();
                w.grab().save(QDir(dir).filePath(QStringLiteral("%1_%2.png").arg(i).arg(pages.at(i))));
            }
            QCoreApplication::quit();
        });
    }
    const int stressArg = args.indexOf(QStringLiteral("--stress-stop"));
    if (stressArg >= 0 && stressArg + 2 < args.size()) {
        // --stress-stop <wav> <cycles> [sources, e.g. wav,pluto,rtl]
        const QString sources = stressArg + 3 < args.size() && !args.at(stressArg + 3).startsWith(QStringLiteral("--"))
            ? args.at(stressArg + 3) : QStringLiteral("wav");
        w.runStopStress(args.at(stressArg + 1), args.at(stressArg + 2).toInt(), sources);
    }
    const int code = app.exec();
    nadirstation::log::write(QStringLiteral("Application exiting normally (code %1)").arg(code));
    nadirstation::log::flush();
    nadirstation::log::setArmed(0, false);
    nadirstation::log::setArmed(1, false);
    // The watchdog is a std::thread: it must be joined before static destruction, or its own
    // destructor would terminate the process on the way out.
    nadirstation::log::stopWatchdog();
    return code;
}
