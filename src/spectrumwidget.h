#pragma once
#include <QAction>
#include <QElapsedTimer>
#include <QImage>
#include <QPointF>
#include <QTimer>
#include <QWidget>
#include <array>
#include <deque>
#include <limits>
#include <vector>

// SatDump-style spectrum view. Upper part: the averaged power spectrum, drawn as a filled trace that
// glides towards each new frame (60 fps, exponential smoothing) instead of jumping once per FFT.
// Lower part: the waterfall, newest row at the top, kept at screen resolution so a new spectrum
// scrolls it by one pixel row and painting is two unscaled blits. The frequency axis is absolute
// (centre frequency +- half the sample rate) once the centre is known. A slider strip on the right
// carries the two cut-offs (dB) that set both the trace's vertical range and the waterfall colours;
// double-click it for the automatic range. Hovering shows the frequency and level under the cursor.
// Tools: peak hold (a second, amber trace keeping each bin's maximum), freeze, two click markers
// with their difference, an in-band C/N estimate inside the signal bandwidth, and a PNG snapshot;
// all on the right-click menu, and the actions are public so the window menu can carry them.
class SpectrumWidget : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumWidget(QWidget* parent = nullptr);
    void setSpectrum(const std::vector<float>& db, float sampleRate);
    void setAverageCount(int count);
    // Centre of the band in Hz; 0 shows offsets from the centre instead of absolute frequencies.
    void setCenterFrequency(double hz);
    // Expected occupied bandwidth of the selected signal (symbol rate x (1 + roll-off)), drawn as two
    // markers around the centre; 0 hides them.
    void setSignalBandwidth(double hz);
    // Where "Save spectrum image" writes; created on demand.
    void setSnapshotDirectory(const QString& dir) { snapshotDir_ = dir; }
    QAction* peakHoldAction() const { return peakAction_; }
    QAction* freezeAction() const { return freezeAction_; }
    QAction* saveImageAction() const { return saveAction_; }
    QAction* clearMarkersAction() const { return clearMarkersAction_; }
    // Mean in-band power over the noise floor outside the signal, dB; NaN when it cannot be measured.
    float inBandCnDb() const { return cnDb_; }
    // A narrow line (a beacon): the strongest averaged bin within offsetHz +- spanHz/2 of the centre,
    // over the median of the whole band (dB). NaN when there is no spectrum yet.
    float peakOverFloorDb(double offsetHz, double spanHz) const;
    QString saveSnapshot();
signals:
    void snapshotSaved(const QString& path);
protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;
    void contextMenuEvent(QContextMenuEvent*) override;
    void showEvent(QShowEvent*) override;
    void hideEvent(QHideEvent*) override;
private:
    enum class Handle { None, Upper, Lower };
    struct Layout { QRectF spec, wf, strip; };
    Layout layout() const;
    double stripY(float db, const QRectF& strip) const;
    float stripDb(double y, const QRectF& strip) const;
    Handle handleAt(const QPointF& pos) const;
    void setRange(float lo, float hi);
    void animate();
    // Waterfall at display resolution.
    void ensureWaterfallImage();
    void renderWaterfallRow(int imageRow, const float* historyRow);
    void rebuildWaterfall();
    double frequencyAt(double x, const QRectF& r) const;
    QString frequencyLabel(double hz, double stepHz) const;
    float levelAt(double hz) const;
    void measureCn();

    QAction* peakAction_ = nullptr;
    QAction* freezeAction_ = nullptr;
    QAction* saveAction_ = nullptr;
    QAction* clearMarkersAction_ = nullptr;
    QAction* autoRangeAction_ = nullptr;
    std::vector<float> peakDb_;
    std::vector<double> markerHz_;      // absolute (or offset when no centre is known), at most two
    QPointF pressPos_;
    float cnDb_ = std::numeric_limits<float>::quiet_NaN();
    QString snapshotDir_;

    // Spectrum: running power average over averageCount_ frames, and the smoothed trace drawn.
    std::vector<float> avgDb_;
    std::vector<float> shownDb_;
    std::vector<float> powerSum_;
    std::deque<std::vector<float>> powerHistory_;
    float sampleRate_ = 6000000.0f;
    double centerHz_ = 0.0;
    bool haveSpectrum_ = false;
    bool traceSettled_ = true;
    int averageCount_ = 8;
    QTimer animation_;
    QElapsedTimer lastFrame_;

    float lowDb_ = -110.0f;
    float highDb_ = -20.0f;
    bool autoRange_ = true;
    Handle drag_ = Handle::None;
    bool hover_ = false;
    QPointF hoverPos_;

    // History of raw dB rows (ring, newest first), reduced to at most kMaxColumns columns by taking
    // the maximum of each group so narrow carriers stay visible.
    static constexpr int kRows = 1024;
    static constexpr int kMaxColumns = 2048;
    int columns_ = 0;
    int newestRow_ = -1;
    int rowsFilled_ = 0;
    std::vector<float> history_;
    std::vector<qint64> rowTimeMs_;   // when each history row arrived, for the waterfall time scale
    QElapsedTimer clock_;
    double signalBwHz_ = 0.0;
    // The coloured waterfall, exactly the waterfall rectangle's size, also a ring (newest at
    // imageNewest_). Rebuilt from history_ on resize or when the cut-offs move.
    QImage waterfall_;
    int imageNewest_ = -1;
    bool rebuildPending_ = true;
    std::array<QRgb, 256> lut_{};
};
