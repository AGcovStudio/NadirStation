#pragma once
#include <QColor>
#include <QWidget>
#include <complex>
#include <vector>

class ConstellationWidget : public QWidget {
    Q_OBJECT
public:
    explicit ConstellationWidget(QWidget* parent = nullptr);
    void setPoints(const std::vector<std::complex<float>>& points);

    // What the display means depends on the service: FY-2 S-VISSR is differentially encoded, so a
    // constant phase offset is irrelevant, while GK-2A LRIT is absolute and the receiver has to
    // resolve the polarity. A label that says "differential" for both tells the reader the wrong
    // thing about what they are looking at.
    void setAnnotation(const QString& modulation, const QString& detail);
    // Colour of the points: the SNR zone the link is in (see snrmeter.h).
    void setTint(const QColor& c) { tint_ = c; update(); }
    // As wide as the Live page column and square, with room for the title and the caption.
    QSize sizeHint() const override { return QSize(320, 320); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    std::vector<std::complex<float>> points_;
    QString modulation_ = QStringLiteral("Constellation");
    QString detail_ = QStringLiteral("Re +Im · 1:1 aspect");
    QColor tint_ = QColor(255, 255, 255);
};
