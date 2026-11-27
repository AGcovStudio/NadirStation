#include "ggakpage.h"

#include <QAbstractItemView>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFrame>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QImageWriter>
#include <QLabel>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QGridLayout>
#include <QScrollArea>
#include <QSplitter>
#include <QTabWidget>
#include <QTableWidget>
#include <QTimeZone>
#include <QShowEvent>
#include <QStandardPaths>
#include <QTimer>
#include <QMouseEvent>
#include <QWheelEvent>
#include <cstdlib>
#include <map>
#include <string>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <set>

using namespace nadirstation::ggak;

namespace {
QString utc(double t) {
    if (t <= 0) return QStringLiteral("—");
    return QDateTime::fromSecsSinceEpoch(static_cast<qint64>(t), QTimeZone::UTC).toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"));
}
// Plots get everything kept (up to 6 h, or the trend window if longer) so they can be dragged
// back; the trend window only sets how much is on screen at once.
double keepSeconds(double window) { return std::max(window, 6.0 * 3600.0); }
QString fmt(double v) {
    if (std::isnan(v)) return QStringLiteral("—");
    if (v != 0 && (std::fabs(v) >= 1e6 || std::fabs(v) < 1e-2)) return QString::number(v, 'e', 3);
    return QString::number(v, 'f', 2);
}
QTableWidgetItem* item(const QString& s, bool right = false) {
    auto* it = new QTableWidgetItem(s);
    it->setFlags(it->flags() & ~Qt::ItemIsEditable);
    if (right) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
    return it;
}
QColor seriesColour(int i) {
    static const QColor c[] = {QColor(0x4a, 0x8f, 0xd0), QColor(0xe0, 0xa8, 0x3a), QColor(0x3f, 0xbf, 0x7f),
                               QColor(0xe8, 0x70, 0x5f), QColor(0xa0, 0x7f, 0xd0), QColor(0x5f, 0xc0, 0xc0),
                               QColor(0xd6, 0xd8, 0xdb), QColor(0xc0, 0xc0, 0x5f)};
    return c[i % 8];
}
QTableWidget* makeTable(const QStringList& headers) {
    auto* t = new QTableWidget(0, headers.size());
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->setVisible(false);
    t->setSelectionBehavior(QAbstractItemView::SelectRows);
    t->setSelectionMode(QAbstractItemView::ExtendedSelection);
    t->horizontalHeader()->setStretchLastSection(true);
    t->setAlternatingRowColors(true);
    return t;
}
QString exportStartPath(const QString& outputDir, const QString& stem) {
    QString folder = outputDir.isEmpty()
        ? QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("FY2_DATA/ELEKTRO-L/GGAK"))
        : outputDir;
    if (!QDir().mkpath(folder)) {
        folder = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
        QDir().mkpath(folder);
    }
    return QDir(folder).filePath(stem + QDateTime::currentDateTimeUtc().toString(QStringLiteral("_yyyyMMdd_HHmmss"))
                                 + QStringLiteral(".png"));
}
}  // namespace

// ---------------------------------------------------------------------------------------------

namespace {
std::vector<GgakTrendPlot*>& trendPlots() {
    static std::vector<GgakTrendPlot*> all;
    return all;
}
}  // namespace

GgakTrendPlot::GgakTrendPlot(QWidget* parent) : QWidget(parent) {
    setMinimumHeight(180);
    setCursor(Qt::OpenHandCursor);
    setToolTip(QStringLiteral("Drag: look back / forward; Ctrl + wheel: zoom the time axis; double-click: back to the latest"));
    trendPlots().push_back(this);
}

GgakTrendPlot::~GgakTrendPlot() {
    auto& all = trendPlots();
    all.erase(std::remove(all.begin(), all.end(), this), all.end());
}

QRectF GgakTrendPlot::plotRect() const { return QRectF(rect()).adjusted(64, 10, -12, -26); }

void GgakTrendPlot::dataRange(double& tFirst, double& tMax) const {
    tMax = 0.0;
    for (const auto& s : series_)
        if (!s.points.empty()) tMax = std::max(tMax, s.points.back().first);
    tFirst = tMax;
    for (const auto& s : series_)
        if (!s.points.empty()) tFirst = std::min(tFirst, s.points.front().first);
}

// Live: the window ends at the newest sample and, while less data than the window has arrived,
// spans what there is. Looking back or zoomed: the shared view.
double GgakTrendPlot::currentSpan(double tFirst, double tMax) const {
    if (viewSpan_ > 0.0) return viewSpan_;
    return std::clamp(tMax - tFirst, 10.0, window_);
}

double GgakTrendPlot::currentEnd(double tMax) const {
    return viewEnd_ > 0.0 ? std::min(viewEnd_, tMax) : tMax;
}

void GgakTrendPlot::shareView(double end, double span) {
    for (GgakTrendPlot* p : trendPlots()) {
        p->viewEnd_ = end;
        p->viewSpan_ = span;
        p->update();
    }
}

void GgakTrendPlot::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    double tFirst, tMax;
    dataRange(tFirst, tMax);
    if (tMax <= 0.0) return;
    dragging_ = true;
    pressX_ = e->position().x();
    pressSpan_ = currentSpan(tFirst, tMax);
    pressEnd_ = currentEnd(tMax);
    setCursor(Qt::ClosedHandCursor);
}

void GgakTrendPlot::mouseMoveEvent(QMouseEvent* e) {
    if (!dragging_) return QWidget::mouseMoveEvent(e);
    double tFirst, tMax;
    dataRange(tFirst, tMax);
    const double w = std::max(1.0, plotRect().width());
    // Dragging to the right moves the window back in time, as on a paper chart.
    double end = pressEnd_ - (e->position().x() - pressX_) / w * pressSpan_;
    end = std::max(end, tFirst + std::min(pressSpan_, tMax - tFirst));   // not past the oldest data
    shareView(end >= tMax - 0.5 ? 0.0 : end, pressSpan_);
}

void GgakTrendPlot::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        setCursor(Qt::OpenHandCursor);
        return;
    }
    QWidget::mouseReleaseEvent(e);
}

void GgakTrendPlot::mouseDoubleClickEvent(QMouseEvent*) {
    shareView(0.0, 0.0);
}

void GgakTrendPlot::wheelEvent(QWheelEvent* e) {
    // Plain wheel scrolls the page the plots sit in; Ctrl + wheel zooms the time axis.
    if (!(e->modifiers() & Qt::ControlModifier)) return e->ignore();
    double tFirst, tMax;
    dataRange(tFirst, tMax);
    if (tMax <= 0.0) return;
    const double steps = e->angleDelta().y() / 120.0;
    const double span = std::clamp(currentSpan(tFirst, tMax) * std::pow(1.25, -steps), 60.0, 7.0 * 86400.0);
    shareView(viewEnd_, span);
    e->accept();
}

void GgakTrendPlot::setSeries(const std::vector<Series>& series) {
    series_ = series;
    update();
}

void GgakTrendPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 21, 23));
    p.setRenderHint(QPainter::Antialiasing, true);
    const int legendRows = std::min(4, static_cast<int>(series_.size()));
    const QRectF r = rect().adjusted(64, 14 + legendRows * 15, -12, -26);
    p.setPen(QPen(QColor(53, 55, 60), 1));
    p.drawRect(r);
    double tFirst = 0.0, tMax = 0.0;
    dataRange(tFirst, tMax);
    QFont small(QStringLiteral("Bahnschrift"), 8);
    p.setFont(small);
    p.save();
    p.setClipRect(rect().adjusted(64, 4, -8, 0));
    QFontMetrics fm(small);
    for (int i = 0; i < legendRows; ++i) {
        const auto& s = series_[static_cast<std::size_t>(i)];
        const int y = 6 + i * 15;
        p.setPen(QPen(s.colour, 2));
        p.drawLine(66, y + 7, 80, y + 7);
        p.setPen(QColor(190, 195, 201));
        p.drawText(QRect(85, y, width() - 98, 14), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(s.name, Qt::ElideRight, std::max(0, width() - 102)));
    }
    p.restore();
    if (tMax <= 0) {
        p.setPen(QColor(110, 115, 122));
        p.drawText(r, Qt::AlignCenter, QStringLiteral("No data yet"));
        return;
    }
    const double span = currentSpan(tFirst, tMax);
    const double tEnd = currentEnd(tMax);
    const double tMin = tEnd - span;
    double vMin = 1e300, vMax = -1e300;
    for (const auto& s : series_)
        for (const auto& [t, v] : s.points) {
            if (t < tMin || t > tEnd || std::isnan(v) || (log_ && v <= 0)) continue;
            const double y = log_ ? std::log10(v) : v;
            vMin = std::min(vMin, y);
            vMax = std::max(vMax, y);
        }
    if (vMin > vMax) { vMin = 0; vMax = 1; }
    if (vMax - vMin < 1e-9) { vMin -= 0.5; vMax += 0.5; }
    const double pad = (vMax - vMin) * 0.05;
    vMin -= pad; vMax += pad;
    auto X = [&](double t) { return r.left() + (t - tMin) / span * r.width(); };
    auto Y = [&](double v) { return r.bottom() - ((log_ ? std::log10(v) : v) - vMin) / (vMax - vMin) * r.height(); };

    p.setPen(QColor(154, 158, 165));
    for (int i = 0; i <= 4; ++i) {
        const double yv = vMin + (vMax - vMin) * i / 4.0;
        const double y = r.bottom() - r.height() * i / 4.0;
        p.setPen(QPen(QColor(43, 45, 49), 1));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(QColor(154, 158, 165));
        const double shown = log_ ? std::pow(10.0, yv) : yv;
        p.drawText(QRectF(0, y - 8, 60, 16), Qt::AlignRight | Qt::AlignVCenter, fmt(shown));
    }
    for (int i = 0; i <= 4; ++i) {
        const double t = tMin + span * i / 4.0;
        const double x = r.left() + r.width() * i / 4.0;
        p.drawText(QRectF(x - 50, r.bottom() + 4, 100, 16), Qt::AlignHCenter,
                   QDateTime::fromSecsSinceEpoch(static_cast<qint64>(t), QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss")));
    }
    p.setClipRect(r);
    for (const auto& s : series_) {
        // At most one min/max pair per pixel column, however many points there are, so drawing
        // cost stays flat over a long session.
        QPainterPath path;
        bool started = false;
        double lastT = 0;
        int col = -1;
        double colMin = 0, colMax = 0;
        auto flush = [&](bool gap) {
            if (col < 0) return;
            const QPointF a(col, colMin), b(col, colMax);
            if (!started || gap) path.moveTo(a); else path.lineTo(a);
            if (colMax != colMin) path.lineTo(b);
            started = true;
        };
        bool pendingGap = false;
        for (const auto& [t, v] : s.points) {
            if (t < tMin || t > tEnd || std::isnan(v) || (log_ && v <= 0)) continue;
            const int x = static_cast<int>(X(t));
            const double y = Y(v);
            const bool gap = started && t - lastT > 120;   // gaps stay gaps
            if (x != col || gap) {
                flush(pendingGap);
                pendingGap = gap;
                col = x;
                colMin = colMax = y;
            } else {
                colMin = std::min(colMin, y);
                colMax = std::max(colMax, y);
            }
            lastT = t;
        }
        flush(pendingGap);
        p.setPen(QPen(s.colour, 1.4));
        p.drawPath(path);
    }
    if (viewEnd_ > 0.0 && tEnd < tMax - 0.5) {
        p.setPen(QColor(233, 189, 98));
        p.drawText(r.adjusted(0, 4, -6, 0), Qt::AlignRight | Qt::AlignTop,
                   QStringLiteral("Looking back · %1 before the latest · double-click to return")
                       .arg(tMax - tEnd >= 3600.0 ? QStringLiteral("%1 h").arg((tMax - tEnd) / 3600.0, 0, 'f', 1)
                                                  : QStringLiteral("%1 min").arg((tMax - tEnd) / 60.0, 0, 'f', 0)));
    }
}

// ---------------------------------------------------------------------------------------------

namespace {
// Dark-blue -> teal -> yellow colour scale for the spectrograms.
QColor heat(double x) {
    if (std::isnan(x)) return QColor(30, 31, 35);
    x = std::clamp(x, 0.0, 1.0);
    static const QColor stops[] = {QColor(28, 30, 80), QColor(40, 90, 160), QColor(40, 160, 150),
                                   QColor(140, 200, 80), QColor(250, 220, 60)};
    const double f = x * 4.0;
    const int i = std::min(3, static_cast<int>(f));
    const double u = f - i;
    const QColor& a = stops[i];
    const QColor& b = stops[i + 1];
    return QColor(static_cast<int>(a.red() + (b.red() - a.red()) * u), static_cast<int>(a.green() + (b.green() - a.green()) * u),
                  static_cast<int>(a.blue() + (b.blue() - a.blue()) * u));
}
}  // namespace

GgakSpectrumPlot::GgakSpectrumPlot(QWidget* parent) : QWidget(parent) { setMinimumHeight(200); }

void GgakSpectrumPlot::setData(const QString& title, const QStringList& labels, const std::vector<double>& latest,
                               const std::vector<double>& lo, const std::vector<double>& hi, bool logScale, const QColor& colour) {
    title_ = title;
    labels_ = labels;
    latest_ = latest;
    lo_ = lo;
    hi_ = hi;
    log_ = logScale;
    colour_ = colour;
    update();
}

void GgakSpectrumPlot::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 21, 23));
    p.setRenderHint(QPainter::Antialiasing, true);
    QFont small(QStringLiteral("Bahnschrift"), 8);
    p.setFont(small);
    p.setPen(QColor(214, 216, 219));
    p.drawText(QRectF(8, 2, width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter, title_);
    const QRectF r = rect().adjusted(64, 22, -12, -34);
    p.setPen(QPen(QColor(53, 55, 60), 1));
    p.drawRect(r);
    const int n = static_cast<int>(labels_.size());
    auto usable = [&](double v) { return !std::isnan(v) && (!log_ || v > 0); };
    double vMin = 1e300, vMax = -1e300;
    for (int i = 0; i < n; ++i)
        for (const auto* vec : {&latest_, &lo_, &hi_})
            if (i < static_cast<int>(vec->size()) && usable((*vec)[static_cast<std::size_t>(i)])) {
                const double y = log_ ? std::log10((*vec)[static_cast<std::size_t>(i)]) : (*vec)[static_cast<std::size_t>(i)];
                vMin = std::min(vMin, y);
                vMax = std::max(vMax, y);
            }
    if (n == 0 || vMin > vMax) {
        p.setPen(QColor(110, 115, 122));
        p.drawText(r, Qt::AlignCenter, QStringLiteral("No data yet"));
        return;
    }
    if (vMax - vMin < 1e-9) { vMin -= 0.5; vMax += 0.5; }
    const double pad = (vMax - vMin) * 0.08;
    vMin -= pad;
    vMax += pad;
    auto X = [&](int i) { return r.left() + (i + 0.5) * r.width() / n; };
    auto Y = [&](double v) { return r.bottom() - ((log_ ? std::log10(v) : v) - vMin) / (vMax - vMin) * r.height(); };
    for (int k = 0; k <= 4; ++k) {
        const double yv = vMin + (vMax - vMin) * k / 4.0;
        const double y = r.bottom() - r.height() * k / 4.0;
        p.setPen(QPen(QColor(43, 45, 49), 1));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        p.setPen(QColor(154, 158, 165));
        p.drawText(QRectF(0, y - 8, 60, 16), Qt::AlignRight | Qt::AlignVCenter, fmt(log_ ? std::pow(10.0, yv) : yv));
    }
    // Session range as a bar behind each channel.
    QColor band = colour_;
    band.setAlpha(60);
    const double barW = std::max(4.0, r.width() / n * 0.5);
    for (int i = 0; i < n; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        if (k < lo_.size() && k < hi_.size() && usable(lo_[k]) && usable(hi_[k]))
            p.fillRect(QRectF(X(i) - barW / 2, Y(hi_[k]), barW, std::max(1.0, Y(lo_[k]) - Y(hi_[k]))), band);
    }
    // Latest values joined by a line.
    QPainterPath path;
    bool started = false;
    for (int i = 0; i < n; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        if (k >= latest_.size() || !usable(latest_[k])) { started = false; continue; }
        const QPointF pt(X(i), Y(latest_[k]));
        if (!started) path.moveTo(pt); else path.lineTo(pt);
        started = true;
    }
    p.setPen(QPen(colour_, 1.6));
    p.drawPath(path);
    p.setBrush(colour_);
    for (int i = 0; i < n; ++i) {
        const std::size_t k = static_cast<std::size_t>(i);
        if (k < latest_.size() && usable(latest_[k])) p.drawEllipse(QPointF(X(i), Y(latest_[k])), 2.5, 2.5);
    }
    p.setBrush(Qt::NoBrush);
    p.setPen(QColor(154, 158, 165));
    for (int i = 0; i < n; ++i)
        p.drawText(QRectF(X(i) - 30, r.bottom() + 4, 60, 14), Qt::AlignHCenter, labels_.at(i));
    p.setPen(QColor(110, 115, 122));
    p.drawText(QRectF(r.left(), r.bottom() + 18, r.width(), 14), Qt::AlignLeft,
               QStringLiteral("line: latest · bar: session min..max"));
}

GgakSpectrogram::GgakSpectrogram(QWidget* parent) : QWidget(parent) { setMinimumHeight(200); }

void GgakSpectrogram::setData(const QString& title, const QStringList& rows,
                              const std::vector<const std::deque<std::pair<double, double>>*>& histories,
                              double windowSeconds, bool logScale) {
    title_ = title;
    rows_ = rows;
    log_ = logScale;
    tMax_ = 0.0;
    for (const auto* h : histories)
        if (h && !h->empty()) tMax_ = std::max(tMax_, h->back().first);
    double tFirst = tMax_;
    for (const auto* h : histories)
        if (h && !h->empty()) tFirst = std::min(tFirst, h->front().first);
    const double span = std::clamp(tMax_ - tFirst, 10.0, windowSeconds);
    tMin_ = tMax_ - span;
    const int cols = std::max(10, std::min(400, width() - 120));
    cells_.assign(histories.size(), std::vector<double>(static_cast<std::size_t>(cols), std::nan("")));
    vMin_ = 1e300;
    vMax_ = -1e300;
    for (std::size_t r = 0; r < histories.size(); ++r) {
        if (!histories[r]) continue;
        std::vector<double> sum(static_cast<std::size_t>(cols), 0.0);
        std::vector<int> count(static_cast<std::size_t>(cols), 0);
        for (const auto& [t, v] : *histories[r]) {
            if (t < tMin_ || std::isnan(v) || (log_ && v <= 0)) continue;
            const int c = std::clamp(static_cast<int>((t - tMin_) / span * cols), 0, cols - 1);
            sum[static_cast<std::size_t>(c)] += log_ ? std::log10(v) : v;
            ++count[static_cast<std::size_t>(c)];
        }
        for (int c = 0; c < cols; ++c)
            if (count[static_cast<std::size_t>(c)] > 0) {
                const double m = sum[static_cast<std::size_t>(c)] / count[static_cast<std::size_t>(c)];
                cells_[r][static_cast<std::size_t>(c)] = m;
                vMin_ = std::min(vMin_, m);
                vMax_ = std::max(vMax_, m);
            }
    }
    if (vMin_ > vMax_) { vMin_ = 0; vMax_ = 1; }
    if (vMax_ - vMin_ < 1e-9) vMax_ = vMin_ + 1.0;
    update();
}

void GgakSpectrogram::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 21, 23));
    QFont small(QStringLiteral("Bahnschrift"), 8);
    p.setFont(small);
    p.setPen(QColor(214, 216, 219));
    p.drawText(QRectF(8, 2, width() - 16, 16), Qt::AlignLeft | Qt::AlignVCenter, title_);
    const QRectF r = rect().adjusted(64, 22, -52, -22);
    const int nRows = static_cast<int>(cells_.size());
    if (nRows == 0 || tMax_ <= 0) {
        p.setPen(QColor(110, 115, 122));
        p.drawText(r, Qt::AlignCenter, QStringLiteral("No data yet"));
        return;
    }
    const int cols = static_cast<int>(cells_.front().size());
    const double cw = r.width() / cols, rh = r.height() / nRows;
    for (int row = 0; row < nRows; ++row) {
        // First channel at the bottom, like an energy axis.
        const double y = r.bottom() - (row + 1) * rh;
        for (int c = 0; c < cols; ++c) {
            const double v = cells_[static_cast<std::size_t>(row)][static_cast<std::size_t>(c)];
            p.fillRect(QRectF(r.left() + c * cw, y, cw + 0.5, rh + 0.5),
                       heat(std::isnan(v) ? std::nan("") : (v - vMin_) / (vMax_ - vMin_)));
        }
        p.setPen(QColor(154, 158, 165));
        if (row < rows_.size()) p.drawText(QRectF(0, y, 60, rh), Qt::AlignRight | Qt::AlignVCenter, rows_.at(row));
    }
    p.setPen(QPen(QColor(53, 55, 60), 1));
    p.drawRect(r);
    p.setPen(QColor(154, 158, 165));
    for (int i = 0; i <= 4; ++i) {
        const double t = tMin_ + (tMax_ - tMin_) * i / 4.0;
        const double x = r.left() + r.width() * i / 4.0;
        p.drawText(QRectF(x - 50, r.bottom() + 4, 100, 16), Qt::AlignHCenter,
                   QDateTime::fromSecsSinceEpoch(static_cast<qint64>(t), QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss")));
    }
    // Colour scale.
    const QRectF bar(r.right() + 8, r.top(), 10, r.height());
    for (int i = 0; i < static_cast<int>(bar.height()); ++i)
        p.fillRect(QRectF(bar.left(), bar.bottom() - i - 1, bar.width(), 1.5), heat(i / bar.height()));
    auto label = [&](double v) { return fmt(log_ ? std::pow(10.0, v) : v); };
    p.drawText(QRectF(bar.left() - 4, bar.top() - 14, 60, 12), Qt::AlignLeft, label(vMax_));
    p.drawText(QRectF(bar.left() - 4, bar.bottom() + 2, 60, 12), Qt::AlignLeft, label(vMin_));
}

// ---------------------------------------------------------------------------------------------

GgakPage::GgakPage(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(4, 4, 4, 4);
    root->setSpacing(6);

    auto* header = new QFrame;
    header->setObjectName(QStringLiteral("SystemCard"));
    auto* hl = new QHBoxLayout(header);
    hl->setContentsMargins(10, 6, 10, 6);
    hl->setSpacing(18);
    state_ = new QLabel(QStringLiteral("GGAK · not receiving"));
    state_->setStyleSheet(QStringLiteral("font-weight:600; color:#eceef0;"));
    counts_ = new QLabel(QStringLiteral("frames 0 · checksum errors 0"));
    carrier_ = new QLabel(QStringLiteral("carrier —"));
    lastFrame_ = new QLabel(QStringLiteral("last frame —"));
    folder_ = new QLabel(QString());
    folder_->setStyleSheet(QStringLiteral("color:#6e737a;"));
    folder_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    load_ = new QPushButton(QStringLiteral("Open .cadu…"));
    clear_ = new QPushButton(QStringLiteral("Clear"));
    hl->addWidget(state_);
    hl->addWidget(counts_);
    hl->addWidget(carrier_);
    hl->addWidget(lastFrame_);
    hl->addWidget(new QLabel(QStringLiteral("GGAK set")));
    levelSelect_ = new QComboBox;
    levelSelect_->addItem(QStringLiteral("L3 · IDs 32/48"), 3);
    levelSelect_->addItem(QStringLiteral("L4 · IDs 12/28/44/60/64/80/112"), 4);
    levelSelect_->setCurrentIndex(1);
    levelSelect_->setToolTip(QStringLiteral("Select the GGAK instrument set. The ID column shows each frame's decoded instrument mapping."));
    hl->addWidget(levelSelect_);
    unitLabel_ = new QLabel(QStringLiteral("unit: identifying"));
    unitLabel_->setStyleSheet(QStringLiteral("color:#9a9ea5;"));
    hl->addWidget(unitLabel_);
    hl->addWidget(folder_, 1);
    hl->addWidget(load_);
    hl->addWidget(clear_);
    root->addWidget(header);

    auto* split = new QSplitter(Qt::Horizontal);
    ids_ = makeTable({QStringLiteral("ID"), QStringLiteral("Instrument"), QStringLiteral("Frames"),
                      QStringLiteral("Checksum err"), QStringLiteral("Master ctr"), QStringLiteral("Ch ctr"),
                      QStringLiteral("Last frame (UTC)")});
    ids_->setSelectionMode(QAbstractItemView::SingleSelection);
    ids_->setMinimumWidth(290);
    split->addWidget(ids_);

    tabs_ = new QTabWidget;
    auto makeInstrumentTab = [&](QTableWidget*& table, GgakTrendPlot*& plot, const QStringList& headers) {
        auto* w = new QSplitter(Qt::Vertical);
        table = makeTable(headers);
        plot = new GgakTrendPlot;
        w->addWidget(table);
        w->addWidget(plot);
        w->setStretchFactor(0, 3);
        w->setStretchFactor(1, 2);
        return w;
    };
    tabs_->addTab(makeInstrumentTab(skl_, sklPlot_, {QStringLiteral("SKL-E channel"), QStringLiteral("Latest · reverse-engineered scale (unit unspecified)"),
                                                     QStringLiteral("Raw"), QStringLiteral("Session min"),
                                                     QStringLiteral("Session max"), QStringLiteral("Samples")}),
                  QStringLiteral("SKL-E · ID 48"));
    tabs_->addTab(makeInstrumentTab(skif_, skifPlot_, {QStringLiteral("SKIF-6 channel"), QStringLiteral("Latest · reverse-engineered scale (unit unspecified)"),
                                                       QStringLiteral("Raw"), QStringLiteral("Session min"),
                                                       QStringLiteral("Session max"), QStringLiteral("Scaling")}),
                  QStringLiteral("SKIF-6 · ID 32"));
    {
        auto* w = new QSplitter(Qt::Vertical);
        raw_ = new QPlainTextEdit;
        raw_->setReadOnly(true);
        raw_->setFont(QFont(QStringLiteral("Consolas"), 10));
        raw_->setPlaceholderText(QStringLiteral("Select a frame type on the left to see its latest frame."));
        log_ = new QPlainTextEdit;
        log_->setReadOnly(true);
        log_->setFont(QFont(QStringLiteral("Consolas"), 9));
        log_->setMaximumBlockCount(800);
        w->addWidget(raw_);
        w->addWidget(log_);
        tabs_->addTab(w, QStringLiteral("Raw frames"));
    }
    {
        // Live link: Eb/N0, carrier frequency and frame rate, one point a second.
        auto* w = new QSplitter(Qt::Vertical);
        snrPlot_ = new GgakTrendPlot;
        carrierPlot_ = new GgakTrendPlot;
        ratePlot_ = new GgakTrendPlot;
        for (auto* plot : {snrPlot_, carrierPlot_, ratePlot_}) {
            plot->setLogScale(false);
            w->addWidget(plot);
        }
        tabs_->addTab(w, QStringLiteral("Link (live)"));
    }
    {
        // Physical data: every decoded channel in its own chart, grouped by instrument.
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        physicalPage_ = new QWidget;
        auto* grid = new QGridLayout(physicalPage_);
        grid->setContentsMargins(4, 4, 4, 4);
        grid->setSpacing(6);
        int row = 0;
        auto* physicalBar = new QHBoxLayout;
        auto* physicalTitle = new QLabel(QStringLiteral("GGAK / PHYSICAL DATA / INDIVIDUAL CHANNELS"));
        physicalTitle->setStyleSheet(QStringLiteral("color:#b9bdc1;font-size:10pt;font-weight:600;letter-spacing:1px;"));
        physicalBar->addWidget(physicalTitle, 1);
        physicalExport_ = new QPushButton(QStringLiteral("Export PNG"));
        physicalExport_->setToolTip(QStringLiteral("Export every physical-data chart of the current instrument group"));
        physicalBar->addWidget(physicalExport_);
        grid->addLayout(physicalBar, row++, 0, 1, 3);
        connect(physicalExport_, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getSaveFileName(
                this, QStringLiteral("Export Physical Data PNG"),
                exportStartPath(outputDir_, QStringLiteral("GGAK_physical_L%1").arg(selectedLevel_)),
                QStringLiteral("PNG image (*.png)"));
            if (!path.isEmpty() && !saveTabPng(QStringLiteral("Physical data"), path))
                QMessageBox::warning(this, QStringLiteral("Export failed"), QStringLiteral("Cannot write the PNG file: %1").arg(path));
        });
        auto heading = [&](const QString& text, bool l4) {
            auto* l = new QLabel(text);
            l->setStyleSheet(QStringLiteral("font-weight:600; color:#d6d8db; padding-top:6px;"));
            grid->addWidget(l, row++, 0, 1, 3);
            (l4 ? l4PhysicalWidgets_ : l3PhysicalWidgets_).push_back(l);
        };
        auto small = [&](std::vector<GgakTrendPlot*>& list, int index) {
            auto* plot = new GgakTrendPlot;
            plot->setMinimumHeight(150);
            grid->addWidget(plot, row + index / 3, index % 3);
            list.push_back(plot);
            l3PhysicalWidgets_.push_back(plot);
        };
        summary_ = new QLabel(QStringLiteral("Waiting for instrument frames."));
        summary_->setTextFormat(Qt::RichText);
        summary_->setWordWrap(true);
        summary_->setStyleSheet(QStringLiteral("background:#1f2023; border:1px solid #313338; border-radius:4px; padding:6px; color:#d6d8db;"));
        grid->addWidget(summary_, row++, 0, 1, 3);
        // ELEKTRO-L N4 (the unit received here). Instrument names from the frame-ID assignment
        // inferred from the record structure; values are raw counts, no published calibration.
        {
            const char* groups[][2] = {
                {"FM-E? magnetometer (ID 80) · raw axis departure from median; no nT calibration", "3"},
                {"GALS-E? cosmic-ray detector (ID 64) · half-word count proxies; 60-s mean", "3"},
                {"SKIF-6? / SKL-E? (IDs 44/60) · selected raw fields; 60-s mean", "4"},
                {"DIR-E X-ray (ID 28) / VUSS-E? UV (ID 12) · uncalibrated raw telemetry", "2"},
                {"ISP-2M? solar irradiance / housekeeping (ID 112) · relative ADC counts", "4"},
            };
            for (const auto& g : groups) {
                heading(QString::fromUtf8(g[0]), true);
                const int n = std::atoi(g[1]);
                for (int i = 0; i < n; ++i) {
                    auto* plot = new GgakTrendPlot;
                    plot->setMinimumHeight(150);
                    grid->addWidget(plot, row + i / 3, i % 3);
                    n4Small_.push_back(plot);
                    l4PhysicalWidgets_.push_back(plot);
                }
                row += (n + 2) / 3;
            }
        }
        heading(QStringLiteral("Legacy SKL-E layout · frame ID 48; reverse-engineered scaling"), false);
        for (int c = 0; c < kSklChannels; ++c) small(sklSmall_, c);
        row += (kSklChannels + 2) / 3;
        heading(QStringLiteral("Legacy SKIF-6 layout · frame ID 32: electrons E1-E10, protons P1-P10"), false);
        for (int c = 0; c < SkifChannelCount; ++c) small(skifSmall_, c);
        row += (SkifChannelCount + 2) / 3;
        auto* note = new QLabel(QStringLiteral(
            "L4: ID 28 tracks the independent GOES X-ray time series; the other instrument and field assignments are inferred. "
            "no per-instrument telemetry/calibration handbook was found. Count and magnetic-field scales are provisional; "
            "UV/X-ray and ISP-2M remain raw/relative because absolute coefficients are unavailable. L3: IDs 48/32 are "
            "SKL-E/SKIF-6; values use reverse-engineered transfer functions. Several SKIF channels are marked "
            "uncertain, and the resulting physical units are not specified. Raw bytes are retained."));
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#6e737a;"));
        grid->addWidget(note, row, 0, 1, 3);
        l3PhysicalWidgets_.push_back(note);
        grid->setRowStretch(row + 1, 1);
        scroll->setWidget(physicalPage_);
        tabs_->insertTab(0, scroll, QStringLiteral("Physical data"));
    }
    {
        // Spectra: each instrument's spectrum now (with its session range) and over time.
        spectraPage_ = new QWidget;
        auto* g = new QGridLayout(spectraPage_);
        g->setContentsMargins(4, 4, 4, 4);
        g->setSpacing(6);
        sklSpectrum_ = new GgakSpectrumPlot;
        electronSpectrum_ = new GgakSpectrumPlot;
        protonSpectrum_ = new GgakSpectrumPlot;
        sklGram_ = new GgakSpectrogram;
        electronGram_ = new GgakSpectrogram;
        protonGram_ = new GgakSpectrogram;
        g->addWidget(sklSpectrum_, 0, 0);
        g->addWidget(electronSpectrum_, 0, 1);
        g->addWidget(protonSpectrum_, 0, 2);
        g->addWidget(sklGram_, 1, 0);
        g->addWidget(electronGram_, 1, 1);
        g->addWidget(protonGram_, 1, 2);
        tabs_->insertTab(1, spectraPage_, QStringLiteral("Spectra"));
    }
    {
        // ELEKTRO-L N4 instruments: the record layouts measured from received frames. Which
        // instrument each frame ID is remains an inference, and every title says so.
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        l4Page_ = new QWidget;
        auto* v = new QVBoxLayout(l4Page_);
        v->setContentsMargins(4, 4, 4, 4);
        v->setSpacing(6);
        auto* l4Bar = new QHBoxLayout;
        auto* l4Title = new QLabel(QStringLiteral("ELEKTRO-L N4 / GGAK / L4 INSTRUMENTS"));
        l4Title->setStyleSheet(QStringLiteral("color:#b9bdc1;font-size:10pt;font-weight:600;letter-spacing:1px;"));
        l4Bar->addWidget(l4Title, 1);
        l4Export_ = new QPushButton(QStringLiteral("Export PNG"));
        l4Export_->setToolTip(QStringLiteral("Export every N4 instrument chart of the current time window"));
        l4Bar->addWidget(l4Export_);
        v->addLayout(l4Bar);
        connect(l4Export_, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getSaveFileName(
                this, QStringLiteral("Export N4 Instruments PNG"),
                exportStartPath(outputDir_, QStringLiteral("N4_GGAK_instruments")),
                QStringLiteral("PNG image (*.png)"));
            if (!path.isEmpty() && !saveTabPng(QStringLiteral("L4 instruments"), path))
                QMessageBox::warning(this, QStringLiteral("Export failed"), QStringLiteral("Cannot write the PNG file: %1").arg(path));
        });
        l4Unit_ = new QLabel(QStringLiteral("Unit: identifying"));
        l4Unit_->setWordWrap(true);
        l4Unit_->setStyleSheet(QStringLiteral("background:#1f2023; border:1px solid #313338; border-radius:4px; padding:6px; color:#d6d8db;"));
        v->addWidget(l4Unit_);
        for (int i = 0; i < 5; ++i) {
            auto* plot = new GgakTrendPlot;
            plot->setMinimumHeight(200);
            plot->setLogScale(false);
            v->addWidget(plot);
            l4Plots_.push_back(plot);
        }
        auto* note = new QLabel(QStringLiteral(
            "Record lengths, counters and field positions were measured from ELEKTRO-L N4 frames received on "
            "2026-10-18 and 2026-10-31; no layout is published. ID 28 follows GOES X-rays; names with '?' are inferences. Values are raw counts. "
            "Count channels are shown as one-minute running means. Raw records are saved as l4_id<N>.csv."));
        note->setWordWrap(true);
        note->setStyleSheet(QStringLiteral("color:#6e737a;"));
        v->addWidget(note);
        v->addStretch(1);
        scroll->setWidget(l4Page_);
        tabs_->insertTab(2, scroll, QStringLiteral("L4 instruments"));
    }
    {
        // Compact live products. The underlying bins are updated on every accepted N4 record;
        // plots are repainted at most once per second.
        auto* scroll = new QScrollArea;
        scroll->setWidgetResizable(true);
        productsPage_ = new QWidget;
        auto* v = new QVBoxLayout(productsPage_);
        v->setContentsMargins(12, 10, 12, 14);
        v->setSpacing(9);
        auto* title = new QFrame;
        title->setStyleSheet(QStringLiteral("QFrame{background:#1f2023;border:0;border-bottom:1px solid #44474b;}"));
        auto* titleLayout = new QVBoxLayout(title);
        titleLayout->setContentsMargins(6, 8, 6, 11);
        auto* kicker = new QLabel(QStringLiteral("GGAK-E  /  TELEMETRY PRODUCTS"));
        kicker->setStyleSheet(QStringLiteral("color:#a7aaad;font-size:9pt;font-weight:700;letter-spacing:1px;border:none;"));
        auto* kickerRow = new QHBoxLayout;
        kickerRow->addWidget(kicker, 1);
        productsExport_ = new QPushButton(QStringLiteral("Export PNG"));
        productsExport_->setToolTip(QStringLiteral("Export every GGAK live product of the current time window"));
        kickerRow->addWidget(productsExport_);
        titleLayout->addLayout(kickerRow);
        connect(productsExport_, &QPushButton::clicked, this, [this] {
            const QString path = QFileDialog::getSaveFileName(
                this, QStringLiteral("Export Products PNG"),
                exportStartPath(outputDir_, QStringLiteral("N4_GGAK_products")),
                QStringLiteral("PNG image (*.png)"));
            if (!path.isEmpty() && !saveTabPng(QStringLiteral("Products"), path))
                QMessageBox::warning(this, QStringLiteral("Export failed"), QStringLiteral("Cannot write the PNG file: %1").arg(path));
        });
        auto* headline = new QLabel(QStringLiteral("Space environment · live products"));
        headline->setWordWrap(true);
        headline->setStyleSheet(QStringLiteral("color:#eceef0;font-size:15pt;font-weight:600;border:none;"));
        titleLayout->addWidget(headline);
        productMode_ = new QLabel(QStringLiteral("Awaiting N4 frames"));
        productMode_->setWordWrap(true);
        productMode_->setStyleSheet(QStringLiteral("color:#a9adb1;font-size:9pt;border:none;"));
        titleLayout->addWidget(productMode_);
        v->addWidget(title);

        auto* grid = new QGridLayout;
        grid->setHorizontalSpacing(7);
        grid->setVerticalSpacing(7);
        const char* titles[] = {"DIR-E · X-RAY RESPONSE", "GALS-E? · COSMIC RAYS",
                                "FM-E? · FIELD DEPARTURE", "RECEPTION · COVERAGE"};
        const char* descriptions[] = {"ID 28 / byte 2 / uncalibrated",
                                      "ID 64 / half-word count proxy",
                                      "ID 80 / vector change in raw counts",
                                      "Valid N4 frames in current minute"};
        for (int i = 0; i < 4; ++i) {
            auto* card = new QFrame;
            card->setStyleSheet(QStringLiteral("QFrame{background:#232427;border:1px solid #414448;border-radius:3px;}"));
            auto* inner = new QVBoxLayout(card);
            inner->setContentsMargins(11, 8, 11, 8);
            auto* t = new QLabel(QString::fromUtf8(titles[i]));
            t->setWordWrap(true);
            t->setStyleSheet(QStringLiteral("color:#b2b6ba;font-size:9pt;font-weight:600;border:none;"));
            inner->addWidget(t);
            productValues_[i] = new QLabel(QStringLiteral("—"));
            productValues_[i]->setStyleSheet(QStringLiteral("color:#f1f1f1;font-size:19pt;font-weight:600;border:none;"));
            inner->addWidget(productValues_[i]);
            productDetails_[i] = new QLabel(QString::fromUtf8(descriptions[i]));
            productDetails_[i]->setWordWrap(true);
            productDetails_[i]->setStyleSheet(QStringLiteral("color:#9da2a6;font-size:9pt;border:none;"));
            inner->addWidget(productDetails_[i]);
            grid->addWidget(card, i / 2, i % 2);
        }
        v->addLayout(grid);
        productEvent_ = new QLabel(QStringLiteral("X-ray response: baseline accumulating"));
        productEvent_->setWordWrap(true);
        productEvent_->setStyleSheet(QStringLiteral("background:#232427;border:1px solid #414448;border-left:3px solid #777c80;"
                                                     "padding:8px;color:#c9cccf;"));
        v->addWidget(productEvent_);
        productQuality_ = new QLabel(QStringLiteral("Waiting for instrument coverage"));
        productQuality_->setWordWrap(true);
        productQuality_->setStyleSheet(QStringLiteral("color:#a2a6aa;font-size:9pt;"));
        v->addWidget(productQuality_);
        const char* headings[] = {"Solar X-ray · raw response", "Particle channels · raw / estimated counts",
                                  "Magnetic field · raw vector departure", "Other instruments · raw telemetry"};
        for (int i = 0; i < 4; ++i) {
            auto* label = new QLabel(QString::fromUtf8(headings[i]));
            label->setStyleSheet(QStringLiteral("color:#d9dcdf;font-size:10pt;font-weight:600;padding-top:5px;"));
            v->addWidget(label);
            productPlots_[i] = new GgakTrendPlot;
            productPlots_[i]->setMinimumHeight(220);
            productPlots_[i]->setLogScale(false);
            v->addWidget(productPlots_[i]);
        }
        auto* caveat = new QLabel(QStringLiteral(
            "Instrument assignments are reverse-engineered. ID 28 tracks an independent X-ray series; other names are estimates. "
            "Values are one-minute means of raw telemetry. The GALS half-word count and FM vector change are proxies, "
            "not calibrated flux or nT. Open the L4 instruments tab for individual fields."));
        caveat->setWordWrap(true);
        caveat->setStyleSheet(QStringLiteral("color:#93989c;padding:7px 2px;"));
        v->addWidget(caveat);
        v->addStretch(1);
        scroll->setWidget(productsPage_);
        scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
        tabs_->insertTab(0, scroll, QStringLiteral("Products · live"));
    }
    tabs_->setCurrentIndex(0);
    connect(levelSelect_, &QComboBox::currentIndexChanged, this, [this] { updateLevelSelection(); });
    updateLevelSelection();
    split->addWidget(tabs_);
    split->setStretchFactor(0, 2);
    split->setStretchFactor(1, 5);
    split->setSizes({310, 1080});
    root->addWidget(split, 1);

    auto* foot = new QHBoxLayout;
    foot->addWidget(new QLabel(QStringLiteral("Trend window")));
    window_ = new QComboBox;
    window_->addItem(QStringLiteral("10 min"), 600.0);
    window_->addItem(QStringLiteral("1 h"), 3600.0);
    window_->addItem(QStringLiteral("6 h"), 21600.0);
    window_->addItem(QStringLiteral("24 h"), 86400.0);
    window_->setCurrentIndex(1);
    foot->addWidget(window_);
    logScale_ = new QCheckBox(QStringLiteral("Logarithmic"));
    logScale_->setChecked(true);
    foot->addWidget(logScale_);
    auto* note = new QLabel(QStringLiteral(
        "L3 transfer functions are reverse-engineered (see THIRD_PARTY_NOTICES.md); several SKIF channels are explicitly uncertain "
        "and SI units are not defined. L4 values use provisional field/count scaling where documented in code; "
        "instrument ID assignments are inferred. Raw telemetry is retained. Select table rows to plot them."));
    note->setStyleSheet(QStringLiteral("color:#6e737a;"));
    note->setWordWrap(true);
    foot->addWidget(note, 1);
    root->addLayout(foot);

    // Fixed rows.
    skl_->setRowCount(kSklChannels);
    for (int c = 0; c < kSklChannels; ++c) skl_->setItem(c, 0, item(QStringLiteral("%1 · %2").arg(c + 1).arg(QString::fromLatin1(sklChannelName(c)))));
    skif_->setRowCount(SkifChannelCount);
    for (int c = 0; c < SkifChannelCount; ++c) {
        skif_->setItem(c, 0, item(QString::fromLatin1(skifChannelName(c))));
        skif_->setItem(c, 5, item(skifScalingUncertain(c) ? QStringLiteral("uncertain (linear guess)") : QStringLiteral("reference")));
    }
    skl_->selectRow(0);
    skif_->selectRow(0);

    connect(ids_, &QTableWidget::itemSelectionChanged, this, [this] {
        const auto sel = ids_->selectedItems();
        if (sel.isEmpty()) return;
        selectedRawId_ = ids_->item(sel.first()->row(), 0)->text().toInt();
        showRawFor(selectedRawId_);
        tabs_->setCurrentWidget(raw_->parentWidget());
    });
    connect(skl_, &QTableWidget::itemSelectionChanged, this, [this] { refreshPlots(); });
    connect(skif_, &QTableWidget::itemSelectionChanged, this, [this] { refreshPlots(); });
    connect(window_, &QComboBox::currentIndexChanged, this, [this] {
        refreshPlots(); refreshLinkPlots(); refreshPhysicalPlots(); refreshSpectra(); refreshL4(); refreshProducts(true);
        emit trendWindowChanged(window_->currentData().toDouble());
    });
    connect(logScale_, &QCheckBox::toggled, this, [this] { refreshPlots(); refreshPhysicalPlots(); refreshSpectra(); });
    connect(tabs_, &QTabWidget::currentChanged, this, [this] {
        // Hidden charts are not redrawn, so bring the newly shown one up to date at once.
        QTimer::singleShot(0, this, [this] { refreshPhysicalPlots(); refreshSpectra(); refreshL4(); refreshLinkPlots(); refreshProducts(true); });
    });
    connect(load_, &QPushButton::clicked, this, [this] { loadCaduFile(); });
    connect(clear_, &QPushButton::clicked, this, [this] { clearSession(); });

    // Tables and plots are redrawn at most four times a second, however fast frames arrive.
    auto* timer = new QTimer(this);
    timer->setInterval(250);
    connect(timer, &QTimer::timeout, this, [this] {
        refreshLinkPlots();
        if (!dirty_) return;
        dirty_ = false;
        refreshTables();
        refreshPlots();
        refreshPhysicalPlots();
        refreshSpectra();
        refreshL4();
        refreshProducts();
        if (selectedRawId_ >= 0) showRawFor(selectedRawId_);
    });
    timer->start();
}

void GgakPage::clearSession() {
    idStats_.clear();
    sklRange_ = {};
    skifRange_ = {};
    for (auto& h : sklHistory_) h.clear();
    for (auto& h : skifHistory_) h.clear();
    log_->clear();
    raw_->clear();
    ids_->setRowCount(0);
    frameHistory_.clear();
    unit_.reset();
    lastMaster_ = -1;
    seenFrames_.clear();
    lostFrames_ = expectedFrames_ = 0;
    productMinutes_.clear();
    productLastRefreshMs_ = 0;
    snrHistory_.clear();
    carrierHistory_.clear();
    goodRateHistory_.clear();
    badRateHistory_.clear();
    linkLastT_ = 0.0;
    linkSamples_ = 0;
    snrSum_ = carrierSum_ = 0.0;
    dirty_ = true;
}

void GgakPage::addFrames(const QByteArray& frames) {
    const int n = frames.size() / kFrameBytes;
    for (int i = 0; i < n; ++i) {
        const auto* b = reinterpret_cast<const std::uint8_t*>(frames.constData()) + i * kFrameBytes;
        const Frame f = parseFrame(b);
        if (f.checksumOk && f.id != kIdFiller) {
            const std::uint64_t key = (static_cast<std::uint64_t>(f.id) << 48)
                                      | (static_cast<std::uint64_t>(f.masterCounter) << 32) | f.timeRaw;
            if (!seenFrames_.insert(key).second) continue;
            if (seenFrames_.size() > 500000) seenFrames_.clear();
        }
        IdStats& s = idStats_[f.id];
        ++s.count;
        unit_.note(f.id, f.checksumOk);
        if (f.checksumOk && f.id != kIdFiller) {
            // Instrument frames carry a master counter that steps by one; filler frames do not.
            const int m = f.masterCounter;
            if (lastMaster_ >= 0) {
                const int gap = (m - lastMaster_) & 0xFFFF;
                if (gap >= 1 && gap < 1000) {
                    expectedFrames_ += static_cast<qulonglong>(gap);
                    lostFrames_ += static_cast<qulonglong>(gap - 1);
                }
            }
            lastMaster_ = m;
        }
        if (!f.checksumOk) ++s.badChecksum;
        if (n - i <= 300)   // a whole file opened at once only logs its last frames
            log_->appendPlainText(QStringLiteral("%1  id %2  master %3  channel %4  %5")
            .arg(utc(f.unixTime)).arg(f.id, 3).arg(f.masterCounter, 5).arg(f.channelCounter, 5)
            .arg(f.checksumOk ? QStringLiteral("ok") : QStringLiteral("CHECKSUM ERROR")));
        if (!f.checksumOk) continue;
        s.lastMaster = f.masterCounter;
        s.lastChannel = f.channelCounter;
        s.lastTime = f.unixTime;
        s.lastFrame = QByteArray(reinterpret_cast<const char*>(b), kFrameBytes);
        if (f.id != kIdFiller) {
            auto& h = frameHistory_[f.id];
            h.emplace_back(f.unixTime, s.lastFrame);
            // Auto-clear: at most 12000 frames per ID and nothing older than 6 hours before the newest,
            // so a long session cannot grow without bound but the plots can be dragged back 6 hours
            // (ID 80 sends a frame every 2 s: 10800 in 6 h; the old 2000 kept about an hour).
            while (h.size() > 12000 || (!h.empty() && f.unixTime - h.front().first > 6 * 3600.0)) h.pop_front();
        }
        if (f.id != kIdFiller && l4Layout(f.id)) {
            const int ids[] = {12, 28, 44, 60, 64, 80, 112};
            const auto found = std::find(std::begin(ids), std::end(ids), f.id);
            if (found != std::end(ids))
                ++productMinutes_[static_cast<qint64>(f.unixTime) / 60].frames[found - std::begin(ids)];
            for (const L4Record& rec : splitL4Records(f)) {
                auto& bin = productMinutes_[static_cast<qint64>(rec.unixTime) / 60];
                auto put = [&](int slot, int offset, int kind, double scale = 1.0) {
                    const L4Field field{rec.id, "", offset, kind};
                    const double x = l4FieldValue(rec, field);
                    if (std::isfinite(x)) { bin.sum[slot] += x * scale; ++bin.samples[slot]; }
                };
                switch (rec.id) {
                    case 12: put(0, 3, 0); break;
                    case 28: put(1, 2, 0); break;
                    case 44: put(2, 5, 0); break;
                    case 60: put(3, 8, 1); break;
                    case 64: {
                        const auto& b = rec.bytes;
                        const double countProxy = 0.5 * ((b[3] << 8 | b[4]) + (b[5] << 8 | b[6]) + (b[7] << 8 | b[8]));
                        bin.sum[4] += countProxy; ++bin.samples[4];
                        break;
                    }
                    case 80: put(5, 4, 3); put(6, 7, 3); put(7, 10, 3); break;
                    case 112: put(8, 0, 1); break;
                    default: break;
                }
            }
            while (productMinutes_.size() > 400) productMinutes_.erase(productMinutes_.begin());
        }

        auto note = [](Range& r, double v, int raw) {
            if (std::isnan(v)) return;
            if (!r.seen) { r.min = r.max = v; r.seen = true; }
            r.min = std::min(r.min, v);
            r.max = std::max(r.max, v);
            r.last = v;
            r.lastRaw = raw;
        };
        auto push = [](std::deque<std::pair<double, double>>& h, double t, double v) {
            h.emplace_back(t, v);
            while (h.size() > 20000) h.pop_front();
        };
        if (f.id == kIdSkl) {
            for (const auto& rec : parseSkl(f))
                for (int c = 0; c < kSklChannels; ++c) {
                    const double v = rec.value[static_cast<std::size_t>(c)];
                    const int raw = c == 11 ? rec.raw[10] : (c < 10 ? rec.raw[static_cast<std::size_t>(c)] : -1);
                    note(sklRange_[static_cast<std::size_t>(c)], v, raw);
                    if (!std::isnan(v)) push(sklHistory_[static_cast<std::size_t>(c)], rec.unixTime, v);
                }
        } else if (f.id == kIdSkif) {
            for (const auto& rec : parseSkif(f)) {
                if (rec.channel >= SkifChannelCount) continue;
                note(skifRange_[static_cast<std::size_t>(rec.channel)], rec.value, rec.raw);
                push(skifHistory_[static_cast<std::size_t>(rec.channel)], rec.unixTime, rec.value);
            }
        }
    }
    if (n > 0) dirty_ = true;
}

void GgakPage::setStatus(int syncState, double carrierOffsetHz, double carrierToNoiseDb, bool carrierFound,
                         double snrDb, qulonglong goodFrames, qulonglong badFrames, const QString& outputDir) {
    static const char* names[] = {"NOSYNC", "SYNCING", "SYNCED"};
    const QString colour = syncState == 2 ? QStringLiteral("#6fd5a1") : (syncState == 1 ? QStringLiteral("#e9bd62") : QStringLiteral("#f08f7f"));
    state_->setText(QStringLiteral("GGAK · %1").arg(QString::fromLatin1(names[std::clamp(syncState, 0, 2)])));
    state_->setStyleSheet(QStringLiteral("font-weight:600; color:%1;").arg(colour));
    counts_->setText(QStringLiteral("frames %1 · checksum errors %2 · lost %3 (%4%) · Eb/N0 %5 dB")
        .arg(goodFrames).arg(badFrames).arg(lostFrames_)
        .arg(expectedFrames_ ? 100.0 * static_cast<double>(lostFrames_) / static_cast<double>(expectedFrames_) : 0.0, 0, 'f', 1)
        .arg(snrDb, 0, 'f', 1));
    counts_->setToolTip(QStringLiteral("lost = gaps in the master counter of instrument frames (gaps over 1000, e.g. "
                                       "between sessions, are not counted)"));
    updateUnitLabel();
    carrier_->setText(carrierFound
        ? QStringLiteral("carrier %1 kHz · signal %2 dB above noise").arg(carrierOffsetHz / 1000.0, 0, 'f', 3).arg(carrierToNoiseDb, 0, 'f', 1)
        : QStringLiteral("searching for carrier (strongest %1 dB above noise)").arg(carrierToNoiseDb, 0, 'f', 1));
    // One point a second for the live link plots: Eb/N0 and carrier averaged over the second, frame
    // rates from the counters.
    if (carrierFound) {
        snrSum_ += snrDb;
        carrierSum_ += carrierOffsetHz;
        ++linkSamples_;
    }
    const double nowT = static_cast<double>(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    if (linkLastT_ <= 0.0) {
        linkLastT_ = nowT;
        linkLastGood_ = goodFrames;
        linkLastBad_ = badFrames;
    } else if (nowT - linkLastT_ >= 1.0) {
        auto push = [](std::deque<std::pair<double, double>>& h, double t, double v) {
            h.emplace_back(t, v);
            while (h.size() > 21600) h.pop_front();   // 6 h at one point a second
        };
        const double dt = nowT - linkLastT_;
        if (goodFrames < linkLastGood_ || badFrames < linkLastBad_) { linkLastGood_ = goodFrames; linkLastBad_ = badFrames; }
        push(goodRateHistory_, nowT, 60.0 * static_cast<double>(goodFrames - linkLastGood_) / dt);
        push(badRateHistory_, nowT, 60.0 * static_cast<double>(badFrames - linkLastBad_) / dt);
        if (linkSamples_ > 0) {
            push(snrHistory_, nowT, snrSum_ / linkSamples_);
            push(carrierHistory_, nowT, carrierSum_ / linkSamples_);
        }
        linkLastT_ = nowT;
        linkLastGood_ = goodFrames;
        linkLastBad_ = badFrames;
        snrSum_ = carrierSum_ = 0.0;
        linkSamples_ = 0;
    }

    // Filler frames carry 0x33333333 in the time field (2027-03-21), so they are left out.
    double latest = 0;
    for (auto it = idStats_.cbegin(); it != idStats_.cend(); ++it)
        if (it.key() != kIdFiller) latest = std::max(latest, it->lastTime);
    lastFrame_->setText(QStringLiteral("last frame %1 UTC").arg(utc(latest)));
    if (!outputDir.isEmpty() && outputDir != outputDir_) {
        outputDir_ = outputDir;
        folder_->setText(QStringLiteral("saving to %1").arg(outputDir));
    }
}

void GgakPage::updateUnitLabel() {
    const int level = levelSelect_ ? levelSelect_->currentData().toInt() : 3;
    unitLabel_->setText(QStringLiteral("selected L%1 · detected %2").arg(level).arg(QString::fromStdString(unit_.label())));
    unitLabel_->setToolTip(QStringLiteral("Instrument set is selected manually. Detection is only a frame-ID fingerprint; %1")
                               .arg(QString::fromStdString(unit_.reason())));
}

void GgakPage::updateLevelSelection() {
    selectedLevel_ = levelSelect_ ? levelSelect_->currentData().toInt() : 3;
    const bool l3 = selectedLevel_ == 3;
    for (int i = 0; i < tabs_->count(); ++i) {
        const QString title = tabs_->tabText(i);
        const bool l3Only = title.startsWith(QStringLiteral("SKL")) || title.startsWith(QStringLiteral("SKIF"))
            || title == QStringLiteral("Spectra");
        const bool l4Only = title.startsWith(QStringLiteral("L4 instruments"))
            || title.startsWith(QStringLiteral("Products"));
        if (l3Only) tabs_->setTabVisible(i, l3);
        if (l4Only) tabs_->setTabVisible(i, !l3);
    }
    for (QWidget* w : l3PhysicalWidgets_) w->setVisible(l3);
    for (QWidget* w : l4PhysicalWidgets_) w->setVisible(!l3);
    updateUnitLabel();
    refreshPhysicalPlots();
    refreshSpectra();
    refreshL4();
}

void GgakPage::showEvent(QShowEvent* event) {
    QWidget::showEvent(event);
    QTimer::singleShot(0, this, [this] {
        updateUnitLabel();
        refreshTables();
        refreshPlots();
        refreshPhysicalPlots();
        refreshSpectra();
        refreshL4();
        refreshLinkPlots();
        refreshProducts(true);
    });
}

void GgakPage::refreshTables() {
    updateUnitLabel();
    ids_->setRowCount(idStats_.size());
    int row = 0;
    for (auto it = idStats_.cbegin(); it != idStats_.cend(); ++it, ++row) {
        const IdStats& s = it.value();
        ids_->setItem(row, 0, item(QString::number(it.key()), true));
        ids_->setItem(row, 1, item(QString::fromLatin1(frameIdName(it.key()))));
        ids_->setItem(row, 2, item(QString::number(s.count), true));
        ids_->setItem(row, 3, item(QString::number(s.badChecksum), true));
        ids_->setItem(row, 4, item(s.lastMaster < 0 ? QStringLiteral("—") : QString::number(s.lastMaster), true));
        ids_->setItem(row, 5, item(s.lastChannel < 0 ? QStringLiteral("—") : QString::number(s.lastChannel), true));
        ids_->setItem(row, 6, item(utc(s.lastTime)));
    }
    ids_->resizeColumnsToContents();
    auto fill = [](QTableWidget* t, int row, const Range& r, std::size_t samples, bool withSamples) {
        t->setItem(row, 1, item(r.seen ? fmt(r.last) : QStringLiteral("—"), true));
        t->setItem(row, 2, item(r.lastRaw < 0 ? QStringLiteral("—") : QString::number(r.lastRaw), true));
        t->setItem(row, 3, item(r.seen ? fmt(r.min) : QStringLiteral("—"), true));
        t->setItem(row, 4, item(r.seen ? fmt(r.max) : QStringLiteral("—"), true));
        if (withSamples) t->setItem(row, 5, item(QString::number(samples), true));
    };
    for (int c = 0; c < kSklChannels; ++c)
        fill(skl_, c, sklRange_[static_cast<std::size_t>(c)], sklHistory_[static_cast<std::size_t>(c)].size(), true);
    for (int c = 0; c < SkifChannelCount; ++c)
        fill(skif_, c, skifRange_[static_cast<std::size_t>(c)], 0, false);
}

void GgakPage::refreshPlots() {
    const double window = window_->currentData().toDouble();
    auto build = [&](QTableWidget* table, auto& history, auto nameOf, GgakTrendPlot* plot) {
        QList<int> rows;
        for (const auto* it : table->selectedItems())
            if (!rows.contains(it->row())) rows.append(it->row());
        std::sort(rows.begin(), rows.end());
        if (rows.size() > 8) rows = rows.mid(0, 8);
        std::vector<GgakTrendPlot::Series> series;
        for (int i = 0; i < rows.size(); ++i) {
            GgakTrendPlot::Series s;
            s.name = nameOf(rows[i]);
            s.colour = seriesColour(i);
            const auto& h = history[static_cast<std::size_t>(rows[i])];
            const double tEnd = h.empty() ? 0 : h.back().first;
            for (const auto& pt : h)
                if (pt.first >= tEnd - keepSeconds(window)) s.points.push_back(pt);
            series.push_back(std::move(s));
        }
        plot->setWindowSeconds(window);
        plot->setLogScale(logScale_->isChecked());
        plot->setSeries(series);
    };
    build(skl_, sklHistory_, [](int c) { return QStringLiteral("SKL %1 · %2").arg(c + 1).arg(QString::fromLatin1(sklChannelName(c))); }, sklPlot_);
    build(skif_, skifHistory_, [](int c) { return QString::fromLatin1(skifChannelName(c)); }, skifPlot_);
}

void GgakPage::showRawFor(int id) {
    const auto it = idStats_.constFind(id);
    if (it == idStats_.cend() || it->lastFrame.isEmpty()) {
        raw_->setPlainText(QStringLiteral("No valid frame with ID %1 yet.").arg(id));
        return;
    }
    const Frame f = parseFrame(reinterpret_cast<const std::uint8_t*>(it->lastFrame.constData()));
    QString text = QStringLiteral("ID %1 · %2\nmaster counter %3 · channel counter %4 · time %5 UTC (raw %6)\n"
                                  "frames of this ID %7 · checksum errors %8\n\n"
                                  "offset  00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n")
        .arg(f.id).arg(QString::fromLatin1(frameIdName(f.id))).arg(f.masterCounter).arg(f.channelCounter)
        .arg(utc(f.unixTime)).arg(f.timeRaw).arg(it->count).arg(it->badChecksum);
    for (int row = 0; row < kFrameBytes; row += 16) {
        QString line = QStringLiteral("%1   ").arg(row, 4, 16, QLatin1Char('0'));
        for (int i = row; i < std::min(row + 16, kFrameBytes); ++i)
            line += QStringLiteral("%1 ").arg(f.bytes[static_cast<std::size_t>(i)], 2, 16, QLatin1Char('0'));
        text += line + QLatin1Char('\n');
    }
    text += QStringLiteral("\nbytes 0-3 ASM · 4 ID · 5-6 master ctr · 7-8 channel ctr · 9-12 time · 13-221 payload · 222-223 checksum");
    raw_->setPlainText(text);
}

void GgakPage::loadCaduFile() {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Open GGAK frames"), outputDir_,
                                                      QStringLiteral("GGAK frames (*.cadu);;All files (*.*)"));
    if (path.isEmpty()) return;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return;
    const QByteArray all = f.readAll();
    const QByteArray frames = all.left(all.size() - all.size() % kFrameBytes);
    addFrames(frames);
    emit framesImported(frames);
    state_->setText(QStringLiteral("GGAK · file %1 (%2 frames)").arg(path).arg(all.size() / kFrameBytes));
}

// ---------------------------------------------------------------------------------------------
// Live line charts
// ---------------------------------------------------------------------------------------------

void GgakPage::refreshLinkPlots() {
    if (!snrPlot_ || !snrPlot_->isVisible()) return;
    const double window = window_->currentData().toDouble();
    auto tail = [window](const std::deque<std::pair<double, double>>& h) {
        std::vector<std::pair<double, double>> out;
        const double tEnd = h.empty() ? 0 : h.back().first;
        for (const auto& pt : h)
            if (pt.first >= tEnd - keepSeconds(window)) out.push_back(pt);
        return out;
    };
    for (auto* plot : {snrPlot_, carrierPlot_, ratePlot_}) plot->setWindowSeconds(window);
    snrPlot_->setSeries({{QStringLiteral("Eb/N0 (dB)"), seriesColour(2), tail(snrHistory_)}});
    carrierPlot_->setSeries({{QStringLiteral("Carrier offset (Hz)"), seriesColour(0), tail(carrierHistory_)}});
    ratePlot_->setSeries({{QStringLiteral("Good frames per minute"), seriesColour(2), tail(goodRateHistory_)},
                          {QStringLiteral("Checksum errors per minute"), seriesColour(3), tail(badRateHistory_)}});
}

void GgakPage::refreshProducts(bool force) {
    if (!productsPage_ || !productsPage_->isVisible()) return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!force && nowMs - productLastRefreshMs_ < 1000) return;
    productLastRefreshMs_ = nowMs;
    if (productMinutes_.isEmpty()) {
        productMode_->setText(QStringLiteral("Awaiting N4 instrument frames · values update as telemetry arrives"));
        productQuality_->setText(QStringLiteral("No instrument coverage yet"));
        productEvent_->setText(QStringLiteral("X-ray response: baseline accumulating"));
        for (QLabel* label : productValues_) label->setText(QStringLiteral("—"));
        for (auto* plot : productPlots_) plot->setSeries({});
        return;
    }
    const qint64 latestMinute = productMinutes_.lastKey();
    const double window = window_->currentData().toDouble();
    const qint64 firstShown = latestMinute - static_cast<qint64>(keepSeconds(window) / 60.0);
    const qint64 ageSec = nowMs / 1000 - latestMinute * 60;
    const QString mode = ageSec >= -120 && ageSec <= 120 ? QStringLiteral("LIVE") : QStringLiteral("RECORDING / REPLAY");
    productMode_->setText(QStringLiteral("%1  ·  latest %2 UTC  ·  one-minute bins, updating every second")
                              .arg(mode, utc(latestMinute * 60)));
    auto at = [&](const ProductMinute& bin, int slot) {
        return bin.samples[slot] > 0 ? bin.sum[slot] / bin.samples[slot] : std::nan("");
    };
    auto median = [](std::vector<double> values) {
        if (values.empty()) return std::nan("");
        std::nth_element(values.begin(), values.begin() + static_cast<std::ptrdiff_t>(values.size() / 2), values.end());
        return values[values.size() / 2];
    };
    std::array<std::vector<double>, 9> history;
    for (auto it = productMinutes_.cbegin(); it != productMinutes_.cend(); ++it)
        for (int slot = 0; slot < 9; ++slot) {
            const double x = at(it.value(), slot);
            if (std::isfinite(x)) history[slot].push_back(x);
        }
    const double xrayBase = median(history[1]);
    const double xrayLatest = at(productMinutes_.last(), 1);
    if (std::isfinite(xrayLatest)) {
        productValues_[0]->setText(fmt(xrayLatest));
        productDetails_[0]->setText(std::isfinite(xrayBase)
            ? QStringLiteral("raw byte 2 · capture median %1 · %2 samples/min")
                  .arg(fmt(xrayBase)).arg(productMinutes_.last().samples[1])
            : QStringLiteral("raw byte 2 · uncalibrated"));
    } else {
        productValues_[0]->setText(QStringLiteral("—"));
        productDetails_[0]->setText(QStringLiteral("No ID 28 records in latest minute"));
    }
    const double galsLatest = at(productMinutes_.last(), 4);
    productValues_[1]->setText(std::isfinite(galsLatest) ? fmt(galsLatest) : QStringLiteral("—"));
    productDetails_[1]->setText(QStringLiteral("three even words / 2 · estimated count proxy"));

    std::array<double, 3> fieldBase{};
    for (int axis = 0; axis < 3; ++axis) {
        const auto& h = history[5 + axis];
        fieldBase[axis] = median(std::vector<double>(h.begin(), h.begin() + std::min<std::size_t>(30, h.size())));
    }
    auto fieldDelta = [&](const ProductMinute& bin) {
        double squared = 0.0;
        for (int axis = 0; axis < 3; ++axis) {
            const double x = at(bin, 5 + axis);
            if (!std::isfinite(x) || !std::isfinite(fieldBase[axis])) return std::nan("");
            squared += (x - fieldBase[axis]) * (x - fieldBase[axis]);
        }
        return std::sqrt(squared);
    };
    const double deltaLatest = fieldDelta(productMinutes_.last());
    productValues_[2]->setText(std::isfinite(deltaLatest) ? fmt(deltaLatest) : QStringLiteral("—"));
    productDetails_[2]->setText(QStringLiteral("Δvector from first 30 min · raw counts, not nT"));
    const ProductMinute& last = productMinutes_.last();
    int frames = 0, activeIds = 0;
    for (int n : last.frames) { frames += n; if (n > 0) ++activeIds; }
    productValues_[3]->setText(QString::number(frames));
    productDetails_[3]->setText(QStringLiteral("valid frames · %1/7 N4 streams in latest minute").arg(activeIds));
    productQuality_->setText(QStringLiteral("DATA QUALITY  ·  %1 valid instrument frames at %2 UTC  ·  %3 of 7 streams present  ·  raw bytes retained")
                                 .arg(frames)
                                 .arg(QDateTime::fromSecsSinceEpoch(latestMinute * 60, QTimeZone::UTC).toString(QStringLiteral("HH:mm")))
                                 .arg(activeIds));

    if (history[1].size() < 30 || !std::isfinite(xrayLatest)) {
        productEvent_->setText(QStringLiteral("X-ray response · accumulating a local baseline (30 minutes required for event candidates)"));
        productEvent_->setStyleSheet(QStringLiteral("background:#232427;border:1px solid #414448;border-left:3px solid #777c80;padding:8px;color:#c9cccf;"));
    } else {
        std::vector<double> deviations;
        deviations.reserve(history[1].size());
        for (double x : history[1]) deviations.push_back(std::fabs(x - xrayBase));
        const double mad = median(deviations);
        const double threshold = xrayBase + std::max(12.0, 4.0 * mad);
        const bool elevated = xrayLatest >= threshold;
        productEvent_->setText(elevated
            ? QStringLiteral("X-ray response elevated · raw %1 versus local threshold %2 · candidate only; no GOES flare class")
                  .arg(fmt(xrayLatest), fmt(threshold))
            : QStringLiteral("X-ray response near local baseline · raw %1 / baseline %2 · threshold %3")
                  .arg(fmt(xrayLatest), fmt(xrayBase), fmt(threshold)));
        productEvent_->setStyleSheet(elevated
            ? QStringLiteral("background:#302a22;border:1px solid #5a4b35;border-left:3px solid #c7a46c;padding:8px;color:#ead0a9;")
            : QStringLiteral("background:#232427;border:1px solid #414448;border-left:3px solid #777c80;padding:8px;color:#c9cccf;"));
    }

    auto points = [&](auto get) {
        std::vector<std::pair<double, double>> out;
        for (auto it = productMinutes_.cbegin(); it != productMinutes_.cend(); ++it) {
            if (it.key() < firstShown) continue;
            const double x = get(it.value());
            if (std::isfinite(x)) out.emplace_back(it.key() * 60.0, x);
        }
        return out;
    };
    for (auto* plot : productPlots_) { plot->setWindowSeconds(window); plot->setLogScale(false); }
    productPlots_[0]->setSeries({{QStringLiteral("DIR-E X-ray · raw byte 2, 1-min mean"), QColor(214, 171, 102),
                                  points([&](const ProductMinute& bin) { return at(bin, 1); })}});
    productPlots_[1]->setSeries({{QStringLiteral("GALS-E? · sum of three half-word count proxies"), QColor(151, 176, 142),
                                  points([&](const ProductMinute& bin) { return at(bin, 4); })}});
    productPlots_[2]->setSeries({{QStringLiteral("FM-E? · raw vector departure from first 30 min"), QColor(193, 195, 192),
                                  points(fieldDelta)}});
    std::vector<GgakTrendPlot::Series> other;
    const int productSlots[] = {0, 2, 3, 8};
    const char* names[] = {"VUSS-E? UV", "SKIF-6?", "SKL-E?", "ISP-2M?"};
    const QColor muted[] = {QColor(195, 159, 116), QColor(148, 166, 141),
                            QColor(175, 155, 162), QColor(174, 174, 170)};
    for (int i = 0; i < 4; ++i) {
        const double base = median(history[productSlots[i]]);
        if (!std::isfinite(base)) continue;
        other.push_back({QStringLiteral("%1 · Δ raw from median").arg(QString::fromLatin1(names[i])),
                         muted[i], points([&, slot = productSlots[i], base](const ProductMinute& bin) {
                             const double x = at(bin, slot);
                             return std::isfinite(x) ? x - base : x;
                         })});
    }
    productPlots_[3]->setSeries(other);
}

void GgakPage::refreshPhysicalPlots() {
    if (!physicalPage_ || !physicalPage_->isVisible()) return;
    const double window = window_->currentData().toDouble();
    const bool logScale = logScale_->isChecked();
    auto draw = [&](GgakTrendPlot* plot, const QString& name, const std::deque<std::pair<double, double>>& h, int colour) {
        GgakTrendPlot::Series s;
        s.name = name;
        s.colour = seriesColour(colour);
        const double tEnd = h.empty() ? 0 : h.back().first;
        for (const auto& pt : h)
            if (pt.first >= tEnd - keepSeconds(window)) s.points.push_back(pt);
        if (!s.points.empty()) s.name += QStringLiteral("  ·  %1").arg(fmt(s.points.back().second));
        plot->setWindowSeconds(window);
        plot->setLogScale(logScale);
        plot->setSeries({s});
    };
    for (int c = 0; c < kSklChannels && c < static_cast<int>(sklSmall_.size()); ++c)
        draw(sklSmall_[static_cast<std::size_t>(c)],
             QStringLiteral("SKL %1 · %2").arg(c + 1).arg(QString::fromLatin1(sklChannelName(c))),
             sklHistory_[static_cast<std::size_t>(c)], 0);
    for (int c = 0; c < SkifChannelCount && c < static_cast<int>(skifSmall_.size()); ++c)
        draw(skifSmall_[static_cast<std::size_t>(c)],
             QString::fromLatin1(skifChannelName(c)) + (skifScalingUncertain(c) ? QStringLiteral(" (uncertain scaling)") : QString()),
             skifHistory_[static_cast<std::size_t>(c)], c < 3 ? 1 : (c < SkifP1 ? 2 : 3));

    const QString n4Summary = refreshN4Physical(window);
    if (selectedLevel_ == 4) {
        summary_->setText(n4Summary.isEmpty() ? QStringLiteral("Waiting for L4 instrument frames.") : n4Summary);
        return;
    }

    // Key values: the integral channels people watch for radiation events.
    auto latest = [](const std::deque<std::pair<double, double>>& h) {
        return h.empty() ? std::make_pair(0.0, std::nan("")) : h.back();
    };
    QStringList parts;
    double newest = 0.0;
    auto add = [&](const QString& name, const std::deque<std::pair<double, double>>& h) {
        const auto [t, v] = latest(h);
        newest = std::max(newest, t);
        // Trend over the last ten minutes of samples: up, down or steady.
        QString arrow;
        if (!h.empty()) {
            double past = std::nan("");
            for (auto it = h.rbegin(); it != h.rend(); ++it)
                if (t - it->first >= 600.0) { past = it->second; break; }
            if (!std::isnan(past) && past > 0 && !std::isnan(v))
                arrow = v > past * 1.5 ? QStringLiteral(" &uarr;") : (v < past / 1.5 ? QStringLiteral(" &darr;") : QStringLiteral(" &rarr;"));
        }
        parts << QStringLiteral("<b>%1</b> %2%3").arg(name.toHtmlEscaped(), fmt(v), arrow);
    };
    add(QStringLiteral("SKL 1 e>0.15 / p>1.5 MeV"), sklHistory_[0]);
    add(QStringLiteral("SKL 2 e>3 / p>7 MeV"), sklHistory_[1]);
    add(QStringLiteral("SKL 9 p 110-300 MeV"), sklHistory_[8]);
    add(QStringLiteral("SKL 12 p>100 MeV"), sklHistory_[11]);
    add(QStringLiteral("SKIF E1"), skifHistory_[SkifE1]);
    add(QStringLiteral("SKIF P1"), skifHistory_[SkifP1]);
    summary_->setText(newest > 0
        ? QStringLiteral("Latest at %1 UTC &nbsp;·&nbsp; %2 &nbsp;<span style='color:#6e737a'>(arrow: vs 10 min earlier, x1.5)</span>")
              .arg(utc(newest), parts.join(QStringLiteral(" &nbsp;·&nbsp; ")))
        : (n4Summary.isEmpty() ? QStringLiteral("Waiting for instrument frames.") : n4Summary));
}

void GgakPage::refreshSpectra() {
    if (!spectraPage_ || !spectraPage_->isVisible()) return;
    const bool logScale = logScale_->isChecked();
    const double window = window_->currentData().toDouble();
    auto collect = [](const auto& ranges, int from, int to, std::vector<double>& last, std::vector<double>& lo, std::vector<double>& hi) {
        for (int c = from; c < to; ++c) {
            const auto& r = ranges[static_cast<std::size_t>(c)];
            last.push_back(r.seen ? r.last : std::nan(""));
            lo.push_back(r.seen ? r.min : std::nan(""));
            hi.push_back(r.seen ? r.max : std::nan(""));
        }
    };
    {
        std::vector<double> last, lo, hi;
        collect(sklRange_, 0, kSklChannels, last, lo, hi);
        QStringList labels;
        for (int c = 0; c < kSklChannels; ++c) labels << QString::number(c + 1);
        sklSpectrum_->setData(QStringLiteral("SKL channels 1-12"), labels, last, lo, hi, logScale, seriesColour(0));
        std::vector<const std::deque<std::pair<double, double>>*> h;
        for (int c = 0; c < kSklChannels; ++c) h.push_back(&sklHistory_[static_cast<std::size_t>(c)]);
        sklGram_->setData(QStringLiteral("SKL over time"), labels, h, window, logScale);
    }
    auto skif = [&](int from, const QString& prefix, GgakSpectrumPlot* plot, GgakSpectrogram* gram, const QString& title, int colour) {
        std::vector<double> last, lo, hi;
        collect(skifRange_, from, from + 10, last, lo, hi);
        QStringList labels;
        std::vector<const std::deque<std::pair<double, double>>*> h;
        for (int i = 0; i < 10; ++i) {
            labels << QStringLiteral("%1%2").arg(prefix).arg(i + 1);
            h.push_back(&skifHistory_[static_cast<std::size_t>(from + i)]);
        }
        plot->setData(title, labels, last, lo, hi, logScale, seriesColour(colour));
        gram->setData(title + QStringLiteral(" over time"), labels, h, window, logScale);
    };
    skif(SkifE1, QStringLiteral("E"), electronSpectrum_, electronGram_, QStringLiteral("SKIF electrons E1-E10"), 2);
    skif(SkifP1, QStringLiteral("P"), protonSpectrum_, protonGram_, QStringLiteral("SKIF protons P1-P10"), 3);
}

void GgakPage::addFrontTab(QWidget* page, const QString& title) {
    tabs_->insertTab(0, page, title);
    tabs_->setCurrentIndex(0);
}

void GgakPage::showTab(const QString& title) {
    for (int i = 0; i < tabs_->count(); ++i)
        if (tabs_->tabText(i).startsWith(title, Qt::CaseInsensitive)) { tabs_->setCurrentIndex(i); return; }
}

bool GgakPage::saveTabPng(const QString& tab, const QString& path) {
    if (path.isEmpty()) return false;
    const bool products = tab.startsWith(QStringLiteral("Products"), Qt::CaseInsensitive);
    const bool physical = tab.startsWith(QStringLiteral("Physical"), Qt::CaseInsensitive);
    const bool l4 = tab.startsWith(QStringLiteral("L4"), Qt::CaseInsensitive)
        || tab.startsWith(QStringLiteral("N4"), Qt::CaseInsensitive);
    if (!products && !physical && !l4) return false;
    if ((products || l4) && selectedLevel_ != 4) return false;
    QWidget* page = products ? productsPage_ : l4 ? l4Page_ : physicalPage_;
    QPushButton* exportButton = products ? productsExport_ : l4 ? l4Export_ : physicalExport_;
    if (!page || !exportButton) return false;
    showTab(products ? QStringLiteral("Products") : l4 ? QStringLiteral("L4 instruments") : QStringLiteral("Physical data"));
    QCoreApplication::processEvents();
    if (products) refreshProducts(true);
    else if (l4) refreshL4();
    else refreshPhysicalPlots();
    const bool restoreButton = exportButton->isVisible();
    exportButton->hide();
    page->layout()->activate();
    const QSize pageSize = page->size();
    if (pageSize.width() <= 0 || pageSize.height() <= 0) {
        qWarning() << "GGAK PNG: invalid page size" << tab << pageSize;
        if (restoreButton) exportButton->show();
        return false;
    }
    constexpr int margin = 28;
    constexpr int header = 66;
    constexpr int footer = 38;
    QImage png((pageSize.width() + 2 * margin) * 2, (pageSize.height() + header + footer + 2 * margin) * 2,
               QImage::Format_ARGB32_Premultiplied);
    if (png.isNull()) {
        qWarning() << "GGAK PNG: image allocation failed" << tab << pageSize;
        if (restoreButton) exportButton->show();
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
    painter.drawText(QRect(margin, margin, pageSize.width(), 27), Qt::AlignLeft | Qt::AlignVCenter,
                     products ? QStringLiteral("ELEKTRO-L N4  /  GGAK  /  PRODUCTS · LIVE")
                              : l4 ? QStringLiteral("ELEKTRO-L N4  /  GGAK  /  L4 INSTRUMENTS")
                                   : QStringLiteral("GGAK  /  PHYSICAL DATA  /  L%1").arg(selectedLevel_));
    titleFont.setPointSizeF(9);
    titleFont.setBold(false);
    painter.setFont(titleFont);
    painter.setPen(QColor(178, 184, 190));
    painter.drawText(QRect(margin, margin + 31, pageSize.width(), 22), Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Trend window  %1  ·  UTC  ·  telemetry received at this station") .arg(window_->currentText()));
    page->render(&painter, QPoint(margin, margin + header), QRegion(), QWidget::DrawChildren);
    painter.setPen(QColor(160, 166, 172));
    painter.drawText(QRect(margin, margin + header + pageSize.height() + 7, pageSize.width(), 24),
                     Qt::AlignLeft | Qt::AlignVCenter,
                     QStringLiteral("Raw / inferred telemetry products · not calibrated fluxes     exported %1 UTC")
                         .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))));
    painter.end();
    if (restoreButton) exportButton->show();
    const QString pngPath = path.endsWith(QStringLiteral(".png"), Qt::CaseInsensitive)
        ? path : path + QStringLiteral(".png");
    QImageWriter writer(pngPath, "PNG");
    if (writer.write(png)) return true;
    qWarning() << "GGAK PNG:" << writer.errorString() << pngPath << png.size();
    return false;
}

double GgakPage::trendWindowSeconds() const {
    return window_->currentData().toDouble();
}

void GgakPage::setTrendWindowSeconds(double seconds) {
    const int index = window_->findData(seconds);
    if (index >= 0 && index != window_->currentIndex()) window_->setCurrentIndex(index);
}

void GgakPage::loadFramesForReview(const QString& path, const QString& tab) {
    QFile f(path);
    if (f.open(QIODevice::ReadOnly)) {
        const QByteArray all = f.readAll();
        const QByteArray frames = all.left(all.size() - all.size() % kFrameBytes);
        addFrames(frames);
        emit framesImported(frames);
    }
    for (int i = 0; i < tabs_->count(); ++i)
        if (!tab.isEmpty() && tabs_->tabText(i).startsWith(tab, Qt::CaseInsensitive)) tabs_->setCurrentIndex(i);
    dirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// ELEKTRO-L N4 instruments
// ---------------------------------------------------------------------------------------------

namespace {
// Drops samples far from the series median (a few records carry other content in the same place),
// so one odd value cannot flatten the whole plot.
void dropOutliers(std::vector<std::pair<double, double>>& pts) {
    if (pts.size() < 10) return;
    std::vector<double> v;
    v.reserve(pts.size());
    for (const auto& p : pts) if (!std::isnan(p.second)) v.push_back(p.second);
    if (v.size() < 10) return;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    const double med = v[v.size() / 2];
    for (auto& x : v) x = std::fabs(x - med);
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    const double mad = std::max(1.0, v[v.size() / 2]);
    pts.erase(std::remove_if(pts.begin(), pts.end(), [&](const std::pair<double, double>& p) {
        return std::isnan(p.second) || std::fabs(p.second - med) > 25.0 * mad;
    }), pts.end());
}
void runningMean(std::vector<std::pair<double, double>>& pts, double seconds) {
    if (pts.empty() || seconds <= 0.0) return;
    std::vector<std::pair<double, double>> out;
    out.reserve(pts.size());
    std::deque<std::pair<double, double>> window;
    double sum = 0.0;
    for (const auto& pt : pts) {
        if (!std::isfinite(pt.second)) continue;
        window.push_back(pt);
        sum += pt.second;
        while (!window.empty() && pt.first - window.front().first >= seconds) {
            sum -= window.front().second;
            window.pop_front();
        }
        if (!window.empty()) out.emplace_back(pt.first, sum / window.size());
    }
    pts.swap(out);
}
double medianOf(const std::vector<std::pair<double, double>>& pts) {
    std::vector<double> v;
    for (const auto& p : pts) v.push_back(p.second);
    if (v.empty()) return 0.0;
    std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
    return v[v.size() / 2];
}
}  // namespace

void GgakPage::refreshL4() {
    if (!l4Page_ || !l4Page_->isVisible()) return;
    l4Unit_->setText(QStringLiteral("<b>Unit: %1</b> &nbsp;(confidence %2%)<br><span style='color:#9a9ea5'>%3</span>")
                         .arg(QString::fromStdString(unit_.label()).toHtmlEscaped())
                         .arg(unit_.confidence())
                         .arg(QString::fromStdString(unit_.reason()).toHtmlEscaped()));
    l4Unit_->setTextFormat(Qt::RichText);
    const double window = window_->currentData().toDouble();

    // Records of one ID within the window, newest frame last.
    auto recordsOf = [&](int id) {
        std::vector<L4Record> out;
        const auto it = frameHistory_.constFind(id);
        if (it == frameHistory_.cend() || it->empty()) return out;
        const double tEnd = it->back().first;
        for (const auto& [t, bytes] : *it) {
            if (t < tEnd - keepSeconds(window)) continue;
            const Frame f = parseFrame(reinterpret_cast<const std::uint8_t*>(bytes.constData()));
            for (auto& r : splitL4Records(f)) out.push_back(std::move(r));
        }
        return out;
    };
    auto fieldSeries = [&](const std::vector<L4Record>& recs, int id, const char* name) {
        std::vector<std::pair<double, double>> pts;
        for (const auto& f : l4Fields()) {
            if (f.id != id || std::string(f.name) != name) continue;
            for (const auto& r : recs) pts.emplace_back(r.unixTime, l4FieldValue(r, f));
        }
        std::sort(pts.begin(), pts.end());
        dropOutliers(pts);
        return pts;
    };
    auto layoutName = [](int id) { const L4Layout* l = l4Layout(id); return QString::fromLatin1(l ? l->guess : "?"); };
    auto set = [&](std::size_t plot, std::vector<GgakTrendPlot::Series> series) {
        if (plot >= l4Plots_.size()) return;
        l4Plots_[plot]->setWindowSeconds(window);
        l4Plots_[plot]->setLogScale(false);
        l4Plots_[plot]->setSeries(series);
    };

    {   // ID 80: the three 24-bit quantities, each about its median so they share one axis.
        const auto recs = recordsOf(80);
        std::vector<GgakTrendPlot::Series> series;
        int k = 0;
        for (const char* n : {"B1 (24-bit)", "B2 (24-bit)", "B3 (24-bit)"}) {
            auto pts = fieldSeries(recs, 80, n);
            const double med = medianOf(pts);
            for (auto& p : pts) p.second -= med;
            series.push_back({QStringLiteral("ID 80 %1 - median %2 · %3").arg(QString::fromLatin1(n)).arg(med, 0, 'f', 0)
                                  .arg(k == 0 ? layoutName(80) : QString()), seriesColour(k), pts});
            ++k;
        }
        set(0, series);
    }
    {   // ID 64: three count channels, one-minute running mean.
        const auto recs = recordsOf(64);
        std::vector<GgakTrendPlot::Series> series;
        int k = 0;
        for (const char* n : {"count 1", "count 2", "count 3"}) {
            auto pts = fieldSeries(recs, 64, n);
            runningMean(pts, 60.0);
            series.push_back({QStringLiteral("ID 64 %1 (1-min mean)%2").arg(QString::fromLatin1(n))
                                  .arg(k == 0 ? QStringLiteral(" · ") + layoutName(64) : QString()), seriesColour(k), pts});
            ++k;
        }
        set(1, series);
    }
    {   // IDs 44 / 60: particle counts, one-minute running mean.
        const auto r44 = recordsOf(44);
        const auto r60 = recordsOf(60);
        std::vector<GgakTrendPlot::Series> series;
        auto add = [&](const std::vector<L4Record>& recs, int id, const char* n, int colour) {
            auto pts = fieldSeries(recs, id, n);
            runningMean(pts, 60.0);
            series.push_back({QStringLiteral("ID %1 %2 (1-min mean)").arg(id).arg(QString::fromLatin1(n)), seriesColour(colour), pts});
        };
        add(r44, 44, "byte 11", 0);
        add(r44, 44, "byte 5", 1);
        add(r60, 60, "count 41", 2);
        add(r60, 60, "count 25", 3);
        if (!series.empty()) series.front().name += QStringLiteral(" · ") + layoutName(44);
        set(2, series);
    }
    {   // IDs 12 / 28: slowly varying bytes.
        std::vector<GgakTrendPlot::Series> series;
        auto r28 = recordsOf(28);
        auto r12 = recordsOf(12);
        series.push_back({QStringLiteral("ID 28 byte 2 · ") + layoutName(28), seriesColour(0), fieldSeries(r28, 28, "byte 2")});
        series.push_back({QStringLiteral("ID 12 byte 3"), seriesColour(1), fieldSeries(r12, 12, "byte 3")});
        set(3, series);
    }
    {   // ID 112: stable 12-bit words about their medians.
        const auto recs = recordsOf(112);
        std::vector<GgakTrendPlot::Series> series;
        int k = 0;
        for (const char* n : {"word 0", "word 2", "word 4", "word 6"}) {
            auto pts = fieldSeries(recs, 112, n);
            const double med = medianOf(pts);
            for (auto& p : pts) p.second -= med;
            series.push_back({QStringLiteral("ID 112 %1 - median %2%3").arg(QString::fromLatin1(n)).arg(med, 0, 'f', 0)
                                  .arg(k == 0 ? QStringLiteral(" · ") + layoutName(112) : QString()), seriesColour(k), pts});
            ++k;
        }
        set(4, series);
    }
}

// ---------------------------------------------------------------------------------------------
// Physical data: ELEKTRO-L N4 instruments
// ---------------------------------------------------------------------------------------------

QString GgakPage::refreshN4Physical(double window) {
    if (n4Small_.empty()) return {};
    auto recordsOf = [&](int id) {
        std::vector<L4Record> out;
        const auto it = frameHistory_.constFind(id);
        if (it == frameHistory_.cend() || it->empty()) return out;
        const double tEnd = it->back().first;
        for (const auto& [t, bytes] : *it) {
            if (t < tEnd - keepSeconds(window)) continue;
            const Frame f = parseFrame(reinterpret_cast<const std::uint8_t*>(bytes.constData()));
            for (auto& r : splitL4Records(f)) out.push_back(std::move(r));
        }
        return out;
    };
    std::map<int, std::vector<L4Record>> recs;
    for (int id : {12, 28, 44, 60, 64, 80, 112}) recs[id] = recordsOf(id);
    auto series = [&](int id, const char* field) {
        std::vector<std::pair<double, double>> pts;
        for (const auto& f : l4Fields()) {
            if (f.id != id || std::string(f.name) != field) continue;
            for (const auto& r : recs[id]) pts.emplace_back(r.unixTime, l4FieldValue(r, f));
        }
        std::sort(pts.begin(), pts.end());
        dropOutliers(pts);
        return pts;
    };
    // mode 0 raw, 1 change from the median, 2 1-min mean. Magnetometer and counter scales below are
    // provisional engineering conversions; no matching L4 calibration handbook/coefficient table was found.
    struct Ch { int id; const char* field; const char* label; int mode; int colour; double scale = 1.0; };
    const Ch chans[] = {
        {80, "B1 (24-bit)", "FM axis 1 Δraw", 1, 0}, {80, "B2 (24-bit)", "FM axis 2 Δraw", 1, 1}, {80, "B3 (24-bit)", "FM axis 3 Δraw", 1, 2},
        {64, "count 1", "GALS channel 1 half-word proxy", 2, 0, 0.5}, {64, "count 2", "GALS channel 2 half-word proxy", 2, 1, 0.5},
        {64, "count 3", "GALS channel 3 half-word proxy", 2, 2, 0.5},
        {44, "byte 11", "ID 44 byte 11 raw", 2, 0}, {44, "byte 5", "ID 44 byte 5 raw", 2, 1},
        {60, "count 41", "ID 60 byte 41 raw", 2, 2}, {60, "count 25", "ID 60 byte 25 raw", 2, 3},
        {28, "byte 2", "DIR-E X-ray response (raw)", 0, 0}, {12, "byte 3", "VUSS-E? UV telemetry (raw)", 0, 1},
        {112, "word 0", "ISP-2M word 0", 1, 0}, {112, "word 2", "ISP-2M word 2", 1, 1}, {112, "word 4", "ISP-2M word 4", 1, 2}, {112, "word 6", "ISP-2M word 6", 1, 3},
    };
    QStringList parts;
    double newest = 0.0;
    std::size_t k = 0;
    for (const Ch& ch : chans) {
        if (k >= n4Small_.size()) break;
        auto pts = series(ch.id, ch.field);
        QString name = QString::fromLatin1(ch.label);
        if (ch.mode == 1 && !pts.empty()) {
            const double med = medianOf(pts);
            for (auto& p : pts) p.second = (p.second - med) * ch.scale;
            // Raw magnetometer axes include the spacecraft field, so show departures from a local median.
            name += ch.scale != 1.0 ? QStringLiteral(" from median") : QStringLiteral(" (median %1)").arg(med, 0, 'f', 0);
        } else if (ch.mode == 2) {
            runningMean(pts, 60.0);
            if (ch.scale != 1.0) for (auto& p : pts) p.second *= ch.scale;
        }
        if (!pts.empty()) {
            name += QStringLiteral("  ·  %1").arg(fmt(pts.back().second));
            newest = std::max(newest, pts.back().first);
        }
        GgakTrendPlot::Series s{name, seriesColour(ch.colour), pts};
        n4Small_[k]->setWindowSeconds(window);
        n4Small_[k]->setLogScale(false);
        n4Small_[k]->setSeries({s});
        if ((ch.id == 64 && std::string(ch.field) == "count 1") || (ch.id == 44 && std::string(ch.field) == "byte 5")
            || (ch.id == 60 && std::string(ch.field) == "count 41"))
            if (!pts.empty()) parts << QStringLiteral("<b>%1</b> %2").arg(QString::fromLatin1(ch.label).toHtmlEscaped(), fmt(pts.back().second));
        ++k;
    }
    if (newest <= 0.0) return {};
    return QStringLiteral("ELEKTRO-L N4 · latest at %1 UTC &nbsp;·&nbsp; %2 &nbsp;<span style='color:#6e737a'>(raw / estimated proxies, 60-s mean)</span>")
        .arg(utc(newest), parts.join(QStringLiteral(" &nbsp;·&nbsp; ")));
}
