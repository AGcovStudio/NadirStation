#include "passpage.h"

#include "globecore.h"
#include "orbit.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QLabel>
#include <QLineEdit>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QNetworkAccessManager>
#include <QNetworkProxyFactory>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUrl>
#include <QPainter>
#include <QPainterPath>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QSplitter>
#include <QTableWidget>
#include <QThread>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using nadirstation::pass::Look;
using nadirstation::pass::Pass;
using nadirstation::pass::TleEntry;
using nadirstation::pass::Tracker;

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr int kMaxLive = 200;      // satellites drawn live at most
constexpr int kMaxRows = 3000;     // pass table rows at most

QSettings appSettings() { return QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation")); }

double nowUnix() { return double(QDateTime::currentMSecsSinceEpoch()) / 1000.0; }

QString durationText(double seconds) {
    const qint64 s = qint64(std::llround(std::max(0.0, seconds)));
    if (s >= 3600) return QStringLiteral("%1:%2:%3").arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

QString compass(double az) {
    static const char* names[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE",
                                  "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    const int i = int(std::floor(std::fmod(az + 11.25 + 360.0, 360.0) / 22.5)) % 16;
    return QString::fromLatin1(names[i]);
}

QString latLonText(double lat, double lon) {
    return QStringLiteral("%1°%2 %3°%4")
        .arg(std::fabs(lat), 0, 'f', 3).arg(lat >= 0 ? QStringLiteral("N") : QStringLiteral("S"))
        .arg(std::fabs(lon), 0, 'f', 3).arg(lon >= 0 ? QStringLiteral("E") : QStringLiteral("W"));
}

// Point at a central angle `distDeg` from (lat, lon) along bearing `brgDeg`.
QPointF destination(double latDeg, double lonDeg, double brgDeg, double distDeg) {
    const double la = latDeg * kPi / 180.0, lo = lonDeg * kPi / 180.0;
    const double b = brgDeg * kPi / 180.0, d = distDeg * kPi / 180.0;
    const double la2 = std::asin(std::sin(la) * std::cos(d) + std::cos(la) * std::sin(d) * std::cos(b));
    double lo2 = lo + std::atan2(std::sin(b) * std::sin(d) * std::cos(la), std::cos(d) - std::sin(la) * std::sin(la2));
    double lonOut = lo2 * 180.0 / kPi;
    while (lonOut > 180.0) lonOut -= 360.0;
    while (lonOut < -180.0) lonOut += 360.0;
    return QPointF(lonOut, la2 * 180.0 / kPi);
}

// Celestrak groups downloaded at start-up (gp.php?GROUP=...). Kept in the settings
// ("passes/onlineGroups") so the list can be changed without a rebuild.
const QStringList kDefaultGroups = {QStringLiteral("weather"), QStringLiteral("noaa"), QStringLiteral("amateur"),
                                    QStringLiteral("stations"), QStringLiteral("geo")};
const int kFy2Norad[] = {40367, 43491};   // FY-2G, FY-2H
// Celestrak asks for the same data not to be fetched more often than every two hours.
constexpr qint64 kMinRefreshSecs = 2 * 3600;

// TLE epoch as a sortable number (year * 1000 + day of year), from line 1 columns 19-32.
double tleEpochKey(const std::string& line1) {
    if (line1.size() < 32) return 0.0;
    const int yy = std::atoi(line1.substr(18, 2).c_str());
    const double doy = std::atof(line1.substr(20, 12).c_str());
    return (yy < 57 ? 2000 + yy : 1900 + yy) * 1000.0 + doy;
}

QString tleCacheDir() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("tle"));
}
QString writableFy2Tle() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation)).filePath(QStringLiteral("fy2.tle"));
}
bool fresh(const QString& path) {
    const QFileInfo fi(path);
    return fi.exists() && fi.lastModified().secsTo(QDateTime::currentDateTime()) < kMinRefreshSecs;
}
QByteArray tleText(const std::vector<TleEntry>& list) {
    QByteArray out;
    for (const TleEntry& e : list) {
        out += QByteArray::fromStdString(e.name) + "\n";
        out += QByteArray::fromStdString(e.line1) + "\n";
        out += QByteArray::fromStdString(e.line2) + "\n";
    }
    return out;
}

const QColor kFollow(0xff, 0x9f, 0x1c);
const QColor kSat(0x6c, 0xc4, 0xff);

}  // namespace

// ================================================================================================
// Sky plot

PassSkyPlot::PassSkyPlot(QWidget* parent) : QWidget(parent) {
    setMinimumSize(220, 220);
}

void PassSkyPlot::setPass(const Pass* pass) {
    hasPass_ = pass != nullptr;
    if (pass) pass_ = *pass;
    update();
}

void PassSkyPlot::setMarkers(const QVector<Marker>& markers) {
    markers_ = markers;
    update();
}

QPointF PassSkyPlot::toScreen(double az, double el, const QPointF& c, double r) const {
    const double rr = r * (90.0 - std::clamp(el, 0.0, 90.0)) / 90.0;
    const double a = az * kPi / 180.0;
    return QPointF(c.x() + rr * std::sin(a), c.y() - rr * std::cos(a));
}

void PassSkyPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x18, 0x19, 0x1b));
    const QPointF c(width() / 2.0, height() / 2.0);
    const double r = std::max(20.0, std::min(width(), height()) / 2.0 - 22.0);

    p.setPen(QPen(QColor(0x35, 0x37, 0x3c), 1));
    p.setBrush(QColor(0x22, 0x24, 0x28));
    p.drawEllipse(c, r, r);
    p.setBrush(Qt::NoBrush);
    for (int el : {30, 60}) {
        const double rr = r * (90 - el) / 90.0;
        p.drawEllipse(c, rr, rr);
    }
    for (int az = 0; az < 360; az += 30) {
        const QPointF e = toScreen(az, 0, c, r);
        p.drawLine(c, e);
    }
    p.setPen(QColor(0x9a, 0x9e, 0xa5));
    QFont f = font();
    f.setPointSizeF(std::max(7.0, f.pointSizeF() - 1));
    p.setFont(f);
    const char* card[] = {"N", "E", "S", "W"};
    for (int i = 0; i < 4; ++i) {
        const QPointF e = toScreen(i * 90.0, 0.0, c, r + 11);
        p.drawText(QRectF(e.x() - 10, e.y() - 8, 20, 16), Qt::AlignCenter, QString::fromLatin1(card[i]));
    }
    p.setPen(QColor(0x58, 0x5c, 0x63));
    for (int el : {30, 60}) {
        const QPointF e = toScreen(0, el, c, r);
        p.drawText(QPointF(e.x() + 3, e.y() - 2), QStringLiteral("%1°").arg(el));
    }

    if (hasPass_ && pass_.track.size() >= 2) {
        QPainterPath path;
        bool first = true;
        for (const auto& pt : pass_.track) {
            if (pt.elDeg < -0.5) continue;
            const QPointF s = toScreen(pt.azDeg, pt.elDeg, c, r);
            if (first) { path.moveTo(s); first = false; }
            else path.lineTo(s);
        }
        p.setPen(QPen(kSat, 2));
        p.drawPath(path);
        if (!pass_.alwaysUp) {
            const QPointF a = toScreen(pass_.track.front().azDeg, std::max(0.0, pass_.track.front().elDeg), c, r);
            const QPointF b = toScreen(pass_.track.back().azDeg, std::max(0.0, pass_.track.back().elDeg), c, r);
            p.setPen(Qt::NoPen);
            p.setBrush(QColor(0x3d, 0xc4, 0x7e));
            p.drawEllipse(a, 4, 4);
            p.setBrush(QColor(0xe0, 0x5d, 0x5d));
            p.drawEllipse(b, 4, 4);
            p.setPen(QColor(0x3d, 0xc4, 0x7e));
            p.drawText(a + QPointF(6, -4), QStringLiteral("AOS"));
            p.setPen(QColor(0xe0, 0x5d, 0x5d));
            p.drawText(b + QPointF(6, -4), QStringLiteral("LOS"));
        }
        const QPointF t = toScreen(pass_.tcaAzDeg, pass_.maxElDeg, c, r);
        p.setPen(QColor(0xd6, 0xd8, 0xdb));
        p.drawText(t + QPointF(6, 12), QStringLiteral("%1°").arg(pass_.maxElDeg, 0, 'f', 1));
        p.drawText(QRectF(6, 4, width() - 12, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QString::fromStdString(pass_.name));
    }

    for (const Marker& m : markers_) {
        if (m.elDeg < 0) continue;
        const QPointF s = toScreen(m.azDeg, m.elDeg, c, r);
        p.setPen(Qt::NoPen);
        p.setBrush(m.followed ? kFollow : kSat);
        p.drawEllipse(s, m.followed ? 5.5 : 3.5, m.followed ? 5.5 : 3.5);
        p.setPen(m.followed ? kFollow : QColor(0xd6, 0xd8, 0xdb));
        p.drawText(s + QPointF(7, 4), m.name);
    }
}

// ================================================================================================
// World map

PassMap::PassMap(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 180);
    QSizePolicy sp(QSizePolicy::Expanding, QSizePolicy::Expanding);
    sp.setHeightForWidth(true);
    setSizePolicy(sp);
}

void PassMap::setBackground(const QImage& image) {
    background_ = image;
    scaled_ = QImage();
    update();
}

void PassMap::setStation(double lat, double lon) {
    stLat_ = lat;
    stLon_ = lon;
    update();
}

void PassMap::setSun(double lat, double lon) {
    sunLat_ = lat;
    sunLon_ = lon;
    sunKnown_ = true;
    update();
}

void PassMap::setSatellites(const QVector<Sat>& sats) {
    sats_ = sats;
    update();
}

void PassMap::setTrack(const QVector<QPointF>& lonLat) {
    track_ = lonLat;
    update();
}

QRectF PassMap::mapRect() const {
    const double w = width() - 2.0, h = height() - 2.0;
    const double mw = std::min(w, 2.0 * h);
    return QRectF((width() - mw) / 2.0, (height() - mw / 2.0) / 2.0, mw, mw / 2.0);
}

QPointF PassMap::toScreen(double lat, double lon, const QRectF& r) const {
    return QPointF(r.left() + (lon + 180.0) / 360.0 * r.width(), r.top() + (90.0 - lat) / 180.0 * r.height());
}

void PassMap::drawPath(QPainter& p, const QVector<QPointF>& ll, const QRectF& r) const {
    QPainterPath path;
    bool started = false;
    for (int i = 0; i < ll.size(); ++i) {
        const QPointF s = toScreen(ll[i].y(), ll[i].x(), r);
        if (!started || std::fabs(ll[i].x() - ll[i - 1].x()) > 180.0) {
            path.moveTo(s);
            started = true;
        } else {
            path.lineTo(s);
        }
    }
    p.drawPath(path);
}

void PassMap::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), QColor(0x14, 0x15, 0x17));
    const QRectF r = mapRect();
    if (!background_.isNull()) {
        if (scaled_.size() != r.size().toSize())
            scaled_ = background_.scaled(r.size().toSize(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        p.drawImage(r.topLeft(), scaled_);
    } else {
        p.fillRect(r, QColor(0x1f, 0x2a, 0x36));
    }
    p.save();
    p.setClipRect(r);

    // Night side
    if (sunKnown_) {
        double dec = sunLat_;
        if (std::fabs(dec) < 0.1) dec = dec < 0 ? -0.1 : 0.1;
        const double td = std::tan(dec * kPi / 180.0);
        QPolygonF poly;
        for (int lon = -180; lon <= 180; lon += 2) {
            const double dl = (lon - sunLon_) * kPi / 180.0;
            const double lat = std::atan(-std::cos(dl) / td) * 180.0 / kPi;
            poly << toScreen(lat, lon, r);
        }
        const double edge = dec > 0 ? -90.0 : 90.0;
        poly << toScreen(edge, 180, r) << toScreen(edge, -180, r);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 105));
        p.drawPolygon(poly);
        const QPointF s = toScreen(sunLat_, sunLon_, r);
        p.setBrush(QColor(0xff, 0xd8, 0x4d));
        p.drawEllipse(s, 4, 4);
    }

    // Grid
    p.setPen(QPen(QColor(255, 255, 255, 35), 1));
    for (int lon = -150; lon <= 150; lon += 30) p.drawLine(toScreen(90, lon, r), toScreen(-90, lon, r));
    for (int lat = -60; lat <= 60; lat += 30) p.drawLine(toScreen(lat, -180, r), toScreen(lat, 180, r));

    // Ground track of the followed satellite
    if (track_.size() >= 2) {
        p.setPen(QPen(QColor(0xff, 0x9f, 0x1c, 200), 1.4, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        drawPath(p, track_, r);
    }

    // Footprints and satellites
    for (const Sat& s : sats_) {
        if (s.footprintDeg > 0.1) {
            QVector<QPointF> circle;
            for (int b = 0; b <= 360; b += 5) circle << destination(s.latDeg, s.lonDeg, b, s.footprintDeg);
            p.setPen(QPen(s.followed ? QColor(0xff, 0x9f, 0x1c, 190) : QColor(0x6c, 0xc4, 0xff, 90), s.followed ? 1.4 : 1.0));
            p.setBrush(Qt::NoBrush);
            drawPath(p, circle, r);
        }
    }
    for (const Sat& s : sats_) {
        const QPointF pt = toScreen(s.latDeg, s.lonDeg, r);
        p.setPen(Qt::NoPen);
        p.setBrush(s.followed ? kFollow : kSat);
        p.drawEllipse(pt, s.followed ? 4.5 : 3.0, s.followed ? 4.5 : 3.0);
        p.setPen(s.followed ? kFollow : QColor(0xd6, 0xd8, 0xdb));
        p.drawText(pt + QPointF(6, -4), s.name);
    }

    // Station
    const QPointF st = toScreen(stLat_, stLon_, r);
    p.setPen(QPen(QColor(0xff, 0x5c, 0x5c), 2));
    p.drawLine(st + QPointF(-6, 0), st + QPointF(6, 0));
    p.drawLine(st + QPointF(0, -6), st + QPointF(0, 6));
    p.restore();
    p.setPen(QColor(0x35, 0x37, 0x3c));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
}

// ================================================================================================
// Page

struct PassPage::Job {
    std::vector<TleEntry> sats;
    nadirstation::pass::Station st;
    nadirstation::pass::PredictOptions opt;
    std::atomic<int> done{0};
    std::atomic<bool> cancel{false};
    std::vector<Pass> result;
    int generation = 0;
    QThread* thread = nullptr;
};

PassPage::PassPage(QWidget* parent) : QWidget(parent) {
    QSettings s = appSettings();
    station_.latDeg = s.value(QStringLiteral("station/latDeg"), 0.0).toDouble();
    station_.lonDeg = s.value(QStringLiteral("station/lonDeg"), 0.0).toDouble();
    station_.altM = s.value(QStringLiteral("station/altM"), 0.0).toDouble();
    horizonDeg_ = s.value(QStringLiteral("station/horizonDeg"), 0.0).toDouble();
    stationSet_ = s.value(QStringLiteral("station/set"), false).toBool();
    for (const QString& n : s.value(QStringLiteral("passes/selected")).toStringList()) {
        bool ok = false;
        const int id = n.toInt(&ok);
        if (ok && !checked_.contains(id)) checked_ << id;
    }
    followedNorad_ = s.value(QStringLiteral("passes/followed"), 0).toInt();
    tleFiles_ = s.value(QStringLiteral("passes/tleFiles")).toStringList();

    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(10, 8, 10, 8);
    root->setSpacing(8);

    // --- Top bar: station and TLE sources
    auto* top = new QHBoxLayout;
    auto* heading = new QLabel(QStringLiteral("Pass prediction"));
    heading->setStyleSheet(QStringLiteral("font-size:15px;font-weight:600;color:#eceef0;"));
    top->addWidget(heading);
    top->addSpacing(16);
    stationLabel_ = new QLabel;
    stationLabel_->setStyleSheet(QStringLiteral("color:#d6d8db;"));
    top->addWidget(stationLabel_);
    auto* settingsBtn = new QPushButton(QStringLiteral("Settings…"));
    settingsBtn->setToolTip(QStringLiteral("Enter the station's latitude, longitude, altitude and horizon on the Settings page"));
    connect(settingsBtn, &QPushButton::clicked, this, &PassPage::settingsRequested);
    top->addWidget(settingsBtn);
    top->addStretch(1);
    tleLabel_ = new QLabel;
    tleLabel_->setStyleSheet(QStringLiteral("color:#9a9ea5;"));
    top->addWidget(tleLabel_);
    auto* addTle = new QPushButton(QStringLiteral("Add TLE…"));
    addTle->setToolTip(QStringLiteral("Add a TLE file in 2-line or 3-line format (Celestrak / Space-Track / Orbitron .txt files all work).\n"
                                      "fy2.tle is always included."));
    connect(addTle, &QPushButton::clicked, this, &PassPage::addTleFiles);
    top->addWidget(addTle);
    auto* clearTle = new QPushButton(QStringLiteral("Remove added"));
    connect(clearTle, &QPushButton::clicked, this, &PassPage::clearTleFiles);
    top->addWidget(clearTle);
    auto* reloadBtn = new QPushButton(QStringLiteral("Reload"));
    reloadBtn->setToolTip(QStringLiteral("Read the TLE files again after they have been updated"));
    connect(reloadBtn, &QPushButton::clicked, this, &PassPage::reloadTle);
    top->addWidget(reloadBtn);
    updateBtn_ = new QPushButton(QStringLiteral("Update online"));
    updateBtn_->setToolTip(QStringLiteral("Download the latest TLEs from Celestrak (%1 and FY-2G/2H). Nothing is downloaded again within two hours.")
                               .arg(kDefaultGroups.join(QStringLiteral("、"))));
    connect(updateBtn_, &QPushButton::clicked, this, [this] { updateTleOnline(false); });
    top->addWidget(updateBtn_);
    autoUpdate_ = new QCheckBox(QStringLiteral("Update at start-up"));
    autoUpdate_->setToolTip(QStringLiteral("Download the latest TLEs in the background at every start; without a network it is skipped and the local files are used"));
    autoUpdate_->setChecked(s.value(QStringLiteral("passes/autoUpdate"), true).toBool());
    connect(autoUpdate_, &QCheckBox::toggled, this, [](bool on) {
        appSettings().setValue(QStringLiteral("passes/autoUpdate"), on);
    });
    top->addWidget(autoUpdate_);
    root->addLayout(top);

    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    root->addWidget(split, 1);

    // --- Left: satellites and options
    auto* left = new QWidget;
    auto* ll = new QVBoxLayout(left);
    ll->setContentsMargins(0, 0, 0, 0);
    filter_ = new QLineEdit;
    filter_->setPlaceholderText(QStringLiteral("Search name or NORAD number"));
    filter_->setClearButtonEnabled(true);
    connect(filter_, &QLineEdit::textChanged, this, [this] { applyFilter(); });
    ll->addWidget(filter_);
    satList_ = new QListWidget;
    satList_->setUniformItemSizes(true);
    connect(satList_, &QListWidget::itemChanged, this, &PassPage::onSatelliteItemChanged);
    connect(satList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* it) {
        followedNorad_ = it->data(Qt::UserRole).toInt();
        appSettings().setValue(QStringLiteral("passes/followed"), followedNorad_);
        selectedPass_ = -1;
        sky_->setPass(nullptr);
        lastTrackAt_ = 0;
        liveTick();
    });
    satList_->setToolTip(QStringLiteral("Tick the satellites to predict; double-click one to track it"));
    ll->addWidget(satList_, 1);
    auto* selRow = new QHBoxLayout;
    auto* allBtn = new QPushButton(QStringLiteral("Tick shown"));
    auto* noneBtn = new QPushButton(QStringLiteral("Untick all"));
    connect(allBtn, &QPushButton::clicked, this, [this] { setAllVisibleChecked(true); });
    connect(noneBtn, &QPushButton::clicked, this, [this] { setAllVisibleChecked(false); });
    selRow->addWidget(allBtn);
    selRow->addWidget(noneBtn);
    ll->addLayout(selRow);

    auto* opts = new QGridLayout;
    opts->addWidget(new QLabel(QStringLiteral("Days ahead")), 0, 0);
    days_ = new QSpinBox;
    days_->setRange(1, 14);
    days_->setValue(s.value(QStringLiteral("passes/days"), 3).toInt());
    opts->addWidget(days_, 0, 1);
    opts->addWidget(new QLabel(QStringLiteral("Minimum peak elevation")), 1, 0);
    minPeak_ = new QDoubleSpinBox;
    minPeak_->setRange(0, 89);
    minPeak_->setDecimals(0);
    minPeak_->setSuffix(QStringLiteral("°"));
    minPeak_->setValue(s.value(QStringLiteral("passes/minPeak"), 5.0).toDouble());
    minPeak_->setToolTip(QStringLiteral("Passes whose highest elevation is below this are not listed"));
    opts->addWidget(minPeak_, 1, 1);
    onlyVisible_ = new QCheckBox(QStringLiteral("Visual passes only"));
    onlyVisible_->setToolTip(QStringLiteral("The satellite is sunlit while the station is in darkness (Sun below −6°)"));
    onlyVisible_->setChecked(s.value(QStringLiteral("passes/onlyVisible"), false).toBool());
    opts->addWidget(onlyVisible_, 2, 0, 1, 2);
    utc_ = new QCheckBox(QStringLiteral("Times in UTC"));
    utc_->setChecked(s.value(QStringLiteral("passes/utc"), false).toBool());
    opts->addWidget(utc_, 3, 0, 1, 2);
    ll->addLayout(opts);
    auto* runRow = new QHBoxLayout;
    computeBtn_ = new QPushButton(QStringLiteral("Compute passes"));
    cancelBtn_ = new QPushButton(QStringLiteral("Cancel"));
    cancelBtn_->setEnabled(false);
    connect(computeBtn_, &QPushButton::clicked, this, &PassPage::compute);
    connect(cancelBtn_, &QPushButton::clicked, this, [this] {
        if (running_) running_->cancel = true;
    });
    runRow->addWidget(computeBtn_, 1);
    runRow->addWidget(cancelBtn_);
    ll->addLayout(runRow);
    progress_ = new QProgressBar;
    progress_->setTextVisible(true);
    progress_->setRange(0, 1);
    progress_->setValue(0);
    ll->addWidget(progress_);
    split->addWidget(left);

    connect(days_, qOverload<int>(&QSpinBox::valueChanged), this, [this](int v) {
        appSettings().setValue(QStringLiteral("passes/days"), v);
        scheduleCompute();
    });
    connect(minPeak_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        appSettings().setValue(QStringLiteral("passes/minPeak"), v);
        scheduleCompute();
    });
    connect(onlyVisible_, &QCheckBox::toggled, this, [this](bool on) {
        appSettings().setValue(QStringLiteral("passes/onlyVisible"), on);
        fillTable();
    });
    connect(utc_, &QCheckBox::toggled, this, [this](bool on) {
        appSettings().setValue(QStringLiteral("passes/utc"), on);
        fillTable();
    });

    // --- Centre: pass table over the map
    auto* centre = new QSplitter(Qt::Vertical);
    centre->setChildrenCollapsible(false);
    auto* tableBox = new QWidget;
    auto* tl = new QVBoxLayout(tableBox);
    tl->setContentsMargins(0, 0, 0, 0);
    resultLabel_ = new QLabel;
    resultLabel_->setStyleSheet(QStringLiteral("color:#9a9ea5;"));
    tl->addWidget(resultLabel_);
    table_ = new QTableWidget(0, 10);
    table_->setHorizontalHeaderLabels({QStringLiteral("Satellite"), QStringLiteral("AOS"), QStringLiteral("AOS az"),
                                       QStringLiteral("Max time"), QStringLiteral("Max el"), QStringLiteral("Max az"),
                                       QStringLiteral("LOS"), QStringLiteral("LOS az"), QStringLiteral("Duration"),
                                       QStringLiteral("Visible")});
    table_->setSelectionBehavior(QAbstractItemView::SelectRows);
    table_->setSelectionMode(QAbstractItemView::SingleSelection);
    table_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    table_->verticalHeader()->setVisible(false);
    table_->verticalHeader()->setDefaultSectionSize(22);
    table_->horizontalHeader()->setSectionResizeMode(QHeaderView::ResizeToContents);
    table_->horizontalHeader()->setStretchLastSection(true);
    connect(table_, &QTableWidget::itemSelectionChanged, this, &PassPage::onTableSelection);
    tl->addWidget(table_, 1);
    centre->addWidget(tableBox);
    map_ = new PassMap;
    centre->addWidget(map_);
    centre->setStretchFactor(0, 3);
    centre->setStretchFactor(1, 2);
    split->addWidget(centre);

    // --- Right: sky plot and live data
    auto* right = new QWidget;
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    sky_ = new PassSkyPlot;
    rl->addWidget(sky_, 3);
    auto* live = new QFrame;
    live->setObjectName(QStringLiteral("PassLiveCard"));
    live->setStyleSheet(QStringLiteral("QFrame#PassLiveCard { background:#232427; border:1px solid #35373c; border-radius:2px; }"));
    auto* lv = new QVBoxLayout(live);
    lv->setContentsMargins(12, 10, 12, 10);
    liveTitle_ = new QLabel(QStringLiteral("Not tracking"));
    liveTitle_->setStyleSheet(QStringLiteral("font-weight:600;color:#ff9f1c;font-size:14px;"));
    lv->addWidget(liveTitle_);
    auto* fr = new QHBoxLayout;
    fr->addWidget(new QLabel(QStringLiteral("Downlink")));
    freqMHz_ = new QDoubleSpinBox;
    freqMHz_->setRange(1, 100000);
    freqMHz_->setDecimals(4);
    freqMHz_->setSuffix(QStringLiteral(" MHz"));
    freqMHz_->setValue(s.value(QStringLiteral("passes/freqMHz"), 137.1).toDouble());
    connect(freqMHz_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        appSettings().setValue(QStringLiteral("passes/freqMHz"), v);
        liveTick();
    });
    fr->addWidget(freqMHz_, 1);
    lv->addLayout(fr);
    liveInfo_ = new QLabel;
    liveInfo_->setTextFormat(Qt::RichText);
    liveInfo_->setStyleSheet(QStringLiteral("color:#d6d8db;font-family:Consolas,'Cascadia Mono',monospace;"));
    liveInfo_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    lv->addWidget(liveInfo_);
    lv->addStretch(1);
    rl->addWidget(live, 2);
    split->addWidget(right);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 6);
    split->setStretchFactor(2, 3);
    split->setSizes({230, 760, 330});

    liveTimer_ = new QTimer(this);
    liveTimer_->setInterval(1000);
    connect(liveTimer_, &QTimer::timeout, this, &PassPage::liveTick);
    computeDebounce_ = new QTimer(this);
    computeDebounce_->setSingleShot(true);
    computeDebounce_->setInterval(700);
    connect(computeDebounce_, &QTimer::timeout, this, &PassPage::compute);
    progressTimer_ = new QTimer(this);
    progressTimer_->setInterval(150);
    connect(progressTimer_, &QTimer::timeout, this, [this] {
        if (running_) progress_->setValue(running_->done.load());
    });

    onlineLabel_ = new QLabel;
    onlineLabel_->setStyleSheet(QStringLiteral("color:#9a9ea5;font-size:11px;"));
    ll->addWidget(onlineLabel_);

    map_->setStation(station_.latDeg, station_.lonDeg);
    updateStationLabel();
    reloadTle();
    // Start-up update, a few seconds after the window is up so it never competes with start-up.
    if (autoUpdate_->isChecked()) QTimer::singleShot(3000, this, [this] { updateTleOnline(false); });
}

PassPage::~PassPage() {
    if (running_) running_->cancel = true;
    for (const QPointer<QThread>& t : jobThreads_)
        if (t) t->wait();
    if (tleThread_) tleThread_->wait();
    if (mapThread_) mapThread_->wait();
}

// ------------------------------------------------------------------------------------------------

void PassPage::setStation(double latDeg, double lonDeg, double altM, double horizonDeg) {
    const bool same = stationSet_ && latDeg == station_.latDeg && lonDeg == station_.lonDeg && altM == station_.altM &&
                      horizonDeg == horizonDeg_;
    station_.latDeg = latDeg;
    station_.lonDeg = lonDeg;
    station_.altM = altM;
    horizonDeg_ = horizonDeg;
    stationSet_ = true;
    map_->setStation(latDeg, lonDeg);
    updateStationLabel();
    if (!same) {
        passes_.clear();
        selectedPass_ = -1;
        sky_->setPass(nullptr);
        fillTable();
        scheduleCompute();
    }
}

void PassPage::setActive(bool on) {
    active_ = on;
    if (on) {
        loadMapInBackground();
        liveTick();
        liveTimer_->start();
    } else {
        liveTimer_->stop();
    }
}

void PassPage::updateStationLabel() {
    if (!stationSet_) {
        stationLabel_->setText(QStringLiteral("<span style='color:#e9bd62'>Station not set: enter its latitude and longitude on the Settings page</span>"));
        return;
    }
    stationLabel_->setText(QStringLiteral("Station %1   altitude %2 m   horizon %3°")
                               .arg(latLonText(station_.latDeg, station_.lonDeg))
                               .arg(station_.altM, 0, 'f', 0)
                               .arg(horizonDeg_, 0, 'f', 1));
}

QString PassPage::timeText(double t) const {
    const QDateTime local = QDateTime::fromMSecsSinceEpoch(qint64(std::llround(t * 1000.0)));
    return (utc_ && utc_->isChecked() ? local.toUTC() : local).toString(QStringLiteral("MM-dd HH:mm:ss"));
}

void PassPage::loadMapInBackground() {
    if (mapLoading_) return;
    mapLoading_ = true;
    const QString path = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("resources/maps/world_relief.jpg"));
    if (!QFileInfo::exists(path)) return;
    QPointer<PassPage> self(this);
    QThread* t = QThread::create([self, path] {
        QImageReader reader(path);
        reader.setScaledSize(QSize(2048, 1024));
        const QImage img = reader.read();
        if (img.isNull()) return;
        QMetaObject::invokeMethod(qApp, [self, img] {
            if (self) self->map_->setBackground(img);
        }, Qt::QueuedConnection);
    });
    mapThread_ = t;
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}

// ------------------------------------------------------------------------------------------------
// TLE

void PassPage::addTleFiles() {
    const QStringList files = QFileDialog::getOpenFileNames(this, QStringLiteral("Add TLE file"), QString(),
                                                            QStringLiteral("TLE (*.txt *.tle *.3le *.2le);;All files (*)"));
    if (files.isEmpty()) return;
    for (const QString& f : files)
        if (!tleFiles_.contains(f)) tleFiles_ << f;
    appSettings().setValue(QStringLiteral("passes/tleFiles"), tleFiles_);
    reloadTle();
}

void PassPage::clearTleFiles() {
    tleFiles_.clear();
    appSettings().setValue(QStringLiteral("passes/tleFiles"), tleFiles_);
    reloadTle();
}

void PassPage::reloadTle() {
    if (tleThread_) {
        // A load is still running; its result is dropped by the generation check.
        QTimer::singleShot(200, this, &PassPage::reloadTle);
        return;
    }
    const int gen = ++tleGeneration_;
    QStringList files;
    const QString fy2 = nadirstation::orbit::preferredTleFilePath();
    if (QFileInfo::exists(fy2)) files << fy2;
    // Downloaded groups, then the files added by hand (a later file wins for the same satellite).
    const QFileInfoList cached = QDir(tleCacheDir()).entryInfoList({QStringLiteral("*.txt")}, QDir::Files, QDir::Name);
    for (const QFileInfo& fi : cached) files << fi.absoluteFilePath();
    for (const QString& f : tleFiles_)
        if (!files.contains(f)) files << f;
    tleLabel_->setText(QStringLiteral("Reading TLEs…"));
    QPointer<PassPage> self(this);
    QThread* t = QThread::create([self, gen, files] {
        auto entries = std::make_shared<std::vector<TleEntry>>();
        QStringList report;
        QHash<int, int> pos;
        for (const QString& f : files) {
            QFile file(f);
            if (!file.open(QIODevice::ReadOnly)) {
                report << QStringLiteral("%1: cannot open").arg(QFileInfo(f).fileName());
                continue;
            }
            int skipped = 0;
            const std::vector<TleEntry> list = nadirstation::pass::parseTle(file.readAll().toStdString(), &skipped);
            for (TleEntry e : list) {
                e.source = QFileInfo(f).fileName().toStdString();
                // The same satellite in several files: the newest element set wins, whatever the
                // file order, so an old hand-added file never hides a fresh download.
                const auto it = pos.constFind(e.norad);
                if (it != pos.constEnd()) {
                    TleEntry& have = (*entries)[size_t(it.value())];
                    if (tleEpochKey(e.line1) >= tleEpochKey(have.line1)) have = e;
                } else {
                    pos.insert(e.norad, int(entries->size()));
                    entries->push_back(e);
                }
            }
            report << QStringLiteral("%1: %2 satellites%3").arg(QFileInfo(f).fileName()).arg(list.size())
                          .arg(skipped ? QStringLiteral(" (%1 sets could not be parsed)").arg(skipped) : QString());
        }
        QMetaObject::invokeMethod(qApp, [self, gen, entries, report] {
            if (self) self->onTleLoaded(gen, entries, report);
        }, Qt::QueuedConnection);
    });
    tleThread_ = t;
    connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start();
}

void PassPage::onTleLoaded(int generation, std::shared_ptr<std::vector<TleEntry>> entries, QStringList report) {
    if (generation != tleGeneration_) return;
    entries_ = std::move(*entries);
    indexByNorad_.clear();
    for (int i = 0; i < int(entries_.size()); ++i) indexByNorad_.insert(entries_[size_t(i)].norad, i);
    trackers_.clear();
    trackers_.resize(entries_.size());
    tleLabel_->setText(QStringLiteral("TLE: %1 satellites").arg(entries_.size()));
    tleLabel_->setToolTip(report.join(QLatin1Char('\n')));
    rebuildSatelliteList();
    if (followedNorad_ && !indexByNorad_.contains(followedNorad_)) followedNorad_ = 0;
    if (!followedNorad_ && !checked_.isEmpty()) followedNorad_ = checked_.first();
    scheduleCompute();
}

void PassPage::rebuildSatelliteList() {
    const QSignalBlocker block(satList_);
    satList_->clear();
    // Ticked satellites first, then the files' order.
    std::vector<int> order(entries_.size());
    for (int i = 0; i < int(order.size()); ++i) order[size_t(i)] = i;
    std::stable_sort(order.begin(), order.end(), [this](int a, int b) {
        return checked_.contains(entries_[size_t(a)].norad) && !checked_.contains(entries_[size_t(b)].norad);
    });
    for (int i : order) {
        const TleEntry& e = entries_[size_t(i)];
        auto* it = new QListWidgetItem(QStringLiteral("%1  (%2)").arg(QString::fromStdString(e.name)).arg(e.norad));
        it->setData(Qt::UserRole, e.norad);
        it->setFlags(it->flags() | Qt::ItemIsUserCheckable);
        it->setCheckState(checked_.contains(e.norad) ? Qt::Checked : Qt::Unchecked);
        satList_->addItem(it);
    }
    applyFilter();
}

void PassPage::applyFilter() {
    const QString f = filter_->text().trimmed();
    for (int i = 0; i < satList_->count(); ++i) {
        QListWidgetItem* it = satList_->item(i);
        it->setHidden(!f.isEmpty() && !it->text().contains(f, Qt::CaseInsensitive));
    }
}

void PassPage::onSatelliteItemChanged(QListWidgetItem* item) {
    const int norad = item->data(Qt::UserRole).toInt();
    if (item->checkState() == Qt::Checked) {
        if (!checked_.contains(norad)) checked_ << norad;
        if (!followedNorad_) followedNorad_ = norad;
    } else {
        checked_.removeAll(norad);
    }
    saveSelection();
    scheduleCompute();
}

void PassPage::setAllVisibleChecked(bool on) {
    {
        const QSignalBlocker block(satList_);
        for (int i = 0; i < satList_->count(); ++i) {
            QListWidgetItem* it = satList_->item(i);
            if (it->isHidden() && on) continue;
            it->setCheckState(on ? Qt::Checked : Qt::Unchecked);
            const int norad = it->data(Qt::UserRole).toInt();
            if (on && !checked_.contains(norad)) checked_ << norad;
            if (!on) checked_.removeAll(norad);
        }
    }
    if (on && !followedNorad_ && !checked_.isEmpty()) followedNorad_ = checked_.first();
    saveSelection();
    scheduleCompute();
}

void PassPage::saveSelection() const {
    QStringList ids;
    for (int n : checked_) ids << QString::number(n);
    QSettings s = appSettings();
    s.setValue(QStringLiteral("passes/selected"), ids);
    s.setValue(QStringLiteral("passes/followed"), followedNorad_);
}

std::vector<TleEntry> PassPage::checkedEntries() const {
    std::vector<TleEntry> out;
    for (int n : checked_) {
        const auto it = indexByNorad_.constFind(n);
        if (it != indexByNorad_.constEnd()) out.push_back(entries_[size_t(it.value())]);
    }
    return out;
}

Tracker* PassPage::trackerFor(int norad) {
    const auto it = indexByNorad_.constFind(norad);
    if (it == indexByNorad_.constEnd()) return nullptr;
    auto& t = trackers_[size_t(it.value())];
    if (!t) t = std::make_unique<Tracker>(entries_[size_t(it.value())]);
    return t->valid() ? t.get() : nullptr;
}

// ------------------------------------------------------------------------------------------------
// Prediction (worker threads)

void PassPage::scheduleCompute() {
    computeDebounce_->start();
}

void PassPage::compute() {
    computeDebounce_->stop();
    if (!stationSet_) {
        resultLabel_->setText(QStringLiteral("Enter the station's latitude and longitude on the Settings page to compute passes."));
        return;
    }
    std::vector<TleEntry> sats = checkedEntries();
    if (running_) running_->cancel = true;   // its result is ignored (generation)
    if (sats.empty()) {
        running_.reset();
        passes_.clear();
        fillTable();
        resultLabel_->setText(QStringLiteral("Tick satellites on the left and the passes are computed automatically."));
        computeBtn_->setEnabled(true);
        cancelBtn_->setEnabled(false);
        progress_->setRange(0, 1);
        progress_->setValue(0);
        return;
    }
    auto job = std::make_shared<Job>();
    job->sats = std::move(sats);
    job->st = station_;
    job->generation = ++jobGeneration_;
    // Start half an hour back so a pass already in progress is listed with its real AOS.
    job->opt.startUnix = nowUnix() - 1800.0;
    job->opt.days = days_->value();
    job->opt.horizonDeg = horizonDeg_;
    job->opt.minPeakDeg = std::max(minPeak_->value(), horizonDeg_);
    windowStart_ = job->opt.startUnix;
    windowEnd_ = job->opt.startUnix + job->opt.days * 86400.0;
    running_ = job;

    progress_->setRange(0, int(job->sats.size()));
    progress_->setValue(0);
    progress_->setFormat(QStringLiteral("%v / %m satellites"));
    computeBtn_->setEnabled(false);
    cancelBtn_->setEnabled(true);
    resultLabel_->setText(QStringLiteral("Computing passes of %1 satellites over %2 days…").arg(job->sats.size()).arg(job->opt.days));
    progressTimer_->start();

    // Leave one core to the decoder when it is running. The worker holds its own reference to the
    // job, so a cancelled job stays valid until its thread has stopped.
    const int threads = std::max(1, QThread::idealThreadCount() - 1);
    QThread* th = QThread::create([job, threads] {
        job->result = nadirstation::pass::predictAll(job->sats, job->st, job->opt, &job->done, &job->cancel, threads);
    });
    job->thread = th;
    jobThreads_.removeAll(QPointer<QThread>());
    jobThreads_ << QPointer<QThread>(th);
    connect(th, &QThread::finished, this, [this, job] { onComputeFinished(job); });
    connect(th, &QThread::finished, th, &QObject::deleteLater);
    th->start(QThread::LowPriority);
}

void PassPage::onComputeFinished(std::shared_ptr<Job> job) {
    job->thread = nullptr;
    if (job != running_) return;   // superseded
    running_.reset();
    progressTimer_->stop();
    computeBtn_->setEnabled(true);
    cancelBtn_->setEnabled(false);
    progress_->setValue(progress_->maximum());
    if (job->cancel) {
        resultLabel_->setText(QStringLiteral("Cancelled"));
        return;
    }
    passes_ = std::move(job->result);
    selectedPass_ = -1;
    fillTable();
    lastTrackAt_ = 0;
    liveTick();
}

// ------------------------------------------------------------------------------------------------
// Table

void PassPage::fillTable() {
    const double now = nowUnix();
    lastTableAt_ = now;
    // Keep the selected pass selected across refreshes.
    int keepNorad = 0;
    double keepAos = 0;
    if (selectedPass_ >= 0 && selectedPass_ < int(passes_.size())) {
        keepNorad = passes_[size_t(selectedPass_)].norad;
        keepAos = passes_[size_t(selectedPass_)].aos;
    }
    const QSignalBlocker block(table_);
    table_->setRowCount(0);
    const bool visOnly = onlyVisible_->isChecked();
    int rows = 0, total = 0, selectRow = -1, nextRow = -1;
    for (int i = 0; i < int(passes_.size()); ++i) {
        const Pass& p = passes_[size_t(i)];
        if (p.los < now && !p.endsAfterWindow) continue;
        if (visOnly && !p.visible) continue;
        ++total;
        if (rows >= kMaxRows) continue;
        table_->insertRow(rows);
        const bool current = p.aos <= now && now <= p.los;
        auto cell = [&](int col, const QString& text, bool right = true) {
            auto* it = new QTableWidgetItem(text);
            if (right) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            if (current) it->setBackground(QColor(0x2f, 0x9e, 0x6a, 70));
            table_->setItem(rows, col, it);
            return it;
        };
        QTableWidgetItem* nameItem = cell(0, QString::fromStdString(p.name), false);
        nameItem->setData(Qt::UserRole, i);
        nameItem->setToolTip(QStringLiteral("NORAD %1").arg(p.norad));
        if (p.alwaysUp) {
            cell(1, QStringLiteral("Always up"));
            cell(2, QString());
        } else {
            cell(1, p.startsBeforeWindow ? QStringLiteral("(already up)") : timeText(p.aos));
            cell(2, QStringLiteral("%1° %2").arg(p.aosAzDeg, 0, 'f', 0).arg(compass(p.aosAzDeg)));
        }
        cell(3, timeText(p.tca));
        QTableWidgetItem* el = cell(4, QStringLiteral("%1°").arg(p.maxElDeg, 0, 'f', 1));
        if (p.maxElDeg >= 45) el->setForeground(QColor(0x3d, 0xc4, 0x7e));
        else if (p.maxElDeg >= 20) el->setForeground(QColor(0xe0, 0xc0, 0x50));
        cell(5, QStringLiteral("%1° %2").arg(p.tcaAzDeg, 0, 'f', 0).arg(compass(p.tcaAzDeg)));
        if (p.alwaysUp || p.endsAfterWindow) {
            cell(6, QStringLiteral("(after window)"));
            cell(7, QString());
        } else {
            cell(6, timeText(p.los));
            cell(7, QStringLiteral("%1° %2").arg(p.losAzDeg, 0, 'f', 0).arg(compass(p.losAzDeg)));
        }
        cell(8, p.alwaysUp ? QStringLiteral("—") : durationText(p.los - p.aos));
        cell(9, p.visible ? QStringLiteral("Visual") : QString(), false);
        if (p.norad == keepNorad && std::fabs(p.aos - keepAos) < 1.0) selectRow = rows;
        if (nextRow < 0 && p.norad == followedNorad_ && p.los >= now) nextRow = rows;
        ++rows;
    }
    if (passes_.empty())
        resultLabel_->setText(running_ ? resultLabel_->text() : QStringLiteral("No passes (or not computed yet)"));
    else
        resultLabel_->setText(QStringLiteral("%1 passes%2   times in %3   window %4 → %5")
                                  .arg(total)
                                  .arg(total > kMaxRows ? QStringLiteral(" (first %1 shown)").arg(kMaxRows) : QString())
                                  .arg(utc_->isChecked() ? QStringLiteral(" UTC") : QStringLiteral("local time"))
                                  .arg(timeText(windowStart_), timeText(windowEnd_)));
    const int row = selectRow >= 0 ? selectRow : nextRow;
    if (row >= 0) {
        table_->selectRow(row);
        selectedPass_ = table_->item(row, 0)->data(Qt::UserRole).toInt();
        sky_->setPass(&passes_[size_t(selectedPass_)]);
    } else {
        selectedPass_ = -1;
        sky_->setPass(nullptr);
    }
}

void PassPage::onTableSelection() {
    const QList<QTableWidgetItem*> sel = table_->selectedItems();
    if (sel.isEmpty()) return;
    const QTableWidgetItem* nameItem = table_->item(sel.first()->row(), 0);
    if (!nameItem) return;
    const int i = nameItem->data(Qt::UserRole).toInt();
    if (i < 0 || i >= int(passes_.size())) return;
    selectedPass_ = i;
    sky_->setPass(&passes_[size_t(i)]);
    if (followedNorad_ != passes_[size_t(i)].norad) {
        followedNorad_ = passes_[size_t(i)].norad;
        appSettings().setValue(QStringLiteral("passes/followed"), followedNorad_);
        lastTrackAt_ = 0;
    }
    liveTick();
}

// ------------------------------------------------------------------------------------------------
// Live

void PassPage::liveTick() {
    if (!active_) return;
    const double now = nowUnix();
    double sunLat = 0, sunLon = 0;
    nadirstation::globe::subSolarPoint(now, &sunLat, &sunLon);
    map_->setSun(sunLat, sunLon);
    if (now - lastTableAt_ > 30.0) fillTable();   // drop finished passes, move the "now" highlight

    QVector<PassSkyPlot::Marker> markers;
    QVector<PassMap::Sat> sats;
    int n = 0;
    for (int norad : checked_) {
        if (++n > kMaxLive) break;
        Tracker* tr = trackerFor(norad);
        if (!tr) continue;
        const Look l = tr->look(station_, now, false);
        if (!l.ok) continue;
        const QString name = QString::fromStdString(tr->tle().name);
        const bool followed = norad == followedNorad_;
        markers.push_back({name, l.azDeg, l.elDeg, followed});
        sats.push_back({name, l.latDeg, l.lonDeg, nadirstation::pass::footprintRadiusDeg(l.altKm, horizonDeg_), followed});
    }
    sky_->setMarkers(markers);
    map_->setSatellites(sats);

    Tracker* ft = followedNorad_ ? trackerFor(followedNorad_) : nullptr;
    if (!ft) {
        liveTitle_->setText(QStringLiteral("Not tracking"));
        liveInfo_->setText(QStringLiteral("<span style='color:#9a9ea5'>Tick satellites, then double-click one in the list or click a row of the pass table to track it.</span>"));
        map_->setTrack({});
        return;
    }
    // Ground track: half a revolution back, one and a half ahead (recomputed every 20 s).
    if (now - lastTrackAt_ > 20.0) {
        lastTrackAt_ = now;
        const double period = ft->periodMinutes() * 60.0;
        QVector<QPointF> track;
        const int pts = 300;
        const double span = std::min(2.0 * period, 2.0 * 86400.0);
        for (int i = 0; i <= pts; ++i) {
            const double t = now - span / 4.0 + span * i / pts;
            double la = 0, lo = 0, alt = 0;
            if (ft->subPoint(t, &la, &lo, &alt)) track << QPointF(lo, la);
        }
        map_->setTrack(track);
    }

    const Look l = ft->look(station_, now, true);
    liveTitle_->setText(QStringLiteral("%1  ·  NORAD %2").arg(QString::fromStdString(ft->tle().name)).arg(ft->tle().norad));
    if (!l.ok) {
        liveInfo_->setText(QStringLiteral("<span style='color:#f0594c'>Cannot propagate (TLE too old or the satellite has re-entered)</span>"));
        return;
    }
    const double dop = nadirstation::pass::dopplerHz(freqMHz_->value() * 1e6, l.rangeRateKmS);
    const double ageDays = (now - ft->epochUnix()) / 86400.0;
    auto row = [](const QString& k, const QString& v) {
        return QStringLiteral("<tr><td style='color:#9a9ea5;padding-right:10px'>%1</td><td>%2</td></tr>").arg(k, v);
    };
    QString h = QStringLiteral("<table cellspacing='1'>");
    const QString elColor = l.elDeg >= 0 ? QStringLiteral("#3dc47e") : QStringLiteral("#9a9ea5");
    h += row(QStringLiteral("Azimuth"), QStringLiteral("%1°　%2").arg(l.azDeg, 0, 'f', 2).arg(compass(l.azDeg)));
    h += row(QStringLiteral("Elevation"), QStringLiteral("<span style='color:%1'>%2°</span>").arg(elColor).arg(l.elDeg, 0, 'f', 2));
    h += row(QStringLiteral("Range"), QStringLiteral("%1 km").arg(l.rangeKm, 0, 'f', 1));
    h += row(QStringLiteral("Range rate"), QStringLiteral("%1 km/s").arg(l.rangeRateKmS, 0, 'f', 3));
    h += row(QStringLiteral("Doppler"), QStringLiteral("%1%2 Hz").arg(dop >= 0 ? QStringLiteral("+") : QString()).arg(dop, 0, 'f', 0));
    h += row(QStringLiteral("Sub-point"), latLonText(l.latDeg, l.lonDeg));
    h += row(QStringLiteral("Altitude"), QStringLiteral("%1 km").arg(l.altKm, 0, 'f', 1));
    h += row(QStringLiteral("Illumination"), l.sunlit ? QStringLiteral("Sunlit") : QStringLiteral("In shadow"));
    h += row(QStringLiteral("Sun elevation"), QStringLiteral("%1° (station)").arg(l.sunElDeg, 0, 'f', 1));
    h += row(QStringLiteral("Period"), QStringLiteral("%1 min").arg(ft->periodMinutes(), 0, 'f', 2));
    h += row(QStringLiteral("TLE age"), QStringLiteral("<span style='color:%1'>%2 days</span>")
                                             .arg(std::fabs(ageDays) > 14 ? QStringLiteral("#e05d5d") : QStringLiteral("#d6d8db"))
                                             .arg(ageDays, 0, 'f', 1));
    h += row(QStringLiteral("TLE epoch"), QStringLiteral("%1 UTC").arg(
             QDateTime::fromMSecsSinceEpoch(qint64(ft->epochUnix() * 1000.0)).toUTC().toString(QStringLiteral("yyyy-MM-dd HH:mm"))));
    h += row(QStringLiteral("TLE source"), QString::fromStdString(ft->tle().source).toHtmlEscaped());
    // Current or next pass of the followed satellite.
    for (const Pass& p : passes_) {
        if (p.norad != followedNorad_ || p.los < now) continue;
        if (p.alwaysUp)
            h += row(QStringLiteral("Pass"), QStringLiteral("Above the horizon for the whole prediction window"));
        else if (p.aos <= now)
            h += row(QStringLiteral("In pass"), QStringLiteral("LOS in %1, max %2°").arg(durationText(p.los - now)).arg(p.maxElDeg, 0, 'f', 1));
        else
            h += row(QStringLiteral("Next pass"), QStringLiteral("in %1 (%2), max %3°")
                                                    .arg(durationText(p.aos - now), timeText(p.aos))
                                                    .arg(p.maxElDeg, 0, 'f', 1));
        break;
    }
    h += QStringLiteral("</table>");
    if (std::fabs(ageDays) > 14)
        h += QStringLiteral("<div style='color:#f0594c;margin-top:4px'>The TLE is more than two weeks old and the position error grows; update the TLE file.</div>");
    liveInfo_->setText(h);
}

// ------------------------------------------------------------------------------------------------
// Online TLE update (asynchronous)

void PassPage::updateTleOnline(bool force) {
    if (downloadsPending_ > 0) return;
    QDir().mkpath(tleCacheDir());
    if (!net_) {
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        net_ = new QNetworkAccessManager(this);
    }
    downloadsOk_ = downloadsFailed_ = downloadsSkipped_ = 0;
    downloadError_.clear();
    fy2Fresh_.clear();
    triedFallback_ = false;
    auto request = [this](const QString& url, const QString& kind, const QString& target) { startDownload(url, kind, target); };

    const QStringList groups = appSettings().value(QStringLiteral("passes/onlineGroups"), kDefaultGroups).toStringList();
    for (const QString& g : groups) {
        const QString file = QDir(tleCacheDir()).filePath(g + QStringLiteral(".txt"));
        if (!force && fresh(file)) { ++downloadsSkipped_; continue; }
        request(QStringLiteral("https://celestrak.org/NORAD/elements/gp.php?GROUP=%1&FORMAT=tle").arg(g),
                QStringLiteral("group"), file);
    }
    if (force || !fresh(writableFy2Tle())) {
        for (int id : kFy2Norad)
            request(QStringLiteral("https://celestrak.org/NORAD/elements/gp.php?CATNR=%1&FORMAT=tle").arg(id),
                    QStringLiteral("fy2"), QString());
    } else {
        ++downloadsSkipped_;
    }

    if (downloadsPending_ == 0) {
        onlineLabel_->setText(QStringLiteral("TLEs were updated less than two hours ago; not downloading"));
        return;
    }
    updateBtn_->setEnabled(false);
    onlineLabel_->setText(QStringLiteral("Updating TLEs online…"));
}

void PassPage::startDownload(const QString& url, const QString& kind, const QString& target) {
    QNetworkRequest req{QUrl(url)};
    req.setTransferTimeout(kind == QStringLiteral("satnogs") ? 60000 : 20000);
    req.setHeader(QNetworkRequest::UserAgentHeader, QStringLiteral("NadirStation (TLE update)"));
    QNetworkReply* r = net_->get(req);
    r->setProperty("kind", kind);
    r->setProperty("target", target);
    ++downloadsPending_;
    connect(r, &QNetworkReply::finished, this, [this, r] { onDownloadFinished(r); });
}

void PassPage::onDownloadFinished(QNetworkReply* r) {
    r->deleteLater();
    const QString kind = r->property("kind").toString();
    bool ok = false;
    if (r->error() == QNetworkReply::NoError) {
        QByteArray data = r->readAll();
        if (kind == QStringLiteral("satnogs")) {
            // SatNOGS DB answers with a JSON array of {tle0, tle1, tle2, norad_cat_id}.
            QByteArray text;
            const QJsonArray arr = QJsonDocument::fromJson(data).array();
            for (const QJsonValue& v : arr) {
                const QJsonObject o = v.toObject();
                QString name = o.value(QStringLiteral("tle0")).toString();
                if (name.startsWith(QStringLiteral("0 "))) name = name.mid(2);
                text += name.toUtf8() + "\n" + o.value(QStringLiteral("tle1")).toString().toUtf8() + "\n" +
                        o.value(QStringLiteral("tle2")).toString().toUtf8() + "\n";
            }
            data = text;
        }
        const std::vector<TleEntry> list = nadirstation::pass::parseTle(data.toStdString());
        // Celestrak answers an unknown group or a rate limit with a short text, not TLEs: only a
        // reply that contains TLEs replaces the local file.
        if (!list.empty()) {
            if (kind == QStringLiteral("fy2")) {
                fy2Fresh_.insert(fy2Fresh_.end(), list.begin(), list.end());
                ok = true;
            } else {
                if (kind == QStringLiteral("satnogs"))
                    for (const TleEntry& e : list)
                        for (int id : kFy2Norad)
                            if (e.norad == id) fy2Fresh_.push_back(e);
                QSaveFile f(r->property("target").toString());
                if (f.open(QIODevice::WriteOnly) && f.write(tleText(list)) >= 0 && f.commit()) ok = true;
                else downloadError_ = QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(f.fileName()));
            }
        } else {
            downloadError_ = QStringLiteral("%1 did not return TLEs: %2").arg(r->url().host(), QString::fromUtf8(data.left(100)).trimmed());
        }
    } else {
        downloadError_ = QStringLiteral("%1：%2").arg(r->url().host(), r->errorString());
    }
    ok ? ++downloadsOk_ : ++downloadsFailed_;
    if (--downloadsPending_ == 0) finishOnlineUpdate();
}

void PassPage::finishOnlineUpdate() {
    // Celestrak is often slow or unreachable from some networks (mainland China in particular).
    // When nothing came from it, fall back to the SatNOGS database (all tracked satellites, one file).
    if (downloadsOk_ == 0 && !triedFallback_) {
        triedFallback_ = true;
        const QString firstError = downloadError_;
        downloadsFailed_ = 0;
        onlineLabel_->setText(QStringLiteral("Celestrak unreachable (%1); downloading from SatNOGS…").arg(firstError.left(60)));
        startDownload(QStringLiteral("https://db.satnogs.org/api/tle/?format=json"), QStringLiteral("satnogs"),
                      QDir(tleCacheDir()).filePath(QStringLiteral("satnogs.txt")));
        downloadError_ = firstError;
        return;
    }
    updateBtn_->setEnabled(true);
    // FY-2G / FY-2H: merged into the writable fy2.tle, keeping any other satellites it holds.
    if (!fy2Fresh_.empty()) {
        std::vector<TleEntry> merged;
        QFile old(nadirstation::orbit::preferredTleFilePath());
        if (old.open(QIODevice::ReadOnly)) merged = nadirstation::pass::parseTle(old.readAll().toStdString());
        old.close();
        for (const TleEntry& e : fy2Fresh_) {
            auto it = std::find_if(merged.begin(), merged.end(), [&](const TleEntry& m) { return m.norad == e.norad; });
            if (it != merged.end()) *it = e;
            else merged.push_back(e);
        }
        QDir().mkpath(QFileInfo(writableFy2Tle()).absolutePath());
        QSaveFile f(writableFy2Tle());
        if (!(f.open(QIODevice::WriteOnly) && f.write(tleText(merged)) >= 0 && f.commit()))
            downloadError_ = QStringLiteral("Cannot write %1").arg(QDir::toNativeSeparators(writableFy2Tle()));
    }
    const QString when = QDateTime::currentDateTime().toString(QStringLiteral("HH:mm"));
    if (downloadsOk_ == 0) {
        onlineLabel_->setText(QStringLiteral("%1 online update failed, using the local TLEs: %2").arg(when, downloadError_.left(120)));
        onlineLabel_->setWordWrap(true);
        onlineLabel_->setToolTip(downloadError_);
        return;
    }
    onlineLabel_->setText(QStringLiteral("%1 TLEs updated online: %2 succeeded%3")
                              .arg(when)
                              .arg(downloadsOk_)
                              .arg(downloadsFailed_ ? QStringLiteral(", %1 failed").arg(downloadsFailed_) : QString()));
    onlineLabel_->setToolTip(downloadError_);
    reloadTle();
}
