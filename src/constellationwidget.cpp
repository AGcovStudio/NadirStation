#include "constellationwidget.h"
#include <QPainter>
#include <algorithm>
#include <cmath>

void ConstellationWidget::setAnnotation(const QString& modulation, const QString& detail) {
    modulation_ = modulation;
    detail_ = detail;
    update();
}

ConstellationWidget::ConstellationWidget(QWidget* parent) : QWidget(parent) {
    setMinimumSize(160, 160);
}

void ConstellationWidget::setPoints(const std::vector<std::complex<float>>& points) {
    points_ = points;
    if (points_.size() > 1600) points_.erase(points_.begin(), points_.end() - 1600);
    update();
}

void ConstellationWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.fillRect(rect(), QColor(20, 21, 23));
    p.setRenderHint(QPainter::Antialiasing, true);

    p.setPen(QColor(214, 216, 219));
    p.setFont(QFont(QStringLiteral("Bahnschrift"), 10, QFont::DemiBold));
    p.drawText(QRectF(10, 8, width() - 20, 22), Qt::AlignLeft | Qt::AlignVCenter, p.fontMetrics().elidedText(modulation_, Qt::ElideRight, width() - 20));

    const int side = std::max(120, std::min(width() - 16, height() - 50));
    const QRectF r((width() - side) * 0.5, 32, side, side);

    p.setPen(QPen(QColor(53, 55, 60), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    for (int i = 1; i < 4; ++i) {
        const double x = r.left() + r.width() * i / 4.0;
        const double y = r.top() + r.height() * i / 4.0;
        p.drawLine(QPointF(x, r.top()), QPointF(x, r.bottom()));
        p.drawLine(QPointF(r.left(), y), QPointF(r.right(), y));
    }
    p.setPen(QPen(QColor(110, 115, 122), 1));
    p.drawLine(QPointF(r.center().x(), r.top()), QPointF(r.center().x(), r.bottom()));
    p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));

    if (points_.empty()) return;

    const bool qpsk=modulation_.contains(QStringLiteral("QPSK"),Qt::CaseInsensitive);
    const float lim=qpsk?1.8f:1.35f;
    if(qpsk){
        p.setPen(QPen(QColor(136,152,163,110),1));
        for(const float i:{-1.f,1.f})for(const float q:{-1.f,1.f}){
            const QPointF target(r.center().x()+i/lim*r.width()*0.47,
                                 r.center().y()-q/lim*r.height()*0.47);
            p.drawLine(QPointF(target.x()-4,target.y()),QPointF(target.x()+4,target.y()));
            p.drawLine(QPointF(target.x(),target.y()-4),QPointF(target.x(),target.y()+4));
        }
    }
    p.setPen(Qt::NoPen);
    QColor dot = tint_;
    dot.setAlpha(150);
    p.setBrush(dot);
    for (const auto& z : points_) {
        const float x = std::clamp(z.real(), -lim, lim);
        const float y = std::clamp(z.imag(), -lim, lim);
        const QPointF pt(r.center().x() + x / lim * r.width() * 0.47,
                         r.center().y() - y / lim * r.height() * 0.47);
        p.drawEllipse(pt, side > 260 ? 2.1 : 1.7, side > 260 ? 2.1 : 1.7);
    }

    p.setPen(QColor(154, 158, 165));
    p.setFont(QFont(QStringLiteral("Bahnschrift"), 8));
    p.drawText(QRectF(8, r.bottom() + 2, width() - 16, 16), Qt::AlignCenter, p.fontMetrics().elidedText(detail_, Qt::ElideRight, width() - 16));
}
