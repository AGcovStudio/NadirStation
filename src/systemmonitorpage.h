#pragma once

#include <QWidget>
#include <QTimer>
#include <QLabel>
#include <array>

class SystemMonitorPage final : public QWidget {
    Q_OBJECT
public:
    explicit SystemMonitorPage(QWidget* parent = nullptr);
    void setActive(bool active);
    void setStationSummary(const QString& state, const QString& input,
                          const QString& sampleRate, const QString& bandwidth,
                          double gainDb);
    void setDecoderSummary(const QString& summary);

signals:
    void refreshStationRequested();

private slots:
    void sampleSystem();

private:
    void setMetric(QLabel* value, QLabel* detail, const QString& main, const QString& sub);

    QTimer timer_;
    QLabel* cpuValue_ = nullptr;
    QLabel* cpuDetail_ = nullptr;
    QLabel* memoryValue_ = nullptr;
    QLabel* memoryDetail_ = nullptr;
    QLabel* processValue_ = nullptr;
    QLabel* processDetail_ = nullptr;
    QLabel* diskValue_ = nullptr;
    QLabel* diskDetail_ = nullptr;
    QLabel* stationState_ = nullptr;
    QLabel* receiverInput_ = nullptr;
    QLabel* receiverConfig_ = nullptr;
    QLabel* decoderSummary_ = nullptr;
    QLabel* coreInfo_ = nullptr;
    QLabel* osInfo_ = nullptr;
    QLabel* refreshInfo_ = nullptr;
    QWidget* chart_ = nullptr;
    QWidget* memoryChart_ = nullptr;
    std::array<double, 240> cpuHistory_{};
    std::array<double, 240> memoryHistory_{};
    int historyCount_ = 0;
    int historyPos_ = 0;
    quint64 previousIdle_ = 0;
    quint64 previousKernel_ = 0;
    quint64 previousUser_ = 0;
    qint64 lastDiskSampleMs_ = 0;
    QString cachedDiskValue_;
    QString cachedDiskDetail_;
};
