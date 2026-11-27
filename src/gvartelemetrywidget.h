#pragma once
// "GOES GVAR" page of the Telemetry tab: block 0 (documentation block) fields, scan/frame geometry, link-quality
// trend and the other non-imager blocks. Fed by NadirStationDecoder::gvarTelemetryUpdated about once a second.
#include <QByteArray>
#include <QList>
#include <QVector>
#include <QWidget>

class QLabel;
class QPlainTextEdit;
class QTableWidget;
class QTabWidget;

class GvarGeometryView;
class GvarTrendView;

class GvarTelemetryWidget final : public QWidget {
public:
    explicit GvarTelemetryWidget(QWidget* parent = nullptr);

    // block0: newest block 0 information field; consensus: 278 documentation bytes; votes: per field; stats: {window,
    // blocks0, parityOk, bit errors vs consensus, bits compared}; productInfo: 5 ints per product; productPayloads: latest payloads.
    void setTelemetry(const QByteArray& block0, const QByteArray& consensus, const QVector<int>& votes, const QVector<int>& stats,
                      const QVector<int>& productInfo, const QList<QByteArray>& productPayloads);
    void clear();

private:
    void refreshProductDump();

    QLabel* chips_[12]{};
    GvarGeometryView* geometry_ = nullptr;
    GvarTrendView* errorTrend_ = nullptr;
    GvarTrendView* lineTrend_ = nullptr;
    QTableWidget* fieldTable_ = nullptr;
    QTableWidget* productTable_ = nullptr;
    QPlainTextEdit* productDump_ = nullptr;
    QPlainTextEdit* rawDump_ = nullptr;
    QTabWidget* tabs_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QList<QByteArray> payloads_;
    QVector<int> productInfo_;
    QByteArray lastBlock0_, lastConsensus_;
    QVector<double> errorHistory_, lineHistory_;
};
