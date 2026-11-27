// The Settings page: ground station, output folder, station log, attribution.
#include "mainwindow.h"
#include "mainwindow_common.h"

QWidget* MainWindow::makeSettingsPage() {
    auto* page = new QWidget;
    auto* root = new QVBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(10);

    root->addWidget(nadirstation::ui::pageHeader(QStringLiteral("Settings"),
        QStringLiteral("Ground station, output folder, station log and attribution")));

    auto* content = new QHBoxLayout;
    content->setSpacing(14);
    root->addLayout(content, 1);

    auto* left = new QWidget;
    auto* leftLayout = new QVBoxLayout(left);

    // The receive chain. This describes the receiver rather than reporting from it, which is why
    // it is here and not under the waterfall: on the processing page it competed for the same
    // vertical space and pushed the status badge against the spectrum.
    {
        auto* chainFrame = new QFrame;
        chainFrame->setObjectName(QStringLiteral("Card"));
        auto* chainLayout = new QVBoxLayout(chainFrame);
        chainLayout->setContentsMargins(14, 12, 14, 12);
        chainLayout->setSpacing(6);
        auto* chainTitle = new QLabel(QStringLiteral("Receive chain"));
        chainTitle->setObjectName(QStringLiteral("CardTitle"));
        chainLayout->addWidget(chainTitle);
        // The text is filled in by applySatelliteSelection, so it follows the chosen spacecraft.
        chainHint_ = new QLabel;
        chainHint_->setWordWrap(true);
        chainHint_->setObjectName(QStringLiteral("Note"));
        chainHint_->setText(QStringLiteral("Select a satellite to show its receive chain."));
        chainLayout->addWidget(chainHint_);
        leftLayout->addWidget(chainFrame);
    }

    leftLayout->setContentsMargins(0, 0, 0, 0);
    leftLayout->setSpacing(12);
    // Ground station for the Passes page (orbit prediction). Saved as it is edited; the passes are
    // recomputed in the background, so typing here never waits for the prediction.
    {
        QSettings s(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        auto* stationFrame = new QFrame;
        stationFrame->setObjectName(QStringLiteral("Card"));
        auto* sl = new QVBoxLayout(stationFrame);
        sl->setContentsMargins(14, 12, 14, 12);
        auto* stTitle = new QLabel(QStringLiteral("Ground station location"));
        stTitle->setObjectName(QStringLiteral("CardTitle"));
        sl->addWidget(stTitle);
        auto* grid = new QGridLayout;
        auto makeSpin = [&](double lo, double hi, int decimals, const QString& suffix, const QString& key, double def) {
            auto* sp = new QDoubleSpinBox;
            sp->setRange(lo, hi);
            sp->setDecimals(decimals);
            sp->setSuffix(suffix);
            sp->setKeyboardTracking(false);
            sp->setValue(s.value(key, def).toDouble());
            return sp;
        };
        stationLat_ = makeSpin(-90.0, 90.0, 6, QStringLiteral("°"), QStringLiteral("station/latDeg"), 0.0);
        stationLon_ = makeSpin(-180.0, 180.0, 6, QStringLiteral("°"), QStringLiteral("station/lonDeg"), 0.0);
        stationAlt_ = makeSpin(-500.0, 9000.0, 0, QStringLiteral(" m"), QStringLiteral("station/altM"), 0.0);
        stationHorizon_ = makeSpin(0.0, 30.0, 1, QStringLiteral("°"), QStringLiteral("station/horizonDeg"), 0.0);
        stationLat_->setToolTip(QStringLiteral("Latitude in decimal degrees, north positive, south negative"));
        stationLon_->setToolTip(QStringLiteral("Longitude in decimal degrees, east positive, west negative"));
        stationAlt_->setToolTip(QStringLiteral("Altitude above sea level (metres)"));
        stationHorizon_->setToolTip(QStringLiteral("Horizon: a satellite counts as visible only above this elevation (raise it if the view is obstructed)"));
        grid->addWidget(new QLabel(QStringLiteral("Latitude")), 0, 0);
        grid->addWidget(stationLat_, 0, 1);
        grid->addWidget(new QLabel(QStringLiteral("Longitude")), 0, 2);
        grid->addWidget(stationLon_, 0, 3);
        grid->addWidget(new QLabel(QStringLiteral("Altitude")), 1, 0);
        grid->addWidget(stationAlt_, 1, 1);
        grid->addWidget(new QLabel(QStringLiteral("Horizon")), 1, 2);
        grid->addWidget(stationHorizon_, 1, 3);
        grid->setColumnStretch(1, 1);
        grid->setColumnStretch(3, 1);
        sl->addLayout(grid);
        auto* stHint = new QLabel(s.value(QStringLiteral("station/set"), false).toBool()
            ? QStringLiteral("Used for orbit prediction on the Passes page. Changes are saved automatically and passes are recalculated in the background.")
            : QStringLiteral("Not set yet: enter latitude and longitude and the Passes page can calculate passes. Changes are saved automatically."));
        stHint->setWordWrap(true);
        stHint->setObjectName(QStringLiteral("Note"));
        sl->addWidget(stHint);
        auto applyStation = [this, stHint] {
            QSettings st(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
            st.setValue(QStringLiteral("station/latDeg"), stationLat_->value());
            st.setValue(QStringLiteral("station/lonDeg"), stationLon_->value());
            st.setValue(QStringLiteral("station/altM"), stationAlt_->value());
            st.setValue(QStringLiteral("station/horizonDeg"), stationHorizon_->value());
            st.setValue(QStringLiteral("station/set"), true);
            stHint->setText(QStringLiteral("Used for orbit prediction on the Passes page. Changes are saved automatically and passes are recalculated in the background."));
            if (passPage_)
                passPage_->setStation(stationLat_->value(), stationLon_->value(), stationAlt_->value(), stationHorizon_->value());
        };
        for (QDoubleSpinBox* sp : {stationLat_, stationLon_, stationAlt_, stationHorizon_})
            connect(sp, qOverload<double>(&QDoubleSpinBox::valueChanged), this, applyStation);
        leftLayout->addWidget(stationFrame);
    }
    auto* outputFrame = new QFrame;
    outputFrame->setObjectName(QStringLiteral("Card"));
    auto* outputLayout = new QVBoxLayout(outputFrame);
    outputLayout->setContentsMargins(14, 12, 14, 12);
    auto* outputTitle = new QLabel(QStringLiteral("Observation output"));
    outputTitle->setObjectName(QStringLiteral("CardTitle"));
    outputLayout->addWidget(outputTitle);
    auto* pathRow = new QHBoxLayout;
    outputPathEdit_ = new QLineEdit(outputRootPath_);
    outputPathEdit_->setPlaceholderText(QStringLiteral("Choose the folder for observation products"));
    auto* browseButton = new QPushButton(QStringLiteral("Browse…"));
    auto* applyButton = new QPushButton(QStringLiteral("Apply path"));
    auto* openFolderButton = new QPushButton(QStringLiteral("Open folder"));
    pathRow->addWidget(outputPathEdit_, 1);
    pathRow->addWidget(browseButton);
    pathRow->addWidget(applyButton);
    pathRow->addWidget(openFolderButton);
    outputLayout->addLayout(pathRow);
    auto* pathHint = new QLabel(QStringLiteral("A new path applies from the next observation; existing observation files are not moved."));
    pathHint->setObjectName(QStringLiteral("Note"));
    outputLayout->addWidget(pathHint);
    // 3x3 median filter on the finished images. Not remembered: it is on again at every start.
    auto* medianCheck = new QCheckBox(QStringLiteral("Apply a 3×3 median filter when building images"));
    medianCheck->setChecked(true);
    applyMedianPolicy();
    outputLayout->addWidget(medianCheck);
    auto* medianHint = new QLabel(QStringLiteral("Suppresses isolated noise pixels and slightly smooths fine texture. On by default at every start; GK-2A never gets the median filter. When off, new products keep the decoded pixels; existing files are unchanged."));
    medianHint->setWordWrap(true);
    medianHint->setObjectName(QStringLiteral("Note"));
    outputLayout->addWidget(medianHint);
    connect(medianCheck, &QCheckBox::toggled, this, [this](bool on) {
        medianUser_ = on;
        applyMedianPolicy();
        statusBar()->showMessage(on ? QStringLiteral("Median filter: on (from the next product)")
                                    : QStringLiteral("Median filter: off (from the next product, this run only)"), 7000);
    });
    leftLayout->addWidget(outputFrame);

    auto* logFrame = new QFrame;
    logFrame->setObjectName(QStringLiteral("Card"));
    auto* logLayout = new QVBoxLayout(logFrame);
    logLayout->setContentsMargins(12, 10, 12, 12);
    auto* logHeader = new QHBoxLayout;
    auto* logTitle = new QLabel(QStringLiteral("Station log"));
    logTitle->setObjectName(QStringLiteral("CardTitle"));
    auto* logPolicy = new QLabel(QStringLiteral("Refreshes automatically · trimmed beyond 30 MB"));
    logPolicy->setObjectName(QStringLiteral("Note"));
    logHeader->addWidget(logTitle);
    logHeader->addStretch();
    logHeader->addWidget(logPolicy);
    logLayout->addLayout(logHeader);
    logViewer_ = new QPlainTextEdit;
    logViewer_->setReadOnly(true);
    logViewer_->setMaximumBlockCount(5000);
    logViewer_->setObjectName(QStringLiteral("LogView"));
    logLayout->addWidget(logViewer_, 1);
    logViewer_->setMinimumHeight(260);
    leftLayout->addWidget(logFrame);
    // Data attribution. This is not a courtesy note: GK-2A LRIT is KMA's data, redistributed
    // under their terms, and the decryption keys are published by them as well. A reader of this
    // station should be able to see whose data they are looking at without reading the source.
    {
        auto* creditFrame = new QFrame;
        creditFrame->setObjectName(QStringLiteral("Card"));
        auto* creditLayout = new QVBoxLayout(creditFrame);
        creditLayout->setContentsMargins(14, 12, 14, 12);
        creditLayout->setSpacing(6);
        auto* creditTitle = new QLabel(QStringLiteral("数据归属与免责声明 · Attribution & disclaimer"));
        creditTitle->setObjectName(QStringLiteral("CardTitle"));
        creditLayout->addWidget(creditTitle);
        // Rich text so the three parts read as sections. Kept in step with THIRD_PARTY_NOTICES.md:
        // what is someone else's data, what is someone else's code,
        // and what this station does not promise.
        auto* creditBody = new QLabel(QStringLiteral(
            "<p style='margin:0 0 4px 0;color:#d6d8db;'><b>数据归属</b></p>"
            "<p style='margin:0 0 8px 0;'>"
            "FY-2G / FY-2H：© 中国气象局（CMA）国家卫星气象中心。<br>"
            "GK-2A LRIT 图像与 ADD 附加数据：© 韩国气象厅（KMA），按其条款接收；"
            "解密密钥不随本软件分发，需自行放入 resources/gk2a/。<br>"
            "ELEKTRO-L GGAK 遥测：卫星由俄罗斯联邦水文气象与环境监测局（Roshydromet）运营。<br>"
            "AI 临近预报的训练与检验另用 KMA GK-2A AMI L1B（NOAA 开放数据）。<br>"
            "国界轮廓：Natural Earth（公有领域，国界为中国视角版本）。<br>"
            "以上数据仍归各发布机构所有。</p>"
            "<p style='margin:0 0 4px 0;color:#d6d8db;'><b>第三方代码与资源</b></p>"
            "<p style='margin:0 0 8px 0;'>"
            "GGAK SKIF/SKL 通道表与换算系数来自他人公开的逆向成果。<br>"
            "FY-2 伪彩查找表：原始项目 eswnl/FY-2x_CLUT。<br>"
            "轨道计算：dnwrnr/sgp4（Apache-2.0）。界面框架：Qt 6（LGPL-3.0）。<br>"
            "本软件以 AGPL-3.0 开源，详见源码中的 LICENSE 与 THIRD_PARTY_NOTICES.md。</p>"
            "<p style='margin:0 0 4px 0;color:#d6d8db;'><b>免责声明</b></p>"
            "<p style='margin:0;'>"
            "本软件为业余接收与研究用途的独立软件，与上述任何机构均无隶属、授权或合作关系。<br>"
            "所有图像、亮温和区域产品均由本站自行接收解码，未经官方质量控制，可能存在解码、定位或定标误差，"
            "不能替代官方发布的产品。<br>"
            "AI 临近预报和选点预报是实验性的外推结果：时效越长误差越大，新生对流报不出来。"
            "它们不是官方天气预报或预警，不能用于防灾、航空、航海等安全相关的决定。<br>"
            "软件按“原样”提供，不作任何明示或默示担保，使用产生的后果由使用者自行承担。<br>"
            "软件离线运行，不连接互联网、不上传数据；网络只用于连接局域网内的 PlutoSDR。</p>"));
        creditBody->setTextFormat(Qt::RichText);
        creditBody->setWordWrap(true);
        creditBody->setObjectName(QStringLiteral("Note"));
        creditLayout->addWidget(creditBody);
        leftLayout->addWidget(creditFrame);
    }
    // The attribution card grew into three sections; on a short window the column scrolls rather
    // than squeezing the cards below it.
    auto* leftScroll = new QScrollArea;
    leftScroll->setWidget(left);
    leftScroll->setWidgetResizable(true);
    leftScroll->setFrameShape(QFrame::NoFrame);
    leftScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    leftScroll->setObjectName(QStringLiteral("PanelScroll"));
    leftScroll->viewport()->setAutoFillBackground(false);
    left->setAutoFillBackground(false);
    content->addWidget(leftScroll, 7);

    auto* right = new QWidget;
    right->setMaximumWidth(370);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->setSpacing(12);
    auto makeStatusCard = [&](const QString& title, QLabel*& value, const QString& initial) {
        auto* frame = new QFrame;
        frame->setObjectName(QStringLiteral("Card"));
        auto* layout = new QVBoxLayout(frame);
        layout->setContentsMargins(14, 11, 14, 12);
        auto* caption = new QLabel(title);
        caption->setObjectName(QStringLiteral("ReadoutCaption"));
        value = new QLabel(initial);
        value->setWordWrap(true);
        value->setObjectName(QStringLiteral("CardTitle"));
        layout->addWidget(caption);
        layout->addWidget(value);
        rightLayout->addWidget(frame);
    };
    makeStatusCard(QStringLiteral("Local time"), localTimeLabel_, QStringLiteral("—"));
    makeStatusCard(QStringLiteral("UTC"), utcTimeLabel_, QStringLiteral("—"));
    makeStatusCard(QStringLiteral("Receiver / decoder"), stationSettingsStatus_, QStringLiteral("Ready"));
    makeStatusCard(QStringLiteral("Satellite"), satelliteSettingsStatus_, QStringLiteral("Not selected"));
    makeStatusCard(QStringLiteral("Calibration-2"), calibrationSettingsStatus_, QStringLiteral("Waiting for calibration data"));
    makeStatusCard(QStringLiteral("Output storage"), storageSettingsStatus_, QStringLiteral("—"));

    auto* about = new QLabel(QStringLiteral(
        "© AGcov (BI2QYN)\nAll rights reserved\nDeveloper: AGcov (BI2QYN)\n\n"
        "FY-2 S-VISSR satellite ground station\nReceive · Decode · Calibrate · Image · Regional products"));
    about->setAlignment(Qt::AlignCenter);
    about->setWordWrap(true);
    about->setObjectName(QStringLiteral("Note"));
    rightLayout->addStretch(1);
    rightLayout->addWidget(about);
    content->addWidget(right, 3);

    connect(browseButton, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose the folder for observation products"),
            outputPathEdit_->text().trimmed());
        if (!chosen.isEmpty()) outputPathEdit_->setText(QDir::toNativeSeparators(chosen));
    });
    connect(applyButton, &QPushButton::clicked, this, [this] {
        const QString path = QDir::cleanPath(QDir::fromNativeSeparators(outputPathEdit_->text().trimmed()));
        if (path.isEmpty() || !QDir().mkpath(path)) {
            QMessageBox::warning(this, QStringLiteral("Invalid output folder"), QStringLiteral("Cannot create or access the chosen folder; check the path and permissions."));
            return;
        }
        outputRootPath_ = QFileInfo(path).absoluteFilePath();
        outputPathEdit_->setText(QDir::toNativeSeparators(outputRootPath_));
        QSettings settings(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        settings.setValue(QStringLiteral("paths/outputRoot"), outputRootPath_);
        NadirStationDecoder* decoder = decoder_.get();
        const QString appliedPath = outputRootPath_;
        QMetaObject::invokeMethod(decoder, [decoder, appliedPath] { decoder->setOutputRoot(appliedPath); }, Qt::QueuedConnection);
        statusBar()->showMessage(QStringLiteral("Output folder updated: the new folder is used from the next observation."), 7000);
        if (globePage_) globePage_->setDataRoot(outputRootPath_);
        if (spaceWeatherPage_) spaceWeatherPage_->setDataRoot(outputRootPath_);
    });
    connect(openFolderButton, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(outputRootPath_));
    });

    settingsRefreshTimer_ = new QTimer(this);
    settingsRefreshTimer_->setInterval(1000);
    connect(settingsRefreshTimer_, &QTimer::timeout, this, [this] {
        const QDateTime now = QDateTime::currentDateTime();
        localTimeLabel_->setText(now.toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss")));
        utcTimeLabel_->setText(now.toUTC().toString(QStringLiteral("yyyy-MM-dd  HH:mm:ss 'UTC'")));
        const bool receiving = (receiver_ && receiver_->isRunning()) || (wavReceiver_ && wavReceiver_->isRunning()) ||
                               (rtlReceiver_ && rtlReceiver_->isRunning());
        const bool decoding = decoder_ && decoder_->enabled();
        stationSettingsStatus_->setText(QStringLiteral("RX %1   ·   Decode %2\n%3")
            .arg(receiving ? QStringLiteral("RUNNING") : QStringLiteral("IDLE"),
                 decoding ? QStringLiteral("ON") : QStringLiteral("OFF"),
                 connLabel_ ? connLabel_->text() : QStringLiteral("Pluto status unavailable")));
        satelliteSettingsStatus_->setText(satelliteSelector_ && satelliteSelector_->currentIndex() > 0
            ? satelliteSelector_->currentText() : QStringLiteral("Select a target satellite"));

        QString calibration = QStringLiteral("No observation yet");
        QDir outputDir(outputRootPath_);
        const QFileInfoList sessions = outputDir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Time);
        for (const QFileInfo& session : sessions) {
            QDir sessionDir(session.absoluteFilePath());
            QString manifestPath = sessionDir.filePath(QStringLiteral("FY2_Observation.json"));
            if (!QFileInfo::exists(manifestPath)) manifestPath = sessionDir.filePath(QStringLiteral("MGAS_Observation.json"));
            QFile manifest(manifestPath);
            if (!manifest.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
            const QJsonObject root = QJsonDocument::fromJson(manifest.readAll()).object();
            manifest.close();
            const QJsonObject cal = root.value(QStringLiteral("radiometric_calibration")).toObject();
            if (cal.isEmpty()) continue;
            const int received = cal.value(QStringLiteral("groups_received")).toInt();
            const int expected = cal.value(QStringLiteral("groups_expected")).toInt(25);
            calibration = cal.value(QStringLiteral("ready")).toBool()
                ? QStringLiteral("Ready · %1/%2 groups").arg(received).arg(expected)
                : QStringLiteral("Collecting · %1/%2 groups").arg(received).arg(expected);
            break;
        }
        calibrationSettingsStatus_->setText(calibration);

        const QStorageInfo storage(outputRootPath_);
        storageSettingsStatus_->setText(storage.isValid() && storage.isReady()
            ? QStringLiteral("%1 available\n%2")
                .arg(QLocale().formattedDataSize(storage.bytesAvailable()),
                     QDir::toNativeSeparators(outputRootPath_))
            : QStringLiteral("Storage status unavailable\n%1").arg(QDir::toNativeSeparators(outputRootPath_)));

        const QString logPath = nadirstation::log::path();
        const QFileInfo logInfo(logPath);
        if (logInfo.exists() && logInfo.size() != displayedLogSize_) {
            QFile logFile(logPath);
            if (logFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                constexpr qint64 tailBytes = 768 * 1024;
                const qint64 size = logFile.size();
                logFile.seek(qMax<qint64>(0, size - tailBytes));
                const QString text = QString::fromUtf8(logFile.readAll());
                logFile.close();
                logViewer_->setPlainText(text);
                QTextCursor cursor = logViewer_->textCursor();
                cursor.movePosition(QTextCursor::End);
                logViewer_->setTextCursor(cursor);
                displayedLogSize_ = size;
            }
        }
    });
    settingsRefreshTimer_->start();
    return page;
}
