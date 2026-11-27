#pragma once
// A small strip chart of the symbol SNR over the last minute, drawn under the constellation so a
// fade, a dropout or a slow drift is visible at a glance. Fed twice a second by the main window.
#include <QPainter>
#include <QPainterPath>
#include <QWidget>
#include <algorithm>
#include <cmath>
#include <deque>
#include "snrmeter.h"

class SnrTrend : public QWidget {
public:
    explicit SnrTrend(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(64);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
    // NaN marks "no signal" (receiver stopped): it breaks the line rather than drawing a zero.
    void push(float db) {
        values_.push_back(db);
        while (values_.size() > kSamples) values_.pop_front();
        update();
    }
    void clear() { values_.clear(); update(); }
    void setZones(const nadirstation::ui::SnrZones& z) { zones_ = z; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        const QRectF frame = rect().adjusted(0.5, 0.5, -0.5, -0.5);
        p.fillRect(rect(), QColor(20, 21, 23));
        p.setPen(QColor(47, 49, 53));
        p.drawRect(frame);
        const QRectF plot = frame.adjusted(30, 6, -6, -14);

        float lo = 0.0f, hi = 10.0f;
        bool any = false;
        for (float v : values_)
            if (std::isfinite(v)) { lo = any ? std::min(lo, v) : v; hi = any ? std::max(hi, v) : v; any = true; }
        // At least a 4 dB window centred on the data, on whole decibels.
        const float mid = (lo + hi) / 2.0f, half = std::max(hi - lo, 4.0f) / 2.0f + 0.5f;
        if (any) {
            lo = std::floor(mid - half);
            hi = std::ceil(mid + half);
        }

        QFont small = font();
        small.setPointSizeF(7.5);
        p.setFont(small);
        for (float v : {lo, (lo + hi) / 2.0f, hi}) {
            const double y = plot.bottom() - (v - lo) / (hi - lo) * plot.height();
            p.setPen(QColor(44, 46, 50));
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(QColor(110, 115, 122));
            p.drawText(QRectF(2, y - 7, 25, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(v, 'f', 0));
        }
        p.drawText(QRectF(plot.left(), plot.bottom() + 1, plot.width(), 12), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("−60 s"));
        p.drawText(QRectF(plot.left(), plot.bottom() + 1, plot.width(), 12), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("now · dB"));
        if (!any) {
            p.drawText(plot, Qt::AlignCenter, QStringLiteral("No signal"));
            return;
        }
        // Faint zone bands behind the line, and each step of the line in its zone's colour.
        p.setPen(Qt::NoPen);
        const float edges[] = {-1000.0f, zones_.weak, zones_.good, zones_.strongAt(), 1000.0f};
        const QColor cols[] = {nadirstation::ui::SnrZones::red(), nadirstation::ui::SnrZones::amber(),
                               nadirstation::ui::SnrZones::green(), nadirstation::ui::SnrZones::teal()};
        auto yOf = [&](float v) { return plot.bottom() - (std::clamp(v, lo, hi) - lo) / (hi - lo) * plot.height(); };
        for (int z = 0; z < 4; ++z) {
            const double y0 = yOf(edges[z + 1]), y1 = yOf(edges[z]);
            if (y1 - y0 < 0.5) continue;
            QColor band = cols[z];
            band.setAlpha(22);
            p.setBrush(band);
            p.drawRect(QRectF(plot.left(), y0, plot.width(), y1 - y0));
        }
        p.setRenderHint(QPainter::Antialiasing, true);
        const std::size_t n = values_.size();
        QPointF prev;
        bool open = false;
        for (std::size_t i = 0; i < n; ++i) {
            const float v = values_[i];
            const double x = plot.right() - (static_cast<double>(n - 1 - i) / (kSamples - 1)) * plot.width();
            if (!std::isfinite(v)) { open = false; continue; }
            const QPointF pt(x, yOf(v));
            if (open) {
                QPen pen(zones_.colour(v));
                pen.setWidthF(1.6);
                p.setPen(pen);
                p.drawLine(prev, pt);
            }
            prev = pt;
            open = true;
        }
    }

private:
    static constexpr std::size_t kSamples = 120;   // 60 s at two samples a second
    std::deque<float> values_;
    nadirstation::ui::SnrZones zones_;
};
