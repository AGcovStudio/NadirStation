#pragma once
// ELEKTRO-L GGAK live page: receiver state, every frame type seen, the SKL and SKIF instruments
// (latest values, session range and trends), and the raw bytes of any frame -- including the
// instruments whose formats are not published, which are shown as they arrive.
#include <unordered_set>
#include <QByteArray>
#include <QColor>
#include <QHash>
#include <QMap>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <array>
#include <deque>
#include <vector>
#include "ggak.h"

class QLabel;
class QTableWidget;
class QTabWidget;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QComboBox;
class QMouseEvent;
class QWheelEvent;

// Time-series plot for a few channels: x is UTC, y linear or logarithmic.
// Drag left/right to look back in time, Ctrl + wheel to zoom the time axis, double-click to return
// to the newest data. All trend plots share one view, so the panels stay lined up in time.
class GgakTrendPlot : public QWidget {
    Q_OBJECT
public:
    explicit GgakTrendPlot(QWidget* parent = nullptr);
    ~GgakTrendPlot() override;
    struct Series { QString name; QColor colour; std::vector<std::pair<double, double>> points; };
    void setSeries(const std::vector<Series>& series);
    void setLogScale(bool on) { log_ = on; update(); }
    void setWindowSeconds(double s) { window_ = s; update(); }
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void mouseDoubleClickEvent(QMouseEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;
private:
    QRectF plotRect() const;
    void dataRange(double& tFirst, double& tMax) const;
    double currentSpan(double tFirst, double tMax) const;
    double currentEnd(double tMax) const;
    static void shareView(double end, double span);   // end 0 = follow the newest data
    std::vector<Series> series_;
    bool log_ = true;
    double window_ = 3600.0;
    double viewEnd_ = 0.0;    // right edge in unix seconds while looking back; 0 = live
    double viewSpan_ = 0.0;   // zoomed span in seconds; 0 = the trend window
    bool dragging_ = false;
    double pressX_ = 0.0, pressEnd_ = 0.0, pressSpan_ = 0.0;
};

// Latest value of each channel of one instrument against channel, with the session's min..max
// band behind it: the instrument's spectrum at a glance.
class GgakSpectrumPlot : public QWidget {
    Q_OBJECT
public:
    explicit GgakSpectrumPlot(QWidget* parent = nullptr);
    void setData(const QString& title, const QStringList& labels, const std::vector<double>& latest,
                 const std::vector<double>& lo, const std::vector<double>& hi, bool logScale, const QColor& colour);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QString title_;
    QStringList labels_;
    std::vector<double> latest_, lo_, hi_;
    bool log_ = true;
    QColor colour_;
};

// Time against channel, coloured by value: how the whole spectrum evolves.
class GgakSpectrogram : public QWidget {
    Q_OBJECT
public:
    explicit GgakSpectrogram(QWidget* parent = nullptr);
    void setData(const QString& title, const QStringList& rows,
                 const std::vector<const std::deque<std::pair<double, double>>*>& histories,
                 double windowSeconds, bool logScale);
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QString title_;
    QStringList rows_;
    std::vector<std::vector<double>> cells_;   // [row][column], NaN where nothing arrived
    double tMin_ = 0.0, tMax_ = 0.0, vMin_ = 0.0, vMax_ = 1.0;
    bool log_ = true;
};

class GgakPage : public QWidget {
    Q_OBJECT
public:
    explicit GgakPage(QWidget* parent = nullptr);
public slots:
    void addFrames(const QByteArray& frames);
    void setStatus(int syncState, double carrierOffsetHz, double carrierToNoiseDb, bool carrierFound,
                   double snrDb, qulonglong goodFrames, qulonglong badFrames, const QString& outputDir);
    void clearSession();
public:
    // Developer aid: adds the frames of a .cadu file and shows the tab whose title starts with `tab`.
    void loadFramesForReview(const QString& path, const QString& tab);
    // Puts another view in front of the instrument tabs (the Space Weather summary) and shows it.
    void addFrontTab(QWidget* page, const QString& title);
    // Shows the tab whose title starts with `title`.
    void showTab(const QString& title);
    bool saveTabPng(const QString& tab, const QString& path);
    double trendWindowSeconds() const;
    void setTrendWindowSeconds(double seconds);
signals:
    void trendWindowChanged(double seconds);
    void framesImported(const QByteArray& frames);
protected:
    // The charts skip redrawing while hidden, so a page shown after frames arrived (a file opened
    // from the command line, or frames received while another page was in front) redraws here.
    void showEvent(QShowEvent* event) override;
private:
    void updateUnitLabel();
    void updateLevelSelection();
    void refreshTables();
    void refreshPlots();
    void showRawFor(int id);
    void loadCaduFile();
    void refreshLinkPlots();
    void refreshPhysicalPlots();
    void refreshSpectra();
    void refreshL4();
    void refreshProducts(bool force = false);
    // Fills n4Small_; returns the summary text for the newest N4 values (empty without N4 frames).
    QString refreshN4Physical(double window);

    struct IdStats {
        qulonglong count = 0, badChecksum = 0;
        int lastMaster = -1, lastChannel = -1;
        double lastTime = 0.0;
        QByteArray lastFrame;
    };
    struct Range { double last = 0, min = 0, max = 0; int lastRaw = -1; bool seen = false; };

    QLabel* state_ = nullptr;
    QLabel* counts_ = nullptr;
    QLabel* carrier_ = nullptr;
    QLabel* lastFrame_ = nullptr;
    QLabel* folder_ = nullptr;
    QTableWidget* ids_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QComboBox* levelSelect_ = nullptr;
    QWidget* sklTab_ = nullptr;
    QWidget* skifTab_ = nullptr;
    QTableWidget* skl_ = nullptr;
    QTableWidget* skif_ = nullptr;
    GgakTrendPlot* sklPlot_ = nullptr;
    GgakTrendPlot* skifPlot_ = nullptr;
    QPlainTextEdit* raw_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QComboBox* window_ = nullptr;
    QCheckBox* logScale_ = nullptr;
    QPushButton* load_ = nullptr;
    QPushButton* clear_ = nullptr;
    QString outputDir_;

    QMap<int, IdStats> idStats_;
    std::array<Range, nadirstation::ggak::kSklChannels> sklRange_{};
    std::array<Range, nadirstation::ggak::SkifChannelCount> skifRange_{};
    std::array<std::deque<std::pair<double, double>>, nadirstation::ggak::kSklChannels> sklHistory_;
    std::array<std::deque<std::pair<double, double>>, nadirstation::ggak::SkifChannelCount> skifHistory_;
    int selectedRawId_ = -1;
    bool dirty_ = false;

    // Live link plots: one point a second from the decoder status.
    GgakTrendPlot* snrPlot_ = nullptr;
    GgakTrendPlot* carrierPlot_ = nullptr;
    GgakTrendPlot* ratePlot_ = nullptr;
    std::deque<std::pair<double, double>> snrHistory_, carrierHistory_, goodRateHistory_, badRateHistory_;
    double linkLastT_ = 0.0;
    qulonglong linkLastGood_ = 0, linkLastBad_ = 0;
    double snrSum_ = 0.0, carrierSum_ = 0.0;
    int linkSamples_ = 0;

    // Incremental one-minute bins for reception and .cadu replay.
    struct ProductMinute {
        std::array<double, 9> sum{};
        std::array<int, 9> samples{};
        std::array<int, 7> frames{};
    };
    QMap<qint64, ProductMinute> productMinutes_;
    qint64 productLastRefreshMs_ = 0;
    QWidget* productsPage_ = nullptr;
    QPushButton* productsExport_ = nullptr;
    QLabel* productMode_ = nullptr;
    QLabel* productQuality_ = nullptr;
    QLabel* productEvent_ = nullptr;
    std::array<QLabel*, 4> productValues_{};
    std::array<QLabel*, 4> productDetails_{};
    std::array<GgakTrendPlot*, 4> productPlots_{};

    // Physical data: one small chart per decoded channel (SKL and SKIF), all at once.
    QWidget* physicalPage_ = nullptr;
    QPushButton* physicalExport_ = nullptr;
    std::vector<GgakTrendPlot*> sklSmall_, skifSmall_;
    std::vector<GgakTrendPlot*> n4Small_;   // Physical data: ELEKTRO-L N4 instruments, one channel each
    QLabel* summary_ = nullptr;   // key values above the charts

    // Spectra tab.
    QWidget* spectraPage_ = nullptr;
    GgakSpectrumPlot* sklSpectrum_ = nullptr;
    GgakSpectrumPlot* electronSpectrum_ = nullptr;
    GgakSpectrumPlot* protonSpectrum_ = nullptr;
    GgakSpectrogram* sklGram_ = nullptr;
    GgakSpectrogram* electronGram_ = nullptr;
    GgakSpectrogram* protonGram_ = nullptr;

    // Unit identification and frame loss (master counter of instrument frames).
    nadirstation::ggak::UnitIdentifier unit_;
    QLabel* unitLabel_ = nullptr;
    int lastMaster_ = -1;
    // Frames already taken in (ID, master counter, frame time). A frame received twice -- the same
    // recording opened again, or a repeat in the downlink -- is counted once and plotted once.
    std::unordered_set<std::uint64_t> seenFrames_;
    qulonglong lostFrames_ = 0, expectedFrames_ = 0;

    // ELEKTRO-L N4 instruments (inferred layouts).
    QWidget* l4Page_ = nullptr;
    QPushButton* l4Export_ = nullptr;
    QWidget* l4Tab_ = nullptr;
    QLabel* l4Unit_ = nullptr;
    std::vector<GgakTrendPlot*> l4Plots_;
    std::vector<QWidget*> l3PhysicalWidgets_, l4PhysicalWidgets_;
    int selectedLevel_ = 3;
    QMap<int, std::deque<std::pair<double, QByteArray>>> frameHistory_;   // per ID, valid frames
};
