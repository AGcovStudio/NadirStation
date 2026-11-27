#pragma once

// The Passes page: Orbitron-style pass prediction and live tracking.
//
//   - the ground station (latitude, longitude, height, horizon) is set on the Settings page;
//   - satellites come from TLE files (fy2.tle by default, plus any 2-/3-line TLE files added here);
//   - the pass list is computed on worker threads (passpredict.*), never on the GUI thread, and a
//     new request cancels the one still running, so the window does not freeze with many satellites;
//   - once a second the page shows where the ticked satellites are: sky plot, world map with
//     footprints and the ground track of the followed satellite, and its azimuth, elevation, range
//     and Doppler.

#include "passpredict.h"

#include <QHash>
#include <QImage>
#include <QList>
#include <QPointer>
#include <QThread>
#include <QPointF>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

#include <atomic>
#include <memory>
#include <vector>

class QCheckBox;
class QNetworkAccessManager;
class QNetworkReply;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;
class QTimer;

// Azimuth/elevation polar plot: the selected pass and the satellites above the horizon now.
class PassSkyPlot : public QWidget {
    Q_OBJECT
public:
    struct Marker {
        QString name;
        double azDeg = 0.0, elDeg = 0.0;
        bool followed = false;
    };
    explicit PassSkyPlot(QWidget* parent = nullptr);
    void setPass(const nadirstation::pass::Pass* pass);   // copied; nullptr clears
    void setMarkers(const QVector<Marker>& markers);
    QSize sizeHint() const override { return {320, 320}; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QPointF toScreen(double azDeg, double elDeg, const QPointF& c, double r) const;
    nadirstation::pass::Pass pass_;
    bool hasPass_ = false;
    QVector<Marker> markers_;
};

// Equirectangular world map: day/night, station, satellites with footprints, followed ground track.
class PassMap : public QWidget {
    Q_OBJECT
public:
    struct Sat {
        QString name;
        double latDeg = 0.0, lonDeg = 0.0;
        double footprintDeg = 0.0;
        bool followed = false;
    };
    explicit PassMap(QWidget* parent = nullptr);
    void setBackground(const QImage& image);
    void setStation(double latDeg, double lonDeg);
    void setSun(double latDeg, double lonDeg);
    void setSatellites(const QVector<Sat>& sats);
    void setTrack(const QVector<QPointF>& lonLat);   // x = lon, y = lat
    QSize sizeHint() const override { return {640, 320}; }
    bool hasHeightForWidth() const override { return true; }
    int heightForWidth(int w) const override { return w / 2; }

protected:
    void paintEvent(QPaintEvent*) override;

private:
    QRectF mapRect() const;
    QPointF toScreen(double latDeg, double lonDeg, const QRectF& r) const;
    void drawPath(QPainter& p, const QVector<QPointF>& lonLat, const QRectF& r) const;
    QImage background_;
    QImage scaled_;
    double stLat_ = 0.0, stLon_ = 0.0;
    double sunLat_ = 0.0, sunLon_ = 0.0;
    bool sunKnown_ = false;
    QVector<Sat> sats_;
    QVector<QPointF> track_;
};

class PassPage : public QWidget {
    Q_OBJECT
public:
    explicit PassPage(QWidget* parent = nullptr);
    ~PassPage() override;

    // The ground station; recomputes the passes. Saved by the Settings page, not here.
    void setStation(double latDeg, double lonDeg, double altM, double horizonDeg);
    // Starts or stops the once-a-second live update (on while the page is visible).
    void setActive(bool on);

signals:
    // "Settings…" was pressed: show the Settings page, where the station is entered.
    void settingsRequested();

private:
    struct Job;
    void addTleFiles();
    void clearTleFiles();
    void reloadTle();
    void onTleLoaded(int generation, std::shared_ptr<std::vector<nadirstation::pass::TleEntry>> entries,
                     QStringList report);
    void rebuildSatelliteList();
    void applyFilter();
    void onSatelliteItemChanged(QListWidgetItem* item);
    void setAllVisibleChecked(bool on);
    void scheduleCompute();
    void compute();
    void onComputeFinished(std::shared_ptr<Job> job);
    void fillTable();
    void onTableSelection();
    void liveTick();
    void updateStationLabel();
    void saveSelection() const;
    std::vector<nadirstation::pass::TleEntry> checkedEntries() const;
    nadirstation::pass::Tracker* trackerFor(int norad);
    QString timeText(double unixSeconds) const;
    void loadMapInBackground();
    // Downloads current TLEs from Celestrak (asynchronously; nothing waits for it). Called once at
    // program start when "启动时在线更新" is on, and by the "在线更新" button. Without a network the
    // requests fail quietly and the local files are used. Group files go to <AppLocalData>/tle/,
    // FY-2G/FY-2H are merged into the writable fy2.tle (which the image navigation also reads).
    void updateTleOnline(bool force);
    void onDownloadFinished(QNetworkReply* reply);
    void finishOnlineUpdate();
    void startDownload(const QString& url, const QString& kind, const QString& target);

    // Station
    nadirstation::pass::Station station_;
    double horizonDeg_ = 0.0;
    bool stationSet_ = false;

    // TLE
    QStringList tleFiles_;
    std::vector<nadirstation::pass::TleEntry> entries_;
    QHash<int, int> indexByNorad_;
    QList<int> checked_;                 // NORAD ids, in tick order
    std::vector<std::unique_ptr<nadirstation::pass::Tracker>> trackers_;   // parallel to entries_
    int tleGeneration_ = 0;

    // Prediction
    std::vector<nadirstation::pass::Pass> passes_;
    std::shared_ptr<Job> running_;
    int jobGeneration_ = 0;
    double windowStart_ = 0.0, windowEnd_ = 0.0;
    int followedNorad_ = 0;
    int selectedPass_ = -1;   // index into passes_

    // Widgets
    QLabel* stationLabel_ = nullptr;
    QLabel* tleLabel_ = nullptr;
    QLineEdit* filter_ = nullptr;
    QListWidget* satList_ = nullptr;
    QSpinBox* days_ = nullptr;
    QDoubleSpinBox* minPeak_ = nullptr;
    QCheckBox* utc_ = nullptr;
    QCheckBox* onlyVisible_ = nullptr;
    QPushButton* computeBtn_ = nullptr;
    QPushButton* cancelBtn_ = nullptr;
    QProgressBar* progress_ = nullptr;
    QLabel* resultLabel_ = nullptr;
    QTableWidget* table_ = nullptr;
    PassSkyPlot* sky_ = nullptr;
    PassMap* map_ = nullptr;
    QLabel* liveTitle_ = nullptr;
    QLabel* liveInfo_ = nullptr;
    QDoubleSpinBox* freqMHz_ = nullptr;
    QTimer* liveTimer_ = nullptr;
    QTimer* computeDebounce_ = nullptr;
    QTimer* progressTimer_ = nullptr;
    double lastTrackAt_ = 0.0;
    double lastTableAt_ = 0.0;
    bool active_ = false;
    bool mapLoading_ = false;
    QNetworkAccessManager* net_ = nullptr;
    QCheckBox* autoUpdate_ = nullptr;
    QPushButton* updateBtn_ = nullptr;
    QLabel* onlineLabel_ = nullptr;
    int downloadsPending_ = 0;
    int downloadsOk_ = 0;
    int downloadsFailed_ = 0;
    int downloadsSkipped_ = 0;
    QString downloadError_;
    bool triedFallback_ = false;
    std::vector<nadirstation::pass::TleEntry> fy2Fresh_;
    QPointer<QThread> tleThread_;
    QPointer<QThread> mapThread_;
    QList<QPointer<QThread>> jobThreads_;   // prediction threads, including cancelled ones still stopping
};
