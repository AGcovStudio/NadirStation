#include "globeview.h"

#include "nowcast.h"
#include "busyoverlay.h"

#include <QCheckBox>
#include <QDirIterator>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QSignalBlocker>
#include <QTreeWidget>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QLineEdit>
#include <QSettings>
#include <QComboBox>
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QImageReader>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPointer>
#include <QPainterPath>
#include <QPushButton>
#include <QShowEvent>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <memory>
#include <thread>
#include <vector>

using namespace nadirstation::globe;

namespace {
constexpr double kPi = 3.14159265358979323846;

// Geostationary satellites this station receives, with the sub-satellite longitudes the
// satellite selector uses.
struct GeoSat { const char* name; double lon; };
constexpr GeoSat kSats[] = {
    {"FY-2H", 79.0}, {"FY-2G", 99.2}, {"GK-2A", 128.2}, {"ELEKTRO-L N4", 165.8},
};

Rgb8 toRgb8(const QImage& src) {
    Rgb8 out;
    if (src.isNull()) return out;
    const QImage im = src.convertToFormat(QImage::Format_RGB888);
    out.width = im.width();
    out.height = im.height();
    out.data.resize(static_cast<std::size_t>(out.width) * out.height * 3);
    for (int y = 0; y < out.height; ++y)
        std::copy_n(im.constScanLine(y), out.width * 3, out.data.data() + static_cast<std::size_t>(y) * out.width * 3);
    return out;
}
}  // namespace

GlobeView::GlobeView(QWidget* parent) : QWidget(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumSize(320, 240);
    setCursor(Qt::OpenHandCursor);
    refine_ = new QTimer(this);
    refine_->setSingleShot(true);
    refine_->setInterval(160);
    connect(refine_, &QTimer::timeout, this, [this] { frameStep_ = 1; frameDirty_ = true; update(); });
    scene_.layer = Layer::TrueColour;
    resetView();
}

void GlobeView::ensureRelief() {
    if (reliefTried_) return;
    reliefTried_ = true;
    // The relief map the GK-2A products already use for their background, read at 7200 x 3600
    // (about 3 km a pixel at the equator) -- enough for a close view without 200 MB in memory.
    relief_ = readRelief();
    scene_.relief = relief_.empty() ? nullptr : &relief_;
}

Rgb8 GlobeView::readRelief() {
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("resources/maps/world_relief.jpg"));
    QImageReader reader(path);
    reader.setScaledSize(QSize(7200, 3600));
    return toRgb8(reader.read());
}

void GlobeView::adoptRelief(Rgb8&& relief) {
    reliefTried_ = true;
    relief_ = std::move(relief);
    scene_.relief = relief_.empty() ? nullptr : &relief_;
    frameDirty_ = true;
    update();
}

namespace {
// Disk geometry from the limb, as gk2a::measureDisk does from the raw counts: the widest extent
// through the middle rows is the diameter, and the Earth's angular radius from geostationary height
// (8.70 degrees) over that radius is the scale. `filled(x, y)` says whether a pixel is on the Earth.
template <typename Filled>
bool measureLimb(int width, int height, Filled filled, DiskGeometry* g) {
    int minX = width, maxX = -1;
    const int mid = height / 2;
    for (int y = std::max(0, mid - 40); y <= std::min(height - 1, mid + 40); ++y)
        for (int x = 0; x < width; ++x)
            if (filled(x, y)) { minX = std::min(minX, x); maxX = std::max(maxX, x); }
    if (maxX <= minX) return false;
    const double limb = (maxX - minX + 1) / 2.0;
    g->subLonDeg = 128.2;
    g->centre = height / 2.0;
    g->radPerPixel = std::asin(6378.137 / 42164.0) / limb;
    return true;
}
}  // namespace

bool GlobeView::loadObservation(const QString& folder, const QString& productFile, const GeoBox* geo) {
    ensureRelief();
    const QDir dir(folder);
    // The brightness temperature of the observation, when the folder has one: the clouds of the
    // true-colour layer, the point query and the forecast all come from it.
    tbb_ = Grey16{};
    observed_ = Grey16{};
    scene_.tbb = nullptr;
    scene_.disk = DiskGeometry{};
    scene_.geo = GeoBox{};
    const QString tbbPath = dir.filePath(QStringLiteral("GK2A_IR_0p01K.png"));
    QImage tbb = cachedTbbPath_ == tbbPath && !cachedTbb_.isNull() ? cachedTbb_ : QImage(tbbPath);
    cachedTbb_ = QImage();
    cachedTbbPath_.clear();
    if (!tbb.isNull()) {
        tbb = tbb.convertToFormat(QImage::Format_Grayscale16);
        Grey16 g;
        g.width = tbb.width();
        g.height = tbb.height();
        g.data.resize(static_cast<std::size_t>(g.width) * g.height);
        for (int y = 0; y < g.height; ++y) {
            const auto* line = reinterpret_cast<const std::uint16_t*>(tbb.constScanLine(y));
            std::copy_n(line, g.width, g.data.data() + static_cast<std::size_t>(y) * g.width);
        }
        DiskGeometry d;
        if (measureLimb(g.width, g.height, [&](int x, int y) { return g.data[static_cast<std::size_t>(y) * g.width + x] != 0; }, &d)) {
            tbb_ = std::move(g);
            observed_ = tbb_;
            scene_.tbb = &tbb_;
            scene_.disk = d;
        }
    }

    product_ = Rgb8{};
    scene_.product = nullptr;
    if (!productFile.isEmpty()) {
        QImage prod(dir.filePath(productFile));
        if (prod.isNull()) return false;
        prod = prod.convertToFormat(QImage::Format_RGB32);   // grey products are indexed
        if (geo && geo->valid()) {
            // A lat/lon (regional) product: placed by its world file, no disk geometry needed.
            scene_.geo = *geo;
        } else {
            // A disk product. With a temperature raster of the same size the disk is the same;
            // otherwise (an older folder without one, or another size) it is measured from the
            // product's own limb, and the temperature is not used.
            if (scene_.tbb && prod.size() != QSize(tbb_.width, tbb_.height)) {
                scene_.tbb = nullptr;
                scene_.disk = DiskGeometry{};
                tbb_ = Grey16{};
                observed_ = Grey16{};
            }
            if (!scene_.disk.valid()) {
                DiskGeometry d;
                if (!measureLimb(prod.width(), prod.height(), [&](int x, int y) {
                        const QRgb c = prod.pixel(x, y);
                        return qRed(c) + qGreen(c) + qBlue(c) > 6;
                    }, &d))
                    return false;
                scene_.disk = d;
            }
            // The display products carry a colour legend burned into the lower-left corner; it sits
            // over the limb and would otherwise be wrapped onto the globe.
            QPainter blank(&prod);
            blank.fillRect(QRect(0, prod.height() * 2030 / 2200, prod.width() * 960 / 2200, prod.height()), Qt::black);
            blank.end();
        }
        product_ = toRgb8(prod);
        scene_.product = &product_;
    } else if (!scene_.tbb) {
        return false;   // the true-colour layer needs the temperature raster
    }

    // Observation time from the folder name (yyyyMMdd_hhmmss, UTC), for the day/night line.
    const QDateTime t = QDateTime::fromString(QFileInfo(folder).fileName().left(15), QStringLiteral("yyyyMMdd_HHmmss"));
    scene_.sunKnown = false;
    observationTime_ = 0.0;
    if (t.isValid()) {
        const QDateTime utc(t.date(), t.time(), QTimeZone::utc());
        subSolarPoint(static_cast<double>(utc.toSecsSinceEpoch()), &scene_.sunLatDeg, &scene_.sunLonDeg);
        scene_.sunKnown = true;
        observationTime_ = static_cast<double>(utc.toSecsSinceEpoch());
        observationLabel_ = QStringLiteral("GK-2A %1 UTC").arg(utc.toString(QStringLiteral("yyyy-MM-dd HH:mm")));
    } else {
        observationLabel_ = QStringLiteral("GK-2A %1").arg(QFileInfo(folder).fileName());
    }
    observedLabel_ = observationLabel_;
    frameStep_ = 1;
    frameDirty_ = true;
    update();
    return true;
}

void GlobeView::showTbb(const std::vector<std::uint16_t>& raw, double unixTime, const QString& label) {
    if (raw.empty() || raw.size() != tbb_.data.size()) return;
    tbb_.data = raw;
    if (unixTime > 0.0) {
        subSolarPoint(unixTime, &scene_.sunLatDeg, &scene_.sunLonDeg);
        scene_.sunKnown = true;
    }
    observationLabel_ = label;
    frameDirty_ = true;
    update();
}

void GlobeView::showObserved() {
    if (observed_.data.size() != tbb_.data.size()) return;
    tbb_.data = observed_.data;
    if (observationTime_ > 0.0) subSolarPoint(observationTime_, &scene_.sunLatDeg, &scene_.sunLonDeg);
    observationLabel_ = observedLabel_;
    frameDirty_ = true;
    update();
}

void GlobeView::setMarker(bool on, double latDeg, double lonDeg) {
    marker_ = on;
    markerLat_ = latDeg;
    markerLon_ = lonDeg;
    update();
}

void GlobeView::centreOn(double latDeg, double lonDeg) {
    view_.centreLatDeg = std::clamp(latDeg, -85.0, 85.0);
    view_.centreLonDeg = lonDeg;
    frameStep_ = 1;
    frameDirty_ = true;
    update();
}

void GlobeView::setLayer(Layer layer) {
    scene_.layer = layer;
    frameDirty_ = true;
    update();
}

void GlobeView::setNightVisible(bool on) {
    scene_.night = on;
    frameDirty_ = true;
    update();
}

void GlobeView::resetView() {
    view_.centreLatDeg = 20.0;
    view_.centreLonDeg = 128.2;
    view_.zoom = 1.0;
    frameDirty_ = true;
    update();
}

void GlobeView::render(int step) {
    ensureRelief();
    view_.width = std::max(1, width());
    view_.height = std::max(1, height());
    if (frame_.size() != QSize(view_.width, view_.height))
        frame_ = QImage(view_.width, view_.height, QImage::Format_RGBA8888);
    // QImage rows may be padded; render into a packed buffer and copy rows over.
    std::vector<std::uint8_t> buf(static_cast<std::size_t>(view_.width) * view_.height * 4);
    const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 16);
    std::vector<std::thread> pool;
    // Bands are a whole number of blocks tall, so no block is split between two threads.
    int band = (view_.height + threads - 1) / threads;
    band = (band + step - 1) / step * step;
    for (int i = 0; i < threads; ++i) {
        const int y0 = i * band, y1 = std::min(view_.height, (i + 1) * band);
        if (y0 >= y1) continue;
        pool.emplace_back([this, y0, y1, step, &buf] { renderRows(scene_, view_, y0, y1, step, buf.data()); });
    }
    for (auto& t : pool) t.join();
    for (int y = 0; y < view_.height; ++y)
        std::copy_n(buf.data() + static_cast<std::size_t>(y) * view_.width * 4, view_.width * 4, frame_.scanLine(y));
    frameDirty_ = false;
}

void GlobeView::scheduleFull() {
    frameStep_ = 3;
    frameDirty_ = true;
    refine_->start();
    update();
}

void GlobeView::paintEvent(QPaintEvent*) {
    if (frameDirty_ || frame_.size() != size()) render(frameStep_);
    QPainter p(this);
    p.drawImage(0, 0, frame_);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawOverlays(p);
}

void GlobeView::drawOverlays(QPainter& p) {
    if (grid_) {
        p.setPen(QPen(QColor(150, 190, 235, 70), 1.0));
        // Lines every 15 degrees (5 when zoomed in), drawn as short visible runs.
        const double stepDeg = view_.zoom > 4.0 ? 5.0 : 15.0;
        auto run = [&](auto pointAt, double from, double to, double d) {
            QPainterPath path;
            bool open = false;
            for (double s = from; s <= to + 1e-9; s += d) {
                double x, y;
                const auto [lat, lon] = pointAt(s);
                if (latLonToScreen(view_, lat, lon, &x, &y)) {
                    if (open) path.lineTo(x, y); else { path.moveTo(x, y); open = true; }
                } else {
                    open = false;
                }
            }
            p.drawPath(path);
        };
        for (double lat = -75.0; lat <= 75.0 + 1e-9; lat += stepDeg)
            run([lat](double lon) { return std::make_pair(lat, lon); }, -180.0, 180.0, 1.0);
        for (double lon = -180.0; lon < 180.0 - 1e-9; lon += stepDeg)
            run([lon](double lat) { return std::make_pair(lat, lon); }, -85.0, 85.0, 1.0);
        // The equator a little stronger.
        p.setPen(QPen(QColor(150, 190, 235, 120), 1.2));
        run([](double lon) { return std::make_pair(0.0, lon); }, -180.0, 180.0, 1.0);
    }
    if (satellites_) {
        QFont f = font();
        f.setPointSizeF(9.0);
        p.setFont(f);
        int n = 0;
        for (const GeoSat& s : kSats) {
            const bool below = (n++ % 2) == 1;   // alternate labels above and below: FY-2H and FY-2G are close
            double x, y;
            if (!latLonToScreen(view_, 0.0, s.lon, &x, &y)) continue;
            p.setPen(QPen(QColor(255, 215, 120), 1.6));
            p.setBrush(QColor(255, 215, 120, 60));
            QPolygonF diamond;
            diamond << QPointF(x, y - 6) << QPointF(x + 6, y) << QPointF(x, y + 6) << QPointF(x - 6, y);
            p.drawPolygon(diamond);
            p.setPen(QColor(255, 230, 170));
            p.drawText(QPointF(x + 9, below ? y + 18 : y - 7), QStringLiteral("%1  %2°E").arg(QString::fromLatin1(s.name)).arg(s.lon, 0, 'f', 1));
        }
        p.setBrush(Qt::NoBrush);
    }
    if (marker_) {
        double x, y;
        if (latLonToScreen(view_, markerLat_, markerLon_, &x, &y)) {
            p.setBrush(Qt::NoBrush);
            p.setPen(QPen(QColor(255, 90, 90), 2.0));
            p.drawEllipse(QPointF(x, y), 9, 9);
            p.drawLine(QPointF(x - 15, y), QPointF(x - 5, y));
            p.drawLine(QPointF(x + 5, y), QPointF(x + 15, y));
            p.drawLine(QPointF(x, y - 15), QPointF(x, y - 5));
            p.drawLine(QPointF(x, y + 5), QPointF(x, y + 15));
            p.setPen(QColor(255, 200, 200));
            p.drawText(QPointF(x + 14, y + 22), QStringLiteral("%1°%2 %3°%4")
                .arg(std::fabs(markerLat_), 0, 'f', 2).arg(markerLat_ >= 0 ? QStringLiteral("N") : QStringLiteral("S"))
                .arg(std::fabs(markerLon_), 0, 'f', 2).arg(markerLon_ >= 0 ? QStringLiteral("E") : QStringLiteral("W")));
        }
    }
    // Caption: what is shown and how to move it.
    p.setPen(QColor(200, 208, 220));
    QFont f = font();
    f.setPointSizeF(9.5);
    p.setFont(f);
    const QString what = observationLabel_.isEmpty() ? QStringLiteral("No GK-2A observation loaded (relief map only)") : observationLabel_;
    p.drawText(QRectF(12, height() - 46, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter, what);
    p.setPen(QColor(140, 148, 160));
    p.drawText(QRectF(12, height() - 26, width() - 24, 18), Qt::AlignLeft | Qt::AlignVCenter,
               QStringLiteral("Drag to rotate · wheel to zoom (about the cursor) · right-click to query a point · double-click to reset · zoom %1×").arg(view_.zoom, 0, 'f', 1));
}

void GlobeView::resizeEvent(QResizeEvent*) {
    frameDirty_ = true;
}

void GlobeView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::RightButton) {
        double lat, lon;
        if (screenToLatLon(view_, e->position().x(), e->position().y(), &lat, &lon)) emit pointPicked(lat, lon);
        return;
    }
    if (e->button() == Qt::LeftButton) {
        dragging_ = true;
        lastPos_ = e->position().toPoint();
        setCursor(Qt::ClosedHandCursor);
    }
}

void GlobeView::mouseMoveEvent(QMouseEvent* e) {
    const QPoint pos = e->position().toPoint();
    if (dragging_) {
        const QPoint d = pos - lastPos_;
        lastPos_ = pos;
        const double r = globeRadius(view_);
        // Dragging moves the surface under the cursor: one globe radius is one radian.
        view_.centreLonDeg -= d.x() / r * 180.0 / kPi / std::max(0.2, std::cos(view_.centreLatDeg * kPi / 180.0));
        view_.centreLatDeg = std::clamp(view_.centreLatDeg + d.y() / r * 180.0 / kPi, -85.0, 85.0);
        view_.centreLonDeg = std::fmod(view_.centreLonDeg + 540.0, 360.0) - 180.0;
        scheduleFull();
    }
    updateHover(e->position());
}

void GlobeView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
    }
}

void GlobeView::mouseDoubleClickEvent(QMouseEvent*) {
    resetView();
}

void GlobeView::wheelEvent(QWheelEvent* e) {
    const double steps = e->angleDelta().y() / 120.0;
    if (steps == 0.0) return;
    const QPointF at = e->position();
    double lat0, lon0;
    const bool on = screenToLatLon(view_, at.x(), at.y(), &lat0, &lon0);
    view_.zoom = std::clamp(view_.zoom * std::pow(1.2, steps), 0.6, 30.0);
    if (on) {
        // Keep the point under the cursor where it was.
        double lat1, lon1;
        if (screenToLatLon(view_, at.x(), at.y(), &lat1, &lon1)) {
            double dLon = lon0 - lon1;
            dLon = std::fmod(dLon + 540.0, 360.0) - 180.0;
            view_.centreLatDeg = std::clamp(view_.centreLatDeg + (lat0 - lat1), -85.0, 85.0);
            view_.centreLonDeg = std::fmod(view_.centreLonDeg + dLon + 540.0, 360.0) - 180.0;
        }
    }
    scheduleFull();
    updateHover(at);
}

void GlobeView::keyPressEvent(QKeyEvent* e) {
    if (e->key() == Qt::Key_Plus || e->key() == Qt::Key_Equal) view_.zoom = std::min(30.0, view_.zoom * 1.25);
    else if (e->key() == Qt::Key_Minus) view_.zoom = std::max(0.6, view_.zoom / 1.25);
    else if (e->key() == Qt::Key_Left) view_.centreLonDeg -= 5.0 / view_.zoom;
    else if (e->key() == Qt::Key_Right) view_.centreLonDeg += 5.0 / view_.zoom;
    else if (e->key() == Qt::Key_Up) view_.centreLatDeg = std::min(85.0, view_.centreLatDeg + 5.0 / view_.zoom);
    else if (e->key() == Qt::Key_Down) view_.centreLatDeg = std::max(-85.0, view_.centreLatDeg - 5.0 / view_.zoom);
    else { QWidget::keyPressEvent(e); return; }
    scheduleFull();
}

void GlobeView::updateHover(const QPointF& pos) {
    double lat, lon;
    if (!screenToLatLon(view_, pos.x(), pos.y(), &lat, &lon)) {
        emit hoverText(QString());
        return;
    }
    QString text = QStringLiteral("%1°%2  %3°%4")
        .arg(std::fabs(lat), 0, 'f', 2).arg(lat >= 0 ? QStringLiteral("N") : QStringLiteral("S"))
        .arg(std::fabs(lon), 0, 'f', 2).arg(lon >= 0 ? QStringLiteral("E") : QStringLiteral("W"));
    const double k = tbbAt(scene_, lat, lon);
    if (std::isfinite(k)) text += QStringLiteral("  ·  brightness temperature %1 K (%2 °C)").arg(k, 0, 'f', 1).arg(k - 273.15, 0, 'f', 1);
    emit hoverText(text);
}

QImage GlobeView::grabView() {
    frameStep_ = 1;
    render(1);
    QImage out = frame_.convertToFormat(QImage::Format_RGB32);
    QPainter p(&out);
    p.setRenderHint(QPainter::Antialiasing, true);
    drawOverlays(p);
    return out;
}

// ---------------------------------------------------------------------------------------------

GlobePage::GlobePage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(6);
    // Data source, as on the ADD page: a folder, scanned with its subfolders (the JSON sidecars and
    // the GK-2A images), and a list of what was found to choose from.
    auto* dirRow = new QHBoxLayout;
    dirRow->setSpacing(8);
    dirEdit_ = new QLineEdit;
    dirEdit_->setPlaceholderText(QStringLiteral("Folder to scan for GK-2A images"));
    browse_ = new QPushButton(QStringLiteral("Browse…"));
    dirRow->addWidget(new QLabel(QStringLiteral("Folder")));
    dirRow->addWidget(dirEdit_, 1);
    dirRow->addWidget(browse_);
    auto* bar = new QHBoxLayout;
    bar->setSpacing(8);
    lead_ = new QComboBox;
    lead_->addItem(QStringLiteral("Observed"), 0);
    lead_->addItem(QStringLiteral("AI nowcast +10 min"), 1);
    lead_->addItem(QStringLiteral("AI nowcast +20 min"), 2);
    lead_->addItem(QStringLiteral("AI nowcast +30 min"), 3);
    lead_->addItem(QStringLiteral("AI nowcast +60 min"), 6);
    lead_->setToolTip(QStringLiteral("Extrapolates cloud-top brightness temperature from this observation and the one 10 minutes earlier: block-matching winds + advection + a small neural-network correction trained on this station's data (runs offline)"));
    grid_ = new QCheckBox(QStringLiteral("Graticule"));
    grid_->setChecked(true);
    satellites_ = new QCheckBox(QStringLiteral("Geostationary satellites"));
    satellites_->setChecked(true);
    night_ = new QCheckBox(QStringLiteral("Day / night"));
    night_->setChecked(true);
    refresh_ = new QPushButton(QStringLiteral("Rescan"));
    dirRow->addWidget(refresh_);
    root->addLayout(dirRow);
    exportButton_ = new QPushButton(QStringLiteral("Export image…"));
    bar->addWidget(lead_);
    bar->addSpacing(8);
    bar->addWidget(grid_);
    bar->addWidget(satellites_);
    bar->addWidget(night_);
    bar->addStretch();
    bar->addWidget(exportButton_);
    root->addLayout(bar);

    // Left: the images the scan found, grouped by observation time (newest first); right: the globe.
    auto* body = new QHBoxLayout;
    body->setSpacing(8);
    auto* side = new QVBoxLayout;
    side->setSpacing(4);
    list_ = new QTreeWidget;
    list_->setHeaderHidden(true);
    list_->setColumnCount(1);
    list_->setMinimumWidth(250);
    list_->setMaximumWidth(330);
    list_->setToolTip(QStringLiteral("GK-2A images found, grouped by observation time (UTC); click one to show it on the globe"));
    listInfo_ = new QLabel;
    listInfo_->setWordWrap(true);
    listInfo_->setStyleSheet(QStringLiteral("color:#6e737a;"));
    side->addWidget(list_, 1);
    side->addWidget(listInfo_);
    body->addLayout(side);
    view_ = new GlobeView;
    body->addWidget(view_, 1);
    root->addLayout(body, 1);

    auto* foot = new QHBoxLayout;
    hover_ = new QLabel;
    hover_->setStyleSheet(QStringLiteral("color:#cfd6e0;"));
    note_ = new QLabel(QStringLiteral("Outside GK-2A's view only the base map is shown; the disk edge fades out between 68° and 83° satellite zenith angle."));
    note_->setStyleSheet(QStringLiteral("color:#6e737a;"));
    foot->addWidget(hover_, 1);
    foot->addWidget(note_);
    root->addLayout(foot);
    aiNote_ = new QLabel;
    aiNote_->setWordWrap(true);
    aiNote_->setStyleSheet(QStringLiteral("color:#6f8294;"));
    aiNote_->hide();
    root->addWidget(aiNote_);

    // Point query: brightness temperature at a latitude/longitude now and at the forecast steps.
    auto* qBox = new QHBoxLayout;
    qBox->setSpacing(8);
    qLat_ = new QDoubleSpinBox;
    qLat_->setRange(-90.0, 90.0);
    qLat_->setDecimals(3);
    qLat_->setSuffix(QStringLiteral(" °N"));
    qLon_ = new QDoubleSpinBox;
    qLon_->setRange(-180.0, 180.0);
    qLon_->setDecimals(3);
    qLon_->setSuffix(QStringLiteral(" °E"));
    {
        QSettings settings(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        qLat_->setValue(settings.value(QStringLiteral("globe/queryLat"), 39.9).toDouble());
        qLon_->setValue(settings.value(QStringLiteral("globe/queryLon"), 116.4).toDouble());
    }
    qRun_ = new QPushButton(QStringLiteral("Query"));
    qClear_ = new QPushButton(QStringLiteral("Clear"));
    qBox->addWidget(new QLabel(QStringLiteral("Query point  lat")));
    qBox->addWidget(qLat_);
    qBox->addWidget(new QLabel(QStringLiteral("lon")));
    qBox->addWidget(qLon_);
    qBox->addWidget(qRun_);
    qBox->addWidget(qClear_);
    qBox->addWidget(new QLabel(QStringLiteral("(or right-click on the globe)")));
    qBox->addStretch();
    root->addLayout(qBox);
    qResult_ = new QLabel;
    qResult_->setTextFormat(Qt::RichText);
    qResult_->setWordWrap(true);
    qResult_->hide();
    root->addWidget(qResult_);

    connect(view_, &GlobeView::hoverText, hover_, &QLabel::setText);
    connect(qRun_, &QPushButton::clicked, this, [this] {
        queryActive_ = true;
        view_->centreOn(qLat_->value(), qLon_->value());
        runQuery();
    });
    connect(qClear_, &QPushButton::clicked, this, [this] {
        queryActive_ = false;
        view_->setMarker(false);
        qResult_->hide();
    });
    connect(view_, &GlobeView::pointPicked, this, [this](double lat, double lon) {
        qLat_->setValue(lat);
        qLon_->setValue(lon);
        queryActive_ = true;
        runQuery();
    });
    connect(list_, &QTreeWidget::currentItemChanged, this, [this](QTreeWidgetItem* item, QTreeWidgetItem*) {
        if (!item) return;
        // A time row stands for its first image (the true-colour view when there is one).
        if (item->childCount() > 0) item = item->child(0);
        const int index = item->data(0, Qt::UserRole).toInt();
        if (index < 0 || index >= static_cast<int>(images_.size()) || index == current_) return;
        current_ = index;
        loadSelected();
    });
    connect(lead_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) { applyLead(); });
    connect(grid_, &QCheckBox::toggled, view_, &GlobeView::setGridVisible);
    connect(satellites_, &QCheckBox::toggled, view_, &GlobeView::setSatellitesVisible);
    connect(night_, &QCheckBox::toggled, view_, &GlobeView::setNightVisible);
    connect(refresh_, &QPushButton::clicked, this, &GlobePage::refreshObservationsAsync);
    connect(dirEdit_, &QLineEdit::returnPressed, this, &GlobePage::refreshObservationsAsync);
    connect(browse_, &QPushButton::clicked, this, [this] {
        const QString chosen = QFileDialog::getExistingDirectory(this, QStringLiteral("Folder with GK-2A observations"), folder());
        if (chosen.isEmpty()) return;
        dirEdit_->setText(QDir::toNativeSeparators(chosen));
        refreshObservationsAsync();
    });
    connect(exportButton_, &QPushButton::clicked, this, [this] {
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export globe view"),
            QDir(dataRoot_).filePath(QStringLiteral("globe_view.png")), QStringLiteral("PNG (*.png)"));
        if (!path.isEmpty()) view_->grabView().save(path, "PNG");
    });
}

void GlobePage::setLead(int steps) {
    if (!loadedOnce_) {
        loadedOnce_ = true;
        refreshObservations();
    }
    const int at = lead_->findData(steps);
    if (at >= 0) lead_->setCurrentIndex(at);
}

void GlobePage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!loadedOnce_) {
        loadedOnce_ = true;
        // Let the page paint first, then read on a worker behind the loading indicator.
        QTimer::singleShot(0, this, &GlobePage::refreshObservationsAsync);
    }
}

void GlobePage::setDataRoot(const QString& root) {
    dataRoot_ = root;
    // Like the ADD page: the folder starts at the output root, and a folder the operator chose stays.
    if (dirEdit_ && dirEdit_->text().trimmed().isEmpty()) {
        QSettings settings(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        const QString saved = settings.value(QStringLiteral("paths/globeFolder")).toString();
        dirEdit_->setText(QDir::toNativeSeparators(saved.isEmpty() ? root : saved));
    }
}

QString GlobePage::folder() const {
    const QString f = dirEdit_ ? QDir::fromNativeSeparators(dirEdit_->text().trimmed()) : QString();
    return f.isEmpty() ? dataRoot_ : f;
}

namespace {
// Observation time from the folder name (yyyyMMdd_HHmmss, UTC); invalid when it is not one.
QDateTime folderTime(const QString& folder) {
    const QDateTime t = QDateTime::fromString(QFileInfo(folder).fileName().left(15), QStringLiteral("yyyyMMdd_HHmmss"));
    return t.isValid() ? QDateTime(t.date(), t.time(), QTimeZone::utc()) : QDateTime();
}
}  // namespace

GlobePage::ScanResult GlobePage::scanObservations(const QString& root) {
    ScanResult r;
    r.root = root;
    // Observations are the folders holding the GK-2A brightness temperature (GK2A_IR_0p01K.png),
    // found from the GK-2A JSON sidecars (L1B dictionary, GK2A_Region.json) and, for the many older
    // folders that have none, from the raster itself. Each gives two views on the globe.
    QDirIterator js(root, {QStringLiteral("GK2A*.json")}, QDir::Files, QDirIterator::Subdirectories);
    while (js.hasNext()) {
        const QString path = js.next();
        if (path.contains(QStringLiteral("_withdrawn"), Qt::CaseInsensitive)) continue;
        QFile f(path);
        if (f.size() > 4 * 1024 * 1024 || !f.open(QIODevice::ReadOnly)) continue;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.value(QStringLiteral("satellite")).toString() != QStringLiteral("GK-2A") &&
            !o.value(QStringLiteral("format")).toString().startsWith(QStringLiteral("GK2A-")))
            continue;
        ++r.jsonFiles;
        const QString dir = QDir::cleanPath(QFileInfo(path).absolutePath());
        if (QFileInfo::exists(QDir(dir).filePath(QStringLiteral("GK2A_IR_0p01K.png")))) r.jsonFolders.insert(dir);
    }
    QStringList folders(r.jsonFolders.begin(), r.jsonFolders.end());
    QDirIterator it(root, {QStringLiteral("GK2A_IR_0p01K.png")}, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        if (path.contains(QStringLiteral("_withdrawn"), Qt::CaseInsensitive)) continue;
        const QString dir = QDir::cleanPath(QFileInfo(path).absolutePath());
        if (!folders.contains(dir)) folders << dir;
    }
    // Newest first, by the folder name (the observation time).
    std::sort(folders.begin(), folders.end(), [](const QString& a, const QString& b) {
        const QString fa = QFileInfo(a).fileName(), fb = QFileInfo(b).fileName();
        return fa != fb ? fa > fb : a < b;
    });
    r.folders = folders;
    return r;
}

void GlobePage::applyScan(const ScanResult& scan) {
    images_.clear();
    tbbFolders_.clear();
    for (const QString& dir : scan.folders) {
        tbbFolders_ << dir;
        for (bool enhanced : {false, true}) {
            Image im;
            im.folder = dir;
            im.enhanced = enhanced;
            im.label = enhanced ? QStringLiteral("True-colour base + enhanced cloud tops") : QStringLiteral("True-colour base + cloud");
            im.fromJson = scan.jsonFolders.contains(dir);
            images_.push_back(im);
        }
    }
    listInfo_->setText(QStringLiteral("%1 GK-2A observations; %2 JSON manifests read (%3 observations have one).")
                           .arg(scan.folders.size()).arg(scan.jsonFiles).arg(scan.jsonFolders.size()));
}

void GlobePage::scanFolder(const QString& root) {
    applyScan(scanObservations(root));
}

void GlobePage::refreshObservationsAsync() {
    if (scanning_) return;
    scanning_ = true;
    if (!busy_) busy_ = new BusyOverlay(this);
    busy_->start(QStringLiteral("Reading observations and the relief map…"));
    const QString root = folder();
    {
        QSettings settings(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        settings.setValue(QStringLiteral("paths/globeFolder"), root);
    }
    const bool haveCurrent = current_ >= 0 && current_ < static_cast<int>(images_.size());
    const QString keep = haveCurrent ? images_[static_cast<std::size_t>(current_)].folder : QString();
    const bool keepEnhanced = haveCurrent && images_[static_cast<std::size_t>(current_)].enhanced;
    const bool needRelief = !view_->hasRelief();
    QPointer<GlobePage> self(this);
    std::thread([self, root, keep, keepEnhanced, needRelief]() {
        ScanResult scan = scanObservations(root);
        auto relief = std::make_shared<Rgb8>();
        if (needRelief) *relief = GlobeView::readRelief();
        // The raster of the observation that will be shown (the one kept, else the newest).
        QString first = keep.isEmpty() || !scan.folders.contains(keep) ? (scan.folders.isEmpty() ? QString() : scan.folders.first()) : keep;
        QString tbbPath;
        QImage tbb;
        if (!first.isEmpty()) {
            tbbPath = QDir(first).filePath(QStringLiteral("GK2A_IR_0p01K.png"));
            tbb = QImage(tbbPath);
        }
        QMetaObject::invokeMethod(qApp, [self, scan = std::move(scan), relief, needRelief, tbbPath, tbb, keep, keepEnhanced]() mutable {
            if (!self) return;
            if (needRelief) self->view_->adoptRelief(std::move(*relief));
            if (!tbbPath.isEmpty()) self->view_->preloadTbb(tbbPath, tbb);
            self->applyScan(scan);
            self->fillList(keep, keepEnhanced);
            self->busy_->stop();
            self->scanning_ = false;
        }, Qt::QueuedConnection);
    }).detach();
}

void GlobePage::refreshObservations() {
    // Keep what is shown across a rescan when it is still there.
    QString keepFolder;
    bool keepEnhanced = false;
    if (current_ >= 0 && current_ < static_cast<int>(images_.size())) {
        keepFolder = images_[static_cast<std::size_t>(current_)].folder;
        keepEnhanced = images_[static_cast<std::size_t>(current_)].enhanced;
    }
    const QString root = folder();
    {
        QSettings settings(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        settings.setValue(QStringLiteral("paths/globeFolder"), root);
    }
    scanFolder(root);
    fillList(keepFolder, keepEnhanced);
}

void GlobePage::fillList(const QString& keepFolder, bool keepEnhanced) {
    QSignalBlocker block(list_);
    list_->clear();
    current_ = -1;
    QTreeWidgetItem* group = nullptr;
    QString groupFolder;
    QTreeWidgetItem* pick = nullptr;
    for (int i = 0; i < static_cast<int>(images_.size()); ++i) {
        const Image& im = images_[static_cast<std::size_t>(i)];
        if (!group || im.folder != groupFolder) {
            groupFolder = im.folder;
            const QDateTime t = folderTime(im.folder);
            group = new QTreeWidgetItem(list_, {t.isValid() ? t.toString(QStringLiteral("yyyy-MM-dd HH:mm 'UTC'"))
                                                            : QFileInfo(im.folder).fileName()});
            group->setData(0, Qt::UserRole, -1);
            group->setToolTip(0, QDir::toNativeSeparators(im.folder));
        }
        auto* item = new QTreeWidgetItem(group, {im.label});
        item->setData(0, Qt::UserRole, i);
        item->setToolTip(0, QDir::toNativeSeparators(QDir(im.folder).filePath(QStringLiteral("GK2A_IR_0p01K.png"))) +
                                (im.fromJson ? QStringLiteral("\n(has a JSON manifest)") : QString()));
        if (!pick && im.folder == keepFolder && im.enhanced == keepEnhanced) pick = item;
    }
    if (!pick && list_->topLevelItemCount() > 0) pick = list_->topLevelItem(0)->child(0);
    for (int i = 0; i < list_->topLevelItemCount(); ++i) list_->topLevelItem(i)->setExpanded(i < 3);
    if (pick) {
        pick->parent()->setExpanded(true);
        list_->setCurrentItem(pick);
        current_ = pick->data(0, Qt::UserRole).toInt();
    }
    block.unblock();
    loadSelected();
}

bool GlobePage::selectImage(const QString& name) {
    // NAME is an observation folder (or its end), optionally followed by ":enhanced".
    if (!loadedOnce_) {
        loadedOnce_ = true;
        refreshObservations();
    }
    QString want = QDir::fromNativeSeparators(name);
    bool enhanced = false;
    if (want.endsWith(QStringLiteral(":enhanced"), Qt::CaseInsensitive)) {
        enhanced = true;
        want.chop(9);
    }
    while (want.endsWith(QLatin1Char('/'))) want.chop(1);
    for (int g = 0; g < list_->topLevelItemCount(); ++g) {
        QTreeWidgetItem* group = list_->topLevelItem(g);
        for (int c = 0; c < group->childCount(); ++c) {
            const int i = group->child(c)->data(0, Qt::UserRole).toInt();
            const Image& im = images_[static_cast<std::size_t>(i)];
            if (im.enhanced == enhanced && (want.isEmpty() || im.folder.endsWith(want, Qt::CaseInsensitive))) {
                group->setExpanded(true);
                list_->setCurrentItem(group->child(c));
                return true;
            }
        }
    }
    return false;
}

void GlobePage::loadSelected() {
    if (current_ < 0 || current_ >= static_cast<int>(images_.size())) {
        currentFolder_.clear();
        note_->setText(QStringLiteral("No GK-2A observation (GK2A_IR_0p01K.png) found in this folder. Choose another folder, or receive GK-2A first."));
        view_->update();
        applyLead();
        return;
    }
    const Image& im = images_[static_cast<std::size_t>(current_)];
    currentFolder_ = im.folder;
    view_->setLayer(im.enhanced ? Layer::EnhancedTops : Layer::TrueColour);
    if (!view_->loadObservation(im.folder, QString()))
        note_->setText(QStringLiteral("Cannot read %1").arg(QDir::toNativeSeparators(QDir(im.folder).filePath(QStringLiteral("GK2A_IR_0p01K.png")))));
    else
        note_->setText(im.enhanced
            ? QStringLiteral("Enhanced cloud tops: tops colder than 240 K are coloured by temperature (light blue 240 · blue 230 · green 220 · yellow 210 · red 200 · magenta 190 · white ≤180 K); colder means stronger convection.")
            : QStringLiteral("Outside GK-2A's view only the base map is shown; the disk edge fades out between 68° and 83° satellite zenith angle."));
    applyLead();
    if (queryActive_) runQuery();
}

QString GlobePage::previousFolder(const QString& folder) const {
    // The observation ten minutes earlier (8 to 12 minutes, the nearest to 10), with a temperature
    // raster. Folder names are the observation time, yyyyMMdd_HHmmss[...] UTC.
    const QDateTime t0 = folderTime(folder);
    if (!t0.isValid()) return {};
    QString best;
    qint64 bestErr = 1000000;
    for (const QString& path : tbbFolders_) {
        const QDateTime t = folderTime(path);
        if (!t.isValid()) continue;
        const qint64 dt = t.secsTo(t0);
        if (dt < 480 || dt > 720) continue;
        const qint64 err = std::llabs(dt - 600);
        if (err < bestErr) { bestErr = err; best = path; }
    }
    return best;
}

void GlobePage::applyLead() {
    const int steps = lead_->currentData().toInt();
    const QString folder = view_->hasTbb() ? currentFolder_ : QString();
    const Layer layer = (current_ >= 0 && current_ < static_cast<int>(images_.size()) &&
                         images_[static_cast<std::size_t>(current_)].enhanced) ? Layer::EnhancedTops : Layer::TrueColour;
    if (steps <= 0 || folder.isEmpty()) {
        if (steps > 0) {
            aiNote_->show();
            aiNote_->setText(QStringLiteral("The AI nowcast needs this observation's brightness temperatures (GK2A_IR_0p01K.png), which this folder does not have."));
        } else {
            aiNote_->hide();
        }
        view_->setLayer(layer);
        view_->showObserved();
        return;
    }
    // The forecast is a temperature field, shown on either view.
    view_->setLayer(layer);
    aiNote_->show();
    if (forecastFolder_ == folder && static_cast<int>(forecastSteps_.size()) >= steps) {
        const double t = view_->observationTime() + steps * 600.0;
        const QString valid = view_->observationTime() > 0.0
            ? QDateTime::fromSecsSinceEpoch(static_cast<qint64>(t), QTimeZone::utc()).toString(QStringLiteral("yyyy-MM-dd HH:mm"))
            : QStringLiteral("?");
        view_->showTbb(forecastSteps_[static_cast<std::size_t>(steps - 1)], t,
                       QStringLiteral("AI nowcast +%1 min · valid %2 UTC (from %3 and %4)")
                           .arg(steps * 10).arg(valid, QFileInfo(forecastPrevious_).fileName().left(13),
                                                QFileInfo(folder).fileName().left(13)));
        aiNote_->setText(QStringLiteral(
            "Model: %1. Method: block-matching winds + advection + a small neural-network correction trained on this station's data, run offline."
            "Held-out test (10-07 05:20, 05:30 → 05:40, not used for training): +10 min RMSE 3.4 K, persistence 5.0 K, advection only 3.7 K; "
            "cold cloud (<240 K) CSI 0.83. The error grows with lead time and new convection cannot be forecast; trained on only 4 cases, it improves as this station collects more data.")
                             .arg(forecastModel_));
        return;
    }
    view_->showObserved();
    if (!forecastRunning_ || forecastFolder_ != folder) startForecast();
}

void GlobePage::startForecast() {
    const QString folder = currentFolder_;
    const QString prev = previousFolder(folder);
    forecastFolder_ = folder;
    forecastPrevious_ = prev;
    forecastSteps_.clear();
    if (prev.isEmpty()) {
        forecastRunning_ = false;
        aiNote_->setText(QStringLiteral("No GK-2A observation from 10 minutes earlier (8–12 min) was found, so cloud motion cannot be estimated and there is no nowcast. Receive two or more images in a row."));
        return;
    }
    QImage prevImage(QDir(prev).filePath(QStringLiteral("GK2A_IR_0p01K.png")));
    prevImage = prevImage.convertToFormat(QImage::Format_Grayscale16);
    const nadirstation::globe::Grey16& now = view_->observedTbb();
    if (prevImage.isNull() || prevImage.width() != now.width || prevImage.height() != now.height || now.width != now.height) {
        aiNote_->setText(QStringLiteral("The observation from 10 minutes earlier has a different size; no nowcast."));
        return;
    }
    std::vector<std::uint16_t> prevRaw(static_cast<std::size_t>(now.width) * now.height);
    for (int y = 0; y < now.height; ++y)
        std::copy_n(reinterpret_cast<const std::uint16_t*>(prevImage.constScanLine(y)), now.width,
                    prevRaw.data() + static_cast<std::size_t>(y) * now.width);
    std::vector<std::uint16_t> nowRaw = now.data;
    forecastPrevRaw_ = prevRaw;
    const int size = now.width;
    const QString modelPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("resources/models/nowcast_v1.txt"));
    const int generation = ++forecastGeneration_;
    forecastRunning_ = true;
    aiNote_->setText(QStringLiteral("Computing the AI nowcast (winds + 6 steps, about 10–20 s)…"));
    QPointer<GlobePage> self(this);
    std::thread([self, generation, prevRaw = std::move(prevRaw), nowRaw = std::move(nowRaw), size, modelPath]() {
        namespace nc = nadirstation::nowcast;
        std::string error;
        const nc::Model model = nc::loadModel(modelPath.toLocal8Bit().toStdString(), &error);
        const nc::Field p = nc::fromRaw16(prevRaw.data(), size), c = nc::fromRaw16(nowRaw.data(), size);
        const nc::Motion m = nc::estimateMotion(p, c);
        const int threads = std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
        const std::vector<nc::Field> out = nc::forecast(p, c, m, model, 6, threads);
        auto steps = std::make_shared<std::vector<std::vector<std::uint16_t>>>();
        for (const nc::Field& f : out) steps->push_back(nc::toRaw16(f));
        const QString name = model.valid() ? QString::fromStdString(model.name)
                                           : QStringLiteral("Model not found (%1); advection only").arg(QString::fromStdString(error));
        QMetaObject::invokeMethod(qApp, [self, generation, steps, name] {
            if (!self || generation != self->forecastGeneration_) return;
            self->forecastSteps_ = std::move(*steps);
            self->forecastModel_ = name;
            self->forecastRunning_ = false;
            self->applyLead();
            if (self->queryActive_) self->runQuery();
        }, Qt::QueuedConnection);
    }).detach();
}

namespace {
// What an IR10.5 brightness temperature usually means. It is the temperature of whatever radiates
// at the top -- the ground or sea in clear sky, the cloud top otherwise -- not the air temperature.
QString reading(double k) {
    if (!std::isfinite(k)) return QStringLiteral("No data");
    if (k >= 285.0) return QStringLiteral("Mostly clear: about the land / sea surface temperature");
    if (k >= 260.0) return QStringLiteral("Low cloud, or a cold surface");
    if (k >= 235.0) return QStringLiteral("Mid-level cloud");
    if (k >= 215.0) return QStringLiteral("High cloud / cirrus");
    return QStringLiteral("Deep convective top (severe convection possible)");
}
QString kc(double k) {
    return std::isfinite(k) ? QStringLiteral("%1 K（%2 °C）").arg(k, 0, 'f', 1).arg(k - 273.15, 0, 'f', 1) : QStringLiteral("—");
}
}  // namespace

void GlobePage::queryPoint(double latDeg, double lonDeg) {
    if (!loadedOnce_) {
        loadedOnce_ = true;
        refreshObservations();
    }
    qLat_->setValue(latDeg);
    qLon_->setValue(lonDeg);
    queryActive_ = true;
    view_->centreOn(latDeg, lonDeg);
    runQuery();
}

void GlobePage::runQuery() {
    const double lat = qLat_->value(), lon = qLon_->value();
    {
        QSettings settings(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        settings.setValue(QStringLiteral("globe/queryLat"), lat);
        settings.setValue(QStringLiteral("globe/queryLon"), lon);
    }
    view_->setMarker(true, lat, lon);
    qResult_->show();
    const nadirstation::globe::Grey16& obs = view_->observedTbb();
    const auto& disk = view_->disk();
    const double t0 = view_->observationTime();
    auto when = [&](double t) {
        if (t <= 0.0) return QStringLiteral("?");
        const QDateTime utc = QDateTime::fromSecsSinceEpoch(static_cast<qint64>(t), QTimeZone::utc());
        return QStringLiteral("%1 UTC (Beijing %2)").arg(utc.toString(QStringLiteral("MM-dd HH:mm")),
                                                    utc.addSecs(8 * 3600).toString(QStringLiteral("HH:mm")));
    };
    double c, r, edge;
    if (obs.empty()) {
        qResult_->setText(QStringLiteral("<b>%1°, %2°</b>: this observation has no brightness temperatures (GK2A_IR_0p01K.png), so it cannot be queried. Choose an observation with \"True-colour base + cloud\".")
                              .arg(lat, 0, 'f', 3).arg(lon, 0, 'f', 3));
        return;
    }
    if (!nadirstation::globe::diskPixel(disk, lat, lon, &c, &r, &edge)) {
        qResult_->setText(QStringLiteral("<b>%1°, %2°</b>: outside GK-2A's disk; no data.").arg(lat, 0, 'f', 3).arg(lon, 0, 'f', 3));
        return;
    }
    const double zen = std::acos(std::clamp(edge, -1.0, 1.0)) * 180.0 / 3.14159265358979;
    QStringList rows;
    auto row = [&](const QString& label, double t, double k, const QString& note) {
        rows << QStringLiteral("<tr><td style='padding-right:14px'>%1</td><td style='padding-right:14px'>%2</td>"
                               "<td style='padding-right:14px'><b>%3</b></td><td style='color:#9a9ea5'>%4</td></tr>")
                    .arg(label, when(t), kc(k), note.isEmpty() ? reading(k) : note);
    };
    const bool havePrev = forecastFolder_ == currentFolder_ && forecastPrevRaw_.size() == obs.data.size();
    double kPrev = std::nan("");
    if (havePrev) {
        nadirstation::globe::Grey16 prev;
        prev.width = obs.width; prev.height = obs.height; prev.data = forecastPrevRaw_;
        kPrev = nadirstation::globe::sampleTbb(prev, disk, lat, lon, 1);
        row(QStringLiteral("10 min earlier (observed)"), t0 - 600.0, kPrev, QString());
    }
    const double kNow = nadirstation::globe::sampleTbb(obs, disk, lat, lon, 1);
    QString nowNote = reading(kNow);
    if (std::isfinite(kPrev) && std::isfinite(kNow)) {
        const double d = kNow - kPrev;
        if (d <= -4.0) nowNote += QStringLiteral("; cooled %1 K in 10 min: cloud developing / rising").arg(-d, 0, 'f', 1);
        else if (d >= 4.0) nowNote += QStringLiteral("; warmed %1 K in 10 min: cloud weakening / moving off").arg(d, 0, 'f', 1);
    }
    row(QStringLiteral("<b>Current observation</b>"), t0, kNow, nowNote);
    const bool haveForecast = forecastFolder_ == currentFolder_ && !forecastSteps_.empty();
    if (haveForecast) {
        for (int s : {1, 2, 3, 6}) {
            if (static_cast<int>(forecastSteps_.size()) < s) break;
            nadirstation::globe::Grey16 f;
            f.width = obs.width; f.height = obs.height; f.data = forecastSteps_[static_cast<std::size_t>(s - 1)];
            row(QStringLiteral("AI nowcast +%1 min").arg(s * 10), t0 + s * 600.0, nadirstation::globe::sampleTbb(f, disk, lat, lon, 1), QString());
        }
    }
    QString foot = QStringLiteral("10.5 µm infrared brightness temperature (3×3 pixel mean, about 12 km): the land or sea surface in clear sky, the cloud top under cloud — <b>not the near-surface air temperature</b>."
                                  "Satellite zenith angle %1°%2.").arg(zen, 0, 'f', 1)
                       .arg(zen > 70.0 ? QStringLiteral(", large: values read colder and blurrier") : QString());
    if (!haveForecast) {
        foot += forecastRunning_ ? QStringLiteral(" The AI nowcast is running; +10/+20/+30/+60 min are added when it finishes.")
                                 : QStringLiteral(" Choose an AI nowcast lead time to get values 10–60 minutes ahead (needs the observation from 10 minutes earlier).");
        if (!forecastRunning_ && !previousFolder(currentFolder_).isEmpty()) startForecast();
    }
    qResult_->setText(QStringLiteral("<b>%1°, %2°</b><table>%3</table><span style='color:#6e737a'>%4</span>")
                          .arg(lat, 0, 'f', 3).arg(lon, 0, 'f', 3).arg(rows.join(QString()), foot));
}
