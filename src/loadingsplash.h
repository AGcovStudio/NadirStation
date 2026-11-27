#pragma once
// Start-up window shown while the main window is being built. Building every page takes a second
// or more (longer on a cold disk), and the event loop is not running yet, so a normal animation
// would freeze. Instead the animation is a pure function of the time since the splash appeared:
// every repaint, whenever it happens, draws the right frame, and MainWindow's constructor calls
// step() between pages, which repaints and lets the window system breathe. Once the event loop
// runs, a 16 ms timer keeps it moving until finish() fades it out over the shown main window.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPropertyAnimation>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <algorithm>
#include <cmath>

class LoadingSplash : public QWidget {
public:
    LoadingSplash()
        : QWidget(nullptr, Qt::SplashScreen | Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint) {
        setAttribute(Qt::WA_TranslucentBackground, false);
        setAttribute(Qt::WA_DeleteOnClose, true);
        setFixedSize(560, 300);
        mark_ = QPixmap(QDir(QCoreApplication::applicationDirPath())
                            .filePath(QStringLiteral("resources/brand/nadir_station_wordmark_ui@2x.png")));
        if (!mark_.isNull()) mark_.setDevicePixelRatio(2.0);
        if (const QScreen* s = QGuiApplication::primaryScreen())
            move(s->availableGeometry().center() - rect().center());
        clock_.start();
        timer_.setInterval(16);
        connect(&timer_, &QTimer::timeout, this, qOverload<>(&QWidget::update));
        timer_.start();
    }

    // Called between construction steps: shows the stage and paints a frame now.
    void step(const QString& text, double fraction) {
        text_ = text;
        target_ = std::clamp(fraction, 0.0, 1.0);
        repaint();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
    }

    // Fade out over the main window, then close.
    void finish(QWidget* mainWindow) {
        step(QStringLiteral("Ready"), 1.0);
        auto* fade = new QPropertyAnimation(this, "windowOpacity", this);
        fade->setDuration(260);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        connect(fade, &QPropertyAnimation::finished, this, [this, mainWindow] {
            close();
            if (mainWindow) { mainWindow->raise(); mainWindow->activateWindow(); }
        });
        QTimer::singleShot(120, fade, [fade] { fade->start(); });
    }

protected:
    void paintEvent(QPaintEvent*) override {
        const double t = clock_.elapsed() / 1000.0;
        // The bar eases towards the stage fraction instead of jumping.
        shown_ += (target_ - shown_) * 0.25;
        if (std::fabs(target_ - shown_) < 0.002) shown_ = target_;

        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(28, 29, 32));
        p.setPen(QColor(53, 55, 60));
        p.drawRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5));

        // Orbit motif on the right: a ring and a satellite going round it, the only moving
        // ornament, drawn in the accent colour.
        const QPointF c(width() - 120.0, 118.0);
        const double r = 58.0;
        p.setPen(QPen(QColor(53, 55, 60), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(c, r, r * 0.42);
        p.setPen(Qt::NoPen);
        QRadialGradient earth(c - QPointF(5, 5), 22);
        earth.setColorAt(0.0, QColor(74, 143, 208));
        earth.setColorAt(1.0, QColor(34, 56, 77));
        p.setBrush(earth);
        p.drawEllipse(c, 15, 15);
        const double a = t * 1.6;
        const QPointF sat(c.x() + r * std::cos(a), c.y() + r * 0.42 * std::sin(a));
        // Hidden behind the globe on the far side of the orbit.
        if (!(std::sin(a) < 0 && std::fabs(sat.x() - c.x()) < 15)) {
            p.setBrush(QColor(214, 216, 219));
            p.drawEllipse(sat, 3.2, 3.2);
            p.setBrush(QColor(74, 143, 208, 70));
            p.drawEllipse(sat, 7.5, 7.5);
        }

        // Wordmark, or the name in text when the image is missing.
        if (!mark_.isNull()) {
            p.drawPixmap(QPointF(36, 52), mark_);
        } else {
            QFont f = font();
            f.setPixelSize(30);
            f.setWeight(QFont::DemiBold);
            p.setFont(f);
            p.setPen(QColor(214, 216, 219));
            p.drawText(QPointF(36, 86), QStringLiteral("NADIR STATION"));
        }
        QFont small = font();
        small.setPixelSize(13);
        p.setFont(small);
        p.setPen(QColor(154, 158, 165));
        p.drawText(QPointF(38, 128), QStringLiteral("Geostationary weather-satellite ground station"));

        // Progress: a determinate bar with a soft highlight sweeping along the filled part.
        const QRectF bar(36, height() - 70, width() - 72, 3);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(43, 45, 49));
        p.drawRoundedRect(bar, 1.5, 1.5);
        const QRectF filled(bar.left(), bar.top(), bar.width() * std::max(0.03, shown_), bar.height());
        p.setBrush(QColor(74, 143, 208));
        p.drawRoundedRect(filled, 1.5, 1.5);
        {
            const double sweep = std::fmod(t * 0.9, 1.4) - 0.2;
            const double x = filled.left() + filled.width() * sweep;
            QLinearGradient g(x - 60, 0, x + 60, 0);
            g.setColorAt(0.0, QColor(255, 255, 255, 0));
            g.setColorAt(0.5, QColor(255, 255, 255, 110));
            g.setColorAt(1.0, QColor(255, 255, 255, 0));
            p.setBrush(g);
            p.drawRoundedRect(filled, 1.5, 1.5);
        }

        p.setPen(QColor(154, 158, 165));
        p.drawText(QRectF(36, height() - 58, width() - 140, 20), Qt::AlignLeft | Qt::AlignVCenter, text_);
        p.setPen(QColor(110, 115, 122));
        p.drawText(QRectF(36, height() - 58, width() - 72, 20), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("%1 %").arg(static_cast<int>(std::round(shown_ * 100.0))));
        p.drawText(QRectF(36, height() - 34, width() - 72, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("v%1  ·  © AGcov (BI2QYN)").arg(QCoreApplication::applicationVersion()));
    }

private:
    QPixmap mark_;
    QElapsedTimer clock_;
    QTimer timer_;
    QString text_ = QStringLiteral("Starting…");
    double target_ = 0.0, shown_ = 0.0;
};
