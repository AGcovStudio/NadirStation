#include "gvartelemetrywidget.h"
#include "gvardoc.h"

#include <QAbstractItemView>
#include <QFont>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPlainTextEdit>
#include <QScrollArea>
#include <QScrollBar>
#include <QTabWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <cstring>

using namespace nadirstation::gvar;

namespace {

const uint8_t* bytesOf(const QByteArray& b) { return reinterpret_cast<const uint8_t*>(b.constData()); }
QString fromStd(const std::string& s) { return QString::fromUtf8(s.c_str()); }

QLabel* makeChip(const QString& title, QWidget* parent) {
    auto* label = new QLabel(title + QStringLiteral("\n—"));
    label->setFixedHeight(78);
    label->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    label->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true);
    label->setStyleSheet(QStringLiteral("QLabel{background:#232427;border:1px solid #35373c;border-radius:2px;padding:6px 10px;color:#eceef0;font-size:12px;font-weight:600;}"));
    label->setParent(parent);
    return label;
}

QFont monoFont() {
    QFont f(QStringLiteral("Consolas"));
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(9);
    return f;
}

QString hexDump(const QByteArray& data, int limit) {
    QString out;
    const int n = std::min<int>(data.size(), limit);
    for (int off = 0; off < n; off += 16) {
        QString hex, asc;
        for (int k = 0; k < 16; ++k) {
            if (off + k < n) {
                const unsigned char c = static_cast<unsigned char>(data[off + k]);
                hex += QStringLiteral("%1 ").arg(uint(c), 2, 16, QLatin1Char('0')).toUpper();
                asc += (c >= 32 && c < 127) ? QChar(c) : QChar('.');
            } else {
                hex += QStringLiteral("   ");
            }
            if (k == 7) hex += QLatin1Char(' ');
        }
        out += QString::number(off, 16).rightJustified(5, QLatin1Char('0')).toUpper() + QStringLiteral("  ") + hex + QStringLiteral(" |") + asc + QStringLiteral("|\n");
    }
    if (n < data.size()) out += QStringLiteral("… %1 more bytes not shown\n").arg(data.size() - n);
    return out;
}

double printableRatio(const QByteArray& data) {
    if (data.isEmpty()) return 0.0;
    int ok = 0;
    for (char ch : data) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if ((c >= 32 && c < 127) || c == '\n' || c == '\r' || c == '\t') ++ok;
    }
    return double(ok) / double(data.size());
}

QString signedDelta(int64_t ms) {
    const QChar sign = ms < 0 ? QChar('-') : QChar('+');
    const int64_t a = ms < 0 ? -ms : ms;
    const int64_t s = a / 1000;
    if (s >= 3600) return QStringLiteral("%1%2:%3:%4").arg(sign).arg(s / 3600).arg((s / 60) % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'));
    return QStringLiteral("%1%2.%3 s").arg(sign).arg(s).arg(a % 1000, 3, 10, QLatin1Char('0'));
}

}  // namespace

// ---- frame geometry ------------------------------------------------------------------------------

class GvarGeometryView final : public QWidget {
public:
    struct Data {
        bool valid = false;
        int west = 0, east = 0, north = 0, south = 0;   // IWFPX, IEFPX, INFLN, ISFLN
        int centreX = 0, centreY = 0;                   // IMDPX, IMDLN
        int currentLine = 0;                            // INSLN
        double subLat = 0, subLon = 0;
    };
    explicit GvarGeometryView(QWidget* parent = nullptr) : QWidget(parent) {
        setMinimumHeight(300);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::MinimumExpanding);
    }
    void setData(const Data& d) { d_ = d; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#232427"));
        p.setPen(QColor("#eceef0"));
        p.setFont(QFont(QStringLiteral("Bahnschrift"), 10, QFont::DemiBold));
        p.drawText(QRect(16, 8, width() - 32, 22), QStringLiteral("Imager frame and current scan position (block 0: IWFPX/IEFPX × INFLN/ISFLN, INSLN)"));
        // Own rows for the sub-satellite note (second header line), the north/west label (above the frame) and the
        // east/south label (below it), so no text shares a place with the frame, the scan line or another label.
        p.setFont(QFont(QStringLiteral("Bahnschrift"), 8));
        p.setPen(QColor("#9a9ea5"));
        if (d_.valid) p.drawText(QRect(16, 30, width() - 32, 16), Qt::AlignLeft | Qt::AlignVCenter,
                                 QStringLiteral("Sub-point %1° / %2° (raw)").arg(d_.subLat, 0, 'f', 2).arg(d_.subLon, 0, 'f', 2));
        const QRectF area(16, 70, width() - 32, height() - 70 - 30);
        if (!d_.valid || d_.east <= d_.west || d_.south <= d_.north) {
            p.setPen(QColor("#9a9ea5"));
            p.drawText(area, Qt::AlignCenter, QStringLiteral("Waiting for the frame fields of block 0"));
            return;
        }
        // Drawing space: the symmetric range around the frame centre that contains the frame, plus a margin.
        const double halfX = std::max<double>({double(d_.centreX - d_.west), double(d_.east - d_.centreX), 1.0});
        const double halfY = std::max<double>({double(d_.centreY - d_.north), double(d_.south - d_.centreY), 1.0});
        const double x0 = d_.centreX - halfX * 1.08, x1 = d_.centreX + halfX * 1.08;
        const double y0 = d_.centreY - halfY * 1.08, y1 = d_.centreY + halfY * 1.08;
        // keep the data aspect ratio (a pixel is not a line: use equal pixel/line scale, as GVAR does for the picture)
        const double sx = area.width() / (x1 - x0), sy = area.height() / (y1 - y0);
        const double s = std::min(sx, sy * 2.0);          // a GVAR line is about two pixels tall in angle
        const double pw = (x1 - x0) * s, ph = (y1 - y0) * s / 2.0;
        const QPointF origin(area.center().x() - pw / 2.0, area.center().y() - ph / 2.0);
        const auto mapX = [&](double x) { return origin.x() + (x - x0) * s; };
        const auto mapY = [&](double y) { return origin.y() + (y - y0) * s / 2.0; };

        p.setPen(QPen(QColor("#35373c"), 1, Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawRect(QRectF(origin, QSizeF(pw, ph)));

        const QRectF frame(QPointF(mapX(d_.west), mapY(d_.north)), QPointF(mapX(d_.east), mapY(d_.south)));
        p.setPen(QPen(QColor("#7cc6bb"), 2));
        p.setBrush(QColor(124, 198, 187, 28));
        p.drawRect(frame);

        const double line = std::clamp<double>(d_.currentLine, d_.north, d_.south);
        const QRectF done(frame.left(), frame.top(), frame.width(), mapY(line) - frame.top());
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(242, 185, 111, 70));
        p.drawRect(done);
        p.setPen(QPen(QColor("#f2b96f"), 2));
        p.drawLine(QPointF(frame.left() - 6, mapY(line)), QPointF(frame.right() + 6, mapY(line)));

        p.setPen(QPen(QColor("#9a9ea5"), 1));
        const QPointF c(mapX(d_.centreX), mapY(d_.centreY));
        p.drawLine(c + QPointF(-6, 0), c + QPointF(6, 0));
        p.drawLine(c + QPointF(0, -6), c + QPointF(0, 6));

        p.setFont(QFont(QStringLiteral("Bahnschrift"), 8));
        p.setPen(QColor("#9a9ea5"));
        p.drawText(QRectF(area.left(), area.top() - 20, area.width(), 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("W %1 · N %2").arg(d_.west).arg(d_.north));
        p.drawText(QRectF(area.left(), area.bottom() + 6, area.width(), 16), Qt::AlignRight | Qt::AlignVCenter,
                   QStringLiteral("E %1 · S %2").arg(d_.east).arg(d_.south));
        const double progress = 100.0 * (line - d_.north) / double(d_.south - d_.north);
        p.setPen(QColor("#f2b96f"));
        p.drawText(QRectF(area.left(), area.bottom() + 6, area.width() / 2.0, 16), Qt::AlignLeft | Qt::AlignVCenter,
                   QStringLiteral("scan line %1 · %2%").arg(d_.currentLine).arg(progress, 0, 'f', 1));
    }

private:
    Data d_;
};

// ---- trend line ----------------------------------------------------------------------------------

class GvarTrendView final : public QWidget {
public:
    GvarTrendView(QString title, QString unit, QColor color, QWidget* parent = nullptr)
        : QWidget(parent), title_(std::move(title)), unit_(std::move(unit)), color_(std::move(color)) {
        setMinimumHeight(190);
        setMinimumWidth(260);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    }
    void setValues(const QVector<double>& v) { values_ = v; update(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillRect(rect(), QColor("#232427"));
        p.setPen(QColor("#eceef0"));
        p.setFont(QFont(QStringLiteral("Bahnschrift"), 10, QFont::DemiBold));
        p.drawText(QRect(16, 8, width() - 32, 22), p.fontMetrics().elidedText(title_, Qt::ElideRight, width() - 32));
        const QRectF plot(54, 48, width() - 68, height() - 48 - 26);
        p.setPen(QPen(QColor("#35373c"), 1));
        for (int i = 0; i < 4; ++i) {
            const qreal y = plot.top() + plot.height() * i / 3.0;
            p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
        }
        if (values_.isEmpty()) {
            p.setPen(QColor("#9a9ea5"));
            p.drawText(plot, Qt::AlignCenter, QStringLiteral("Waiting for block 0"));
            return;
        }
        double lo = *std::min_element(values_.begin(), values_.end());
        double hi = *std::max_element(values_.begin(), values_.end());
        if (lo > 0 && lo < hi * 0.6) lo = 0;
        if (hi - lo < 1e-9) { hi = lo + 1.0; }
        p.setFont(QFont(QStringLiteral("Bahnschrift"), 8));
        p.setPen(QColor("#9a9ea5"));
        p.drawText(QRectF(2, plot.top() - 7, 48, 14), Qt::AlignRight, QString::number(hi, 'g', 5));
        p.drawText(QRectF(2, plot.bottom() - 7, 48, 14), Qt::AlignRight, QString::number(lo, 'g', 5));
        p.drawText(QRectF(plot.left(), plot.bottom() + 6, plot.width(), 14), Qt::AlignRight, unit_);
        QPainterPath path;
        for (int i = 0; i < values_.size(); ++i) {
            const qreal x = values_.size() == 1 ? plot.center().x() : plot.left() + plot.width() * i / (values_.size() - 1);
            const qreal y = plot.bottom() - plot.height() * (values_[i] - lo) / (hi - lo);
            if (!i) path.moveTo(x, y); else path.lineTo(x, y);
        }
        p.setPen(QPen(color_, 2));
        p.setBrush(Qt::NoBrush);
        p.drawPath(path);
        p.setBrush(color_);
        p.setPen(Qt::NoPen);
        const qreal ly = plot.bottom() - plot.height() * (values_.back() - lo) / (hi - lo);
        p.drawEllipse(QPointF(values_.size() == 1 ? plot.center().x() : plot.right(), ly), 3.5, 3.5);
    }

private:
    QString title_, unit_;
    QColor color_;
    QVector<double> values_;
};

// ---- the page --------------------------------------------------------------------------------------

GvarTelemetryWidget::GvarTelemetryWidget(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(22, 18, 22, 18);
    root->setSpacing(12);

    auto* head = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("GOES GVAR telemetry (block 0 documentation and other non-imager blocks)"));
    title->setStyleSheet(QStringLiteral("font-size:16px;font-weight:600;color:#eceef0;"));
    statusLabel_ = new QLabel(QStringLiteral("Waiting for GVAR block 0"));
    statusLabel_->setAlignment(Qt::AlignCenter);
    statusLabel_->setStyleSheet(QStringLiteral("color:#e9bd62;background:#3a3328;border-radius:2px;padding:8px 14px;font-weight:600;"));
    head->addWidget(title);
    head->addStretch();
    head->addWidget(statusLabel_);
    root->addLayout(head);
    auto* note = new QLabel(QStringLiteral("GVAR has no forward error correction: about 0.3% symbol errors leave a few wrong bits in each 278-byte block 0."
                                            "The Consensus column takes each field's most frequent value over the last 48 blocks 0 (fields that stay fixed within a frame); Latest is the raw decode of the last block. Where they differ, the latest block has bit errors."
                                            "Fields that change every scan (scan counts, times …) have no consensus; only the latest block is shown."));
    note->setWordWrap(true);
    note->setStyleSheet(QStringLiteral("color:#9a9ea5;font-size:12px;"));
    root->addWidget(note);

    tabs_ = new QTabWidget;
    root->addWidget(tabs_, 1);

    // --- overview
    auto* overview = new QWidget;
    overview->setObjectName(QStringLiteral("gvarOverview"));
    auto* ov = new QVBoxLayout(overview);
    ov->setContentsMargins(10, 12, 10, 10);
    ov->setSpacing(10);
    const QStringList names = {QStringLiteral("Spacecraft SPCID / SPSID"), QStringLiteral("Imaging mode IMODE"), QStringLiteral("Frame IFRAM · frame start CIFST"),
                               QStringLiteral("Sub-point SUBLA / SUBLO"), QStringLiteral("Scan count RISCT / AISCT"), QStringLiteral("Current scan line INSLN"),
                               QStringLiteral("Frame (pixels × lines)"), QStringLiteral("Current time TCURR (latest block)"), QStringLiteral("Block 0 received / parity ok"),
                               QStringLiteral("Telemetry bit errors (vs consensus)"), QStringLiteral("Scan status ISCAN"), QStringLiteral("GVAR lines × pixels")};
    auto* grid = new QGridLayout;
    grid->setSpacing(8);
    for (int i = 0; i < 12; ++i) {
        chips_[i] = makeChip(names[i], overview);
        grid->addWidget(chips_[i], i / 4, i % 4);
    }
    ov->addLayout(grid);
    geometry_ = new GvarGeometryView;
    ov->addWidget(geometry_, 1);
    auto* charts = new QHBoxLayout;
    charts->setSpacing(10);
    errorTrend_ = new GvarTrendView(QStringLiteral("Block 0 stable-field bits differing from consensus · last 120 blocks"), QStringLiteral("bit"), QColor("#f08f7f"));
    lineTrend_ = new GvarTrendView(QStringLiteral("Current scan line INSLN · last 120 blocks"), QStringLiteral("line"), QColor("#7cc6bb"));
    charts->addWidget(errorTrend_);
    charts->addWidget(lineTrend_);
    ov->addLayout(charts);
    // Inside a scroll area the three blocks keep their own heights: a short window scrolls instead of squeezing the
    // frame picture and the two curves into one another.
    auto* overviewScroll = new QScrollArea;
    overviewScroll->setWidgetResizable(true);
    overviewScroll->setFrameShape(QFrame::NoFrame);
    overviewScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    overviewScroll->setWidget(overview);
    overview->setStyleSheet(QStringLiteral("QWidget#gvarOverview{background:transparent;}"));
    tabs_->addTab(overviewScroll, QStringLiteral("Overview"));

    // --- all block 0 fields
    auto* fieldsPage = new QWidget;
    auto* fl = new QVBoxLayout(fieldsPage);
    fl->setContentsMargins(10, 12, 10, 10);
    const auto& defs = docFieldDefs();
    fieldTable_ = new QTableWidget(int(defs.size()), 7);
    fieldTable_->setHorizontalHeaderLabels({QStringLiteral("Field"), QStringLiteral("Meaning"), QStringLiteral("Raw bytes"), QStringLiteral("Latest"),
                                            QStringLiteral("Consensus"), QStringLiteral("Votes"), QStringLiteral("From TCURR")});
    fieldTable_->verticalHeader()->hide();
    fieldTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    fieldTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    fieldTable_->setAlternatingRowColors(true);
    fieldTable_->setFont(monoFont());
    for (int r = 0; r < int(defs.size()); ++r) {
        fieldTable_->setItem(r, 0, new QTableWidgetItem(QString::fromLatin1(defs[size_t(r)].key)));
        fieldTable_->setItem(r, 1, new QTableWidgetItem(QString::fromUtf8(defs[size_t(r)].meaning)));
        for (int c = 2; c < 7; ++c) fieldTable_->setItem(r, c, new QTableWidgetItem(QStringLiteral("—")));
    }
    for (int c = 0; c < 6; ++c) fieldTable_->horizontalHeader()->setSectionResizeMode(c, QHeaderView::ResizeToContents);
    fieldTable_->horizontalHeader()->setSectionResizeMode(6, QHeaderView::Stretch);
    fl->addWidget(fieldTable_);
    tabs_->addTab(fieldsPage, QStringLiteral("All block 0 fields"));

    // --- other blocks
    auto* prodPage = new QWidget;
    auto* pl = new QVBoxLayout(prodPage);
    pl->setContentsMargins(10, 12, 10, 10);
    auto* pnote = new QLabel(QStringLiteral("Blocks other than block 0 and the imager blocks (block number ≥ 11). They are saved as received and shown in hexadecimal; their internal structure is not decoded field by field; "
                                            "the latest of each kind is written to the Telemetry_… / Products folder of the output directory."));
    pnote->setWordWrap(true);
    pnote->setStyleSheet(QStringLiteral("color:#9a9ea5;font-size:12px;"));
    pl->addWidget(pnote);
    productTable_ = new QTableWidget(0, 6);
    productTable_->setHorizontalHeaderLabels({QStringLiteral("Block"), QStringLiteral("Product"), QStringLiteral("Word (bit)"), QStringLiteral("Words"),
                                              QStringLiteral("Received"), QStringLiteral("Content")});
    productTable_->verticalHeader()->hide();
    productTable_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    productTable_->setSelectionBehavior(QAbstractItemView::SelectRows);
    productTable_->setSelectionMode(QAbstractItemView::SingleSelection);
    productTable_->setMaximumHeight(170);
    productTable_->horizontalHeader()->setStretchLastSection(true);
    pl->addWidget(productTable_);
    productDump_ = new QPlainTextEdit;
    productDump_->setReadOnly(true);
    productDump_->setLineWrapMode(QPlainTextEdit::NoWrap);
    productDump_->setFont(monoFont());
    productDump_->setPlaceholderText(QStringLiteral("Select a row above to see the latest copy of that block in hexadecimal"));
    pl->addWidget(productDump_, 1);
    tabs_->addTab(prodPage, QStringLiteral("Other blocks"));
    QObject::connect(productTable_, &QTableWidget::itemSelectionChanged, productTable_, [this] { refreshProductDump(); });

    // --- raw
    rawDump_ = new QPlainTextEdit;
    rawDump_->setReadOnly(true);
    rawDump_->setLineWrapMode(QPlainTextEdit::NoWrap);
    rawDump_->setFont(monoFont());
    rawDump_->setPlaceholderText(QStringLiteral("Waiting for block 0"));
    tabs_->addTab(rawDump_, QStringLiteral("Block 0 raw data"));
}

void GvarTelemetryWidget::clear() {
    lastBlock0_.clear();
    lastConsensus_.clear();
    errorHistory_.clear();
    lineHistory_.clear();
    payloads_.clear();
    productInfo_.clear();
    statusLabel_->setText(QStringLiteral("Waiting for GVAR block 0"));
    statusLabel_->setStyleSheet(QStringLiteral("color:#e9bd62;background:#3a3328;border-radius:2px;padding:8px 14px;font-weight:600;"));
    for (auto* c : chips_) {
        const QString t = c->text().section(QLatin1Char('\n'), 0, 0);
        c->setText(t + QStringLiteral("\n—"));
    }
    geometry_->setData({});
    errorTrend_->setValues({});
    lineTrend_->setValues({});
    productTable_->setRowCount(0);
    productDump_->clear();
    rawDump_->clear();
    for (int r = 0; r < fieldTable_->rowCount(); ++r)
        for (int c = 2; c < 7; ++c) fieldTable_->item(r, c)->setText(QStringLiteral("—"));
}

void GvarTelemetryWidget::refreshProductDump() {
    const auto rows = productTable_->selectionModel() ? productTable_->selectionModel()->selectedRows() : QModelIndexList();
    if (rows.isEmpty()) return;
    const int r = rows.first().row();
    if (r < 0 || r >= payloads_.size()) return;
    const int ws = productInfo_.size() >= (r + 1) * 5 ? productInfo_[r * 5 + 2] : 8;
    QString text = QStringLiteral("Block %1 · product %2 · word %3 bit · %4 bytes (bit-packed, MSB first)\n\n")
                       .arg(productInfo_[r * 5]).arg(productInfo_[r * 5 + 1]).arg(ws).arg(payloads_[r].size());
    text += hexDump(payloads_[r], 4096);
    productDump_->setPlainText(text);
}

void GvarTelemetryWidget::setTelemetry(const QByteArray& block0, const QByteArray& consensus, const QVector<int>& votes, const QVector<int>& stats,
                                       const QVector<int>& productInfo, const QList<QByteArray>& productPayloads) {
    if (block0.size() < kDocBytes || stats.size() < 5) return;
    lastBlock0_ = block0;
    lastConsensus_ = consensus;
    const auto& defs = docFieldDefs();
    const int window = stats[0], blocks0 = stats[1], parityOk = stats[2], errBits = stats[3], compared = stats[4];
    const bool haveCons = window >= 4 && consensus.size() >= kDocBytes && votes.size() == int(defs.size());
    const uint8_t* latest = bytesOf(block0);
    const uint8_t* cons = haveCons ? bytesOf(consensus) : nullptr;

    // the value of a field to use for display: the consensus when it is stable, the newest block otherwise
    const auto stable = [&](int idx) { return haveCons && idx >= 0 && idx < votes.size() && votes[idx] * 2 >= window && votes[idx] >= 3; };
    const auto best = [&](const char* key) -> const uint8_t* {
        const int idx = docFieldIndex(key);
        return stable(idx) ? cons : latest;
    };
    const auto defOf = [&](const char* key) -> const DocFieldDef& { return defs[size_t(docFieldIndex(key))]; };
    const auto uval = [&](const char* key, const uint8_t* src) { const auto& d = defOf(key); return decodeU(src + d.off, d.len); };
    const auto fval = [&](const char* key, const uint8_t* src) { return decodeGvarFloat(src + defOf(key).off); };
    const auto tval = [&](const char* key, const uint8_t* src) { return decodeBcdTime(src + defOf(key).off); };

    // --- status
    const bool parityNow = docParityOk(latest);
    statusLabel_->setText(QStringLiteral("%1 blocks 0 received · current block parity %2").arg(blocks0).arg(parityNow ? QStringLiteral("ok") : QStringLiteral("failed (bit errors)")));
    statusLabel_->setStyleSheet(QStringLiteral("color:#9fd6a8;background:#26382b;border-radius:2px;padding:8px 14px;font-weight:600;"));

    // --- chips
    const uint8_t* b;
    b = best("SPCID");
    chips_[0]->setText(QStringLiteral("Spacecraft SPCID / SPSID\n%1 / %2").arg(uval("SPCID", b)).arg(uval("SPSID", best("SPSID"))));
    b = best("IMODE");
    chips_[1]->setText(QStringLiteral("Imaging mode IMODE\n%1 · %2").arg(uval("IMODE", b)).arg(QString::fromUtf8(imodeName(int(uval("IMODE", b))))));
    {
        const DocTime cf = tval("CIFST", best("CIFST"));
        chips_[2]->setText(QStringLiteral("Frame IFRAM · frame start CIFST\n%1 · %2").arg(uval("IFRAM", best("IFRAM"))).arg(cf.valid ? fromStd(formatTimeIso(cf)).left(19) + QStringLiteral(" UTC") : QStringLiteral("—")));
    }
    chips_[3]->setText(QStringLiteral("Sub-point SUBLA / SUBLO\n%1° / %2°").arg(fval("SUBLA", best("SUBLA")), 0, 'f', 3).arg(fval("SUBLO", best("SUBLO")), 0, 'f', 3));
    chips_[4]->setText(QStringLiteral("Scan count RISCT / AISCT\n%1 / %2").arg(uval("RISCT", latest)).arg(uval("AISCT", latest)));
    const int west = int(uval("IWFPX", best("IWFPX"))), east = int(uval("IEFPX", best("IEFPX")));
    const int north = int(uval("INFLN", best("INFLN"))), south = int(uval("ISFLN", best("ISFLN")));
    const int insln = int(uval("INSLN", latest));
    {
        const double prog = south > north ? 100.0 * (insln - north) / double(south - north) : 0.0;
        chips_[5]->setText(QStringLiteral("Current scan line INSLN\n%1 · %2% through the frame").arg(insln).arg(std::clamp(prog, 0.0, 100.0), 0, 'f', 1));
    }
    chips_[6]->setText(QStringLiteral("Frame type · size (pixels × lines)\n%1 · %2 × %3").arg(QString::fromUtf8(frameKindName(west, east, north, south))).arg(east - west + 1).arg(south - north + 1));
    {
        const DocTime tc = tval("TCURR", latest);
        chips_[7]->setText(QStringLiteral("Current time TCURR (latest block)\n%1").arg(tc.valid ? fromStd(formatTimeIso(tc)).left(23) : QStringLiteral("invalid BCD")));
    }
    chips_[8]->setText(QStringLiteral("Block 0 received / parity ok\n%1 / %2").arg(blocks0).arg(parityOk));
    if (compared > 0)
        chips_[9]->setText(QStringLiteral("Telemetry bit errors (vs consensus)\n%1 / %2 bit ≈ %3").arg(errBits).arg(compared).arg(double(errBits) / double(compared), 0, 'e', 1));
    else
        chips_[9]->setText(QStringLiteral("Telemetry bit errors (vs consensus)\nneeds at least 4 blocks"));
    {
        const auto& d = defOf("ISCAN");
        const QString hexWord = QString::number(decodeU(latest + d.off, 4), 16).rightJustified(8, QLatin1Char('0')).toUpper();
        chips_[10]->setText(QStringLiteral("Scan status ISCAN\n0x%1 · set: %2").arg(hexWord, fromStd(iscanBits(latest + d.off))));
    }
    chips_[11]->setText(QStringLiteral("GVAR lines × pixels\n%1 × %2").arg(uval("IGVLN", best("IGVLN"))).arg(uval("IGVPX", best("IGVPX"))));

    // --- geometry
    GvarGeometryView::Data g;
    g.valid = east > west && south > north;
    g.west = west; g.east = east; g.north = north; g.south = south;
    g.centreX = int(uval("IMDPX", best("IMDPX")));
    g.centreY = int(uval("IMDLN", best("IMDLN")));
    if (g.centreX <= 0 || g.centreY <= 0) { g.centreX = (west + east) / 2; g.centreY = (north + south) / 2; }
    g.currentLine = insln;
    g.subLat = fval("SUBLA", best("SUBLA"));
    g.subLon = fval("SUBLO", best("SUBLO"));
    geometry_->setData(g);

    // --- trends
    errorHistory_.push_back(double(errBits));
    lineHistory_.push_back(double(insln));
    while (errorHistory_.size() > 120) errorHistory_.removeFirst();
    while (lineHistory_.size() > 120) lineHistory_.removeFirst();
    errorTrend_->setValues(errorHistory_);
    lineTrend_->setValues(lineHistory_);

    // --- field table
    const DocTime tcurr = tval("TCURR", latest);
    for (int r = 0; r < int(defs.size()); ++r) {
        const auto& d = defs[size_t(r)];
        fieldTable_->item(r, 2)->setText(fromStd(hexString(latest + d.off, std::min(d.len, 8))) + (d.len > 8 ? QStringLiteral(" …") : QString()));
        const QString latestText = fromStd(formatField(d, latest));
        fieldTable_->item(r, 3)->setText(latestText);
        QString consText = QStringLiteral("—"), votesText = QStringLiteral("—");
        bool differs = false;
        if (haveCons) {
            const bool st = stable(r);
            votesText = QStringLiteral("%1 / %2").arg(votes[r]).arg(window);
            if (st) {
                consText = fromStd(formatField(d, cons));
                differs = std::memcmp(latest + d.off, cons + d.off, size_t(d.len)) != 0;
            } else {
                consText = QStringLiteral("changes every scan");
            }
        }
        fieldTable_->item(r, 4)->setText(consText);
        fieldTable_->item(r, 5)->setText(votesText);
        fieldTable_->item(r, 3)->setForeground(differs ? QColor("#f2b96f") : QColor("#eceef0"));
        QString delta = QStringLiteral("—");
        if (d.kind == DocKind::Bcd && tcurr.valid) {
            const DocTime t = decodeBcdTime(latest + d.off);
            if (t.valid) delta = signedDelta(t.epochMs - tcurr.epochMs);
        }
        fieldTable_->item(r, 6)->setText(delta);
    }

    // --- other blocks
    int keyBlock = -1, keyProduct = -1;
    {
        const auto rows = productTable_->selectionModel() ? productTable_->selectionModel()->selectedRows() : QModelIndexList();
        if (!rows.isEmpty() && rows.first().row() * 5 + 1 < productInfo_.size()) {
            keyBlock = productInfo_[rows.first().row() * 5];
            keyProduct = productInfo_[rows.first().row() * 5 + 1];
        }
    }
    productInfo_ = productInfo;
    payloads_ = productPayloads;
    const int n = std::min<int>(productInfo.size() / 5, productPayloads.size());
    productTable_->blockSignals(true);
    productTable_->setRowCount(n);
    int reselect = -1;
    for (int r = 0; r < n; ++r) {
        const int* p = productInfo.constData() + r * 5;
        const double ratio = printableRatio(productPayloads[r]);
        const QString kind = ratio > 0.6 ? QStringLiteral("text (%1% printable)").arg(ratio * 100.0, 0, 'f', 0) : QStringLiteral("binary");
        const QStringList cells = {QString::number(p[0]), QString::number(p[1]), QString::number(p[2]), QString::number(p[3]), QString::number(p[4]), kind};
        for (int c = 0; c < 6; ++c) {
            if (!productTable_->item(r, c)) productTable_->setItem(r, c, new QTableWidgetItem);
            productTable_->item(r, c)->setText(cells[c]);
        }
        if (p[0] == keyBlock && p[1] == keyProduct) reselect = r;
    }
    if (reselect >= 0) productTable_->selectRow(reselect);
    productTable_->blockSignals(false);
    if (reselect >= 0) refreshProductDump();

    // --- raw dump (only worth building while it is on screen)
    if (rawDump_->isVisible()) {
        QString text = QStringLiteral("Latest block 0, %1 bytes (the first 278 are the documentation fields SPCID … PXOR1, the rest is fill).\n\n").arg(block0.size());
        text += hexDump(block0.left(kDocBytes), kDocBytes);
        if (haveCons) {
            text += QStringLiteral("\nConsensus (each field's most voted value; fields that change every scan keep the latest block's value):\n\n");
            text += hexDump(consensus.left(kDocBytes), kDocBytes);
        }
        text += QStringLiteral("\nRemaining bytes:\n\n") + hexDump(block0.mid(kDocBytes), 1024);
        const int scroll = rawDump_->verticalScrollBar()->value();
        rawDump_->setPlainText(text);
        rawDump_->verticalScrollBar()->setValue(scroll);
    }
}
