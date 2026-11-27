#pragma once
// The Decoder page: the receive chain drawn as a row of stages (IQ input → carrier → demodulator →
// frame sync → error correction → data layer → products), live. Each stage is green, amber or red
// with the reason; the first red one is where the problem is. Under it: every stage's health over
// the last five minutes, where data is lost along the chain, the selected stage's numbers with a
// scope, and a log of every change of state.
//
// Fed by NadirStationDecoder::pipelineSnapshot (about 4 Hz) and by the main window (receiver state,
// constellation points, spectrum, receiver sample-loss reports).
#include "pipelinediag.h"
#include <QElapsedTimer>
#include <QPointer>
#include <QTimer>
#include <QWidget>
#include <complex>
#include <deque>
#include <vector>

class QLabel;
class QPlainTextEdit;
class QPushButton;
class PipelineFlowView;
class HealthTimelineView;
class LossFunnelView;
class StageScopeView;
class MetricListView;

class DecoderPage : public QWidget {
    Q_OBJECT
public:
    explicit DecoderPage(QWidget* parent = nullptr);

    // Animate and repaint only while the page is shown; data is taken in either way.
    void setActive(bool active);
    // Called twice a second by the main window.
    void setRunState(bool receiving, bool decoding, bool fileInput, const QString& satellite);

    // Called directly from the main window's display timer. Deliberately not slots: as slots, moc
    // would instantiate QMetaTypeId<std::vector<...>> here, before nadirstationdecoder.h declares
    // those metatypes, and the build fails with "specialization after instantiation".
    void setConstellation(const std::vector<std::complex<float>>& points);
    void setSpectrum(const std::vector<float>& db, double sampleRate);

public slots:
    void setSnapshot(const nadirstation::diag::Snapshot& snapshot);
    // A receiver's own "samples lost" report (USB / network overflow before the decoder queue).
    void noteReceiverLoss(const QString& text);

private slots:
    void tick();                      // 1 Hz: timeline column, staleness, verdict
    void select(int index);           // show this stage in the detail panel

private:
    void present();                   // pushes the shown snapshot into the views
    nadirstation::diag::Snapshot decorated() const;   // the snapshot plus what only the GUI knows
    nadirstation::diag::Snapshot idleTemplate() const;
    void logTransition(const nadirstation::diag::Stage& st, int from);

    nadirstation::diag::Snapshot last_;
    bool haveSnapshot_ = false;
    QElapsedTimer sinceSnapshot_;
    bool receiving_ = false, decoding_ = false, fileInput_ = false, active_ = false, paused_ = false;
    QString satellite_;
    int selected_ = 3;
    QVector<int> shownHealth_;        // per stage, for the transition log
    QVector<int> pendingHealth_, pendingCount_;   // debounce for the log
    QVector<int> secondWorst_;                    // worst health per stage since the last timeline column
    QStringList secondWhy_;
    int receiverLossCount_ = 0;
    QString receiverLossText_;
    QElapsedTimer sinceReceiverLoss_;
    // Per stage: the spark value of every snapshot (last 300) and the worst health of every second.
    QVector<std::deque<double>> spark_;
    std::vector<std::complex<float>> points_;
    std::vector<float> spectrum_;
    double spectrumRate_ = 0.0;

    QLabel* subtitle_ = nullptr;
    QLabel* verdict_ = nullptr;
    QWidget* verdictBox_ = nullptr;
    QLabel* uptime_ = nullptr;
    QPushButton* pause_ = nullptr;
    PipelineFlowView* flow_ = nullptr;
    HealthTimelineView* timeline_ = nullptr;
    LossFunnelView* funnel_ = nullptr;
    StageScopeView* scope_ = nullptr;
    MetricListView* metrics_ = nullptr;
    QLabel* stageTitle_ = nullptr;
    QLabel* stageDetail_ = nullptr;
    QLabel* stageWhy_ = nullptr;
    QPlainTextEdit* log_ = nullptr;
    QTimer secondTimer_;
};
