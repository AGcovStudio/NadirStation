#pragma once
#include <QMainWindow>
#include <QListWidget>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QComboBox>
#include <QCheckBox>
#include <deque>
#include <QLineEdit>
#include <QSpinBox>
#include <QTimer>
#include <QDateTime>
#include <QElapsedTimer>
#include <memory>
#include <functional>
#include <mutex>
#include <vector>
#include <complex>
#include <limits>
#include "plutoreceiver.h"
#include "wavreceiver.h"
#include "rtlsdrreceiver.h"
#include "hackrfreceiver.h"
#include "nadirstationdecoder.h"
#include "spectrumwidget.h"
#include "constellationwidget.h"
#include "imageview.h"
#include "systemmonitorpage.h"
#include "telemetrypage.h"
#include "iqrecorder.h"

class QPlainTextEdit;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    // Start-up progress hook (the loading splash): stage text and fraction done. Set by main()
    // before the window is built and cleared afterwards.
    static std::function<void(const QString&, double)> startupProgress;
    explicit MainWindow(QWidget* parent = nullptr);
    ~MainWindow() override;
protected:
    // Remembers the window geometry, the page, the satellite and the input for the next start.
    void closeEvent(QCloseEvent* event) override;
public:
    void runWavFileForDiagnosis(const QString& path, bool autoSelectSatellite = true);
    void setGlobeLead(int steps);
    void setGlobeQuery(double latDeg, double lonDeg);
    void setGlobeImage(const QString& name);
    // Selects a satellite by the name shown in the selector, so a recording can be replayed
    // against the service it came from. The satellite decides the symbol rate and the modulation,
    // and a mismatched selection decodes nothing.
    void selectSatelliteByName(const QString& name);
    // Shows the page with this name -- processing, image, add, system, telemetry, settings. Used by
    // --page so a page can be opened without driving the interface, which the tabs do not allow.
    void showPageByName(const QString& name);
    // Points the additional-data page at another folder and rescans (command line: --add-dir DIR).
    void setAddFolder(const QString& dir);
    void reviewGgakFrames(const QString& caduPath, const QString& tab);
    bool exportSpaceWeatherPng(const QString& path);
    bool exportGgakPng(const QString& tab, const QString& path);
    // Opens the additional-data page with this file chosen, for inspecting one without hunting for it.
    void showAddFileByName(const QString& path);
    // The id a visible satellite name refers to, used when the combo box's data value and its
    // displayed text disagree.
    static int satelliteIdFromName(const QString& text);
    void scheduleCaptureWindow(const QString& startTime, const QString& stopTime);
    // Developer aid: random start/stop sequences on the real buttons (command line --stress-stop).
    void runStopStress(const QString& wavPath, int cycles, const QString& sources);

private slots:
    // True when the satellite box shows an actual satellite. Every input source needs one: the
    // downlink frequency, the symbol rate and the decoder chain all come from it. Otherwise it
    // warns the operator and returns false.
    bool requireSatellite(const QString& action);
    void startRx();
    void stopRx();
    void startDecode();
    void stopDecode();
    void browseWav();
    QString selectSatelliteForFile(const QString& path);
    void updateInputUi();
    void updateMetrics(float snr, float peak, float avg, double sr);
    void updateStatus(const QString& text);
    void onImagePreviewReady(const QString& timestamp, const QList<QString>& paths);
    void onHritLivePreview(const QString& timestamp, const QImage& image, const QString& info);
    void onImageReady(const QString& timestamp, const QList<QString>& paths);
    void selectImage(QListWidgetItem* item);
    void saveCurrentImage();
    void importSessionJson();
    void regenerateGk2aRegionalProducts();
    void projectHritFiveChannels();
    void applyImageProcessing();
public:
    // Channel images of an observation, named the way RGB formulas refer to them (ch1, ch3a ...),
    // each with a short description; from the file names in `paths`.
    static QList<std::pair<QString, std::pair<QString, QString>>> rgbChannelFiles(const QStringList& paths);
private:
    void switchPage(int index);
    void refreshSystemStatus();
    void applySpectrumSettings();

private:
    QWidget* makeProcessingPage();
    QWidget* makeImagePage();
    QWidget* makeSettingsPage();
    // The additional-data page: the text records LRIT carries in a file's secondary headers.
    QWidget* makeAddPage();
    void refreshAddFiles();
    void showAddFile(const QString& path);
    // Grouped as the service defines it: service -> data type -> file.
    QTreeWidget* addFileTree_ = nullptr;
    // The additional data itself: the text of a schedule, or a weather chart drawn.
    QPlainTextEdit* addContent_ = nullptr;
    // The image viewer the image page uses, so a chart can be zoomed and panned rather than shown
    // at one fixed scale.
    ImageView* addContentImage_ = nullptr;
    // The selected file as its parts: name fields, header records, data field.
    QTreeWidget* addStructure_ = nullptr;
    QLabel* addSummary_ = nullptr;
    // Where the page looks. The files are still written to <root>/<spacecraft>/ADD/<category>/;
    // this is where the scan is pointed, so an archive or another machine's folder can be read.
    QLineEdit* addDirEdit_ = nullptr;
    QPushButton* addBrowse_ = nullptr;
    QPushButton* addRefresh_ = nullptr;
    QWidget* makeControlPanel();
    // Mirrors the run buttons' text into the property the stylesheet reads, and the connection label's tone.
    void refreshRunButtons();
    void setConnState(const QString& text, const QString& tone);
    void loadImagePaths(const QStringList& paths, const QString& timestamp,
                        const QString& preferredPath = QString());
    void loadCurrentPath(const QString& path);
    QImage processedCurrentImage() const;
    void applySatelliteSelection();
    void generateRegionalProductsForObservation(const QString& timestamp,
                                                const QList<QString>& sourcePaths);

    QStackedWidget* stack_ = nullptr;
    // The shell: a navigation rail on the left (one button per stack page, the page index in the
    // button's "page" property) and a link strip across the top that reports the receive chain
    // whichever page is shown.
    QWidget* makeNavRail();
    QWidget* makeLinkStrip();
    void refreshLinkStrip();
    void makeMenus();
    void restoreUiState();
    void showAbout();
    QLabel* statusOutput_ = nullptr;
    // Live page event list: status messages and link-state changes with their time, so a warning
    // that flashed past in the status bar can still be read.
    QPlainTextEdit* eventLog_ = nullptr;
    QString lastLoggedState_;
    void logEvent(const QString& text);
    QLabel* linkNext_ = nullptr;
    QWidget* linkNextBox_ = nullptr;
    QList<QPushButton*> navButtons_;
    QLabel* railClock_ = nullptr;
    QLabel* linkFreq_ = nullptr;
    QLabel* linkInput_ = nullptr;
    QLabel* linkState_ = nullptr;
    QLabel* linkSnr_ = nullptr;
    QLabel* linkCarrier_ = nullptr;
    QLabel* linkRec_ = nullptr;
    QLabel* linkUtc_ = nullptr;
    QString linkStateText_ = QStringLiteral("Idle");
    QString linkStateTone_ = QStringLiteral("idle");
    float lastSnrDb_ = std::numeric_limits<float>::quiet_NaN();
    // The symbol SNR as shown: the decoder's estimate jumps from one 200 ms report to the next, so the
    // read-out is a median of the last five reports followed by a 1 s exponential average; the peak is
    // the peak of that, and "1 s avg" the plain mean of the last second (snrSmoothing in mainwindow.cpp).
    double snrEma_ = std::numeric_limits<double>::quiet_NaN();
    qint64 snrEmaMs_ = 0;
    qint64 snrFirstMs_ = 0;     // first report since the decoder started; peak waits out the acquisition
    float snrShownPeak_ = std::numeric_limits<float>::quiet_NaN();
    std::deque<std::pair<qint64, float>> snrReports_;
    void resetSnrSmoothing();
    float lastCarrierHz_ = std::numeric_limits<float>::quiet_NaN();
    class GgakPage* ggakPage_ = nullptr;
    class SpaceWeatherPage* spaceWeatherPage_ = nullptr;
    class GlobePage* globePage_ = nullptr;
    // Orbit / pass prediction (passpage.*); the ground station it uses is entered on the Settings page.
    class PassPage* passPage_ = nullptr;
    // Every stage of the receive chain, live (decoderpage.*).
    class DecoderPage* decoderPage_ = nullptr;
    QDoubleSpinBox* stationLat_ = nullptr;
    QDoubleSpinBox* stationLon_ = nullptr;
    QDoubleSpinBox* stationAlt_ = nullptr;
    QDoubleSpinBox* stationHorizon_ = nullptr;
    SystemMonitorPage* systemPage_ = nullptr;
    TelemetryPage* telemetryPage_ = nullptr;

    QDoubleSpinBox* freq_ = nullptr;
    QComboBox* satelliteSelector_ = nullptr;
    QDoubleSpinBox* gain_ = nullptr;
    QComboBox* sampleRate_ = nullptr;
    QDoubleSpinBox* sampleRateManual_ = nullptr;   // MS/s, shown when "Manual" is chosen
    QComboBox* bandwidth_ = nullptr;
    QDoubleSpinBox* bandwidthManual_ = nullptr;   // MHz, shown when "Manual" is chosen
    QComboBox* rrcAlpha_ = nullptr;
    QComboBox* pllBandwidth_ = nullptr;
    QComboBox* fftSize_ = nullptr;
    QComboBox* fftWindow_ = nullptr;
    QComboBox* fftAvgNum_ = nullptr;
    QPushButton* rxBtn_ = nullptr;
    QPushButton* decodeBtn_ = nullptr;
    // Records the live receiver's IQ (Pluto: .cs16, RTL-SDR: .cu8) under <output root>/IQ_Recordings.
    // Checked = armed: recording runs whenever a live receiver does.
    QPushButton* recordBtn_ = nullptr;
    QTimer* recordTimer_ = nullptr;
    QComboBox* recordFormat_ = nullptr;   // Auto / cs16 / cs8 / cu8 / WAV
    QComboBox* recordLimit_ = nullptr;    // stop after N minutes
    QElapsedTimer recordClock_;
    std::shared_ptr<nadirstation::IqRecorder> iqRecorder_ = std::make_shared<nadirstation::IqRecorder>();
    void startIqRecording(double sampleRate, double centreHz, bool rtl);
    // Values are the Input box indices.
    enum class InputSource { None = -1, Pluto, Rtl, File, HackRf };
    InputSource activeInput_ = InputSource::None;
    void finishRx(bool inputFinished);
    void stopIqRecording();
    void refreshRecordButton();
    QLabel* connLabel_ = nullptr;
    QComboBox* inputSource_ = nullptr;
    QWidget* rtlOptions_ = nullptr;
    QSpinBox* rtlDeviceIndex_ = nullptr;
    QCheckBox* rtlAutoBiasTee_ = nullptr;
    QWidget* hackrfOptions_ = nullptr;
    QSpinBox* hackrfDeviceIndex_ = nullptr;
    QCheckBox* hackrfAntennaPower_ = nullptr;
    QDoubleSpinBox* freqShift_ = nullptr;   // kHz, issue #4
    double frequencyShiftHz() const;
    double sampleRateHz() const;    // Sample Rate box in Hz (the typed value when Manual is chosen)
    double rfBandwidthHz() const;   // RF bandwidth box: 0 = None (automatic), otherwise Hz
    QLineEdit* wavPath_ = nullptr;
    QPushButton* wavBrowse_ = nullptr;
    QComboBox* wavSpeed_ = nullptr;
    QComboBox* wavRange_ = nullptr;
    QLabel* snr_ = nullptr;
    QLabel* peakSnr_ = nullptr;
    QLabel* avgSnr_ = nullptr;
    QLabel* carrier_ = nullptr;
    // Builds a read-out tile (caption above, value below) and returns the value label.
    QWidget* metricTile(const QString& caption, QLabel*& value, const QString& tip = QString());
    // Live page, right column: decoder table and SNR strip chart.
    void refreshDecoderPanel(bool receiving);
    class SnrTrend* snrTrend_ = nullptr;
    class SnrMeter* snrMeter_ = nullptr;
    QLabel* decModulation_ = nullptr;
    QLabel* decSymbolRate_ = nullptr;
    QLabel* decSps_ = nullptr;
    QLabel* decLock_ = nullptr;
    QLabel* decFrames_ = nullptr;
    QLabel* decQuality_ = nullptr;
    // Session statistics: time since the input started, decoded-frame rate (smoothed), and a
    // summary line in the event log when the input stops.
    QLabel* decSession_ = nullptr;
    QLabel* decRate_ = nullptr;
    QElapsedTimer sessionClock_;
    bool sessionRunning_ = false;
    int rateLastFrames_ = 0;
    qint64 rateLastMs_ = 0;
    double frameRate_ = 0.0;
    float sessionPeakSnr_ = std::numeric_limits<float>::quiet_NaN();
    // Flash the taskbar entry and beep when a locked link drops (Receiver menu, remembered).
    bool alertOnLoss_ = false;
    void saveWindowScreenshot();
    void saveEventLog();
    int lastLock_ = 0;
    int lastFrames_ = 0;
    int lastBad_ = 0;
    float lastQuality_ = 0.0f;
    SpectrumWidget* spectrum_ = nullptr;
    ConstellationWidget* constellation_ = nullptr;
    QLabel* decodeState_ = nullptr;
    QLabel* preview_ = nullptr;

    QListWidget* sessions_ = nullptr;
    // Sessions dropped from the list to keep it bounded. The
    // directories remain on disk.
    int droppedSessionCount_ = 0;
    // Set when an HRIT reception or decode is stopped: the observation it closes is imported and
    // the image page brought forward.
    bool hritShowOnFinalize_ = false;
    bool medianUser_ = true;   // the 3x3 median checkbox; never applied to GK-2A products
    void applyMedianPolicy();
    bool saveHritObservation();
    QLabel* sessionsHint_ = nullptr;
    ImageView* imageView_ = nullptr;
    QComboBox* product_ = nullptr;          // every picture of the observation (hidden; indexes currentPaths_)
    QComboBox* instrumentPick_ = nullptr;   // the instrument (or product family) ...
    QComboBox* imagePick_ = nullptr;        // ... and its pictures; drive product_
    bool pickersRebuildQueued_ = false;
    void rebuildProductPickers();
    void fillImagePick();
    static QString productGroupOf(const QString& path);
    // RGB composite from a channel formula ("ch2, ch2, ch1"); see polar::composeRgb.
    QLineEdit* rgbFormula_ = nullptr;
    QCheckBox* rgbStretch_ = nullptr;
    QLabel* rgbChannels_ = nullptr;
    void composeRgbFromFormula();
    void refreshRgbChannels();
    QDoubleSpinBox* projLat_ = nullptr;
    QDoubleSpinBox* projLon_ = nullptr;
    QDoubleSpinBox* projSpan_ = nullptr;
    QPushButton* saveImage_ = nullptr;
    QPushButton* visCalButton_ = nullptr;
    // GK-2A HRIT five-channel calibrated projection: generated only when its button is pressed.
    QPushButton* hritProjectButton_ = nullptr;
    bool hritProjectBusy_ = false;
    void finishHritProjection(const QString& observationDir, const QStringList& views, const QString& message);
    QPushButton* importSession_ = nullptr;
    QLabel* imageInfo_ = nullptr;
    QLabel* hoverTemperaturePopup_ = nullptr;
    QLabel* satelliteState_ = nullptr;
    // The side panel describes the selected service, so these are members rather
    // than literals: a fixed label naming FY-2 while GK-2A is selected is a
    // statement about the signal, and it was the wrong one.
    QLabel* modeNote_ = nullptr;
    QLabel* chainHint_ = nullptr;
    QLineEdit* outputPathEdit_ = nullptr;
    QPlainTextEdit* logViewer_ = nullptr;
    QLabel* localTimeLabel_ = nullptr;
    QLabel* utcTimeLabel_ = nullptr;
    QLabel* stationSettingsStatus_ = nullptr;
    QLabel* satelliteSettingsStatus_ = nullptr;
    QLabel* calibrationSettingsStatus_ = nullptr;
    QLabel* storageSettingsStatus_ = nullptr;
    QTimer* settingsRefreshTimer_ = nullptr;
    QString outputRootPath_;
    qint64 displayedLogSize_ = -1;

    std::unique_ptr<PlutoReceiver> receiver_;
    std::unique_ptr<WavReceiver> wavReceiver_;
    std::unique_ptr<RtlSdrReceiver> rtlReceiver_;
    std::unique_ptr<HackRfReceiver> hackrfReceiver_;
    std::unique_ptr<NadirStationDecoder> decoder_;
    QThread* decoderThread_ = nullptr;
    struct DisplayMailbox {
        std::mutex mutex;
        std::vector<float> spectrum;
        double sampleRate = 0.0;
        std::shared_ptr<std::vector<std::complex<float>>> constellation;
    };
    std::shared_ptr<DisplayMailbox> displayMailbox_ = std::make_shared<DisplayMailbox>();
    QTimer* displayTimer_ = nullptr;

    QStringList currentPaths_;
    QString currentTimestamp_;
    QImage currentImage_;
    bool nativeDisplay_ = false;   // current image is an original-size channel image shown unreduced
    QImage hoverTbbImage_;
    QString hoverTbbLabel_;
    QString currentImagePath_;
    QString selectedSatelliteName_;
    double selectedSatelliteLongitudeDeg_ = 0.0;
    bool satelliteSelected_ = false;
    bool diagnosticWavMode_ = false;
    QTimer* scheduledStart_ = nullptr;
    QTimer* scheduledStop_ = nullptr;
};
