#include "spaceweatherpage.h"

#include "ggak.h"

#include <QComboBox>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QImage>
#include <QImageWriter>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimeZone>
#include <QTimer>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

using namespace nadirstation::spaceweather;

namespace {
QColor levelColour(Level l) {
    switch (l) {
    case Level::Quiet: return QColor(95, 191, 143);
    case Level::Active: return QColor(233, 189, 98);
    case Level::Elevated: return QColor(224, 138, 74);
    case Level::Strong: return QColor(224, 90, 90);
    case Level::Unknown: break;
    }
    return QColor(110, 115, 122);
}
const QColor kLineColours[] = {QColor(122, 176, 224), QColor(150, 210, 170), QColor(220, 180, 120)};

QString utcText(double t, const char* format = "MM-dd HH:mm") {
    return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(t * 1000.0), QTimeZone::utc()).toString(QString::fromLatin1(format));
}

// Values are kept per channel by time (milliseconds), so a frame seen twice -- live, and again when
// the day's file is read -- counts once.
using Store = std::map<long long, double>;
std::map<std::string, Store>& stores() {
    static std::map<std::string, Store> s;
    return s;
}
}  // namespace

// ---------------------------------------------------------------------------------------------

SwSparkline::SwSparkline(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(142);
}

void SwSparkline::setData(const std::vector<Line>& lines, Mode mode, double tStart, double tEnd) {
    lines_ = lines;
    mode_ = mode;
    t0_ = tStart;
    t1_ = std::max(tEnd, tStart + 1.0);
    update();
}

void SwSparkline::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.fillRect(rect(), QColor(22, 24, 27));
    const QRectF r = QRectF(rect()).adjusted(48, 30, -12, -24);
    if (r.width() <= 0.0 || r.height() <= 0.0) return;
    QFont f = font();
    f.setPointSizeF(7.5);
    p.setFont(f);
    p.setPen(QColor(164, 169, 175));
    p.drawText(QRectF(7, 5, 110, 17), Qt::AlignLeft | Qt::AlignVCenter,
               mode_ == Mode::Decrease ? QStringLiteral("Relative change %") : QStringLiteral("Relative to baseline ×"));
    int legendX = 116;
    for (const Line& l : lines_) {
        if (legendX + 80 > width()) break;
        p.setPen(QPen(l.colour, 2));
        p.drawLine(legendX, 13, legendX + 13, 13);
        p.setPen(QColor(196, 199, 203));
        const int textWidth = std::min(95, width() - legendX - 24);
        const QString label = p.fontMetrics().elidedText(l.label, Qt::ElideRight, textWidth);
        p.drawText(QRectF(legendX + 18, 5, textWidth, 17), Qt::AlignLeft | Qt::AlignVCenter, label);
        legendX += 24 + textWidth;
    }
    // y: ratio to baseline (1 = usual) or percent change (0 = usual).
    auto yOf = [&](const Bin& b, double base) {
        if (mode_ == Mode::Decrease) return base != 0.0 ? (b.value / base - 1.0) * 100.0 : 0.0;
        return base != 0.0 ? b.value / base : b.value;
    };
    double lo = mode_ == Mode::Decrease ? -4.0 : 0.6, hi = mode_ == Mode::Decrease ? 4.0 : 1.6;
    for (const Line& l : lines_) {
        if (l.baseline == 0.0) continue;
        for (const Bin& b : l.bins) {
            if (b.samples < 10) continue;
            const double y = yOf(b, l.baseline);
            lo = std::min(lo, y); hi = std::max(hi, y);
        }
    }
    const double pad = (hi - lo) * 0.08;
    lo -= pad; hi += pad;
    auto X = [&](double t) { return r.left() + (t - t0_) / (t1_ - t0_) * r.width(); };
    auto Y = [&](double y) { return r.bottom() - (y - lo) / (hi - lo) * r.height(); };
    for (int i = 0; i <= 4; ++i) {
        const double y = lo + (hi - lo) * i / 4.0;
        const double py = Y(y);
        p.setPen(QPen(QColor(54, 58, 63), 1));
        p.drawLine(QPointF(r.left(), py), QPointF(r.right(), py));
        p.setPen(QColor(143, 149, 155));
        p.drawText(QRectF(2, py - 8, 41, 16), Qt::AlignRight | Qt::AlignVCenter,
                   QString::number(y, 'f', mode_ == Mode::Decrease ? 1 : 2));
    }
    for (int i = 0; i <= 4; ++i) {
        const double x = r.left() + r.width() * i / 4.0;
        p.setPen(QPen(QColor(43, 47, 51), 1));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.setPen(QColor(143, 149, 155));
        const double t = t0_ + (t1_ - t0_) * i / 4.0;
        p.drawText(QRectF(x - 35, r.bottom() + 4, 70, 16), Qt::AlignCenter,
                   utcText(t, "MM-dd HH:mm"));
    }
    // The "usual" line.
    const double usual = mode_ == Mode::Decrease ? 0.0 : 1.0;
    p.setPen(QPen(QColor(110, 115, 122, 150), 1.0, Qt::DashLine));
    p.drawLine(QPointF(r.left(), Y(usual)), QPointF(r.right(), Y(usual)));
    int k = 0;
    for (const Line& l : lines_) {
        const QColor c = l.colour.isValid() ? l.colour : kLineColours[k % 3];
        ++k;
        if (l.baseline == 0.0 || l.bins.empty()) continue;
        QPainterPath path;
        bool first = true;
        double prevT = 0.0;
        for (const Bin& b : l.bins) {
            if (b.t < t0_ || b.t > t1_ + 300.0 || b.samples < 10) continue;   // a bin of a few samples is noise
            const QPointF pt(X(std::min(b.t, t1_)), Y(yOf(b, l.baseline)));
            // A gap of more than half an hour (no reception) breaks the line.
            if (first || b.t - prevT > 1800.0) path.moveTo(pt); else path.lineTo(pt);
            first = false;
            prevT = b.t;
        }
        p.setPen(QPen(c, 1.6));
        p.drawPath(path);
        if (!first) {
            p.setBrush(c);
            p.drawEllipse(path.currentPosition(), 2.6, 2.6);
            p.setBrush(Qt::NoBrush);
        }
    }
    if (lines_.empty()) {
        p.setPen(QColor(143, 149, 155));
        p.drawText(r, Qt::AlignCenter, QStringLiteral("Waiting for valid telemetry"));
    }
}

// ---------------------------------------------------------------------------------------------

SpaceWeatherPage::SpaceWeatherPage(QWidget* parent) : QWidget(parent) {
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea(this);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setWidgetResizable(true);
    outer->addWidget(scroll);
    report_ = new QWidget;
    scroll->setWidget(report_);
    auto* root = new QVBoxLayout(report_);
    root->setContentsMargins(8, 8, 8, 12);
    root->setSpacing(10);

    auto* head = new QFrame;
    head->setObjectName(QStringLiteral("SwBanner"));
    head->setStyleSheet(QStringLiteral("#SwBanner{background:#1f2023;border:1px solid #35373c;border-left:3px solid #b2b6ba;border-radius:2px;}"));
    auto* hl = new QHBoxLayout(head);
    hl->setContentsMargins(16, 12, 16, 12);
    auto* bannerText = new QVBoxLayout;
    auto* reportTitle = new QLabel(QStringLiteral("ELEKTRO-L N4  /  GGAK  /  SPACE ENVIRONMENT"));
    reportTitle->setStyleSheet(QStringLiteral("color:#b9bdc1;font-size:10pt;font-weight:600;letter-spacing:1px;"));
    bannerText->addWidget(reportTitle);
    banner_ = new QLabel(QStringLiteral("Space environment: —"));
    bannerDetail_ = new QLabel;
    bannerDetail_->setStyleSheet(QStringLiteral("color:#9a9ea5;"));
    bannerDetail_->setWordWrap(true);
    bannerText->addWidget(banner_);
    bannerText->addWidget(bannerDetail_);
    range_ = new QLabel(QStringLiteral("Shown: —"));
    range_->setWordWrap(true);
    range_->setStyleSheet(QStringLiteral("color:#c2c6ca;font-family:Consolas,monospace;font-size:9pt;"));
    bannerText->addWidget(range_);
    hl->addLayout(bannerText, 1);
    controls_ = new QWidget(head);
    auto* wl = new QVBoxLayout(controls_);
    wl->setContentsMargins(0, 0, 0, 0);
    wl->setSpacing(4);
    wl->addWidget(new QLabel(QStringLiteral("Time window · UTC")));
    window_ = new QComboBox;
    window_->addItem(QStringLiteral("10 min"), 600.0);
    window_->addItem(QStringLiteral("1 h"), 3600.0);
    window_->addItem(QStringLiteral("6 h"), 21600.0);
    window_->addItem(QStringLiteral("24 h"), 86400.0);
    window_->setCurrentIndex(1);
    wl->addWidget(window_);
    auto* exportButton = new QPushButton(QStringLiteral("Export PNG"));
    exportButton->setToolTip(QStringLiteral("Export the space-weather charts and data notes of the current time window"));
    wl->addWidget(exportButton);
    connect(exportButton, &QPushButton::clicked, this, [this] {
        const QString folder = dataRoot_.isEmpty()
            ? QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation)
            : QDir(dataRoot_).filePath(QStringLiteral("ELEKTRO-L/GGAK"));
        QDir().mkpath(folder);
        const QString initial = QDir(folder)
            .filePath(QStringLiteral("N4_GGAK_space_weather_%1.png")
                          .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
        const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export space weather PNG"), initial,
                                                          QStringLiteral("PNG image (*.png)"));
        if (!path.isEmpty() && !savePng(path))
            bannerDetail_->setText(QStringLiteral("PNG export failed: %1").arg(path));
    });
    hl->addWidget(controls_);
    root->addWidget(head);

    // Channels checked on the 10-18 N4 frames (1547-1573 records per ID, 02:28-03:36 UTC):
    //  - particle counts: within each minute the variance equals the mean (Poisson, ratio 1.03-1.07)
    //    and successive seconds are uncorrelated: 44 byte 5; 60 bytes 5, 17, 25, 39, 41, 45.
    //    44 byte 11, used before as electron channel A, is not a count (variance 37 times the mean,
    //    a regular square-wave pattern), so it is left out.
    //  - GALS-E words 3, 5, 7 hold even numbers only, twice a Poisson count (ratio 1.8-1.9).
    //  - magnetometer B1-B3: continuous 24-bit values (the low byte wraps 255 <-> 0 as the high byte
    //    steps), so the field positions are right; units unknown.
    //  - 28 byte 2 is a slow analogue value and 12 byte 3 is nearly constant (225-239): nothing shows
    //    they are solar X-ray / UV, so that card is not verified.
    // Physical units: particle and cosmic-ray channels are counts per one-second record, so the
    // 10-minute mean is a count rate in s^-1 (GALS-E words carry twice the count, scale 0.5).
    // FM-E: 0.01 nT per count, inferred, not from a calibration document: FM-E measures +-600 nT per
    // component with 0.01 nT as its smallest step (WMO OSCAR, eoPortal); the 24-bit values here stay
    // inside +-60000 (top byte is only sign), which +-600 nT at 0.01 nT fills; and at 0.01 nT the
    // minute-to-minute changes (0.2-0.6 nT RMS) and 10-minute ranges (0.6-2.5 nT) are the size
    // GOES-18 saw over the same minutes (0.1-0.8 nT, 0.3-3.6 nT). The absolute vector at that scale
    // is about 516 nT, several times the 100-115 nT geomagnetic field at GEO, so it includes the
    // spacecraft's own field and sensor offsets: only changes (delta B, delta B per minute) are used.
    cards_ = {
        {"Solar X-ray / UV", "VUSS-E / DIR-E (unverified)", Mode::Ratio, {{"28 byte 2", "Sensor A"}, {"12 byte 3", "Sensor B"}}},
        {"Energetic electrons", "Particle spectrometer (electrons)", Mode::Ratio, {{"44 byte 5", "Count rate", 1.0, "s⁻¹"}}},
        {"Energetic protons", "Particle spectrometer (protons)", Mode::Ratio, {{"60 count 41", "Main channel", 1.0, "s⁻¹"}, {"60 low sum", "Sum of 5 channels", 1.0, "s⁻¹"}}},
        {"Galactic cosmic rays", "GALS-E", Mode::Decrease, {{"64 sum", "Sum of 3 channels", 0.5, "s⁻¹"}}},
        {"Magnetic field at the spacecraft", "FM-E magnetometer", Mode::Variability, {{"80 B1 (24-bit)", "B1", 0.01, "nT"}, {"80 B2 (24-bit)", "B2", 0.01, "nT"}, {"80 B3 (24-bit)", "B3", 0.01, "nT"}}},
    };
    cards_[0].verified = false;   // solar X-ray / UV
    auto* grid = new QGridLayout;
    grid->setSpacing(10);
    grid->setColumnStretch(0, 1);
    int i = 0;
    for (Card& c : cards_) {
        auto* fr = new QFrame;
        fr->setObjectName(QStringLiteral("SwCard"));
        fr->setStyleSheet(QStringLiteral("#SwCard{background:#1f2023;border:1px solid #35373c;border-radius:2px;}"));
        auto* v = new QVBoxLayout(fr);
        v->setContentsMargins(12, 10, 12, 10);
        v->setSpacing(4);
        auto* top = new QHBoxLayout;
        auto* title = new QLabel(QStringLiteral("<b>%1</b> <span style='color:#6e737a'>%2</span>")
                                     .arg(QString::fromUtf8(c.title), QString::fromUtf8(c.instrument)));
        title->setWordWrap(true);
        c.state = new QLabel(QStringLiteral("—"));
        c.state->setAlignment(Qt::AlignCenter);
        c.state->setMinimumWidth(84);
        top->addWidget(title, 1);
        top->addWidget(c.state);
        v->addLayout(top);
        c.value = new QLabel;
        c.value->setWordWrap(true);
        // Sizes by style sheet: the application style sheet sets QLabel fonts and would win over setFont.
        c.value->setStyleSheet(QStringLiteral("font-size:13pt;"));
        v->addWidget(c.value);
        c.plot = new SwSparkline;
        v->addWidget(c.plot, 1);
        c.detail = new QLabel;
        c.detail->setStyleSheet(QStringLiteral("color:#6f8294;"));
        c.detail->setWordWrap(true);
        v->addWidget(c.detail);
        c.frame = fr;
        grid->addWidget(fr, i, 0);
        ++i;
    }
    root->addLayout(grid, 1);

    auto* note = new QLabel(QStringLiteral(
        "Source  ELEKTRO-L N4 · GGAK · telemetry received at this station (165.8°E)    |    time  UTC    |    statistics  10-minute means; the median over the chosen window is the station baseline.\n"
        "Particles / cosmic rays: count rates in s⁻¹, not calibrated fluxes, and not convertible to NOAA S levels. Magnetic field: 0.01 nT per count is an inferred scale; B1–B3 are instrument axes and only the change is shown.\n"
        "Station-relative indices: particles 1.5× / 3× / 10×, cosmic-ray decrease 3% / 6% / 10%, field disturbance 2× / 4× / 8×. The solar X-ray / UV channels are unverified and do not count towards the overall level."));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#9a9ea5;background:#1f2023;border:1px solid #35373c;padding:10px;font-size:9pt;"));
    root->addWidget(note);

    timer_ = new QTimer(this);
    timer_->setInterval(30000);
    connect(timer_, &QTimer::timeout, this, [this] { if (isVisible()) refresh(); });
    timer_->start();
    connect(window_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, [this](int) {
        dirty_ = true;
        refresh();
        emit windowSecondsChanged(window_->currentData().toDouble());
    });
}

void SpaceWeatherPage::setWindowSeconds(double seconds) {
    const int index = window_->findData(seconds);
    if (index >= 0 && index != window_->currentIndex()) window_->setCurrentIndex(index);
}

void SpaceWeatherPage::ingest(const char* data, int size) {
    using namespace nadirstation::ggak;
    auto& st = stores();
    for (int off = 0; off + kFrameBytes <= size; off += kFrameBytes) {
        const Frame f = parseFrame(reinterpret_cast<const std::uint8_t*>(data + off));
        if (!f.checksumOk) continue;
        for (const L4Record& r : splitL4Records(f)) {
            if (!std::isfinite(r.unixTime) || r.unixTime <= 0.0) continue;
            const long long ms = static_cast<long long>(std::llround(r.unixTime * 1000.0));
            double gcr = 0.0;
            int gcrParts = 0;
            for (const L4Field& fd : l4Fields()) {
                if (fd.id != r.id) continue;
                const double v = l4FieldValue(r, fd);
                if (!std::isfinite(v)) continue;
                st[std::to_string(fd.id) + " " + fd.name][ms] = v;
                if (fd.id == 64 && std::string(fd.name).rfind("count ", 0) == 0) { gcr += v; ++gcrParts; }
            }
            if (r.id == 64 && gcrParts == 3) st["64 sum"][ms] = gcr;
            // The five low-rate proton count channels together (each alone is 1-4 counts a second).
            if (r.id == 60 && r.bytes.size() > 45)
                st["60 low sum"][ms] = r.bytes[5] + r.bytes[17] + r.bytes[25] + r.bytes[39] + r.bytes[45];
        }
    }
    dirty_ = true;
}

void SpaceWeatherPage::addFrames(const QByteArray& frames) {
    ingest(frames.constData(), static_cast<int>(frames.size()));
    if (isVisible()) refresh();
}

void SpaceWeatherPage::loadHistory() {
    historyLoaded_ = true;
    // The last seven days of saved frames: <root>/ELEKTRO-L/GGAK/<date>/ggak.cadu.
    const QDir dir(QDir(dataRoot_).filePath(QStringLiteral("ELEKTRO-L/GGAK")));
    QStringList days = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);
    const QDate today = QDateTime::currentDateTimeUtc().date();
    for (const QString& d : days) {
        const QDate date = QDate::fromString(d, QStringLiteral("yyyy-MM-dd"));
        if (!date.isValid() || date > today || date.daysTo(today) > 7) continue;
        QFile f(dir.filePath(d + QStringLiteral("/ggak.cadu")));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QByteArray all = f.readAll();
        ingest(all.constData(), static_cast<int>(all.size() - all.size() % nadirstation::ggak::kFrameBytes));
    }
}

void SpaceWeatherPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    if (!historyLoaded_) loadHistory();
    refresh();
}

void SpaceWeatherPage::refresh() {
    auto& st = stores();
    double newest = 0.0;
    for (const auto& [k, s] : st) if (!s.empty()) newest = std::max(newest, s.rbegin()->first / 1000.0);
    const double seconds = window_->currentData().toDouble();
    const double tEnd = newest > 0.0 ? newest : static_cast<double>(QDateTime::currentSecsSinceEpoch());
    const double tStart = tEnd - seconds;
    range_->setText(QStringLiteral("Shown  %1 — %2 UTC  ·  %3")
                        .arg(utcText(tStart, "yyyy-MM-dd HH:mm"), utcText(tEnd, "yyyy-MM-dd HH:mm"), window_->currentText()));

    Level overall = Level::Unknown;
    double baseHours = 0.0;
    for (Card& c : cards_) {
        Level cardLevel = Level::Unknown;
        QStringList values, details;
        std::vector<SwSparkline::Line> lines;
        int k = 0;
        for (const Channel& ch : c.channels) {
            Series s;
            const auto it = st.find(ch.key);
            if (it != st.end())
                for (auto at = it->second.lower_bound(static_cast<long long>(tStart * 1000.0)); at != it->second.end(); ++at)
                    s.emplace_back(at->first / 1000.0, at->second);
            // A one-hour window contains six ten-minute bins; four prior bins and at least
            // half an hour of history are enough for a local baseline.
            const Index ix = evaluate(s, c.mode, 600.0, 0.5);
            cardLevel = worse(cardLevel, ix.level);
            baseHours = std::max(baseHours, ix.historyHours);
            const QString name = QString::fromUtf8(ch.label);
            const QString unit = QString::fromUtf8(ch.unit);
            const bool known = ix.level != Level::Unknown;
            const QString ratio = known ? QStringLiteral("（%1×）").arg(ix.score, 0, 'f', 2) : QStringLiteral(" (building baseline)");
            if (ix.bins.empty()) {
                values << QStringLiteral("%1 —").arg(name);
            } else if (ix.reason == "silent") {
                values << QStringLiteral("%1 no counts").arg(name);
                details << QStringLiteral("%1 all zero for the last 10 minutes: channel off or instrument mode changed; not rated").arg(name);
            } else if (unit.isEmpty()) {
                values << QStringLiteral("%1 %2%3").arg(name).arg(ix.current, 0, 'g', 4).arg(known ? ratio : QString());
                details << QStringLiteral("%1 raw %2 / baseline %3").arg(name).arg(ix.current, 0, 'g', 4)
                               .arg(ix.baseline != 0.0 ? QString::number(ix.baseline, 'g', 4) : QStringLiteral("—"));
            } else if (c.mode == Mode::Ratio) {
                // Count rate over the 10-minute bin, with its Poisson 1-sigma.
                const double rate = ix.current * ch.scale;
                const double sigma = ix.currentSamples > 0 ? std::sqrt(std::max(rate, 0.0) / ix.currentSamples) : 0.0;
                values << QStringLiteral("%1 %2 %3%4").arg(name).arg(rate, 0, 'f', rate < 10 ? 2 : 1).arg(unit, ratio);
                details << QStringLiteral("%1 last 10 min %2 ± %3 %4, usually %5")
                               .arg(name).arg(rate, 0, 'f', 2).arg(sigma, 0, 'f', 2).arg(unit)
                               .arg(ix.baseline != 0.0 ? QStringLiteral("%1 %2").arg(ix.baseline * ch.scale, 0, 'f', 2).arg(unit) : QStringLiteral("—"));
            } else if (c.mode == Mode::Decrease) {
                values << (known ? QStringLiteral("%1 %2%3 %").arg(name).arg(ix.score >= 0 ? QStringLiteral("+") : QString()).arg(ix.score, 0, 'f', 1)
                                 : QStringLiteral("%1 %2 %3 (building baseline)").arg(name).arg(ix.current * ch.scale, 0, 'f', 1).arg(unit));
                // Per channel: integral thresholds by the published GALS-E channels, assigned in
                // order of count rate (a higher threshold counts fewer protons).
                const char* thr[] = {"≥600 MeV", "≥800 MeV", "≥1200 MeV"};
                QStringList per;
                for (int n = 1; n <= 3; ++n) {
                    const auto cit = st.find("64 count " + std::to_string(n));
                    if (cit == st.end()) continue;
                    double sum = 0.0; int cnt = 0;
                    for (auto at = cit->second.lower_bound(static_cast<long long>((newest - 600.0) * 1000.0)); at != cit->second.end(); ++at) { sum += at->second; ++cnt; }
                    if (cnt > 0) per << QStringLiteral("%1 %2 %3").arg(QString::fromUtf8(thr[n - 1])).arg(0.5 * sum / cnt, 0, 'f', 1).arg(unit);
                }
                details << QStringLiteral("Protons %1 (last 10 min; channels in count-rate order)").arg(per.join(QStringLiteral("，")));
                if (ix.baseline != 0.0)
                    details << QStringLiteral("total %1 %3, usually %2 %3").arg(ix.current * ch.scale, 0, 'f', 1).arg(ix.baseline * ch.scale, 0, 'f', 1).arg(unit);
            } else {
                // Magnetometer: dB = 1-minute variability; DB = change of the 10-minute mean from the
                // baseline level (the absolute value includes the spacecraft field, so it is not shown).
                const Index lv = evaluate(s, Mode::Ratio, 600.0, 0.5);
                const double db = ix.current * ch.scale;
                values << QStringLiteral("%1 δB %2 %3%4").arg(name).arg(db, 0, 'f', 2).arg(unit, ratio);
                QString line = QStringLiteral("%1 1-min change %2 %3").arg(name).arg(db, 0, 'f', 2).arg(unit);
                if (ix.baseline != 0.0) line += QStringLiteral(" (usually %1)").arg(ix.baseline * ch.scale, 0, 'f', 2);
                if (lv.baseline != 0.0)
                    line += QStringLiteral("，ΔB %1%2 %3").arg(lv.current >= lv.baseline ? QStringLiteral("+") : QString())
                                .arg((lv.current - lv.baseline) * ch.scale, 0, 'f', 2).arg(unit);
                details << line;
            }
            SwSparkline::Line line;
            line.bins = ix.bins;
            // Without a trusted baseline the plot is drawn against the window's own median.
            double base = ix.baseline;
            if (base == 0.0 && !ix.bins.empty()) {
                std::vector<double> v;
                for (const Bin& b : ix.bins) v.push_back(b.value);
                std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
                base = v[v.size() / 2];
            }
            line.baseline = base;
            line.colour = kLineColours[k % 3];
            line.label = name;
            lines.push_back(line);
            ++k;
        }
        // Stale data says nothing about now.
        const bool stale = newest <= 0.0 || QDateTime::currentSecsSinceEpoch() - newest > 3.0 * 3600.0;
        const Level shown = stale || !c.verified ? Level::Unknown : cardLevel;
        const QColor col = levelColour(shown);
        c.state->setText(newest <= 0.0 ? QStringLiteral("No data")
                         : stale ? QStringLiteral("Stale")
                         : !c.verified ? QStringLiteral("Unverified")
                                 : QString::fromUtf8(levelName(cardLevel, c.mode)));
        if (!c.verified) details.prepend(QStringLiteral("Channel meaning unverified; not counted in the overall level"));
        c.state->setStyleSheet(QStringLiteral("QLabel{color:#111;background:%1;border-radius:2px;padding:2px 8px;font-weight:600;}").arg(col.name()));
        c.value->setText(values.join(QStringLiteral("  ·  ")));
        c.detail->setText(details.join(QStringLiteral("；")));
        c.plot->setData(lines, c.mode, tStart, tEnd);
        if (!stale && c.verified) overall = worse(overall, cardLevel);
    }

    if (newest <= 0.0) {
        banner_->setText(QStringLiteral("Space environment: no data"));
        banner_->setStyleSheet(QStringLiteral("color:#9a9ea5;font-size:22pt;font-weight:700;"));
        bannerDetail_->setText(QStringLiteral("No ELEKTRO-L N4 GGAK data yet. Receive ELEKTRO-L GGAK and this page updates by itself."));
        return;
    }
    const qint64 ageMin = std::max<qint64>(0, (QDateTime::currentSecsSinceEpoch() - static_cast<qint64>(newest)) / 60);
    const bool stale = ageMin > 180;
    const QString word = stale ? QStringLiteral("Stale")
                       : overall == Level::Unknown ? QStringLiteral("Building baseline")
                       : overall == Level::Quiet ? QStringLiteral("Quiet")
                       : overall == Level::Active ? QStringLiteral("Active")
                       : overall == Level::Elevated ? QStringLiteral("Disturbed") : QStringLiteral("Strongly disturbed");
    banner_->setText(QStringLiteral("Space environment: %1").arg(word));
    banner_->setStyleSheet(QStringLiteral("color:%1;font-size:22pt;font-weight:700;").arg(levelColour(stale ? Level::Unknown : overall).name()));
    const QString age = ageMin < 120 ? QStringLiteral("%1 min ago").arg(ageMin) : QStringLiteral("%1 h ago").arg(ageMin / 60);
    bannerDetail_->setText(QStringLiteral("Latest data %1 UTC (%2) · baseline from %3 h of station data%4")
        .arg(utcText(newest, "yyyy-MM-dd HH:mm"), age).arg(baseHours, 0, 'f', 1)
         .arg(baseHours < 0.5 ? QStringLiteral(", under 30 minutes: not rated yet") : QString()));
    dirty_ = false;
}

bool SpaceWeatherPage::savePng(const QString& path) {
    if (!report_ || path.isEmpty()) return false;
    const QString pngPath = path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
        ? path : path + QStringLiteral(".png");
    if (!historyLoaded_) loadHistory();
    refresh();
    const bool showControls = controls_->isVisible();
    controls_->hide();
    report_->layout()->activate();
    const QSize page = report_->size();
    if (page.width() <= 0 || page.height() <= 0) {
        qWarning() << "Space weather PNG: invalid report size" << page;
        if (showControls) controls_->show();
        return false;
    }
    constexpr int margin = 28;
    constexpr int header = 68;
    constexpr int footer = 40;
    QImage png((page.width() + 2 * margin) * 2, (page.height() + header + footer + 2 * margin) * 2,
               QImage::Format_ARGB32_Premultiplied);
    if (png.isNull()) {
        qWarning() << "Space weather PNG: image allocation failed" << png.size();
        if (showControls) controls_->show();
        return false;
    }
    png.fill(QColor(28, 29, 32));
    QPainter painter(&png);
    painter.scale(2.0, 2.0);
    painter.setPen(QColor(235, 237, 239));
    QFont titleFont = font();
    titleFont.setPointSizeF(16);
    titleFont.setBold(true);
    painter.setFont(titleFont);
    painter.drawText(QRect(margin, margin, page.width(), 27), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("ELEKTRO-L N4  /  GGAK  /  space weather"));
    titleFont.setPointSizeF(9);
    titleFont.setBold(false);
    painter.setFont(titleFont);
    painter.drawText(QRect(margin, margin + 31, page.width(), 22), Qt::AlignLeft | Qt::AlignVCenter,
                     range_->text());
    painter.fillRect(QRect(margin, margin + header, page.width(), page.height()), QColor(28, 29, 32));
    report_->render(&painter, QPoint(margin, margin + header), QRegion(), QWidget::DrawChildren);
    painter.setPen(QColor(160, 166, 172));
    painter.drawText(QRect(margin, margin + header + page.height() + 7, page.width(), 24),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Telemetry received at this station · relative indices · not a NOAA product     exported %1 UTC")
                         .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    painter.end();
    if (showControls) controls_->show();
    QImageWriter writer(pngPath, "PNG");
    if (writer.write(png)) return true;
    qWarning() << "Space weather PNG:" << writer.errorString() << pngPath << png.size();
    return false;
}
