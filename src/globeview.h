#pragma once

// The Globe page: a zoomable, rotatable Earth built from the world relief map and the newest GK-2A
// full disk this station decoded (brightness temperature as clouds, or a product image draped on
// the sphere), with a lat/lon grid, the geostationary satellites' sub-points and the day/night line.

#include "globecore.h"

#include <QDateTime>
#include <QImage>
#include <QPoint>
#include <QString>
#include <QWidget>
#include <QSet>
#include <QStringList>

#include <cstdint>
#include <vector>

class QComboBox;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTimer;

class GlobeView : public QWidget {
    Q_OBJECT
public:
    explicit GlobeView(QWidget* parent = nullptr);

    // Loads a GK-2A observation folder: GK2A_IR_0p01K.png when present (clouds, query, forecast)
    // and the image named by productFile -- a full-disk product, or with `geo` a lat/lon regional one.
    // An empty productFile is the true-colour view, which needs the temperature raster.
    bool loadObservation(const QString& folder, const QString& productFile, const nadirstation::globe::GeoBox* geo = nullptr);
    bool hasTbb() const { return !observed_.empty(); }
    void setLayer(nadirstation::globe::Layer layer);
    void setGridVisible(bool on) { grid_ = on; update(); }
    void setSatellitesVisible(bool on) { satellites_ = on; update(); }
    void setNightVisible(bool on);
    void resetView();
    QImage grabView();
    QString observationLabel() const { return observationLabel_; }
    // The observed brightness temperature of the loaded observation and its time (unix seconds, 0
    // when unknown), for the forecast.
    const nadirstation::globe::Grey16& observedTbb() const { return observed_; }
    double observationTime() const { return observationTime_; }
    // Shows another temperature field of the same disk (a forecast) in place of the observation;
    // the day/night line follows `unixTime`. showObserved() goes back.
    void showTbb(const std::vector<std::uint16_t>& raw, double unixTime, const QString& label);
    void showObserved();
    const nadirstation::globe::DiskGeometry& disk() const { return scene_.disk; }
    // A marked point (the query location), drawn as a ring with its coordinates.
    void setMarker(bool on, double latDeg = 0.0, double lonDeg = 0.0);
    // Turns the view so that a point is in the middle.
    void centreOn(double latDeg, double lonDeg);

signals:
    void hoverText(const QString& text);
    // Right click on the globe.
    void pointPicked(double latDeg, double lonDeg);

public:
    // Loading off the GUI thread: the relief map and an observation's temperature raster can be read
    // on a worker (readRelief / QImage) and handed over here; loadObservation then uses them.
    static nadirstation::globe::Rgb8 readRelief();
    void adoptRelief(nadirstation::globe::Rgb8&& relief);
    bool hasRelief() const { return reliefTried_; }
    void preloadTbb(const QString& path, const QImage& image) { cachedTbbPath_ = path; cachedTbb_ = image; }

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void keyPressEvent(QKeyEvent*) override;

private:
    QString cachedTbbPath_;
    QImage cachedTbb_;
    void ensureRelief();
    void render(int step);
    void scheduleFull();
    void drawOverlays(QPainter& p);
    void updateHover(const QPointF& pos);

    nadirstation::globe::Rgb8 relief_;
    nadirstation::globe::Grey16 tbb_;
    nadirstation::globe::Rgb8 product_;
    nadirstation::globe::Scene scene_;
    nadirstation::globe::View view_;
    QImage frame_;
    bool frameDirty_ = true;
    int frameStep_ = 1;
    bool grid_ = true, satellites_ = true;
    bool dragging_ = false;
    QPoint lastPos_;
    QTimer* refine_ = nullptr;
    QString observationLabel_;
    QString observedLabel_;
    nadirstation::globe::Grey16 observed_;
    double observationTime_ = 0.0;
    bool reliefTried_ = false;
    bool marker_ = false;
    double markerLat_ = 0.0, markerLon_ = 0.0;
};

class GlobePage : public QWidget {
    Q_OBJECT
public:
    explicit GlobePage(QWidget* parent = nullptr);
    // The output root; the page's folder starts there unless one was chosen before.
    void setDataRoot(const QString& root);
    // Re-reads the list of GK-2A observations and shows the newest when nothing is shown yet.
    void refreshObservations();
    GlobeView* view() const { return view_; }
    // Selects the forecast lead (10-minute steps, 0 = observation); loads the newest observation
    // first if the page has not been shown yet. For --globe-lead.
    void setLead(int steps);
    // Marks and queries a point (for --globe-query).
    void queryPoint(double latDeg, double lonDeg);
    // Shows an observation (folder name or its ending, ":enhanced" for the enhanced view); for --globe-image.
    bool selectImage(const QString& name);

    // One entry of the list: a GK-2A observation folder (with GK2A_IR_0p01K.png) in one of the two
    // views, true colour with clouds or with the cold cloud tops enhanced.
    struct Image {
        QString folder;          // the observation folder
        QString label;           // shown in the list
        bool enhanced = false;
        bool fromJson = false;   // the folder has a GK-2A JSON sidecar
    };

protected:
    void showEvent(QShowEvent* event) override;

private:
    void loadSelected();
    void applyLead();
    void startForecast();
    QString previousFolder(const QString& folder) const;
    QString folder() const;
    void runQuery();
    void scanFolder(const QString& root);
    // The folder scan as a pure function (worker thread) and its application to the page.
    struct ScanResult {
        QString root;
        QStringList folders;          // newest first
        QSet<QString> jsonFolders;
        int jsonFiles = 0;
    };
    static ScanResult scanObservations(const QString& root);
    void applyScan(const ScanResult& scan);
    void fillList(const QString& keepFolder, bool keepEnhanced);
    // First show, Refresh, a new folder: scan, relief map and the newest raster are read on a worker
    // thread behind a loading indicator, so the window does not freeze.
    void refreshObservationsAsync();
    class BusyOverlay* busy_ = nullptr;
    bool scanning_ = false;

    QString dataRoot_;
    GlobeView* view_ = nullptr;
    QLineEdit* dirEdit_ = nullptr;
    QPushButton* browse_ = nullptr;
    class QTreeWidget* list_ = nullptr;
    QLabel* listInfo_ = nullptr;
    std::vector<Image> images_;
    QStringList tbbFolders_;          // folders with GK2A_IR_0p01K.png (for the forecast pair)
    QString currentFolder_;           // folder of the image shown
    int current_ = -1;                // index into images_
    QComboBox* lead_ = nullptr;
    QLabel* aiNote_ = nullptr;
    // Forecast of the shown observation: one 0.01 K raster per 10-minute step.
    QString forecastFolder_, forecastPrevious_, forecastModel_;
    std::vector<std::vector<std::uint16_t>> forecastSteps_;
    bool forecastRunning_ = false;
    int forecastGeneration_ = 0;
    QCheckBox* grid_ = nullptr;
    QCheckBox* satellites_ = nullptr;
    QCheckBox* night_ = nullptr;
    QPushButton* refresh_ = nullptr;
    QPushButton* exportButton_ = nullptr;
    QLabel* hover_ = nullptr;
    QLabel* note_ = nullptr;
    bool loadedOnce_ = false;
    // Point query
    class QDoubleSpinBox* qLat_ = nullptr;
    class QDoubleSpinBox* qLon_ = nullptr;
    QPushButton* qRun_ = nullptr;
    QPushButton* qClear_ = nullptr;
    QLabel* qResult_ = nullptr;
    bool queryActive_ = false;
    std::vector<std::uint16_t> forecastPrevRaw_;   // the observation 10 minutes earlier
};
