#pragma once
#include <QWidget>
#include <QVector>
#include <QByteArray>
#include <QList>

class QLabel;
class QTableWidget;
class QPlainTextEdit;
class QWidget;
class QComboBox;
class GvarTelemetryWidget;

class TelemetryPage final : public QWidget {
    Q_OBJECT
public:
    explicit TelemetryPage(QWidget* parent = nullptr);
public slots:
    void updateTelemetry(const QByteArray& statusBlock);
    void updateDocPayload(const QByteArray& payload, bool crcPassed);
    void updateOrbitAttitude(const QByteArray& block);
    void updateSatelliteGeometry(int satelliteId, double longitudeDeg, int confirmations, bool confirmed);
    void updateCalibration2(const QByteArray& block);
    void resetCalibration2();
    void updateManamProgress(int revision, int groupMask);
    void updateManam(int revision, const QByteArray& scheduleText);
    void resetManam();
    // GOES GVAR page (block 0 and the other non-imager blocks)
    void updateGvarTelemetry(const QByteArray& block0, const QByteArray& consensus, const QVector<int>& votes, const QVector<int>& stats,
                             const QVector<int>& productInfo, const QList<QByteArray>& productPayloads);
    void resetGvar();
private:
    QLabel* values_[16]{};
    QLabel* linkState_ = nullptr;
    QLabel* progressLabel_ = nullptr;
    QLabel* timeLabel_ = nullptr;
    QTableWidget* table_ = nullptr;
    QWidget* scanChart_ = nullptr;
    QWidget* errorChart_ = nullptr;
    QLabel* manamProgressLabel_ = nullptr;
    QTableWidget* manamBlocks_ = nullptr;
    QPlainTextEdit* manamText_ = nullptr;
    QLabel* sourceStatus_ = nullptr;
    QLabel* geometryStatus_ = nullptr;
    QLabel* calibrationStatus_ = nullptr;
    QPlainTextEdit* sourcePayload_ = nullptr;
    QWidget* longitudeChart_ = nullptr;
    QWidget* orbitTrackChart_ = nullptr;
    QPlainTextEdit* orbitDetails_ = nullptr;
    QWidget* calibrationChart_ = nullptr;
    QComboBox* calibrationChannel_ = nullptr;
    GvarTelemetryWidget* gvarWidget_ = nullptr;
    QVector<QVector<float>> calibrationValues_;
    QVector<double> longitudeHistory_;
    QVector<int> scanHistory_;
    QVector<int> errorHistory_;
};
