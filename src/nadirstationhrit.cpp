#include "nadirstationdecoder.h"
#include "runtime_logger.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <algorithm>

void NadirStationDecoder::publishHritPreview() {
    if(!hritImages_)return;
    auto update=hritImages_->takeUpdate();
    if(!update.message.isEmpty()) {
        emit statusMessage(update.message);
        nadirstation::log::write(update.message);
    }
    if(!update.live.isNull())
        emit hritLivePreviewReady(update.stamp,update.live,update.message);
    if(!update.path.isEmpty())
        emit imagePreviewReady(update.stamp,update.paths.isEmpty()?QList<QString>{update.path}:update.paths);
    // Finished observations (full disk, a new observation starting, or reception stopped) come
    // through their own queue so the latest-only preview cannot overwrite them. Each one has its
    // five channel images, L2 products and GK2A_Observation.json on disk; the image page imports it.
    for(auto& done:hritImages_->takeCompleted()) {
        if(!done.message.isEmpty()){emit statusMessage(done.message);nadirstation::log::write(done.message);}
        if(!done.paths.isEmpty())emit imageReady(done.stamp,done.paths);
    }
}
void NadirStationDecoder::finalizeHritObservation() {
    if(!hritImages_)return;
    // Close files still open at the end of reception (lost closing packet): the missing tail is
    // zero-filled and the segment is decoded as far as it goes instead of being dropped.
    for(auto& file:hritAssembler_.flush()){
        ++hritFilesCompleted_;
        hritImages_->submit(std::move(file),outputRoot_,gk2aKeyPath_);
    }
    hritImages_->drain();
    hritImages_->requestFinalize();
    hritImages_->drain();
    publishHritPreview();
}
void NadirStationDecoder::handleHritFrame(nadirstation::gk2a::Cadu&& frame) {
    if(!frame.rsOk)return;
    if(!hritImages_)hritImages_=std::make_unique<nadirstation::gk2a::HritImages>();
    for(auto& file:hritAssembler_.feed(frame.bytes)){
        ++hritFilesCompleted_;
        hritImages_->submit(std::move(file),outputRoot_,gk2aKeyPath_);
    }
}
void NadirStationDecoder::processHritSamples(const std::vector<std::complex<float>>& iq) {
    // The signal occupies 4.5 MHz (3 Mbaud, roll-off 0.5), so 4.5 MS/s takes all of it and 6 MS/s
    // (2 samples a symbol) is the comfortable rate. Below that the roll-off is cut off or folded
    // back: in simulation the polyphase demodulator still locks and tracks down to about 1.05 samples
    // a symbol (RTL-SDR at 3.2 MS/s), about 2-3 dB less sensitive than at 6 MS/s. It is allowed as an
    // experiment, with a notice; anything below 1.05 samples a symbol is refused.
    if(sampleRate_<1.05*symbolRate_){
        if(!lowRateReported_){lowRateReported_=true;emit statusMessage(QStringLiteral("GK-2A HRIT needs IQ at %1 MS/s or more (3 Msym/s); use 3.2 MS/s or more.").arg(1.05*symbolRate_/1e6,0,'f',2));}
        return;
    }
    if(sampleRate_<4500000.0 && !lowRateReported_){
        lowRateReported_=true;
        emit statusMessage(QStringLiteral("GK-2A HRIT: %1 MS/s is below 4.5 MS/s, so the roll-off is cut; experimental demodulation, about 2–3 dB less sensitive, needs a strong signal (Es/N0 above about 6 dB).").arg(sampleRate_/1e6,0,'f',2));
    }
    hritIqSamples_+=iq.size();
    if(!hritImages_)hritImages_=std::make_unique<nadirstation::gk2a::HritImages>();
    // Resolve keys locally only. Key bytes are never written to diagnostics or logs.
    if(!QFileInfo::exists(gk2aKeyPath_)) {
        const QStringList candidates{
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("resources/gk2a/EncryptionKeyMessage.bin")),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("EncryptionKeyMessage.bin")),
            QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("../resources/gk2a/EncryptionKeyMessage.bin"))};
        for(const auto& path:candidates)if(QFileInfo::exists(path)){gk2aKeyPath_=path;break;}
    }
    const auto demod=hritDemod_.process(iq);
    carrierOffsetHz_=demod.carrierOffsetHz;
    publishConstellation(demod.symbols);
    hritDecoder_.feed(demod.symbols,[this](nadirstation::gk2a::Cadu&& frame){handleHritFrame(std::move(frame));});
    // SNR the way SatDump reports it: M2M4 moment estimator over every demodulated symbol,
    // exponential averages with alpha = 0.001 (about 1000 symbols), kept across blocks.
    {
        constexpr double alpha=0.001,beta=1.0-alpha;
        double y1=hritSnrY1_,y2=hritSnrY2_;
        for(const auto& z:demod.symbols){
            const double m=double(z.real())*z.real()+double(z.imag())*z.imag();
            y1=alpha*m+beta*y1;
            y2=alpha*m*m+beta*y2;
        }
        hritSnrY1_=std::isfinite(y1)?y1:0.0;
        hritSnrY2_=std::isfinite(y2)?y2:0.0;
    }
    publishHritPreview();
    const qint64 now=QDateTime::currentMSecsSinceEpoch();
    if(lastStatsEmitMs_==0 || now-lastStatsEmitMs_>=200) {
        const auto& stats=hritDecoder_.stats();
        if(stats.cadusWithGoodRs==0 && stats.bitsIn>6000000 &&
           (lastHritNoFrameStatusMs_==0||now-lastHritNoFrameStatusMs_>5000)){
            const QString message=QStringLiteral("GK-2A HRIT: no CADU has passed RS yet; carrier quality %1%, offset %2 Hz; images need valid segments.")
                .arg(hritDemod_.lockMeasure()*100.f,0,'f',1).arg(carrierOffsetHz_,0,'f',0);
            emit statusMessage(message);
            nadirstation::log::write(message);
            lastHritNoFrameStatusMs_=now;
        }
        if(stats.cadusWithGoodRs>0 &&
           (lastHritProgressStatusMs_==0||now-lastHritProgressStatusMs_>5000)){
            const QString message=QStringLiteral("GK-2A HRIT: RS ok %1 / sync %2; packets %3; raw segments %4; assembly buffer %5 MiB")
                .arg(stats.cadusWithGoodRs).arg(stats.cadusWithSync)
                .arg(hritAssembler_.packetsSeen).arg(hritFilesCompleted_)
                .arg(hritAssembler_.bufferedBytes()/1048576.0,0,'f',1);
            emit statusMessage(message);
            nadirstation::log::write(message+QStringLiteral("; carrier quality %1% · offset %2 Hz · IQ queue %3 samples · QPSK mapping %4 · IQ total %5 · symbols %6 · fill frames %7 · VC gaps %8 · CRC fail %9")
                .arg(hritDemod_.lockMeasure()*100.f,0,'f',1).arg(carrierOffsetHz_,0,'f',0)
                .arg(ingressQueuedSamples_.load()).arg(hritDecoder_.mapping())
                .arg(hritIqSamples_).arg(stats.bitsIn).arg(hritAssembler_.fillerCadus)
                .arg(hritAssembler_.frameDiscontinuities).arg(hritAssembler_.crcFailures));
            lastHritProgressStatusMs_=now;
        }
        lock_=hritDecoder_.locked()?100:0;
        const auto count=[](size_t n){return int(std::min<size_t>(n,2147483647));};
        emit decodeStats(lock_,count(stats.cadusWithGoodRs),count(stats.cadusWithSync-stats.cadusWithGoodRs),
                         float(std::clamp(hritDemod_.lockMeasure(),0.f,1.f)),carrierOffsetHz_);
        // M2M4: signal = sqrt(2*M2^2 - M4), noise = M2 - signal; SatDump shows max(0, 10*log10(S/N)).
        {
            const double signal=std::sqrt(std::max(0.0,2.0*hritSnrY1_*hritSnrY1_-hritSnrY2_));
            const double noise=hritSnrY1_-signal;
            const float snr=(signal>0.0 && noise>1e-12)?float(std::max(0.0,10.0*std::log10(signal/noise))):0.0f;
            peakDemodSnr_=std::max(peakDemodSnr_,snr);
            emit demodSnrMetricsReady(snr,peakDemodSnr_,snr);
        }
        lastStatsEmitMs_=now;
    }
}
