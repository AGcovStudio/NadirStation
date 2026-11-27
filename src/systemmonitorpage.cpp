#include "systemmonitorpage.h"

#include <QDateTime>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QStorageInfo>
#include <QSysInfo>
#include <QThread>
#include <QVBoxLayout>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {
QString bytesText(quint64 bytes) {
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return QString::number(gib, 'f', 1) + QStringLiteral(" GB");
}

QFrame* makeCard(const QString& eyebrow, QLabel*& value, QLabel*& detail) {
    auto* card = new QFrame;
    card->setObjectName(QStringLiteral("SystemCard"));
    auto* layout = new QVBoxLayout(card);
    layout->setContentsMargins(16, 14, 16, 14);
    layout->setSpacing(5);
    auto* title = new QLabel(eyebrow);
    title->setObjectName(QStringLiteral("SystemEyebrow"));
    value = new QLabel(QStringLiteral("—"));
    value->setObjectName(QStringLiteral("SystemValue"));
    detail = new QLabel(QStringLiteral("Collecting system data"));
    detail->setObjectName(QStringLiteral("SystemDetail"));
    layout->addWidget(title);
    layout->addWidget(value);
    layout->addWidget(detail);
    return card;
}

class SystemLoadChart final : public QWidget {
public:
    explicit SystemLoadChart(QString label, QColor color, QWidget* parent = nullptr)
        : QWidget(parent), label_(std::move(label)), color_(std::move(color)) {
        setMinimumHeight(175);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }

    void setHistory(const std::array<double, 240>& cpu,
                    int count, int pos) {
        cpu_ = cpu;
        count_ = count;
        pos_ = pos;
        update();
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.fillRect(rect(), QColor(QStringLiteral("#1c1d20")));
        const QRectF plot(48, 18, width() - 66, height() - 66);
        if (plot.width() <= 0 || plot.height() <= 0) return;

        p.setFont(QFont(QStringLiteral("Bahnschrift"), 8));
        p.setPen(QPen(QColor(QStringLiteral("#2b2d31")), 1));
        for (int i = 0; i <= 4; ++i) {
            const qreal y = plot.top() + plot.height() * i / 4.0;
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
            p.setPen(QColor(QStringLiteral("#6e737a")));
            p.drawText(QRectF(0, y - 8, 34, 16), Qt::AlignRight | Qt::AlignVCenter,
                       QStringLiteral("%1%").arg(100 - i * 25));
            p.setPen(QPen(QColor(QStringLiteral("#2b2d31")), 1));
        }
        for (int i = 0; i <= 4; ++i) {
            const qreal x = plot.left() + plot.width() * i / 4.0;
            p.setPen(QPen(QColor(QStringLiteral("#2b2d31")), 1, Qt::DashLine));
            p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        }

        const auto drawLine = [&](const std::array<double, 240>& history, const QColor& color) {
            if (count_ <= 0) return;
            QPainterPath path;
            for (int n = 0; n < count_; ++n) {
                const int index = (pos_ - count_ + n + 240) % 240;
                // Sweep forward: new samples advance from the left edge toward
                // the right over the 60-second history window.
                const qreal x = plot.left() + plot.width() * n / 239.0;
                const qreal y = plot.bottom() - plot.height() * qBound(0.0, history[index], 100.0) / 100.0;
                if (n == 0) path.moveTo(x, y); else path.lineTo(x, y);
            }
            const auto latest = (pos_ - 1 + 240) % 240;
            const qreal latestX = plot.left() + plot.width() * (count_ - 1) / 239.0;
            const qreal dotY = plot.bottom() - plot.height() * qBound(0.0, history[latest], 100.0) / 100.0;
            if (count_ > 1) {
                QPainterPath fill = path;
                fill.lineTo(latestX, plot.bottom());
                fill.lineTo(plot.left(), plot.bottom());
                fill.closeSubpath();
                QLinearGradient area(0, plot.top(), 0, plot.bottom());
                QColor areaColor = color;
                areaColor.setAlpha(34);
                area.setColorAt(0.0, areaColor);
                areaColor.setAlpha(0);
                area.setColorAt(1.0, areaColor);
                p.fillPath(fill, area);
                p.setPen(QPen(color, 2.3, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
                p.drawPath(path);
            }
            p.setPen(Qt::NoPen);
            p.setBrush(color);
            p.drawEllipse(QPointF(latestX, dotY), 3.4, 3.4);
        };
        drawLine(cpu_, color_);

        p.setPen(QColor(QStringLiteral("#9a9ea5")));
        for (int i = 0; i <= 4; ++i) {
            const qreal x = plot.left() + plot.width() * i / 4.0;
            p.drawText(QRectF(x - 22, plot.bottom() + 8, 44, 16), Qt::AlignHCenter | Qt::AlignVCenter,
                       QStringLiteral("%1 s").arg(i * 15));
        }
        p.setPen(color_);
        p.drawText(QRectF(plot.left(), height() - 25, 70, 18), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("● %1").arg(label_));
    }

private:
    std::array<double, 240> cpu_{};
    QString label_;
    QColor color_;
    int count_ = 0;
    int pos_ = 0;
};
}

SystemMonitorPage::SystemMonitorPage(QWidget* parent) : QWidget(parent) {
    auto* pageLayout = new QVBoxLayout(this);
    pageLayout->setContentsMargins(12, 12, 12, 12);
    pageLayout->setSpacing(12);

    auto* heading = new QHBoxLayout;
    auto* titleBox = new QVBoxLayout;
    auto* title = new QLabel(QStringLiteral("System overview"));
    title->setObjectName(QStringLiteral("SystemTitle"));
    auto* subtitle = new QLabel(QStringLiteral("Live hardware telemetry · 4 Hz refresh · 60 second history"));
    subtitle->setObjectName(QStringLiteral("SystemDetail"));
    titleBox->addWidget(title);
    titleBox->addWidget(subtitle);
    heading->addLayout(titleBox);
    heading->addStretch();
    auto* live = new QLabel(QStringLiteral("Live"));
    live->setObjectName(QStringLiteral("SystemLive"));
    auto* liveStack = new QVBoxLayout;
    liveStack->setSpacing(4);
    liveStack->addWidget(live, 0, Qt::AlignRight);
    refreshInfo_ = new QLabel(QStringLiteral("Waiting for sample"));
    refreshInfo_->setObjectName(QStringLiteral("SystemDetail"));
    refreshInfo_->setAlignment(Qt::AlignRight);
    liveStack->addWidget(refreshInfo_);
    heading->addLayout(liveStack);
    pageLayout->addLayout(heading);

    auto* metrics = new QGridLayout;
    metrics->setSpacing(10);
    metrics->addWidget(makeCard(QStringLiteral("CPU load"), cpuValue_, cpuDetail_), 0, 0);
    metrics->addWidget(makeCard(QStringLiteral("Memory"), memoryValue_, memoryDetail_), 0, 1);
    metrics->addWidget(makeCard(QStringLiteral("Ground station"), processValue_, processDetail_), 0, 2);
    metrics->addWidget(makeCard(QStringLiteral("System drive"), diskValue_, diskDetail_), 0, 3);
    for (int col = 0; col < 4; ++col) metrics->setColumnStretch(col, 1);
    pageLayout->addLayout(metrics);

    auto* middle = new QHBoxLayout;
    auto* chartCard = new QFrame;
    chartCard->setObjectName(QStringLiteral("SystemCard"));
    auto* chartLayout = new QVBoxLayout(chartCard);
    chartLayout->setContentsMargins(14, 12, 14, 8);
    auto* chartTitle = new QLabel(QStringLiteral("Resource history (60 s)"));
    chartTitle->setObjectName(QStringLiteral("SystemEyebrow"));
    auto* chartStack = new QVBoxLayout;
    chartStack->setSpacing(4);
    chart_ = new SystemLoadChart(QStringLiteral("CPU"), QColor(QStringLiteral("#8f86c9")));
    memoryChart_ = new SystemLoadChart(QStringLiteral("Memory"), QColor(QStringLiteral("#8cc0ee")));
    chartLayout->addWidget(chartTitle);
    chartStack->addWidget(chart_, 1);
    chartStack->addWidget(memoryChart_, 1);
    chartLayout->addLayout(chartStack, 1);
    middle->addWidget(chartCard, 7);

    auto* station = new QFrame;
    station->setObjectName(QStringLiteral("SystemCard"));
    auto* stationLayout = new QVBoxLayout(station);
    stationLayout->setContentsMargins(18, 16, 18, 16);
    stationLayout->setSpacing(8);
    auto* stationTitle = new QLabel(QStringLiteral("Receiver status"));
    stationTitle->setObjectName(QStringLiteral("SystemEyebrow"));
    stationState_ = new QLabel(QStringLiteral("IDLE · READY"));
    stationState_->setObjectName(QStringLiteral("SystemValueSmall"));
    receiverInput_ = new QLabel(QStringLiteral("PlutoSDR · 192.168.3.10"));
    receiverInput_->setObjectName(QStringLiteral("SystemDetail"));
    receiverConfig_ = new QLabel(QStringLiteral("2.000 MS/s   ·   2.000 MHz\nGain 35 dB"));
    receiverConfig_->setObjectName(QStringLiteral("SystemConfig"));
    decoderSummary_ = new QLabel(QStringLiteral("Decoder waiting for telemetry"));
    decoderSummary_->setObjectName(QStringLiteral("SystemDetail"));
    decoderSummary_->setWordWrap(true);
    stationLayout->addWidget(stationTitle);
    stationLayout->addSpacing(6);
    stationLayout->addWidget(stationState_);
    stationLayout->addWidget(receiverInput_);
    stationLayout->addWidget(receiverConfig_);
    stationLayout->addWidget(decoderSummary_);
    stationLayout->addStretch();
    middle->addWidget(station, 3);
    pageLayout->addLayout(middle, 1);

    auto* systemInfo = new QFrame;
    systemInfo->setObjectName(QStringLiteral("SystemCard"));
    auto* infoLayout = new QHBoxLayout(systemInfo);
    infoLayout->setContentsMargins(18, 13, 18, 13);
    auto* infoTitle = new QLabel(QStringLiteral("Host"));
    infoTitle->setObjectName(QStringLiteral("SystemEyebrow"));
    coreInfo_ = new QLabel(QStringLiteral("CPU threads: %1").arg(QThread::idealThreadCount()));
    coreInfo_->setObjectName(QStringLiteral("SystemDetail"));
    osInfo_ = new QLabel(QStringLiteral("%1 · %2").arg(QSysInfo::prettyProductName(), QSysInfo::currentCpuArchitecture()));
    osInfo_->setObjectName(QStringLiteral("SystemDetail"));
    infoLayout->addWidget(infoTitle);
    infoLayout->addSpacing(12);
    infoLayout->addWidget(coreInfo_);
    infoLayout->addStretch();
    infoLayout->addWidget(osInfo_);
    auto* copyright = new QLabel(QStringLiteral("© 2026 AGcov (BI2QYN)"));
    copyright->setObjectName(QStringLiteral("SystemDetail"));
    infoLayout->addSpacing(18);
    infoLayout->addWidget(copyright);
    pageLayout->addWidget(systemInfo);

    timer_.setTimerType(Qt::PreciseTimer);
    timer_.setInterval(250);
    connect(&timer_, &QTimer::timeout, this, &SystemMonitorPage::sampleSystem);
    sampleSystem();
    timer_.start();
}

void SystemMonitorPage::setActive(bool active) {
    Q_UNUSED(active);
}

void SystemMonitorPage::setStationSummary(const QString& state, const QString& input,
                                          const QString& sampleRate, const QString& bandwidth,
                                          double gainDb) {
    stationState_->setText(state);
    const bool active = state.contains(QStringLiteral("RECEIVING"));
    stationState_->setStyleSheet(active
        ? QStringLiteral("color:#6fd5a1;")
        : QStringLiteral("color:#e9bd62;"));
    receiverInput_->setText(input);
    receiverConfig_->setText(QStringLiteral("%1   ·   %2\nGain %3 dB")
        .arg(sampleRate, bandwidth).arg(gainDb, 0, 'f', 0));
}

void SystemMonitorPage::setDecoderSummary(const QString& summary) {
    if (decoderSummary_) decoderSummary_->setText(summary);
}

void SystemMonitorPage::setMetric(QLabel* value, QLabel* detail, const QString& main, const QString& sub) {
    value->setText(main);
    detail->setText(sub);
}

void SystemMonitorPage::sampleSystem() {
    double cpuPercent = 0.0;
    double memoryPercent = 0.0;
    quint64 totalMemory = 0;
    quint64 availableMemory = 0;
    quint64 processBytes = 0;
#ifdef Q_OS_WIN
    FILETIME idleTime{}, kernelTime{}, userTime{};
    if (GetSystemTimes(&idleTime, &kernelTime, &userTime)) {
        const auto asTicks = [](const FILETIME& t) {
            return (static_cast<quint64>(t.dwHighDateTime) << 32) | t.dwLowDateTime;
        };
        const quint64 idle = asTicks(idleTime);
        const quint64 kernel = asTicks(kernelTime);
        const quint64 user = asTicks(userTime);
        if (previousKernel_ || previousUser_) {
            const quint64 totalDelta = (kernel - previousKernel_) + (user - previousUser_);
            const quint64 idleDelta = idle - previousIdle_;
            if (totalDelta) cpuPercent = 100.0 * (1.0 - static_cast<double>(idleDelta) / totalDelta);
        }
        previousIdle_ = idle;
        previousKernel_ = kernel;
        previousUser_ = user;
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (GlobalMemoryStatusEx(&memory)) {
        totalMemory = memory.ullTotalPhys;
        availableMemory = memory.ullAvailPhys;
        memoryPercent = memory.dwMemoryLoad;
    }
    PROCESS_MEMORY_COUNTERS counters{};
    if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)))
        processBytes = static_cast<quint64>(counters.WorkingSetSize);
#endif
    cpuPercent = qBound(0.0, cpuPercent, 100.0);
    memoryPercent = qBound(0.0, memoryPercent, 100.0);
    setMetric(cpuValue_, cpuDetail_, QStringLiteral("%1%").arg(cpuPercent, 0, 'f', 0),
              QStringLiteral("%1 logical threads").arg(QThread::idealThreadCount()));
    setMetric(memoryValue_, memoryDetail_, QStringLiteral("%1%").arg(memoryPercent, 0, 'f', 0),
              totalMemory ? QStringLiteral("%1 / %2 in use").arg(bytesText(totalMemory - availableMemory), bytesText(totalMemory))
                          : QStringLiteral("Memory telemetry unavailable"));
    setMetric(processValue_, processDetail_, processBytes ? bytesText(processBytes) : QStringLiteral("—"),
              QStringLiteral("FY-2G Station working set"));

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (lastDiskSampleMs_ == 0 || nowMs - lastDiskSampleMs_ >= 10000) {
        const QStorageInfo drive = QStorageInfo::root();
        if (drive.isValid() && drive.isReady() && drive.bytesTotal() > 0) {
            const auto total = static_cast<quint64>(drive.bytesTotal());
            const auto free = static_cast<quint64>(drive.bytesAvailable());
            const auto usedPercent = 100.0 * static_cast<double>(total - free) / total;
            cachedDiskValue_ = QStringLiteral("%1%").arg(usedPercent, 0, 'f', 0);
            cachedDiskDetail_ = QStringLiteral("%1 free of %2").arg(bytesText(free), bytesText(total));
        } else {
            cachedDiskValue_ = QStringLiteral("—");
            cachedDiskDetail_ = QStringLiteral("Drive telemetry unavailable");
        }
        lastDiskSampleMs_ = nowMs;
    }
    setMetric(diskValue_, diskDetail_, cachedDiskValue_, cachedDiskDetail_);

    cpuHistory_[historyPos_] = cpuPercent;
    memoryHistory_[historyPos_] = memoryPercent;
    historyPos_ = (historyPos_ + 1) % static_cast<int>(cpuHistory_.size());
    historyCount_ = qMin(historyCount_ + 1, static_cast<int>(cpuHistory_.size()));
    static_cast<SystemLoadChart*>(chart_)->setHistory(cpuHistory_, historyCount_, historyPos_);
    static_cast<SystemLoadChart*>(memoryChart_)->setHistory(memoryHistory_, historyCount_, historyPos_);
    if (refreshInfo_)
        refreshInfo_->setText(QStringLiteral("%1   ·   250 ms").arg(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))));
    emit refreshStationRequested();
}
