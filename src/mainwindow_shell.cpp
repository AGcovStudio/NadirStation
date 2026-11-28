// The window's shell: the navigation rail, the link strip across the top, and page switching.
#include "mainwindow.h"
#include "ggakpage.h"
#include "passpage.h"
#include "decoderpage.h"
#include "runtime_logger.h"
#include "uikit.h"
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QLocale>
#include <QTimer>
#include <QTimeZone>
#include <QHBoxLayout>
#include <QPixmap>
#include <QVBoxLayout>
#include <QAction>
#include <QCloseEvent>
#include <QDesktopServices>
#include <QFileInfo>
#include <QKeySequence>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QSettings>
#include <QStatusBar>
#include <QStorageInfo>
#include <QUrl>
#include <cmath>

namespace {
// Stack positions, in the order the constructor adds the pages.
enum Page { Live = 0, Images = 1, AddData = 2, System = 3, Telemetry = 4, Settings = 5, Ggak = 6, Globe = 7, Passes = 8, Decoder = 9 };
}

QWidget* MainWindow::makeNavRail() {
    using namespace nadirstation::ui;
    auto* rail = new QWidget;
    rail->setObjectName(QStringLiteral("NavRail"));
    rail->setAttribute(Qt::WA_StyledBackground, true);
    rail->setFixedWidth(252);
    auto* v = new QVBoxLayout(rail);
    v->setContentsMargins(0, 16, 0, 0);
    v->setSpacing(0);

    // Wordmark (resources/brand, copied next to the executable); plain text if the file is missing.
    auto* title = new QLabel(QStringLiteral("NADIR STATION"));
    title->setObjectName(QStringLiteral("Wordmark"));
    {
        QPixmap mark(QDir(QCoreApplication::applicationDirPath())
                         .filePath(QStringLiteral("resources/brand/nadir_station_wordmark_ui@2x.png")));
        if (!mark.isNull()) {
            mark.setDevicePixelRatio(2.0);
            title->setPixmap(mark);
            title->setToolTip(QStringLiteral("NADIR STATION 2.1.1"));
        }
    }
    auto* brand = new QVBoxLayout;
    brand->setContentsMargins(10, 0, 10, 6);
    brand->setSpacing(4);
    brand->addWidget(title);
    v->addLayout(brand);

    const auto group = [&](const QString& name) { v->addWidget(label(name, "NavGroup")); };
    const auto item = [&](const QString& name, int page, const QString& tip) {
        auto* b = new QPushButton(name);
        b->setObjectName(QStringLiteral("NavItem"));
        b->setCheckable(true);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolTip(tip);
        b->setProperty("page", page);
        connect(b, &QPushButton::clicked, this, [this, page] { switchPage(page); });
        navButtons_ << b;
        v->addWidget(b);
    };
    group(QStringLiteral("Receive"));
    item(QStringLiteral("Live"), Live, QStringLiteral("Receiver, spectrum, constellation and decoder state"));
    item(QStringLiteral("Decoder"), Decoder, QStringLiteral("Every stage of the receive chain, live: where data is lost and why"));
    group(QStringLiteral("Products"));
    item(QStringLiteral("Images"), Images, QStringLiteral("Decoded observations, channels and regional products"));
    item(QStringLiteral("Globe"), Globe, QStringLiteral("GK-2A imagery on a globe, with the AI nowcast"));
    item(QStringLiteral("Additional data"), AddData, QStringLiteral("LRIT additional data: schedules, text and weather charts"));
    group(QStringLiteral("Telemetry"));
    item(QStringLiteral("Spacecraft telemetry"), Telemetry, QStringLiteral("FY-2 DOC, MANAM and calibration; GOES GVAR block 0"));
    item(QStringLiteral("GGAK && space weather"), Ggak, QStringLiteral("ELEKTRO-L heliogeophysical telemetry and the space-weather view"));
    group(QStringLiteral("Planning"));
    item(QStringLiteral("Passes"), Passes, QStringLiteral("Orbit prediction: pass table, sky plot, ground track, live az/el/Doppler"));
    group(QStringLiteral("Station"));
    item(QStringLiteral("System"), System, QStringLiteral("CPU, memory, disk and receiver status"));
    item(QStringLiteral("Settings"), Settings, QStringLiteral("Ground station, output folder, log, attribution"));

    v->addStretch(1);
    railClock_ = label(QStringLiteral("--:--:-- UTC"), "RailClock");
    v->addWidget(railClock_);
    v->addSpacing(4);
    v->addWidget(label(QStringLiteral("v2.1.1 · © AGcov (BI2QYN)"), "RailFoot"));
    return rail;
}

namespace {
QWidget* readout(const QString& caption, QLabel*& value, const QString& initial, int minWidth) {
    using namespace nadirstation::ui;
    auto* w = new QWidget;
    auto* l = new QHBoxLayout(w);
    l->setContentsMargins(8, 0, 8, 0);
    l->setSpacing(0);
    l->addWidget(label(caption, "ReadoutCaption"));
    value = label(initial, "ReadoutValue");
    value->setMinimumWidth(minWidth);
    l->addWidget(value);
    value->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    return w;
}
QWidget* divider() {
    auto* d = new QWidget;
    d->setObjectName(QStringLiteral("StripDivider"));
    d->setAttribute(Qt::WA_StyledBackground, true);
    d->setFixedHeight(18);
    return d;
}
}

QWidget* MainWindow::makeLinkStrip() {
    using namespace nadirstation::ui;
    auto* strip = new QWidget;
    strip->setObjectName(QStringLiteral("LinkStrip"));
    strip->setAttribute(Qt::WA_StyledBackground, true);
    strip->setFixedHeight(40);
    auto* h = new QHBoxLayout(strip);
    h->setContentsMargins(14, 0, 6, 0);
    h->setSpacing(0);

    satelliteSelector_ = new QComboBox;
    satelliteSelector_->setObjectName(QStringLiteral("SatellitePicker"));
    satelliteSelector_->addItem(QStringLiteral("Select satellite"), 0);
    satelliteSelector_->addItem(QStringLiteral("FY-2G · 99.2°E · 1687.500 MHz"), 8);
    satelliteSelector_->addItem(QStringLiteral("FY-2H · 79.0°E · 1687.500 MHz"), 9);
    satelliteSelector_->addItem(QStringLiteral("GK-2A · 128.2°E · 1692.140 MHz"), 10);
    satelliteSelector_->addItem(QStringLiteral("ELEKTRO-L GGAK · 1693.000 MHz"), 11);
    satelliteSelector_->addItem(QStringLiteral("GK-2A HRIT · 128.2°E · 1695.400 MHz"), 12);
    satelliteSelector_->addItem(QStringLiteral("GOES GVAR · GOES-N/O/P · 1685.700 MHz"), 13);
    satelliteSelector_->addItem(QStringLiteral("ELEKTRO-L LRIT · 1691.000 MHz"), 14);
    satelliteSelector_->addItem(QStringLiteral("ELEKTRO-L HRIT · 1691.000 MHz"), 15);
    satelliteSelector_->addItem(QStringLiteral("MetOp AHRPT · polar · 1701.300 MHz"), 16);
    satelliteSelector_->addItem(QStringLiteral("METEOR HRPT · polar · 1700.000 MHz"), 17);
    satelliteSelector_->addItem(QStringLiteral("GOES SD · GOES-N/O/P · 1676.000 MHz"), 18);
    satelliteSelector_->setMinimumWidth(268);
    satelliteSelector_->setMaximumWidth(400);
    satelliteSelector_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    satelliteSelector_->setToolTip(QStringLiteral(
        "Choose the satellite (and so the sub-satellite point) by hand; the projection does not use the satellite identity from the DOC.\n"
        "Choosing a satellite also presets the centre frequency, the symbol rate and the demodulator:\n"
        "  FY-2G / FY-2H 1687.500 MHz\n"
        "  GK-2A LRIT 1692.140 MHz · GK-2A HRIT 1695.400 MHz\n"
        "  ELEKTRO-L GGAK 1693.000 MHz (telemetry, 5 kBd PM/BPSK)\n"
        "  ELEKTRO-L LRIT 1691.000 MHz (BPSK 294 ksym/s) · ELEKTRO-L HRIT 1691.000 MHz (OQPSK 1.15 Msym/s)\n"
        "  GOES GVAR 1685.700 MHz · GOES SD 1676.000 MHz (sensor data: imager 2.6208 Mbit/s + sounder)"));
    h->addWidget(satelliteSelector_);
    // The service description the selection sets; shown as the picker's status tip, not as a line.
    satelliteState_ = label(QStringLiteral("Select a satellite · projection locked"), "TopTarget");
    satelliteState_->setVisible(false);
    h->addWidget(satelliteState_);
    h->addSpacing(8);

    h->addWidget(divider());
    h->addWidget(readout(QStringLiteral("Freq"), linkFreq_, QStringLiteral("—"), 96));
    // The input source is shown on the Live page itself; the readout stays for the colour it gives
    // the strip when a receiver runs, but only on wide windows.
    QWidget* inputBox = readout(QStringLiteral("Input"), linkInput_, QStringLiteral("—"), 64);
    inputBox->setObjectName(QStringLiteral("WideOnly"));
    h->addWidget(inputBox);
    h->addWidget(divider());
    h->addWidget(readout(QStringLiteral("Link"), linkState_, QStringLiteral("Idle"), 64));
    h->addWidget(divider());
    h->addWidget(readout(QStringLiteral("SNR"), linkSnr_, QStringLiteral("—"), 56));
    h->addWidget(divider());
    h->addWidget(readout(QStringLiteral("Δf"), linkCarrier_, QStringLiteral("—"), 72));
    h->addWidget(divider());
    linkNextBox_ = readout(QStringLiteral("Next"), linkNext_, QStringLiteral("—"), 92);
    linkNextBox_->setToolTip(QStringLiteral("GK-2A takes a full-disk image every 10 minutes; its segments are broadcast in the first "
                                            "two to three minutes after each slot (:00, :10, :20 …), with fill frames in between."));
    h->addWidget(linkNextBox_);
    h->addWidget(divider());
    h->addWidget(readout(QStringLiteral("REC"), linkRec_, QStringLiteral("Off"), 56));

    auto* timer = new QTimer(this);
    timer->setInterval(500);
    connect(timer, &QTimer::timeout, this, &MainWindow::refreshLinkStrip);
    timer->start();
    return strip;
}

void MainWindow::refreshLinkStrip() {
    using nadirstation::ui::setTone;
    refreshRunButtons();
    const QDateTime now = QDateTime::currentDateTimeUtc();
    if (linkUtc_) linkUtc_->setText(now.toString(QStringLiteral("HH:mm:ss")));
    // UTC is the station's time (observation slots, logs, file names); local time is the tooltip.
    if (railClock_) {
        railClock_->setText(now.toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss 'UTC'")));
        railClock_->setToolTip(QDateTime::currentDateTime().toString(QStringLiteral("'Local' yyyy-MM-dd HH:mm:ss")));
    }
    if (linkFreq_ && freq_) linkFreq_->setText(QStringLiteral("%1 MHz").arg(freq_->value(), 0, 'f', 3));
    if (spectrum_ && freq_)
        spectrum_->setCenterFrequency(inputSource_ && inputSource_->currentIndex() == 2 ? 0.0 : freq_->value() * 1e6);

    const int source = inputSource_ ? inputSource_->currentIndex() : 0;
    const bool pluto = receiver_ && receiver_->isRunning();
    const bool rtl = (rtlReceiver_ && rtlReceiver_->isRunning()) || (hackrfReceiver_ && hackrfReceiver_->isRunning());
    const bool file = wavReceiver_ && wavReceiver_->isRunning();
    const bool receiving = pluto || rtl || file;
    if (linkInput_) {
        const QString name = source == 1 ? QStringLiteral("RTL-SDR") : source == 2 ? QStringLiteral("File")
                           : source == 3 ? QStringLiteral("HackRF") : QStringLiteral("PlutoSDR");
        linkInput_->setText(name);
        setTone(linkInput_, "tone", receiving ? QStringLiteral("ok") : QStringLiteral("idle"));
    }
    if (linkState_) {
        linkState_->setText(receiving ? linkStateText_ : QStringLiteral("Idle"));
        setTone(linkState_, "tone", receiving ? linkStateTone_ : QStringLiteral("idle"));
    }
    if (linkSnr_) {
        const bool have = receiving && std::isfinite(lastSnrDb_);
        linkSnr_->setText(have ? QStringLiteral("%1 dB").arg(lastSnrDb_, 0, 'f', 1) : QStringLiteral("—"));
        setTone(linkSnr_, "tone", have ? QString() : QStringLiteral("idle"));
    }
    if (linkCarrier_) {
        const bool have = receiving && std::isfinite(lastCarrierHz_);
        linkCarrier_->setText(have ? (std::fabs(lastCarrierHz_) >= 1000.0f
                                          ? QStringLiteral("%1 kHz").arg(lastCarrierHz_ / 1000.0f, 0, 'f', 2)
                                          : QStringLiteral("%1 Hz").arg(lastCarrierHz_, 0, 'f', 0))
                                    : QStringLiteral("—"));
        setTone(linkCarrier_, "tone", have ? QString() : QStringLiteral("idle"));
    }
    if (auto* wide = findChild<QWidget*>(QStringLiteral("WideOnly"))) wide->setVisible(width() >= 1500);
    refreshDecoderPanel(receiving);
    if (decoderPage_) decoderPage_->setRunState(receiving, decoder_ && decoder_->enabled(), file, selectedSatelliteName_);
    // GK-2A only: time to the next 10-minute full-disk slot, or that one is being broadcast now.
    if (linkNext_ && linkNextBox_) {
        const int sat = satelliteSelector_ ? satelliteSelector_->currentData().toInt() : 0;
        const bool gk2a = sat == 10 || sat == 12;
        linkNextBox_->setVisible(gk2a);
        if (gk2a) {
            const qint64 s = now.toSecsSinceEpoch();
            const qint64 intoSlot = s % 600;
            if (intoSlot < 180) {
                linkNext_->setText(QStringLiteral("now %1:%2").arg(intoSlot / 60).arg(intoSlot % 60, 2, 10, QLatin1Char('0')));
                setTone(linkNext_, "tone", QStringLiteral("ok"));
            } else {
                const qint64 left = 600 - intoSlot;
                const QDateTime next = QDateTime::fromSecsSinceEpoch(s + left, QTimeZone::UTC);
                linkNext_->setText(QStringLiteral("%1 −%2:%3").arg(next.toString(QStringLiteral("HH:mm")))
                                       .arg(left / 60).arg(left % 60, 2, 10, QLatin1Char('0')));
                setTone(linkNext_, "tone", QString());
            }
        }
    }
    if (linkRec_ && iqRecorder_) {
        const bool rec = iqRecorder_->active();
        const bool armed = recordBtn_ && recordBtn_->isChecked();
        linkRec_->setText(rec ? QStringLiteral("● %1 MB").arg(static_cast<double>(iqRecorder_->bytesWritten()) / 1e6, 0, 'f', 0)
                              : armed ? QStringLiteral("Armed") : QStringLiteral("Off"));
        setTone(linkRec_, "tone", rec ? QStringLiteral("rec") : armed ? QStringLiteral("warn") : QStringLiteral("idle"));
    }
}

void MainWindow::showPageByName(const QString& name) {
    const QString key = name.trimmed().toLower();
    if (key.isEmpty()) return;
    int index = -1;
    if (key.startsWith(QStringLiteral("proc")) || key.startsWith(QStringLiteral("live"))) index = Live;
    else if (key.startsWith(QStringLiteral("img")) || key.startsWith(QStringLiteral("image"))) index = Images;
    else if (key.startsWith(QStringLiteral("add"))) index = AddData;
    else if (key.startsWith(QStringLiteral("sys"))) index = System;
    else if (key.startsWith(QStringLiteral("tel"))) index = Telemetry;
    else if (key.startsWith(QStringLiteral("set"))) index = Settings;
    else if (key.startsWith(QStringLiteral("ggak"))) index = Ggak;
    else if (key.startsWith(QStringLiteral("globe"))) index = Globe;
    else if (key.startsWith(QStringLiteral("pass")) || key.startsWith(QStringLiteral("orbit"))) index = Passes;
    else if (key.startsWith(QStringLiteral("dec")) || key.startsWith(QStringLiteral("pipe"))) index = Decoder;
    else if (key.startsWith(QStringLiteral("space")) || key == QStringLiteral("sw")) {
        index = Ggak;
        if (ggakPage_) ggakPage_->showTab(QStringLiteral("Space Weather"));
    }
    if (index < 0) {
        nadirstation::log::write(QStringLiteral("GUI --page: no page named '%1'").arg(name));
        nadirstation::log::flush();
        return;
    }
    switchPage(index);
    // The additional-data page reads its folder when it is shown, which switchPage does not do.
    if (index == AddData) refreshAddFiles();
    nadirstation::log::write(QStringLiteral("GUI --page: showing page %1").arg(index));
    nadirstation::log::flush();
}

void MainWindow::switchPage(int index) {
    stack_->setCurrentIndex(index);
    for (QPushButton* b : navButtons_) b->setChecked(b->property("page").toInt() == index);
    // The passes page updates the satellites' positions once a second while it is visible.
    if (passPage_) passPage_->setActive(index == Passes);
    // The system page samples the machine while it is visible and not otherwise.
    if (systemPage_) systemPage_->setActive(index == System);
    // The decoder page animates only while it is shown; it collects its history either way.
    if (decoderPage_) decoderPage_->setActive(index == Decoder);
    if (index == System) refreshSystemStatus();
    refreshLinkStrip();
}

// ---- menus, shortcuts, about, remembered state --------------------------------------------------

void MainWindow::makeMenus() {
    QMenuBar* bar = menuBar();
    const auto add = [this](QMenu* menu, const QString& text, const QKeySequence& key, auto slot) {
        QAction* a = menu->addAction(text);
        if (!key.isEmpty()) a->setShortcut(key);
        connect(a, &QAction::triggered, this, slot);
        return a;
    };

    QMenu* file = bar->addMenu(QStringLiteral("&File"));
    add(file, QStringLiteral("Open recording…"), QKeySequence::Open, [this] {
        if (activeInput_ != InputSource::None) {
            statusBar()->showMessage(QStringLiteral("Stop receiving before opening a recording."), 5000);
            return;
        }
        if (inputSource_) inputSource_->setCurrentIndex(2);
        switchPage(0);
        browseWav();
    });
    add(file, QStringLiteral("Import observation…"), QKeySequence(QStringLiteral("Ctrl+I")), [this] {
        switchPage(1);
        importSessionJson();
    });
    file->addSeparator();
    add(file, QStringLiteral("Open output folder"), QKeySequence(QStringLiteral("Ctrl+Shift+O")), [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(outputRootPath_));
    });
    add(file, QStringLiteral("Save screenshot"), QKeySequence(QStringLiteral("Ctrl+Shift+S")), [this] { saveWindowScreenshot(); });
    add(file, QStringLiteral("Save event log"), QKeySequence(), [this] { saveEventLog(); });
    add(file, QStringLiteral("Open runtime log"), QKeySequence(), [] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(nadirstation::log::path()));
    });
    file->addSeparator();
    add(file, QStringLiteral("Exit"), QKeySequence(QStringLiteral("Ctrl+Q")), [this] { close(); });

    QMenu* rx = bar->addMenu(QStringLiteral("&Receiver"));
    add(rx, QStringLiteral("Start / stop receiving"), QKeySequence(Qt::Key_F5), [this] { if (rxBtn_) rxBtn_->click(); });
    add(rx, QStringLiteral("Start / stop decoding"), QKeySequence(Qt::Key_F6), [this] { if (decodeBtn_) decodeBtn_->click(); });
    add(rx, QStringLiteral("Record IQ on / off"), QKeySequence(Qt::Key_F7), [this] { if (recordBtn_ && recordBtn_->isEnabled()) recordBtn_->click(); });
    rx->addSeparator();
    {
        QAction* alert = rx->addAction(QStringLiteral("Alert when the link drops"));
        alert->setCheckable(true);
        QSettings st(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        alertOnLoss_ = st.value(QStringLiteral("ui2/alertOnLoss"), false).toBool();
        alert->setChecked(alertOnLoss_);
        alert->setToolTip(QStringLiteral("Flash the taskbar entry and beep when a locked link loses sync"));
        connect(alert, &QAction::toggled, this, [this](bool on) {
            alertOnLoss_ = on;
            QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation")).setValue(QStringLiteral("ui2/alertOnLoss"), on);
        });
    }
    rx->addSeparator();
    add(rx, QStringLiteral("Choose satellite…"), QKeySequence(QStringLiteral("Ctrl+L")), [this] {
        if (satelliteSelector_) { satelliteSelector_->setFocus(); satelliteSelector_->showPopup(); }
    });

    QMenu* view = bar->addMenu(QStringLiteral("&View"));
    int n = 1;
    for (QPushButton* b : navButtons_) {
        const int page = b->property("page").toInt();
        QString text = b->text();
        text.replace(QStringLiteral("&&"), QStringLiteral("&"));
        // Ctrl+1 .. Ctrl+9 for the first nine pages; "Ctrl+10" is not a key.
        const QKeySequence key = n <= 9 ? QKeySequence(QStringLiteral("Ctrl+%1").arg(n)) : QKeySequence();
        ++n;
        add(view, text.replace(QLatin1Char('&'), QStringLiteral("&&")), key, [this, page] { switchPage(page); });
    }
    view->addSeparator();
    if (spectrum_) {
        QMenu* spec = view->addMenu(QStringLiteral("Spectrum"));
        spectrum_->peakHoldAction()->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+P")));
        spectrum_->freezeAction()->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+F")));
        spectrum_->clearMarkersAction()->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+M")));
        spectrum_->saveImageAction()->setShortcut(QKeySequence(QStringLiteral("Ctrl+Alt+S")));
        spec->addAction(spectrum_->peakHoldAction());
        spec->addAction(spectrum_->freezeAction());
        spec->addAction(spectrum_->clearMarkersAction());
        spec->addSeparator();
        spec->addAction(spectrum_->saveImageAction());
        connect(spectrum_, &SpectrumWidget::snapshotSaved, this, [this](const QString& path) {
            statusBar()->showMessage(QStringLiteral("Spectrum saved to %1").arg(QDir::toNativeSeparators(path)), 6000);
            logEvent(QStringLiteral("Spectrum image %1").arg(QFileInfo(path).fileName()));
        });
    }
    add(view, QStringLiteral("Full screen"), QKeySequence(Qt::Key_F11), [this] {
        isFullScreen() ? showNormal() : showFullScreen();
    });

    QMenu* help = bar->addMenu(QStringLiteral("&Help"));
    add(help, QStringLiteral("Keyboard shortcuts"), QKeySequence(), [this] {
        QMessageBox::information(this, QStringLiteral("Keyboard shortcuts"), QStringLiteral(
            "F5  start / stop receiving\nF6  start / stop decoding\nF7  record IQ on / off\n"
            "Ctrl+L  choose satellite\nCtrl+1 … Ctrl+9  pages\nCtrl+O  open a recording\n"
            "Ctrl+I  import an observation\nCtrl+Shift+O  open the output folder\n"
            "Ctrl+Shift+S  save a screenshot of the window\n\n"
            "Spectrum: click to place markers M1 / M2, right-click for the tools\n"
            "Ctrl+Shift+P  peak hold\nCtrl+Shift+F  freeze\nCtrl+Shift+M  clear markers\n"
            "Ctrl+Alt+S  save the spectrum image\n"
            "Double-click the level slider for the automatic range\n\nF11  full screen\nCtrl+Q  exit"));
    });
    add(help, QStringLiteral("About NADIR STATION"), QKeySequence(), [this] { showAbout(); });

    // Permanent right side of the status bar: where products go and how much room is left there.
    statusOutput_ = new QLabel;
    statusOutput_->setObjectName(QStringLiteral("Note"));
    statusOutput_->setContentsMargins(8, 0, 8, 0);
    statusBar()->addPermanentWidget(statusOutput_);
    const auto refreshOutput = [this] {
        if (spectrum_) spectrum_->setSnapshotDirectory(QDir(outputRootPath_).filePath(QStringLiteral("Screenshots")));
        const QStorageInfo st(outputRootPath_);
        statusOutput_->setText(QStringLiteral("Output %1  ·  %2 free")
            .arg(QDir::toNativeSeparators(outputRootPath_),
                 st.isValid() ? QLocale().formattedDataSize(st.bytesAvailable()) : QStringLiteral("—")));
    };
    refreshOutput();
    auto* t = new QTimer(this);
    t->setInterval(15000);
    connect(t, &QTimer::timeout, this, refreshOutput);
    t->start();
}

void MainWindow::showAbout() {
    QMessageBox box(this);
    box.setWindowTitle(QStringLiteral("About NADIR STATION"));
    box.setIconPixmap(windowIcon().pixmap(64, 64));
    box.setTextFormat(Qt::RichText);
    box.setText(QStringLiteral("<b>NADIR STATION</b> 2.1.1<br>Geostationary weather-satellite ground station"));
    box.setInformativeText(QStringLiteral(
        "FY-2G / FY-2H S-VISSR · GK-2A LRIT and HRIT · GOES GVAR · ELEKTRO-L GGAK<br>"
        "PlutoSDR, RTL-SDR and baseband recordings.<br><br>"
        "© AGcov (BI2QYN). Built with Qt %1.<br>"
        "Data attribution and the disclaimer are on the Settings page.").arg(QString::fromLatin1(qVersion())));
    box.exec();
}

void MainWindow::restoreUiState() {
    QSettings s(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
    s.beginGroup(QStringLiteral("ui2"));
    const QByteArray geometry = s.value(QStringLiteral("geometry")).toByteArray();
    if (!geometry.isEmpty()) restoreGeometry(geometry);
    if (inputSource_) {
        const int src = s.value(QStringLiteral("inputSource"), 0).toInt();
        if (src >= 0 && src < inputSource_->count()) inputSource_->setCurrentIndex(src);
    }
    if (wavPath_) {
        const QString path = s.value(QStringLiteral("filePath")).toString();
        if (!path.isEmpty() && QFileInfo::exists(path)) wavPath_->setText(path);
    }
    if (satelliteSelector_) {
        const int sat = s.value(QStringLiteral("satellite"), 0).toInt();
        const int idx = satelliteSelector_->findData(sat);
        if (idx > 0) satelliteSelector_->setCurrentIndex(idx);
    }
    const int page = s.value(QStringLiteral("page"), 0).toInt();
    if (page >= 0 && stack_ && page < stack_->count()) switchPage(page);
    s.endGroup();
    updateInputUi();
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QSettings s(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
    s.beginGroup(QStringLiteral("ui2"));
    s.setValue(QStringLiteral("geometry"), saveGeometry());
    if (inputSource_) s.setValue(QStringLiteral("inputSource"), inputSource_->currentIndex());
    if (wavPath_) s.setValue(QStringLiteral("filePath"), wavPath_->text().trimmed());
    if (satelliteSelector_) s.setValue(QStringLiteral("satellite"), satelliteSelector_->currentData().toInt());
    if (stack_) s.setValue(QStringLiteral("page"), stack_->currentIndex());
    s.endGroup();
    QMainWindow::closeEvent(event);
}
