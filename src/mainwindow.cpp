// Main window: construction, satellite selection, receiving and decoding control.
#include "mainwindow.h"
#include "snrtrend.h"
#include "mainwindow_common.h"
#include "decoderpage.h"
#include <QApplication>
#include <QStandardItemModel>
#include <utility>

std::function<void(const QString&, double)> MainWindow::startupProgress;

MainWindow::MainWindow(QWidget* parent) : QMainWindow(parent) {
    setWindowTitle(QStringLiteral("NADIR STATION"));
    resize(1440, 900);
    setMinimumSize(1280, 760);

    qRegisterMetaType<std::shared_ptr<std::vector<std::complex<float>>>>("FY2IQPtr");
    qRegisterMetaType<std::vector<float>>("FY2FloatVector");
    receiver_ = std::make_unique<PlutoReceiver>();
    wavReceiver_ = std::make_unique<WavReceiver>();
    rtlReceiver_ = std::make_unique<RtlSdrReceiver>();
    hackrfReceiver_ = std::make_unique<HackRfReceiver>();
    decoder_ = std::make_unique<NadirStationDecoder>();
    QSettings appSettings(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
    const QString defaultOutput = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("FY2_DATA"));
    outputRootPath_ = QDir::cleanPath(appSettings.value(QStringLiteral("paths/outputRoot"), defaultOutput).toString());
    QDir().mkpath(outputRootPath_);
    decoder_->setOutputRoot(outputRootPath_);
    decoderThread_ = new QThread(this);
    decoder_->moveToThread(decoderThread_);
    decoderThread_->start();

    // Shell: navigation rail | (link strip / page stack). The pages are built first because the
    // link strip's satellite selector presets their controls as soon as it exists.
    stack_ = new QStackedWidget;
    if (startupProgress) startupProgress(QStringLiteral("Building the receiver…"), 0.10);
    stack_->addWidget(makeProcessingPage());
    if (startupProgress) startupProgress(QStringLiteral("Loading the image archive…"), 0.22);
    stack_->addWidget(makeImagePage());
    if (startupProgress) startupProgress(QStringLiteral("Additional data…"), 0.32);
    stack_->addWidget(makeAddPage());
    if (startupProgress) startupProgress(QStringLiteral("System monitor…"), 0.38);
    systemPage_ = new SystemMonitorPage;
    stack_->addWidget(systemPage_);
    if (startupProgress) startupProgress(QStringLiteral("Spacecraft telemetry…"), 0.44);
    telemetryPage_ = new TelemetryPage;
    stack_->addWidget(telemetryPage_);
    if (startupProgress) startupProgress(QStringLiteral("Settings…"), 0.52);
    stack_->addWidget(makeSettingsPage());
    if (startupProgress) startupProgress(QStringLiteral("GGAK and space weather…"), 0.60);
    ggakPage_ = new GgakPage;
    stack_->addWidget(ggakPage_);   // 6
    if (startupProgress) startupProgress(QStringLiteral("Globe…"), 0.68);
    globePage_ = new GlobePage;
    globePage_->setDataRoot(outputRootPath_);
    stack_->addWidget(globePage_);  // 7
    if (startupProgress) startupProgress(QStringLiteral("Pass prediction…"), 0.76);
    passPage_ = new PassPage;
    stack_->addWidget(passPage_);   // 8
    connect(passPage_, &PassPage::settingsRequested, this, [this] { switchPage(5); });
    if (startupProgress) startupProgress(QStringLiteral("Decoder pipeline…"), 0.80);
    decoderPage_ = new DecoderPage;
    stack_->addWidget(decoderPage_);   // 9
    connect(decoder_.get(), &NadirStationDecoder::pipelineSnapshot, decoderPage_, &DecoderPage::setSnapshot, Qt::QueuedConnection);
    // Space weather is a view of the GGAK data, so it is the first tab of the GGAK page.
    spaceWeatherPage_ = new SpaceWeatherPage;
    spaceWeatherPage_->setDataRoot(outputRootPath_);
    ggakPage_->addFrontTab(spaceWeatherPage_, QStringLiteral("Space Weather"));
    connect(ggakPage_, &GgakPage::trendWindowChanged, spaceWeatherPage_, &SpaceWeatherPage::setWindowSeconds);
    connect(spaceWeatherPage_, &SpaceWeatherPage::windowSecondsChanged, ggakPage_, &GgakPage::setTrendWindowSeconds);
    connect(ggakPage_, &GgakPage::framesImported, spaceWeatherPage_, &SpaceWeatherPage::addFrames);
    spaceWeatherPage_->setWindowSeconds(ggakPage_->trendWindowSeconds());

    auto* root = new QWidget;
    auto* rootLayout = new QHBoxLayout(root);
    rootLayout->setContentsMargins(0, 0, 0, 0);
    rootLayout->setSpacing(0);
    rootLayout->addWidget(makeNavRail());
    auto* mainColumn = new QWidget;
    auto* mainLayout = new QVBoxLayout(mainColumn);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(0);
    mainLayout->addWidget(makeLinkStrip());
    auto* pageHost = new QWidget;
    auto* pageHostLayout = new QVBoxLayout(pageHost);
    pageHostLayout->setContentsMargins(8, 8, 8, 8);
    pageHostLayout->addWidget(stack_);
    mainLayout->addWidget(pageHost, 1);
    rootLayout->addWidget(mainColumn, 1);
    setCentralWidget(root);
    connect(satelliteSelector_, QOverload<int>::of(&QComboBox::currentIndexChanged), this,
        [this](int) { applySatelliteSelection(); refreshLinkStrip(); });
    applySatelliteSelection();
    switchPage(0);
    connect(decoder_.get(), &NadirStationDecoder::ggakFrames, ggakPage_, &GgakPage::addFrames, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::ggakFrames, spaceWeatherPage_, &SpaceWeatherPage::addFrames, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::ggakStatus, ggakPage_, &GgakPage::setStatus, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::telemetryUpdated, telemetryPage_, &TelemetryPage::updateTelemetry, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::docPayloadUpdated, telemetryPage_, &TelemetryPage::updateDocPayload, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::orbitAttitudeUpdated, telemetryPage_, &TelemetryPage::updateOrbitAttitude, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::satelliteIdentityUpdated, telemetryPage_, &TelemetryPage::updateSatelliteGeometry, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::calibration2Updated, telemetryPage_, &TelemetryPage::updateCalibration2, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::calibration2Reset, telemetryPage_, &TelemetryPage::resetCalibration2, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::manamProgress, telemetryPage_, &TelemetryPage::updateManamProgress, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::manamUpdated, telemetryPage_, &TelemetryPage::updateManam, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::manamReset, telemetryPage_, &TelemetryPage::resetManam, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::gvarTelemetryUpdated, telemetryPage_, &TelemetryPage::updateGvarTelemetry, Qt::QueuedConnection);
    connect(systemPage_, &SystemMonitorPage::refreshStationRequested, this, &MainWindow::refreshSystemStatus);
    connect(rxBtn_, &QPushButton::clicked, this, [this] {
        activeInput_ != InputSource::None ? stopRx() : startRx();
    });
    connect(decodeBtn_, &QPushButton::clicked, this, [this] {
        // DIAGNOSTIC: which branch, and what the decoder thought its state was.
        const bool alreadyOn = (decoder_ != nullptr && decoder_->enabled());
        nadirstation::log::write(QStringLiteral("GUI decode button clicked; decoder=%1 enabled=%2 -> %3")
            .arg(decoder_ ? QStringLiteral("present") : QStringLiteral("null"))
            .arg(alreadyOn ? QStringLiteral("yes") : QStringLiteral("no"))
            .arg(alreadyOn ? QStringLiteral("stopDecode") : QStringLiteral("startDecode")));
        nadirstation::log::flush();
        alreadyOn ? stopDecode() : startDecode();
    });
    connect(receiver_.get(), &PlutoReceiver::samplesReady, decoder_.get(), &NadirStationDecoder::processSamples, Qt::DirectConnection);
    // IQ recording: the capture thread hands the block to the recorder's own writer thread and
    // returns (a no-op unless a recording is running). Live receivers only -- a file is already a file.
    const auto toRecorder = [rec = iqRecorder_](const std::shared_ptr<std::vector<std::complex<float>>>& s) { rec->push(s); };
    connect(receiver_.get(), &PlutoReceiver::samplesReady, this, toRecorder, Qt::DirectConnection);
    connect(rtlReceiver_.get(), &RtlSdrReceiver::samplesReady, this, toRecorder, Qt::DirectConnection);
    connect(hackrfReceiver_.get(), &HackRfReceiver::samplesReady, this, toRecorder, Qt::DirectConnection);
    connect(recordBtn_, &QPushButton::toggled, this, [this](bool armed) {
        if (!armed) {
            stopIqRecording();
        } else if (rtlReceiver_->isRunning()) {
            startIqRecording(rtlReceiver_->sampleRate(), rtlReceiver_->frequencyHz(), true);
        } else if (hackrfReceiver_->isRunning()) {
            startIqRecording(hackrfReceiver_->sampleRate(), hackrfReceiver_->frequencyHz(), false);
        } else if (receiver_->isRunning()) {
            startIqRecording(receiver_->sampleRate(), receiver_->frequencyHz(), false);
        }
        refreshRecordButton();
    });
    // Lost-sample reports from the receivers go to the log as well: the status bar shows one for a
    // few seconds, and whether samples were lost is the first question when a pass decodes badly.
    const auto logLoss = [this](const QString& text) {
        if (!text.contains(QStringLiteral("samples lost"))) return;
        nadirstation::log::write(QStringLiteral("RX sample loss: %1").arg(text));
        if (decoderPage_) decoderPage_->noteReceiverLoss(text);
    };
    connect(receiver_.get(), &PlutoReceiver::statusMessage, this, logLoss);
    connect(rtlReceiver_.get(), &RtlSdrReceiver::statusMessage, this, logLoss);
    connect(hackrfReceiver_.get(), &HackRfReceiver::statusMessage, this, logLoss);
    recordTimer_ = new QTimer(this);
    recordTimer_->setInterval(1000);
    connect(recordTimer_, &QTimer::timeout, this, [this] {
        const bool timedOut = iqRecorder_->active() && recordLimit_ && recordLimit_->currentData().toInt() > 0 &&
            recordClock_.isValid() && recordClock_.elapsed() >= qint64(recordLimit_->currentData().toInt()) * 60000;
        if (iqRecorder_->failed() || timedOut) {
            const QSignalBlocker block(recordBtn_);
            recordBtn_->setChecked(false);
            stopIqRecording();
        }
        refreshRecordButton();
    });
    recordTimer_->start();
    refreshRecordButton();
    connect(receiver_.get(), &PlutoReceiver::metricsReady, this, &MainWindow::updateMetrics);
    connect(decoder_.get(), &NadirStationDecoder::demodSnrMetricsReady, this,
        [this](float rawSnr, float, float) {
            if (!std::isfinite(rawSnr)) return;
            // Smoothing, in the spirit of SatDump's moment estimator with its long average: a median
            // of the last five reports (drops single jumps), then an exponential average with a 1 s
            // time constant.
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            snrReports_.emplace_back(now, rawSnr);
            while (snrReports_.size() > 1 && now - snrReports_.front().first > 5000) snrReports_.pop_front();
            std::vector<float> last;
            for (auto it = snrReports_.rbegin(); it != snrReports_.rend() && last.size() < 5; ++it) last.push_back(it->second);
            std::nth_element(last.begin(), last.begin() + last.size() / 2, last.end());
            const float median = last[last.size() / 2];
            // While the demodulator acquires (AGC settling, carrier and clock loops pulling in) its first
            // reports can read several dB high. For the first 3 s, and until five reports are in, the
            // read-out follows the median directly (no memory to carry the spike forward) and the peak
            // is not taken.
            if (snrFirstMs_ == 0) snrFirstMs_ = now;
            const bool settled = now - snrFirstMs_ >= 3000 && snrReports_.size() >= 5;
            if (!settled || !std::isfinite(snrEma_) || snrEmaMs_ == 0) snrEma_ = median;
            else snrEma_ += (1.0 - std::exp(-double(now - snrEmaMs_) / 1000.0)) * (median - snrEma_);
            snrEmaMs_ = now;
            const float snr = float(snrEma_);
            double sum = 0.0; int n = 0;
            for (const auto& [t, v] : snrReports_) if (now - t <= 1000) { sum += v; ++n; }
            const float avg = n > 0 ? float(sum / n) : snr;
            if (settled && (!std::isfinite(snrShownPeak_) || snr > snrShownPeak_)) snrShownPeak_ = snr;
            const float peak = snrShownPeak_;
            snr_->setText(QStringLiteral("%1 dB").arg(snr, 0, 'f', 1));
            peakSnr_->setText(std::isfinite(peak) ? QStringLiteral("%1 dB").arg(peak, 0, 'f', 1) : QStringLiteral("—"));
            avgSnr_->setText(QStringLiteral("%1 dB").arg(avg, 0, 'f', 1));
            if (snrMeter_) {
                snrMeter_->setValues(snr, peak, avg);
                if (constellation_) constellation_->setTint(snrMeter_->zones().colour(snr));
            }
            lastSnrDb_ = snr;
            if (settled && std::isfinite(snr) && (!std::isfinite(sessionPeakSnr_) || snr > sessionPeakSnr_)) sessionPeakSnr_ = snr;
        }, Qt::QueuedConnection);
    connect(receiver_.get(), &PlutoReceiver::metricsReady, this, [this](float, float, float, double sr) {
        QMetaObject::invokeMethod(decoder_.get(), [this, sr] { decoder_->setSampleRate(sr); }, Qt::QueuedConnection);
    });
    connect(receiver_.get(), &PlutoReceiver::statusMessage, this, &MainWindow::updateStatus);
    connect(receiver_.get(), &PlutoReceiver::deviceConnected, this, [this](bool yes) {
        if (activeInput_ != InputSource::Pluto) return;
        setConnState(yes ? "Pluto: connected" : "Pluto: offline", yes ? QStringLiteral("ok") : QStringLiteral("warn"));
        if (!yes && !receiver_->isRunning()) finishRx(true);
    });
    connect(wavReceiver_.get(), &WavReceiver::samplesReady, decoder_.get(), &NadirStationDecoder::processSamples, Qt::DirectConnection);
    connect(wavReceiver_.get(), &WavReceiver::metricsReady, this, &MainWindow::updateMetrics);
    connect(wavReceiver_.get(), &WavReceiver::metricsReady, this, [this](float, float, float, double sr) {
        QMetaObject::invokeMethod(decoder_.get(), [this, sr] { decoder_->setSampleRate(sr); }, Qt::QueuedConnection);
    });
    connect(wavReceiver_.get(), &WavReceiver::statusMessage, this, &MainWindow::updateStatus);
    connect(wavReceiver_.get(), &WavReceiver::deviceConnected, this, [this](bool yes) {
        if (activeInput_ == InputSource::File) {
            setConnState(yes ? "WAV: playing" : "WAV: stopped", yes ? QStringLiteral("ok") : QStringLiteral("warn"));
        }
    });
    connect(wavReceiver_.get(), &WavReceiver::finished, this, [this] {
        if (activeInput_ == InputSource::File && !wavReceiver_->isRunning()) finishRx(true);
    }, Qt::QueuedConnection);
    connect(rtlReceiver_.get(), &RtlSdrReceiver::samplesReady, decoder_.get(), &NadirStationDecoder::processSamples, Qt::DirectConnection);
    connect(rtlReceiver_.get(), &RtlSdrReceiver::metricsReady, this, &MainWindow::updateMetrics);
    connect(rtlReceiver_.get(), &RtlSdrReceiver::metricsReady, this, [this](float, float, float, double sr) {
        QMetaObject::invokeMethod(decoder_.get(), [this, sr] { decoder_->setSampleRate(sr); }, Qt::QueuedConnection);
    });
    connect(rtlReceiver_.get(), &RtlSdrReceiver::spectrumReady, this, [mailbox=displayMailbox_](const std::vector<float>& db, double sr) {
        std::lock_guard lock(mailbox->mutex);
        mailbox->spectrum=db;mailbox->sampleRate=sr;
    }, Qt::DirectConnection);
    connect(rtlReceiver_.get(), &RtlSdrReceiver::statusMessage, this, &MainWindow::updateStatus);
    connect(rtlReceiver_.get(), &RtlSdrReceiver::deviceConnected, this, [this](bool yes) {
        if (activeInput_ != InputSource::Rtl) return;
        setConnState(yes ? "RTL-SDR: connected" : "RTL-SDR: offline", yes ? QStringLiteral("ok") : QStringLiteral("warn"));
    });
    connect(rtlReceiver_.get(), &RtlSdrReceiver::finished, this, [this] {
        if (activeInput_ == InputSource::Rtl && !rtlReceiver_->isRunning()) finishRx(true);
    }, Qt::QueuedConnection);
    // HackRF One (issue #2): wired exactly like the RTL-SDR.
    connect(hackrfReceiver_.get(), &HackRfReceiver::samplesReady, decoder_.get(), &NadirStationDecoder::processSamples, Qt::DirectConnection);
    connect(hackrfReceiver_.get(), &HackRfReceiver::metricsReady, this, &MainWindow::updateMetrics);
    connect(hackrfReceiver_.get(), &HackRfReceiver::metricsReady, this, [this](float, float, float, double sr) {
        QMetaObject::invokeMethod(decoder_.get(), [this, sr] { decoder_->setSampleRate(sr); }, Qt::QueuedConnection);
    });
    connect(hackrfReceiver_.get(), &HackRfReceiver::spectrumReady, this, [mailbox=displayMailbox_](const std::vector<float>& db, double sr) {
        std::lock_guard lock(mailbox->mutex);
        mailbox->spectrum=db;mailbox->sampleRate=sr;
    }, Qt::DirectConnection);
    connect(hackrfReceiver_.get(), &HackRfReceiver::statusMessage, this, &MainWindow::updateStatus);
    connect(hackrfReceiver_.get(), &HackRfReceiver::deviceConnected, this, [this](bool yes) {
        if (activeInput_ != InputSource::HackRf) return;
        setConnState(yes ? "HackRF: connected" : "HackRF: offline", yes ? QStringLiteral("ok") : QStringLiteral("warn"));
    });
    connect(hackrfReceiver_.get(), &HackRfReceiver::finished, this, [this] {
        if (activeInput_ == InputSource::HackRf && !hackrfReceiver_->isRunning()) finishRx(true);
    }, Qt::QueuedConnection);
    connect(decoder_.get(), &NadirStationDecoder::statusMessage, this, &MainWindow::updateStatus);
    connect(decoder_.get(), &NadirStationDecoder::symbolConstellation, this, [mailbox=displayMailbox_](const std::shared_ptr<std::vector<std::complex<float>>>& s) {
        std::lock_guard lock(mailbox->mutex);
        mailbox->constellation=s;
    }, Qt::DirectConnection);
    connect(decoder_.get(), &NadirStationDecoder::decodeStats, this, [this](int lock, int frames, int bad, float q, float carrier) {
        // The two services report different things, so the badge says different things.
        //
        // FY-2 S-VISSR recovers scan lines and accepts or rejects each one, so a count of good and
        // bad frames is exactly what the receiver knows. GK-2A LRIT either holds the convolutional
        // code and finds frame markers or it does not, and the state that matters is locked,
        // locking or not -- a frame count beside that reads as noise.
        const bool gk2a = selectedSatelliteName_.startsWith(QStringLiteral("GK-2A"))
                          || selectedSatelliteName_.startsWith(QStringLiteral("ELEKTRO"));   // same badge: sync state, frames, faults
        // Chosen once, then used by the badge and by the system page summary. Declaring it inside
        // each branch left the summary -- further down -- without one.
        const QString state = gk2a
            ? (lock >= 100 ? QStringLiteral("SYNCED")
                           : (lock >= 50 ? QStringLiteral("SYNCING") : QStringLiteral("NOSYNC")))
            : (lock >= 100 ? QStringLiteral("LOCK")
                           : (lock >= 50 ? QStringLiteral("SYNC") : QStringLiteral("SEARCH")));
        // Traffic light: green = lines/frames passing checks, yellow = sync found but data failing,
        // red = searching. The colour is on the badge's left bar and on the state word.
        // After Stop the decoder still drains and finalizes, and its last statistics arrive then;
        // those must not turn the light green again, so a stopped input stays grey.
        const bool inputStopped = rxBtn_ && rxBtn_->text() == QLatin1String("Start Receiving");
        const char* light = inputStopped ? "idle"
                          : (lock >= 100 ? "green" : (lock >= 50 ? "yellow" : "red"));
        const QString lightColour = inputStopped ? QStringLiteral("#6e737a")
                                  : (lock >= 100 ? QStringLiteral("#3fca85")
                                  : (lock >= 50 ? QStringLiteral("#f0b83c") : QStringLiteral("#f0594c")));
        if (decodeState_->property("sync").toString() != QLatin1String(light)) {
            decodeState_->setProperty("sync", QLatin1String(light));
            decodeState_->style()->unpolish(decodeState_);
            decodeState_->style()->polish(decodeState_);
        }
        const QString head = QStringLiteral("<span style=\"color:%1; font-weight:600;\">&#9679; %2</span>")
                                 .arg(lightColour, inputStopped ? QStringLiteral("STOPPED") : state);
        if (gk2a) {
            decodeState_->setText(
                QString("%1<br>%2% locked · RS pass %3 · RS reject %4<br>quality %5%")
                    .arg(head).arg(lock).arg(frames).arg(bad).arg(q * 100.0f, 0, 'f', 1));
        } else {
            decodeState_->setText(QString("%1 %2%<br>GOOD %3 · BAD %4<br>QUALITY %5%")
                .arg(head).arg(lock).arg(frames).arg(bad).arg(q * 100.0f, 0, 'f', 1));
        }
        carrier_->setText(QStringLiteral("%1 Hz").arg(carrier, 0, 'f', 1));
        lastCarrierHz_ = carrier;
        if (state != lastLoggedState_ && !inputStopped) {
            lastLoggedState_ = state;
            logEvent(QStringLiteral("Link %1 (%2% locked)").arg(state).arg(lock));
        }
        if (alertOnLoss_ && !inputStopped && lastLock_ >= 100 && lock < 50) {
            logEvent(QStringLiteral("Link lost (alert)"));
            QApplication::alert(this, 0);
            QApplication::beep();
        }
        lastLock_ = lock;
        lastFrames_ = frames;
        lastBad_ = bad;
        lastQuality_ = q;
        linkStateText_ = inputStopped ? QStringLiteral("Stopped") : state;
        linkStateTone_ = inputStopped ? QStringLiteral("idle")
                       : (lock >= 100 ? QStringLiteral("ok") : (lock >= 50 ? QStringLiteral("warn") : QStringLiteral("error")));
        if (systemPage_) {
            systemPage_->setDecoderSummary(gk2a
                ? QStringLiteral("%1 · RS pass %2 · RS reject %3\nsoft quality %4% · carrier offset %5 kHz")
                .arg(state).arg(frames).arg(bad).arg(q * 100.0f, 0, 'f', 1)
                .arg(carrier / 1000.0f, 0, 'f', 1)
                : QStringLiteral("%1 · good %2 / bad %3\nSoft quality %4% · carrier offset %5 kHz")
                    .arg(state).arg(frames).arg(bad).arg(q * 100.0f, 0, 'f', 1)
                    .arg(carrier / 1000.0f, 0, 'f', 1));
        }
    });
    connect(decoder_.get(), &NadirStationDecoder::imagePreviewReady, this, &MainWindow::onImagePreviewReady);
    connect(decoder_.get(), &NadirStationDecoder::hritLivePreviewReady, this, &MainWindow::onHritLivePreview);
    connect(decoder_.get(), &NadirStationDecoder::imageReady, this, &MainWindow::onImageReady);
    // Producer threads replace their previous display frame. The GUI consumes only the newest
    // frame on a fixed timer, so a slow repaint cannot build an unbounded signal queue.
    const auto acceptSpectrum=[mailbox=displayMailbox_](const std::vector<float>& db,double sr){
        std::lock_guard lock(mailbox->mutex);
        mailbox->spectrum=db;mailbox->sampleRate=sr;
    };
    connect(receiver_.get(), &PlutoReceiver::spectrumReady, this, acceptSpectrum, Qt::DirectConnection);
    connect(wavReceiver_.get(), &WavReceiver::spectrumReady, this, acceptSpectrum, Qt::DirectConnection);
    displayTimer_=new QTimer(this);
    displayTimer_->setTimerType(Qt::PreciseTimer);
    displayTimer_->setInterval(25);
    connect(displayTimer_,&QTimer::timeout,this,[this]{
        std::vector<float> db;double rate=0.0;
        std::shared_ptr<std::vector<std::complex<float>>> points;
        {
            std::lock_guard lock(displayMailbox_->mutex);
            db.swap(displayMailbox_->spectrum);
            rate=displayMailbox_->sampleRate;
            points=std::move(displayMailbox_->constellation);
        }
        if(!db.empty()&&spectrum_)spectrum_->setSpectrum(db,float(rate));
        if(points&&constellation_)constellation_->setPoints(*points);
        if(decoderPage_){
            if(!db.empty())decoderPage_->setSpectrum(db,rate);
            if(points)decoderPage_->setConstellation(*points);
        }
    });
    displayTimer_->start();
    connect(sessions_, &QListWidget::itemClicked, this, &MainWindow::selectImage);
    connect(importSession_, &QPushButton::clicked, this, &MainWindow::importSessionJson);
    connect(saveImage_, &QPushButton::clicked, this, &MainWindow::saveCurrentImage);
    connect(product_, &QComboBox::currentIndexChanged, this, [this] {
        const int index = product_ ? product_->currentIndex() : -1;
        if (sessions_ && sessions_->currentItem() && index >= 0 && index < currentPaths_.size())
            sessions_->currentItem()->setData(Qt::UserRole + 1, currentPaths_.at(index));
        applyImageProcessing();
    });
    connect(wavBrowse_, &QPushButton::clicked, this, &MainWindow::browseWav);
    connect(wavPath_, &QLineEdit::editingFinished, this, [this] { updateInputUi(); });
    connect(inputSource_, &QComboBox::currentIndexChanged, this, [this](int index) {
        if (activeInput_ != InputSource::None) {
            if (index != static_cast<int>(activeInput_)) {
                const QSignalBlocker block(inputSource_);
                inputSource_->setCurrentIndex(static_cast<int>(activeInput_));
            }
            return;
        }
        applySatelliteSelection();
        updateInputUi();
    });
    connect(wavSpeed_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (!wavRange_) return;
        // Every playback mode, Max included, plays the full file; Range can still be narrowed by hand.
        wavRange_->setCurrentIndex(0);
    });
    updateInputUi();

    makeMenus();
    if (startupProgress) startupProgress(QStringLiteral("Restoring the last session…"), 0.92);
    restoreUiState();
    statusBar()->showMessage(QStringLiteral("Ready"), 4000);

    // GUI heartbeat for the hang watchdog in runtime_logger: a stall of the event loop longer than
    // its limit is logged with a dump of every thread's stack, so a freeze leaves evidence.
    auto* heartbeat = new QTimer(this);
    heartbeat->setInterval(500);
    connect(heartbeat, &QTimer::timeout, this, [] { nadirstation::log::beat(0); });
    heartbeat->start();
    nadirstation::log::setArmed(0, true);
}

MainWindow::~MainWindow() {
    // A regional-product generator still running is a child QProcess; ~QWidget deletes it after this
    // class's members are gone, its destructor kills the process and emits finished(), and the
    // handler then used sessions_ and the status bar of a half-destroyed window (a crash on exit
    // seen when the window closed right after an image). Cut its connections first.
    for (QProcess* process : findChildren<QProcess*>()) {
        process->disconnect(this);
        if (process->state() != QProcess::NotRunning) {
            nadirstation::log::write(QStringLiteral("Closing: regional-product generator stopped before it finished (%1)")
                .arg(process->program()));
            process->kill();
            process->waitForFinished(2000);
        }
    }
    stopRx();
    if (iqRecorder_) iqRecorder_->stop();
    if (wavReceiver_) wavReceiver_->stop();
    if (rtlReceiver_) rtlReceiver_->stop();
    if (hackrfReceiver_) hackrfReceiver_->stop();
    if (decoderThread_) {
        QMetaObject::invokeMethod(decoder_.get(), [this] { decoder_->setEnabled(false); }, Qt::BlockingQueuedConnection);
        decoderThread_->quit();
        decoderThread_->wait();
    }
}

// GK-2A imagery is shown as decoded: the 3x3 median filter is for FY-2's noisy S-VISSR products and
// is off for every GK-2A selection whatever the checkbox says.
void MainWindow::applyMedianPolicy() {
    // The 3x3 median is for FY-2 S-VISSR line noise. GK-2A and ELEKTRO-L arrive Reed-Solomon
    // checked and compressed at the source; a median only softens them.
    const bool gk2a = selectedSatelliteName_.startsWith(QStringLiteral("GK-2A"));
    const bool elektro = selectedSatelliteName_.startsWith(QStringLiteral("ELEKTRO-L LRIT"))
                      || selectedSatelliteName_.startsWith(QStringLiteral("ELEKTRO-L HRIT"))
                      || selectedSatelliteName_.startsWith(QStringLiteral("MetOp"))
                      || selectedSatelliteName_.startsWith(QStringLiteral("METEOR"));
    nadirstation::products::setMedianFilterEnabled(medianUser_ && !gk2a && !elektro);
}

void MainWindow::applySatelliteSelection() {
    const int satelliteId = satelliteSelector_ ? satelliteSelector_->currentData().toInt() : 0;
    if (visCalButton_ != nullptr)
        visCalButton_->setVisible(satelliteId == 8 || satelliteId == 9);
    // Preset the downlink frequency with the spacecraft, since the three are different services
    // and a wrong centre frequency means no lock at all. The values come from the operators'
    // specifications, not from tuning: FY-2's S-VISSR channels and GK-2A's LRIT carrier are
    // documented frequencies, and GK-2A LRIT is specified in the NMSC mission specification
    // (Table 9.1) as 1692.14 MHz with 1 MHz bandwidth.
    double presetMHz = 0.0;
    if (satelliteId == 8) {
        selectedSatelliteName_ = QStringLiteral("FY-2G");
        selectedSatelliteLongitudeDeg_ = 99.2;
        satelliteSelected_ = true;
        presetMHz = 1687.500;
        satelliteState_->setText(QStringLiteral("Manual selection · FY-2G · 99.2°E · S-VISSR"));
    } else if (satelliteId == 9) {
        selectedSatelliteName_ = QStringLiteral("FY-2H");
        selectedSatelliteLongitudeDeg_ = 79.0;
        satelliteSelected_ = true;
        // FY-2H uses the same S-VISSR channel as FY-2G, so this preset is the same value. An
        // earlier version offered a second frequency here; it was removed rather than left as an
        // alternative, because a preset that does not match the satellite is worse than none --
        // it looks authoritative and sends the receiver somewhere the signal is not.
        presetMHz = 1687.500;
        satelliteState_->setText(QStringLiteral("Manual selection · FY-2H · 79.0°E · S-VISSR"));
    } else if (satelliteId == 10) {
        selectedSatelliteName_ = QStringLiteral("GK-2A");
        selectedSatelliteLongitudeDeg_ = 128.2;
        satelliteSelected_ = true;
        presetMHz = 1692.140;
        satelliteState_->setText(QStringLiteral("Manual selection · GK-2A · 128.2°E · LRIT "
                                                "64 kbps BPSK"));
    } else if (satelliteId == 12) {
        selectedSatelliteName_=QStringLiteral("GK-2A HRIT");
        selectedSatelliteLongitudeDeg_=128.2; satelliteSelected_=true; presetMHz=1695.400;
        satelliteState_->setText(QStringLiteral("GK-2A · 128.2°E · HRIT QPSK 3 Msym/s · AMI 5 channels"));
    } else if (satelliteId == 13) {
        // GOES-N/O/P GVAR: BPSK 2.11136 Msym/s, differential NRZ-S, no FEC. The sub-satellite longitude is
        // read from the stream (block 0), so none is preset.
        selectedSatelliteName_ = QStringLiteral("GOES GVAR");
        selectedSatelliteLongitudeDeg_ = 0.0;
        satelliteSelected_ = true;
        presetMHz = 1685.700;
        satelliteState_->setText(QStringLiteral("GOES GVAR · BPSK 2.111 Msym/s · Imager 5 channels"));
    } else if (satelliteId == 18) {
        // GOES-N/O/P Sensor Data: raw imager (8 visible + IR detectors) and sounder, BPSK in quadrature.
        selectedSatelliteName_ = QStringLiteral("GOES SD");
        selectedSatelliteLongitudeDeg_ = 0.0;
        satelliteSelected_ = true;
        presetMHz = 1676.000;
        satelliteState_->setText(QStringLiteral("GOES SD · imager 2.6208 Mbit/s + sounder 40 kbit/s · raw detector data"));
    } else if (satelliteId == 14 || satelliteId == 15) {
        // ELEKTRO-L xRIT at 1691 MHz: LRIT BPSK 294 ksym/s or HRIT OQPSK 1.15 Msym/s (SatDump's
        // Elektro_Arktika pipelines). The sub-satellite point comes from the image navigation header.
        const bool hrit = satelliteId == 15;
        selectedSatelliteName_ = hrit ? QStringLiteral("ELEKTRO-L HRIT") : QStringLiteral("ELEKTRO-L LRIT");
        selectedSatelliteLongitudeDeg_ = 0.0;
        satelliteSelected_ = true;
        presetMHz = 1691.000;
        satelliteState_->setText(hrit ? QStringLiteral("ELEKTRO-L · HRIT OQPSK 1.15 Msym/s · MSU-GS")
                                      : QStringLiteral("ELEKTRO-L · LRIT BPSK 294 ksym/s · MSU-GS"));
    } else if (satelliteId == 16 || satelliteId == 17) {
        // Polar orbiters: MetOp AHRPT (QPSK 2.33 Msym/s) and METEOR-M HRPT (PM / Manchester 665.4 kbit/s).
        // No fixed sub-satellite point; the pictures are scan lines along the ground track.
        const bool metop = satelliteId == 16;
        selectedSatelliteName_ = metop ? QStringLiteral("MetOp AHRPT") : QStringLiteral("METEOR HRPT");
        selectedSatelliteLongitudeDeg_ = 0.0;
        satelliteSelected_ = true;
        presetMHz = metop ? 1701.300 : 1700.000;
        satelliteState_->setText(metop ? QStringLiteral("MetOp · AHRPT QPSK 2.33 Msym/s · AVHRR/3")
                                       : QStringLiteral("METEOR-M · HRPT PM/Manchester 665.4 kbit/s · MSU-MR"));
    } else if (satelliteId == 11) {
        // ELEKTRO-L GGAK: the heliogeophysical telemetry downlink.
        // No imagery, so no sub-satellite longitude is set for projection.
        selectedSatelliteName_ = QStringLiteral("ELEKTRO-L GGAK");
        selectedSatelliteLongitudeDeg_ = 0.0;
        satelliteSelected_ = true;
        presetMHz = 1693.000;
        satelliteState_->setText(QStringLiteral("Manual selection · ELEKTRO-L · GGAK telemetry 5 kBd"));
    } else {
        selectedSatelliteName_.clear();
        selectedSatelliteLongitudeDeg_ = 0.0;
        satelliteSelected_ = false;
        satelliteState_->setText(QStringLiteral("Select a satellite · projection locked"));
    }
    applyMedianPolicy();
    // Where the SNR read-out turns from red to amber to green for this link (dB on the read-out:
    // about where frames start to come, and where they come with margin). From the decoders'
    // measured thresholds; METEOR's read-out is the bit-decision SNR of an uncoded link.
    if (snrMeter_) {
        const std::pair<float, float> z =
              satelliteId == 18 ? std::pair{4.0f, 8.0f}   // GOES SD: needs ~9 dB for clean pictures; soft decoding works below
            : satelliteId == 13 ? std::pair{3.0f, 6.0f}   // GVAR: red < 3, amber < 6, green < 10, teal above
            : (satelliteId == 8 || satelliteId == 9) ? std::pair{1.5f, 4.0f}
            : satelliteId == 10 ? std::pair{1.0f, 4.0f}
            : satelliteId == 11 ? std::pair{6.0f, 10.0f}
            : satelliteId == 12 ? std::pair{2.0f, 5.0f}
            : satelliteId == 14 ? std::pair{1.0f, 4.0f}
            : satelliteId == 15 ? std::pair{2.5f, 5.5f}
            : satelliteId == 16 ? std::pair{4.0f, 5.5f}   // MetOp: rate 3/4; 99% of CADUs at Es/N0 5 dB (real pass + noise)
            : satelliteId == 17 ? std::pair{6.0f, 9.0f} : std::pair{2.0f, 6.0f};
        snrMeter_->setScale(z.first, z.second, satelliteId == 13 ? 10.0f : -1.0f);
        if (snrTrend_) snrTrend_->setZones(snrMeter_->zones());
    }
    if (presetMHz > 0.0 && freq_) {
        freq_->setValue(presetMHz);
    }
    // The gain last used for this satellite and input; ELEKTRO-L GGAK on the Pluto starts at 60 dB,
    // measured on 10-23: about 14 dB symbol SNR and no checksum errors, against bursts of errors
    // at 35 dB.
    if (gain_ && satelliteSelected_) {
        const int source = inputSource_ ? inputSource_->currentIndex() : 0;
        QSettings s(QStringLiteral("AGcov"), QStringLiteral("NadirStation"));
        const QVariant saved = s.value(QStringLiteral("rx/gainDb/%1/%2").arg(source).arg(satelliteId));
        if (saved.isValid()) gain_->setValue(saved.toDouble());
        else if (satelliteId == 11 && source == 0) gain_->setValue(60.0);
    }
    // The symbol rate belongs to the spacecraft, so it moves with the selection. The decoder's
    // PSK chain is the same object the constellation view is fed from, so retuning it here is what
    // makes the constellation meaningful for whichever satellite is chosen: FY-2 S-VISSR runs at
    // 660 kbaud and GK-2A LRIT at 128 kbaud.
    if (decoder_) {
        const double symbolRate = (satelliteId == 8 || satelliteId == 9) ? 660000.0
                                 : satelliteId == 10 ? 128000.0
                                 : satelliteId == 11 ? 5000.0 : satelliteId == 12 ? 3000000.0
                                 : satelliteId == 13 ? 2111360.0 : satelliteId == 18 ? 2620800.0
                                 : satelliteId == 14 ? 294000.0 : satelliteId == 15 ? 1150000.0
                                 : satelliteId == 16 ? 2333333.0 : satelliteId == 17 ? 665400.0 : 0.0;
        const QString spacecraft = selectedSatelliteName_;
        NadirStationDecoder* const target = decoder_.get();
        const auto configureDecoder = [target, symbolRate, spacecraft] {
            if (symbolRate > 0.0) target->setSymbolRate(symbolRate);
            target->setSatelliteName(spacecraft);
        };
        // The decoder owns stateful filters and frame assemblers. Retuning it directly from the
        // UI races the IQ worker and can leave half a block using the old rate and half using the
        // new one. Serialize configuration with sample processing on its QObject thread.
        if (decoderThread_ && decoderThread_->isRunning())
            QMetaObject::invokeMethod(target, configureDecoder, Qt::QueuedConnection);
        else
            configureDecoder();
    }
    // The two services need different demodulator settings, so the control panel is biased
    // towards the right ones rather than left at whatever the previous spacecraft needed.
    const bool rtlInput = inputSource_ && inputSource_->currentIndex() == static_cast<int>(InputSource::Rtl);
    // The RF bandwidth box belongs to the user: it starts on None and a satellite choice no longer moves it.
    const auto setBandwidthPreset = [rtlInput](int hz) { (void)rtlInput; (void)hz; };
    if(satelliteId==12) {
        if(sampleRate_)sampleRate_->setCurrentText(QStringLiteral("6.000 MS/s"));
        setBandwidthPreset(5200000);
        // NMSC HRIT Mission Specification, Table 9.1: root-raised cosine, roll-off 0.5.
    }
    if(satelliteId==13) {
        // The signal occupies 3.17 MHz (2.111 Msym/s, roll-off 0.5): 3 MS/s keeps all but the faint outer
        // roll-off and stays clear of FY-2 at 1687.5 MHz.
        if(sampleRate_)sampleRate_->setCurrentText(QStringLiteral("3.000 MS/s"));
        setBandwidthPreset(5200000);
    }
    if (satelliteId == 10 || satelliteId == 11) {
        setBandwidthPreset(1000000);
    }
    if (satelliteId == 16) {
        // 2.33 Msym/s with roll-off 0.5 occupies 3.5 MHz, and the Doppler moves it +-40 kHz.
        if (sampleRate_) sampleRate_->setCurrentText(QStringLiteral("6.000 MS/s"));
        setBandwidthPreset(5200000);
    }
    if (satelliteId == 17) {
        // Manchester at 665.4 kbit/s puts its energy within about +-1.3 MHz of the carrier.
        if (sampleRate_) sampleRate_->setCurrentText(QStringLiteral("3.000 MS/s"));
        setBandwidthPreset(5200000);
    }
    if (satelliteId == 14 || satelliteId == 15) {
        // LRIT occupies 0.44 MHz, HRIT 1.55 MHz; 3 MS/s holds either with room for the carrier offset.
        if (sampleRate_) sampleRate_->setCurrentText(QStringLiteral("3.000 MS/s"));
        setBandwidthPreset(satelliteId == 15 ? 5200000 : 1000000);
    }

    // Demodulator defaults for the service, applied on every satellite change; both boxes accept a
    // typed value afterwards, which is used from the next Start Decode. RRC roll-off 0.5 is what every
    // service here specifies. The PLL bandwidth is the BPSK carrier loop; GK-2A HRIT runs its own QPSK
    // demodulator with fixed loops (carrier 0.0003, timing 0.0002), so the box shows that and is locked.
    {
        const bool hrit = satelliteId == 12 || satelliteId == 15 || satelliteId == 16 || satelliteId == 17;
        // ELEKTRO-L LRIT: a narrower carrier loop than the BPSK default, 0.005 against 0.020, passed
        // 53 % against 44 % and 62 % against 54 % of CADUs on the two LRIT recordings at the same
        // noise (and the same 637 / 635 frames over a 40 s span with a carrier jump in it).
        const QString pll = satelliteId == 12 ? QStringLiteral("0.0003") : satelliteId == 15 ? QStringLiteral("0.0003")
                          : satelliteId == 14 ? QStringLiteral("0.005")
                          : satelliteId == 13 ? QStringLiteral("0.002") : satelliteId == 18 ? QStringLiteral("0.003") : QStringLiteral("0.020");
        // ELEKTRO-L HRIT: the downlink's spectrum follows sinc^2 (nearly rectangular pulses, band-limited
        // on the way), and a wide RRC matches it best. Measured on two recordings with noise added:
        // at the same noise, 0.85 passed 74 % of CADUs where 0.35 passed 30 % (6 % on the second), about
        // 0.4 dB of threshold; a pure integrate-and-dump came second (it ignores the band limit).
        // ELEKTRO-L LRIT's spectrum is the same shape, and 0.80 beats 0.50 there as well (at the same
        // noise 98 % against 92 % and 98 % against 88 % of CADUs on the two LRIT recordings).
        // Everything else here is shaped with roll-off 0.5.
        if (rrcAlpha_) rrcAlpha_->setCurrentText(satelliteId == 15 ? QStringLiteral("0.85")
                                                 : satelliteId == 14 ? QStringLiteral("0.80") : QStringLiteral("0.50"));
        if (pllBandwidth_) {
            pllBandwidth_->setCurrentText(pll);
            pllBandwidth_->setEnabled(!hrit);
            pllBandwidth_->setToolTip(hrit
                ? QStringLiteral("This service uses its own QPSK / OQPSK demodulator with fixed carrier and timing loops; this setting does not apply.")
                : QStringLiteral("Carrier loop bandwidth (normalised, 0.0005–0.25). Type a value or pick one; used from the next Start Decode. "
                                 "Changing the satellite restores its default."));
        }
    }

    // The receiver and the service, nothing more: the link parameters are not something to show the operator.
    if (modeNote_ != nullptr) {
        const int source = inputSource_ ? inputSource_->currentIndex() : 0;
        const QString receiverName = source == 1 ? QStringLiteral("RTL-SDR")
                                   : source == 2 ? QStringLiteral("File input")
                                   : source == 3 ? QStringLiteral("HackRF One") : QStringLiteral("PlutoSDR");
        const QString service = satelliteId == 16 ? QStringLiteral("MetOp AHRPT")
                              : satelliteId == 17 ? QStringLiteral("METEOR HRPT")
                              : satelliteId == 15 ? QStringLiteral("ELEKTRO-L HRIT")
                              : satelliteId == 14 ? QStringLiteral("ELEKTRO-L LRIT")
                              : satelliteId == 18 ? QStringLiteral("GOES SD")
                              : satelliteId == 13 ? QStringLiteral("GOES GVAR")
                              : satelliteId == 12 ? QStringLiteral("GK-2A HRIT")
                              : satelliteId == 11 ? QStringLiteral("ELEKTRO-L GGAK")
                              : satelliteId == 10 ? QStringLiteral("GK-2A LRIT")
                              : satelliteId == 8 ? QStringLiteral("FY-2G")
                              : satelliteId == 9 ? QStringLiteral("FY-2H")
                              : QStringLiteral("No satellite selected");
        modeNote_->setText(receiverName + QStringLiteral("\n") + service);
    }
    if (constellation_ != nullptr) {
        // Absolute or differential is the difference that matters to a reader: an absolute service
        // needs its polarity resolved and a differential one does not.
        if (satelliteId == 11)
            constellation_->setAnnotation(QStringLiteral("Constellation · GGAK BPSK (5 kHz subcarrier)"),
                                          QStringLiteral("Re axis · after carrier PLL and subcarrier shift"));
        else if (satelliteId == 16)
            constellation_->setAnnotation(QStringLiteral("Constellation · MetOp AHRPT QPSK"),
                                          QStringLiteral("Matched-filter symbols · I/Q · four decision points"));
        else if (satelliteId == 17)
            constellation_->setAnnotation(QStringLiteral("Constellation · METEOR HRPT soft bits"),
                                          QStringLiteral("Manchester matched filter · Re axis · 2 clusters"));
        else if (satelliteId == 15)
            constellation_->setAnnotation(QStringLiteral("Constellation · ELEKTRO-L HRIT OQPSK"),
                                          QStringLiteral("I at the symbol, Q half a symbol later · four points"));
        else if (satelliteId == 14)
            constellation_->setAnnotation(QStringLiteral("Constellation · ELEKTRO-L LRIT BPSK"),
                                          QStringLiteral("Re axis · 2 clusters · polarity resolved"));
        else if (satelliteId == 18)
            constellation_->setAnnotation(QStringLiteral("Constellation · GOES SD imager BPSK (NRZ-M, 2.6208 Mbit/s)"),
                                          QStringLiteral("Re axis · 2 clusters · near threshold, soft decisions"));
        else if (satelliteId == 13)
            constellation_->setAnnotation(QStringLiteral("Constellation · GOES GVAR BPSK (differential NRZ-S)"),
                                          QStringLiteral("Re axis · 2 clusters · polarity free"));
        else if (satelliteId == 12)
            constellation_->setAnnotation(QStringLiteral("Constellation · GK-2A HRIT QPSK"),
                                          QStringLiteral("Matched-filter symbols · I/Q · four decision points"));
        else if (satelliteId == 10)
            constellation_->setAnnotation(QStringLiteral("Constellation · BPSK (absolute, NRZ-L)"),
                                          QStringLiteral("Re axis · 2 clusters · polarity resolved"));
        else if (satelliteId == 8 || satelliteId == 9)
            constellation_->setAnnotation(QStringLiteral("Constellation · DPSK (differential)"),
                                          QStringLiteral("Re +Im · 1:1 aspect"));
        else
            constellation_->setAnnotation(QStringLiteral("Constellation · no satellite selected"),
                                          QStringLiteral("Re +Im · 1:1 aspect"));
    }
    if (chainHint_ != nullptr) {
        chainHint_->setText(satelliteId == 16
            ? QStringLiteral("IQ → carrier search ±100 kHz → RRC 0.5 → Gardner → QPSK Costas → soft Viterbi → RS → CCSDS packets → AVHRR lines (live)")
            : satelliteId == 17
            ? QStringLiteral("IQ → residual-carrier PLL → PM detection → Manchester matched filter → M&M clock → soft frame sync → MSU-MR lines (live)")
            : satelliteId == 11
            ? QStringLiteral("IQ → carrier search (FFT) → mix + decimate to 50 kS/s → AGC → carrier PLL → "
                             "PM phase → 5 kHz subcarrier → RRC → Costas → M&M → ASM deframe → checksum → "
                             "SKL / SKIF / raw frames")
            : satelliteId == 15
            ? QStringLiteral("IQ → RRC 0.85 → offset-Q Gardner → QPSK Costas → soft Viterbi → RS → xRIT → JPEG → channels")
            : satelliteId == 14
            ? QStringLiteral("IQ → RRC 0.8 → Gardner → BPSK Costas → soft Viterbi → RS → xRIT → JPEG → channels")
            : satelliteId == 18
            ? QStringLiteral("IQ → RRC → timing → BPSK Costas → soft NRZ-M → PN sync (14 bit) → dewhiten → soft bit-plane vote → scan split / direction → detector images + sounder")
            : satelliteId == 13
            ? QStringLiteral("IQ → RRC → timing → BPSK Costas → soft sync (PN 10032) → soft header → pixel soft recovery → channels + false colour")
            : satelliteId == 12
            ? QStringLiteral("IQ → RRC → Gardner → QPSK Costas → soft Viterbi → RS → DES → VIS live")
            : satelliteId == 10
            ? QStringLiteral("IQ → RRC → AGC → M&M timing → BPSK Costas PLL → Viterbi → "
                             "CADU sync → RS check → LRIT files → images")
            : QStringLiteral("IQ → RRC → AGC → M&M timing → BPSK Costas PLL → PN / S-VISSR "
                             "deframe → image products"));
    }
    // Called once at construction, before the labels exist, so guard on that too.
    if (modeNote_ == nullptr && chainHint_ == nullptr) return;
}

// The satellite a visible name refers to. It exists because the combo box's data value and the entry
// it displays can disagree: the value is what the code reads, the text is what the operator sees, and
// when they differed the operator was refused a start while looking at a satellite they had chosen.
// The ids are the ones the selector's entries carry.
int MainWindow::satelliteIdFromName(const QString& text) {
    if (text.contains(QStringLiteral("GK-2A HRIT"))) return 12;
    if (text.contains(QStringLiteral("GOES SD"))) return 18;
    if (text.contains(QStringLiteral("GOES GVAR"))) return 13;
    if (text.contains(QStringLiteral("ELEKTRO-L LRIT"), Qt::CaseInsensitive)) return 14;
    if (text.contains(QStringLiteral("ELEKTRO-L HRIT"), Qt::CaseInsensitive)) return 15;
    if (text.contains(QStringLiteral("MetOp"), Qt::CaseInsensitive)) return 16;
    if (text.contains(QStringLiteral("METEOR"), Qt::CaseInsensitive)) return 17;
    if (text.contains(QStringLiteral("GK-2A"))) return 10;
    if (text.contains(QStringLiteral("ELEKTRO"), Qt::CaseInsensitive)) return 11;
    if (text.contains(QStringLiteral("FY-2G"))) return 8;
    if (text.contains(QStringLiteral("FY-2H"))) return 9;
    return 0;
}

void MainWindow::selectSatelliteByName(const QString& name) {
    if (!satelliteSelector_ || name.isEmpty()) return;
    for (int i = 0; i < satelliteSelector_->count(); ++i) {
        if (satelliteSelector_->itemText(i).contains(name, Qt::CaseInsensitive)) {
            // Setting the index runs the ordinary selection handler, so there is one place that
            // decides what choosing a satellite implies rather than two that can disagree.
            satelliteSelector_->setCurrentIndex(i);
            return;
        }
    }
}

void MainWindow::setGlobeLead(int steps) {
    if (globePage_) globePage_->setLead(steps);
}

void MainWindow::setGlobeQuery(double latDeg, double lonDeg) {
    if (globePage_) globePage_->queryPoint(latDeg, lonDeg);
}

void MainWindow::setGlobeImage(const QString& name) {
    if (globePage_) globePage_->selectImage(name);
}

void MainWindow::runWavFileForDiagnosis(const QString& path, bool autoSelectSatellite) {
    if (!inputSource_ || !wavPath_ || !wavSpeed_ || !wavRange_) return;
    diagnosticWavMode_ = true;
    inputSource_->setCurrentIndex(2);
    wavPath_->setText(path);
    if (autoSelectSatellite) selectSatelliteForFile(path);
    // 4x playback processes the entire recording while keeping the normal
    // streaming DSP chain and its stateful block boundaries intact.
    wavSpeed_->setCurrentIndex(1);
    wavRange_->setCurrentIndex(0);
    updateInputUi();
    QTimer::singleShot(0, this, [this] { startRx(); });
}




void MainWindow::reviewGgakFrames(const QString& caduPath, const QString& tab) {
    if (ggakPage_) ggakPage_->loadFramesForReview(caduPath, tab);
}

bool MainWindow::exportSpaceWeatherPng(const QString& path) {
    showPageByName(QStringLiteral("space"));
    QCoreApplication::processEvents();
    return spaceWeatherPage_ && spaceWeatherPage_->savePng(path);
}

bool MainWindow::exportGgakPng(const QString& tab, const QString& path) {
    showPageByName(QStringLiteral("ggak"));
    QCoreApplication::processEvents();
    return ggakPage_ && ggakPage_->saveTabPng(tab, path);
}

void MainWindow::refreshSystemStatus() {
    if (!systemPage_) return;
    const bool receiving = (receiver_ && receiver_->isRunning()) || (wavReceiver_ && wavReceiver_->isRunning()) ||
                           (rtlReceiver_ && rtlReceiver_->isRunning()) || (hackrfReceiver_ && hackrfReceiver_->isRunning());
    const bool decoding = decoder_ && decoder_->enabled();
    const int sourceIndex = inputSource_ ? inputSource_->currentIndex() : 0;
    const QString source = sourceIndex == 2 ? QStringLiteral("WAV playback")
                         : sourceIndex == 1 ? QStringLiteral("RTL-SDR · USB")
                         : sourceIndex == 3 ? QStringLiteral("HackRF One · USB") : QStringLiteral("PlutoSDR · 192.168.3.10");
    QString state;
    if (receiving && decoding) state = QStringLiteral("RECEIVING · DECODING");
    else if (receiving) state = QStringLiteral("RECEIVING · IDLE DECODE");
    else if (decoding) state = QStringLiteral("RX IDLE · DECODER ARMED");
    else state = QStringLiteral("IDLE · READY");
    systemPage_->setStationSummary(state, source,
        sampleRate_ ? QStringLiteral("%1 MS/s").arg(sampleRateHz() / 1e6, 0, 'f', 3) : QStringLiteral("—"),
        bandwidth_ ? bandwidth_->currentText() : QStringLiteral("—"),
        gain_ ? gain_->value() : 0.0);
}

bool MainWindow::requireSatellite(const QString& action) {
    // A satellite has to be chosen before anything starts, whatever the input is. Without one
    // applySatelliteSelection presets nothing: the receiver would start on whatever the frequency
    // box holds (FY-2's 1687.5 MHz) with the decoder configured for a service that is not being
    // transmitted, and the status would read NOSYNC without saying why.
    //
    // The choice is resolved from what the box shows, because the value it reports and the entry
    // it displays can disagree. The placeholder is the only entry that does not name a satellite,
    // so both have to say "nothing chosen" before a start is refused: a guard must not be able to
    // refuse a correct action.
    const bool placeholder = satelliteSelector_ == nullptr
        || satelliteSelector_->currentText().startsWith(QStringLiteral("Select satellite"));
    const int chosenId = (satelliteSelector_ == nullptr)
        ? 0
        : (satelliteSelector_->currentData().toInt() != 0
               ? satelliteSelector_->currentData().toInt()
               : satelliteIdFromName(satelliteSelector_->currentText()));
    if (!(placeholder && chosenId == 0)) return true;
    nadirstation::log::write(QStringLiteral("GUI %1 refused: no satellite selected (input %2)")
        .arg(action).arg(inputSource_ ? inputSource_->currentIndex() : -1));
    nadirstation::log::flush();
    QMessageBox::warning(this, QStringLiteral("Select a satellite"),
        QStringLiteral("Choose a satellite before starting: the downlink frequency, the symbol rate "
                       "and the decoder chain all come from it, and none of them is set until one "
                       "is chosen.\n\nGK-2A is 1692.140 MHz at 128 kbaud; FY-2G and FY-2H are "
                       "1687.500 MHz at 660 kbaud; ELEKTRO-L GGAK is 1693.000 MHz at 5 kBd."));
    return false;
}

void MainWindow::startRx() {
    if (activeInput_ != InputSource::None || !requireSatellite(QStringLiteral("startRx"))) return;
    if (!receiver_->readyToStart()) {
        statusBar()->showMessage(QStringLiteral("Pluto is still stopping. Try again when it disconnects."), 5000);
        return;
    }
    updateInputUi();
    if (inputSource_ && inputSource_->currentIndex() == 2) {
        const QString path = wavPath_ ? wavPath_->text().trimmed() : QString();
        if (path.isEmpty() || !QFileInfo::exists(path)) {
            QMessageBox::warning(this, "File input", "Choose a valid baseband file first (WAV / cs16 / cs8 / cu8).");
            return;
        }
        const double speed = (wavSpeed_->currentIndex() == 0) ? 1.0 :
                             (wavSpeed_->currentIndex() == 1 ? 4.0 : 1.0e9);
        const double fraction = (wavRange_->currentIndex() == 0) ? 1.0 :
                                (wavRange_->currentIndex() == 1 ? 0.5 : 0.25);
        wavReceiver_->stop();
        wavReceiver_->setFilePath(path);
        wavReceiver_->setPlaybackFactor(speed);
        wavReceiver_->setFraction(fraction);
        // A raw I/Q file carries no rate of its own unless a sidecar or its name says it; the
        // Sample Rate box ("2.500 MS/s") is the fallback.
        wavReceiver_->setRawFallbackRate(sampleRate_ ? sampleRateHz() : 0.0);
        if (decoder_) decoder_->setIngressBackpressureEnabled(true);

        // IMPORTANT: parse the WAV header before the worker starts emitting IQ.
        // The previous version started the worker first, then updated the decoder
        // sample rate; a fast Max replay could deliver the first block while the
        // decoder was still at its default 6 MS/s.
        if (!wavReceiver_->prepare()) {
            if (decoder_) decoder_->setIngressBackpressureEnabled(false);
            return;
        }
        const double wavSr = wavReceiver_->sampleRate();
        if (decoderThread_ && decoderThread_->isRunning()) {
            QMetaObject::invokeMethod(decoder_.get(), [this, wavSr] {
                decoder_->setSampleRate(wavSr);
                nadirstation::log::write(QStringLiteral("WAV decoder sample rate armed before playback: %1").arg(wavSr, 0, 'f', 0));
            }, Qt::BlockingQueuedConnection);
        } else {
            decoder_->setSampleRate(wavSr);
        }
        // Max-speed WAV uses the same bounded streaming transport as real-time playback.
        // The persistent demodulator keeps the DSP state across blocks.
        if (decoder_ && !decoder_->enabled()) {
            const float alpha = rrcAlpha_ ? rrcAlpha_->currentText().toFloat() : 0.5f;
            const float pll = pllBandwidth_ ? pllBandwidth_->currentText().toFloat() : 0.02f;
            QMetaObject::invokeMethod(decoder_.get(), [this, alpha, pll] {
                decoder_->setDemodParameters(alpha, pll);
                decoder_->setEnabled(true);
            },
                                      Qt::BlockingQueuedConnection);
            decodeBtn_->setText("Stop Decode");
        }
        if (!wavReceiver_->start()) {
            decoder_->setIngressBackpressureEnabled(false);
            stopDecode();
            return;
        }
        activeInput_ = InputSource::File;
        rxBtn_->setText("Stop File Input");
        updateInputUi();
        statusBar()->showMessage(QStringLiteral("File input running · %1 · %2 MS/s (%3)")
            .arg(QString::fromLatin1(WavReceiver::formatName(wavReceiver_->format())))
            .arg(wavSr / 1e6, 0, 'f', 3).arg(wavReceiver_->rateSource()));
        return;
    }

    if (inputSource_ && inputSource_->currentIndex() == 1 && (selectedSatelliteName_==QStringLiteral("GOES GVAR")||selectedSatelliteName_==QStringLiteral("GOES SD"))) {
        statusBar()->showMessage(QStringLiteral("GOES GVAR needs wideband IQ of at least 2.75 MS/s (the RTL-SDR tops out at 2.4 MS/s); use a Pluto or file input."),10000);
        return;
    }
    // GK-2A HRIT from an RTL-SDR is an experiment at 3.2 MS/s, the most it delivers: the signal is 4.5 MHz
    // wide, so the roll-off is cut off (see processHritSamples). Any other rate cannot work.
    const bool rtlHrit = inputSource_ && inputSource_->currentIndex() == 1 && selectedSatelliteName_==QStringLiteral("GK-2A HRIT");
    if (rtlHrit && sampleRate_ && std::abs(sampleRateHz() - 3200000.0) > 1.0) {
        statusBar()->showMessage(QStringLiteral("With an RTL-SDR, GK-2A HRIT can only be demodulated experimentally at 3.2 MS/s (the signal is 4.5 MHz wide, costing about 2–3 dB of sensitivity); select 3.200 MS/s, or use a Pluto / file input."),12000);
        return;
    }
    if (inputSource_ && inputSource_->currentIndex() == 1) {
        if (sampleRateHz() <= 0.0 ||
            (sampleRateHz() > 2400000.0 && !(rtlHrit && sampleRateHz() == 3200000.0)))
            sampleRate_->setCurrentText(QStringLiteral("2.400 MS/s"));
        const double rtlRate = sampleRateHz();
        if (decoder_) decoder_->setIngressBackpressureEnabled(true);
        const QString sourceName = QStringLiteral("RTL-SDR USB device %1").arg(rtlDeviceIndex_->value());
        if (decoderThread_ && decoderThread_->isRunning()) {
            QMetaObject::invokeMethod(decoder_.get(), [this, rtlRate, sourceName] {
                decoder_->setSampleRate(rtlRate);
                decoder_->setSourceLabel(sourceName);
            }, Qt::BlockingQueuedConnection);
        } else {
            decoder_->setSampleRate(rtlRate);
            decoder_->setSourceLabel(sourceName);
        }
        // HRIT: tuner bandwidth 0 = automatic (follows the sample rate); the 1 MHz the box would give
        // is far too narrow for a 4.5 MHz signal.
        const double bw = rfBandwidthHz();   // None = 0 = automatic
        if (std::abs(frequencyShiftHz()) >= 0.45 * rtlRate) {
            if (decoder_) decoder_->setIngressBackpressureEnabled(false);
            statusBar()->showMessage(QStringLiteral("The frequency shift must stay within ±%1 kHz at %2 MS/s.")
                .arg(0.45 * rtlRate / 1e3, 0, 'f', 0).arg(rtlRate / 1e6, 0, 'f', 3), 10000);
            return;
        }
        rtlReceiver_->setParameters(freq_->value() * 1e6, rtlRate, bw, gain_->value(),
                                    static_cast<unsigned>(rtlDeviceIndex_->value()), rtlAutoBiasTee_->isChecked());
        rtlReceiver_->setFrequencyShift(frequencyShiftHz());
        rtlReceiver_->start();
        activeInput_ = InputSource::Rtl;
        if (recordBtn_ && recordBtn_->isChecked()) startIqRecording(rtlRate, freq_->value() * 1e6, true);
        rxBtn_->setText("Stop RTL-SDR Receiving");
        updateInputUi();
        return;
    }

    if (inputSource_ && inputSource_->currentIndex() == static_cast<int>(InputSource::HackRf)) {
        // HackRF One (issue #2): 2-20 MS/s; the rates in the box from 2 MS/s up all work.
        const double rate = sampleRateHz();
        if (rate < 2000000.0 || rate > 20000000.0) {
            statusBar()->showMessage(QStringLiteral("HackRF One runs at 2 to 20 MS/s: choose a rate in Sample Rate first."), 8000);
            return;
        }
        const double shift = frequencyShiftHz();
        if (std::abs(shift) >= 0.45 * rate) {
            statusBar()->showMessage(QStringLiteral("The frequency shift must stay within ±%1 kHz at %2 MS/s.")
                .arg(0.45 * rate / 1e3, 0, 'f', 0).arg(rate / 1e6, 0, 'f', 3), 10000);
            return;
        }
        if (decoder_) decoder_->setIngressBackpressureEnabled(true);
        const QString sourceName = QStringLiteral("HackRF One USB device %1").arg(hackrfDeviceIndex_->value());
        if (decoderThread_ && decoderThread_->isRunning()) {
            QMetaObject::invokeMethod(decoder_.get(), [this, rate, sourceName] {
                decoder_->setSampleRate(rate);
                decoder_->setSourceLabel(sourceName);
            }, Qt::BlockingQueuedConnection);
        } else {
            decoder_->setSampleRate(rate);
            decoder_->setSourceLabel(sourceName);
        }
        hackrfReceiver_->setParameters(freq_->value() * 1e6, rate, rfBandwidthHz(), gain_->value(),
                                       static_cast<unsigned>(hackrfDeviceIndex_->value()), hackrfAntennaPower_->isChecked());
        hackrfReceiver_->setFrequencyShift(shift);
        nadirstation::log::write(QStringLiteral("GUI startRx: HackRF starting; freq=%1 Hz, shift=%2 Hz, rate=%3 Hz, gain=%4")
            .arg(freq_->value() * 1e6, 0, 'f', 0).arg(shift, 0, 'f', 0).arg(rate, 0, 'f', 0).arg(gain_->value(), 0, 'f', 1));
        hackrfReceiver_->start();
        activeInput_ = InputSource::HackRf;
        if (recordBtn_ && recordBtn_->isChecked()) startIqRecording(rate, freq_->value() * 1e6, false);
        rxBtn_->setText("Stop HackRF Receiving");
        updateInputUi();
        return;
    }

    // Keep Pluto ingress bounded just like WAV playback. If DSP briefly falls
    // behind, let the receiver thread wait for the decoder instead of allowing
    // an ever-growing queue to turn live demodulation into delayed playback.
    if (decoder_) decoder_->setIngressBackpressureEnabled(true);
    QMetaObject::invokeMethod(decoder_.get(), [this] { decoder_->setSourceLabel(QStringLiteral("PlutoSDR IP 192.168.3.10")); }, Qt::QueuedConnection);
    const double freqHz = freq_->value() * 1e6;
    const double sr = sampleRateHz();
    if (sr <= 0.0) {
        if (decoder_) decoder_->setIngressBackpressureEnabled(false);
        statusBar()->showMessage(QStringLiteral("Choose a sample rate in Sample Rate first."), 8000);
        return;
    }
    const double bw = rfBandwidthHz();   // None = 0 = automatic
    receiver_->setIp(QStringLiteral(NADIRSTATION_DEFAULT_PLUTO_IP));
    if (std::abs(frequencyShiftHz()) >= 0.45 * sr) {
        if (decoder_) decoder_->setIngressBackpressureEnabled(false);
        statusBar()->showMessage(QStringLiteral("The frequency shift must stay within ±%1 kHz at %2 MS/s.")
            .arg(0.45 * sr / 1e3, 0, 'f', 0).arg(sr / 1e6, 0, 'f', 3), 10000);
        return;
    }
    receiver_->setParameters(freqHz, sr, bw, gain_->value());
    receiver_->setFrequencyShift(frequencyShiftHz());
    // Blocking, and before the receiver starts. A queued call returns immediately, so the worker
    // could deliver IQ at a rate the decoder had not yet been told -- and the carrier bank is built
    // from that rate once and never rebuilt, which made a wrong value permanent: five demodulators
    // configured for the wrong rate never lock, so nothing is ever committed, so nothing is ever
    // rebuilt, and the status reads NOSYNC while the samples are perfectly good.
    //
    // The WAV path already did it this way, which is why the same decoder worked from a file.
    if (decoderThread_ && decoderThread_->isRunning()) {
        QMetaObject::invokeMethod(decoder_.get(), [this, sr] {
            decoder_->setSampleRate(sr);
            nadirstation::log::write(QStringLiteral("Live decoder sample rate armed before the receiver "
                                            "started: %1").arg(sr, 0, 'f', 0));
            nadirstation::log::flush();
        }, Qt::BlockingQueuedConnection);
    } else if (decoder_) {
        decoder_->setSampleRate(sr);
    }
    nadirstation::log::write(QStringLiteral("GUI startRx: receiver starting; freq=%1 Hz, shift=%5 Hz, rate=%2 Hz, bw=%3 Hz, gain=%4")
        .arg(freqHz, 0, 'f', 0).arg(sr, 0, 'f', 0).arg(bw, 0, 'f', 0).arg(gain_->value(), 0, 'f', 1).arg(frequencyShiftHz(), 0, 'f', 0));
    nadirstation::log::flush();
    if (!receiver_->start()) {
        decoder_->setIngressBackpressureEnabled(false);
        return;
    }
    activeInput_ = InputSource::Pluto;
    if (recordBtn_ && recordBtn_->isChecked()) startIqRecording(sr, freqHz, false);
    rxBtn_->setText("Stop Receiving");
    updateInputUi();
}

void MainWindow::startIqRecording(double sampleRate, double centreHz, bool rtl) {
    if (!iqRecorder_ || sampleRate <= 0.0) return;
    const QString dir = QDir(outputRootPath_).filePath(QStringLiteral("IQ_Recordings"));
    QString error;
    using Fmt = nadirstation::IqRecorder::Format;
    const QString choice = recordFormat_ ? recordFormat_->currentData().toString() : QStringLiteral("auto");
    Fmt format = rtl ? Fmt::CU8 : Fmt::CS16;
    if (choice == QLatin1String("cs16")) format = Fmt::CS16;
    else if (choice == QLatin1String("cs8")) format = Fmt::CS8;
    else if (choice == QLatin1String("cu8")) format = Fmt::CU8;
    else if (choice == QLatin1String("wav")) format = Fmt::WAV16;
    if (!iqRecorder_->start(dir, sampleRate, centreHz, format, &error)) {
        nadirstation::log::write(QStringLiteral("IQ recording not started: %1").arg(error));
        statusBar()->showMessage(QStringLiteral("IQ recording not started: %1").arg(error), 10000);
        if (recordBtn_) {
            const QSignalBlocker block(recordBtn_);
            recordBtn_->setChecked(false);
        }
    } else {
        recordClock_.start();
        nadirstation::log::write(QStringLiteral("IQ recording started: %1 (%2 S/s, %3 Hz)")
            .arg(iqRecorder_->path()).arg(sampleRate, 0, 'f', 0).arg(centreHz, 0, 'f', 0));
        statusBar()->showMessage(QStringLiteral("Recording IQ: %1").arg(iqRecorder_->path()), 8000);
    }
    refreshRecordButton();
}

void MainWindow::stopIqRecording() {
    if (!iqRecorder_ || (!iqRecorder_->active() && !iqRecorder_->failed())) return;
    iqRecorder_->stop();
    const QString why = iqRecorder_->takeFailure();
    const bool failed = !why.isEmpty();
    if (failed && recordBtn_) {
        const QSignalBlocker block(recordBtn_);
        recordBtn_->setChecked(false);
    }
    const QString summary = QStringLiteral("%1 · %2 MB%3")
        .arg(iqRecorder_->path()).arg(static_cast<double>(iqRecorder_->bytesWritten()) / 1e6, 0, 'f', 1)
        .arg(iqRecorder_->droppedSamples() > 0
                 ?QStringLiteral(" · disk too slow, %1 samples not recorded").arg(iqRecorder_->droppedSamples()) : QString());
    nadirstation::log::write(QStringLiteral("IQ recording %1: %2 %3")
        .arg(failed ? QStringLiteral("FAILED") : QStringLiteral("stopped"), summary, why));
    statusBar()->showMessage(failed ? QStringLiteral("IQ recording aborted (%1): %2").arg(why, summary)
                                    : QStringLiteral("IQ recording saved: %1").arg(summary), 12000);
    refreshRecordButton();
}

void MainWindow::refreshRecordButton() {
    if (!recordBtn_ || !iqRecorder_) return;
    const bool active = iqRecorder_->active();
    if (recordFormat_ && recordFormat_->isEnabled() == active) recordFormat_->setEnabled(!active);
    if (recordLimit_ && recordLimit_->isEnabled() == active) recordLimit_->setEnabled(!active);
    QString text;
    if (active) {
        const qint64 s = recordClock_.isValid() ? recordClock_.elapsed() / 1000 : 0;
        text = QStringLiteral("● REC  %1 MB  %2:%3").arg(static_cast<double>(iqRecorder_->bytesWritten()) / 1e6, 0, 'f', 0)
                   .arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
        if (iqRecorder_->droppedSamples() > 0) text += QStringLiteral("  · samples lost!");
    } else if (recordBtn_->isChecked()) {
        text = QStringLiteral("Record IQ: armed (starts with receiving)");
    } else {
        text = QStringLiteral("Record IQ");
    }
    if (recordBtn_->text() != text) recordBtn_->setText(text);

}

void MainWindow::stopRx() {
    finishRx(false);
}

void MainWindow::finishRx(bool inputFinished) {
    const auto source = std::exchange(activeInput_, InputSource::None);
    if (source == InputSource::None) return;
    // Release backpressure before joining a worker blocked on the decoder's queue.
    if (decoder_) decoder_->setIngressBackpressureEnabled(false);
    if (source == InputSource::File) wavReceiver_->stop();
    if (source == InputSource::Rtl) rtlReceiver_->stop();
    if (source == InputSource::HackRf) hackrfReceiver_->stop();
    if (source == InputSource::Pluto) receiver_->stop();
    stopIqRecording();
    nadirstation::log::write(QStringLiteral("GUI stopRx: %1 receiver stopped")
        .arg(source == InputSource::File ? QStringLiteral("WAV") :
             source == InputSource::Rtl ? QStringLiteral("RTL-SDR") :
             source == InputSource::HackRf ? QStringLiteral("HackRF") : QStringLiteral("Pluto")));
    nadirstation::log::flush();

    const bool quitAfterDecode = source == InputSource::File && inputFinished && diagnosticWavMode_;
    if (decoder_) {
        if (selectedSatelliteName_ == QStringLiteral("GK-2A HRIT")) hritShowOnFinalize_ = true;
        QMetaObject::invokeMethod(decoder_.get(), [this, quitAfterDecode] {
            decoder_->setEnabled(false);
            if (quitAfterDecode) {
                nadirstation::log::write(QStringLiteral("Diagnostic WAV decode finalized; closing runner."));
                QMetaObject::invokeMethod(this, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
            }
        }, Qt::QueuedConnection);
    } else if (quitAfterDecode) {
        QCoreApplication::quit();
    }
    decodeBtn_->setText("Start Decode");
    rxBtn_->setText("Start Receiving");
    markDecodeBadgeStopped(decodeState_);
    updateInputUi();
    if (source == InputSource::File)
        setConnState(inputFinished ? QStringLiteral("WAV: finished") : QStringLiteral("WAV: stopped"), QStringLiteral("idle"));
}

// The satellite a recording belongs to, from the centre frequency in its name (and for ELEKTRO-L,
// from the width of the signal in it). Returns a note for the status bar; empty when nothing changed.
QString MainWindow::selectSatelliteForFile(const QString& path) {
    QString note;
    // Recorders commonly put the centre frequency in the name ("..._1693000000Hz.cs8").
    // When it is GK-2A's or ELEKTRO-L's, that satellite is selected, since the decoder chain
    // (and for ELEKTRO the whole demodulator) depends on it; FY-2G and FY-2H share 1687.5 MHz,
    // so for those the selection is left to the operator.
    static const QRegularExpression freqRx(QStringLiteral("([0-9]{9,10})\\s*Hz|([0-9]{3,4}(?:\\.[0-9]+)?)\\s*MHz"),
                                           QRegularExpression::CaseInsensitiveOption);
    const auto fm = freqRx.match(QFileInfo(path).fileName());
    if (fm.hasMatch()) {
        const double mhz = fm.captured(1).isEmpty() ? fm.captured(2).toDouble() : fm.captured(1).toDouble() / 1e6;
        QString sat;
        if (std::fabs(mhz - 1693.0) < 0.3) sat = QStringLiteral("ELEKTRO-L GGAK");
        else if (std::fabs(mhz - 1691.0) < 0.3) {
            // LRIT and HRIT share 1691 MHz. The width of the signal itself decides when it can be measured: LRIT occupies
            // about 0.3 MHz and HRIT about 1.15 MHz, whatever rate the recording was made at
            // (a 3 MS/s LRIT recording was taken for HRIT by the rate rule, and decoded nothing).
            const QString current = satelliteSelector_ ? satelliteSelector_->currentText() : QString();
            double rate = WavReceiver::rateFromSidecar(path);
            if (rate <= 0.0) rate = WavReceiver::rateFromName(path);
            const double width = WavReceiver::occupiedBandwidthHz(path, rate);
            if (width > 0.0) {
                sat = width > 650e3 ? QStringLiteral("ELEKTRO-L HRIT") : QStringLiteral("ELEKTRO-L LRIT");
                note += QStringLiteral(" · signal %1 kHz wide").arg(width / 1e3, 0, 'f', 0);
                nadirstation::log::write(QStringLiteral("File %1: signal %2 kHz wide, taken as %3")
                    .arg(QFileInfo(path).fileName()).arg(width / 1e3, 0, 'f', 0).arg(sat));
            } else if (rate >= 4.0e6 || current.contains(QStringLiteral("ELEKTRO-L HRIT"))) {
                sat = QStringLiteral("ELEKTRO-L HRIT");
            } else {
                sat = QStringLiteral("ELEKTRO-L LRIT");
            }
        }
        else if (std::fabs(mhz - 1692.14) < 0.3) sat = QStringLiteral("GK-2A");
        else if (std::fabs(mhz - 1701.3) < 0.15) sat = QStringLiteral("MetOp AHRPT");
        else if (std::fabs(mhz - 1700.0) < 0.15) sat = QStringLiteral("METEOR HRPT");
        else if (std::fabs(mhz - 1685.7) < 0.3) sat = QStringLiteral("GOES GVAR");
        else if (std::fabs(mhz - 1676.0) < 0.3) sat = QStringLiteral("GOES SD");
        if (!sat.isEmpty() && satelliteSelector_ && !satelliteSelector_->currentText().contains(sat, Qt::CaseInsensitive)) {
            selectSatelliteByName(sat);
            note += QStringLiteral(" · %1 MHz in the name, satellite set to %2").arg(mhz, 0, 'f', 3).arg(sat);
        }
    }
    return note;
}

void MainWindow::browseWav() {
    const QString path = QFileDialog::getOpenFileName(this, "Select baseband recording", wavPath_ ? wavPath_->text() : QString(),
                                                      "Baseband (*.wav *.cs16 *.cs8 *.cu8 *.s16 *.s8 *.u8 *.c16 *.c8);;"
                                                      "WAV (*.wav);;Raw I/Q int16 (*.cs16 *.s16 *.c16);;"
                                                      "Raw I/Q int8 (*.cs8 *.s8 *.c8);;Raw I/Q uint8 (*.cu8 *.u8);;All files (*.*)");
    if (!path.isEmpty()) {
        wavPath_->setText(path);
        QString note;
        if (WavReceiver::isRawIq(path)) {
            double hz = WavReceiver::rateFromSidecar(path);
            QString from = QStringLiteral("sidecar .txt");
            if (hz <= 0.0) { hz = WavReceiver::rateFromName(path); from = QStringLiteral("file name"); }
            note = hz > 0.0 ? QStringLiteral(" · %1 · %2 MS/s from %3").arg(QString::fromLatin1(WavReceiver::formatName(WavReceiver::formatForPath(path))))
                                  .arg(hz / 1e6, 0, 'f', 3).arg(from)
                            : QStringLiteral(" · %1 · rate not stated, Sample Rate box will be used")
                                  .arg(QString::fromLatin1(WavReceiver::formatName(WavReceiver::formatForPath(path))));
        }
        note += selectSatelliteForFile(path);
        statusBar()->showMessage(QStringLiteral("File selected: %1%2").arg(path, note));
        updateInputUi();
    }
}

void MainWindow::updateInputUi() {
    const bool wav = inputSource_ && inputSource_->currentIndex() == 2;
    const bool rtl = inputSource_ && inputSource_->currentIndex() == 1;
    const bool hackrf = inputSource_ && inputSource_->currentIndex() == static_cast<int>(InputSource::HackRf);
    const bool running = activeInput_ != InputSource::None;
    if (inputSource_) inputSource_->setEnabled(!running);
    if (wavPath_) wavPath_->setEnabled(wav && !running);
    if (wavBrowse_) wavBrowse_->setEnabled(wav && !running);
    if (wavSpeed_) wavSpeed_->setEnabled(wav);
    if (wavRange_) wavRange_->setEnabled(wav);
    if (freq_) freq_->setEnabled(!wav);
    if (gain_) gain_->setEnabled(!wav);
    // For a raw I/Q file the Sample Rate box is the fallback rate, so it stays usable.
    const bool rawFile = wav && wavPath_ && WavReceiver::isRawIq(wavPath_->text().trimmed());
    if (sampleRate_) {
        const bool rateEditable = !wav || (rawFile && !running);
        sampleRate_->setEnabled(rateEditable);
        if (sampleRateManual_) {
            sampleRateManual_->setVisible(sampleRate_->currentData().toInt() == -1);
            sampleRateManual_->setEnabled(rateEditable);
        }
    }
    if (bandwidth_) {
        const bool editable = !wav && !running;
        bandwidth_->setEnabled(editable);
        if (bandwidthManual_) {
            bandwidthManual_->setVisible(bandwidth_->currentData().toInt() == -1);
            bandwidthManual_->setEnabled(editable);
        }
    }
    if (rtlOptions_) rtlOptions_->setVisible(rtl);
    if (rtlDeviceIndex_) rtlDeviceIndex_->setEnabled(rtl && !running);
    if (rtlAutoBiasTee_) rtlAutoBiasTee_->setEnabled(rtl && !running);
    if (hackrfOptions_) hackrfOptions_->setVisible(hackrf);
    if (hackrfDeviceIndex_) hackrfDeviceIndex_->setEnabled(hackrf && !running);
    if (hackrfAntennaPower_) hackrfAntennaPower_->setEnabled(hackrf && !running);
    if (freqShift_) freqShift_->setEnabled(!wav && !running);
    if (recordBtn_) recordBtn_->setEnabled(!wav);
    if (wav && !(wavReceiver_ && wavReceiver_->isRunning())) {
        setConnState("WAV: ready", QStringLiteral("idle"));
    } else if (rtl && !(rtlReceiver_ && rtlReceiver_->isRunning())) {
        setConnState("RTL-SDR: ready", QStringLiteral("warn"));
    } else if (hackrf && !(hackrfReceiver_ && hackrfReceiver_->isRunning())) {
        setConnState("HackRF: ready", QStringLiteral("warn"));
    } else if (!wav && !rtl && !hackrf && !(receiver_ && receiver_->isRunning())) {
        setConnState("Pluto: offline (simulation)", QStringLiteral("warn"));
    }
}

void MainWindow::startDecode() {
    nadirstation::log::write(QStringLiteral("GUI startDecode entered; decoder=%1")
        .arg(decoder_ ? QStringLiteral("present") : QStringLiteral("null")));
    nadirstation::log::flush();
    if (!decoder_) return;
    if (!requireSatellite(QStringLiteral("startDecode"))) return;
    const float alpha = rrcAlpha_ ? rrcAlpha_->currentText().toFloat() : 0.5f;
    const float pll = pllBandwidth_ ? pllBandwidth_->currentText().toFloat() : 0.02f;
    QMetaObject::invokeMethod(decoder_.get(), [this, alpha, pll] {
        decoder_->setDemodParameters(alpha, pll);
        nadirstation::log::write(QStringLiteral("Demod parameters RRC alpha=%1 PLL bandwidth=%2")
            .arg(alpha, 0, 'f', 3).arg(pll, 0, 'f', 3));
        decoder_->setEnabled(true);
    }, Qt::QueuedConnection);
    resetSnrSmoothing();     // a new acquisition: peak and average start over
    if (snrMeter_) snrMeter_->clear();
    decodeBtn_->setText("Stop Decode");
}
void MainWindow::stopDecode() {
    nadirstation::log::write(QStringLiteral("GUI stopDecode entered; decoder=%1")
        .arg(decoder_ ? QStringLiteral("present") : QStringLiteral("null")));
    nadirstation::log::flush();
    if (!decoder_) return;
    // setEnabled(false) drains pending IQ and builds final image products. A
    // blocking queued call here freezes the UI for the entire drain/finalize.
    // HRIT: stopping closes the observation in progress; its JSON is then imported and the image
    // page brought forward (onImageReady).
    if(selectedSatelliteName_==QStringLiteral("GK-2A HRIT"))hritShowOnFinalize_=true;
    QMetaObject::invokeMethod(decoder_.get(), [this] { decoder_->setEnabled(false); }, Qt::QueuedConnection);
    decodeBtn_->setText("Start Decode");
}

void MainWindow::updateMetrics(float snr, float peak, float avg, double sr) {
    // The receiver's signal estimate, which is a different measurement from the decoder's and was
    // being written to the same three labels as it -- the two alternate, so the display sat at 17.5
    // and flashed to 5. The decoder's is the signal-to-noise ratio of the demodulated symbols, over
    // a bandwidth that contains the signal; the receiver's is taken before demodulation over the
    // whole capture, where a 128 kbaud signal inside 2 MHz is a small part of what is measured.
    //
    // The labels name the demodulator's figure, so they belong to it. The receiver's figure is not
    // shown at all: it disagreed with the decoder's and only confused the reading. The status bar keeps
    // the sample rate and the input source.
    Q_UNUSED(snr);
    Q_UNUSED(peak);
    Q_UNUSED(avg);
    // This arrives ten times a second. Written unconditionally it replaced every other status
    // message within 100 ms -- a receiver error, a lost-samples warning, "recording saved" -- so
    // none of them could be read while receiving. A message that is up stays until its own timeout.
    const QString shown = statusBar()->currentMessage();
    if (!shown.isEmpty() && !shown.startsWith(QStringLiteral("RX "))) return;
    const int sourceIndex = inputSource_ ? inputSource_->currentIndex() : 0;
    const QString input = sourceIndex == 1 ? QStringLiteral("RTL-SDR USB")
                        : sourceIndex == 2 ? QStringLiteral("WAV input")
                        : sourceIndex == 3 ? QStringLiteral("HackRF One USB") : QStringLiteral("Pluto 192.168.3.10");
    statusBar()->showMessage(QString("RX %1 MS/s · %2")
        .arg(sr / 1e6, 0, 'f', 3).arg(input));
}
void MainWindow::applySpectrumSettings() {
    if (!fftSize_ || !fftWindow_ || !fftAvgNum_ || !spectrum_) return;
    const std::size_t points=static_cast<std::size_t>(fftSize_->currentText().toUInt());
    const auto window=static_cast<nadirstation::dsp::SpectrumWindow>(fftWindow_->currentIndex());
    spectrum_->setAverageCount(fftAvgNum_->currentText().toInt());
    if(receiver_) receiver_->setSpectrumSettings(points,window);
    if(wavReceiver_) wavReceiver_->setSpectrumSettings(points,window);
    if(rtlReceiver_) rtlReceiver_->setSpectrumSettings(points,window);
    if(hackrfReceiver_) hackrfReceiver_->setSpectrumSettings(points,window);
    statusBar()->showMessage(QStringLiteral("Spectrum: %1 points · %2 · AvgNum %3 (display only)")
        .arg(points).arg(fftWindow_->currentText()).arg(fftAvgNum_->currentText()),3500);
}
void MainWindow::updateStatus(const QString& text) {
    statusBar()->showMessage(text, 6000);
    logEvent(text);
}

void MainWindow::logEvent(const QString& text) {
    if (!eventLog_ || text.trimmed().isEmpty()) return;
    // Periodic progress lines repeat every few seconds and would bury everything else; they stay in
    // the status bar and the runtime log.
    static const char* const periodic[] = {"GK-2A HRIT: RS ok", "GK-2A HRIT: no CADU", "GOES GVAR: sync", "GOES SD: sync", "File playback", "RX ",
                                           "GOES GVAR · ", "GOES SD · ", "GK-2A HRIT · "};
    for (const char* prefix : periodic)
        if (text.startsWith(QLatin1String(prefix))) return;
    eventLog_->appendPlainText(QStringLiteral("%1  %2")
        .arg(QDateTime::currentDateTimeUtc().toString(QStringLiteral("HH:mm:ss")), text.simplified()));
}

void MainWindow::scheduleCaptureWindow(const QString& startTime, const QString& stopTime) {
    const QTime startClock = QTime::fromString(startTime, QStringLiteral("HH:mm"));
    const QTime stopClock = QTime::fromString(stopTime, QStringLiteral("HH:mm"));
    if (!startClock.isValid() || !stopClock.isValid() || startClock == stopClock) {
        nadirstation::log::write(QStringLiteral("scheduled capture rejected: expected different HH:mm start/stop values"));
        return;
    }
    const QDateTime now = QDateTime::currentDateTime();
    QDateTime startAt(now.date(), startClock, now.timeZone());
    QDateTime stopAt(now.date(), stopClock, now.timeZone());
    if (stopClock <= startClock) stopAt = stopAt.addDays(1);
    if (startAt < now && stopAt > now) startAt = now;
    // A window that is already over today means tomorrow's: asked at 23:00 for 01:00-02:00, both
    // times lie behind "today" and the request used to be dropped as past.
    if (stopAt <= now) {
        startAt = startAt.addDays(1);
        stopAt = stopAt.addDays(1);
    }
    // Arming again replaces the earlier window instead of leaving its timers to fire as well.
    if (scheduledStart_) { scheduledStart_->stop(); scheduledStart_->deleteLater(); }
    if (scheduledStop_) { scheduledStop_->stop(); scheduledStop_->deleteLater(); }

    scheduledStart_ = new QTimer(this);
    scheduledStart_->setSingleShot(true);
    connect(scheduledStart_, &QTimer::timeout, this, [this, stopAt] {
        nadirstation::log::write(QStringLiteral("scheduled Pluto capture starting"));
        if (decoder_ && !decoder_->enabled()) startDecode();
        if ((receiver_ && !receiver_->isRunning()) &&
            (!rtlReceiver_ || !rtlReceiver_->isRunning()) &&
            (!hackrfReceiver_ || !hackrfReceiver_->isRunning()) &&
            (!wavReceiver_ || !wavReceiver_->isRunning())) startRx();
        statusBar()->showMessage(QStringLiteral("Scheduled reception in progress; ends automatically at %1").arg(stopAt.toString(QStringLiteral("HH:mm"))));
    });

    scheduledStop_ = new QTimer(this);
    scheduledStop_->setSingleShot(true);
    connect(scheduledStop_, &QTimer::timeout, this, [this] {
        nadirstation::log::write(QStringLiteral("scheduled Pluto capture stopping and finalizing"));
        if ((receiver_ && receiver_->isRunning()) || (rtlReceiver_ && rtlReceiver_->isRunning()) ||
            (hackrfReceiver_ && hackrfReceiver_->isRunning()) ||
            (wavReceiver_ && wavReceiver_->isRunning())) stopRx();
        statusBar()->showMessage(QStringLiteral("Scheduled reception finished; the image is saved and loaded, the current page is unchanged"), 12000);
    });

    const qint64 startDelay = std::max<qint64>(0, now.msecsTo(startAt));
    const qint64 stopDelay = now.msecsTo(stopAt);
    scheduledStart_->start(static_cast<int>(std::min<qint64>(startDelay, std::numeric_limits<int>::max())));
    scheduledStop_->start(static_cast<int>(std::min<qint64>(stopDelay, std::numeric_limits<int>::max())));
    nadirstation::log::write(QStringLiteral("scheduled Pluto capture armed start=%1 stop=%2")
        .arg(startAt.toString(Qt::ISODate)).arg(stopAt.toString(Qt::ISODate)));
    statusBar()->showMessage(QStringLiteral("Pluto reception scheduled: %1–%2")
        .arg(startAt.toString(QStringLiteral("HH:mm")), stopAt.toString(QStringLiteral("HH:mm"))));
}

void MainWindow::regenerateGk2aRegionalProducts() {
    QString observationDir;
    for (const QString& path : currentPaths_) {
        const QFileInfo info(path);
        const QString candidate = info.absolutePath();
        if (info.fileName().startsWith(QStringLiteral("GK2A"), Qt::CaseInsensitive)
            && QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("GK2A_IR_raw.png")))) {
            observationDir = candidate;
            break;
        }
    }
    if (observationDir.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Load an observation that contains the GK-2A IR image first."), 7000);
        return;
    }
    const QString rawPath = QDir(observationDir).filePath(QStringLiteral("GK2A_IR_raw.png"));
    const QString tbbPath = QDir(observationDir).filePath(QStringLiteral("GK2A_IR_0p01K.png"));
    if (!QFileInfo::exists(tbbPath)) {
        statusBar()->showMessage(QStringLiteral("This observation has no brightness-temperature grid; receive a complete image again with the current version, then regenerate."), 9000);
        return;
    }
    QImageReader::setAllocationLimit(512);
    QImage disk(rawPath);
    QImage tbb(tbbPath);
    if (disk.isNull() || tbb.isNull() || disk.size() != tbb.size()) {
        statusBar()->showMessage(QStringLiteral("The IR image or brightness-temperature grid could not be read, or their sizes differ."), 8000);
        return;
    }
    tbb = tbb.convertToFormat(QImage::Format_Grayscale16);
    const nadirstation::gk2a::DiskGeometry geometry = nadirstation::gk2a::measureDisk(disk, 128.2);
    if (!geometry.measured) {
        statusBar()->showMessage(QStringLiteral("Cannot measure the GK-2A Earth disc in the image, so it cannot be reprojected."), 8000);
        return;
    }
    nadirstation::gk2a::Region region;
    region.centreLat = projLat_->value();
    region.centreLon = projLon_->value();
    region.spanDeg = projSpan_->value();
    const QStringList generated = nadirstation::gk2a::renderRegionalProductsFromTbb(
        disk, tbb, geometry, observationDir, QStringLiteral("GK2A"), region);
    if (generated.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("Regional product generation failed; check the log and the input data."), 8000);
        return;
    }

    QStringList paths = currentPaths_;
    for (const QString& generatedPath : generated)
        if (!paths.contains(generatedPath)) paths.append(generatedPath);
    loadImagePaths(paths, currentTimestamp_);
    if (sessions_ && sessions_->currentItem())
        sessions_->currentItem()->setData(Qt::UserRole, paths);
    const QString preferred = QDir(observationDir).filePath(QStringLiteral("GK2A_IR_Region_enhanced.png"));
    const int index = paths.indexOf(preferred);
    if (index >= 0) product_->setCurrentIndex(index);
    statusBar()->showMessage(QStringLiteral("MCIR and enhanced IR images regenerated for %1°N, %2°E / %3°.")
        .arg(projLat_->value(), 0, 'f', 3).arg(projLon_->value(), 0, 'f', 3)
        .arg(projSpan_->value(), 0, 'f', 1), 9000);
}

// Developer aid (--stress-stop): drives the real buttons through start/stop sequences in random
// order and timing, to reproduce faults in the stop paths without anyone at the keyboard. Every step
// is logged before it is taken, so the last line of the log names the step a fault happened in.
void MainWindow::runStopStress(const QString& wavPath, int cycles, const QString& sources) {
    struct State {
        int cycle = 0;
        int cycles = 0;
        QStringList sources;
        QString wav;
    };
    auto st = std::make_shared<State>();
    st->cycles = std::max(1, cycles);
    st->sources = sources.split(',', Qt::SkipEmptyParts);
    if (st->sources.isEmpty()) st->sources = {QStringLiteral("wav")};
    st->wav = wavPath;
    auto rng = std::make_shared<std::mt19937>(12345u);
    auto ms = [rng](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(*rng); };
    auto log = [](const QString& s) { nadirstation::log::write(QStringLiteral("STRESS ") + s); nadirstation::log::flush(); };
    auto receiving = [this] { return activeInput_ != InputSource::None; };

    auto next = std::make_shared<std::function<void()>>();
    *next = [this, st, rng, ms, log, receiving, next]() {
        if (st->cycle >= st->cycles) {
            log(QStringLiteral("done: %1 cycles").arg(st->cycles));
            if (receiving()) rxBtn_->click();
            QTimer::singleShot(3000, this, [] { QCoreApplication::quit(); });
            return;
        }
        const int k = st->cycle++;
        const QString src = st->sources.at(k % st->sources.size());
        if (receiving()) rxBtn_->click();
        if (src == QStringLiteral("wav")) {
            inputSource_->setCurrentIndex(2);
            wavPath_->setText(st->wav);
            wavSpeed_->setCurrentIndex(k % 3 == 2 ? 2 : k % 2);   // real-time, 4x, Max
        } else if (src == QStringLiteral("rtl")) {
            inputSource_->setCurrentIndex(1);
        } else {
            inputSource_->setCurrentIndex(0);
        }
        const int scenario = ms(0, 3);
        log(QStringLiteral("cycle %1/%2 source=%3 scenario=%4: start receiving")
            .arg(k + 1).arg(st->cycles).arg(src).arg(scenario));
        rxBtn_->click();
        if (src != QStringLiteral("wav") && !decoder_->enabled()) {
            log(QStringLiteral("cycle %1: start decode").arg(k + 1));
            decodeBtn_->click();
        }
        const int t1 = ms(800, 6000);
        QTimer::singleShot(t1, this, [this, k, scenario, ms, log, receiving, next] {
            switch (scenario) {
            case 0:   // stop decode, restart it, then stop receiving
                log(QStringLiteral("cycle %1: stop decode").arg(k + 1));
                if (decoder_->enabled()) decodeBtn_->click();
                QTimer::singleShot(ms(200, 3000), this, [this, k, ms, log, receiving, next] {
                    log(QStringLiteral("cycle %1: start decode again").arg(k + 1));
                    if (!decoder_->enabled()) decodeBtn_->click();
                    QTimer::singleShot(ms(500, 3000), this, [this, k, log, receiving, next] {
                        log(QStringLiteral("cycle %1: stop receiving").arg(k + 1));
                        if (receiving()) rxBtn_->click();
                        QTimer::singleShot(1500, this, [next] { (*next)(); });
                    });
                });
                break;
            case 1:   // stop receiving while decoding
                log(QStringLiteral("cycle %1: stop receiving while decoding").arg(k + 1));
                if (receiving()) rxBtn_->click();
                QTimer::singleShot(ms(300, 2000), this, [next] { (*next)(); });
                break;
            case 2:   // stop decode then stop receiving at once
                log(QStringLiteral("cycle %1: stop decode, then stop receiving immediately").arg(k + 1));
                if (decoder_->enabled()) decodeBtn_->click();
                if (receiving()) rxBtn_->click();
                QTimer::singleShot(ms(300, 2000), this, [next] { (*next)(); });
                break;
            default:  // stop receiving, then press start again quickly (restart race)
                log(QStringLiteral("cycle %1: stop receiving and restart quickly").arg(k + 1));
                if (receiving()) rxBtn_->click();
                QTimer::singleShot(ms(50, 400), this, [this, k, ms, log, receiving, next] {
                    log(QStringLiteral("cycle %1: quick restart").arg(k + 1));
                    rxBtn_->click();
                    QTimer::singleShot(ms(500, 2500), this, [this, k, log, receiving, next] {
                        log(QStringLiteral("cycle %1: stop receiving after quick restart").arg(k + 1));
                        if (receiving()) rxBtn_->click();
                        QTimer::singleShot(1500, this, [next] { (*next)(); });
                    });
                });
                break;
            }
        });
    };
    log(QStringLiteral("begin: %1 cycles, sources %2, wav %3").arg(st->cycles).arg(st->sources.join(','), wavPath));
    QTimer::singleShot(1500, this, [next] { (*next)(); });
}

double MainWindow::frequencyShiftHz() const {
    return freqShift_ ? freqShift_->value() * 1e3 : 0.0;
}

double MainWindow::rfBandwidthHz() const {
    if (!bandwidth_) return 0.0;
    if (bandwidth_->currentData().toInt() == -1)   // "Manual": the value typed next to the box, in MHz
        return bandwidthManual_ ? bandwidthManual_->value() * 1e6 : 0.0;
    return bandwidth_->currentData().toDouble();   // 0 = None
}

double MainWindow::sampleRateHz() const {
    if (!sampleRate_) return 0.0;
    if (sampleRate_->currentData().toInt() == -1)   // "Manual": the value typed next to the box, in MS/s
        return sampleRateManual_ ? std::round(sampleRateManual_->value() * 1e6) : 0.0;
    return std::round(sampleRate_->currentText().section(QLatin1Char(' '), 0, 0).toDouble() * 1e6);   // "2.500 MS/s"
}
