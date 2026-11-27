#include "spectrumwidget.h"
#include <QLinearGradient>
#include <QMouseEvent>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QDir>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPolygonF>
#include <algorithm>
#include <cmath>

namespace {
// Range covered by the cut-off slider strip, and the smallest allowed span between the handles.
constexpr float kStripTopDb = 20.0f;
constexpr float kStripBottomDb = -160.0f;
constexpr float kMinSpanDb = 6.0f;

constexpr int kInfoHeight = 22;    // sample rate / range line above the spectrum
constexpr int kFreqHeight = 20;    // frequency labels between spectrum and waterfall
constexpr int kLeftAxis = 46;
constexpr int kStripWidth = 12;
constexpr int kStripGap = 8;
constexpr int kRightMargin = 4;
constexpr double kSpectrumShare = 0.42;   // of the plot height; the waterfall gets the rest
constexpr double kSmoothingMs = 70.0;     // time constant of the trace's glide towards a new frame

const QColor kBackground(10, 16, 23);
const QColor kGrid(28, 40, 53);
const QColor kAxisText(142, 160, 177);
const QColor kTrace(222, 226, 230);

QColor mix(const QColor& a, const QColor& b, double t) {
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t,
                            a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t);
}

double niceStep(double span, int targetTicks) {
    const double raw = span / std::max(1, targetTicks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * mag >= raw) return m * mag;
    return 10.0 * mag;
}
}  // namespace

SpectrumWidget::SpectrumWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(360, 300);
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    // Waterfall palette in the manner of SatDump's classic map:
    // black -> navy -> blue -> cyan -> yellow -> orange -> red.
    const QColor stops[] = {QColor(0, 0, 12), QColor(0, 0, 110), QColor(0, 90, 230), QColor(0, 220, 230),
                            QColor(245, 240, 40), QColor(255, 130, 0), QColor(220, 20, 10)};
    constexpr int n = sizeof(stops) / sizeof(stops[0]);
    for (int i = 0; i < 256; ++i) {
        const double t = i / 255.0 * (n - 1);
        const int k = std::min(n - 2, static_cast<int>(t));
        lut_[i] = mix(stops[k], stops[k + 1], t - k).rgb();
    }

    animation_.setTimerType(Qt::PreciseTimer);
    animation_.setInterval(16);
    connect(&animation_, &QTimer::timeout, this, &SpectrumWidget::animate);
    lastFrame_.start();

    peakAction_ = new QAction(QStringLiteral("Peak hold"), this);
    peakAction_->setCheckable(true);
    peakAction_->setToolTip(QStringLiteral("Keep the maximum of every bin as a second trace"));
    connect(peakAction_, &QAction::toggled, this, [this](bool on) { if (!on) peakDb_.clear(); update(); });
    freezeAction_ = new QAction(QStringLiteral("Freeze spectrum"), this);
    freezeAction_->setCheckable(true);
    connect(freezeAction_, &QAction::toggled, this, [this] { update(); });
    clearMarkersAction_ = new QAction(QStringLiteral("Clear markers"), this);
    connect(clearMarkersAction_, &QAction::triggered, this, [this] { markerHz_.clear(); update(); });
    autoRangeAction_ = new QAction(QStringLiteral("Automatic range"), this);
    connect(autoRangeAction_, &QAction::triggered, this, [this] { autoRange_ = true; update(); });
    saveAction_ = new QAction(QStringLiteral("Save spectrum image"), this);
    connect(saveAction_, &QAction::triggered, this, [this] { saveSnapshot(); });
}

QString SpectrumWidget::saveSnapshot() {
    QString dir = snapshotDir_.isEmpty() ? QDir::currentPath() : snapshotDir_;
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("spectrum_%1.png")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    const bool hadHover = hover_;
    hover_ = false;   // no cursor read-out in the picture
    const bool ok = grab().save(path);
    hover_ = hadHover;
    if (!ok) return QString();
    emit snapshotSaved(path);
    return path;
}

void SpectrumWidget::contextMenuEvent(QContextMenuEvent* e) {
    QMenu menu(this);
    menu.addAction(peakAction_);
    menu.addAction(freezeAction_);
    menu.addSeparator();
    menu.addAction(clearMarkersAction_);
    clearMarkersAction_->setEnabled(!markerHz_.empty());
    menu.addAction(autoRangeAction_);
    menu.addSeparator();
    menu.addAction(saveAction_);
    menu.exec(e->globalPos());
    clearMarkersAction_->setEnabled(true);
}

float SpectrumWidget::levelAt(double hz) const {
    const std::vector<float>& t = shownDb_.size() == avgDb_.size() ? shownDb_ : avgDb_;
    if (t.empty() || sampleRate_ <= 0.0f) return std::numeric_limits<float>::quiet_NaN();
    const double frac = (hz - centerHz_) / static_cast<double>(sampleRate_) + 0.5;
    const auto bin = static_cast<long long>(frac * static_cast<double>(t.size()));
    if (bin < 0 || bin >= static_cast<long long>(t.size())) return std::numeric_limits<float>::quiet_NaN();
    return t[static_cast<std::size_t>(bin)];
}

// In-band C/N: mean power over the middle 80 % of the expected signal bandwidth against the median
// of the bins clearly outside it (skipping the outer 5 % of the band, where the anti-alias filter
// rolls off). Rough, but it reads the same way as SatDump's SNR box.
void SpectrumWidget::measureCn() {
    cnDb_ = std::numeric_limits<float>::quiet_NaN();
    const std::size_t n = avgDb_.size();
    if (n < 64 || signalBwHz_ <= 0.0 || signalBwHz_ >= 0.9 * static_cast<double>(sampleRate_)) return;
    const double binHz = static_cast<double>(sampleRate_) / static_cast<double>(n);
    const double half = signalBwHz_ / 2.0;
    double inSum = 0.0;
    int inCount = 0;
    std::vector<float> outside;
    outside.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double off = (static_cast<double>(i) + 0.5) * binHz - static_cast<double>(sampleRate_) / 2.0;
        const double a = std::abs(off);
        if (a <= half * 0.8) { inSum += std::pow(10.0, avgDb_[i] / 10.0); ++inCount; }
        else if (a >= half * 1.15 && a <= static_cast<double>(sampleRate_) * 0.45) outside.push_back(avgDb_[i]);
    }
    if (inCount < 8 || outside.size() < 16) return;
    std::nth_element(outside.begin(), outside.begin() + outside.size() / 2, outside.end());
    const double noise = std::pow(10.0, outside[outside.size() / 2] / 10.0);
    const double ratio = inSum / inCount / noise - 1.0;   // signal + noise over noise, minus the noise
    cnDb_ = ratio > 1e-3 ? static_cast<float>(10.0 * std::log10(ratio)) : -30.0f;
}

float SpectrumWidget::peakOverFloorDb(double offsetHz, double spanHz) const {
    const std::size_t n = avgDb_.size();
    if (n < 64 || sampleRate_ <= 0.0f) return std::numeric_limits<float>::quiet_NaN();
    const double binHz = static_cast<double>(sampleRate_) / static_cast<double>(n);
    float peak = -1e30f;
    std::vector<float> all;
    all.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
        const double off = (static_cast<double>(i) + 0.5) * binHz - static_cast<double>(sampleRate_) / 2.0;
        if (std::abs(off) <= static_cast<double>(sampleRate_) * 0.45) all.push_back(avgDb_[i]);
        if (std::abs(off - offsetHz) <= spanHz / 2.0) peak = std::max(peak, avgDb_[i]);
    }
    if (all.size() < 32 || peak < -1e29f) return std::numeric_limits<float>::quiet_NaN();
    std::nth_element(all.begin(), all.begin() + all.size() / 2, all.end());
    return peak - all[all.size() / 2];
}

void SpectrumWidget::setSignalBandwidth(double hz) {
    if (std::abs(hz - signalBwHz_) < 1.0) return;
    signalBwHz_ = hz;
    update();
}

void SpectrumWidget::setCenterFrequency(double hz) {
    if (std::abs(hz - centerHz_) < 0.5) return;
    centerHz_ = hz;
    update();
}

void SpectrumWidget::setSpectrum(const std::vector<float>& db, float sampleRate) {
    if (db.empty() || freezeAction_->isChecked()) return;
    sampleRate_ = sampleRate;
    if (powerSum_.size() != db.size()) {
        powerSum_.assign(db.size(), 0.0f);
        powerHistory_.clear();
        avgDb_.assign(db.size(), -110.0f);
        shownDb_ = db;   // a new FFT size starts from the frame itself, not a glide from nothing
    }

    std::vector<float> power(db.size());
    for (std::size_t i = 0; i < db.size(); ++i) {
        power[i] = std::pow(10.0f, db[i] / 10.0f);
        powerSum_[i] += power[i];
    }
    powerHistory_.push_back(std::move(power));
    while (powerHistory_.size() > static_cast<std::size_t>(averageCount_)) {
        const auto& old = powerHistory_.front();
        for (std::size_t i = 0; i < powerSum_.size(); ++i) powerSum_[i] -= old[i];
        powerHistory_.pop_front();
    }
    const float inv = 1.0f / static_cast<float>(std::max<std::size_t>(1, powerHistory_.size()));
    for (std::size_t i = 0; i < avgDb_.size(); ++i)
        avgDb_[i] = 10.0f * std::log10(std::max(powerSum_[i] * inv, 1e-14f));
    if (peakAction_->isChecked()) {
        if (peakDb_.size() != avgDb_.size()) peakDb_ = avgDb_;
        for (std::size_t i = 0; i < avgDb_.size(); ++i) peakDb_[i] = std::max(peakDb_[i], avgDb_[i]);
    }
    measureCn();

    if (autoRange_) {
        // As SatDump does: the noise floor near the bottom (dark in the waterfall), the strongest
        // signal near the top. The floor is the 10th percentile, so a wide signal does not lift it.
        std::vector<float> sorted(db);
        std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 10, sorted.end());
        const float floorDb = sorted[sorted.size() / 10];
        const float peak = *std::max_element(db.begin(), db.end());
        const float lo = std::floor((floorDb - 8.0f) / 5.0f) * 5.0f;
        const float hi = std::max(lo + 40.0f, std::ceil((peak + 6.0f) / 5.0f) * 5.0f);
        autoRange_ = false;
        setRange(lo, hi);
    }

    // History ring, newest first, reduced to at most kMaxColumns columns.
    const int bins = static_cast<int>(db.size());
    const int cols = std::min(bins, kMaxColumns);
    if (cols != columns_) {
        columns_ = cols;
        history_.assign(static_cast<std::size_t>(cols) * kRows, lowDb_);
        rowTimeMs_.assign(kRows, 0);
        newestRow_ = -1;
        rowsFilled_ = 0;
        rebuildPending_ = true;
    }
    newestRow_ = newestRow_ < 0 ? 0 : (newestRow_ + kRows - 1) % kRows;
    rowsFilled_ = std::min(rowsFilled_ + 1, kRows);
    if (!clock_.isValid()) clock_.start();
    if (rowTimeMs_.size() == static_cast<std::size_t>(kRows)) rowTimeMs_[newestRow_] = clock_.elapsed();
    float* row = history_.data() + static_cast<std::size_t>(newestRow_) * cols;
    for (int c = 0; c < cols; ++c) {
        const int b0 = static_cast<int>(static_cast<long long>(c) * bins / cols);
        const int b1 = std::max(b0 + 1, static_cast<int>(static_cast<long long>(c + 1) * bins / cols));
        float m = db[b0];
        for (int b = b0 + 1; b < b1; ++b) m = std::max(m, db[b]);
        row[c] = m;
    }
    // Scroll the screen-resolution waterfall by one row and colour only that row.
    if (!rebuildPending_ && !waterfall_.isNull()) {
        imageNewest_ = (imageNewest_ + waterfall_.height() - 1) % waterfall_.height();
        renderWaterfallRow(imageNewest_, row);
    }

    haveSpectrum_ = true;
    traceSettled_ = false;
    if (isVisible() && !animation_.isActive()) animation_.start();
}

void SpectrumWidget::setAverageCount(int count) {
    averageCount_ = std::clamp(count, 1, 128);
    while (powerHistory_.size() > static_cast<std::size_t>(averageCount_)) {
        const auto& old = powerHistory_.front();
        for (std::size_t i = 0; i < powerSum_.size(); ++i) powerSum_[i] -= old[i];
        powerHistory_.pop_front();
    }
    update();
}

// One animation step: move the drawn trace part of the way to the averaged spectrum. Repaints only
// while something moves, so an idle receiver costs nothing.
void SpectrumWidget::animate() {
    const double dt = std::clamp(static_cast<double>(lastFrame_.restart()), 1.0, 100.0);
    if (!haveSpectrum_ || traceSettled_) return;
    if (shownDb_.size() != avgDb_.size()) shownDb_ = avgDb_;
    const float a = static_cast<float>(1.0 - std::exp(-dt / kSmoothingMs));
    float moved = 0.0f;
    for (std::size_t i = 0; i < shownDb_.size(); ++i) {
        const float d = avgDb_[i] - shownDb_[i];
        shownDb_[i] += a * d;
        moved = std::max(moved, std::abs(d));
    }
    traceSettled_ = moved < 0.05f;
    update();
}

void SpectrumWidget::showEvent(QShowEvent* e) {
    QWidget::showEvent(e);
    lastFrame_.restart();
    animation_.start();
}

void SpectrumWidget::hideEvent(QHideEvent* e) {
    animation_.stop();
    QWidget::hideEvent(e);
}

void SpectrumWidget::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    rebuildPending_ = true;
}

// ---- waterfall -------------------------------------------------------------------------------

void SpectrumWidget::ensureWaterfallImage() {
    const QRectF wf = layout().wf;
    const QSize size(std::max(1, static_cast<int>(std::lround(wf.width()))),
                     std::max(1, static_cast<int>(std::lround(wf.height()))));
    if (waterfall_.size() != size) {
        waterfall_ = QImage(size, QImage::Format_RGB32);
        rebuildPending_ = true;
    }
}

void SpectrumWidget::renderWaterfallRow(int imageRow, const float* values) {
    auto* out = reinterpret_cast<QRgb*>(waterfall_.scanLine(imageRow));
    const int w = waterfall_.width();
    const float scale = 255.0f / std::max(highDb_ - lowDb_, 1e-3f);
    for (int x = 0; x < w; ++x) {
        int c0 = static_cast<int>(static_cast<long long>(x) * columns_ / w);
        int c1 = std::max(c0 + 1, static_cast<int>(static_cast<long long>(x + 1) * columns_ / w));
        c0 = std::min(c0, columns_ - 1);
        float m = values[c0];
        for (int c = c0 + 1; c < c1 && c < columns_; ++c) m = std::max(m, values[c]);
        out[x] = lut_[std::clamp(static_cast<int>((m - lowDb_) * scale), 0, 255)];
    }
}

void SpectrumWidget::rebuildWaterfall() {
    ensureWaterfallImage();
    if (!rebuildPending_) return;
    rebuildPending_ = false;
    waterfall_.fill(lut_[0]);
    imageNewest_ = 0;
    if (columns_ == 0 || newestRow_ < 0) return;
    const int rows = std::min(rowsFilled_, waterfall_.height());
    for (int i = 0; i < rows; ++i) {
        const int ring = (newestRow_ + i) % kRows;
        renderWaterfallRow(i, history_.data() + static_cast<std::size_t>(ring) * columns_);
    }
}

void SpectrumWidget::setRange(float lo, float hi) {
    lowDb_ = lo;
    highDb_ = hi;
    rebuildPending_ = true;   // recoloured once, at the next paint
    update();
}

// ---- geometry and cut-off handles ------------------------------------------------------------

SpectrumWidget::Layout SpectrumWidget::layout() const {
    const double left = kLeftAxis;
    const double right = width() - kRightMargin - kStripWidth - kStripGap;
    const double plotH = std::max(60.0, static_cast<double>(height() - kInfoHeight - kFreqHeight - 2));
    const double specH = std::round(plotH * kSpectrumShare);
    Layout l;
    l.spec = QRectF(left, kInfoHeight, right - left, specH);
    l.wf = QRectF(left, kInfoHeight + specH + kFreqHeight, right - left, plotH - specH);
    l.strip = QRectF(width() - kRightMargin - kStripWidth, kInfoHeight, kStripWidth, specH);
    return l;
}

double SpectrumWidget::stripY(float db, const QRectF& strip) const {
    return strip.top() + (kStripTopDb - db) / (kStripTopDb - kStripBottomDb) * strip.height();
}

float SpectrumWidget::stripDb(double y, const QRectF& strip) const {
    const double t = std::clamp((y - strip.top()) / std::max(1.0, strip.height()), 0.0, 1.0);
    return static_cast<float>(kStripTopDb - t * (kStripTopDb - kStripBottomDb));
}

SpectrumWidget::Handle SpectrumWidget::handleAt(const QPointF& pos) const {
    const QRectF strip = layout().strip;
    if (pos.x() < strip.left() - 6 || pos.x() > strip.right() + 6) return Handle::None;
    const double yh = stripY(highDb_, strip);
    const double yl = stripY(lowDb_, strip);
    if (std::abs(pos.y() - yh) <= 6) return Handle::Upper;
    if (std::abs(pos.y() - yl) <= 6) return Handle::Lower;
    return Handle::None;
}

double SpectrumWidget::frequencyAt(double x, const QRectF& r) const {
    const double t = (x - r.left()) / std::max(1.0, r.width());
    return centerHz_ + (t - 0.5) * static_cast<double>(sampleRate_);
}

QString SpectrumWidget::frequencyLabel(double hz, double stepHz) const {
    const int decimals = stepHz >= 1e6 ? 0 : stepHz >= 1e5 ? 1 : stepHz >= 1e4 ? 2 : 3;
    if (centerHz_ > 0.0) return QString::number(hz / 1e6, 'f', decimals);
    return QStringLiteral("%1%2").arg(hz > 0 ? QStringLiteral("+") : QString()).arg(hz / 1e6, 0, 'f', decimals);
}

void SpectrumWidget::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return QWidget::mousePressEvent(e);
    const QPointF pos = e->position();
    pressPos_ = pos;
    drag_ = handleAt(pos);
    const QRectF strip = layout().strip.adjusted(-6, -6, 6, 6);
    if (drag_ == Handle::None && strip.contains(pos)) {
        // Click on the track: move the nearer handle there and keep dragging it.
        const QRectF s = layout().strip;
        drag_ = std::abs(pos.y() - stripY(highDb_, s)) <= std::abs(pos.y() - stripY(lowDb_, s)) ? Handle::Upper : Handle::Lower;
        mouseMoveEvent(e);
    }
}

void SpectrumWidget::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    hoverPos_ = pos;
    if (drag_ == Handle::None) {
        const Layout l = layout();
        const bool onStrip = l.strip.adjusted(-6, -6, 6, 6).contains(pos);
        setCursor(handleAt(pos) != Handle::None ? Qt::SizeVerCursor : (onStrip ? Qt::PointingHandCursor : Qt::CrossCursor));
        hover_ = !onStrip && (l.spec.contains(pos) || l.wf.contains(pos));
        update();
        return;
    }
    const float db = std::round(stripDb(pos.y(), layout().strip));
    autoRange_ = false;
    if (drag_ == Handle::Upper) setRange(lowDb_, std::clamp(db, lowDb_ + kMinSpanDb, kStripTopDb));
    else setRange(std::clamp(db, kStripBottomDb, highDb_ - kMinSpanDb), highDb_);
}

void SpectrumWidget::mouseReleaseEvent(QMouseEvent* e) {
    // A click (not a handle drag) in the plot drops a marker; a third starts a new pair.
    const Layout l = layout();
    if (e->button() == Qt::LeftButton && drag_ == Handle::None
        && (l.spec.contains(e->position()) || l.wf.contains(e->position()))
        && (e->position() - pressPos_).manhattanLength() < 4) {
        if (markerHz_.size() >= 2) markerHz_.clear();
        markerHz_.push_back(frequencyAt(e->position().x(), l.spec));
        update();
    }
    drag_ = Handle::None;
    QWidget::mouseReleaseEvent(e);
}

void SpectrumWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (layout().strip.adjusted(-6, -6, 6, 6).contains(e->position())) {
        autoRange_ = true;   // taken from the next frame
        update();
        return;
    }
    QWidget::mouseDoubleClickEvent(e);
}

void SpectrumWidget::leaveEvent(QEvent* e) {
    hover_ = false;
    update();
    QWidget::leaveEvent(e);
}

// ---- painting --------------------------------------------------------------------------------

void SpectrumWidget::paintEvent(QPaintEvent*) {
    rebuildWaterfall();
    QPainter p(this);
    p.fillRect(rect(), kBackground);
    const Layout l = layout();
    const QRectF& r = l.spec;
    const float topDb = highDb_;
    const float bottomDb = lowDb_;

    QFont small = font();
    small.setPointSizeF(8.5);
    p.setFont(small);
    p.setPen(kAxisText);
    {
        QString right;
        if (std::isfinite(cnDb_)) right = QStringLiteral("C/N %1 dB").arg(cnDb_, 0, 'f', 1);
        if (freezeAction_->isChecked()) right = QStringLiteral("FROZEN") + (right.isEmpty() ? QString() : QStringLiteral("   ") + right);
        const QFontMetrics fm(small);
        const QRectF info(r.left(), 0, r.width(), kInfoHeight - 4);
        const double rightW = right.isEmpty() ? 0.0 : fm.horizontalAdvance(right) + 12.0;
        if (!right.isEmpty()) {
            p.save();
            p.setPen(freezeAction_->isChecked() ? QColor(240, 184, 60) : kAxisText);
            p.drawText(info, Qt::AlignRight | Qt::AlignVCenter, right);
            p.restore();
        }
        const QString left = QStringLiteral("%1 MS/s  ·  avg %2  ·  %3 … %4 dB%5")
            .arg(sampleRate_ / 1e6, 0, 'f', 3).arg(averageCount_)
            .arg(bottomDb, 0, 'f', 0).arg(topDb, 0, 'f', 0)
            .arg(autoRange_ ? QStringLiteral(" (auto)") : QString());
        p.drawText(info.adjusted(0, 0, -rightW, 0), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(left, Qt::ElideRight, static_cast<int>(info.width() - rightW)));
    }

    // Frequency grid: round frequencies, about eight lines whatever the span.
    const double span = static_cast<double>(sampleRate_);
    const double fStep = niceStep(span, 8);
    const double fLo = centerHz_ - span / 2.0;
    const double first = std::ceil(fLo / fStep) * fStep;
    std::vector<double> gridX;
    for (double f = first; f <= fLo + span + 1e-6; f += fStep)
        gridX.push_back(r.left() + (f - fLo) / span * r.width());

    p.setPen(QPen(kGrid, 1));
    for (double x : gridX) p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
    const float dbSpan = std::max(topDb - bottomDb, 1e-3f);
    const float step = dbSpan <= 24.0f ? 5.0f : dbSpan <= 48.0f ? 10.0f : dbSpan <= 96.0f ? 20.0f : dbSpan <= 150.0f ? 30.0f : 40.0f;
    auto yOfDb = [&](float v) { return r.bottom() - (v - bottomDb) / dbSpan * r.height(); };
    for (float v = std::ceil(bottomDb / step) * step; v <= topDb + 0.01f; v += step) {
        p.setPen(QPen(kGrid, 1));
        p.drawLine(QPointF(r.left(), yOfDb(v)), QPointF(r.right(), yOfDb(v)));
        if (yOfDb(v) > r.bottom() - 7.0 || yOfDb(v) < r.top() + 5.0) continue;   // keep labels off the frequency row
        p.setPen(kAxisText);
        p.drawText(QRectF(2, yOfDb(v) - 9, kLeftAxis - 6, 18), Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'f', 0));
    }

    // Frequency labels between the two halves.
    p.setPen(kAxisText);
    {
        int i = 0;
        for (double f = first; f <= fLo + span + 1e-6; f += fStep, ++i) {
            const double x = r.left() + (f - fLo) / span * r.width();
            QRectF box(x - 50, r.bottom() + 2, 100, kFreqHeight - 4);
            Qt::Alignment a = Qt::AlignHCenter;
            if (x - r.left() < 30) { a = Qt::AlignLeft; box.moveLeft(x); }
            if (r.right() - x < 30) { a = Qt::AlignRight; box.moveRight(x); }
            p.drawText(box, a | Qt::AlignVCenter, frequencyLabel(centerHz_ > 0.0 ? f : f - centerHz_, fStep));
        }
        p.setPen(QColor(110, 115, 122));
        p.drawText(QRectF(2, r.bottom() + 2, kLeftAxis - 6, kFreqHeight - 4), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("MHz"));
    }

    // Waterfall: two unscaled blits of the screen-resolution ring.
    if (!waterfall_.isNull()) {
        const int h = waterfall_.height();
        const int top = h - imageNewest_;
        const QPointF origin = l.wf.topLeft();
        p.drawImage(origin, waterfall_, QRectF(0, imageNewest_, waterfall_.width(), top));
        if (imageNewest_ > 0)
            p.drawImage(origin + QPointF(0, top), waterfall_, QRectF(0, 0, waterfall_.width(), imageNewest_));
    }

    // Cut-off slider strip.
    {
        const QRectF& s = l.strip;
        const double yh = stripY(highDb_, s);
        const double yl = stripY(lowDb_, s);
        p.fillRect(QRectF(s.center().x() - 2, s.top(), 4, s.height()), QColor(43, 45, 49));
        p.fillRect(QRectF(s.center().x() - 2, yh, 4, yl - yh), QColor(74, 143, 208));
        p.setRenderHint(QPainter::Antialiasing, true);
        for (const double y : {yh, yl}) {
            p.setPen(QPen(QColor(28, 29, 32), 1));
            p.setBrush(QColor(214, 216, 219));
            p.drawRect(QRectF(s.left(), y - 3, s.width(), 6));
        }
        // Waterfall colour bar beside the waterfall, matching the cut-offs.
        const QRectF bar(s.left() + 2, l.wf.top(), s.width() - 4, l.wf.height());
        QLinearGradient g(bar.bottomLeft(), bar.topLeft());
        for (int i = 0; i <= 8; ++i) g.setColorAt(i / 8.0, QColor(lut_[std::min(255, i * 32)]));
        p.setPen(Qt::NoPen);
        p.setBrush(g);
        p.drawRect(bar);
        p.setRenderHint(QPainter::Antialiasing, false);
    }

    // Waterfall time scale: seconds before now, at the left of the waterfall.
    if (rowsFilled_ > 1 && clock_.isValid() && rowTimeMs_.size() == static_cast<std::size_t>(kRows)) {
        const qint64 now = clock_.elapsed();
        const int rows = std::min(rowsFilled_, static_cast<int>(l.wf.height()));
        const double span = (now - rowTimeMs_[(newestRow_ + rows - 1) % kRows]) / 1000.0;
        const double tick = span <= 12 ? 2 : span <= 30 ? 5 : span <= 60 ? 10 : span <= 180 ? 30 : 60;
        p.setPen(kAxisText);
        double next = tick;
        for (int i = 0; i < rows; ++i) {
            const double age = (now - rowTimeMs_[(newestRow_ + i) % kRows]) / 1000.0;
            if (age >= next) {
                const double y = l.wf.top() + i;
                p.drawLine(QPointF(l.wf.left() - 4, y), QPointF(l.wf.left(), y));
                p.drawText(QRectF(2, y - 8, kLeftAxis - 8, 16), Qt::AlignRight | Qt::AlignVCenter,
                           QStringLiteral("−%1 s").arg(next, 0, 'f', 0));
                next += tick;
            }
        }
    }

    if (!haveSpectrum_) {
        p.setPen(QColor(110, 115, 122));
        p.drawText(r, Qt::AlignCenter, QStringLiteral("Waiting for IQ samples…"));
        p.drawText(l.wf, Qt::AlignCenter, QStringLiteral("Waterfall starts with the receiver"));
        return;
    }

    // Trace: one point per pixel column (the maximum of the bins in it), filled down to the floor.
    const std::vector<float>& trace = shownDb_.size() == avgDb_.size() ? shownDb_ : avgDb_;
    const std::size_t n = trace.size();
    const int pixels = std::max(2, static_cast<int>(r.width()));
    auto yOf = [&](float v) { return r.bottom() - (std::clamp(v, bottomDb, topDb) - bottomDb) / dbSpan * r.height(); };
    QPolygonF line;
    if (n <= static_cast<std::size_t>(pixels)) {
        line.reserve(static_cast<int>(n));
        for (std::size_t i = 0; i < n; ++i)
            line << QPointF(r.left() + r.width() * i / std::max<std::size_t>(1, n - 1), yOf(trace[i]));
    } else {
        line.reserve(pixels);
        for (int x = 0; x < pixels; ++x) {
            const std::size_t b0 = static_cast<std::size_t>(static_cast<unsigned long long>(x) * n / pixels);
            const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(static_cast<unsigned long long>(x + 1) * n / pixels));
            float m = trace[b0];
            for (std::size_t b = b0 + 1; b < b1 && b < n; ++b) m = std::max(m, trace[b]);
            line << QPointF(r.left() + r.width() * x / (pixels - 1), yOf(m));
        }
    }
    p.save();
    p.setClipRect(r);
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen tracePen(kTrace);
    tracePen.setWidthF(0.0);   // cosmetic one-pixel line: the fast path
    tracePen.setCosmetic(true);
    p.setPen(tracePen);
    p.setBrush(Qt::NoBrush);
    if (peakAction_->isChecked() && peakDb_.size() == n) {
        QPolygonF peak;
        peak.reserve(line.size());
        const int count = static_cast<int>(line.size());
        for (int x = 0; x < count; ++x) {
            const std::size_t b0 = static_cast<std::size_t>(static_cast<unsigned long long>(x) * n / count);
            const std::size_t b1 = std::max(b0 + 1, static_cast<std::size_t>(static_cast<unsigned long long>(x + 1) * n / count));
            float m = peakDb_[std::min(b0, n - 1)];
            for (std::size_t b = b0 + 1; b < b1 && b < n; ++b) m = std::max(m, peakDb_[b]);
            peak << QPointF(line[x].x(), yOf(m));
        }
        QPen peakPen(QColor(240, 184, 60, 200));
        peakPen.setCosmetic(true);
        p.setPen(peakPen);
        p.drawPolyline(peak);
        p.setPen(tracePen);
    }
    p.drawPolyline(line);
    p.restore();

    // Markers: numbered lines through both halves, read-out box top left with the difference.
    if (!markerHz_.empty()) {
        const double fLo2 = centerHz_ - static_cast<double>(sampleRate_) / 2.0;
        QStringList lines;
        for (std::size_t i = 0; i < markerHz_.size(); ++i) {
            const double x = r.left() + (markerHz_[i] - fLo2) / static_cast<double>(sampleRate_) * r.width();
            if (x < r.left() || x > r.right()) continue;
            const QColor c = i == 0 ? QColor(120, 200, 255) : QColor(255, 140, 200);
            p.setPen(QPen(c, 1));
            p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
            p.setPen(QPen(QColor(c.red(), c.green(), c.blue(), 110), 1));
            p.drawLine(QPointF(x, l.wf.top()), QPointF(x, l.wf.bottom()));
            const float lv = levelAt(markerHz_[i]);
            const double yy = yOf(std::isfinite(lv) ? lv : bottomDb);
            p.fillRect(QRectF(x - 3, yy - 3, 6, 6), c);
            p.setPen(c);
            p.drawText(QRectF(x + 3, r.top() + 2, 30, 14), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("M%1").arg(i + 1));
            lines << QStringLiteral("M%1  %2 MHz  %3 dB").arg(i + 1)
                         .arg((centerHz_ > 0.0 ? markerHz_[i] : markerHz_[i] - centerHz_) / 1e6, 0, 'f', 4)
                         .arg(lv, 0, 'f', 1);
        }
        if (markerHz_.size() == 2) {
            const double df = markerHz_[1] - markerHz_[0];
            const float dl = levelAt(markerHz_[1]) - levelAt(markerHz_[0]);
            const QString dfText = std::abs(df) >= 1e6 ? QStringLiteral("%1 MHz").arg(df / 1e6, 0, 'f', 4)
                                                       : QStringLiteral("%1 kHz").arg(df / 1e3, 0, 'f', 2);
            lines << QStringLiteral("Δ   %1  %2 dB").arg(dfText).arg(dl, 0, 'f', 1);
        }
        if (!lines.isEmpty()) {
            const QFontMetrics fm(small);
            int w = 0;
            for (const QString& s : lines) w = std::max(w, fm.horizontalAdvance(s));
            const QRectF box(r.left() + 6, r.top() + 18, w + 14, lines.size() * fm.height() + 8);
            p.setPen(QPen(QColor(69, 72, 78), 1));
            p.setBrush(QColor(20, 21, 23, 225));
            p.drawRect(box);
            p.setPen(QColor(214, 216, 219));
            p.drawText(box.adjusted(7, 4, -4, -4), Qt::AlignLeft | Qt::AlignTop, lines.join(QLatin1Char('\n')));
        }
    }

    // Expected signal edges (symbol rate x (1 + roll-off)) around the centre.
    if (signalBwHz_ > 0.0 && signalBwHz_ < static_cast<double>(sampleRate_)) {
        const double half = signalBwHz_ / 2.0 / static_cast<double>(sampleRate_) * r.width();
        const double cx = r.center().x();
        p.setPen(QPen(QColor(74, 143, 208, 170), 1, Qt::DashLine));
        for (double x : {cx - half, cx + half}) p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.fillRect(QRectF(cx - half, r.top(), 2 * half, 3), QColor(74, 143, 208, 150));
    }

    // Hover read-out: a cursor line through both halves and the frequency / level under it.
    if (hover_) {
        const double x = std::clamp(hoverPos_.x(), r.left(), r.right());
        p.setPen(QPen(QColor(236, 238, 240, 150), 1, Qt::DashLine));
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(x, l.wf.top()), QPointF(x, l.wf.bottom()));
        const double hz = frequencyAt(x, r);
        const std::size_t bin = std::min(n - 1, static_cast<std::size_t>(std::max(0.0, (x - r.left()) / r.width() * n)));
        const QString text = QStringLiteral("%1 MHz   %2 dB")
            .arg(centerHz_ > 0.0 ? hz / 1e6 : (hz - centerHz_) / 1e6, 0, 'f', 4)
            .arg(trace[bin], 0, 'f', 1);
        const QFontMetrics fm(small);
        QRectF box(x + 10, r.top() + 6, fm.horizontalAdvance(text) + 14, fm.height() + 8);
        if (box.right() > r.right()) box.moveRight(x - 10);
        p.setPen(QPen(QColor(69, 72, 78), 1));
        p.setBrush(QColor(35, 36, 39, 235));
        p.drawRect(box);
        p.setPen(QColor(236, 238, 240));
        p.drawText(box, Qt::AlignCenter, text);
    }
}
