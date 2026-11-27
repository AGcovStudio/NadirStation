#pragma once

// Space Weather page: the state of the space environment at geostationary orbit as ELEKTRO-L N4's
// GGAK instruments report it, from frames this station received. Each card compares a channel's
// last ten minutes with the station's own history of that channel (swcore.h says what that does
// and does not mean); the banner shows the worst card.

#include "swcore.h"

#include <QByteArray>
#include <QColor>
#include <QString>
#include <QWidget>

#include <map>
#include <string>
#include <vector>

class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

class SwSparkline : public QWidget {
    Q_OBJECT
public:
    explicit SwSparkline(QWidget* parent = nullptr);
    struct Line { std::vector<nadirstation::spaceweather::Bin> bins; double baseline = 0.0; QColor colour; QString label; };
    // mode decides the y axis: ratio to baseline (Ratio, Variability) or percent change (Decrease).
    void setData(const std::vector<Line>& lines, nadirstation::spaceweather::Mode mode, double tStart, double tEnd);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    std::vector<Line> lines_;
    nadirstation::spaceweather::Mode mode_ = nadirstation::spaceweather::Mode::Ratio;
    double t0_ = 0.0, t1_ = 1.0;
};

class SpaceWeatherPage : public QWidget {
    Q_OBJECT
public:
    explicit SpaceWeatherPage(QWidget* parent = nullptr);
    void setDataRoot(const QString& root) { dataRoot_ = root; }
    bool savePng(const QString& path);
public slots:
    void addFrames(const QByteArray& frames);
    void setWindowSeconds(double seconds);
signals:
    void windowSecondsChanged(double seconds);
protected:
    void showEvent(QShowEvent* event) override;
private:
    // scale: physical units per stored value (counts/s per count, nT per magnetometer count); unit
    // empty for a channel whose meaning is not established (shown as the raw value).
    struct Channel { std::string key; const char* label; double scale = 1.0; const char* unit = ""; };
    struct Card {
        const char* title;
        const char* instrument;
        nadirstation::spaceweather::Mode mode;
        std::vector<Channel> channels;
        // false: what the channels measure has not been shown from the data (see the card table in
        // spaceweatherpage.cpp); the card is drawn but kept out of the overall level.
        bool verified = true;
        QLabel* state = nullptr;
        QLabel* value = nullptr;
        QLabel* detail = nullptr;
        SwSparkline* plot = nullptr;
        QWidget* frame = nullptr;
    };
    void ingest(const char* data, int size);
    void loadHistory();
    void refresh();

    QString dataRoot_;
    bool historyLoaded_ = false;
    bool dirty_ = true;
    std::vector<Card> cards_;
    QLabel* banner_ = nullptr;
    QLabel* bannerDetail_ = nullptr;
    QLabel* range_ = nullptr;
    QComboBox* window_ = nullptr;
    QWidget* controls_ = nullptr;
    QWidget* report_ = nullptr;
    QTimer* timer_ = nullptr;
};
