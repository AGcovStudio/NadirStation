// The Images page and everything that loads, processes, projects and saves observations.
#include "mainwindow.h"
#include "mainwindow_common.h"
#include "polarhrpt.h"

QWidget* MainWindow::makeImagePage() {
    using namespace nadirstation::ui;
    auto* page = new QWidget;
    auto* root = new QHBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    // Left: the observations this session knows about.
    importSession_ = new QPushButton(QStringLiteral("Import…"));
    importSession_->setToolTip(QStringLiteral("Open an observation's JSON manifest (FY2_Observation.json, GK2A_Observation.json, MetOp_Observation.json …)"));
    Card left = card(QStringLiteral("Observations"), importSession_);
    left.frame->setFixedWidth(280);
    sessions_ = new QListWidget;
    left.body->addWidget(sessions_, 1);
    sessions_->setToolTip(QStringLiteral("Click an observation to show it. In the viewer: wheel to zoom, drag to pan, double-click to fit."));
    root->addWidget(left.frame);

    // Centre: the viewer, with the loaded image's description and the save action above it.
    saveImage_ = new QPushButton("Save Enhanced Image");
    imageInfo_ = label(QStringLiteral("No image loaded"), "CardTitle");
    imageInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    Card centre = card(QString(), saveImage_);
    centre.header->insertWidget(0, imageInfo_, 1);
    auto* cl = centre.body;
    imageView_ = new ImageView;
    cl->addWidget(imageView_, 1);
    connect(imageView_, &ImageView::imagePositionHovered, this, [this](const QPointF& position) {
        if (hoverTbbImage_.isNull() || !hoverTemperaturePopup_) return;
        const int x = static_cast<int>(std::floor(position.x()));
        const int y = static_cast<int>(std::floor(position.y()));
        if (x < 0 || y < 0 || x >= hoverTbbImage_.width() || y >= hoverTbbImage_.height()) {
            hoverTemperaturePopup_->hide();
            return;
        }
        const auto* row = reinterpret_cast<const quint16*>(hoverTbbImage_.constScanLine(y));
        const quint16 hundredthsKelvin = row[x];
        QString text;
        if (hundredthsKelvin < 15000 || hundredthsKelvin > 35000) {
            text = QStringLiteral("%1\n(x=%2, y=%3) no valid brightness temperature").arg(hoverTbbLabel_).arg(x).arg(y);
        } else {
            const double kelvin = hundredthsKelvin / 100.0;
            text = QStringLiteral("%1\n%2 K  /  %3 °C\n(x=%4, y=%5)")
                .arg(hoverTbbLabel_).arg(kelvin, 0, 'f', 2)
                .arg(kelvin - 273.15, 0, 'f', 2).arg(x).arg(y);
        }
        hoverTemperaturePopup_->setText(text);
        hoverTemperaturePopup_->adjustSize();
        const QPoint cursor = imageView_->viewport()->mapFromGlobal(QCursor::pos());
        const QRect bounds = imageView_->viewport()->rect();
        int popupX = cursor.x() + 16;
        int popupY = cursor.y() + 18;
        if (popupX + hoverTemperaturePopup_->width() > bounds.right())
            popupX = cursor.x() - hoverTemperaturePopup_->width() - 12;
        if (popupY + hoverTemperaturePopup_->height() > bounds.bottom())
            popupY = cursor.y() - hoverTemperaturePopup_->height() - 12;
        popupX = qBound(bounds.left(), popupX,
            qMax(bounds.left(), bounds.right() - hoverTemperaturePopup_->width() + 1));
        popupY = qBound(bounds.top(), popupY,
            qMax(bounds.top(), bounds.bottom() - hoverTemperaturePopup_->height() + 1));
        hoverTemperaturePopup_->move(popupX, popupY);
        hoverTemperaturePopup_->show();
        hoverTemperaturePopup_->raise();
    });
    connect(imageView_, &ImageView::imageHoverLeft, this, [this]() {
        if (hoverTemperaturePopup_) hoverTemperaturePopup_->hide();
    });
    hoverTemperaturePopup_ = new QLabel(imageView_->viewport());
    hoverTemperaturePopup_->setAttribute(Qt::WA_TransparentForMouseEvents);
    hoverTemperaturePopup_->setStyleSheet(QStringLiteral(
        "QLabel { background: rgba(8, 15, 24, 235); color: #eceef0; "
        "border: 1px solid #8cc0ee; border-radius:2px; padding: 7px 9px; font-weight: 600; }"));
    hoverTemperaturePopup_->hide();
    root->addWidget(centre.frame, 1);

    // Right: what to show and what to make from it.
    auto* rightContent = new QWidget;
    auto* rl = new QVBoxLayout(rightContent);
    rl->setContentsMargins(14, 6, 12, 14);
    rl->setSpacing(6);
    // Two pickers: the instrument (AVHRR, MHS, ... or Imager / L2 products / RGB composites for the
    // geostationary services), then its pictures. Both drive the full list in product_, which stays
    // the index into currentPaths_ the rest of the page uses, and is not shown.
    rl->addWidget(label(QStringLiteral("Instrument"), "SectionTitle"));
    instrumentPick_ = new QComboBox;
    rl->addWidget(instrumentPick_);
    rl->addWidget(label(QStringLiteral("Image"), "SectionTitle"));
    imagePick_ = new QComboBox;
    rl->addWidget(imagePick_);
    product_ = new QComboBox(rightContent);
    product_->addItems({"CH1 VIS", "CH2 IR4", "CH3 IR3", "CH4 IR1", "CH5 IR2", "IR Enhanced"});
    product_->hide();
    auto queueRebuild = [this] {
        if (pickersRebuildQueued_) return;
        pickersRebuildQueued_ = true;
        QTimer::singleShot(0, this, [this] { pickersRebuildQueued_ = false; rebuildProductPickers(); });
    };
    connect(product_->model(), &QAbstractItemModel::rowsInserted, this, queueRebuild);
    connect(product_->model(), &QAbstractItemModel::rowsRemoved, this, queueRebuild);
    connect(product_->model(), &QAbstractItemModel::modelReset, this, queueRebuild);
    connect(product_->model(), &QAbstractItemModel::dataChanged, this, queueRebuild);
    connect(product_, &QComboBox::currentIndexChanged, this, queueRebuild);
    connect(instrumentPick_, &QComboBox::activated, this, [this] {
        fillImagePick();
        if (imagePick_->count() > 0) {
            imagePick_->setCurrentIndex(0);
            product_->setCurrentIndex(imagePick_->itemData(0).toInt());
        }
    });
    connect(imagePick_, &QComboBox::activated, this, [this](int i) {
        const int master = imagePick_->itemData(i).toInt();
        if (master >= 0 && master < product_->count()) product_->setCurrentIndex(master);
    });
    rl->addSpacing(8);
    rl->addWidget(rule());
    // RGB composite from a formula: one expression per colour, e.g. "ch2, ch2, ch1" (the 221 false
    // colour) or "ch3a, ch2, ch1"; arithmetic and inv()/min()/max()/clamp()/pow() are allowed.
    rl->addWidget(label(QStringLiteral("RGB composite"), "SectionTitle"));
    rgbFormula_ = new QLineEdit;
    rgbFormula_->setPlaceholderText(QStringLiteral("ch2, ch2, ch1"));
    rgbFormula_->setText(QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation"))
                             .value(QStringLiteral("images/rgbFormula"), QStringLiteral("ch2, ch2, ch1")).toString());
    rgbFormula_->setToolTip(QStringLiteral(
        "Red, green, blue: one expression each, separated by commas (one expression gives a grey picture).\n"
        "Channels are named ch1, ch2 … as listed below and read as 0 … 1.\n"
        "Allowed: numbers, + − × ÷, parentheses, inv(x) = 1 − x, abs, min, max, clamp(x, lo, hi), pow(x, y).\n"
        "Examples: ch2, ch2, ch1 · ch3a, ch2, ch1 · inv(ch4), inv(ch4), inv(ch5) · ch4 − ch5, ch4, ch5"));
    rl->addWidget(rgbFormula_);
    auto* rgbRow = new QWidget;
    auto* rgbRowLayout = new QHBoxLayout(rgbRow);
    rgbRowLayout->setContentsMargins(0, 0, 0, 0);
    rgbStretch_ = new QCheckBox(QStringLiteral("Stretch each colour"));
    rgbStretch_->setChecked(true);
    rgbStretch_->setToolTip(QStringLiteral("Stretch every component between its 0.5 % and 99.5 % values; off: 0 … 1 maps to 0 … 255 as written."));
    auto* rgbMake = new QPushButton(QStringLiteral("Compose"));
    rgbRowLayout->addWidget(rgbStretch_, 1);
    rgbRowLayout->addWidget(rgbMake);
    rl->addWidget(rgbRow);
    rgbChannels_ = label(QStringLiteral("Load an observation to see its channels."), "Note");
    rgbChannels_->setWordWrap(true);
    rgbChannels_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    rl->addWidget(rgbChannels_);
    connect(rgbMake, &QPushButton::clicked, this, &MainWindow::composeRgbFromFormula);
    connect(rgbFormula_, &QLineEdit::returnPressed, this, &MainWindow::composeRgbFromFormula);
    rl->addSpacing(8);
    rl->addWidget(rule());
    rl->addWidget(label(QStringLiteral("Region"), "SectionTitle"));
    projLat_ = new QDoubleSpinBox;
        projLat_->setRange(-90, 90); projLat_->setValue(36); projLat_->setSuffix(" °");
    projLon_ = new QDoubleSpinBox;
        projLon_->setRange(-180, 180); projLon_->setValue(104); projLon_->setSuffix(" °");
    auto* regionBox = new QWidget;
    Form pf(regionBox);
    pf.row(QStringLiteral("Centre latitude"), projLat_);
    pf.row(QStringLiteral("Centre longitude"), projLon_);
    projSpan_ = new QDoubleSpinBox;
    projSpan_->setRange(2.0, 100.0);
    projSpan_->setDecimals(1);
    projSpan_->setSingleStep(2.0);
    projSpan_->setValue(40.0);
    projSpan_->setSuffix(QStringLiteral(" °"));
    pf.row(QStringLiteral("Span (GK-2A)"), projSpan_);
    rl->addWidget(regionBox);
    rl->addSpacing(4);
    rl->addWidget(label(QStringLiteral("Regional products"), "SectionTitle"));
    auto* regenerateGk2a = new QPushButton(QStringLiteral("GK-2A regional products"));
    regenerateGk2a->setToolTip(QStringLiteral("Rebuilds the regional MCIR and enhanced IR images for the chosen centre and extent from the current observation's IR image and brightness-temperature grid."));
    rl->addWidget(regenerateGk2a);
    connect(regenerateGk2a, &QPushButton::clicked, this, &MainWindow::regenerateGk2aRegionalProducts);
    hritProjectButton_ = new QPushButton(QStringLiteral("HRIT 5-channel calibrated projection"));
    hritProjectButton_->setToolTip(QStringLiteral(
        "Converts the five channels of the current GK-2A HRIT observation to brightness temperature (K) or albedo (%) with each channel's calibration table, "
        "projects them onto an equirectangular grid for the centre and extent above, and saves them in the Projection subfolder of the observation folder "
        "(per channel: one 16-bit image in 0.01 units and one image with a legend). Never generated automatically while receiving."));
    rl->addWidget(hritProjectButton_);
    connect(hritProjectButton_, &QPushButton::clicked, this, &MainWindow::projectHritFiveChannels);
    visCalButton_ = new QPushButton("FY-2 regional VIS reflectance");
    visCalButton_->setToolTip(QStringLiteral("Builds a 68°×42° regional reflectance value image and enhanced image around the centre entered above; only the regional products are saved."));
    rl->addWidget(visCalButton_);
    connect(visCalButton_, &QPushButton::clicked, this, [this] {
        if (selectedSatelliteName_ != QStringLiteral("FY-2G") &&
            selectedSatelliteName_ != QStringLiteral("FY-2H")) {
            statusBar()->showMessage(QStringLiteral("Regional VIS reflectance works only for FY-2G / FY-2H."), 6000);
            return;
        }
        if (!satelliteSelected_) {
            statusBar()->showMessage(QStringLiteral("Select FY-2G or FY-2H at the top of the window first."), 7000);
            return;
        }
        const QString sourcePath = currentPaths_.value(0);
        if (sourcePath.isEmpty() || !QFileInfo::exists(sourcePath)) {
            statusBar()->showMessage(QStringLiteral("Load an observation that contains CH1_VIS first."), 6000);
            return;
        }
        // Do not let a manually selected FY-2 spacecraft turn an arbitrary
        // grayscale image (for example GK2A_IR_raw.png) into a nominal VIS
        // reflectance product. This retrieval is defined only for the decoded
        // FY-2 CH1 visible channel; the source basename is part of its provenance.
        if (QFileInfo(sourcePath).fileName().compare(QStringLiteral("CH1_VIS.png"),
                                                     Qt::CaseInsensitive) != 0) {
            statusBar()->showMessage(
                QStringLiteral("Regional reflectance accepts only a decoded FY-2 CH1_VIS.png; current input: %1")
                    .arg(QFileInfo(sourcePath).fileName()), 8000);
            return;
        }
        const QImage source(sourcePath);
        const QString noradId = selectedSatelliteName_ == QStringLiteral("FY-2G")
            ? QStringLiteral("40367") : QStringLiteral("43491");
        nadirstation::orbit::SatellitePosition rawOrbit, cameraOrbit;
        int sourceFirstLine = 1;
        QString orbitError;
        if (!resolveObservationOrbit(sourcePath, currentTimestamp_, noradId, false,
                                     source.width() > 3000 ? 4 : 1,
                                     rawOrbit, cameraOrbit, sourceFirstLine, orbitError)) {
            statusBar()->showMessage(QStringLiteral("Orbit determination for the visible product failed: %1").arg(orbitError), 8000);
            return;
        }
        int repairedRows = 0;
        int repairedPixels = 0;
        QImage repairedVisible = nadirstation::products::repairMissingLines(source, 8, &repairedRows);
        repairedVisible = nadirstation::products::medianBlur(repairedVisible, 3, &repairedPixels);
        // Prefer this scene's per-observation spin-scan navigation; fall back to the calibrated
        // legacy lattice when the session cannot be navigated.
        QString navigationReport;
        const nadirstation::products::ScanGeometry scan = nadirstation::products::navigateObservation(
            noradId, QFileInfo(sourcePath).absolutePath(), &navigationReport);
        QDateTime illuminationUtc = rawOrbit.propagatedAtUtc;
        if (scan.navigation.valid)
            illuminationUtc = scan.navigation.scanStartUtc.addMSecs(
                scan.navigation.scanStartUtc.msecsTo(scan.navigation.scanEndUtc) / 2);
        else
            statusBar()->showMessage(navigationReport, 10000);
        QImage factor = nadirstation::products::chinaGeostationaryReflectanceFactor(
            repairedVisible, projLat_->value(), projLon_->value(), cameraOrbit,
            sourceFirstLine, scan, illuminationUtc);
        // Keep the exact geometry/no-data footprint so regional gap repair and
        // smoothing cannot put values back into night-side or off-disk pixels.
        const QImage factorFootprint = factor.copy();
        const QImage sourceDisplay = nadirstation::products::visibleReflectanceEnhancement(repairedVisible);
        QImage enhanced = nadirstation::products::chinaGeostationaryProjection(
            sourceDisplay, projLat_->value(), projLon_->value(), cameraOrbit,
            sourceFirstLine, 0, 0, scan);
        // Reflectance uses 0..65534 with 65535 as no-data; the helper's
        // default 15000..35000 range is reserved for calibrated brightness
        // temperatures and would misclassify most reflectance values.
        factor = nadirstation::products::repairMissingLinesMedian(factor, 2, nullptr, 0, 65534);
        factor = nadirstation::products::medianBlurPreservingFormat(factor, 3);
        enhanced = nadirstation::products::repairMissingLines(enhanced, 8);
        enhanced = nadirstation::products::medianBlurPreservingFormat(enhanced, 3);
        if (factor.format() == QImage::Format_Grayscale16 &&
            factorFootprint.format() == QImage::Format_Grayscale16 &&
            factor.size() == factorFootprint.size() && enhanced.format() == QImage::Format_Grayscale8 &&
            enhanced.size() == factor.size()) {
            for (int y = 0; y < factor.height(); ++y) {
                const auto* footprintRow = reinterpret_cast<const quint16*>(factorFootprint.constScanLine(y));
                auto* factorRow = reinterpret_cast<quint16*>(factor.scanLine(y));
                auto* displayRow = enhanced.scanLine(y);
                for (int x = 0; x < factor.width(); ++x) {
                    if (footprintRow[x] == 65535) {
                        factorRow[x] = 65535;
                        displayRow[x] = 115;
                    }
                }
            }
        }
        const QDir dir(QFileInfo(sourcePath).absolutePath());
        const QString factorPath = dir.filePath(QStringLiteral("VIS_ReflectanceFactor_Region.png"));
        const QString enhancedPath = dir.filePath(QStringLiteral("VIS_Reflectance_Enhanced_Region.png"));
        if (factor.isNull() || enhanced.isNull() || !factor.save(factorPath, "PNG") ||
            !enhanced.save(enhancedPath, "PNG")) {
            statusBar()->showMessage(QStringLiteral("Saving the reflectance product failed."), 6000);
            return;
        }
        QJsonObject metadata;
        metadata["format"] = QStringLiteral("FY2-Regional-Visible-Reflectance-1");
        metadata["units"] = QStringLiteral("0.01 percent reflectance per uint16 pixel");
        metadata["channel"] = QStringLiteral("CH1_VIS");
        metadata["calibration_lut"] = QStringLiteral("NSMC FY-2 RSB LUT updated 2016-12-26");
        metadata["satellite"] = selectedSatelliteName_;
        metadata["satellite_identity_source"] = QStringLiteral("manual UI selection");
        metadata["tle_subpoint_raw_longitude_deg"] = rawOrbit.subpointLongitudeDeg;
        metadata["tle_subpoint_raw_latitude_deg"] = rawOrbit.subpointLatitudeDeg;
        metadata["camera_boresight_longitude_deg"] = cameraOrbit.subpointLongitudeDeg;
        metadata["camera_boresight_latitude_deg"] = cameraOrbit.subpointLatitudeDeg;
        metadata["tle_propagated_at_utc"] = rawOrbit.propagatedAtUtc.toString(Qt::ISODateWithMs);
        metadata["navigation_note"] = scan.navigation.valid
            ? scan.navigation.summary
            : navigationReport + QStringLiteral("; camera assumed Earth-pointing/equatorial at the TLE longitude, expect 10-40 px errors.");
        metadata["navigation_model"] = scan.navigation.valid
            ? QStringLiteral("spin_scan_per_observation") : QStringLiteral("legacy_lattice_fallback");
        metadata["calibration_source"] = QStringLiteral("https://nsmc.org.cn/nsmc/en/news/100194.html");
        metadata["source_product"] = QFileInfo(sourcePath).fileName();
        metadata["source_note"] = QStringLiteral("Legacy 8-bit image is reverse-mapped to native 6-bit DN from floor(DN*255/63).");
        metadata["center_latitude_deg"] = projLat_->value();
        metadata["center_longitude_deg"] = projLon_->value();
        metadata["bounds_lonlat_deg"] = QJsonArray{projLon_->value() - 34.0,
            projLat_->value() - 21.0, projLon_->value() + 34.0, projLat_->value() + 21.0};
        metadata["projection"] = QStringLiteral("Equirectangular lon/lat grid; 1360x840 pixels over 68x42 degrees (20 px/degree)");
        metadata["no_data_value"] = 65535;
        metadata["solar_zenith_limit_deg"] = 85.0;
        metadata["night_side_mask_applied"] = true;
        metadata["night_side_note"] = QStringLiteral("Pixels with solar zenith angle >85 degrees (night and low-sun twilight) are set to no data.");
        metadata["no_data_note"] = QStringLiteral("65535 marks locations outside the visible geostationary disk or with solar zenith >85 degrees; zero remains the published DN=0 reflectance value.");
        metadata["repair_blank_rows"] = repairedRows;
        metadata["repair_impulse_pixels"] = repairedPixels;
        metadata["limitations"] = QStringLiteral("Sensor reflectance factor only; no atmospheric correction or surface-albedo retrieval. Legacy zero fill is indistinguishable from calibrated DN 0.");
        QFile sidecar(dir.filePath(QStringLiteral("VIS_ReflectanceFactor_Region.json")));
        if (sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            sidecar.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
            sidecar.close();
        }
        QJsonObject enhancedMetadata = metadata;
        enhancedMetadata["product"] = QStringLiteral("Regional calibrated VIS reflectance display enhancement");
        enhancedMetadata["units"] = QStringLiteral("display grayscale; sqrt reflectance stretch with robust regional contrast");
        enhancedMetadata["display_quality_note"] = QStringLiteral("Small projected background components up to 64 pixels are median-filled only when surrounded by valid pixels; numerical factor raster is not interpolated.");
        QFile enhancedSidecar(dir.filePath(QStringLiteral("VIS_Reflectance_Enhanced_Region.json")));
        if (enhancedSidecar.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            enhancedSidecar.write(QJsonDocument(enhancedMetadata).toJson(QJsonDocument::Indented));
            enhancedSidecar.close();
        }
        QStringList paths = currentPaths_;
        for (const QString& path : {factorPath, enhancedPath})
            if (!paths.contains(path)) paths << path;
        QFile manifestFile(observationManifestPath(dir));
        if (manifestFile.open(QIODevice::ReadOnly)) {
            QJsonParseError parseError;
            const QJsonDocument manifestDocument = QJsonDocument::fromJson(manifestFile.readAll(), &parseError);
            manifestFile.close();
            if (parseError.error == QJsonParseError::NoError && manifestDocument.isObject()) {
                QJsonObject manifestObject = manifestDocument.object();
                QJsonArray products = manifestObject.value(QStringLiteral("products")).toArray();
                for (const QString& path : {factorPath, enhancedPath}) {
                    bool exists = false;
                    for (const auto& product : products)
                        if (product.toObject().value(QStringLiteral("path")).toString() == QFileInfo(path).fileName()) exists = true;
                    if (!exists) {
                        QJsonObject product;
                        product[QStringLiteral("path")] = QFileInfo(path).fileName();
                        product[QStringLiteral("absolute_path")] = QFileInfo(path).absoluteFilePath();
                        products.append(product);
                    }
                }
                manifestObject[QStringLiteral("products")] = products;
                manifestFile.setFileName(dir.filePath(QStringLiteral("FY2_Observation.json")));
                if (manifestFile.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
                    manifestFile.write(QJsonDocument(manifestObject).toJson(QJsonDocument::Indented));
                    manifestFile.close();
                    QFile::remove(dir.filePath(QStringLiteral("MGAS_Observation.json")));
                }
            }
        }
        auto* item = new QListWidgetItem(currentTimestamp_ + QStringLiteral(" · Regional visible reflectance"));
        item->setData(Qt::UserRole, paths);
        sessions_->addItem(item);
        sessions_->setCurrentItem(item);
        loadImagePaths(paths, item->text());
        product_->setCurrentIndex(paths.indexOf(enhancedPath));
        statusBar()->showMessage(QStringLiteral("Regional reflectance grid and enhanced image written for the entered lat/lon: %1").arg(dir.absolutePath()), 9000);
    });
    auto* regionalProducts = new QPushButton(QStringLiteral("Equirectangular regional products"));
    rl->addWidget(regionalProducts);
    connect(regionalProducts, &QPushButton::clicked, this, [this] {
        generateRegionalProductsForObservation(currentTimestamp_, currentPaths_);
    });
    auto* derivedProduct = new QComboBox;
    derivedProduct->addItem(QStringLiteral("Cloud detection (IR1 threshold proxy)"), QStringLiteral("CLD"));
    derivedProduct->addItem(QStringLiteral("Cloud-top temperature mask (CTT)"), QStringLiteral("CTT"));
    derivedProduct->addItem(QStringLiteral("Thermal cloud class proxy (CLC)"), QStringLiteral("CLC"));
    derivedProduct->addItem(QStringLiteral("Cloud fraction grid (32×32 samples)"), QStringLiteral("CFR"));
    // Multispectral diagnostics. They read the sibling CH5/CH3/CH2 TBB rasters of the same
    // observation and are candidate products: thresholds are conventional, not fitted.
    derivedProduct->addItem(QStringLiteral("Cloud mask, 4-channel (IR1–IR4)"), QStringLiteral("CLD4"));
    derivedProduct->addItem(QStringLiteral("Cloud classes, 4-channel (IR1–IR4)"), QStringLiteral("CLC4"));
    derivedProduct->addItem(QStringLiteral("Cloud level low/mid/high (ISCCP pressure bounds)"), QStringLiteral("CLL"));
    derivedProduct->addItem(QStringLiteral("Split-window difference IR1−IR2"), QStringLiteral("BTD_SW"));
    derivedProduct->addItem(QStringLiteral("Water-vapour minus window IR3−IR1"), QStringLiteral("BTD_WV"));
    derivedProduct->addItem(QStringLiteral("Convective cloud / overshooting-top candidates"), QStringLiteral("CONV"));
    derivedProduct->addItem(QStringLiteral("Night fog / low-stratus candidates (IR1−IR4)"), QStringLiteral("FOGN"));
    derivedProduct->addItem(QStringLiteral("Water-vapour colour enhancement (IR3)"), QStringLiteral("WVE"));
    rl->addSpacing(4);
    rl->addWidget(rule());
    rl->addWidget(label(QStringLiteral("Derived products (calibrated TBB)"), "SectionTitle"));
    rl->addWidget(derivedProduct);
    auto* makeDerived = new QPushButton(QStringLiteral("Generate selected product"));
    rl->addWidget(makeDerived);
    connect(makeDerived, &QPushButton::clicked, this, [this, derivedProduct] {
        QString tbbPath;
        for (const QString& path : currentPaths_) {
            if (QFileInfo(path).completeBaseName().compare(QStringLiteral("CH4_IR1_TBB_0p01K"), Qt::CaseInsensitive) == 0) {
                tbbPath = path;
                break;
            }
        }
        if (tbbPath.isEmpty() || !QFileInfo::exists(tbbPath)) {
            statusBar()->showMessage(QStringLiteral("This observation has no valid IR1 brightness-temperature calibration product; decode a complete Calibration-2 block first."), 8000);
            return;
        }
        const QImage tbb(tbbPath);
        const QString code = derivedProduct->currentData().toString();
        QImage result;
        QString suffix, title, units, method;
        // New multispectral products: the numeric raster (when there is one) is written beside the
        // picture, un-filtered, and the legacy smoothing below is skipped for them.
        QImage numeric;
        QString numericSuffix;
        bool postProcess = true;
        QString productStatus = QStringLiteral("single_scene_proxy");
        const QStringList multiChannelCodes{QStringLiteral("CLD4"), QStringLiteral("CLC4"),
            QStringLiteral("CLL"), QStringLiteral("BTD_SW"), QStringLiteral("BTD_WV"),
            QStringLiteral("CONV"), QStringLiteral("FOGN"), QStringLiteral("WVE")};
        QImage ir2, ir3, ir4;
        if (multiChannelCodes.contains(code)) {
            const QDir tbbDir(QFileInfo(tbbPath).absolutePath());
            const auto loadChannel = [&tbbDir](const QString& base) {
                const QString path = tbbDir.filePath(base + QStringLiteral(".png"));
                return QFileInfo::exists(path) ? QImage(path) : QImage();
            };
            ir2 = loadChannel(QStringLiteral("CH5_IR2_TBB_0p01K"));
            ir3 = loadChannel(QStringLiteral("CH3_IR3_TBB_0p01K"));
            ir4 = loadChannel(QStringLiteral("CH2_IR4_TBB_0p01K"));
            const bool needIr2 = code == QStringLiteral("BTD_SW") || code == QStringLiteral("CLD4") ||
                                 code == QStringLiteral("CLC4");
            const bool needIr3 = code == QStringLiteral("BTD_WV") || code == QStringLiteral("CONV") ||
                                 code == QStringLiteral("WVE") || code == QStringLiteral("CLD4") ||
                                 code == QStringLiteral("CLC4");
            const bool needIr4 = code == QStringLiteral("FOGN") || code == QStringLiteral("CLD4") ||
                                 code == QStringLiteral("CLC4");
            if ((needIr2 && ir2.isNull()) || (needIr3 && ir3.isNull()) || (needIr4 && ir4.isNull())) {
                statusBar()->showMessage(QStringLiteral("This product needs the IR2/IR3/IR4 brightness-temperature calibration products of the same observation, and one of them is missing."), 8000);
                return;
            }
            productStatus = QStringLiteral("diagnostic_candidate_unfitted_thresholds");
            postProcess = false;
        }
        if (code == QStringLiteral("BTD_SW")) {
            numeric = nadirstation::products::brightnessTemperatureDifference(tbb, ir2);
            result = nadirstation::products::btdColourEnhancement(numeric, -3.0, 6.0);
            suffix = QStringLiteral("BTD_IR1mIR2_SplitWindow_Enhanced.png");
            numericSuffix = QStringLiteral("BTD_IR1mIR2_SplitWindow_Numeric.png");
            title = QStringLiteral("Split-window brightness-temperature difference (IR1 − IR2)");
            units = QStringLiteral("K; numeric raster stores (difference + 100 K) × 100 as uint16, 65535 = no data");
            method = QStringLiteral("Difference of the two calibrated longwave channels. Large positive values mark thin cirrus and moist boundary layers; negative values can mark mineral dust. Diagnostic only; colour scale saturates at −3 / +6 K.");
        } else if (code == QStringLiteral("BTD_WV")) {
            numeric = nadirstation::products::brightnessTemperatureDifference(ir3, tbb);
            result = nadirstation::products::btdColourEnhancement(numeric, -30.0, 6.0);
            suffix = QStringLiteral("BTD_IR3mIR1_WVminusIRW_Enhanced.png");
            numericSuffix = QStringLiteral("BTD_IR3mIR1_WVminusIRW_Numeric.png");
            title = QStringLiteral("Water-vapour minus window brightness-temperature difference (IR3 − IR1)");
            units = QStringLiteral("K; numeric raster stores (difference + 100 K) × 100 as uint16, 65535 = no data");
            method = QStringLiteral("Strongly negative over clear sky and thin cloud; close to zero over thick cold cloud; values at or above zero are the usual flag for tops reaching above the water-vapour weighting level. Diagnostic only; colour scale saturates at −30 / +6 K.");
        } else if (code == QStringLiteral("CONV")) {
            result = nadirstation::products::convectiveCloudCandidates(tbb, ir3);
            suffix = QStringLiteral("CONV_IR1_IR3_ConvectiveCandidates.png");
            title = QStringLiteral("Convective cloud and overshooting-top candidates");
            units = QStringLiteral("categorical: 0 other, 1 cold top (<=233.15 K), 2 deep-convection candidate, 3 overshooting-top candidate, 255 invalid");
            method = QStringLiteral("Class 2: IR1 <= 218.15 K and (IR3 − IR1) >= −5 K. Class 3: IR1 <= 208.15 K and (IR3 − IR1) >= 0 K. Conventional values, not fitted to this instrument; at 5 km sampling overshooting tops are often sub-pixel.");
        } else if (code == QStringLiteral("CLL")) {
            const QImage mask = (!ir2.isNull() && !ir3.isNull() && !ir4.isNull())
                ? nadirstation::products::cloudDetectionFromTbb(tbb, ir2, ir3, ir4) : QImage();
            result = nadirstation::products::cloudLevelFromTbb(tbb, mask);
            suffix = QStringLiteral("CLL_CloudLevel_ISCCP.png");
            title = QStringLiteral("Cloud level (low / mid / high)");
            units = QStringLiteral("categorical: 0 clear, 1 low (>680 hPa), 2 mid (440–680 hPa), 3 high (<440 hPa), 255 invalid");
            method = mask.isNull()
                ? QStringLiteral("Single-channel: cloud where IR1 <= 273.15 K, so the low class is almost empty. Cloud-top pressure from a standard atmosphere; semi-transparent cloud is placed too low.")
                : QStringLiteral("Cloud from the 4-channel mask; cloud-top pressure from IR1 through a standard atmosphere, so semi-transparent cloud is placed too low.");
            productStatus = QStringLiteral("diagnostic_candidate_unfitted_thresholds");
            postProcess = false;
        } else if (code == QStringLiteral("FOGN")) {
            numeric = nadirstation::products::nightFogCandidatesFromTbb(tbb, ir4);
            result = QImage(numeric.size(), QImage::Format_RGB32);
            for (int y = 0; y < numeric.height(); ++y) {
                const auto* src = reinterpret_cast<const quint16*>(numeric.constScanLine(y));
                auto* dst = reinterpret_cast<QRgb*>(result.scanLine(y));
                for (int x = 0; x < numeric.width(); ++x) {
                    if (src[x] == 65535) { dst[x] = qRgb(30, 38, 46); continue; }
                    const double t = qBound(0.0, (src[x] - 20.0) / 80.0, 1.0);
                    dst[x] = qRgb(qRound(60 + 195 * t), qRound(150 + 105 * t), qRound(190 + 65 * t));
                }
            }
            suffix = QStringLiteral("FOG_Night_IR1mIR4_Enhanced.png");
            numericSuffix = QStringLiteral("FOG_Night_IR1mIR4_Numeric.png");
            title = QStringLiteral("Night fog / low-stratus candidates (IR1 − IR4)");
            units = QStringLiteral("numeric raster: confidence 20–100, 65535 = no candidate");
            method = QStringLiteral("Warm top (265–295 K) with IR1 − IR4 >= 1.5 K. Valid at night only: reflected sunlight raises IR4 in daylight and the test then under-detects. No reflectance raster is used here, so daytime pixels are not masked.");
        } else if (code == QStringLiteral("WVE")) {
            result = nadirstation::products::waterVapourColourEnhancement(ir3);
            suffix = QStringLiteral("WV_IR3_ColorEnhanced.png");
            title = QStringLiteral("Water-vapour channel colour enhancement (IR3)");
            units = QStringLiteral("display only; cold / moist white–blue, warm / dry orange–brown");
            method = QStringLiteral("Colour map of calibrated IR3 brightness temperature, 200–285 K. Display product, no thresholds.");
        } else if (code == QStringLiteral("CLD4")) {
            result = nadirstation::products::cloudDetectionFromTbb(tbb, ir2, ir3, ir4);
            suffix = QStringLiteral("CLD_Multispectral_IR1to4.png");
            title = QStringLiteral("Multispectral cloud mask (IR1–IR4)");
            units = QStringLiteral("categorical: 0 clear, 1 cloud candidate, 255 invalid");
            method = QStringLiteral("Cold-top, thin-cirrus (split window) and local-contrast tests on calibrated TBB. Fixed thresholds; sensitive to cold land and viewing angle.");
        } else if (code == QStringLiteral("CLC4")) {
            result = nadirstation::products::cloudClassificationFromTbb(tbb, ir2, ir3, ir4);
            suffix = QStringLiteral("CLC_Multispectral_IR1to4.png");
            title = QStringLiteral("Multispectral cloud classes (IR1–IR4)");
            units = QStringLiteral("categorical: 0 clear, 1 cloud, 2 cold (<=235 K), 3 warmer (<=273.15 K), 4 thin-cirrus candidate, 255 invalid");
            method = QStringLiteral("Classes from the same tests as the 4-channel cloud mask. Fixed thresholds; no land/sea separation of the clear class.");
        } else if (code == QStringLiteral("CTT")) {
            result = nadirstation::products::cloudTopTemperatureFromTbb(tbb);
            suffix = QStringLiteral("CTT_cloud_only_0p01K.png");
            title = QStringLiteral("Cloud-top brightness temperature mask");
            units = QStringLiteral("0.01 K; zero denotes clear or invalid pixels");
            method = QStringLiteral("IR1 TBB retained at pixels <=273.15 K; single-channel threshold proxy, not an operational cloud-top retrieval.");
        } else if (code == QStringLiteral("CLC")) {
            result = nadirstation::products::cloudClassificationFromTbb(tbb);
            suffix = QStringLiteral("CLC_thermal_proxy.png");
            title = QStringLiteral("Single-channel thermal cloud-class proxy");
            units = QStringLiteral("categorical RGB classes");
            method = QStringLiteral("TBB thresholds: clear/warm >273.15 K; low/warm 260–273.15 K; mid 235–260 K; cold/high <=235 K. Land and high terrain can be misclassified.");
        } else if (code == QStringLiteral("CFR")) {
            result = nadirstation::products::cloudFractionFromTbb(tbb);
            suffix = QStringLiteral("CFR_thermal_32px.png");
            title = QStringLiteral("Cloud fraction per 32x32 source-pixel cell");
            units = QStringLiteral("percent (0–100), grayscale");
            method = QStringLiteral("Fraction of valid IR1 samples <=273.15 K in each 32x32 cell; threshold proxy, not the official CFR retrieval.");
        } else {
            result = nadirstation::products::cloudDetectionFromTbb(tbb);
            suffix = QStringLiteral("CLD_IR1_threshold_proxy.png");
            title = QStringLiteral("IR1 single-channel cloud detection proxy");
            units = QStringLiteral("grayscale mask: 255 cloud, 72 clear, 0 invalid");
            method = QStringLiteral("Cloud if calibrated IR1 TBB <=273.15 K. This simple threshold is sensitive to surface temperature and viewing conditions.");
        }
        if (result.isNull()) {
            statusBar()->showMessage(QStringLiteral("Generating the derived product failed."), 6000);
            return;
        }
        const bool encodedRaster = result.format() == QImage::Format_Grayscale16 ||
            result.format() == QImage::Format_Indexed8 || code == QStringLiteral("CFR") ||
            code == QStringLiteral("CLC") || code == QStringLiteral("CLD");
        if (postProcess) {
            result = encodedRaster ? nadirstation::products::repairMissingLinesMedian(result, 2)
                                   : nadirstation::products::repairMissingLines(result, 8);
            result = nadirstation::products::medianBlurPreservingFormat(result, 3);
        }
        const QString outPath = QDir(QFileInfo(tbbPath).absolutePath()).filePath(suffix);
        if (!result.save(outPath, "PNG")) {
            statusBar()->showMessage(QStringLiteral("Cannot save the derived product: %1").arg(outPath), 7000);
            return;
        }
        QJsonObject metadata;
        metadata[QStringLiteral("format")] = QStringLiteral("FY2-Derived-Product-1");
        metadata[QStringLiteral("product_code")] = code;
        metadata[QStringLiteral("product_name")] = title;
        metadata[QStringLiteral("source_product")] = QFileInfo(tbbPath).fileName();
        metadata[QStringLiteral("units")] = units;
        metadata[QStringLiteral("method_and_limitations")] = method;
        metadata[QStringLiteral("status")] = productStatus;
        if (!numericSuffix.isEmpty()) metadata[QStringLiteral("numeric_companion")] = numericSuffix;
        QFile sidecar(outPath + QStringLiteral(".json"));
        if (sidecar.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            sidecar.write(QJsonDocument(metadata).toJson(QJsonDocument::Indented));
            sidecar.close();
        }
        QStringList paths = currentPaths_;
        if (!paths.contains(outPath)) paths << outPath;
        if (!numeric.isNull() && !numericSuffix.isEmpty()) {
            const QString numericPath = QDir(QFileInfo(tbbPath).absolutePath()).filePath(numericSuffix);
            if (numeric.save(numericPath, "PNG") && !paths.contains(numericPath)) paths << numericPath;
        }
        auto* item = new QListWidgetItem(currentTimestamp_ + QStringLiteral(" · ") + title);
        item->setData(Qt::UserRole, paths);
        sessions_->addItem(item);
        sessions_->setCurrentItem(item);
        loadImagePaths(paths, item->text());
        product_->setCurrentIndex(paths.indexOf(outPath));
        statusBar()->showMessage(QStringLiteral("Generated %1; the JSON notes the limits of this experimental threshold product. %2")
            .arg(title, outPath), 9000);
    });
    rl->addSpacing(8);
    auto* note = label(QStringLiteral("Regional products use the selected satellite and its position from the TLE at observation time. "
                                      "Numeric TBB images show the calibrated temperature under the cursor; display copies carry a grayscale legend."), "Note");
    note->setWordWrap(true);
    rl->addWidget(note);
    rl->addStretch();
    for (QComboBox* box : rightContent->findChildren<QComboBox*>()) {
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(6);
    }
    auto* right = new QFrame;
    right->setObjectName(QStringLiteral("Card"));
    right->setFixedWidth(300);
    auto* rightLayout = new QVBoxLayout(right);
    rightLayout->setContentsMargins(0, 0, 0, 0);
    rightLayout->addWidget(panelScroll(rightContent));
    root->addWidget(right);
    return page;
}

void MainWindow::onImagePreviewReady(const QString& timestamp, const QList<QString>& paths) {
    if (!preview_ || paths.isEmpty()) return;
    QString preferredPath=currentPaths_.value(product_?product_->currentIndex():-1);
    if(!paths.contains(preferredPath))preferredPath=paths.first();
    const QImage img(preferredPath);
    if (img.isNull()) {   // only a failure is worth a line: previews arrive about once a second
        nadirstation::log::write(QStringLiteral("GUI preview could not be read: %1").arg(preferredPath));
        return;
    }
    QImage displayImg=img;
    if(selectedSatelliteName_==QStringLiteral("GK-2A HRIT")) {
        // A partial full-disk quicklook is mostly empty rows. Show the received strip
        // at its natural aspect ratio while the remaining segments are arriving.
        if(img.format()==QImage::Format_Grayscale8 &&
           preferredPath.endsWith(QStringLiteral("_preview.png"),Qt::CaseInsensitive)){
            int last=-1;
            for(int y=img.height()-1;y>=0 && last<0;--y){
                const auto* row=img.constScanLine(y);
                for(int x=0;x<img.width();++x)if(row[x]>2){last=y;break;}
            }
            if(last>=0 && last+32<img.height())displayImg=img.copy(0,0,img.width(),last+32);
        }
        currentTimestamp_=timestamp+QStringLiteral(" · GK-2A HRIT · live");
        currentImage_=displayImg; currentImagePath_=preferredPath; currentPaths_=paths;
        if(product_){
            const QSignalBlocker block(product_);product_->clear();
            for(const auto& path:paths){
                const QString base=QFileInfo(path).completeBaseName();
                product_->addItem(base.startsWith(QStringLiteral("GK2A_HRIT_"))
                    ?base.mid(10).replace(QLatin1Char('_'),QLatin1Char(' '))
                    :QString(base).replace(QLatin1Char('_'),QLatin1Char(' ')));
            }
            product_->setCurrentIndex(int(std::max<qsizetype>(0,paths.indexOf(preferredPath))));
        }
        applyImageProcessing();
    }
    // Keep the placeholder centered while waiting. Once packet rows arrive, pin the image
    // to the top so each longer preview reveals the next rows below it.
    preview_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    preview_->setPixmap(QPixmap::fromImage(displayImg.scaled(preview_->width(), preview_->height(),
        Qt::KeepAspectRatio, Qt::SmoothTransformation)));
}

// Live GK-2A HRIT visible-channel view while segments arrive. Drawn into the preview label only:
// nothing is written to disk and nothing is added to the image page (that happens once, when the
// observation is complete, with the full-resolution channel images and L2 products).
void MainWindow::onHritLivePreview(const QString& timestamp, const QImage& image, const QString& info) {
    if (!preview_ || image.isNull() ||
        (selectedSatelliteName_ != QStringLiteral("GK-2A HRIT") && selectedSatelliteName_ != QStringLiteral("GOES GVAR") && selectedSatelliteName_ != QStringLiteral("GOES SD")
         && !selectedSatelliteName_.startsWith(QStringLiteral("ELEKTRO-L LRIT"))
         && !selectedSatelliteName_.startsWith(QStringLiteral("ELEKTRO-L HRIT"))
         && !selectedSatelliteName_.startsWith(QStringLiteral("MetOp"))
         && !selectedSatelliteName_.startsWith(QStringLiteral("METEOR")))) return;
    preview_->setAlignment(Qt::AlignTop | Qt::AlignHCenter);
    preview_->setPixmap(QPixmap::fromImage(image.scaled(std::max(1, preview_->width()), std::max(1, preview_->height()),
        Qt::KeepAspectRatio, Qt::SmoothTransformation)));
    if (!info.isEmpty()) statusBar()->showMessage(QStringLiteral("%1 · live preview · %2").arg(timestamp, info), 4000);
}

void MainWindow::onImageReady(const QString& timestamp, const QList<QString>& paths) {
    nadirstation::log::write(QStringLiteral("GUI onImageReady timestamp=%1 paths=%2").arg(timestamp).arg(paths.size()));
    if(paths.isEmpty())return;
    currentTimestamp_ = timestamp;
    QStringList imagePaths;
    QString visPath;
    const bool hritSession=selectedSatelliteName_==QStringLiteral("GK-2A HRIT") ||
        QFileInfo::exists(QDir(QFileInfo(paths.first()).absolutePath()).filePath(QStringLiteral("GK2A_Observation.json")));
    QList<QString> sourcePaths=paths;
    if(hritSession){
        const QString observationDir=QFileInfo(paths.first()).absolutePath();
        QFile manifest(QDir(observationDir).filePath(QStringLiteral("GK2A_Observation.json")));
        if(manifest.open(QIODevice::ReadOnly)){
            const QJsonObject observation=QJsonDocument::fromJson(manifest.readAll()).object();
            QList<QString> fromJson;
            for(const auto& entry:observation.value(QStringLiteral("products")).toArray()){
                const QString relative=entry.toObject().value(QStringLiteral("path")).toString();
                const QString candidate=QDir(observationDir).filePath(relative);
                if(!relative.isEmpty() && QFileInfo::exists(candidate))fromJson.append(candidate);
            }
            if(!fromJson.isEmpty())sourcePaths=fromJson;
        }
    }
    for (const QString& path : sourcePaths) {
        const QString fileName = QFileInfo(path).fileName();
        if (fileName.compare(QStringLiteral("Preview_CH1_VIS.png"), Qt::CaseInsensitive) == 0)
            continue;
        // HRIT quicklooks are for the live screen only. Finished observations
        // retain their original .hrit files in five channel folders and import
        // the derived full-disk products into the session list.
        if(hritSession && fileName.endsWith(QStringLiteral("_preview.png"),Qt::CaseInsensitive))
            continue;
        imagePaths.append(path);
        if (fileName.compare(QStringLiteral("CH1_VIS.png"), Qt::CaseInsensitive) == 0)
            visPath = path;
    }
    if(imagePaths.isEmpty()){
        statusBar()->showMessage(QStringLiteral("Raw HRIT saved, but the product images were not generated; check the output folder and the log."),10000);
        return;
    }
    if (visPath.isEmpty() && !imagePaths.isEmpty())
        visPath = imagePaths.first();
    const QString directory = imagePaths.isEmpty() ? QString() : QFileInfo(imagePaths.first()).absolutePath();
    QListWidgetItem* item = nullptr;
    for (int row = 0; row < sessions_->count(); ++row) {
        QListWidgetItem* candidate = sessions_->item(row);
        if (candidate->data(Qt::UserRole + 2).toString() == directory) {
            item = candidate;
            break;
        }
    }
    if (!item) {
        item = new QListWidgetItem(timestamp);
        sessions_->addItem(item);
        // Bound the list. Each item is small -- a timestamp and a list of path strings -- so this
        // is not about memory but about a list that otherwise grows for as long as the station is
        // left running, one entry per session, with no way to reach the recent ones.
        //
        // Only the list entry is dropped. The directory and its products stay on disk: discarding
        // a user's observations to tidy a list would trade their data for our convenience, and the
        // count of what was dropped is shown instead.
        constexpr int kMaxSessionItems = 200;
        while (sessions_->count() > kMaxSessionItems) {
            const int removed = sessions_->count() - kMaxSessionItems;
            for (int i = 0; i < removed; ++i)
                delete sessions_->takeItem(0);
            droppedSessionCount_ += removed;
            if (sessionsHint_ != nullptr) {
                sessionsHint_->setText(QStringLiteral(
                    "Showing the most recent %1 sessions; %2 older ones remain on disk.")
                    .arg(kMaxSessionItems).arg(droppedSessionCount_));
            }
        }
    }
    item->setText(timestamp);
    if(hritSession)item->setToolTip(QStringLiteral("Raw HRIT segments: %1/{VI006,SW038,WV069,IR105,IR123}/IMG_FD_*.hrit")
        .arg(directory));
    item->setData(Qt::UserRole, imagePaths);
    item->setData(Qt::UserRole + 1, visPath);
    item->setData(Qt::UserRole + 2, directory);
    sessions_->setCurrentItem(item);
    if(hritSession){
        // Prefer the IR 10.5 channel as the first view of a GK-2A observation.
        const QString ir=QDir(directory).filePath(QStringLiteral("GK2A_HRIT_IR105.png"));
        if(imagePaths.contains(ir))visPath=ir;
        item->setData(Qt::UserRole + 1, visPath);
    }
    loadImagePaths(imagePaths, timestamp, visPath);
    if(hritSession){
        hritShowOnFinalize_=false;
        // No automatic jump to the image page: the finished observation is imported and selected in the list, and the
        // page the user is on stays as it is.
        statusBar()->showMessage(QStringLiteral("GK-2A HRIT %1 imported automatically: %2 images (five channels + L2), JSON: %3")
            .arg(timestamp).arg(imagePaths.size())
            .arg(QDir(directory).filePath(QStringLiteral("GK2A_Observation.json"))),12000);
    }
    if(QFileInfo::exists(QDir(directory).filePath(QStringLiteral("GVAR_Observation.json")))){
        // GOES GVAR: the false colour is the first view; imported and selected, without switching pages.
        statusBar()->showMessage(QStringLiteral("GOES GVAR %1 imported automatically: %2 images (false colour + 5 raw channels), JSON: %3")
            .arg(timestamp).arg(imagePaths.size())
            .arg(QDir(directory).filePath(QStringLiteral("GVAR_Observation.json"))),12000);
    }
    if(QFileInfo::exists(QDir(directory).filePath(QStringLiteral("GOES_SD_Observation.json")))){
        statusBar()->showMessage(QStringLiteral("GOES SD %1 imported automatically: %2 images (false colour, visible, IR detector groups), JSON: %3")
            .arg(timestamp).arg(imagePaths.size())
            .arg(QDir(directory).filePath(QStringLiteral("GOES_SD_Observation.json"))),12000);
    }
    nadirstation::log::write(QStringLiteral("GUI products saved and loaded; keeping the current page selected"));
    // Generate lon/lat products as soon as an observation is finalized. If
    // Calibration-2 arrives later, the decoder emits imageReady again after
    // converting saved counts, and the same observation's selector is updated.
    //
    // Only for an observation that carries FY-2 channels. The regional products are built from
    // CH1_VIS and CH2_IR4 and the rest of SEVIRI-style set, and a GK-2A observation has a disk and
    // its strips instead -- handing it those paths killed the process with an access violation, at
    // this line, every time. A guard rather than an implementation: regional products from a
    // geostationary disk are a different piece of work, and until they exist this must not be given
    // input it was never written for.
    bool hasFy2Channels = false;
    for (const QString& path : imagePaths) {
        if (QFileInfo(path).fileName().contains(QStringLiteral("CH1_VIS"))
            || QFileInfo(path).fileName().contains(QStringLiteral("CH2_IR4"))) {
            hasFy2Channels = true;
            break;
        }
    }
    if (hasFy2Channels) {
        generateRegionalProductsForObservation(timestamp, imagePaths);
    } else {
        static bool noted = false;
        if (!noted) {
            noted = true;
            nadirstation::log::write(QStringLiteral(
                "GUI: no FY-2 channels in this observation, so no regional products are built "
                "from it"));
        }
    }
}

void MainWindow::generateRegionalProductsForObservation(const QString& timestamp,
                                                        const QList<QString>& sourcePaths) {
    if (sourcePaths.isEmpty()) return;
    const QString directory = QFileInfo(sourcePaths.first()).absolutePath();
    const QString manifestPath = observationManifestPath(QDir(directory));
    QJsonObject manifest;
    QFile manifestFile(manifestPath);
    if (manifestFile.open(QIODevice::ReadOnly | QIODevice::Text))
        manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    const QJsonObject gcp = manifest.value(QStringLiteral("navigation_gcp")).toObject();
    const QString norad = selectedSatelliteName_ == QStringLiteral("FY-2H")
        ? QStringLiteral("43491") : QStringLiteral("40367");
    QStringList arguments{directory,
        QString::number(projLat_->value(), 'f', 4), QString::number(projLon_->value(), 'f', 4), norad};
    const bool gcpMatchesObservation = gcp.value(QStringLiteral("validated_observation_only")).toString()
        == QFileInfo(directory).fileName();
    const int shiftX = gcpMatchesObservation
        ? gcp.value(QStringLiteral("projected_translation_x_pixels")).toInt(0) : 0;
    const int shiftY = gcpMatchesObservation
        ? gcp.value(QStringLiteral("projected_translation_y_pixels")).toInt(0) : 0;
    if (shiftX != 0 || shiftY != 0) {
        arguments << QString::number(shiftX) << QString::number(shiftY)
            << QString::number(gcp.value(QStringLiteral("latitude_deg")).toDouble(), 'f', 5)
            << QString::number(gcp.value(QStringLiteral("longitude_deg")).toDouble(), 'f', 5)
            << QString::number(gcp.value(QStringLiteral("observed_source_x")).toInt())
            << QString::number(gcp.value(QStringLiteral("observed_source_y")).toInt());
    }
    const QString executable = QDir(QCoreApplication::applicationDirPath())
        .filePath(QStringLiteral("NadirStation_RegionalProducts.exe"));
    auto* process = new QProcess(this);
    process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    connect(process, &QProcess::errorOccurred, this, [this, process, executable](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart) return;
        statusBar()->showMessage(QStringLiteral("The regional-product generator could not start: %1").arg(executable), 9000);
        process->deleteLater();
    });
    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
        [this, process, directory, timestamp, sourcePaths](int exitCode, QProcess::ExitStatus exitStatus) {
            const QString output = QString::fromLocal8Bit(process->readAllStandardOutput()).trimmed();
            const QString error = QString::fromLocal8Bit(process->readAllStandardError()).trimmed();
            process->deleteLater();
            if (exitStatus != QProcess::NormalExit || exitCode != 0) {
                statusBar()->showMessage(QStringLiteral("Automatic regional-product generation failed: %1").arg(error), 10000);
                return;
            }
            QJsonObject updated;
            QFile updatedManifest(observationManifestPath(QDir(directory)));
            if (updatedManifest.open(QIODevice::ReadOnly | QIODevice::Text))
                updated = QJsonDocument::fromJson(updatedManifest.readAll()).object();
            QStringList paths;
            for (const QString& path : sourcePaths)
                if (QFileInfo::exists(path) && !paths.contains(path)) paths.append(path);
            for (const QJsonValue& value : updated.value(QStringLiteral("products")).toArray()) {
                const QString name = value.toObject().value(QStringLiteral("path")).toString();
                if (!name.startsWith(QStringLiteral("Region_"), Qt::CaseInsensitive)) continue;
                const QString path = QDir(directory).filePath(name);
                if (QFileInfo::exists(path) && !paths.contains(path)) paths.append(path);
            }
            if (paths.size() == sourcePaths.size()) {
                statusBar()->showMessage(output.isEmpty()
                    ? QStringLiteral("Observation processed; there is no regional product to project.") : output, 10000);
                return;
            }
            QListWidgetItem* item = nullptr;
            for (int row = 0; row < sessions_->count(); ++row) {
                QListWidgetItem* candidate = sessions_->item(row);
                if (candidate->data(Qt::UserRole + 2).toString() == directory) {
                    item = candidate;
                    break;
                }
            }
            if (!item) {
                item = new QListWidgetItem(timestamp + QStringLiteral(" · Regional products"));
                sessions_->addItem(item);
            }
            item->setData(Qt::UserRole, paths);
            item->setData(Qt::UserRole + 2, directory);
            sessions_->setCurrentItem(item);
            const QString preferred = QDir(directory).filePath(QStringLiteral("Region_VIS_Reflectance_Enhanced.png"));
            loadImagePaths(paths, timestamp + QStringLiteral(" · Regional products"), preferred);
            const QString report = output.isEmpty()
                ? QStringLiteral("Equirectangular products were generated automatically; switch to them in the image product list.") : output;
            statusBar()->showMessage(report, 12000);
        });
    process->start(executable, arguments);
    statusBar()->showMessage(QStringLiteral("Generating equirectangular products for the selected satellite and observation time…"), 8000);
}

void MainWindow::selectImage(QListWidgetItem* item) {
    if (!item) return;
    const QStringList paths = item->data(Qt::UserRole).toStringList();
    loadImagePaths(paths, item->text(), item->data(Qt::UserRole + 1).toString());
}

void MainWindow::loadImagePaths(const QStringList& paths, const QString& timestamp, const QString& preferredPath) {
    if (!timestamp.isEmpty()) currentTimestamp_ = timestamp;
    if (product_) {
        const QHash<QString, QString> productNames{
            {QStringLiteral("GK2A_HRIT_VI006_preview"), QStringLiteral("GK-2A HRIT · VIS 0.6 µm")},
            {QStringLiteral("GK2A_HRIT_SW038_preview"), QStringLiteral("GK-2A HRIT · SWIR 3.8 µm")},
            {QStringLiteral("GK2A_HRIT_WV069_preview"), QStringLiteral("GK-2A HRIT · WV 6.9 µm")},
            {QStringLiteral("GK2A_HRIT_IR105_preview"), QStringLiteral("GK-2A HRIT · IR 10.5 µm")},
            {QStringLiteral("GK2A_HRIT_IR123_preview"), QStringLiteral("GK-2A HRIT · IR 12.3 µm")},
            {QStringLiteral("GK2A_HRIT_VI006"), QStringLiteral("GK-2A HRIT · CH VI006 · VIS 0.64 µm")},
            {QStringLiteral("GK2A_HRIT_SW038"), QStringLiteral("GK-2A HRIT · CH SW038 · SWIR 3.8 µm")},
            {QStringLiteral("GK2A_HRIT_WV069"), QStringLiteral("GK-2A HRIT · CH WV069 · WV 6.9 µm")},
            {QStringLiteral("GK2A_HRIT_IR105"), QStringLiteral("GK-2A HRIT · CH IR105 · IR 10.5 µm")},
            {QStringLiteral("GK2A_HRIT_IR123"), QStringLiteral("GK-2A HRIT · CH IR123 · IR 12.3 µm")},
            {QStringLiteral("GK2A_RGB_FalseColor"), QStringLiteral("GK-2A RGB · False colour (calibrated)")},
            {QStringLiteral("GK2A_RGB_DayMicrophysics"), QStringLiteral("GK-2A RGB · Day microphysics")},
            {QStringLiteral("GK2A_RGB_NightMicrophysics"), QStringLiteral("GK-2A RGB · Night microphysics")},
            {QStringLiteral("GK2A_L2_CloudTopTemp_IR105"), QStringLiteral("GK-2A L2 · Cloud-top temperature (IR 10.5 µm, K)")},
            {QStringLiteral("GK2A_L2_WaterVapor_WV069"), QStringLiteral("GK-2A L2 · Water vapour BT (6.9 µm, K)")},
            {QStringLiteral("GK2A_L2_SplitWindow_BTD_105-123"), QStringLiteral("GK-2A L2 · Split-window BTD 10.5−12.3 µm")},
            {QStringLiteral("GK2A_L2_ConvectiveTops_BTD_WV069-IR105"), QStringLiteral("GK-2A L2 · Convective tops BTD 6.9−10.5 µm")},
            {QStringLiteral("GK2A_L2_FogLowCloud_BTD_SW038-IR105"), QStringLiteral("GK-2A L2 · Fog / low cloud BTD 3.8−10.5 µm")},
            {QStringLiteral("GK2A_L2_VisibleReflectance_VI006"), QStringLiteral("GK-2A L2 · Visible reflectance (0.64 µm)")},
            {QStringLiteral("GK2A_L2_CloudMask_Threshold"), QStringLiteral("GK-2A L2 · Cloud mask (threshold)")},
            {QStringLiteral("GK2A_Proj_VI006_view"), QStringLiteral("GK-2A projection · VI006 albedo (calibrated)")},
            {QStringLiteral("GK2A_Proj_SW038_view"), QStringLiteral("GK-2A projection · SW038 BT (calibrated)")},
            {QStringLiteral("GK2A_Proj_WV069_view"), QStringLiteral("GK-2A projection · WV069 BT (calibrated)")},
            {QStringLiteral("GK2A_Proj_IR105_view"), QStringLiteral("GK-2A projection · IR105 BT (calibrated)")},
            {QStringLiteral("GK2A_Proj_IR123_view"), QStringLiteral("GK-2A projection · IR123 BT (calibrated)")},
            {QStringLiteral("CH1_VIS"), QStringLiteral("CH1 VIS")},
            {QStringLiteral("Preview_CH1_VIS"), QStringLiteral("Preview · CH1 VIS")},
            {QStringLiteral("CH2_IR4"), QStringLiteral("CH2 IR4")},
            {QStringLiteral("CH3_IR3"), QStringLiteral("CH3 IR3")},
            {QStringLiteral("CH4_IR1"), QStringLiteral("CH4 IR1")},
            {QStringLiteral("CH5_IR2"), QStringLiteral("CH5 IR2")},
            {QStringLiteral("GK2A_IR_raw"), QStringLiteral("GK-2A IR · Raw counts")},
            {QStringLiteral("GK2A_IR_enhanced"), QStringLiteral("GK-2A IR · Calibrated enhanced (hover for K/°C)")},
            {QStringLiteral("GK2A_MCir"), QStringLiteral("GK-2A MCIR · Full disk")},
            {QStringLiteral("GK2A_MCir_Region"), QStringLiteral("GK-2A MCIR · Imported region")},
            {QStringLiteral("GK2A_IR_Region_enhanced"), QStringLiteral("GK-2A IR · Selected region (hover for K/°C)")},
            {QStringLiteral("CH4_IR1_Counts10"), QStringLiteral("CH4 IR1 · Native 10-bit counts")},
            {QStringLiteral("CH5_IR2_Counts10"), QStringLiteral("CH5 IR2 · Native 10-bit counts")},
            {QStringLiteral("CH3_IR3_Counts10"), QStringLiteral("CH3 IR3 · Native 10-bit counts")},
            {QStringLiteral("CH2_IR4_Counts10"), QStringLiteral("CH2 IR4 · Native 10-bit counts")},
            {QStringLiteral("CH4_IR1_TBB_0p01K"), QStringLiteral("CH4 IR1 · Brightness temperature (0.01 K)")},
            {QStringLiteral("CH5_IR2_TBB_0p01K"), QStringLiteral("CH5 IR2 · Brightness temperature (0.01 K)")},
            {QStringLiteral("CH3_IR3_TBB_0p01K"), QStringLiteral("CH3 IR3 · Brightness temperature (0.01 K)")},
            {QStringLiteral("CH2_IR4_TBB_0p01K"), QStringLiteral("CH2 IR4 · Brightness temperature (0.01 K)")},
            {QStringLiteral("CH4_IR1_TBB_Enhanced"), QStringLiteral("CH4 IR1 · TBB display (cold clouds bright)")},
            {QStringLiteral("CH4_IR1_Longwave_ColorEnhanced"), QStringLiteral("Longwave IR1 · color-enhanced brightness temperature")},
            {QStringLiteral("CH5_IR2_TBB_Enhanced"), QStringLiteral("CH5 IR2 · TBB display (cold clouds bright)")},
            {QStringLiteral("CH3_IR3_TBB_Enhanced"), QStringLiteral("CH3 IR3 · TBB display (cold clouds bright)")},
            {QStringLiteral("CH2_IR4_TBB_Enhanced"), QStringLiteral("CH2 IR4 · TBB display (cold clouds bright)")},
            {QStringLiteral("Region_VIS_ReflectanceFactor_0p01pct"), QStringLiteral("Regional VIS · reflectance factor")},
            {QStringLiteral("Region_VIS_Reflectance_Enhanced"), QStringLiteral("Regional VIS · enhanced reflectance")},
            {QStringLiteral("Region_CH4_IR1_TBB_0p01K"), QStringLiteral("Regional IR1 · brightness temperature")},
            {QStringLiteral("Region_CH5_IR2_TBB_0p01K"), QStringLiteral("Regional IR2 · brightness temperature")},
            {QStringLiteral("Region_CH3_IR3_TBB_0p01K"), QStringLiteral("Regional IR3 · brightness temperature")},
            {QStringLiteral("Region_CH2_IR4_TBB_0p01K"), QStringLiteral("Regional IR4 · brightness temperature")},
            {QStringLiteral("Region_CH4_IR1_TBB_Enhanced"), QStringLiteral("Regional IR1 · TBB display (cold bright; hover for K/°C)")},
            {QStringLiteral("Region_CH5_IR2_TBB_Enhanced"), QStringLiteral("Regional IR2 · TBB display (cold bright; hover for K/°C)")},
            {QStringLiteral("Region_CH3_IR3_TBB_Enhanced"), QStringLiteral("Regional IR3 · TBB display (cold bright; hover for K/°C)")},
            {QStringLiteral("Region_CH2_IR4_TBB_Enhanced"), QStringLiteral("Regional IR4 · TBB display (cold bright; hover for K/°C)")},
            {QStringLiteral("Region_CLD_CloudMask"), QStringLiteral("Regional CLD · multispectral cloud candidates")},
            {QStringLiteral("Region_CLC_ExperimentalGrouping"), QStringLiteral("Regional CLC · experimental cloud grouping")},
            {QStringLiteral("Region_CTT_IR1_TbbCandidate_0p01K"), QStringLiteral("Regional CTT · color map (numeric temperature on hover)")},
            {QStringLiteral("Region_CTT_IR1_TbbCandidate_Enhanced"), QStringLiteral("Regional CTT · temperature color map (hover for K/°C)")},
            {QStringLiteral("Region_CFR_2x2_CloudFractionPercent"), QStringLiteral("Regional CTA1 legacy · 2×2 cloud amount (%)")},
            {QStringLiteral("Region_CFR_3x3Sliding_CloudFractionPercent"), QStringLiteral("Regional CFR · 3×3 sliding cloud fraction (%)")},
            {QStringLiteral("Region_OLR_Enhanced"), QStringLiteral("Regional OLR · EXPERIMENTAL regression, not validated (r≈0.22, RMSE≈49 W/m²)")},
            {QStringLiteral("Region_PRE_Enhanced"), QStringLiteral("Regional PRE · calibrated 1-hour estimate (mm)")},
            {QStringLiteral("Region_UTH_Enhanced"), QStringLiteral("Regional UTH · calibrated estimate (%)")},
            {QStringLiteral("Region_SST_Enhanced"), QStringLiteral("Regional SST · official 3-hour temperature (°C)")},
            {QStringLiteral("Region_SST_Candidate_Enhanced"), QStringLiteral("SST candidate · clear-ocean split-window proxy (K)")},
            {QStringLiteral("Region_SST_MCSST_Enhanced"), QStringLiteral("Regional SST · MCSST, buoy-validated (day RMSE 0.8 K)")},
            {QStringLiteral("IR_Enhanced"), QStringLiteral("IR Enhanced")},
            {QStringLiteral("GOES_SD_FalseColor"), QStringLiteral("GOES SD · False colour (VIS × IR group 2, uncalibrated)")},
            {QStringLiteral("GOES_SD_VIS"), QStringLiteral("GOES SD · Visible 0.65 µm · raw counts")},
            {QStringLiteral("GOES_SD_VIS_Enhanced"), QStringLiteral("GOES SD · Visible 0.65 µm · enhanced")},
            {QStringLiteral("GOES_SD_IR1"), QStringLiteral("GOES SD · IR group 1 · raw (colder = brighter)")},
            {QStringLiteral("GOES_SD_IR2"), QStringLiteral("GOES SD · IR group 2 (window, ~10.7 µm) · raw")},
            {QStringLiteral("GOES_SD_IR3"), QStringLiteral("GOES SD · IR group 3 · raw (colder = brighter)")},
            {QStringLiteral("GOES_SD_IR1_Enhanced"), QStringLiteral("GOES SD · IR group 1 · enhanced")},
            {QStringLiteral("GOES_SD_IR2_Enhanced"), QStringLiteral("GOES SD · IR group 2 (window) · colour enhanced")},
            {QStringLiteral("GOES_SD_IR3_Enhanced"), QStringLiteral("GOES SD · IR group 3 · enhanced")},
            {QStringLiteral("GVAR_FalseColor"), QStringLiteral("GOES GVAR · False colour (VIS × IR 10.7, SatDump palette)")},
            {QStringLiteral("GVAR_VIS_0p65um"), QStringLiteral("GOES GVAR · CH1 VIS 0.65 µm · raw image")},
            {QStringLiteral("GVAR_IR2_3p9um"), QStringLiteral("GOES GVAR · CH2 IR 3.9 µm · raw image")},
            {QStringLiteral("GVAR_IR3_6p5um"), QStringLiteral("GOES GVAR · CH3 water vapour 6.5 µm · raw image")},
            {QStringLiteral("GVAR_IR4_10p7um"), QStringLiteral("GOES GVAR · CH4 IR 10.7 µm · raw image")},
            {QStringLiteral("GVAR_IR5_13p3um"), QStringLiteral("GOES GVAR · CH5 IR 13.3 µm · raw image")},
            {QStringLiteral("FalseColor"), QStringLiteral("False colour · VIS + IR4 (daytime)")},
            {QStringLiteral("GLOBAL_FalseColor"), QStringLiteral("False colour · VIS + IR4 (full disk)")},
            {QStringLiteral("VIS_ReflectanceFactor_Region"), QStringLiteral("Regional VIS Reflectance Factor")},
            {QStringLiteral("VIS_Reflectance_Enhanced_Region"), QStringLiteral("Regional VIS Reflectance Enhanced")},
            {QStringLiteral("BTD_IR1mIR2_SplitWindow_Enhanced"), QStringLiteral("Split-window difference · IR1 − IR2 (K)")},
            {QStringLiteral("BTD_IR3mIR1_WVminusIRW_Enhanced"), QStringLiteral("Water vapour − window · IR3 − IR1 (K)")},
            {QStringLiteral("CONV_IR1_IR3_ConvectiveCandidates"), QStringLiteral("Convective / overshooting-top candidates")},
            {QStringLiteral("CLL_CloudLevel_ISCCP"), QStringLiteral("Cloud level · low / mid / high")},
            {QStringLiteral("FOG_Night_IR1mIR4_Enhanced"), QStringLiteral("Night fog candidates · IR1 − IR4")},
            {QStringLiteral("WV_IR3_ColorEnhanced"), QStringLiteral("Water vapour · colour enhancement (IR3)")},
            {QStringLiteral("CLD_Multispectral_IR1to4"), QStringLiteral("Cloud mask · multispectral (IR1–IR4)")},
            {QStringLiteral("CLC_Multispectral_IR1to4"), QStringLiteral("Cloud classes · multispectral (IR1–IR4)")},
            {QStringLiteral("VIS_ReflectanceFactor_x100"), QStringLiteral("VIS Reflectance Factor · x100%")},
            {QStringLiteral("VIS_Reflectance_Enhanced"), QStringLiteral("VIS Reflectance · Enhanced")},
            {QStringLiteral("VIS_Reflectance_Color"), QStringLiteral("VIS Reflectance · Color")}
        };
        const QSignalBlocker blocker(product_);
        product_->clear();
        QStringList visiblePaths;
        for (const auto& path : paths) {
            const QString baseName = QFileInfo(path).completeBaseName();
            if (baseName == QStringLiteral("Region_OLR_StefanBoltzmannProxy_0p1Wm2") ||
                baseName == QStringLiteral("Region_PRE_ConvectivePotentialIndex") ||
                baseName == QStringLiteral("Region_UTH_WaterVaporProxyIndex") ||
                baseName == QStringLiteral("Region_SST_ClearOceanSplitWindowProxy_0p01K") ||
                baseName == QStringLiteral("Region_OLR_StefanBoltzmannProxy_Enhanced") ||
                baseName == QStringLiteral("Region_OLR_Official_0p1Wm2") ||
                baseName == QStringLiteral("Region_OLR_Calibrated_0p1Wm2") ||
                baseName == QStringLiteral("Region_PRE_Official_0p01mm") ||
                baseName == QStringLiteral("Region_PRE_Calibrated_0p01mm") ||
                baseName == QStringLiteral("Region_UTH_Official_1pct") ||
                baseName == QStringLiteral("Region_UTH_Calibrated_1pct") ||
                baseName == QStringLiteral("Region_SST_Official_0p01C") ||
                baseName == QStringLiteral("Region_SST_MCSST_QualityFlags") ||
                // Numeric brightness temperature: written for the hover to read, not a picture to
                // look at. Listing it would offer a grey rectangle beside the coloured map of the
                // same data and invite the reader to wonder which one they wanted.
                baseName.endsWith(QStringLiteral("_0p01K")) ||
                // Derived-product numeric companions (BTD, night fog): data, not pictures.
                baseName.endsWith(QStringLiteral("_Numeric")))
                continue;
            visiblePaths.append(path);
            QString displayName = baseName.endsWith(QStringLiteral("_MCIR_projected"),
                                                           Qt::CaseInsensitive)
                ? QStringLiteral("GK-2A MCIR · Selected region (no borders)")
                : productNames.value(baseName, baseName);
            // ELEKTRO-L L2 / RGB and the polar HRPT pictures: readable names from the file names.
            static const QHash<QString, QString> extraNames{
                {QStringLiteral("L2_CloudTopTemp_IR107_TBB_Enhanced"), QStringLiteral("L2 · Cloud-top temperature (10.7 µm, hover for K/°C)")},
                {QStringLiteral("L2_WaterVapour_WV64_TBB_Enhanced"), QStringLiteral("L2 · Water vapour BT (6.4 µm, hover for K/°C)")},
                {QStringLiteral("L2_SplitWindow_BTD_107-119"), QStringLiteral("L2 · Split-window BTD 10.7−11.9 µm")},
                {QStringLiteral("L2_FogLowCloud_BTD_38-107"), QStringLiteral("L2 · Fog / low cloud BTD 3.8−10.7 µm")},
                {QStringLiteral("L2_ConvectiveTops_BTD_64-107"), QStringLiteral("L2 · Convective tops BTD 6.4−10.7 µm")},
                {QStringLiteral("L2_VisibleAlbedo_VIS06"), QStringLiteral("L2 · Visible albedo (0.6 µm)")},
                {QStringLiteral("L2_CloudMask"), QStringLiteral("L2 · Cloud mask")},
                {QStringLiteral("L2_CloudLevel"), QStringLiteral("L2 · Cloud level (high / middle / low)")},
                {QStringLiteral("RGB_Airmass"), QStringLiteral("RGB · Airmass")},
                {QStringLiteral("RGB_Dust"), QStringLiteral("RGB · Dust")},
                {QStringLiteral("RGB_NightMicrophysics"), QStringLiteral("RGB · Night microphysics")},
                {QStringLiteral("RGB_221_FalseColour"), QStringLiteral("RGB · 221 false colour")},
                {QStringLiteral("RGB_321_NaturalColour"), QStringLiteral("RGB · 321 natural colour")},
                {QStringLiteral("RGB_IR_FalseColour"), QStringLiteral("RGB · IR false colour (3.7, 10.8, 12 µm)")},
                {QStringLiteral("IR108_Grey"), QStringLiteral("10.8 µm infrared (cold bright)")},
                {QStringLiteral("IR108_Enhanced"), QStringLiteral("10.8 µm infrared, cold tops coloured")}};
            static const QRegularExpression elektroL2(QStringLiteral("^ELEKTRO-L_(LRIT|HRIT)_[0-9]{8}_[0-9]{4}_(.+)$"));
            static const QRegularExpression polarName(QStringLiteral("^(AVHRR|MSUMR)_(.+)$"));
            if (const auto m = elektroL2.match(baseName); m.hasMatch())
                displayName = QStringLiteral("ELEKTRO-L %1 · %2").arg(m.captured(1), extraNames.value(m.captured(2), m.captured(2)));
            else if (const auto q = polarName.match(baseName); q.hasMatch()) {
                const QString inst = q.captured(1) == QLatin1String("AVHRR") ? QStringLiteral("MetOp AVHRR") : QStringLiteral("METEOR MSU-MR");
                const QString rest = q.captured(2);
                displayName = rest.startsWith(QStringLiteral("CH")) ? QStringLiteral("%1 · channel %2").arg(inst, rest.mid(2))
                                                                     : QStringLiteral("%1 · %2").arg(inst, extraNames.value(rest, rest));
            } else if (baseName.startsWith(QStringLiteral("RGB_")) && !extraNames.contains(baseName))
                displayName = QStringLiteral("RGB formula · %1").arg(baseName.mid(4).replace(QLatin1Char('_'), QLatin1Char(' ')));
            product_->addItem(displayName);
        }
        currentPaths_ = visiblePaths;
        const int preferredIndex = preferredPath.isEmpty() ? -1 : visiblePaths.indexOf(preferredPath);
        product_->setCurrentIndex(preferredIndex >= 0 ? preferredIndex : 0);
    }
    if (sessions_ && sessions_->currentItem() && product_ && product_->currentIndex() >= 0)
        sessions_->currentItem()->setData(Qt::UserRole + 1, currentPaths_.value(product_->currentIndex()));
    refreshRgbChannels();
    applyImageProcessing();
}

QList<std::pair<QString, std::pair<QString, QString>>> MainWindow::rgbChannelFiles(const QStringList& paths) {
    QList<std::pair<QString, std::pair<QString, QString>>> out;   // name -> (path, description)
    auto add = [&](const QString& name, const QString& path, const QString& what) {
        for (const auto& e : out) if (e.first == name) return;
        out.append({name, {path, what}});
    };
    static const QRegularExpression polarRx(QStringLiteral("^(AVHRR|MSUMR)_CH([0-9][AB]?)$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression fy2Rx(QStringLiteral("^CH([1-5])_(VIS|IR[1-4])$"), QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression elektroRx(QStringLiteral("^ELEKTRO-L_(?:LRIT|HRIT)_([0-9]+\\.[0-9]+)um_[0-9_]+(?:_partial)?$"));
    static const QHash<QString, QString> polarWhat{{QStringLiteral("1"), QStringLiteral("0.6 µm")}, {QStringLiteral("2"), QStringLiteral("0.8 µm")},
        {QStringLiteral("3A"), QStringLiteral("1.6 µm")}, {QStringLiteral("3B"), QStringLiteral("3.7 µm")}};
    std::vector<std::pair<double, QString>> elektro;
    for (const QString& path : paths) {
        const QString base = QFileInfo(path).completeBaseName();
        if (auto m = polarRx.match(base); m.hasMatch()) {
            const bool avhrr = m.captured(1).compare(QStringLiteral("AVHRR"), Qt::CaseInsensitive) == 0;
            const QString n = m.captured(2).toUpper();
            static const char* msu[] = {"", "0.6 µm", "0.8 µm", "1.6 µm", "3.7 µm", "10.8 µm", "12.0 µm"};
            static const QHash<QString, QString> av{{QStringLiteral("1"), QStringLiteral("0.63 µm")}, {QStringLiteral("2"), QStringLiteral("0.86 µm")},
                {QStringLiteral("3A"), QStringLiteral("1.6 µm")}, {QStringLiteral("3B"), QStringLiteral("3.7 µm")},
                {QStringLiteral("4"), QStringLiteral("10.8 µm")}, {QStringLiteral("5"), QStringLiteral("12.0 µm")}};
            const int k = n.left(1).toInt();
            add(QStringLiteral("ch") + n.toLower(), path, avhrr ? av.value(n) : QString::fromUtf8(msu[std::clamp(k, 0, 6)]));
        } else if (auto f = fy2Rx.match(base); f.hasMatch()) {
            add(QStringLiteral("ch") + f.captured(1), path, f.captured(2).toUpper());
        } else if (base.startsWith(QStringLiteral("GK2A_HRIT_")) && !base.endsWith(QStringLiteral("_preview"))) {
            static const QStringList order{QStringLiteral("VI006"), QStringLiteral("SW038"), QStringLiteral("WV069"), QStringLiteral("IR105"), QStringLiteral("IR123")};
            const int k = order.indexOf(base.mid(10));
            if (k >= 0) add(QStringLiteral("ch%1").arg(k + 1), path, base.mid(10));
        } else if (base.startsWith(QStringLiteral("GVAR_")) && base.contains(QStringLiteral("um"))) {
            static const QStringList order{QStringLiteral("GVAR_VIS_0p65um"), QStringLiteral("GVAR_IR2_3p9um"), QStringLiteral("GVAR_IR3_6p5um"),
                                           QStringLiteral("GVAR_IR4_10p7um"), QStringLiteral("GVAR_IR5_13p3um")};
            const int k = order.indexOf(base);
            if (k >= 0) add(QStringLiteral("ch%1").arg(k + 1), path, base.mid(5).replace(QLatin1Char('_'), QLatin1Char(' ')).replace(QStringLiteral("p"), QStringLiteral(".")));
        } else if (auto e = elektroRx.match(base); e.hasMatch()) {
            elektro.emplace_back(e.captured(1).toDouble(), path);
        }
    }
    // ELEKTRO-L MSU-GS: channels numbered by wavelength (0.6 … 11.9 µm is ch1 … ch10 when all are there).
    std::sort(elektro.begin(), elektro.end());
    for (std::size_t i = 0; i < elektro.size(); ++i)
        add(QStringLiteral("ch%1").arg(i + 1), elektro[i].second,
            QStringLiteral("%1 µm%2").arg(elektro[i].first).arg(elektro[i].first >= 3.4 ? QStringLiteral(", cold bright") : QString()));
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) {
        const int na = a.first.mid(2).toInt(), nb = b.first.mid(2).toInt();
        return na != nb ? na < nb : a.first < b.first;
    });
    return out;
}

void MainWindow::refreshRgbChannels() {
    if (!rgbChannels_) return;
    const auto files = rgbChannelFiles(currentPaths_);
    if (files.isEmpty()) {
        rgbChannels_->setText(QStringLiteral("This observation has no single-channel images to combine."));
        return;
    }
    QStringList parts;
    for (const auto& [name, info] : files) parts << QStringLiteral("%1 %2").arg(name, info.second);
    rgbChannels_->setText(QStringLiteral("Channels: ") + parts.join(QStringLiteral(" · ")));
}

void MainWindow::composeRgbFromFormula() {
    if (!rgbFormula_) return;
    const QString formula = rgbFormula_->text().trimmed().isEmpty() ? rgbFormula_->placeholderText() : rgbFormula_->text().trimmed();
    const auto files = rgbChannelFiles(currentPaths_);
    if (files.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Load an observation with channel images first."), 7000);
        return;
    }
    std::map<QString, QImage> channels;
    for (const auto& [name, info] : files) {
        // Only the channels the formula names are read from disk.
        if (!formula.contains(QRegularExpression(QStringLiteral("\\b%1\\b").arg(name), QRegularExpression::CaseInsensitiveOption))) {
            channels[name] = QImage(1, 1, QImage::Format_Grayscale8);   // a name the parser knows, never used
            continue;
        }
        QImage img(info.first);
        if (img.isNull()) {
            statusBar()->showMessage(QStringLiteral("Cannot read %1 (%2).").arg(name, info.first), 8000);
            return;
        }
        channels[name] = img;
    }
    QString error;
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QImage out = nadirstation::polar::composeRgb(formula, channels, rgbStretch_ && rgbStretch_->isChecked(), &error);
    QApplication::restoreOverrideCursor();
    if (out.isNull()) {
        statusBar()->showMessage(QStringLiteral("RGB formula: %1").arg(error), 10000);
        return;
    }
    QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation")).setValue(QStringLiteral("images/rgbFormula"), formula);
    QString tag = formula.toLower();
    tag.replace(QRegularExpression(QStringLiteral("[^a-z0-9.]+")), QStringLiteral("_"));
    tag = tag.left(60);
    while (tag.endsWith(QLatin1Char('_'))) tag.chop(1);
    while (tag.startsWith(QLatin1Char('_'))) tag.remove(0, 1);
    const QString dir = QFileInfo(files.first().second.first).absolutePath();
    const QString outPath = QDir(dir).filePath(QStringLiteral("RGB_%1.png").arg(tag));
    if (!out.save(outPath, "PNG")) {
        statusBar()->showMessage(QStringLiteral("Saving %1 failed.").arg(outPath), 8000);
        return;
    }
    QStringList paths = currentPaths_;
    paths.removeAll(outPath);
    paths << outPath;
    if (sessions_ && sessions_->currentItem()) {
        sessions_->currentItem()->setData(Qt::UserRole, paths);
        sessions_->currentItem()->setData(Qt::UserRole + 1, outPath);
    }
    loadImagePaths(paths, currentTimestamp_, outPath);
    statusBar()->showMessage(QStringLiteral("RGB composite %1 saved: %2").arg(formula, outPath), 9000);
}

void MainWindow::loadCurrentPath(const QString& path) {
    if (path.isEmpty()) return;
    if (hoverTemperaturePopup_) hoverTemperaturePopup_->hide();
    hoverTbbImage_ = {};
    hoverTbbLabel_.clear();
    const QFileInfo selectedInfo(path);
    QString displayPath = path;
    QString tbbName = selectedInfo.completeBaseName();
    if (tbbName.contains(QStringLiteral("_Longwave_ColorEnhanced"), Qt::CaseInsensitive))
        tbbName.replace(QStringLiteral("_Longwave_ColorEnhanced"), QStringLiteral("_TBB_0p01K"), Qt::CaseInsensitive);
    else if (tbbName.contains(QStringLiteral("_TBB_Enhanced"), Qt::CaseInsensitive))
        tbbName.replace(QStringLiteral("_TBB_Enhanced"), QStringLiteral("_TBB_0p01K"), Qt::CaseInsensitive);
    if (tbbName.contains(QStringLiteral("_CTT_IR1_TbbCandidate_Enhanced"), Qt::CaseInsensitive))
        tbbName.replace(QStringLiteral("_CTT_IR1_TbbCandidate_Enhanced"),
                        QStringLiteral("_CTT_IR1_TbbCandidate_0p01K"), Qt::CaseInsensitive);
    bool isNumericTbb = tbbName.contains(QStringLiteral("_TBB_0p01K"), Qt::CaseInsensitive) ||
        tbbName.contains(QStringLiteral("TbbCandidate_0p01K"), Qt::CaseInsensitive);
    // GK-2A names its products differently from the FY-2 ones this was written against, so the test
    // above never armed the hover for them -- hovering a brightness temperature showed nothing at
    // all. Each GK-2A picture is matched to the numeric image written beside it, in the same pixel
    // space: the disk products to the disk one, the projected products to the projected one.
    if (!isNumericTbb && tbbName.startsWith(QStringLiteral("GK2A"))) {
        if (tbbName.contains(QStringLiteral("_MCIR_Region"), Qt::CaseInsensitive)) {
            tbbName = QStringLiteral("GK2A_IR_Region_0p01K");
            isNumericTbb = true;
        } else if (tbbName.contains(QStringLiteral("_TBB_projected"), Qt::CaseInsensitive) ||
            tbbName.contains(QStringLiteral("_MCIR_projected"), Qt::CaseInsensitive)) {
            tbbName = QStringLiteral("GK2A_TBB_0p01K");
            isNumericTbb = true;
        } else if (tbbName.contains(QStringLiteral("_IR_Region_enhanced"), Qt::CaseInsensitive)) {
            tbbName = QStringLiteral("GK2A_IR_Region_0p01K");
            isNumericTbb = true;
        } else if (tbbName.contains(QStringLiteral("_IR_raw"), Qt::CaseInsensitive) ||
                   tbbName.contains(QStringLiteral("_IR_enhanced"), Qt::CaseInsensitive) ||
                   tbbName.contains(QStringLiteral("_IR_FD"), Qt::CaseInsensitive) ||
                   tbbName.endsWith(QStringLiteral("_MCIR"), Qt::CaseInsensitive)) {
            tbbName = QStringLiteral("GK2A_IR_0p01K");
            isNumericTbb = true;
        }
    }
    if (selectedInfo.completeBaseName().contains(QStringLiteral("_CTT_IR1_TbbCandidate_0p01K"), Qt::CaseInsensitive)) {
        QString cttDisplayName = selectedInfo.completeBaseName();
        cttDisplayName.replace(QStringLiteral("_0p01K"), QStringLiteral("_Enhanced"), Qt::CaseInsensitive);
        const QString candidateDisplayPath = selectedInfo.dir().filePath(cttDisplayName + QStringLiteral(".png"));
        if (QFileInfo::exists(candidateDisplayPath)) displayPath = candidateDisplayPath;
    }
    if (isNumericTbb) {
        QString tbbPath = selectedInfo.dir().filePath(tbbName + QStringLiteral(".png"));
        if (!QFileInfo::exists(tbbPath)) tbbPath = path;
        QImageReader temperatureReader(tbbPath);
        hoverTbbImage_ = temperatureReader.read();
        if (!hoverTbbImage_.isNull() && hoverTbbImage_.format() != QImage::Format_Grayscale16)
            hoverTbbImage_ = hoverTbbImage_.convertToFormat(QImage::Format_Grayscale16);
        const auto channel = QRegularExpression(QStringLiteral("CH[2345]_IR[1-4]"), QRegularExpression::CaseInsensitiveOption)
            .match(selectedInfo.completeBaseName());
        hoverTbbLabel_ = channel.hasMatch() ? channel.captured().replace('_', ' ')
                                        : (tbbName.startsWith(QStringLiteral("GK2A"))
                                               ? QStringLiteral("GK-2A IR105 brightness temperature")
                                               : QStringLiteral("IR1 cloud-top brightness temperature (candidate)"));
    }
    // FY-2 VIS is a large 8-bit PNG. Qt's default image allocation ceiling is
    // too low for some builds to decode it (the file header is readable, but
    // read() then fails with "Unable to read image data"). Allow enough room
    // for the decoder, while asking it to produce a view-sized image first.
    // Large products (the GVAR false colour is up to 20944 pixels wide) come with a display copy in .preview/;
    // decoding the full PNG on the GUI thread stalled the window. The original stays the file of record.
    {
        const QString previewPath = selectedInfo.dir().filePath(QStringLiteral(".preview/") + selectedInfo.completeBaseName() + QStringLiteral(".jpg"));
        if (displayPath == path && QFileInfo::exists(previewPath)) displayPath = previewPath;
    }
    QImageReader::setAllocationLimit(512);
    QImageReader reader(displayPath);
    const QSize originalSize = reader.size();
    // The GK-2A HRIT channel images (VI006 is 11000 x 11000) are shown at their original resolution; everything
    // else keeps the 4096-pixel limit.
    const bool hritChannel = selectedInfo.completeBaseName().startsWith(QStringLiteral("GK2A_HRIT_"), Qt::CaseInsensitive) &&
        !selectedInfo.completeBaseName().endsWith(QStringLiteral("_preview"), Qt::CaseInsensitive);
    const int displayLimit = hritChannel ? 12000 : 4096;
    if (originalSize.isValid() && (originalSize.width() > displayLimit || originalSize.height() > displayLimit)) {
        const QSize displaySize = originalSize.scaled(QSize(displayLimit, displayLimit), Qt::KeepAspectRatio);
        reader.setScaledSize(displaySize);
        currentImage_ = reader.read();
        if (currentImage_.isNull()) {
            // Some Qt PNG plugin versions cannot decode directly to a reduced
            // size. Retry at source size under the higher allocation ceiling,
            // then downscale once; keep the retry error for the diagnostic log.
            reader.setScaledSize(QSize());
            currentImage_ = reader.read();
            if (!currentImage_.isNull())
                currentImage_ = currentImage_.scaled(displaySize, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        }
    } else {
        currentImage_ = reader.read();
    }
    currentImagePath_ = path;
    nadirstation::log::write(QStringLiteral("GUI image read path=%1 original=%2x%3 shown=%4x%5 error=%6")
        .arg(path).arg(originalSize.width()).arg(originalSize.height())
        .arg(currentImage_.width()).arg(currentImage_.height())
        // QImageReader keeps "Unknown error" as its text even after a successful read, which made
        // every line of this log look like a failure.
        .arg(currentImage_.isNull() ? reader.errorString() : QStringLiteral("none")));
    if (currentImage_.isNull()) {
        imageInfo_->setText(QStringLiteral("Cannot read the image: %1 · %2")
            .arg(QFileInfo(path).fileName(), reader.errorString()));
        imageView_->setImage(QImage());
        return;
    }
    imageInfo_->setText(path);
    applyImageProcessing();
}

QImage MainWindow::processedCurrentImage() const {
    const QImage src = currentImage_;
    if (src.isNull()) return {};
    // An original-resolution HRIT channel (VI006, 11000 x 11000) is shown as received: the line repair and 3x3 median
    // below would soften it and take tens of seconds over 121 megapixels.
    if (src.width() > 4096 || src.height() > 4096) return src;
    const QString sourcePath = currentPaths_.value(product_ ? product_->currentIndex() : -1);
    const QString fileName = QFileInfo(sourcePath).completeBaseName();
    // The labelled projection pictures carry their own text and graticule; no repair or median pass.
    if (fileName.startsWith(QStringLiteral("GK2A_Proj_"), Qt::CaseInsensitive)) return src;
    // ELEKTRO-L xRIT images are shown as decoded: no median, and no line repair either (black rows
    // there are segments not received, which must not be painted over with their neighbours).
    if (fileName.startsWith(QStringLiteral("ELEKTRO-L_"), Qt::CaseInsensitive)) return src;
    const bool cloudFraction = fileName.contains(QStringLiteral("CFR"), Qt::CaseInsensitive);
    const bool temperatureRaster = fileName.contains(QStringLiteral("TBB"), Qt::CaseInsensitive) ||
        fileName.contains(QStringLiteral("CTT"), Qt::CaseInsensitive);
    int mapHeight = src.height();
    QFile sidecar(sourcePath + QStringLiteral(".json"));
    if (sidecar.open(QIODevice::ReadOnly)) {
        const QJsonObject metadata = QJsonDocument::fromJson(sidecar.readAll()).object();
        const int recordedHeight = metadata.value(QStringLiteral("map_height_pixels")).toInt();
        if (recordedHeight > 0) mapHeight = recordedHeight;
    }
    if (mapHeight == src.height() && fileName.contains(QStringLiteral("Region_"), Qt::CaseInsensitive) &&
        fileName.contains(QStringLiteral("Enhanced"), Qt::CaseInsensitive)) {
        const int footer = cloudFraction ? 78 : 74;
        if (src.height() > footer) mapHeight = src.height() - footer;
    }
    mapHeight = std::clamp(mapHeight, 1, src.height());
    QImage map = src.copy(0, 0, src.width(), mapHeight);
    if (map.format() == QImage::Format_Grayscale16) {
        if (temperatureRaster) map = nadirstation::products::repairMissingLinesMedian(map, 2);
    } else if (map.format() == QImage::Format_Indexed8 || cloudFraction) {
        map = nadirstation::products::repairMissingLinesMedian(map, 2);
    } else {
        map = nadirstation::products::repairMissingLines(map, 8);
    }
    map = nadirstation::products::medianBlurPreservingFormat(map, 3, nullptr, cloudFraction);
    if (mapHeight == src.height()) return map;
    QImage output = src.copy();
    QPainter painter(&output);
    painter.drawImage(0, 0, map);
    return output;
}

void MainWindow::importSessionJson() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import image JSON"), QString(),
        QStringLiteral("JSON files (*.json);;All files (*.*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        statusBar()->showMessage(QStringLiteral("Cannot open JSON: %1").arg(path), 5000);
        return;
    }
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    f.close();
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        statusBar()->showMessage(QStringLiteral("Invalid observation JSON: %1").arg(pe.errorString()), 5000);
        return;
    }
    const QJsonObject root = doc.object();
    const QString base = QFileInfo(path).absolutePath();
    QStringList paths;
    QJsonArray arr = root.value("products").toArray();
    if (arr.isEmpty()) arr = root.value("images").toArray();
    for (const QJsonValue& value : arr) {
        QString absolutePath;
        QString relativePath;
        if (value.isString()) {
            relativePath = value.toString();
        } else if (value.isObject()) {
            const QJsonObject product = value.toObject();
            absolutePath = product.value("absolute_path").toString();
            relativePath = product.value("path").toString();
            if (relativePath.isEmpty()) relativePath = product.value("file").toString();
        }
        QString productPath = absolutePath;
        if (productPath.isEmpty() || !QFileInfo::exists(productPath))
            productPath = QDir(base).filePath(relativePath);
        if (QFileInfo::exists(productPath)) {
            const QString resolved = QFileInfo(productPath).absoluteFilePath();
            if (!paths.contains(resolved)) paths << resolved;
        }
    }
    if (paths.isEmpty()) {
        statusBar()->showMessage("No product files found for this observation", 5000);
        return;
    }
    const QString stamp = root.value("created_utc").toString(
        root.value("observation_timestamp").toString(QFileInfo(path).completeBaseName()));
    currentTimestamp_ = stamp;
    currentPaths_ = paths;
    auto* item = new QListWidgetItem(stamp);
    item->setData(Qt::UserRole, paths);
    sessions_->addItem(item);
    sessions_->setCurrentItem(item);
    product_->setCurrentIndex(0);
    loadImagePaths(paths, stamp);
    switchPage(1);
    statusBar()->showMessage(QStringLiteral("Imported observation: %1 products").arg(paths.size()), 5000);
}

// On demand only: the five calibrated HRIT channels of the loaded observation resampled onto the region set above.
void MainWindow::projectHritFiveChannels() {
    QString observationDir;
    for (const QString& path : currentPaths_) {
        const QString candidate = QFileInfo(path).absolutePath();
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("GK2A_Observation.json")))) {
            observationDir = candidate;
            break;
        }
    }
    if (observationDir.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Select a GK-2A HRIT observation on the left first (its folder must contain GK2A_Observation.json)."), 8000);
        return;
    }
    if (hritProjectBusy_) {
        statusBar()->showMessage(QStringLiteral("The calibrated projection is being generated; please wait."), 4000);
        return;
    }
    hritProjectBusy_ = true;
    if (hritProjectButton_) hritProjectButton_->setEnabled(false);
    QImageReader::setAllocationLimit(512);   // the VI006 channel image is 242 MB when decoded
    const double lat = projLat_->value(), lon = projLon_->value(), span = projSpan_->value();
    nadirstation::log::write(QStringLiteral("HRIT projection start dir=%1 centre=%2,%3 span=%4")
        .arg(observationDir).arg(lat).arg(lon).arg(span));
    statusBar()->showMessage(QStringLiteral("Generating the HRIT five-channel calibrated projection; this takes tens of seconds and the interface stays usable…"));
    const QPointer<MainWindow> self(this);
    std::thread([self, observationDir, lat, lon, span] {
        QString message;
        const QStringList views = nadirstation::gk2a::HritImages::projectObservation(observationDir, lat, lon, span, message);
        QMetaObject::invokeMethod(qApp, [self, observationDir, views, message] {
            if (self) self->finishHritProjection(observationDir, views, message);
        }, Qt::QueuedConnection);
    }).detach();
}

void MainWindow::finishHritProjection(const QString& observationDir, const QStringList& views, const QString& message) {
    hritProjectBusy_ = false;
    if (hritProjectButton_) hritProjectButton_->setEnabled(true);
    nadirstation::log::write(QStringLiteral("HRIT projection done: %1").arg(message));
    if (views.isEmpty()) {
        statusBar()->showMessage(message, 15000);
        return;
    }
    // Show the result only if the observation it belongs to is still the one on screen.
    bool sameObservation = false;
    for (const QString& path : currentPaths_)
        if (QFileInfo(path).absolutePath() == observationDir) { sameObservation = true; break; }
    if (!sameObservation) {
        statusBar()->showMessage(message, 15000);
        return;
    }
    QStringList paths = currentPaths_;
    for (const QString& view : views)
        if (!paths.contains(view)) paths.append(view);
    loadImagePaths(paths, currentTimestamp_);
    if (sessions_ && sessions_->currentItem())
        sessions_->currentItem()->setData(Qt::UserRole, paths);
    const int index = paths.indexOf(views.first());
    if (product_ && index >= 0) product_->setCurrentIndex(index);
    statusBar()->showMessage(message, 15000);
}

void MainWindow::applyImageProcessing() {
    if (currentPaths_.isEmpty() && currentImage_.isNull()) return;
    const int idx = product_->currentIndex();
    const QString path = currentPaths_.value(idx);
    if (!path.isEmpty() && currentImagePath_ != path) {
        loadCurrentPath(path);
        return;
    }
    if (currentImage_.isNull()) {
        imageView_->setImage(QImage());
        return;
    }
    if (!path.isEmpty()) {
        imageInfo_->setText(QString("%1 · %2").arg(currentTimestamp_, path));
    }
    imageView_->setImage(processedCurrentImage());
}

// Save for a GK-2A HRIT observation: the five channel images, every L2 product and the JSON
// manifests go to one folder the user picks, plus a manifest of what was saved.
bool MainWindow::saveHritObservation() {
    QString observationDir;
    for(const QString& path:currentPaths_){
        const QString dir=QFileInfo(path).absolutePath();
        if(QFileInfo::exists(QDir(dir).filePath(QStringLiteral("GK2A_Observation.json")))){observationDir=dir;break;}
    }
    if(observationDir.isEmpty())return false;
    QFile manifestFile(QDir(observationDir).filePath(QStringLiteral("GK2A_Observation.json")));
    if(!manifestFile.open(QIODevice::ReadOnly))return false;
    const QJsonObject observation=QJsonDocument::fromJson(manifestFile.readAll()).object();
    manifestFile.close();
    const QString stamp=observation.value(QStringLiteral("observation_timestamp")).toString(QFileInfo(observationDir).fileName());
    const QString target=QFileDialog::getExistingDirectory(this,
        QStringLiteral("Save GK-2A HRIT observation %1 (five channels + L2 + JSON)").arg(stamp), QDir::homePath());
    if(target.isEmpty())return true;
    const QDir out(QDir(target).filePath(QStringLiteral("GK2A_HRIT_")+stamp));
    if(!QDir().mkpath(out.absolutePath())){
        statusBar()->showMessage(QStringLiteral("Cannot create the folder: %1").arg(out.absolutePath()),8000);return true;
    }
    QStringList files;
    for(const auto& value:observation.value(QStringLiteral("products")).toArray())
        files.append(value.toObject().value(QStringLiteral("path")).toString());
    files<<QStringLiteral("GK2A_Observation.json")<<QStringLiteral("GK2A_Products_Metadata.json");
    int channels=0,l2=0,copied=0;QStringList missing;
    QJsonArray saved;
    for(const QString& name:files){
        if(name.isEmpty())continue;
        const QString from=QDir(observationDir).filePath(name);
        if(!QFileInfo::exists(from)){if(!name.endsWith(QStringLiteral("Metadata.json")))missing<<name;continue;}
        const QString to=out.filePath(name);
        QFile::remove(to);
        if(!QFile::copy(from,to)){missing<<name;continue;}
        ++copied;saved.append(name);
        if(name.startsWith(QStringLiteral("GK2A_HRIT_"))&&name.endsWith(QStringLiteral(".png")))++channels;
        else if(name.endsWith(QStringLiteral(".png")))++l2;
    }
    QJsonObject record{{QStringLiteral("format"),QStringLiteral("NadirStation-GK2A-HRIT-Saved-1")},
                       {QStringLiteral("created_utc"),QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs)},
                       {QStringLiteral("observation_timestamp"),stamp},
                       {QStringLiteral("source_directory"),observationDir},
                       {QStringLiteral("channel_images"),channels},{QStringLiteral("l2_images"),l2},
                       {QStringLiteral("files"),saved}};
    QFile jf(out.filePath(QStringLiteral("GK2A_Saved.json")));
    if(jf.open(QIODevice::WriteOnly|QIODevice::Truncate|QIODevice::Text)){
        jf.write(QJsonDocument(record).toJson(QJsonDocument::Indented));jf.close();
    }
    statusBar()->showMessage(missing.isEmpty()
        ? QStringLiteral("Saved to %1: %2 channels, %3 L2 images, JSON").arg(out.absolutePath()).arg(channels).arg(l2)
        : QStringLiteral("Saved to %1: %2 channels, %3 L2 images, JSON; missing %4").arg(out.absolutePath()).arg(channels).arg(l2).arg(missing.join(QStringLiteral(", "))),
        12000);
    return true;
}

void MainWindow::saveCurrentImage() {
    if(saveHritObservation())return;
    const QString sourcePath = currentPaths_.value(product_->currentIndex());
    const QImage source = sourcePath.isEmpty() ? currentImage_ : QImage(sourcePath);
    QImage out = source.isNull() ? QImage() : source.copy();
    if (!out.isNull()) {
        out = out.format() == QImage::Format_Grayscale16 || out.format() == QImage::Format_Indexed8
            ? nadirstation::products::repairMissingLinesMedian(out, 2)
            : nadirstation::products::repairMissingLines(out, 8);
        out = nadirstation::products::medianBlurPreservingFormat(out, 3);
    }
    if (out.isNull()) return;
    const QString path = QFileDialog::getSaveFileName(this, "Save Enhanced Image", "NADIR_enhanced.png", "PNG (*.png);;TIFF (*.tif *.tiff)");
    if (path.isEmpty()) return;
    if (out.save(path)) {
        QJsonObject sidecar;
        sidecar["format"] = QStringLiteral("NadirStation-Enhanced-Image-1");
        sidecar["created_utc"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
        sidecar["observation_timestamp"] = currentTimestamp_;
        sidecar["source_product"] = currentPaths_.value(product_->currentIndex());
        sidecar["saved_enhanced_image"] = QFileInfo(path).absoluteFilePath();
        sidecar["product_index"] = product_->currentIndex();
        sidecar["product_name"] = product_->currentText();
        QFile jf(path + QStringLiteral(".json"));
        if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
            jf.write(QJsonDocument(sidecar).toJson(QJsonDocument::Indented));
            jf.close();
        }
        statusBar()->showMessage("Saved: " + path + " + JSON", 5000);
    }
}

// ---------------------------------------------------------------------------------------------
// Instrument / image pickers over the full product list.
QString MainWindow::productGroupOf(const QString& path) {
    const QString b = QFileInfo(path).completeBaseName();
    struct Rule { const char* prefix; const char* group; };
    static const Rule rules[] = {
        {"AVHRR_", "AVHRR/3"}, {"MHS_", "MHS"}, {"AMSU_", "AMSU-A"}, {"IASI_IIS", "IASI Imaging"},
        {"IASI_", "IASI"}, {"ASCAT_", "ASCAT"}, {"GOME2_", "GOME-2"}, {"SEM_", "SEM"},
        {"MSUMR_", "MSU-MR"}, {"MTVZA_", "MTVZA"}};
    for (const auto& r : rules)
        if (b.startsWith(QLatin1String(r.prefix))) return QString::fromLatin1(r.group);
    if (b.startsWith(QLatin1String("RGB_")) || b.contains(QLatin1String("_RGB_"))) return QStringLiteral("RGB composites");
    if (b.contains(QLatin1String("L2_")) || b.contains(QLatin1String("Region_")) || b.contains(QLatin1String("_TBB")))
        return QStringLiteral("L2 / products");
    return QStringLiteral("Imager");
}

void MainWindow::rebuildProductPickers() {
    if (!product_ || !instrumentPick_ || !imagePick_) return;
    const int cur = product_->currentIndex();
    const QString curGroup = cur >= 0 && cur < currentPaths_.size() ? productGroupOf(currentPaths_.at(cur)) : QString();
    QStringList groups;
    for (int i = 0; i < product_->count(); ++i) {
        const QString g = i < currentPaths_.size() ? productGroupOf(currentPaths_.at(i)) : QStringLiteral("Imager");
        if (!groups.contains(g)) groups << g;
    }
    static const QStringList order{QStringLiteral("Imager"), QStringLiteral("AVHRR/3"), QStringLiteral("MSU-MR"),
        QStringLiteral("MHS"), QStringLiteral("AMSU-A"), QStringLiteral("IASI Imaging"), QStringLiteral("IASI"),
        QStringLiteral("ASCAT"), QStringLiteral("GOME-2"), QStringLiteral("SEM"), QStringLiteral("MTVZA"),
        QStringLiteral("L2 / products"), QStringLiteral("RGB composites")};
    std::stable_sort(groups.begin(), groups.end(), [](const QString& a, const QString& b) {
        const auto ia = order.indexOf(a), ib = order.indexOf(b);
        return (ia < 0 ? 99 : ia) < (ib < 0 ? 99 : ib);
    });
    {
        const QSignalBlocker b(instrumentPick_);
        instrumentPick_->clear();
        instrumentPick_->addItems(groups);
        instrumentPick_->setCurrentIndex(std::max<qsizetype>(0, groups.indexOf(curGroup)));
        instrumentPick_->setEnabled(groups.size() > 1);
    }
    fillImagePick();
    const QSignalBlocker b(imagePick_);
    for (int i = 0; i < imagePick_->count(); ++i)
        if (imagePick_->itemData(i).toInt() == cur) { imagePick_->setCurrentIndex(i); break; }
}

void MainWindow::fillImagePick() {
    const QString group = instrumentPick_->currentText();
    const QSignalBlocker b(imagePick_);
    imagePick_->clear();
    for (int i = 0; i < product_->count(); ++i) {
        const QString g = i < currentPaths_.size() ? productGroupOf(currentPaths_.at(i)) : QStringLiteral("Imager");
        if (g == group) imagePick_->addItem(product_->itemText(i), i);
    }
}
