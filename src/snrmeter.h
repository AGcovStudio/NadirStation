#pragma once
// Symbol SNR read-out for the Live page: the current value in large type, coloured by how it stands
// against what this link needs, a bar with the zones, the peak, and the 1 s average.
//
// Zones (dB on this read-out, set per service with setScale):
//   below `weak`         red     -- below what the decoder needs; expect no or broken frames
//   weak .. good         amber   -- decoding, with little margin
//   good .. strong       green   -- solid (strong = good + 6 unless the link sets it)
//   above strong         teal    -- strong
#include <QColor>
#include <QPainter>
#include <QWidget>
#include <algorithm>
#include <cmath>

namespace nadirstation::ui {
struct SnrZones {
    float weak = 2.0f, good = 6.0f, strong = -1.0f;   // strong < 0: good + 6
    float strongAt() const { return strong > good ? strong : good + 6.0f; }
    static QColor red() { return QColor(229, 83, 75); }
    static QColor amber() { return QColor(224, 163, 58); }
    static QColor green() { return QColor(76, 183, 130); }
    static QColor teal() { return QColor(74, 175, 208); }
    QColor colour(float db) const {
        if (!std::isfinite(db)) return QColor(110, 115, 122);
        if (db < weak) return red();
        if (db < good) return amber();
        if (db < strongAt()) return green();
        return teal();
    }
    QString word(float db) const {
        if (!std::isfinite(db)) return QStringLiteral("No signal");
        if (db < weak) return QStringLiteral("Too weak");
        if (db < good) return QStringLiteral("Marginal");
        if (db < strongAt()) return QStringLiteral("Good");
        return QStringLiteral("Strong");
    }
};
}  // namespace nadirstation::ui

class SnrMeter : public QWidget {
public:
    // Three significant digits: 0.523, 3.12, 12.4 dB.
    static QString threeDigits(float v) {
        const float a = std::fabs(v);
        return QString::number(v, 'f', a < 1.0f ? 3 : a < 10.0f ? 2 : a < 100.0f ? 1 : 0);
    }
    explicit SnrMeter(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(92);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setScale(float weak, float good, float strong = -1.0f) { zones_.weak = weak; zones_.good = good; zones_.strong = strong; update(); }
    const nadirstation::ui::SnrZones& zones() const { return zones_; }
    void setValues(float now, float peak, float avg) { now_ = now; peak_ = peak; avg_ = avg; update(); }
    void clear() { now_ = peak_ = avg_ = std::nanf(""); update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(20, 21, 23));
        const QColor c = zones_.colour(now_);
        const double w = width();

        // Value and verdict.
        QFont big = font();
        big.setFamily(QStringLiteral("Bahnschrift"));
        big.setPointSizeF(22);
        big.setWeight(QFont::DemiBold);
        p.setFont(big);
        p.setPen(c);
        const QString value = std::isfinite(now_) ? threeDigits(now_) : QStringLiteral("—");
        const QRectF valueBox(10, 2, w * 0.6, 38);
        p.drawText(valueBox, Qt::AlignLeft | Qt::AlignVCenter, value);
        const double valueWidth = p.fontMetrics().horizontalAdvance(value);
        QFont unit = font();
        unit.setPointSizeF(9);
        p.setFont(unit);
        p.setPen(QColor(154, 158, 165));
        p.drawText(QRectF(14 + valueWidth, 16, 30, 20), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("dB"));

        QFont pillFont = font();
        pillFont.setPointSizeF(8.5);
        pillFont.setWeight(QFont::DemiBold);
        p.setFont(pillFont);
        const QString word = zones_.word(now_);
        const double pw = p.fontMetrics().horizontalAdvance(word) + 18;
        const QRectF pill(w - pw - 10, 11, pw, 20);
        QColor fill = c;
        fill.setAlpha(46);
        p.setPen(Qt::NoPen);
        p.setBrush(fill);
        p.drawRoundedRect(pill, 10, 10);
        p.setPen(c);
        p.drawText(pill, Qt::AlignCenter, word);

        // Bar: the zones dimmed, filled up to the current value, peak tick.
        const float top = std::max(zones_.good + 10.0f, std::isfinite(peak_) ? std::ceil(peak_ + 1.0f) : 0.0f);
        const QRectF bar(10, 46, w - 20, 10);
        auto xOf = [&](float db) { return bar.left() + std::clamp(db / top, 0.0f, 1.0f) * bar.width(); };
        const float edges[] = {0.0f, zones_.weak, zones_.good, zones_.strongAt(), top};
        const QColor cols[] = {nadirstation::ui::SnrZones::red(), nadirstation::ui::SnrZones::amber(),
                               nadirstation::ui::SnrZones::green(), nadirstation::ui::SnrZones::teal()};
        for (int z = 0; z < 4; ++z) {
            QColor dim = cols[z];
            dim.setAlpha(55);
            p.setBrush(dim);
            p.drawRect(QRectF(xOf(edges[z]), bar.top(), xOf(edges[z + 1]) - xOf(edges[z]), bar.height()));
        }
        if (std::isfinite(now_)) {
            for (int z = 0; z < 4; ++z) {
                const float a = edges[z], b = std::min(edges[z + 1], std::max(now_, 0.0f));
                if (b <= a) break;
                p.setBrush(cols[z]);
                p.drawRect(QRectF(xOf(a), bar.top(), xOf(b) - xOf(a), bar.height()));
            }
        }
        if (std::isfinite(peak_)) {
            p.setPen(QPen(QColor(236, 238, 240), 2));
            p.drawLine(QPointF(xOf(peak_), bar.top() - 3), QPointF(xOf(peak_), bar.bottom() + 3));
        }
        // Zone edges, labelled.
        QFont tiny = font();
        tiny.setPointSizeF(7.5);
        p.setFont(tiny);
        p.setPen(QColor(110, 115, 122));
        for (float e : {zones_.weak, zones_.good, zones_.strongAt()})
            p.drawText(QRectF(xOf(e) - 20, bar.bottom() + 2, 40, 12), Qt::AlignCenter, QString::number(e, 'g', 3));

        // Peak and average.
        p.setFont(unit);
        p.setPen(QColor(154, 158, 165));
        const QString foot = QStringLiteral("Peak %1   ·   1 s avg %2")
            .arg(std::isfinite(peak_) ? QStringLiteral("%1 dB").arg(threeDigits(peak_)) : QStringLiteral("—"),
                 std::isfinite(avg_) ? QStringLiteral("%1 dB").arg(threeDigits(avg_)) : QStringLiteral("—"));
        p.drawText(QRectF(10, 72, w - 20, 18), Qt::AlignLeft | Qt::AlignVCenter, foot);
    }

private:
    nadirstation::ui::SnrZones zones_;
    float now_ = std::nanf(""), peak_ = std::nanf(""), avg_ = std::nanf("");
};
