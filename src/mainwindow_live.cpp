// The Live page: receiver controls on the left, the spectrum in the middle, the constellation,
// receive quality and the image preview on the right.
#include "mainwindow.h"
#include "uikit.h"
#include "snrtrend.h"
#include "snrmeter.h"
#include <QDoubleValidator>
#include <QFormLayout>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QClipboard>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMenu>
#include <QStatusBar>
#include <QTextStream>
#include <QLocale>
#include <algorithm>
#include <QFrame>
#include <QHBoxLayout>
#include <QMetaObject>
#include <QScrollArea>
#include <QSettings>
#include <QThread>
#include <QVBoxLayout>
#include <cmath>
#include <limits>
namespace {
// Keeps one child square and centred in whatever space the layout gives it.
class SquareHost : public QWidget {
public:
    QWidget* child = nullptr;
    SquareHost() {
        setMinimumSize(80, 80);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    }
protected:
    void resizeEvent(QResizeEvent*) override {
        if (!child) return;
        const int side = std::min(width(), height());
        child->setGeometry((width() - side) / 2, 0, side, side);
    }
};
}  // namespace


QWidget* MainWindow::metricTile(const QString& caption, QLabel*& value, const QString& tip) {
    using namespace nadirstation::ui;
    auto* tile = new QFrame;
    tile->setObjectName(QStringLiteral("MetricTile"));
    auto* l = new QVBoxLayout(tile);
    l->setContentsMargins(10, 7, 10, 8);
    l->setSpacing(2);
    l->addWidget(label(caption, "MetricCaption"));
    value = label(QStringLiteral("—"), "MetricValue");
    l->addWidget(value);
    if (!tip.isEmpty()) tile->setToolTip(tip);
    return tile;
}

QWidget* MainWindow::makeControlPanel() {
    using namespace nadirstation::ui;
    auto* panel = new QFrame;
    panel->setObjectName(QStringLiteral("Card"));
    panel->setFixedWidth(320);
    auto* outer = new QVBoxLayout(panel);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(0);

    // Everything that is configured scrolls; the buttons that run the station stay put below it.
    auto* content = new QWidget;
    auto* lay = new QVBoxLayout(content);
    lay->setContentsMargins(14, 6, 12, 10);
    lay->setSpacing(4);

    // ---- Signal source ----
    lay->addWidget(label(QStringLiteral("Signal source"), "SectionTitle"));
    auto* sourceBox = new QWidget;
    Form sourceForm(sourceBox);
    inputSource_ = new QComboBox;
    inputSource_->addItems({"Pluto IP", "RTL-SDR", "File input (WAV / cs16 / cs8)", "HackRF One"});
    sourceForm.row(QStringLiteral("Input"), inputSource_);

    rtlOptions_ = new QWidget;
    {
        Form rtlForm(rtlOptions_);
        rtlDeviceIndex_ = new QSpinBox;
        rtlDeviceIndex_->setRange(0, 31);
        rtlDeviceIndex_->setValue(2);
        rtlDeviceIndex_->setToolTip(QStringLiteral("RTL-SDR index in the driver's device list, counting from 0; index 2 is the working device detected on this workstation."));
        rtlForm.row(QStringLiteral("Device index"), rtlDeviceIndex_);
        rtlAutoBiasTee_ = new QCheckBox(QStringLiteral("Bias-T power"));
        rtlAutoBiasTee_->setChecked(true);
        rtlForm.row(QString(), rtlAutoBiasTee_);
    }
    sourceForm.row(QString(), rtlOptions_);
    rtlOptions_->hide();

    hackrfOptions_ = new QWidget;
    {
        Form hackrfForm(hackrfOptions_);
        hackrfDeviceIndex_ = new QSpinBox;
        hackrfDeviceIndex_->setRange(0, 15);
        hackrfDeviceIndex_->setValue(0);
        hackrfDeviceIndex_->setToolTip(QStringLiteral("HackRF index in libhackrf's device list, counting from 0."));
        hackrfForm.row(QStringLiteral("Device index"), hackrfDeviceIndex_);
        hackrfAntennaPower_ = new QCheckBox(QStringLiteral("Antenna port power (3.3 V)"));
        hackrfAntennaPower_->setChecked(false);
        hackrfAntennaPower_->setToolTip(QStringLiteral("Powers an LNA through the antenna port. Leave off unless the LNA needs it."));
        hackrfForm.row(QString(), hackrfAntennaPower_);
    }
    sourceForm.row(QString(), hackrfOptions_);
    hackrfOptions_->hide();

    auto* wavRow = new QWidget;
    auto* wavLay = new QHBoxLayout(wavRow);
    wavLay->setContentsMargins(0, 0, 0, 0);
    wavLay->setSpacing(6);
    wavPath_ = new QLineEdit;
    wavPath_->setPlaceholderText("WAV, or raw I/Q .cs16 / .cs8 / .cu8…");
    wavPath_->setToolTip(QStringLiteral(
        "WAV: 2-channel 16-bit PCM; the sample rate is read from the header.\n"
        "Raw I/Q: .cs16 (int16), .cs8 (int8), .cu8 (uint8, rtl_sdr), I/Q interleaved. The sample rate is taken from, in order:\n"
        "the rate field of a .txt with the same name -> a number with a unit in the file name (e.g. 2500000SPS, 2.5MSPS) -> the Sample rate below."));
    wavBrowse_ = new QPushButton(QStringLiteral("…"));
    wavBrowse_->setToolTip(QStringLiteral("Choose a recording"));
    wavBrowse_->setFixedWidth(34);
    wavLay->addWidget(wavPath_, 1);
    wavLay->addWidget(wavBrowse_);
    sourceForm.row(QStringLiteral("File"), wavRow);
    wavSpeed_ = new QComboBox;
    wavSpeed_->addItems({"Real-time", "4×", "Offline contiguous (Max)"});
    wavSpeed_->setCurrentIndex(2);
    sourceForm.row(QStringLiteral("Playback"), wavSpeed_);
    wavRange_ = new QComboBox;
    wavRange_->addItems({"Full file", "First 50%", "First 25%"});
    wavRange_->setCurrentIndex(1);
    sourceForm.row(QStringLiteral("Range"), wavRange_);
    lay->addWidget(sourceBox);

    // ---- Tuning ----
    lay->addSpacing(6);
    lay->addWidget(rule());
    lay->addWidget(label(QStringLiteral("Tuning"), "SectionTitle"));
    auto* tuneBox = new QWidget;
    Form tuneForm(tuneBox);
    freq_ = new QDoubleSpinBox;
    freq_->setObjectName(QStringLiteral("BigNumber"));
    freq_->setRange(70.0, 6000.0);   // MHz
    freq_->setDecimals(3);
    freq_->setValue(1687.500);
    freq_->setSuffix(" MHz");
    freq_->setSingleStep(0.001);
    tuneForm.row(QStringLiteral("Centre frequency"), freq_);

    // Issue #4: tune the SDR this far from the downlink and move the signal back digitally, so the
    // SDR's DC spike (HackRF, Pluto, RTL-SDR all have one at the tuned frequency) falls outside the
    // signal. Remembered; applies to the live inputs on the next Start.
    freqShift_ = new QDoubleSpinBox;
    freqShift_->setRange(-4000.0, 4000.0);
    freqShift_->setDecimals(0);
    freqShift_->setSingleStep(50.0);
    freqShift_->setSuffix(QStringLiteral(" kHz"));
    freqShift_->setValue(QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation")).value(QStringLiteral("rx/frequencyShiftKHz"), 0.0).toDouble());
    freqShift_->setToolTip(QStringLiteral("Decoder frequency shift: the receiver is tuned this far from the centre frequency and the "
                                          "signal is moved back in software, so the DC spike lands beside the signal instead of on it. "
                                          "Choose more than half the signal's width and less than half the sample rate minus half the "
                                          "signal's width (for example 400 kHz for FY-2 S-VISSR at 2.5 MS/s). Applies on the next Start; "
                                          "not used for file input."));
    connect(freqShift_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [](double v) {
        QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation")).setValue(QStringLiteral("rx/frequencyShiftKHz"), v);
    });
    tuneForm.row(QStringLiteral("Frequency shift"), freqShift_);

    gain_ = new QDoubleSpinBox;
    gain_->setRange(-30, 73);
    gain_->setValue(35);
    gain_->setSuffix(" dB");
    gain_->setToolTip(QStringLiteral("Takes effect at once while receiving; remembered per satellite and input source"));
    tuneForm.row(QStringLiteral("Gain"), gain_);
    // Applied at once while the Pluto runs, and remembered per satellite and input source, so the
    // value tuned for one downlink comes back with it.
    connect(gain_, QOverload<double>::of(&QDoubleSpinBox::valueChanged), this, [this](double v) {
        if (receiver_ && receiver_->isRunning()) receiver_->setGainLive(v);
        const int sat = satelliteSelector_ ? satelliteSelector_->currentData().toInt() : 0;
        if (sat > 0) {
            QSettings s(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
            s.setValue(QStringLiteral("rx/gainDb/%1/%2").arg(inputSource_ ? inputSource_->currentIndex() : 0).arg(sat), v);
        }
    });

    sampleRate_ = new QComboBox;
    sampleRate_->addItems({"6.000 MS/s", "5.000 MS/s", "4.000 MS/s", "3.000 MS/s", "2.500 MS/s", "2.400 MS/s", "2.000 MS/s", "8.000 MS/s", "10.000 MS/s", "3.200 MS/s"});
    // 2.500 MS/s, the rate the bank holds a lock at (index four of the list above).
    sampleRate_->setCurrentIndex(4);
    tuneForm.row(QStringLiteral("Sample rate"), sampleRate_);

    bandwidth_ = new QComboBox;
    bandwidth_->addItem(QStringLiteral("2.000 MHz"), 2000000);
    bandwidth_->addItem(QStringLiteral("1.800 MHz"), 1800000);
    bandwidth_->addItem(QStringLiteral("1.000 MHz"), 1000000);
    bandwidth_->addItem(QStringLiteral("5.200 MHz"), 5200000);
    bandwidth_->addItem(QStringLiteral("Automatic (RTL-SDR / HackRF)"), 0);
    // S-VISSR's transmitted channel bandwidth is 2 MHz; narrowing this to 1 MHz rejects valid sidebands.
    bandwidth_->setCurrentIndex(0);
    tuneForm.row(QStringLiteral("RF bandwidth"), bandwidth_);
    lay->addWidget(tuneBox);

    const auto applyKey = [this](const QString& path) {
        if (path.isEmpty() || !decoder_) return;
        auto* target = decoder_.get();
        if (decoderThread_ && decoderThread_->isRunning())
            QMetaObject::invokeMethod(target, [target, path] { target->setGk2aKeyPath(path); }, Qt::QueuedConnection);
        else target->setGk2aKeyPath(path);
    };
    applyKey(QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation")).value(QStringLiteral("gk2a/keyPath")).toString());

    // ---- Demodulator ----
    lay->addSpacing(6);
    lay->addWidget(rule());
    lay->addWidget(label(QStringLiteral("Demodulator"), "SectionTitle"));
    auto* demodBox = new QWidget;
    Form demodForm(demodBox);
    // Both accept a typed value as well as the presets; a satellite change restores its defaults
    // (applySatelliteSelection). The ranges are the ones the decoder clamps to.
    const auto editableNumber = [](QComboBox* box, const QStringList& presets, double lo, double hi, int decimals) {
        box->addItems(presets);
        box->setEditable(true);
        box->setInsertPolicy(QComboBox::NoInsert);
        auto* v = new QDoubleValidator(lo, hi, decimals, box);
        v->setNotation(QDoubleValidator::StandardNotation);
        v->setLocale(QLocale::c());
        box->setValidator(v);
        // An empty or out-of-range entry falls back to the nearest limit when the box loses focus.
        QObject::connect(box->lineEdit(), &QLineEdit::editingFinished, box, [box, lo, hi, decimals] {
            bool ok = false;
            const double x = QLocale::c().toDouble(box->currentText().trimmed(), &ok);
            if (!ok || x < lo || x > hi)
                box->setCurrentText(QString::number(ok ? std::clamp(x, lo, hi) : lo, 'f', decimals));
        });
    };
    rrcAlpha_ = new QComboBox;
    editableNumber(rrcAlpha_, {"0.25", "0.35", "0.50", "0.65", "0.85"}, 0.05, 1.0, 2);
    rrcAlpha_->setCurrentText("0.50");
    rrcAlpha_->setToolTip(QStringLiteral("Root-raised-cosine roll-off of the matched filter (0.05–1.00). Type a value or pick one; "
                                         "used from the next Start Decode. Changing the satellite restores 0.50."));
    demodForm.row(QStringLiteral("RRC roll-off"), rrcAlpha_);
    pllBandwidth_ = new QComboBox;
    editableNumber(pllBandwidth_, {"0.002", "0.005", "0.010", "0.020", "0.040"}, 0.0005, 0.25, 4);
    pllBandwidth_->setCurrentText("0.020");
    demodForm.row(QStringLiteral("PLL bandwidth"), pllBandwidth_);
    lay->addWidget(demodBox);

    // ---- Recording ----
    lay->addSpacing(6);
    lay->addWidget(rule());
    lay->addWidget(label(QStringLiteral("IQ recording"), "SectionTitle"));
    {
        QSettings rs(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        auto* row = new QWidget;
        Form recForm(row);
        recordFormat_ = new QComboBox;
        recordFormat_->addItem(QStringLiteral("Auto (cs16 Pluto / cu8 RTL-SDR)"), QStringLiteral("auto"));
        recordFormat_->addItem(QStringLiteral("cs16 - 16-bit I/Q"), QStringLiteral("cs16"));
        recordFormat_->addItem(QStringLiteral("cs8 - 8-bit signed I/Q"), QStringLiteral("cs8"));
        recordFormat_->addItem(QStringLiteral("cu8 - 8-bit unsigned I/Q"), QStringLiteral("cu8"));
        recordFormat_->addItem(QStringLiteral("WAV - 16-bit stereo (I/Q)"), QStringLiteral("wav"));
        recordFormat_->setToolTip(QStringLiteral(
            "cs16 keeps every Pluto sample; cu8 keeps every RTL-SDR sample (the formats the devices deliver).\n"
            "cs8 is half the size of cs16 (a Pluto recording keeps its top 8 bits).\n"
            "WAV is 16-bit stereo with the sample rate in its header, for other tools."));
        const int fi = recordFormat_->findData(rs.value(QStringLiteral("record/format"), QStringLiteral("auto")).toString());
        recordFormat_->setCurrentIndex(fi >= 0 ? fi : 0);
        recForm.row(QStringLiteral("Format"), recordFormat_);
        recordLimit_ = new QComboBox;
        recordLimit_->addItem(QStringLiteral("No limit"), 0);
        for (int m : {1, 5, 10, 30, 60}) recordLimit_->addItem(QStringLiteral("%1 min").arg(m), m);
        recordLimit_->setToolTip(QStringLiteral("Stop the recording automatically after this long."));
        const int li = recordLimit_->findData(rs.value(QStringLiteral("record/limitMin"), 0).toInt());
        recordLimit_->setCurrentIndex(li >= 0 ? li : 0);
        recForm.row(QStringLiteral("Stop after"), recordLimit_);
        connect(recordFormat_, &QComboBox::currentIndexChanged, this, [this] {
            QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation")).setValue(QStringLiteral("record/format"), recordFormat_->currentData().toString());
        });
        connect(recordLimit_, &QComboBox::currentIndexChanged, this, [this] {
            QSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation")).setValue(QStringLiteral("record/limitMin"), recordLimit_->currentData().toInt());
        });
        lay->addWidget(row);
    }
    // The receiver and service the panel is set up for; the link strip shows the same, so this stays hidden.
    modeNote_ = label(QString(), "Note");
    modeNote_->setVisible(false);
    lay->addWidget(modeNote_);
    lay->addStretch(1);
    // Long item texts must not widen the panel: combo boxes size to a short minimum and elide.
    for (QComboBox* box : content->findChildren<QComboBox*>()) {
        box->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        box->setMinimumContentsLength(6);
    }
    outer->addWidget(panelScroll(content), 1);

    // ---- Actions (fixed) ----
    auto* actions = new QWidget;
    auto* al = new QVBoxLayout(actions);
    al->setContentsMargins(14, 10, 14, 14);
    al->setSpacing(8);
    al->addWidget(rule());
    connLabel_ = label(QStringLiteral("Pluto: offline (simulation)"), "ConnState");
    connLabel_->setProperty("tone", QStringLiteral("warn"));
    al->addWidget(connLabel_);
    rxBtn_ = new QPushButton("Start Receiving");
    rxBtn_->setObjectName(QStringLiteral("Primary"));
    rxBtn_->setMinimumHeight(40);
    rxBtn_->setCursor(Qt::PointingHandCursor);
    al->addWidget(rxBtn_);
    decodeBtn_ = new QPushButton("Start Decode");
    decodeBtn_->setObjectName(QStringLiteral("Secondary"));
    decodeBtn_->setMinimumHeight(36);
    decodeBtn_->setCursor(Qt::PointingHandCursor);
    al->addWidget(decodeBtn_);
    recordBtn_ = new QPushButton;
    recordBtn_->setObjectName(QStringLiteral("RecordButton"));
    recordBtn_->setCheckable(true);
    recordBtn_->setMinimumHeight(32);
    recordBtn_->setToolTip(QStringLiteral(
        "Record the live IQ stream to a file under <output folder>/IQ_Recordings.\n"
        "The file name carries the sample rate and centre frequency, so the File input can play it back\n"
        "and decode it again.\n"
        "Pressing the button arms it: recording starts at once if a receiver is running, otherwise at the\n"
        "next Start Receiving. It stops when you press it again, when receiving stops, when the chosen\n"
        "time limit is reached, or when the disk has less than 1 GB free."));
    al->addWidget(recordBtn_);
    outer->addWidget(actions);

    // The two run buttons say whether they are running by their text; the stylesheet follows it.
    for (QPushButton* b : {rxBtn_, decodeBtn_}) {
        connect(b, &QAbstractButton::clicked, this, [this] { QTimer::singleShot(0, this, [this] { refreshRunButtons(); }); });
    }
    return panel;
}

void MainWindow::resetSnrSmoothing() {
    snrEma_ = std::numeric_limits<double>::quiet_NaN();
    snrEmaMs_ = 0;
    snrFirstMs_ = 0;
    snrShownPeak_ = std::numeric_limits<float>::quiet_NaN();
    snrReports_.clear();
    lastSnrDb_ = std::numeric_limits<float>::quiet_NaN();
}

// Mirrors the run buttons' text ("Stop ...") into the "running" property the stylesheet reads.
void MainWindow::refreshRunButtons() {
    using nadirstation::ui::setTone;
    if (rxBtn_) setTone(rxBtn_, "running", rxBtn_->text().startsWith(QLatin1String("Stop")) ? QStringLiteral("true") : QStringLiteral("false"));
    if (decodeBtn_) setTone(decodeBtn_, "running", decodeBtn_->text().startsWith(QLatin1String("Stop")) ? QStringLiteral("true") : QStringLiteral("false"));
}

void MainWindow::setConnState(const QString& text, const QString& tone) {
    if (!connLabel_) return;
    connLabel_->setText(text);
    nadirstation::ui::setTone(connLabel_, "tone", tone);
}

QWidget* MainWindow::makeProcessingPage() {
    using namespace nadirstation::ui;
    auto* page = new QWidget;
    auto* root = new QHBoxLayout(page);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(8);

    root->addWidget(makeControlPanel());

    // ---- Centre: spectrum ----
    auto* fftControls = new QWidget;
    auto* fftRow = new QHBoxLayout(fftControls);
    fftRow->setContentsMargins(0, 0, 0, 0);
    fftRow->setSpacing(6);
    fftSize_ = new QComboBox;
    fftSize_->addItems({"512", "1024", "2048", "4096", "8192", "16384"});
    fftSize_->setCurrentText("2048");
    fftSize_->setToolTip(QStringLiteral("FFT size: larger gives finer frequency resolution but costs more CPU"));
    fftWindow_ = new QComboBox;
    fftWindow_->addItems({"Hann", "Hamming", "Blackman", "Nuttall", "Rectangular"});
    fftWindow_->setToolTip(QStringLiteral("Spectrum window: trades spectral leakage against main-lobe width"));
    fftAvgNum_ = new QComboBox;
    fftAvgNum_->addItems({"1", "2", "4", "8", "16", "32", "64"});
    fftAvgNum_->setCurrentText("8");
    fftAvgNum_->setToolTip(QStringLiteral("Number of spectra averaged; only smooths the display, the demodulator is not affected"));
    fftRow->addWidget(fftSize_);
    fftRow->addWidget(fftWindow_);
    fftRow->addSpacing(6);
    fftRow->addWidget(label(QStringLiteral("Avg"), "FieldLabel"));
    fftRow->addWidget(fftAvgNum_);

    for (QComboBox* box : {fftSize_, fftWindow_, fftAvgNum_}) {
        box->setSizeAdjustPolicy(QComboBox::AdjustToContents);
        box->setMinimumWidth(box == fftWindow_ ? 96 : 86);
    }
    Card centre = card(QStringLiteral("Spectrum"), fftControls);
    centre.frame->setObjectName(QStringLiteral("CenterStage"));
    spectrum_ = new SpectrumWidget;
    centre.body->addWidget(spectrum_, 1);
    connect(fftSize_, &QComboBox::currentTextChanged, this, [this] { applySpectrumSettings(); });
    connect(fftWindow_, &QComboBox::currentIndexChanged, this, [this] { applySpectrumSettings(); });
    connect(fftAvgNum_, &QComboBox::currentTextChanged, this, [this] { applySpectrumSettings(); });
    applySpectrumSettings();

    auto* statusRow = new QHBoxLayout;
    statusRow->setSpacing(10);
    decodeState_ = new QLabel;
    decodeState_->setObjectName("DecodeBadge");
    decodeState_->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    decodeState_->setTextFormat(Qt::RichText);
    decodeState_->setText(QStringLiteral("<span style=\"color:#6e737a; font-weight:600;\">&#9679; IDLE</span><br>0% locked · 0 frames · 0 parity faults<br>quality 0%"));
    decodeState_->setProperty("sync", QStringLiteral("idle"));
    decodeState_->setMinimumHeight(64);
    decodeState_->setWordWrap(false);
    statusRow->addWidget(decodeState_, 1);
    QWidget* carrierTile = metricTile(QStringLiteral("Carrier offset"), carrier_,
        QStringLiteral("Frequency offset of the carrier the demodulator has locked to"));
    carrierTile->setMinimumWidth(150);
    statusRow->addWidget(carrierTile);
    centre.body->addLayout(statusRow);

    // Events: status messages and link-state changes, newest at the bottom.
    centre.body->addWidget(label(QStringLiteral("Events"), "SectionTitle"));
    eventLog_ = new QPlainTextEdit;
    eventLog_->setObjectName(QStringLiteral("LogView"));
    eventLog_->setReadOnly(true);
    eventLog_->setMaximumBlockCount(300);
    eventLog_->setLineWrapMode(QPlainTextEdit::NoWrap);
    eventLog_->setFixedHeight(86);
    eventLog_->setPlaceholderText(QStringLiteral("No events yet"));
    eventLog_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(eventLog_, &QWidget::customContextMenuRequested, this, [this](const QPoint& at) {
        QMenu menu(eventLog_);
        menu.addAction(QStringLiteral("Copy all"), this, [this] {
            QGuiApplication::clipboard()->setText(eventLog_->toPlainText());
        });
        menu.addAction(QStringLiteral("Save to output folder"), this, [this] { saveEventLog(); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("Clear"), eventLog_, &QPlainTextEdit::clear);
        menu.exec(eventLog_->mapToGlobal(at));
    });
    centre.body->addWidget(eventLog_);
    root->addWidget(centre.frame, 1);

    // ---- Right: constellation, signal strength, decoder, preview ----
    auto* right = new QWidget;
    right->setFixedWidth(318);
    auto* rl = new QVBoxLayout(right);
    rl->setContentsMargins(0, 0, 0, 0);
    rl->setSpacing(8);

    // The constellation as large as the column allows (square), the points in the colour of the
    // SNR zone; under it the SNR in large type with its zone bar, and its last minute.
    // Three cards: the constellation, the SNR read-out, and (further down) the image preview. The
    // constellation and the preview share the height equally (stretch 1 : 1); the SNR card is fixed.
    Card constel = card();   // the widget draws its own title, which names the modulation
    constellation_ = new ConstellationWidget;
    constellation_->setMinimumHeight(150);
    constellation_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    constel.body->addWidget(constellation_, 1);
    rl->addWidget(constel.frame, 1);
    // These come from the symbol constellation, not from the spectrum. Measured on FY-2 recordings
    // with known added noise they read 1 to 2.5 dB below the input Es/N0, so say so where they are.
    const QString tip = QStringLiteral(
        "Symbol SNR, estimated from the demodulated constellation.\n"
        "Colour: red = below what this link needs, amber = decoding with little margin, green = good, blue = strong.\n"
        "The zone edges are set for each satellite; the white tick on the bar is the peak.\n"
        "FY-2 test (2026-10-20): reads about 1-2.5 dB below the input Es/N0 measured from the spectrum.");
    Card snrCard = card();
    snrMeter_ = new SnrMeter;
    snrMeter_->setToolTip(tip);
    snrCard.body->addWidget(snrMeter_);
    // The numeric labels the rest of the window updates; the meter draws the values.
    snr_ = new QLabel(right); snr_->hide();
    peakSnr_ = new QLabel(right); peakSnr_->hide();
    avgSnr_ = new QLabel(right); avgSnr_->hide();
    snrTrend_ = new SnrTrend;
    snrTrend_->setToolTip(tip);
    snrTrend_->setFixedHeight(54);
    snrCard.body->addWidget(snrTrend_);
    rl->addWidget(snrCard.frame, 0);

    // What the decoder is set up for and what it reports, as a table.
    Card dec = card(QStringLiteral("Decoder"));
    auto* decBox = new QWidget;
    Form decForm(decBox);
    decForm.layout()->setVerticalSpacing(1);
    const auto value = [](QLabel*& l) {
        l = label(QStringLiteral("—"), "DataValue");
        l->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
        return l;
    };
    decForm.row(QStringLiteral("Modulation"), value(decModulation_));
    decForm.row(QStringLiteral("Symbol rate"), value(decSymbolRate_));
    decForm.row(QStringLiteral("Samples / symbol"), value(decSps_));
    decForm.row(QStringLiteral("Lock"), value(decLock_));
    decForm.row(QStringLiteral("Frames ok / rejected"), value(decFrames_));
    decForm.row(QStringLiteral("Soft quality"), value(decQuality_));
    decForm.row(QStringLiteral("Frames / s"), value(decRate_));
    decForm.row(QStringLiteral("Session"), value(decSession_));
    decRate_->setToolTip(QStringLiteral("Accepted frames per second over the last few seconds"));
    decSession_->setToolTip(QStringLiteral("Time since the input started"));
    dec.body->addWidget(decBox);
    rl->addWidget(dec.frame);
    // Hidden at the operator's request (2026-11-20): the constellation and SNR say what matters, and the
    // space goes to the constellation. The labels are still kept up to date for the code that reads them.
    dec.frame->hide();

    Card prev = card(QStringLiteral("Image preview"));
    // Square, like the full-disk images it shows, and as large as the column leaves room for: the
    // side is the smaller of the space's width and height, so a short window shrinks it instead of
    // squeezing the constellation.
    auto* square = new SquareHost;
    preview_ = new QLabel(QStringLiteral("Waiting for images…"), square);
    preview_->setObjectName(QStringLiteral("Preview"));
    preview_->setAlignment(Qt::AlignCenter);
    preview_->setScaledContents(false);
    square->child = preview_;
    prev.body->addWidget(square, 1);
    rl->addWidget(prev.frame, 1);
    prev.frame->setMinimumHeight(150);
    root->addWidget(right);
    return page;
}

namespace {
QString modulationOf(int sat) {
    switch (sat) {
    case 8: case 9: return QStringLiteral("BPSK · NRZ-M");
    case 10: return QStringLiteral("BPSK · NRZ-L");
    case 11: return QStringLiteral("PM / BPSK 5 kHz SC");
    case 12: return QStringLiteral("QPSK");
    case 13: return QStringLiteral("BPSK · NRZ-S");
    case 14: return QStringLiteral("BPSK · NRZ-L");
    case 15: return QStringLiteral("OQPSK");
    case 16: return QStringLiteral("QPSK");
    case 17: return QStringLiteral("PM · Manchester");
    case 18: return QStringLiteral("BPSK · NRZ-M (I imager / Q sounder)");
    default: return QStringLiteral("—");
    }
}
double symbolRateOf(int sat) {
    return (sat == 8 || sat == 9) ? 660000.0 : sat == 10 ? 128000.0 : sat == 11 ? 5000.0
         : sat == 12 ? 3000000.0 : sat == 13 ? 2111360.0
         : sat == 14 ? 294000.0 : sat == 15 ? 1150000.0
         : sat == 16 ? 2333333.0 : sat == 17 ? 665400.0 : sat == 18 ? 2620800.0 : 0.0;
}
}

// The Decoder table and the SNR strip chart, refreshed with the link strip (twice a second).
void MainWindow::refreshDecoderPanel(bool receiving) {
    const int sat = satelliteSelector_ ? satelliteSelector_->currentData().toInt() : 0;
    const double sym = symbolRateOf(sat);
    const double fs = sampleRate_ ? sampleRate_->currentText().section(QLatin1Char(' '), 0, 0).toDouble() * 1e6 : 0.0;
    if (decModulation_) decModulation_->setText(modulationOf(sat));
    if (decSymbolRate_) decSymbolRate_->setText(sym <= 0.0 ? QStringLiteral("—")
        : sym >= 1e6 ? QStringLiteral("%1 Msym/s").arg(sym / 1e6, 0, 'f', 3) : QStringLiteral("%1 ksym/s").arg(sym / 1e3, 0, 'f', 0));
    if (decSps_) decSps_->setText(sym > 0.0 && fs > 0.0 ? QString::number(fs / sym, 'f', 2) : QStringLiteral("—"));
    const bool live = receiving && decoder_ && decoder_->enabled();
    if (decLock_) decLock_->setText(live ? QStringLiteral("%1 %").arg(lastLock_) : QStringLiteral("—"));
    if (decFrames_) decFrames_->setText(live ? QStringLiteral("%1 / %2").arg(lastFrames_).arg(lastBad_) : QStringLiteral("—"));
    if (decQuality_) decQuality_->setText(live ? QStringLiteral("%1 %").arg(lastQuality_ * 100.0f, 0, 'f', 1) : QStringLiteral("—"));
    if (spectrum_) {
        const float alpha = rrcAlpha_ ? rrcAlpha_->currentText().toFloat() : 0.5f;
        // METEOR HRPT is Manchester: its main lobes sit at +-1 and +-2 times the bit rate.
        spectrum_->setSignalBandwidth(sat == 17 ? 4.0 * sym : sym > 0.0 ? sym * (1.0 + alpha) : 0.0);
    }
    // Session clock and frame rate.
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (receiving && !sessionRunning_) {
        sessionRunning_ = true;
        sessionClock_.start();
        rateLastFrames_ = 0;
        rateLastMs_ = nowMs;
        frameRate_ = 0.0;
        sessionPeakSnr_ = std::numeric_limits<float>::quiet_NaN();
    } else if (!receiving && sessionRunning_) {
        sessionRunning_ = false;
        const qint64 s = sessionClock_.elapsed() / 1000;
        logEvent(QStringLiteral("Session ended after %1:%2:%3 · %4 frames ok, %5 rejected · peak SNR %6")
            .arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
            .arg(lastFrames_).arg(lastBad_)
            .arg(std::isfinite(sessionPeakSnr_) ? QStringLiteral("%1 dB").arg(sessionPeakSnr_, 0, 'f', 1) : QStringLiteral("—")));
    }
    if (live && nowMs - rateLastMs_ >= 1000) {
        const int delta = lastFrames_ - rateLastFrames_;
        const double inst = delta >= 0 ? delta * 1000.0 / static_cast<double>(nowMs - rateLastMs_) : 0.0;
        frameRate_ = frameRate_ <= 0.0 ? inst : 0.7 * frameRate_ + 0.3 * inst;
        rateLastFrames_ = lastFrames_;
        rateLastMs_ = nowMs;
    }
    if (decRate_) decRate_->setText(live ? QString::number(frameRate_, 'f', frameRate_ < 10 ? 1 : 0) : QStringLiteral("—"));
    if (decSession_) {
        if (sessionRunning_) {
            const qint64 s = sessionClock_.elapsed() / 1000;
            decSession_->setText(QStringLiteral("%1:%2:%3").arg(s / 3600)
                .arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0')));
        } else {
            decSession_->setText(QStringLiteral("—"));
        }
    }
    if (snrTrend_) snrTrend_->push(receiving && std::isfinite(lastSnrDb_) ? lastSnrDb_ : std::numeric_limits<float>::quiet_NaN());
    if (!receiving && snrMeter_) snrMeter_->clear();
    if (!receiving) resetSnrSmoothing();
}

// Event list to <output root>/Logs/events_<UTC>.txt.
void MainWindow::saveEventLog() {
    if (!eventLog_) return;
    const QString dir = QDir(outputRootPath_).filePath(QStringLiteral("Logs"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("events_%1.txt")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(path)), 6000);
        return;
    }
    QTextStream(&f) << eventLog_->toPlainText() << '\n';
    statusBar()->showMessage(QStringLiteral("Events saved to %1").arg(QDir::toNativeSeparators(path)), 6000);
}

// The whole window to <output root>/Screenshots/NADIR_<UTC>.png.
void MainWindow::saveWindowScreenshot() {
    const QString dir = QDir(outputRootPath_).filePath(QStringLiteral("Screenshots"));
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(QStringLiteral("NADIR_%1.png")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyyMMdd_HHmmss"))));
    if (grab().save(path)) {
        statusBar()->showMessage(QStringLiteral("Screenshot saved to %1").arg(QDir::toNativeSeparators(path)), 6000);
        logEvent(QStringLiteral("Screenshot %1").arg(QFileInfo(path).fileName()));
    } else {
        statusBar()->showMessage(QStringLiteral("Could not write %1").arg(QDir::toNativeSeparators(path)), 6000);
    }
}
