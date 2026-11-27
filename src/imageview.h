#pragma once
#include <QGraphicsView>
#include <QGraphicsPixmapItem>
#include <QImage>
#include <QString>

class ImageView : public QGraphicsView {
    Q_OBJECT
public:
    explicit ImageView(QWidget* parent = nullptr);
    void setImage(const QImage& image);
    void fitImage();
signals:
    void saved(const QString& path);
    void imagePositionHovered(const QPointF& position);
    void imageHoverLeft();
protected:
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void leaveEvent(QEvent*) override;
private:
    QGraphicsScene scene_;
    QGraphicsPixmapItem* pixmapItem_ = nullptr;
    QGraphicsPixmapItem* lodItem_ = nullptr;   // reduced copy drawn while zoomed out on a very large image
    void updateLod();
    bool hasImage_ = false;
    bool dragging_ = false;
    QPoint lastDragPos_;
};
