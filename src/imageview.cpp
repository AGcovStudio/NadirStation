#include "imageview.h"
#include <QFileDialog>
#include <QGraphicsPixmapItem>
#include <QMouseEvent>
#include <QScrollBar>
#include <QWheelEvent>
#include <algorithm>
#include <cstdint>
#include <QTransform>

namespace {
// Box-filtered reduction by an integer factor for 16-bit grey images (a 11000 x 11000 image otherwise has to be
// converted to 32 bits just to be resampled).
QImage reduceGray16(const QImage& src, int k) {
    const int w = (src.width() + k - 1) / k, h = (src.height() + k - 1) / k;
    QImage out(w, h, QImage::Format_Grayscale16);
    if (out.isNull()) return {};
    for (int y = 0; y < h; ++y) {
        auto* dst = reinterpret_cast<uint16_t*>(out.scanLine(y));
        const int y1 = std::min(src.height(), (y + 1) * k);
        for (int x = 0; x < w; ++x) {
            const int x1 = std::min(src.width(), (x + 1) * k);
            uint32_t sum = 0, n = 0;
            for (int sy = y * k; sy < y1; ++sy) {
                const auto* p = reinterpret_cast<const uint16_t*>(src.constScanLine(sy));
                for (int sx = x * k; sx < x1; ++sx) { sum += p[sx]; ++n; }
            }
            dst[x] = uint16_t(n ? sum / n : 0);
        }
    }
    return out;
}
}

ImageView::ImageView(QWidget* parent) : QGraphicsView(parent) {
    setScene(&scene_);
    setBackgroundBrush(QColor(20, 21, 23));
    setRenderHint(QPainter::SmoothPixmapTransform, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setTransformationAnchor(QGraphicsView::AnchorUnderMouse);
    setResizeAnchor(QGraphicsView::AnchorViewCenter);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setSceneRect(scene_.sceneRect());
}
void ImageView::setImage(const QImage& image) {
    scene_.clear();
    lodItem_ = nullptr;
    pixmapItem_ = scene_.addPixmap(QPixmap::fromImage(image));
    hasImage_ = !image.isNull();
    // A pixmap this large is resampled on every repaint when the view is zoomed out, which makes panning crawl.
    // While the view shows less than about a third of the pixels, a reduced copy is drawn instead.
    const int longSide = std::max(image.width(), image.height());
    if (longSide > 4096) {
        const int k = (longSide + 4095) / 4096;
        const QImage small = image.format() == QImage::Format_Grayscale16
            ? reduceGray16(image, k)
            : image.scaled(image.width() / k, image.height() / k, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (!small.isNull()) {
            lodItem_ = scene_.addPixmap(QPixmap::fromImage(small));
            lodItem_->setTransform(QTransform::fromScale(double(image.width()) / small.width(),
                                                         double(image.height()) / small.height()));
            lodItem_->setVisible(false);
        }
    }
    scene_.setSceneRect(pixmapItem_ ? pixmapItem_->boundingRect() : QRectF());
    setSceneRect(scene_.sceneRect());
    fitImage();
}
void ImageView::updateLod() {
    if (!lodItem_ || !pixmapItem_) return;
    const bool zoomedOut = transform().m11() < 0.33;
    lodItem_->setVisible(zoomedOut);
    pixmapItem_->setVisible(!zoomedOut);
}
void ImageView::fitImage() { if (hasImage_) { resetTransform(); fitInView(pixmapItem_, Qt::KeepAspectRatio); updateLod(); } }
void ImageView::wheelEvent(QWheelEvent* e) {
    const double current = transform().m11();
    const double s = e->angleDelta().y() > 0 ? 1.15 : 1.0 / 1.15;
    if ((s > 1.0 && current < 16.0) || (s < 1.0 && current > 0.05))
    scale(s, s);
    updateLod();
    e->accept();
}
void ImageView::mousePressEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && hasImage_) {
        emit imageHoverLeft();
        dragging_ = true;
        lastDragPos_ = e->position().toPoint();
        viewport()->setCursor(Qt::ClosedHandCursor);
        e->accept();
        return;
    }
    QGraphicsView::mousePressEvent(e);
}
void ImageView::mouseMoveEvent(QMouseEvent* e) {
    if (dragging_) {
        const QPoint delta = e->position().toPoint() - lastDragPos_;
        lastDragPos_ = e->position().toPoint();
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        e->accept();
        return;
    }
    if (hasImage_ && pixmapItem_) emit imagePositionHovered(mapToScene(e->position().toPoint()));
    QGraphicsView::mouseMoveEvent(e);
}
void ImageView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && dragging_) {
        dragging_ = false;
        viewport()->setCursor(Qt::OpenHandCursor);
        e->accept();
        return;
    }
    QGraphicsView::mouseReleaseEvent(e);
}
void ImageView::mouseDoubleClickEvent(QMouseEvent* e) {
    Q_UNUSED(e);
    fitImage();
}
void ImageView::resizeEvent(QResizeEvent* e) { QGraphicsView::resizeEvent(e); }
void ImageView::leaveEvent(QEvent* e) {
    emit imageHoverLeft();
    QGraphicsView::leaveEvent(e);
}
