#include "telemetrypage.h"
#include <QByteArray>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QFrame>
#include <QTabWidget>
#include <QTableWidget>
#include <QHeaderView>
#include <QAbstractItemView>
#include <QPlainTextEdit>
#include <QFileDialog>
#include <QFile>
#include <QMessageBox>
#include <QPushButton>
#include <QDate>
#include <QTime>
#include <QDateTime>
#include <QComboBox>
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

#include "nadirstationdecoder.h"
#include "gvartelemetrywidget.h"

namespace {
class TrendChart final : public QWidget {
public:
    TrendChart(QString title, QString unit, QColor color, QWidget* parent = nullptr)
        : QWidget(parent), title_(std::move(title)), unit_(std::move(unit)), color_(std::move(color)) {
        setMinimumHeight(175); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    }
    void setValues(QVector<int> values, int maxValue) { values_ = std::move(values); maxValue_ = std::max(1, maxValue); update(); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect(), QColor("#232427"));
        p.setPen(QColor("#eceef0")); p.setFont(QFont("Bahnschrift", 10, QFont::DemiBold)); p.drawText(QRect(16, 10, width()-32, 22), title_);
        const QRectF plot(44, 42, width()-58, height()-62);
        p.setPen(QPen(QColor("#35373c"), 1));
        for (int i=0; i<4; ++i) { const qreal y=plot.top()+plot.height()*i/3.0; p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y)); }
        p.setFont(QFont("Bahnschrift", 8)); p.setPen(QColor("#9a9ea5"));
        p.drawText(QRectF(2, plot.top()-7, 38, 14), Qt::AlignRight, QString::number(maxValue_));
        p.drawText(QRectF(2, plot.bottom()-7, 40, 14), Qt::AlignRight, QStringLiteral("0 %1").arg(unit_));
        if (values_.isEmpty()) { p.setPen(QColor("#9a9ea5")); p.drawText(plot, Qt::AlignCenter, QStringLiteral("Waiting for telemetry")); return; }
        if (values_.size() == 1) {
            const qreal y=plot.bottom()-plot.height()*std::clamp(values_[0],0,maxValue_)/maxValue_;
            p.setPen(QPen(color_,2)); p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));
            p.setBrush(color_); p.setPen(Qt::NoPen); p.drawEllipse(QPointF(plot.right(),y),3.5,3.5); return;
        }
        QPainterPath path;
        for (int i=0; i<values_.size(); ++i) {
            const qreal x=plot.left()+plot.width()*i/(values_.size()-1);
            const qreal y=plot.bottom()-plot.height()*std::clamp(values_[i],0,maxValue_)/maxValue_;
            if (!i) path.moveTo(x,y); else path.lineTo(x,y);
        }
        p.setPen(QPen(color_,2)); p.drawPath(path); p.setBrush(color_); p.setPen(Qt::NoPen);
        const QPointF end(plot.right(), plot.bottom()-plot.height()*std::clamp(values_.back(),0,maxValue_)/maxValue_); p.drawEllipse(end,3.5,3.5);
    }
private:
    QString title_, unit_; QColor color_; QVector<int> values_; int maxValue_=100;
};

class LiveLongitudeChart final : public QWidget {
public:
    explicit LiveLongitudeChart(QWidget* parent=nullptr):QWidget(parent){setMinimumHeight(220);}
    void setValues(QVector<double> values){values_=std::move(values);update();}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#232427"));
        p.setPen(QColor("#eceef0"));p.setFont(QFont("Bahnschrift",10,QFont::DemiBold));p.drawText(QRect(14,8,width()-28,22),QStringLiteral("Sub-satellite longitude reported by CRC-checked DOC"));
        const QRectF plot(56,38,width()-72,height()-66);p.setPen(QPen(QColor("#35373c"),1));
        for(int lon=-180;lon<=180;lon+=60){const qreal x=plot.left()+(lon+180.0)/360.0*plot.width();p.drawLine(QPointF(x,plot.top()),QPointF(x,plot.bottom()));p.setPen(QColor("#9a9ea5"));p.setFont(QFont("Bahnschrift",8));p.drawText(QRectF(x-20,plot.bottom()+5,40,14),Qt::AlignHCenter,QString::number(lon));p.setPen(QPen(QColor("#35373c"),1));}
        for(int i=0;i<4;++i){const qreal y=plot.top()+plot.height()*i/3.0;p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));}
        if(values_.isEmpty()){p.setPen(QColor("#9a9ea5"));p.drawText(plot,Qt::AlignCenter,QStringLiteral("Waiting for DOC frames that pass CRC with a valid longitude"));return;}
        QPainterPath line;for(int i=0;i<values_.size();++i){const qreal x=values_.size()==1?plot.center().x():plot.left()+plot.width()*i/(values_.size()-1);const qreal y=plot.bottom()-(values_[i]+180.0)/360.0*plot.height();if(!i)line.moveTo(x,y);else line.lineTo(x,y);}
        p.setPen(QPen(QColor("#7cc6bb"),2));p.drawPath(line);p.setBrush(QColor("#f2b96f"));p.setPen(Qt::NoPen);const double lon=values_.back();const QPointF last(values_.size()==1?plot.center().x():plot.right(),plot.bottom()-(lon+180.0)/360.0*plot.height());p.drawEllipse(last,4,4);
        p.setPen(QColor("#9a9ea5"));p.setFont(QFont("Bahnschrift",8));p.drawText(QRectF(4,plot.top()-6,48,14),Qt::AlignRight,QStringLiteral("180°E"));p.drawText(QRectF(4,plot.center().y()-7,48,14),Qt::AlignRight,QStringLiteral("0°"));p.drawText(QRectF(4,plot.bottom()-7,48,14),Qt::AlignRight,QStringLiteral("180°W"));
        p.drawText(QRectF(plot.left(),plot.bottom()+20,plot.width(),14),Qt::AlignCenter,QStringLiteral("Reception order · older on the left, newest on the right"));
    }
private: QVector<double> values_;
};

class OrbitTrackChart final : public QWidget {
public:
    explicit OrbitTrackChart(QWidget* parent=nullptr):QWidget(parent){setMinimumHeight(240);}
    void setPoints(QVector<QPointF> points){points_=std::move(points);update();}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#232427"));
        p.setPen(QColor("#eceef0"));p.setFont(QFont("Bahnschrift",10,QFont::DemiBold));p.drawText(QRect(14,8,width()-28,22),QStringLiteral("Live DOC orbit prediction: sub-satellite point (lat/lon)"));
        const QRectF plot(52,38,width()-68,height()-68);p.setPen(QPen(QColor("#35373c"),1));
        for(int lon=-180;lon<=180;lon+=60){const qreal x=plot.left()+(lon+180.0)/360.0*plot.width();p.drawLine(QPointF(x,plot.top()),QPointF(x,plot.bottom()));p.setPen(QColor("#9a9ea5"));p.setFont(QFont("Bahnschrift",8));p.drawText(QRectF(x-20,plot.bottom()+4,40,14),Qt::AlignHCenter,QString::number(lon));p.setPen(QPen(QColor("#35373c"),1));}
        for(int lat=-60;lat<=60;lat+=30){const qreal y=plot.bottom()-(lat+90.0)/180.0*plot.height();p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));}
        if(points_.isEmpty()){p.setPen(QColor("#9a9ea5"));p.drawText(plot,Qt::AlignCenter,QStringLiteral("Waiting for all 25 orbit/attitude telemetry groups"));return;}
        QPainterPath path;for(int i=0;i<points_.size();++i){const auto pt=points_[i];const QPointF q(plot.left()+(pt.x()+180.0)/360.0*plot.width(),plot.bottom()-(pt.y()+90.0)/180.0*plot.height());if(!i)path.moveTo(q);else path.lineTo(q);}
        p.setPen(QPen(QColor("#7cc6bb"),2));p.drawPath(path);for(int i=0;i<points_.size();++i){const auto pt=points_[i];const QPointF q(plot.left()+(pt.x()+180.0)/360.0*plot.width(),plot.bottom()-(pt.y()+90.0)/180.0*plot.height());p.setBrush(i==points_.size()-1?QColor("#f2b96f"):QColor("#7cc6bb"));p.setPen(Qt::NoPen);p.drawEllipse(q,4,4);}
        p.setPen(QColor("#9a9ea5"));p.setFont(QFont("Bahnschrift",8));p.drawText(QRectF(4,plot.top()-6,44,14),Qt::AlignRight,QStringLiteral("90°N"));p.drawText(QRectF(4,plot.bottom()-7,44,14),Qt::AlignRight,QStringLiteral("90°S"));
    }
private: QVector<QPointF> points_;
};

class CalibrationChart final : public QWidget {
public:
    explicit CalibrationChart(QWidget* parent=nullptr):QWidget(parent){setMinimumHeight(210);}
    void setValues(QVector<float> v, QString label){values_=std::move(v);label_=std::move(label);update();}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);p.setRenderHint(QPainter::Antialiasing);p.fillRect(rect(),QColor("#232427"));
        p.setPen(QColor("#eceef0"));p.setFont(QFont("Bahnschrift",10,QFont::DemiBold));p.drawText(QRect(14,8,width()-28,22),label_);
        const QRectF plot(54,38,width()-70,height()-64);p.setPen(QPen(QColor("#35373c"),1));
        for(int i=0;i<4;++i){qreal y=plot.top()+plot.height()*i/3.0;p.drawLine(QPointF(plot.left(),y),QPointF(plot.right(),y));}
        if(values_.isEmpty()){p.setPen(QColor("#9a9ea5"));p.drawText(plot,Qt::AlignCenter,QStringLiteral("Calibration source data unavailable"));return;}
        float lo=*std::min_element(values_.cbegin(),values_.cend()),hi=*std::max_element(values_.cbegin(),values_.cend());
        if(!(hi>lo)){hi=lo+1.0f;}
        QPainterPath path;for(int i=0;i<values_.size();++i){qreal x=plot.left()+plot.width()*i/(values_.size()-1.0);qreal y=plot.bottom()-(values_[i]-lo)/(hi-lo)*plot.height();if(!i)path.moveTo(x,y);else path.lineTo(x,y);}
        p.setPen(QPen(QColor("#7cc6bb"),1.7));p.drawPath(path);p.setPen(QColor("#9a9ea5"));p.setFont(QFont("Bahnschrift",8));
        p.drawText(QRectF(3,plot.top()-6,46,14),Qt::AlignRight,QString::number(hi,'f',1));p.drawText(QRectF(3,plot.bottom()-7,46,14),Qt::AlignRight,QString::number(lo,'f',1));
        p.drawText(QRectF(plot.left(),plot.bottom()+5,plot.width(),16),Qt::AlignCenter,QStringLiteral("Digital count (0–1023)"));
    }
private: QVector<float> values_;QString label_;
};

QWidget* makeSourceVisualization(QWidget* parent, QLabel** sourceStatus,
                                 QLabel** geometryStatus, QPlainTextEdit** sourcePayload,
                                 QWidget** longitudeChart, QWidget** orbitTrackChart, QPlainTextEdit** orbitDetails, QLabel** calibrationStatus,
                                 QWidget** calibrationChart, QComboBox** calibrationChannel) {
    auto* page=new QWidget(parent);auto* root=new QVBoxLayout(page);
    root->setContentsMargins(12,12,12,12);root->setSpacing(10);
    auto* liveTabs=new QTabWidget;
    auto* geometry=new QWidget;auto* geoLayout=new QVBoxLayout(geometry);
    *geometryStatus=new QLabel(QStringLiteral("Waiting for CRC-checked DOC identity / longitude fields. Only the sub-satellite longitude the protocol provides is plotted; DOC does not carry latitude, ECEF position or the orbit normal."));
    (*geometryStatus)->setWordWrap(true);(*geometryStatus)->setStyleSheet("color:#d6d9de;background:#232427;padding:10px;");
    auto* longitude=new LiveLongitudeChart;*longitudeChart=longitude;geoLayout->addWidget(*geometryStatus);geoLayout->addWidget(longitude,1);
    auto* orbit=new QWidget;auto* orbitLayout=new QVBoxLayout(orbit);auto* map=new OrbitTrackChart;*orbitTrackChart=map;orbitLayout->addWidget(map,2);
    *orbitDetails=new QPlainTextEdit;(*orbitDetails)->setReadOnly(true);(*orbitDetails)->setMaximumHeight(190);(*orbitDetails)->setStyleSheet("QPlainTextEdit{background:#141517;color:#d6d8db;border:1px solid #35373c;font-family:Consolas,monospace;font-size:11px;}");(*orbitDetails)->setPlaceholderText(QStringLiteral("Shows the orbit elements, ECEF position/velocity and attitude predictions assembled from live DOC sub-commutated groups."));orbitLayout->addWidget(*orbitDetails,1);
    liveTabs->addTab(orbit,QStringLiteral("Live orbit / attitude telemetry"));
    liveTabs->addTab(geometry,QStringLiteral("Live satellite geometry"));
    auto* source=new QWidget;auto* sourceLayout=new QVBoxLayout(source);
    *sourceStatus=new QLabel(QStringLiteral("No DOC source data yet"));(*sourceStatus)->setStyleSheet("color:#9a9ea5;background:#232427;padding:8px;");sourceLayout->addWidget(*sourceStatus);
    *sourcePayload=new QPlainTextEdit;(*sourcePayload)->setReadOnly(true);(*sourcePayload)->setLineWrapMode(QPlainTextEdit::NoWrap);(*sourcePayload)->setPlaceholderText(QStringLiteral("Once a complete DOC sector payload arrives, its received bytes are shown in hexadecimal."));(*sourcePayload)->setStyleSheet("QPlainTextEdit{background:#141517;color:#d6d8db;border:1px solid #35373c;font-family:Consolas,monospace;font-size:11px;}");sourceLayout->addWidget(*sourcePayload,1);
    liveTabs->addTab(source,QStringLiteral("Live DOC source bytes"));
    auto* calibration=new QWidget;auto* calLayout=new QVBoxLayout(calibration);
    *calibrationStatus=new QLabel(QStringLiteral("Waiting for all 25 Calibration-2 groups of this reception to arrive and pass the decoder's validity checks. No local backup or fallback table is shown."));(*calibrationStatus)->setWordWrap(true);(*calibrationStatus)->setStyleSheet("color:#d6d9de;background:#232427;padding:8px;");calLayout->addWidget(*calibrationStatus);
    auto* channel=new QComboBox;channel->addItems({QStringLiteral("VIS"),QStringLiteral("IR1"),QStringLiteral("IR2"),QStringLiteral("IR3"),QStringLiteral("IR4")});*calibrationChannel=channel;calLayout->addWidget(channel);
    auto* chart=new CalibrationChart;*calibrationChart=chart;calLayout->addWidget(chart,1);
    liveTabs->addTab(calibration,QStringLiteral("Live Calibration-2"));
    root->addWidget(liveTabs,1);return page;
}

QLabel* makeValue(const QString& title, QWidget* parent) {
    auto* label = new QLabel(title + "\n—"); label->setMinimumHeight(68);
    label->setAlignment(Qt::AlignLeft|Qt::AlignVCenter); label->setTextFormat(Qt::PlainText);
    label->setWordWrap(true); label->setStyleSheet("QLabel{background:#232427;border:1px solid #35373c;border-radius:2px;padding:10px;color:#eceef0;font-size:14px;font-weight:600;}");
    label->setParent(parent); return label;
}
int bcdByte(const QByteArray& b, int i) {
    if (i<0 || i>=b.size()) return -1;
    const int hi=(static_cast<quint8>(b[i])>>4)&15, lo=static_cast<quint8>(b[i])&15;
    return hi<=9 && lo<=9 ? hi*10+lo : -1;
}
int bcd2(const QByteArray& b, int i) { const int a=bcdByte(b,i), c=bcdByte(b,i+1); return a<0||c<0 ? -1 : a*100+c; }
double r6(const QByteArray& b,int i,int decimals) {
    if(i<0||i+6>b.size())return std::numeric_limits<double>::quiet_NaN();
    quint64 raw=0;for(int k=0;k<6;++k)raw=(raw<<8)|static_cast<quint8>(b[i+k]);
    const bool negative=(raw&0x800000000000ULL)!=0;const quint64 magnitude=raw&0x7fffffffffffULL;
    const double value=static_cast<double>(magnitude)/std::pow(10.0,decimals);
    return negative?-value:value;
}
QString bcdTimestamp(const QByteArray& b,int i) {
    const int yy=bcdByte(b,i),mo=bcdByte(b,i+1),dd=bcdByte(b,i+2),hh=bcdByte(b,i+3),mm=bcdByte(b,i+4),ss=bcdByte(b,i+5);
    if(yy<0||mo<0||dd<0||hh<0||mm<0||ss<0)return QStringLiteral("invalid BCD time");
    const QDateTime dt(QDate(2000+yy,mo,dd),QTime(hh,mm,ss),Qt::UTC);
    return dt.isValid()?dt.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss 'UTC'")):QStringLiteral("invalid BCD time");
}
quint32 u24(const QByteArray& b, int i) { return (quint32(static_cast<quint8>(b[i]))<<16)|(quint32(static_cast<quint8>(b[i+1]))<<8)|quint32(static_cast<quint8>(b[i+2])); }
QString hx(quint32 v, int digits=2) { return QStringLiteral("0x%1").arg(v,digits,16,QLatin1Char('0')).toUpper(); }
QString flagValue(int v) { return v==0xff ? QStringLiteral("ON") : v==0 ? QStringLiteral("OFF") : QStringLiteral("unmapped"); }
QString scanModeValue(int v) {
    switch(v) { case 0x00:return "Normal scan"; case 0x0f:return "Partial / hemispheric"; case 0x01:return "15-minute observation"; case 0xff:return "Single-line scan"; default:return "Unmapped mode"; }
}
QStringList wordNames() {
    QStringList n(126, QStringLiteral("Reserved / spare"));
    n[0]="Scan mode"; n[1]="Scan status bits"; n[2]="Frame flag"; n[3]="Picture flag";
    n[4]="Picture flag set line (BCD high)"; n[5]="Picture flag set line (BCD low)";
    n[6]="Picture flag reset line (BCD high)"; n[7]="Picture flag reset line (BCD low)";
    n[8]="Scan count (BCD high)"; n[9]="Scan count (BCD low)";
    n[10]="West horizon point (MSB nibble)"; n[11]="West horizon point (LSB)";
    n[12]="East horizon point (MSB nibble)"; n[13]="East horizon point (LSB)";
    n[14]="Sync lock / Q-D error"; n[15]="Bit error count (MSB nibble)"; n[16]="Bit error count (LSB)";
    n[17]="Observation year (BCD high)"; n[18]="Observation year (BCD low)";
    n[19]="Observation month"; n[20]="Observation day"; n[21]="Observation hour"; n[22]="Observation minute"; n[23]="Observation second"; n[24]="Observation centisecond";
    n[25]="Calibration table ID (MSB)"; n[26]="Calibration table ID (LSB)";
    n[27]="MANAM revision (MSB)"; n[28]="MANAM revision (LSB)"; n[29]="Data source";
    n[30]="Electrometer 1"; n[31]="Electrometer 2";
    for(int i=32;i<=63;++i) n[i]=QStringLiteral("Spare (%1)").arg(i+1);
    n[64]="Scanner select"; n[65]="Scan count (binary MSB nibble)"; n[66]="Scan count (binary LSB)";
    n[67]="Sensor select mask"; n[68]="Sensor patch"; n[69]="Beta count (MSB)"; n[70]="Beta count"; n[71]="Beta count (LSB)";
    n[72]="Spin period count (MSB)"; n[73]="Spin period count"; n[74]="Spin period count (LSB)";
    n[75]="Scan SYNC detect angle (MSB)"; n[76]="Scan SYNC detect angle"; n[77]="Scan SYNC detect angle (LSB)";
    n[78]="Spacecraft clock count (MSB)"; n[79]="Spacecraft clock count"; n[80]="Spacecraft clock count (LSB)";
    n[81]="Earth pulse angle 1 (MSB)"; n[82]="Earth pulse angle 1"; n[83]="Earth pulse angle 1 (LSB)";
    n[84]="Earth pulse angle 2 (MSB)"; n[85]="Earth pulse angle 2"; n[86]="Earth pulse angle 2 (LSB)";
    n[87]="Resampling mode"; n[88]="DPL status"; n[89]="Satellite ID";
    n[90]="Analog sun pulse angle (MSB)"; n[91]="Analog sun pulse angle"; n[92]="Analog sun pulse angle (LSB)";
    n[93]="DPL tracking error (MSB)"; n[94]="DPL tracking error"; n[95]="DPL tracking error (LSB)";
    n[96]="Scanner expanded mode"; n[97]="Bit / frame sync flags"; n[98]="Navigation update flag";
    for(int i=99;i<=105;++i) n[i]=QStringLiteral("Navigation update time BCD (%1)").arg(i-98);
    n[106]="Ground scan line count (MSB)"; n[107]="Ground scan line count (LSB)";
    n[108]="Focusing criterion (MSB)"; n[109]="Focusing criterion (LSB)";
    for(int i=110;i<126;++i) n[i]=QStringLiteral("Spare (%1)").arg(i+1);
    return n;
}

QStringList interpretations(const QByteArray& b) {
    QStringList out(126, QStringLiteral("Raw byte; reserved by the current interface specification."));
    auto raw=[&](int i){return int(static_cast<quint8>(b[i]));};
    out[0]=QStringLiteral("%1 (%2)").arg(scanModeValue(raw(0)),hx(raw(0)));
    const int s=raw(1);
    out[1]=QStringLiteral("%1; step mode %2; %3")
        .arg((s&0x03)==0x03?QStringLiteral("North→South"):((s&0x0c)==0?QStringLiteral("South→North"):QStringLiteral("direction bits unmapped")))
        .arg((s&0x30)==0x30?QStringLiteral("normal"):((s&0xc0)==0?QStringLiteral("rapid (10 steps/spin)"):QStringLiteral("off / unmapped")))
        .arg(hx(s));
    out[2]=QStringLiteral("%1 (%2)").arg(flagValue(raw(2)),hx(raw(2)));
    out[3]=QStringLiteral("%1 (%2)").arg(flagValue(raw(3)),hx(raw(3)));
    auto bcdDescription=[&](int start, const QString& name, int val) {
        out[start]=val<0?QStringLiteral("Invalid BCD"):QStringLiteral("%1 = %2").arg(name).arg(val);
        if(start+1<out.size()) out[start+1]=QStringLiteral("Continuation of %1").arg(name);
    };
    bcdDescription(4,"Picture flag set line",bcd2(b,4)); bcdDescription(6,"Picture flag reset line",bcd2(b,6)); bcdDescription(8,"Scan count",bcd2(b,8));
    const int west=((raw(10)&0x0f)<<8)|raw(11), east=((raw(12)&0x0f)<<8)|raw(13);
    out[10]=west==0xfff?"FFFF sentinel: horizon unavailable / Q-D error":QStringLiteral("12-bit pixel coordinate = %1 (%2)").arg(west).arg(hx(west,3)); out[11]="Continuation of west horizon point";
    out[12]=east==0xfff?"FFFF sentinel: horizon unavailable / Q-D error":QStringLiteral("12-bit pixel coordinate = %1 (%2)").arg(east).arg(hx(east,3)); out[13]="Continuation of east horizon point";
    out[14]=raw(14)==0?"Normal (00)":(raw(14)==0xff?"Error (FF)":QStringLiteral("Unmapped status (%1)").arg(hx(raw(14))));
    const int errors=((raw(15)&15)<<8)|raw(16); out[15]=errors==0xfff?"FFFF sentinel: line quality bad":QStringLiteral("12-bit line bit-error count = %1").arg(errors); out[16]="Continuation of bit error count";
    const int year=bcd2(b,17), month=bcdByte(b,19), day=bcdByte(b,20), hour=bcdByte(b,21), minute=bcdByte(b,22), second=bcdByte(b,23), centi=bcdByte(b,24);
    if(year>=0&&month>0&&day>0&&hour>=0&&minute>=0&&second>=0&&centi>=0&&QDate(year,month,day).isValid()&&QTime(hour,minute,second,centi*10).isValid()) {
        const QString stamp=QStringLiteral("%1-%2-%3 %4:%5:%6.%7 UTC").arg(year,4,10,QChar('0')).arg(month,2,10,QChar('0')).arg(day,2,10,QChar('0')).arg(hour,2,10,QChar('0')).arg(minute,2,10,QChar('0')).arg(second,2,10,QChar('0')).arg(centi,2,10,QChar('0'));
        out[17]=QStringLiteral("Observation time: %1").arg(stamp);
    } else out[17]="Invalid / unavailable BCD timestamp";
    for(int i=18;i<=24;++i) out[i]="Part of observation timestamp";
    out[25]=QStringLiteral("16-bit calibration table ID = %1").arg((raw(25)<<8)|raw(26)); out[26]="Continuation of calibration table ID";
    out[27]=QStringLiteral("16-bit MANAM revision = %1").arg((raw(27)<<8)|raw(28)); out[28]="Continuation of MANAM revision";
    out[29]=raw(29)==0xff?"Operation data (FF)":(raw(29)==0?"Test data (00)":QStringLiteral("Unmapped (%1)").arg(hx(raw(29))));
    out[30]=QStringLiteral("Raw electrometer reading %1").arg(hx(raw(30))); out[31]=QStringLiteral("Raw electrometer reading %1").arg(hx(raw(31)));
    switch(raw(64)) {case 0xff:out[64]="Mirror 1-A";break;case 0xf0:out[64]="Mirror 1-B";break;case 0x00:out[64]="Mirror 2-A";break;case 0x0f:out[64]="Mirror 2-B";break;default:out[64]=QStringLiteral("Unmapped scanner (%1)").arg(hx(raw(64)));}
    const int scan2=((raw(65)&15)<<8)|raw(66); out[65]=scan2==0xfff?"FFFF sentinel: Q/D error":QStringLiteral("12-bit scan count = %1").arg(scan2); out[66]="Continuation of binary scan count";
    QStringList selected; const QStringList sensors={"IR1","IR2","IR3","IR4","VIS1","VIS2","VIS3","VIS4"};
    for(int bit=0;bit<8;++bit) selected<<QStringLiteral("%1:%2").arg(sensors[bit],(raw(67)&(1<<bit))?QStringLiteral("primary"):QStringLiteral("redundant"));
    out[67]=selected.join(QStringLiteral(" · "));
    const int patch=raw(68);QStringList patchLabels;
    for(int pair=0;pair<4;++pair){const int code=(patch>>(6-pair*2))&3;const QStringList sources={"V1","V2","V3","V4"};const QString slot=QStringLiteral("VIS%1").arg(4-pair);patchLabels<<QStringLiteral("%1←%2 (%3)").arg(slot,sources[code],QString::number(code,2).rightJustified(2,'0'));}
    out[68]=patchLabels.join(QStringLiteral(" · "))+QStringLiteral(" · raw %1").arg(hx(patch));
    const int beta=int(u24(b,69)); out[69]=QStringLiteral("β count = %1 µrad (%2 rad)").arg(beta).arg(beta/1.0e6,0,'f',6); out[70]=out[71]="Continuation of beta count";
    const int spin=int(u24(b,72)); out[72]=QStringLiteral("24-bit reference-clock count = %1 (%2 ms at 20 MHz)").arg(spin).arg(spin/20000.0,0,'f',3); out[73]=out[74]="Continuation of spin period count";
    const QStringList triples={"Scan SYNC detect angle","Spacecraft clock count","Earth pulse angle 1","Earth pulse angle 2"};
    const int starts[]={75,78,81,84};
    for(int j=0;j<4;++j){const quint32 value=u24(b,starts[j]);out[starts[j]]=value==0xffffff?QStringLiteral("FFFFFF sentinel: Q/D error"):QStringLiteral("24-bit count = %1 (%2)").arg(value).arg(hx(value,6));out[starts[j]+1]=out[starts[j]+2]=QStringLiteral("Continuation of %1").arg(triples[j]);}
    const int resample=raw(87); out[87]=QStringLiteral("Nearest=%1 · linear=%2 · cubic=%3 (%4)").arg(bool(resample&0x80)).arg(bool(resample&0x40)).arg(bool(resample&0x20)).arg(hx(resample));
    const int dplStatus=raw(88), dplMode=dplStatus&15;const QStringList dplModes={"unknown","SSD auto","Analog sun pulse auto","Earth pulse auto","SSD manual","Analog sun pulse manual","Earth pulse manual"};
    out[88]=QStringLiteral("Time constant code %1 · %2 · raw %3").arg((dplStatus>>4)&15).arg(dplMode<dplModes.size()?dplModes[dplMode]:QStringLiteral("unmapped tracking mode"),hx(dplStatus));
    out[89]=QStringLiteral("Satellite ID = %1").arg(raw(89));
    const quint32 sun=u24(b,90);out[90]=QStringLiteral("24-bit angle count = %1 (%2)").arg(sun).arg(hx(sun,6));out[91]=out[92]="Continuation of analog sun pulse angle";
    const quint32 dpl=u24(b,93);out[93]=QStringLiteral("24-bit tracking error count = %1 (%2)").arg(dpl).arg(hx(dpl,6));out[94]=out[95]="Continuation of DPL tracking error";
    switch(raw(96)){case 0:out[96]="Normal";break;case 0xf0:out[96]="Northward expansion";break;case 0x0f:out[96]="Southward expansion";break;default:out[96]=QStringLiteral("Unmapped (%1)").arg(hx(raw(96)));}
    out[97]=QStringLiteral("Raw flags %1 · bit0: reception finished normally=%2 · bit1: scan/frame sync locked=%3").arg(hx(raw(97))).arg((raw(97)&1)==0).arg((raw(97)&2)==0);
    switch(raw(98)){case 0:out[98]="Navigation predicted from earlier 24-hour data";break;case 0x0f:out[98]="Navigation predicted from earlier 6-hour data";break;case 0xff:out[98]="Navigation predicted from earlier 1-hour data";break;default:out[98]=QStringLiteral("Unmapped (%1)").arg(hx(raw(98)));}
    bool navOk=true; QString nav; for(int i=99;i<=105;++i){const int v=bcdByte(b,i);if(v<0)navOk=false;nav+=v<0?"??":QStringLiteral("%1").arg(v,2,10,QChar('0'));}
    out[99]=navOk?QStringLiteral("Navigation update time YYYYMMDDhhmmss = %1").arg(nav):"Invalid BCD navigation update time";
    for(int i=100;i<=105;++i)out[i]="Continuation of navigation update time";
    out[106]=QStringLiteral("Ground-station scan line count = %1").arg((raw(106)<<8)|raw(107));out[107]="Continuation of ground scan line count";
    out[108]=QStringLiteral("Focusing criterion = %1").arg((raw(108)<<8)|raw(109));out[109]="Continuation of focusing criterion";
    return out;
}
}

TelemetryPage::TelemetryPage(QWidget* parent) : QWidget(parent) {
    setStyleSheet("TelemetryPage{background:#1c1d20;}");
    // Two satellites, two pages: the original FY-2 telemetry, and GOES GVAR (block 0 documentation block).
    auto* outerLayout=new QVBoxLayout(this); outerLayout->setContentsMargins(0,0,0,0);outerLayout->setSpacing(0);
    auto* outerTabs=new QTabWidget; outerLayout->addWidget(outerTabs);
    auto* fy2Page=new QWidget; outerTabs->addTab(fy2Page,QStringLiteral("FY-2"));
    auto* root=new QVBoxLayout(fy2Page); root->setContentsMargins(22,18,22,18);root->setSpacing(12);
    auto* head=new QHBoxLayout;auto* title=new QLabel("Satellite Telemetry");title->setStyleSheet("font-size:16px;font-weight:600;color:#eceef0;");
    linkState_=new QLabel("Waiting for DOC fields");linkState_->setAlignment(Qt::AlignCenter);linkState_->setStyleSheet("color:#e9bd62;background:#3a3328;border-radius:2px;padding:8px 14px;font-weight:600;");head->addWidget(title);head->addStretch();head->addWidget(linkState_);root->addLayout(head);
    auto* subtitle=new QLabel("DOC status is shown after sector ID/filler checks; satellite identity additionally requires a valid DOC CRC and matching satellite ID + subsatellite longitude across five consecutive frames.");subtitle->setWordWrap(true);subtitle->setStyleSheet("color:#9a9ea5;font-size:12px;");root->addWidget(subtitle);
    auto* tabs=new QTabWidget;
    auto* overview=new QWidget;auto* ov=new QVBoxLayout(overview);ov->setContentsMargins(10,12,10,10);ov->setSpacing(10);
    auto* progress=new QFrame;progress->setObjectName("ScanProgress");progress->setStyleSheet("QFrame#ScanProgress{background:#232427;border:1px solid #35373c;border-radius:2px;}");auto* ph=new QHBoxLayout(progress);ph->setContentsMargins(14,8,14,8);
    progressLabel_=new QLabel("Image scan position   —");progressLabel_->setStyleSheet("color:#eceef0;font-weight:600;font-size:14px;");timeLabel_=new QLabel("Satellite time: —");timeLabel_->setStyleSheet("color:#9a9ea5;");ph->addWidget(progressLabel_);ph->addStretch();ph->addWidget(timeLabel_);ov->addWidget(progress);
    const QStringList names={"Satellite ID","Scan mode","Scan direction / step","Frame flag","Picture flag","Set line","Reset line","Scan count","West horizon","East horizon","Sync lock / Q-D","Bit errors","Calibration table","MANAM revision","Data source","Scanner select"};
    auto* grid=new QGridLayout;grid->setSpacing(8);for(int i=0;i<names.size();++i){values_[i]=makeValue(names[i],overview);grid->addWidget(values_[i],i/4,i%4);}ov->addLayout(grid);
    auto* charts=new QHBoxLayout;charts->setSpacing(10);auto* scan=new TrendChart("Image line position · last 120 DOC blocks","lines",QColor("#7cc6bb"));auto* err=new TrendChart("DOC line bit errors · last 120 DOC blocks","bits",QColor("#f08f7f"));scanChart_=scan;errorChart_=err;charts->addWidget(scan);charts->addWidget(err);ov->addLayout(charts,1);
    auto* tablePage=new QWidget;auto* tl=new QVBoxLayout(tablePage);auto* explanation=new QLabel("Every byte of the 126-byte S/C & CDAS status block is listed below (Table 8, words 1–126). Values are shown as received; interpretations follow the published field definitions.");explanation->setWordWrap(true);explanation->setStyleSheet("color:#9a9ea5;padding:8px;");tl->addWidget(explanation);
    table_=new QTableWidget(126,4);table_->setHorizontalHeaderLabels({"Word","Field","Raw byte","Decoded / meaning"});table_->verticalHeader()->hide();table_->setEditTriggers(QAbstractItemView::NoEditTriggers);table_->setSelectionBehavior(QAbstractItemView::SelectRows);table_->setAlternatingRowColors(true);
    table_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::ResizeToContents);table_->horizontalHeader()->setSectionResizeMode(1,QHeaderView::ResizeToContents);table_->horizontalHeader()->setSectionResizeMode(2,QHeaderView::ResizeToContents);table_->horizontalHeader()->setSectionResizeMode(3,QHeaderView::Stretch);tl->addWidget(table_);
    auto* manamPage=new QWidget;auto* ml=new QVBoxLayout(manamPage);ml->setContentsMargins(12,12,12,12);ml->setSpacing(10);
    auto* manamIntro=new QLabel(QStringLiteral("MANAM is the spacecraft's weekly operations schedule inside the batch-02 DOC file segment. The receiver collects its 25 blocks by sub-commutation number; each block is 410 bytes of 5 ASCII lines, repeated over consecutive scan lines. A complete table needs every block (usually about 200 scan lines)."));manamIntro->setWordWrap(true);manamIntro->setStyleSheet("color:#9a9ea5;background:#232427;border:1px solid #35373c;border-radius:2px;padding:10px;");ml->addWidget(manamIntro);
    auto* mh=new QHBoxLayout;manamProgressLabel_=new QLabel(QStringLiteral("Waiting for MANAM blocks · 0 / 25"));manamProgressLabel_->setStyleSheet("color:#eceef0;font-weight:600;font-size:15px;");auto* exportManam=new QPushButton(QStringLiteral("Save received MANAM…"));mh->addWidget(manamProgressLabel_);mh->addStretch();mh->addWidget(exportManam);ml->addLayout(mh);
    manamBlocks_=new QTableWidget(1,25);manamBlocks_->verticalHeader()->hide();manamBlocks_->horizontalHeader()->hide();manamBlocks_->setFixedHeight(42);manamBlocks_->setEditTriggers(QAbstractItemView::NoEditTriggers);manamBlocks_->setSelectionMode(QAbstractItemView::NoSelection);manamBlocks_->setFocusPolicy(Qt::NoFocus);manamBlocks_->setShowGrid(false);manamBlocks_->setStyleSheet("QTableWidget{background:#1c1d20;border:0;}");for(int i=0;i<25;++i){auto* cell=new QTableWidgetItem(QString::number(i));cell->setTextAlignment(Qt::AlignCenter);cell->setBackground(QColor("#35373c"));cell->setForeground(QColor("#9a9ea5"));manamBlocks_->setItem(0,i,cell);manamBlocks_->setColumnWidth(i,28);}ml->addWidget(manamBlocks_);
    manamText_=new QPlainTextEdit;manamText_->setReadOnly(true);manamText_->setLineWrapMode(QPlainTextEdit::NoWrap);manamText_->setPlaceholderText(QStringLiteral("Waiting for the spacecraft to broadcast the full MANAM table…"));manamText_->setStyleSheet("QPlainTextEdit{background:#141517;color:#d6d8db;border:1px solid #35373c;border-radius:2px;padding:10px;font-family:Consolas,monospace;font-size:12px;selection-background-color:#22384d;}");ml->addWidget(manamText_,1);
    connect(exportManam,&QPushButton::clicked,this,[this]{if(manamText_->toPlainText().isEmpty())return;const QString path=QFileDialog::getSaveFileName(this,QStringLiteral("Save the broadcast MANAM"),QStringLiteral("NadirStation_MANAM.txt"),QStringLiteral("Text (*.txt);;All files (*.*)"));if(path.isEmpty())return;QFile f(path);if(!f.open(QIODevice::WriteOnly|QIODevice::Truncate|QIODevice::Text)){QMessageBox::warning(this,QStringLiteral("Save failed"),f.errorString());return;}f.write(manamText_->toPlainText().toUtf8());});
    tabs->addTab(overview,"Overview");tabs->addTab(tablePage,"All 126 status words");tabs->addTab(manamPage,"MANAM Plan");
    tabs->addTab(makeSourceVisualization(tabs,&sourceStatus_,&geometryStatus_,&sourcePayload_,
        &longitudeChart_,&orbitTrackChart_,&orbitDetails_,&calibrationStatus_,&calibrationChart_,&calibrationChannel_),QStringLiteral("Live FY-2 Data & Geometry"));root->addWidget(tabs,1);
    gvarWidget_=new GvarTelemetryWidget; outerTabs->addTab(gvarWidget_,QStringLiteral("GOES GVAR"));
    connect(calibrationChannel_,qOverload<int>(&QComboBox::currentIndexChanged),this,[this](int channel){
        if(channel<0||channel>=calibrationValues_.size())return;
        const QString title=channel==0?QStringLiteral("Live VIS calibration table"):QStringLiteral("Live IR%1 brightness-temperature table").arg(channel);
        static_cast<CalibrationChart*>(calibrationChart_)->setValues(calibrationValues_[channel],title);
    });
    const auto fieldNames=wordNames();for(int r=0;r<126;++r){table_->setItem(r,0,new QTableWidgetItem(QString::number(r+1)));table_->setItem(r,1,new QTableWidgetItem(fieldNames[r]));table_->setItem(r,2,new QTableWidgetItem("—"));table_->setItem(r,3,new QTableWidgetItem("Waiting for DOC data"));}
}

void TelemetryPage::updateManamProgress(int revision,int groupMask) {
    int receivedGroups=0;for(int i=0;i<25;++i)if((static_cast<quint32>(groupMask)&(quint32(1)<<i))!=0)++receivedGroups;
    manamProgressLabel_->setText(QStringLiteral("MANAM revision %1 · %2 / 25 blocks received%3")
        .arg(revision).arg(receivedGroups).arg(receivedGroups==25?QStringLiteral(" · complete"):QStringLiteral(" · receiving")));
    for(int i=0;i<25;++i){auto* cell=manamBlocks_->item(0,i);const bool got=(static_cast<quint32>(groupMask)&(quint32(1)<<i))!=0;cell->setBackground(got?QColor("#3d7a55"):QColor("#35373c"));cell->setForeground(got?QColor("#eceef0"):QColor("#9a9ea5"));}
}

void TelemetryPage::updateManam(int revision,const QByteArray& scheduleText) {
    updateManamProgress(revision,0x01ffffff);
    QByteArray normalized;normalized.reserve(scheduleText.size());
    for(int i=0;i<scheduleText.size();++i){const auto ch=static_cast<unsigned char>(scheduleText[i]);if(ch=='\r'){if(i+1<scheduleText.size()&&scheduleText[i+1]=='\n')++i;normalized.append('\n');}else if(ch=='\n')normalized.append('\n');else if(ch==0)normalized.append(' ');else normalized.append(static_cast<char>(ch));}
    manamText_->setPlainText(QString::fromLatin1(normalized));
}

void TelemetryPage::resetManam() {
    manamProgressLabel_->setText(QStringLiteral("Waiting for MANAM blocks · 0 / 25"));
    for(int i=0;i<25;++i){auto* cell=manamBlocks_->item(0,i);cell->setBackground(QColor("#35373c"));cell->setForeground(QColor("#9a9ea5"));}
    manamText_->clear();
}

void TelemetryPage::updateTelemetry(const QByteArray& block) {
    if(block.size()<126) return;
    const QByteArray b=block.left(126);
    const auto names=wordNames();
    const auto meanings=interpretations(b);
    auto u8=[&](int i){return int(static_cast<quint8>(b[i]));};auto set2=bcd2(b,4),reset=bcd2(b,6),scan=bcd2(b,8);
    const int imageLine=(set2>=0&&scan>=0)?scan-set2+1:-1;
    const int west=((u8(10)&15)<<8)|u8(11),east=((u8(12)&15)<<8)|u8(13),errors=((u8(15)&15)<<8)|u8(16);
    QString direction=(u8(1)&3)==3?"North→South":"South→North";QString step=(u8(1)&0x30)==0x30?"normal step":((u8(1)&0xc0)==0?"rapid step":"step off / unmapped");
    const QString vals[]={QString::number(u8(89)),scanModeValue(u8(0)),direction+" · "+step,flagValue(u8(2))+" ("+hx(u8(2))+")",flagValue(u8(3))+" ("+hx(u8(3))+")",set2<0?"Invalid BCD":QString::number(set2),reset<0?"Invalid BCD":QString::number(reset),scan<0?"Invalid BCD":QString::number(scan),west==0xfff?"Unavailable (FFFF)":QString::number(west),east==0xfff?"Unavailable (FFFF)":QString::number(east),u8(14)==0?"Normal":(u8(14)==255?"Error":"Unmapped "+hx(u8(14))),errors==0xfff?"Bad line (FFFF)":QString::number(errors),QString::number((u8(25)<<8)|u8(26)),QString::number((u8(27)<<8)|u8(28)),u8(29)==255?"Operational":(u8(29)==0?"Test":"Unmapped "+hx(u8(29))),hx(u8(64))};
    for(int i=0;i<16;++i)values_[i]->setText(names[i]+"\n"+vals[i]);
    const int percent=imageLine>=0?std::clamp(imageLine*100/2291,0,100):-1;
    progressLabel_->setText(imageLine<0?"Image scan position   — (invalid BCD)":QString("Image scan position   %1 / 2291 lines   ·   %2%").arg(imageLine).arg(percent));
    int year=bcd2(b,17),month=bcdByte(b,19),day=bcdByte(b,20),hour=bcdByte(b,21),minute=bcdByte(b,22),second=bcdByte(b,23),cs=bcdByte(b,24);
    const bool validTime=year>=1&&month>=1&&day>=1&&hour>=0&&minute>=0&&second>=0&&cs>=0&&QDate(year,month,day).isValid()&&QTime(hour,minute,second,cs*10).isValid();
    const QString utc=validTime?QString("%1-%2-%3 %4:%5:%6.%7 UTC").arg(year,4,10,QChar('0')).arg(month,2,10,QChar('0')).arg(day,2,10,QChar('0')).arg(hour,2,10,QChar('0')).arg(minute,2,10,QChar('0')).arg(second,2,10,QChar('0')).arg(cs,2,10,QChar('0')):QStringLiteral("Unavailable / invalid BCD");
    timeLabel_->setText("Satellite time: "+utc);
    linkState_->setText("Raw DOC block received (unverified)");linkState_->setStyleSheet("color:#6fd5a1;background:#26342b;border-radius:2px;padding:8px 14px;font-weight:600;");
    if(imageLine>=0){scanHistory_.push_back(std::clamp(imageLine,0,2291));if(scanHistory_.size()>120)scanHistory_.remove(0,scanHistory_.size()-120);static_cast<TrendChart*>(scanChart_)->setValues(scanHistory_,2291);}
    errorHistory_.push_back(errors==0xfff?4095:errors);if(errorHistory_.size()>120)errorHistory_.remove(0,errorHistory_.size()-120);const int maxError=std::max(16,*std::max_element(errorHistory_.cbegin(),errorHistory_.cend()));static_cast<TrendChart*>(errorChart_)->setValues(errorHistory_,maxError);
    for(int i=0;i<126;++i){table_->item(i,2)->setText(hx(u8(i)));table_->item(i,3)->setText(meanings[i]);}
}

void TelemetryPage::updateDocPayload(const QByteArray& payload,bool crcPassed) {
    const QString crc=crcPassed?QStringLiteral("sector CRC ok"):QStringLiteral("sector CRC failed");
    sourceStatus_->setText(QStringLiteral("Live DOC · %1 bytes · %2 · shown at %3 UTC")
        .arg(payload.size()).arg(crc,QDateTime::currentDateTimeUtc().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz"))));
    QString dump;dump.reserve(payload.size()*4);
    for(qsizetype i=0;i<payload.size();++i){if(i%16==0)dump+=QStringLiteral("\n%1  ").arg(i,4,16,QChar('0')).toUpper();dump+=QStringLiteral("%1 ").arg(static_cast<quint8>(payload[i]),2,16,QChar('0')).toUpper();}
    sourcePayload_->setPlainText(QStringLiteral("DOC sector raw payload (from the current reception; hexadecimal, offsets from 0)\n%1\n\nStatus: %2")
        .arg(dump.trimmed(),crc));
}

void TelemetryPage::updateSatelliteGeometry(int satelliteId,double longitudeDeg,int confirmations,bool confirmed) {
    if((satelliteId!=8&&satelliteId!=9)||!std::isfinite(longitudeDeg)||longitudeDeg < -180.0||longitudeDeg > 180.0)return;
    if(confirmed){longitudeHistory_.append(longitudeDeg);if(longitudeHistory_.size()>120)longitudeHistory_.remove(0,longitudeHistory_.size()-120);}
    static_cast<LiveLongitudeChart*>(longitudeChart_)->setValues(longitudeHistory_);
    const QString name=satelliteId==8?QStringLiteral("FY-2G"):satelliteId==9?QStringLiteral("FY-2H"):QStringLiteral("ID %1").arg(satelliteId);
    geometryStatus_->setText(QStringLiteral("Live CRC-checked DOC fields · %1 · reported sub-satellite longitude %2° · consistent frames %3 · identity %4\n"
        "Once the identity is confirmed, the chart plots the last %5 longitude samples in reception order. DOC carries no sub-satellite latitude, altitude / ECEF position or orbit normal, so none is filled in from a local TLE or model.")
        .arg(name).arg(longitudeDeg,0,'f',3).arg(confirmations)
        .arg(confirmed?QStringLiteral("confirmed"):QStringLiteral("unconfirmed"))
        .arg(longitudeHistory_.size()));
}

void TelemetryPage::updateOrbitAttitude(const QByteArray& block) {
    if(block.size()!=3200)return;
    QVector<QPointF> points;
    QString details;
    auto put=[&](const QString& s){details+=s+QLatin1Char('\n');};
    put(QStringLiteral("Orbit elements (R*6.8/R*6.6, from the orbit/attitude block received now)"));
    put(QStringLiteral("Epoch MJD: %1 · semi-major axis: %2 km · eccentricity: %3 · inclination: %4°")
        .arg(r6(block,156,8),0,'f',8).arg(r6(block,162,6),0,'f',6).arg(r6(block,168,8),0,'f',8).arg(r6(block,174,8),0,'f',8));
    put(QStringLiteral("RAAN: %1° · argument of perigee: %2° · mean anomaly: %3° · sub-point: %4°N, %5°E")
        .arg(r6(block,180,8),0,'f',6).arg(r6(block,186,8),0,'f',6).arg(r6(block,192,8),0,'f',6)
        .arg(r6(block,198,8),0,'f',6).arg(r6(block,204,8),0,'f',6));
    put(QStringLiteral("Attitude: epoch MJD %1 · α %2° · δ %3° · spin rate %4")
        .arg(r6(block,210,8),0,'f',8).arg(r6(block,216,8),0,'f',6).arg(r6(block,222,11),0,'f',6).arg(r6(block,240,8),0,'f',8));
    put(QStringLiteral("Attitude prediction (10 rows): time | α | δ | Sun-Earth angle | spin rate | spin-axis RA/Dec"));
    for(int n=0;n<10;++n){const int o=256+n*64;put(QStringLiteral("%1 | %2° | %3° | %4° | %5 | %6° / %7°")
        .arg(bcdTimestamp(block,o+6)).arg(r6(block,o+12,8),0,'f',5).arg(r6(block,o+18,11),0,'f',5)
        .arg(r6(block,o+24,8),0,'f',5).arg(r6(block,o+30,8),0,'f',8).arg(r6(block,o+36,8),0,'f',5).arg(r6(block,o+42,8),0,'f',5));}
    put(QStringLiteral("Orbit prediction (8 rows): time | ECEF X/Y/Z (m) | ECEF VX/VY/VZ | sub-point N/E | altitude (m)"));
    for(int n=0;n<8;++n){const int o=896+n*256;const double lat=r6(block,o+194,8),lon=r6(block,o+200,8),alt=r6(block,o+206,6);
        if(std::isfinite(lat)&&std::isfinite(lon)&&std::isfinite(alt)&&lat>=-90&&lat<=90&&lon>=-180&&lon<=180&&alt>0)points.append(QPointF(lon,lat));
        put(QStringLiteral("%1 | P=[%2, %3, %4] | V=[%5, %6, %7] | %8°N, %9°E | %10 m")
            .arg(bcdTimestamp(block,o+6)).arg(r6(block,o+54,6),0,'f',2).arg(r6(block,o+60,6),0,'f',2).arg(r6(block,o+66,6),0,'f',2)
            .arg(r6(block,o+72,10),0,'f',5).arg(r6(block,o+78,10),0,'f',5).arg(r6(block,o+84,10),0,'f',5)
            .arg(lat,0,'f',6).arg(lon,0,'f',6).arg(alt,0,'f',1));}
    const int attitudeCount=(static_cast<quint8>(block[2962])<<8)|static_cast<quint8>(block[2963]);
    const int orbitCount=(static_cast<quint8>(block[2982])<<8)|static_cast<quint8>(block[2983]);
    put(QStringLiteral("Declared record counts: attitude %1 (specified 10), orbit %2 (specified 8).")
        .arg(attitudeCount).arg(orbitCount));
    put(QStringLiteral("\nComplete orbit/attitude block raw bytes (assembled from live groups, offsets from 0)"));
    for(int i=0;i<block.size();i+=16){
        QString row=QStringLiteral("%1  ").arg(i,4,16,QChar('0')).toUpper();
        for(int j=i;j<std::min(i+16,static_cast<int>(block.size()));++j)row+=QStringLiteral("%1 ").arg(static_cast<quint8>(block[j]),2,16,QChar('0')).toUpper();
        put(row.trimmed());
    }
    static_cast<OrbitTrackChart*>(orbitTrackChart_)->setPoints(points);
    orbitDetails_->setPlainText(details);
    geometryStatus_->setText(QStringLiteral("The complete orbit/attitude telemetry block (3200 bytes, 25 sub-commutated groups) was assembled from live CRC-checked DOC sectors. The positions and predictions below come from the broadcast fields; no local sample or TLE propagation is used. Valid orbit prediction points: %1 / 8.")
        .arg(points.size()));
}

void TelemetryPage::updateCalibration2(const QByteArray& block) {
    std::array<QVector<float>,5> parsed;
    nadirstation::parseCalibration2Tables(block,parsed);
    calibrationValues_.clear();for(const auto& channel:parsed)calibrationValues_.append(channel);
    const int channel=calibrationChannel_->currentIndex();
    const QString title=channel==0?QStringLiteral("Live VIS calibration table"):QStringLiteral("Live IR%1 brightness-temperature table").arg(channel);
    static_cast<CalibrationChart*>(calibrationChart_)->setValues(calibrationValues_[channel],title);
    calibrationStatus_->setText(QStringLiteral("This reception's Calibration-2 is complete (%1 bytes / 25 groups) and passed the decoder's validity checks. The chart shows the values parsed from it, not a local fallback table.")
        .arg(block.size()));
}

void TelemetryPage::resetCalibration2() {
    calibrationValues_.clear();
    static_cast<CalibrationChart*>(calibrationChart_)->setValues({},QStringLiteral("Live Calibration-2"));
    calibrationStatus_->setText(QStringLiteral("Waiting for all 25 Calibration-2 groups of this reception to arrive and pass the decoder's validity checks. No local backup or fallback table is shown."));
}

void TelemetryPage::updateGvarTelemetry(const QByteArray& block0,const QByteArray& consensus,const QVector<int>& votes,const QVector<int>& stats,const QVector<int>& productInfo,const QList<QByteArray>& productPayloads) {
    if(gvarWidget_)gvarWidget_->setTelemetry(block0,consensus,votes,stats,productInfo,productPayloads);
}

void TelemetryPage::resetGvar() {
    if(gvarWidget_)gvarWidget_->clear();
}
