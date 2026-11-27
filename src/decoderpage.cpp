#include "decoderpage.h"

#include "uikit.h"
#include "uitheme.h"
#include <QDateTime>
#include <QEvent>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QHelpEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSplitter>
#include <QToolTip>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

using namespace nadirstation::diag;
namespace tone = nadirstation::ui::tone;

namespace {

constexpr int kSparkLen = 300;      // snapshots kept per stage (~75 s at 4 Hz)
constexpr int kTimelineSec = 300;   // seconds in the health timeline

QColor healthColour(int h) {
    switch (h) {
    case Ok: return tone::ok();
    case Warn: return tone::warn();
    case Error: return tone::error();
    default: return tone::muted();
    }
}
QString healthWord(int h) {
    switch (h) {
    case Ok: return QStringLiteral("Healthy");
    case Warn: return QStringLiteral("Marginal");
    case Error: return QStringLiteral("Fault");
    default: return QStringLiteral("Idle");
    }
}
QFont uiFont(double px, int weight = QFont::Normal) {
    QFont f(QStringLiteral("Bahnschrift"));
    f.setFamilies({QStringLiteral("Bahnschrift"), QStringLiteral("Segoe UI"), QStringLiteral("Inter"), QStringLiteral("DejaVu Sans")});
    f.setPixelSize(int(std::lround(px)));
    f.setWeight(QFont::Weight(weight));
    return f;
}
QFont monoFont(double px, int weight = QFont::Normal) {
    QFont f(QStringLiteral("Cascadia Mono"));
    f.setFamilies({QStringLiteral("Cascadia Mono"), QStringLiteral("JetBrains Mono"), QStringLiteral("Consolas"), QStringLiteral("DejaVu Sans Mono")});
    f.setStyleHint(QFont::Monospace);
    f.setPixelSize(int(std::lround(px)));
    f.setWeight(QFont::Weight(weight));
    return f;
}
QColor alpha(QColor c, int a) { c.setAlpha(a); return c; }
QString grouped(double v) {
    if (!std::isfinite(v)) return QStringLiteral("—");
    const bool neg = v < 0;
    QString d = QString::number(qint64(std::llround(std::fabs(v))));
    for (int i = d.size() - 3; i > 0; i -= 3) d.insert(i, QChar(0x2009));
    return neg ? QStringLiteral("−") + d : d;
}
// Counts for the funnel: grouped digits, millions once they stop being readable.
QString compact(double v) {
    if (std::fabs(v) >= 1e7) return QStringLiteral("%1 M").arg(v / 1e6, 0, 'f', v >= 1e9 ? 0 : 1);
    return grouped(v);
}

// A sparkline or trend path through the finite values of `v`, scaled into `r`.
void drawTrend(QPainter& p, const QRectF& r, const std::deque<double>& v, const QColor& c, bool fill,
               double* outLo = nullptr, double* outHi = nullptr, bool percent = false) {
    double lo = 0, hi = 0;
    bool any = false;
    for (double x : v)
        if (std::isfinite(x)) { lo = any ? std::min(lo, x) : x; hi = any ? std::max(hi, x) : x; any = true; }
    if (!any) return;
    if (hi - lo < 1e-9) { lo -= 1.0; hi += 1.0; }
    const double pad = (hi - lo) * 0.08;
    lo -= pad; hi += pad;
    if (percent) { lo = std::max(lo, 0.0); hi = std::min(std::max(hi, lo + 5.0), 100.0); }
    if (outLo) *outLo = lo;
    if (outHi) *outHi = hi;
    const int n = int(v.size());
    const double dx = r.width() / double(std::max(1, kSparkLen - 1));
    const double x0 = r.right() - dx * (n - 1);
    QPainterPath path;
    bool pen = false;
    double firstX = 0, lastX = 0;
    for (int i = 0; i < n; ++i) {
        const double x = x0 + dx * i;
        if (!std::isfinite(v[size_t(i)])) { pen = false; continue; }
        const double y = r.bottom() - (v[size_t(i)] - lo) / (hi - lo) * r.height();
        if (!pen) { path.moveTo(x, y); if (path.elementCount() == 1) firstX = x; pen = true; }
        else path.lineTo(x, y);
        lastX = x;
    }
    if (fill && path.elementCount() > 1) {
        QPainterPath area = path;
        area.lineTo(lastX, r.bottom());
        area.lineTo(firstX, r.bottom());
        area.closeSubpath();
        QLinearGradient g(0, r.top(), 0, r.bottom());
        g.setColorAt(0.0, alpha(c, 60));
        g.setColorAt(1.0, alpha(c, 0));
        p.fillPath(area, g);
    }
    p.setPen(QPen(c, 1.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
}

}  // namespace

// ============================================================================ pipeline flow
class PipelineFlowView : public QWidget {
    Q_OBJECT
public:
    explicit PipelineFlowView(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(160);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setMouseTracking(true);
        anim_.setInterval(33);
        connect(&anim_, &QTimer::timeout, this, [this] { phase_ += 0.022; update(); });
        clock_.start();
    }
    void setStages(const QVector<Stage>& s, const QVector<std::deque<double>>* spark) { stages_ = s; spark_ = spark; update(); }
    void setSelected(int i) { selected_ = i; update(); }
    void setAnimating(bool on) { on ? anim_.start() : anim_.stop(); }
signals:
    void stageClicked(int index);
protected:
    QRectF nodeRect(int i) const {
        const int n = std::max<int>(1, stages_.size());
        const double m = 2.0, gap = 30.0;
        const double w = (width() - 2 * m - (n - 1) * gap) / n;
        return QRectF(m + i * (w + gap), 8.0, w, height() - 16.0);
    }
    void mousePressEvent(QMouseEvent* e) override {
        for (int i = 0; i < stages_.size(); ++i)
            if (nodeRect(i).contains(e->position())) { emit stageClicked(i); return; }
    }
    void mouseMoveEvent(QMouseEvent* e) override {
        int hit = -1;
        for (int i = 0; i < stages_.size(); ++i) if (nodeRect(i).contains(e->position())) hit = i;
        setCursor(hit >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            for (int i = 0; i < stages_.size(); ++i)
                if (nodeRect(i).contains(he->pos())) {
                    const Stage& s = stages_[i];
                    QToolTip::showText(he->globalPos(), QStringLiteral("<b>%1</b> · %2<br>%3<br><span style='color:#9a9ea5'>%4</span>")
                                           .arg(s.title.toHtmlEscaped(), healthWord(s.health), s.why.toHtmlEscaped(), s.detail.toHtmlEscaped()), this);
                    return true;
                }
            QToolTip::hideText();
            e->ignore();
            return true;
        }
        return QWidget::event(e);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        const double t = double(clock_.elapsed()) / 1000.0;
        for (int i = 0; i + 1 < stages_.size(); ++i) drawConnector(p, i, t);
        for (int i = 0; i < stages_.size(); ++i) drawNode(p, i, t);
    }
private:
    void drawConnector(QPainter& p, int i, double t) {
        const QRectF a = nodeRect(i), b = nodeRect(i + 1);
        const double y = a.top() + 46.0;
        const QPointF p0(a.right() + 3, y), p1(b.left() - 3, y);
        const int up = stages_[i].health, down = stages_[i + 1].health;
        const bool flowing = up == Ok || up == Warn;
        const bool blocked = flowing && down == Error;
        const QColor c = blocked ? tone::error() : flowing ? healthColour(up) : tone::border();
        p.setPen(QPen(alpha(c, flowing ? 150 : 255), 1.5, flowing && !blocked ? Qt::SolidLine : Qt::DashLine, Qt::FlatCap));
        p.drawLine(p0, p1 - QPointF(5, 0));
        // arrow head
        QPainterPath head;
        head.moveTo(p1);
        head.lineTo(p1 + QPointF(-6, -4));
        head.lineTo(p1 + QPointF(-6, 4));
        head.closeSubpath();
        p.fillPath(head, alpha(c, flowing ? 200 : 255));
        if (blocked) {
            // a break in the line, where the data stops
            const QPointF mid = (p0 + p1) / 2.0;
            const double pulse = 0.55 + 0.45 * std::sin(t * 5.0);
            p.setPen(QPen(alpha(tone::error(), int(160 + 95 * pulse)), 2.0, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(mid + QPointF(-4, -4), mid + QPointF(4, 4));
            p.drawLine(mid + QPointF(-4, 4), mid + QPointF(4, -4));
            return;
        }
        if (flowing && anim_.isActive()) {
            p.setPen(Qt::NoPen);
            p.setBrush(healthColour(up));
            const double len = p1.x() - 6 - p0.x();
            for (int k = 0; k < 3; ++k) {
                const double f = std::fmod(phase_ * (up == Warn ? 0.6 : 1.0) * 1.6 + k / 3.0, 1.0);
                p.drawEllipse(QPointF(p0.x() + f * len, y), 2.0, 2.0);
            }
        }
    }
    void drawNode(QPainter& p, int i, double t) {
        const Stage& s = stages_[i];
        const QRectF r = nodeRect(i).adjusted(0.5, 0.5, -0.5, -0.5);
        const QColor hc = healthColour(s.health);
        if (s.health == Error) {
            const double pulse = 0.5 + 0.5 * std::sin(t * 4.0);
            for (int k = 3; k >= 1; --k) {
                p.setPen(QPen(alpha(tone::error(), int((18 + 40 * pulse) / k)), 2.0 * k));
                p.setBrush(Qt::NoBrush);
                p.drawRoundedRect(r.adjusted(-k, -k, k, k), 4, 4);
            }
        }
        p.setPen(QPen(i == selected_ ? tone::accent() : QColor(0x35, 0x37, 0x3c), i == selected_ ? 1.5 : 1.0));
        p.setBrush(i == selected_ ? QColor(0x27, 0x29, 0x2d) : tone::surface());
        p.drawRoundedRect(r, 3, 3);
        // health rail on the left edge
        p.setPen(Qt::NoPen);
        p.setBrush(hc);
        p.drawRoundedRect(QRectF(r.left(), r.top(), 3.0, r.height()), 1.5, 1.5);

        const QRectF inner = r.adjusted(12, 9, -9, -8);
        p.setFont(uiFont(12, QFont::DemiBold));
        p.setPen(QColor(0xc4, 0xc7, 0xcb));
        const QString title = QFontMetrics(p.font()).elidedText(s.title, Qt::ElideRight, int(inner.width() - 16));
        p.drawText(QRectF(inner.left(), inner.top(), inner.width() - 14, 16), Qt::AlignLeft | Qt::AlignVCenter, title);
        // status dot
        p.setBrush(hc);
        p.drawEllipse(QPointF(inner.right() - 4, inner.top() + 8), 4.0, 4.0);
        p.setFont(uiFont(11));
        p.setPen(alpha(hc, s.health == Idle ? 200 : 255));
        p.drawText(QRectF(inner.left(), inner.top() + 17, inner.width(), 14), Qt::AlignLeft | Qt::AlignVCenter, healthWord(s.health));

        p.setFont(monoFont(inner.width() < 125 ? 15 : inner.width() < 150 ? 17 : 20, QFont::Medium));
        p.setPen(s.health == Idle ? tone::secondary() : QColor(0xec, 0xee, 0xf0));
        const QString head = QFontMetrics(p.font()).elidedText(s.headline.isEmpty() ? QStringLiteral("—") : s.headline,
                                                               Qt::ElideRight, int(inner.width()));
        p.drawText(QRectF(inner.left(), inner.top() + 36, inner.width(), 26), Qt::AlignLeft | Qt::AlignVCenter, head);
        p.setFont(uiFont(11));
        p.setPen(tone::muted());
        p.drawText(QRectF(inner.left(), inner.top() + 62, inner.width(), 14), Qt::AlignLeft | Qt::AlignVCenter,
                   QFontMetrics(p.font()).elidedText(s.caption, Qt::ElideRight, int(inner.width())));
        // sparkline
        const QRectF sr(inner.left(), inner.top() + 84, inner.width(), inner.bottom() - inner.top() - 84);
        if (sr.height() > 12 && spark_ && i < spark_->size()) {
            p.setPen(QPen(QColor(0x2f, 0x31, 0x35), 1));
            p.drawLine(QPointF(sr.left(), sr.bottom() + 0.5), QPointF(sr.right(), sr.bottom() + 0.5));
            drawTrend(p, sr, (*spark_)[i], s.health == Idle ? tone::muted() : hc, true, nullptr, nullptr, s.sparkUnit.startsWith(QLatin1Char('%')));
        }
    }

    QVector<Stage> stages_;
    const QVector<std::deque<double>>* spark_ = nullptr;
    int selected_ = -1;
    QTimer anim_;
    QElapsedTimer clock_;
    double phase_ = 0.0;
};

// ============================================================================ health timeline
class HealthTimelineView : public QWidget {
    Q_OBJECT
public:
    explicit HealthTimelineView(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(150);
        setMouseTracking(true);
    }
    void setTitles(const QStringList& t) {
        if (t == titles_) return;
        titles_ = t;
        cells_.clear();
        update();
    }
    void push(const QVector<int>& health, const QStringList& why, qint64 utcMs) {
        Column c{health, why, utcMs};
        cells_.push_back(c);
        while (int(cells_.size()) > kTimelineSec) cells_.pop_front();
        update();
    }
    void clear() { cells_.clear(); update(); }
signals:
    void rowClicked(int row);
protected:
    QRectF plot() const { return QRectF(118, 4, width() - 126, height() - 24); }
    void mousePressEvent(QMouseEvent* e) override {
        const QRectF pr = plot();
        if (titles_.isEmpty() || e->position().y() < pr.top() || e->position().y() > pr.bottom()) return;
        emit rowClicked(int((e->position().y() - pr.top()) / (pr.height() / titles_.size())));
    }
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            const QRectF pr = plot();
            if (!titles_.isEmpty() && pr.contains(he->pos()) && !cells_.empty()) {
                const double cw = pr.width() / kTimelineSec;
                const int fromRight = int((pr.right() - he->pos().x()) / cw);
                const int idx = int(cells_.size()) - 1 - fromRight;
                const int row = int((he->pos().y() - pr.top()) / (pr.height() / titles_.size()));
                if (idx >= 0 && idx < int(cells_.size()) && row >= 0 && row < titles_.size()) {
                    const Column& c = cells_[size_t(idx)];
                    const int h = row < c.health.size() ? c.health[row] : Idle;
                    QToolTip::showText(he->globalPos(), QStringLiteral("%1 UTC · <b>%2</b> · %3<br>%4")
                        .arg(QDateTime::fromMSecsSinceEpoch(c.ms, QTimeZone::UTC).toString(QStringLiteral("HH:mm:ss")),
                             titles_[row].toHtmlEscaped(), healthWord(h),
                             row < c.why.size() ? c.why[row].toHtmlEscaped() : QString()), this);
                    return true;
                }
            }
            QToolTip::hideText();
            e->ignore();
            return true;
        }
        return QWidget::event(e);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), tone::sunken());
        const QRectF pr = plot();
        const int rows = titles_.size();
        if (rows == 0) return;
        const double rh = pr.height() / rows;
        const double cw = pr.width() / kTimelineSec;
        p.setFont(uiFont(11));
        for (int r = 0; r < rows; ++r) {
            const QRectF lr(6, pr.top() + r * rh, pr.left() - 12, rh);
            p.setPen(tone::secondary());
            p.drawText(lr, Qt::AlignRight | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(titles_[r], Qt::ElideRight, int(lr.width())));
            p.fillRect(QRectF(pr.left(), pr.top() + r * rh + 1, pr.width(), rh - 2), QColor(0x1c, 0x1d, 0x20));
        }
        const int n = int(cells_.size());
        for (int i = 0; i < n; ++i) {
            const Column& c = cells_[size_t(i)];
            const double x = pr.right() - (n - i) * cw;
            for (int r = 0; r < rows && r < c.health.size(); ++r) {
                const int h = c.health[r];
                if (h == Idle) continue;
                const int a = h == Ok ? 115 : h == Warn ? 210 : 235;
                p.fillRect(QRectF(x, pr.top() + r * rh + 2, std::max(1.0, cw - 0.25), rh - 4), alpha(healthColour(h), a));
            }
        }
        // minute marks
        p.setPen(QPen(QColor(0x45, 0x48, 0x4e), 1, Qt::DotLine));
        p.setFont(uiFont(10));
        for (int m = 0; m <= 5; ++m) {
            const double x = pr.right() - m * 60 * cw;
            p.setPen(QPen(QColor(0x3a, 0x3c, 0x41), 1, Qt::DotLine));
            p.drawLine(QPointF(x, pr.top()), QPointF(x, pr.bottom()));
            p.setPen(tone::muted());
            p.drawText(QRectF(x - 40, pr.bottom() + 3, 80, 14), Qt::AlignCenter,
                       m == 0 ? QStringLiteral("now") : QStringLiteral("−%1 min").arg(m));
        }
    }
private:
    struct Column { QVector<int> health; QStringList why; qint64 ms; };
    QStringList titles_;
    std::deque<Column> cells_;
};

// ============================================================================ loss funnel
class LossFunnelView : public QWidget {
public:
    explicit LossFunnelView(QWidget* parent = nullptr) : QWidget(parent) { setMinimumHeight(60); }
    void setStages(const QVector<Stage>& s) {
        rows_.clear();
        for (const Stage& st : s) if (st.passed >= 0 || st.rejected > 0) rows_ << st;
        setMinimumHeight(std::max(60, int(rows_.size()) * 26 + 6));
        update();
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const double labelW = std::min(118.0, width() * 0.26), textW = std::min(330.0, width() * 0.5);
        const QRectF barArea(labelW, 0, width() - labelW - textW - 12, height());
        if (rows_.isEmpty()) {
            p.setPen(tone::muted());
            p.setFont(uiFont(12));
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No data has entered the chain yet"));
            return;
        }
        for (int i = 0; i < rows_.size(); ++i) {
            const Stage& s = rows_[i];
            const double y = 3 + i * 26;
            p.setFont(uiFont(11));
            p.setPen(tone::secondary());
            p.drawText(QRectF(6, y, labelW - 12, 20), Qt::AlignRight | Qt::AlignVCenter,
                       QFontMetrics(p.font()).elidedText(s.title, Qt::ElideRight, int(labelW - 12)));
            const double pass = std::max(0.0, s.passed), rej = std::max(0.0, s.rejected);
            const double total = pass + rej;
            const QRectF track(barArea.left(), y + 5, barArea.width(), 10);
            p.fillRect(track, QColor(0x1c, 0x1d, 0x20));
            if (total > 0) {
                const double fr = rej / total;
                // A sliver of red stays visible however small the loss: losses are what this view is for.
                const double redW = rej > 0 ? std::max(3.0, fr * track.width()) : 0.0;
                p.fillRect(QRectF(track.left(), track.top(), track.width() - redW, track.height()), alpha(healthColour(s.health == Idle ? Idle : Ok), 150));
                if (redW > 0) p.fillRect(QRectF(track.right() - redW, track.top(), redW, track.height()), tone::error());
            }
            p.setFont(monoFont(11));
            p.setPen(rej > 0 ? QColor(0xf0, 0x8f, 0x7f) : tone::text());
            const auto fmt = [](double v) { return compact(v); };
            const QString text = rej > 0
                ? QStringLiteral("%1 %2 · %3 lost (%4 %)").arg(fmt(pass), s.unit, fmt(rej)).arg(100.0 * rej / std::max(1.0, total), 0, 'f', rej / total < 0.01 ? 2 : 1)
                : QStringLiteral("%1 %2").arg(fmt(pass), s.unit);
            p.drawText(QRectF(barArea.right() + 12, y, textW, 20), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(p.font()).elidedText(text, Qt::ElideRight, int(textW)));
        }
    }
private:
    QVector<Stage> rows_;
};

// ============================================================================ scope
class StageScopeView : public QWidget {
public:
    enum Mode { Trend, Constellation, Spectrum };
    explicit StageScopeView(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(170);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    void setMode(Mode m) { mode_ = m; update(); }
    Mode mode() const { return mode_; }
    void setTrend(const std::deque<double>& v, const QString& unit, int health) { trend_ = v; unit_ = unit; health_ = health; if (mode_ == Trend) update(); }
    void setPoints(const std::vector<std::complex<float>>& pts) { pts_ = pts; if (mode_ == Constellation) update(); }
    void setSpectrum(const std::vector<float>& db) { db_ = db; if (mode_ == Spectrum) update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), tone::sunken());
        p.setPen(QColor(0x2f, 0x31, 0x35));
        p.drawRect(rect().adjusted(0, 0, -1, -1));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setFont(uiFont(10));
        if (mode_ == Constellation) paintConstellation(p);
        else if (mode_ == Spectrum) paintSpectrum(p);
        else paintTrend(p);
    }
private:
    void paintTrend(QPainter& p) {
        const QRectF r = QRectF(rect()).adjusted(46, 10, -10, -22);
        double lo = 0, hi = 1;
        p.setPen(QColor(0x2b, 0x2d, 0x31));
        for (int k = 0; k <= 4; ++k) {
            const double y = r.top() + r.height() * k / 4.0;
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
        }
        bool any = false;
        for (double v : trend_) if (std::isfinite(v)) any = true;
        if (!any) {
            p.setPen(tone::muted());
            p.drawText(r, Qt::AlignCenter, QStringLiteral("No values yet"));
            return;
        }
        drawTrend(p, r, trend_, healthColour(health_), true, &lo, &hi, unit_.startsWith(QLatin1Char('%')));
        p.setPen(tone::muted());
        for (int k = 0; k <= 4; ++k) {
            const double v = hi - (hi - lo) * k / 4.0;
            const double y = r.top() + r.height() * k / 4.0;
            p.drawText(QRectF(0, y - 7, r.left() - 6, 14), Qt::AlignRight | Qt::AlignVCenter,
                       std::fabs(v) >= 1000 ? QString::number(v, 'f', 0) : QString::number(v, 'g', 3));
        }
        p.drawText(QRectF(r.left(), r.bottom() + 4, r.width(), 14), Qt::AlignLeft, QStringLiteral("−75 s"));
        p.drawText(QRectF(r.left(), r.bottom() + 4, r.width(), 14), Qt::AlignRight, QStringLiteral("now · %1").arg(unit_));
    }
    void paintConstellation(QPainter& p) {
        const double side = std::min(width(), height()) - 20.0;
        const QRectF r((width() - side) / 2.0, (height() - side) / 2.0, side, side);
        p.setPen(QColor(0x2b, 0x2d, 0x31));
        p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
        p.drawEllipse(r.center(), side / 4.0, side / 4.0);
        if (pts_.empty()) {
            p.setPen(tone::muted());
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No symbols"));
            return;
        }
        double m = 0;
        for (const auto& z : pts_) m += std::abs(z);
        m = pts_.empty() ? 1.0 : std::max(1e-6, m / double(pts_.size()));
        const double scale = side / 4.0 / m;
        p.setPen(Qt::NoPen);
        p.setBrush(alpha(tone::accent().lighter(130), 110));
        const std::size_t step = std::max<std::size_t>(1, pts_.size() / 3000);
        for (std::size_t i = 0; i < pts_.size(); i += step) {
            const QPointF q(r.center().x() + pts_[i].real() * scale, r.center().y() - pts_[i].imag() * scale);
            if (r.contains(q)) p.drawEllipse(q, 1.2, 1.2);
        }
        // magnitude spread: how far the symbols scatter round their mean distance from the origin
        double var = 0;
        for (const auto& z : pts_) { const double d = std::abs(z) - m; var += d * d; }
        const double spread = std::sqrt(var / double(pts_.size())) / m;
        p.setPen(tone::secondary());
        p.drawText(QRectF(8, 6, width() - 16, 14), Qt::AlignLeft,
                   QStringLiteral("%1 symbols · magnitude spread %2 %").arg(pts_.size()).arg(spread * 100.0, 0, 'f', 1));
    }
    void paintSpectrum(QPainter& p) {
        const QRectF r = QRectF(rect()).adjusted(40, 10, -10, -18);
        if (db_.size() < 8) {
            p.setPen(tone::muted());
            p.drawText(rect(), Qt::AlignCenter, QStringLiteral("No spectrum"));
            return;
        }
        float lo = *std::min_element(db_.begin(), db_.end()), hi = *std::max_element(db_.begin(), db_.end());
        lo = std::floor(lo / 10.0f) * 10.0f;
        hi = std::ceil(hi / 10.0f) * 10.0f;
        if (hi - lo < 10) hi = lo + 10;
        p.setPen(QColor(0x2b, 0x2d, 0x31));
        for (float v = lo; v <= hi; v += 10.0f) {
            const double y = r.bottom() - (v - lo) / (hi - lo) * r.height();
            p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
            p.setPen(tone::muted());
            p.drawText(QRectF(0, y - 7, r.left() - 5, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'f', 0));
            p.setPen(QColor(0x2b, 0x2d, 0x31));
        }
        QPainterPath path;
        const int n = int(db_.size());
        for (int i = 0; i < n; ++i) {
            const double x = r.left() + r.width() * i / double(n - 1);
            const double y = r.bottom() - (db_[size_t(i)] - lo) / (hi - lo) * r.height();
            i == 0 ? path.moveTo(x, y) : path.lineTo(x, y);
        }
        p.setPen(QPen(tone::accent().lighter(120), 1.2));
        p.drawPath(path);
        p.setPen(QPen(alpha(tone::warn(), 120), 1, Qt::DashLine));
        p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
        p.setPen(tone::muted());
        p.drawText(QRectF(r.left(), r.bottom() + 2, r.width(), 14), Qt::AlignCenter, QStringLiteral("tuned frequency · dB"));
    }
    Mode mode_ = Trend;
    std::deque<double> trend_;
    QString unit_;
    int health_ = Idle;
    std::vector<std::complex<float>> pts_;
    std::vector<float> db_;
};

// ============================================================================ metric list
class MetricListView : public QWidget {
public:
    explicit MetricListView(QWidget* parent = nullptr) : QWidget(parent) { setMouseTracking(true); }
    void setMetrics(const QVector<Metric>& m) {
        metrics_ = m;
        setMinimumHeight(int(m.size()) * kRow + 4);
        updateGeometry();
        update();
    }
protected:
    static constexpr int kRow = 22;
    bool event(QEvent* e) override {
        if (e->type() == QEvent::ToolTip) {
            auto* he = static_cast<QHelpEvent*>(e);
            const int row = he->pos().y() / kRow;
            if (row >= 0 && row < metrics_.size() && !metrics_[row].tip.isEmpty()) {
                QToolTip::showText(he->globalPos(), metrics_[row].tip, this);
                return true;
            }
            QToolTip::hideText();
            e->ignore();
            return true;
        }
        return QWidget::event(e);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        for (int i = 0; i < metrics_.size(); ++i) {
            const Metric& m = metrics_[i];
            const QRectF r(0, i * kRow, width(), kRow);
            if (i % 2 == 1) p.fillRect(r, QColor(0x26, 0x27, 0x2b));
            p.setPen(Qt::NoPen);
            p.setBrush(healthColour(m.health));
            p.drawEllipse(QPointF(r.left() + 9, r.center().y()), 3.0, 3.0);
            p.setFont(uiFont(12));
            p.setPen(tone::secondary());
            const double nameW = std::min(200.0, r.width() * 0.48);
            p.drawText(QRectF(r.left() + 20, r.top(), nameW, r.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       QFontMetrics(p.font()).elidedText(m.name + (m.tip.isEmpty() ? QString() : QStringLiteral(" ⓘ")), Qt::ElideRight, int(nameW)));
            p.setFont(monoFont(12));
            p.setPen(m.health == Error ? QColor(0xf0, 0x8f, 0x7f) : m.health == Warn ? QColor(0xe9, 0xbd, 0x62) : tone::text());
            const QRectF vr(r.left() + 20 + nameW, r.top(), r.width() - nameW - 28, r.height());
            p.drawText(vr, Qt::AlignRight | Qt::AlignVCenter, QFontMetrics(p.font()).elidedText(m.value, Qt::ElideLeft, int(vr.width())));
        }
    }
private:
    QVector<Metric> metrics_;
};

// ============================================================================ page
DecoderPage::DecoderPage(QWidget* parent) : QWidget(parent) {
    using namespace nadirstation::ui;
    auto* v = new QVBoxLayout(this);
    v->setContentsMargins(4, 2, 4, 4);
    v->setSpacing(8);

    pause_ = new QPushButton(QStringLiteral("Pause"));
    pause_->setObjectName(QStringLiteral("Quiet"));
    pause_->setCheckable(true);
    pause_->setToolTip(QStringLiteral("Freeze the display; the data keeps being collected"));
    auto* clear = new QPushButton(QStringLiteral("Clear history"));
    clear->setObjectName(QStringLiteral("Quiet"));
    auto* trailing = new QWidget;
    auto* th = new QHBoxLayout(trailing);
    th->setContentsMargins(0, 0, 0, 0);
    uptime_ = label(QString(), "CardHint");
    th->addWidget(uptime_);
    th->addSpacing(8);
    th->addWidget(pause_);
    th->addWidget(clear);
    auto* header = pageHeader(QStringLiteral("Decoder"), QStringLiteral("Every stage of the receive chain, live"), trailing);
    subtitle_ = header->findChild<QLabel*>(QStringLiteral("PageSubtitle"));
    v->addWidget(header);

    // Verdict: one line that says where the chain is broken, in the colour of the worst stage.
    verdictBox_ = new QWidget;
    verdictBox_->setObjectName(QStringLiteral("DecoderVerdict"));
    verdictBox_->setAttribute(Qt::WA_StyledBackground, true);
    auto* vb = new QHBoxLayout(verdictBox_);
    vb->setContentsMargins(14, 9, 14, 9);
    verdict_ = new QLabel;
    verdict_->setObjectName(QStringLiteral("DecoderVerdictText"));
    verdict_->setWordWrap(true);
    vb->addWidget(verdict_, 1);
    v->addWidget(verdictBox_);

    flow_ = new PipelineFlowView;
    v->addWidget(flow_);

    auto* split = new QSplitter(Qt::Horizontal);
    split->setChildrenCollapsible(false);
    split->setHandleWidth(8);

    auto* left = new QWidget;
    auto* lv = new QVBoxLayout(left);
    lv->setContentsMargins(0, 0, 0, 0);
    lv->setSpacing(8);
    Card tl = card(QStringLiteral("Health over time"), nullptr, QStringLiteral("one column per second · hover for the reason · click a row to inspect"));
    timeline_ = new HealthTimelineView;
    tl.body->addWidget(timeline_, 1);
    lv->addWidget(tl.frame, 1);
    // Loss funnel and change log side by side: the page has to fit a 768-px-high screen.
    auto* lower = new QHBoxLayout;
    lower->setSpacing(8);
    Card fn = card(QStringLiteral("Where data is lost"), nullptr, QStringLiteral("since decoding started"));
    funnel_ = new LossFunnelView;
    fn.body->addWidget(funnel_);
    fn.body->addStretch(1);
    lower->addWidget(fn.frame, 1);
    Card lg = card(QStringLiteral("Changes"), nullptr, QStringLiteral("every time a stage changes colour"));
    log_ = new QPlainTextEdit;
    log_->setReadOnly(true);
    log_->setMaximumBlockCount(600);
    log_->setFont(monoFont(11));
    log_->setMinimumHeight(90);
    lg.body->addWidget(log_, 1);
    lower->addWidget(lg.frame, 1);
    lv->addLayout(lower, 1);
    split->addWidget(left);

    Card det = card(QString());
    stageTitle_ = label(QString(), "PageTitle");
    stageDetail_ = label(QString(), "Note");
    stageDetail_->setWordWrap(true);
    stageWhy_ = new QLabel;
    stageWhy_->setObjectName(QStringLiteral("DecoderWhy"));
    stageWhy_->setWordWrap(true);
    scope_ = new StageScopeView;
    metrics_ = new MetricListView;
    det.body->addWidget(stageTitle_);
    det.body->addWidget(stageDetail_);
    det.body->addWidget(stageWhy_);
    det.body->addWidget(scope_, 1);
    det.body->addWidget(metrics_);
    split->addWidget(det.frame);
    det.frame->setMinimumWidth(330);
    split->setStretchFactor(0, 5);
    split->setStretchFactor(1, 3);
    v->addWidget(split, 1);

    setStyleSheet(QStringLiteral(
        "#DecoderVerdict { background: #232427; border: 1px solid #2f3135; border-left: 3px solid #6e737a; border-radius: 2px; }"
        "#DecoderVerdict[tone=\"ok\"] { border-left-color: #3fca85; }"
        "#DecoderVerdict[tone=\"warn\"] { border-left-color: #f0b83c; background: #2a2620; }"
        "#DecoderVerdict[tone=\"error\"] { border-left-color: #f0594c; background: #2d2122; }"
        "#DecoderVerdictText { color: #eceef0; font-size: 13px; }"
        "#DecoderWhy { color: #d6d8db; font-size: 12px; padding: 4px 0 2px 0; }"));

    spark_.resize(7);
    connect(flow_, &PipelineFlowView::stageClicked, this, &DecoderPage::select);
    connect(timeline_, &HealthTimelineView::rowClicked, this, &DecoderPage::select);
    connect(pause_, &QPushButton::toggled, this, [this](bool on) { paused_ = on; pause_->setText(on ? QStringLiteral("Resume") : QStringLiteral("Pause")); if (!on) present(); });
    connect(clear, &QPushButton::clicked, this, [this] {
        timeline_->clear();
        for (auto& d : spark_) d.clear();
        log_->clear();
        receiverLossCount_ = 0;
        present();
    });
    secondTimer_.setInterval(1000);
    connect(&secondTimer_, &QTimer::timeout, this, &DecoderPage::tick);
    secondTimer_.start();
    sinceSnapshot_.start();
    present();
}

void DecoderPage::setActive(bool active) {
    active_ = active;
    flow_->setAnimating(active && receiving_ && decoding_);
    if (active) present();
}

void DecoderPage::setRunState(bool receiving, bool decoding, bool fileInput, const QString& satellite) {
    const bool changed = receiving != receiving_ || decoding != decoding_ || satellite != satellite_;
    if (decoding && !decoding_) {
        // A new decoding run: the counters start again, so the old history would mislead.
        haveSnapshot_ = false;
        for (auto& d : spark_) d.clear();
        shownHealth_.clear();
        pendingHealth_.clear();
        secondWorst_.clear();
        receiverLossCount_ = 0;
        log_->appendPlainText(QStringLiteral("%1  — decoding started (%2)")
            .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")), satellite.isEmpty() ? QStringLiteral("no satellite") : satellite));
    } else if (!decoding && decoding_) {
        log_->appendPlainText(QStringLiteral("%1  — decoding stopped").arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss"))));
    }
    receiving_ = receiving;
    decoding_ = decoding;
    fileInput_ = fileInput;
    satellite_ = satellite;
    flow_->setAnimating(active_ && receiving_ && decoding_);
    if (changed) present();
}

void DecoderPage::setSnapshot(const Snapshot& snapshot) {
    last_ = snapshot;
    haveSnapshot_ = true;
    sinceSnapshot_.restart();
    if (spark_.size() != snapshot.stages.size()) spark_.resize(snapshot.stages.size());
    for (int i = 0; i < snapshot.stages.size(); ++i) {
        spark_[i].push_back(snapshot.stages[i].spark);
        while (int(spark_[i].size()) > kSparkLen) spark_[i].pop_front();
    }
    const Snapshot d = decorated();
    const int n = d.stages.size();
    if (shownHealth_.size() != n) shownHealth_ = QVector<int>(n, Idle);
    if (pendingHealth_.size() != n) { pendingHealth_ = QVector<int>(n, Idle); pendingCount_ = QVector<int>(n, 0); }
    if (secondWorst_.size() != n) { secondWorst_ = QVector<int>(n, Idle); secondWhy_ = QStringList(); for (int i = 0; i < n; ++i) secondWhy_ << QString(); }
    for (int i = 0; i < n; ++i) {
        const Stage& st = d.stages[i];
        // Worst state within the current second, so a short fault still marks the timeline.
        if (st.health >= secondWorst_[i]) { secondWorst_[i] = st.health; secondWhy_[i] = st.why; }
        // The log takes a change only once it has held for four snapshots (~1 s): a value sitting on
        // a threshold would otherwise fill it with flapping.
        if (st.health == shownHealth_[i]) { pendingCount_[i] = 0; continue; }
        if (st.health != pendingHealth_[i]) { pendingHealth_[i] = st.health; pendingCount_[i] = 0; }
        if (++pendingCount_[i] >= 4) {
            logTransition(st, shownHealth_[i]);
            shownHealth_[i] = st.health;
            pendingCount_[i] = 0;
        }
    }
    if (active_ && !paused_) present();
}

void DecoderPage::setConstellation(const std::vector<std::complex<float>>& points) {
    if (!active_ || paused_) return;
    points_ = points;
    if (scope_->mode() == StageScopeView::Constellation) scope_->setPoints(points_);
}

void DecoderPage::setSpectrum(const std::vector<float>& db, double sampleRate) {
    if (!active_ || paused_) return;
    spectrum_ = db;
    spectrumRate_ = sampleRate;
    if (scope_->mode() == StageScopeView::Spectrum) scope_->setSpectrum(spectrum_);
}

void DecoderPage::noteReceiverLoss(const QString& text) {
    ++receiverLossCount_;
    receiverLossText_ = text;
    sinceReceiverLoss_.restart();
    log_->appendPlainText(QStringLiteral("%1  IQ input             receiver: %2")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")), text));
}

Snapshot DecoderPage::idleTemplate() const {
    Snapshot s;
    s.link = satellite_.isEmpty() ? QStringLiteral("No satellite selected") : satellite_;
    const QStringList titles{QStringLiteral("IQ input"), QStringLiteral("Carrier"), QStringLiteral("Demodulator"), QStringLiteral("Frame sync"),
                             QStringLiteral("Error correction"), QStringLiteral("Data layer"), QStringLiteral("Products")};
    const QStringList keys{QStringLiteral("iq"), QStringLiteral("carrier"), QStringLiteral("demod"), QStringLiteral("sync"),
                           QStringLiteral("fec"), QStringLiteral("data"), QStringLiteral("product")};
    for (int i = 0; i < titles.size(); ++i) {
        Stage st;
        st.key = keys[i];
        st.title = titles[i];
        st.headline = QStringLiteral("—");
        st.caption = QStringLiteral("waiting");
        st.why = !receiving_ ? QStringLiteral("Not receiving.") : QStringLiteral("The decoder is off.");
        s.stages << st;
    }
    return s;
}

// The decoder's snapshot plus what only the window knows: whether anything runs at all, whether the
// input is a file (then DSP load says nothing about real time), receiver-side sample loss, and a
// decoder that has stopped reporting.
Snapshot DecoderPage::decorated() const {
    if (!receiving_ || !decoding_ || !haveSnapshot_) return idleTemplate();
    Snapshot s = last_;
    for (Stage& st : s.stages) {
        if (st.key != QLatin1String("iq")) continue;
        if (fileInput_) {
            // A file is read as fast as the DSP can go, so the load is ~100 % by design.
            for (Metric& m : st.metrics)
                if (m.name == QLatin1String("DSP load")) { m.health = Ok; m.tip = QStringLiteral("File input: the DSP runs flat out, so ~100 % is normal."); }
            int h = Ok;
            for (const Metric& m : st.metrics) h = worst(h, m.health);
            st.health = h;
            st.headline = QString();
            for (const Metric& m : st.metrics) if (m.name == QLatin1String("Throughput")) st.headline = m.value.section(QLatin1Char(' '), 0, 0) + QStringLiteral(" ×");
            st.caption = QStringLiteral("file playback, × real time");
            if (st.health == Ok) st.why = QStringLiteral("Reading the recording as fast as the DSP allows.");
        }
        if (receiverLossCount_ > 0) {
            const bool recent = sinceReceiverLoss_.isValid() && sinceReceiverLoss_.elapsed() < 10000;
            st.metrics << Metric{QStringLiteral("Receiver sample loss"), QStringLiteral("%1 reports").arg(receiverLossCount_),
                                 recent ? int(Error) : int(Warn), receiverLossText_};
            if (recent) {
                st.health = Error;
                st.why = QStringLiteral("The receiver reported lost samples (USB / network overflow): %1").arg(receiverLossText_);
            } else {
                st.health = worst(st.health, Warn);
            }
        }
        if (sinceSnapshot_.elapsed() > 2500) {
            st.health = Error;
            st.why = QStringLiteral("No samples have reached the decoder for %1 s.").arg(double(sinceSnapshot_.elapsed()) / 1000.0, 0, 'f', 0);
            st.headline = QStringLiteral("stalled");
        }
    }
    // Starved downstream of a red stage (the decoder does this for its own stages; the window can
    // have turned the input red since).
    bool down = false;
    for (Stage& st : s.stages) {
        if (down && st.health == Error) {
            st.health = Idle;
            if (!st.why.startsWith(QStringLiteral("Starved"))) st.why = QStringLiteral("Starved: an earlier stage is failing. ") + st.why;
        }
        if (st.health == Error) down = true;
    }
    return s;
}

void DecoderPage::logTransition(const Stage& st, int from) {
    const int to = st.health;
    // Healthy → healthy and idle flapping are not news; anything into or out of amber/red is.
    if (to <= Ok && from <= Ok) return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    const QString line = QStringLiteral("%1  %2 %3 → %4   %5")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")))
        .arg(st.title.leftJustified(18, QLatin1Char(' '), true))
        .arg(healthWord(from).leftJustified(8, QLatin1Char(' ')))
        .arg(healthWord(to).leftJustified(8, QLatin1Char(' ')))
        .arg(st.why);
    log_->appendPlainText(line);
    Q_UNUSED(now);
}

void DecoderPage::select(int index) {
    selected_ = index;
    present();
}

void DecoderPage::tick() {
    const Snapshot d = decorated();
    QStringList titles, why;
    QVector<int> health;
    for (int i = 0; i < d.stages.size(); ++i) {
        const Stage& st = d.stages[i];
        titles << st.title;
        const bool have = i < secondWorst_.size() && haveSnapshot_ && receiving_ && decoding_;
        health << (have ? std::max(secondWorst_[i], st.health) : st.health);
        why << (have && secondWorst_[i] > st.health ? secondWhy_[i] : st.why);
    }
    for (int& h : secondWorst_) h = Idle;
    timeline_->setTitles(titles);
    if (receiving_ && decoding_) timeline_->push(health, why, QDateTime::currentMSecsSinceEpoch());
    if (uptime_) {
        if (haveSnapshot_ && receiving_ && decoding_) {
            const int s = int(last_.streamSeconds);
            uptime_->setText(QStringLiteral("signal %1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0')));
        } else {
            uptime_->clear();
        }
    }
    if (active_ && !paused_) present();
}

void DecoderPage::present() {
    if (paused_) return;
    const Snapshot d = decorated();
    if (subtitle_)
        subtitle_->setText(d.chain.isEmpty() ? d.link : QStringLiteral("%1 · %2").arg(d.link, d.chain));
    flow_->setStages(d.stages, &spark_);
    if (selected_ >= d.stages.size()) selected_ = d.stages.size() - 1;
    flow_->setSelected(selected_);
    funnel_->setStages(d.stages);

    // verdict
    int worstH = Idle;
    const Stage* first = nullptr;
    for (const Stage& st : d.stages) {
        if (st.health > worstH) { worstH = st.health; first = &st; }
        else if (st.health == worstH && !first) first = &st;
    }
    QString text, toneName;
    if (!receiving_) { text = QStringLiteral("Not receiving. Start the receiver on the Live page to see the chain."); toneName = QStringLiteral("idle"); }
    else if (!decoding_) { text = QStringLiteral("Receiving, but the decoder is off. Press Start Decoding to run the chain."); toneName = QStringLiteral("idle"); }
    else if (!haveSnapshot_) { text = QStringLiteral("Starting… waiting for the first samples to pass through the decoder."); toneName = QStringLiteral("idle"); }
    else if (worstH == Error && first) { text = QStringLiteral("<b>Fault at %1.</b> %2").arg(first->title.toHtmlEscaped(), first->why.toHtmlEscaped()); toneName = QStringLiteral("error"); }
    else if (worstH == Warn && first) { text = QStringLiteral("<b>Marginal at %1.</b> %2").arg(first->title.toHtmlEscaped(), first->why.toHtmlEscaped()); toneName = QStringLiteral("warn"); }
    else if (worstH == Ok) {
        const Stage& last = d.stages.back();
        text = last.health == Ok ? QStringLiteral("<b>All stages healthy.</b> Data reaches the products: %1 %2.").arg(last.headline, last.caption)
                                 : QStringLiteral("<b>All active stages healthy.</b> %1").arg(last.why.toHtmlEscaped());
        toneName = QStringLiteral("ok");
    } else { text = QStringLiteral("Waiting for signal."); toneName = QStringLiteral("idle"); }
    verdict_->setText(text);
    nadirstation::ui::setTone(verdictBox_, "tone", toneName);

    // selected stage
    if (selected_ >= 0 && selected_ < d.stages.size()) {
        const Stage& st = d.stages[selected_];
        stageTitle_->setText(st.title);
        stageDetail_->setText(st.detail.isEmpty() ? QString() : st.detail);
        stageDetail_->setVisible(!st.detail.isEmpty());
        stageWhy_->setText(QStringLiteral("<span style='color:%1'>●</span> %2 — %3")
            .arg(healthColour(st.health).name(), healthWord(st.health), st.why.toHtmlEscaped()));
        const StageScopeView::Mode mode = st.key == QLatin1String("demod") ? StageScopeView::Constellation
                                        : st.key == QLatin1String("carrier") ? StageScopeView::Spectrum
                                        : StageScopeView::Trend;
        scope_->setMode(mode);
        if (mode == StageScopeView::Constellation) scope_->setPoints(points_);
        else if (mode == StageScopeView::Spectrum) scope_->setSpectrum(spectrum_);
        else if (selected_ < spark_.size()) scope_->setTrend(spark_[selected_], st.sparkUnit, st.health);
        metrics_->setMetrics(st.metrics);
    }
}

#include "decoderpage.moc"
