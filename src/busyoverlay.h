#pragma once
// A loading indicator laid over a page while its data is read on a worker thread: a dim scrim,
// a rotating arc and one line of text. It follows its parent's size and swallows clicks while shown.
#include <QElapsedTimer>
#include <QEvent>
#include <QPainter>
#include <QTimer>
#include <QWidget>
#include <cmath>

class BusyOverlay : public QWidget {
public:
    explicit BusyOverlay(QWidget* parent) : QWidget(parent) {
        setAttribute(Qt::WA_TransparentForMouseEvents, false);
        setAttribute(Qt::WA_NoSystemBackground, true);
        hide();
        parent->installEventFilter(this);
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    }
    void start(const QString& text) {
        text_ = text;
        setGeometry(parentWidget()->rect());
        raise();
        show();
        clock_.restart();
        timer_.start();
    }
    void setText(const QString& text) { text_ = text; update(); }
    void stop() {
        timer_.stop();
        hide();
    }
    bool running() const { return isVisible(); }

protected:
    bool eventFilter(QObject* o, QEvent* e) override {
        if (o == parentWidget() && e->type() == QEvent::Resize) setGeometry(parentWidget()->rect());
        return QWidget::eventFilter(o, e);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(20, 21, 23, 200));
        const QPointF c(width() / 2.0, height() / 2.0 - 14.0);
        const double r = 18.0;
        p.setPen(QPen(QColor(53, 55, 60), 3.0));
        p.drawEllipse(c, r, r);
        const double t = clock_.isValid() ? clock_.elapsed() / 1000.0 : 0.0;
        const int start = int(-t * 360.0 * 16.0) % (360 * 16);
        const int span = int((90.0 + 60.0 * std::sin(t * 2.4)) * 16.0);
        p.setPen(QPen(QColor(74, 143, 208), 3.0, Qt::SolidLine, Qt::RoundCap));
        p.drawArc(QRectF(c.x() - r, c.y() - r, 2 * r, 2 * r), start, span);
        p.setPen(QColor(154, 158, 165));
        p.drawText(QRectF(0, c.y() + r + 12, width(), 22), Qt::AlignHCenter | Qt::AlignVCenter, text_);
    }

private:
    QTimer timer_;
    QElapsedTimer clock_;
    QString text_;
};
