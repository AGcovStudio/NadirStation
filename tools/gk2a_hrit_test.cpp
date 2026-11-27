#include "gk2ahrit.h"
#include "gk2ahritimages.h"
#include "dsp.h"
#include "des.h"
#include <QCoreApplication>
#include <QFile>
#include <QTemporaryDir>
#include <QDir>
#include <QImageReader>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <turbojpeg.h>
#include <openjpeg.h>
#include <random>
#include <chrono>
#include <iostream>
#include <stdexcept>
#include <bit>
#include <algorithm>
using namespace nadirstation::gk2a;
void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
std::vector<std::complex<float>> encode(const std::vector<uint8_t>& bits){
    std::vector<std::complex<float>> out;out.reserve(bits.size());unsigned state=0;
    for(auto b:bits){state=((state<<1)|b)&127;out.emplace_back((std::popcount(state&0x4fu)&1)?1.f:-1.f,(std::popcount(state&0x6du)&1)?1.f:-1.f);}
    return out;
}
void vitTest(){
    std::mt19937 rng(44);std::normal_distribution<float> noise(0,.6f);
    std::vector<uint8_t> bits(300000);for(auto& b:bits)b=rng()&1;
    auto syms=encode(bits);HritViterbi soft,hard;std::vector<uint8_t> result,hardResult;
    for(auto s:syms){s+=std::complex<float>(noise(rng),noise(rng));
        soft.feed(s.real(),s.imag(),[&](uint8_t b){result.push_back(b);});
        hard.feed(s.real()>0?1.f:-1.f,s.imag()>0?1.f:-1.f,[&](uint8_t b){hardResult.push_back(b);});
    }
    size_t errors=0,hardErrors=0;for(size_t i=64;i<result.size();++i){errors+=result[i]!=bits[i];hardErrors+=hardResult[i]!=bits[i];}
    require(result.size()>299700,"Viterbi output stalled");require(errors<100,"soft Viterbi BER too high");
    require(errors<hardErrors,"soft decisions do not improve noisy decoding");
    std::cout<<"Soft errors "<<errors<<", hard errors "<<hardErrors<<" / "<<result.size()<<" bits\n";
}
std::vector<std::complex<float>> frames(){
    std::vector<uint8_t> bits;const auto& pn=LritDecoder::randomiserBits();
    for(int f=0;f<45;++f)for(int i=0;i<1024;++i){
        // All-zero RS codewords are a valid independently known RS codeword.
        const uint8_t byte=i<4?uint8_t(0x1acffc1du>>(24-i*8)):pn[(i-4)%255];
        for(int b=7;b>=0;--b)bits.push_back((byte>>b)&1);
    }
    return encode(bits);
}
void frameTest(){
    auto original=frames();
    for(int map=0;map<8;++map){
        auto syms=original;for(auto& z:syms){float a=z.real(),b=z.imag();if(map&1)std::swap(a,b);if(map&2)a=-a;if(map&4)b=-b;z={a,b};}
        HritDecoder decoder;size_t good=0;
        // Deliberately odd-sized chunks, not a multiple of trellis/frame length.
        for(size_t i=0;i<syms.size();i+=997){auto end=std::min(i+997,syms.size());
            decoder.feed({syms.begin()+i,syms.begin()+end},[&](Cadu&& c){if(c.rsOk){++good;for(size_t k=4;k<c.bytes.size();++k)require(c.bytes[k]==0,"RS payload mismatch");}});
        }
        require(good>=12,"QPSK ambiguity acquisition failed");
        std::cout<<"Mapping "<<map<<": "<<good<<" valid CADUs\n";
    }
}
void threadedFrameTest(){
    const auto symbols=frames();HritDecoder single,parallel;parallel.setAsync(true);
    std::vector<std::vector<uint8_t>> a,b;
    auto acceptA=[&](Cadu&& c){if(c.rsOk)a.push_back(std::move(c.bytes));};
    auto acceptB=[&](Cadu&& c){if(c.rsOk)b.push_back(std::move(c.bytes));};
    for(size_t i=0;i<symbols.size();i+=4093){
        const auto end=std::min(i+4093,symbols.size());
        std::vector<std::complex<float>> block(symbols.begin()+i,symbols.begin()+end);
        single.feed(block,acceptA);parallel.feed(std::move(block),acceptB);
    }
    parallel.drain(acceptB);
    require(!a.empty()&&a==b,"threaded HRIT decoding changed frame contents/order");
    std::cout<<"Threaded demod/link handoff: "<<b.size()<<" frames match synchronous output\n";
}
void fadeHoldTest(){
    HritDecoder decoder;size_t good=0;
    const auto valid=frames();
    decoder.feed(valid,[&](Cadu&& frame){good+=frame.rsOk;});
    require(good>0&&decoder.locked(),"HRIT acquisition before fade");
    const int mapping=decoder.mapping();
    const std::vector<std::complex<float>> fade(100000,{0.f,0.f});
    for(int i=0;i<20;++i)decoder.feed(fade,[](Cadu&&){});
    require(decoder.locked()&&decoder.mapping()==mapping,"short fade changed validated HRIT mapping");
    std::cout<<"HRIT validated mapping held through 0.67 s fade\n";
}
void jpegTest(){
    const int w=11000,h=1100;std::vector<int16_t> pixels(size_t(w)*h);
    for(int y=0;y<h;++y)for(int x=0;x<w;++x)pixels[size_t(y)*w+x]=int16_t(x*1023/(w-1));
    auto enc=tj3Init(TJINIT_COMPRESS);require(enc,"JPEG encoder");
    tj3Set(enc,TJPARAM_PRECISION,10);tj3Set(enc,TJPARAM_LOSSLESS,1);
    unsigned char* buf=nullptr;size_t length=0;
    require(tj3Compress12(enc,pixels.data(),w,w,h,TJPF_GRAY,&buf,&length)==0,"10-bit JPEG fixture encoding");
    std::vector<uint8_t> bytes(buf,buf+length);tj3Free(buf);tj3Destroy(enc);
    QString error;auto preview=HritImages::decodePreview(bytes,w,h,10,error);
    if(preview.isNull())std::cerr<<error.toStdString()<<"\n";
    require(!preview.isNull()&&preview.width()==1375&&preview.height()<=138,"full VIS segment decode failed");
    require(preview.constScanLine(0)[0]<3&&preview.constScanLine(0)[1374]>250,"10-bit grayscale scaling");
    const auto native=HritImages::decodePreview(bytes,w,h,10,error,1,true);
    require(native.size()==QSize(w,h)&&native.format()==QImage::Format_Grayscale16,"native JPEG geometry or precision");
    for(int x:{0,1234,5500,10999}){
        const auto actual=reinterpret_cast<const quint16*>(native.constScanLine(0))[x];
        require(actual==unsigned(pixels[x])*65535u/1023u,"native JPEG counts changed");
    }
    require(HritImages::decodePreview(bytes,65535,65535,10,error).isNull(),"oversize image accepted");
    require(HritImages::decodePreview(bytes,2750,275,10,error).isNull(),"mismatched image accepted");
    bytes.resize(25);require(HritImages::decodePreview(bytes,w,h,10,error).isNull(),"truncated image accepted");
    std::cout<<"VIS 11000x1100 10-bit JPEG -> 1375 preview; malformed geometry rejected\n";
}
void desTest(){
    const uint8_t key[]={0x13,0x34,0x57,0x79,0x9b,0xbc,0xdf,0xf1};
    uint8_t data[]={0x85,0xe8,0x13,0x54,0x0f,0x0a,0xb4,0x05};
    const uint8_t expected[]={1,0x23,0x45,0x67,0x89,0xab,0xcd,0xef};
    nadirstation::crypto::desEcbDecrypt(data,data,8,nadirstation::crypto::desKeySchedule(key));
    require(std::equal(data,data+8,expected),"DES-ECB standard vector/in-place mismatch");
}
void iqTest(){
    const auto symbols=frames();
    const double pi=3.141592653589793,alpha=.35,sps=2.;
    auto pulse=[&](double t){
        if(std::abs(t)<1e-8)return 1.+alpha*(4./pi-1.);
        if(std::abs(std::abs(4*alpha*t)-1)<1e-8)return alpha/std::sqrt(2.)*((1+2/pi)*std::sin(pi/(4*alpha))+(1-2/pi)*std::cos(pi/(4*alpha)));
        return (std::sin(pi*t*(1-alpha))+4*alpha*t*std::cos(pi*t*(1+alpha)))/(pi*t*(1-16*alpha*alpha*t*t));
    };
    std::vector<std::complex<float>> iq(symbols.size()*2);
    std::mt19937 rng(123);std::normal_distribution<float> noise(0,.04f);
    for(size_t n=0;n<iq.size();++n){
        const double t=double(n)/sps-.31;const int middle=int(std::floor(t));
        std::complex<float> z{};
        for(int k=middle-4;k<=middle+4;++k)if(k>=0&&size_t(k)<symbols.size())z+=symbols[k]*float(pulse(t-k));
        z*=std::polar(1.f,float(2*pi*35000*n/6000000.+.8));
        iq[n]=z+std::complex<float>(noise(rng),noise(rng));
    }
    nadirstation::dsp::PskDemod demod;demod.setQpsk(true);demod.configure(6000000,3000000,.35f,.002f);
    HritDecoder decoder;size_t good=0;
    const auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<iq.size();i+=4093){
        auto decoded=demod.process({iq.begin()+i,iq.begin()+std::min(i+4093,iq.size())});
        decoder.feed(decoded.symbols,[&](Cadu&& c){good+=c.rsOk;});
    }
    const double elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"RRC-shaped IQ +35kHz CFO +0.31T timing: "<<good<<" good CADUs; "<<iq.size()/elapsed/1e6<<" MS/s end-to-end\n";
    require(good>=20,"QPSK IQ synchronization failed");
}
void boundedAssemblerTest(){
    LritFileAssembler assembler;assembler.setStrict(true);
    for(int n=0;n<110000;++n){
        std::vector<uint8_t> cadu(1024,0);cadu[0]=0x1a;cadu[1]=0xcf;cadu[2]=0xfc;cadu[3]=0x1d;
        cadu[6]=uint8_t(n>>16);cadu[7]=uint8_t(n>>8);cadu[8]=uint8_t(n);
        const int apid=n%32,seq=n/32;auto* p=cadu.data()+12;
        p[0]=0;p[1]=uint8_t(apid);p[2]=uint8_t((seq==0?0x40:0)|((seq>>8)&63));p[3]=uint8_t(seq);
        p[4]=3;p[5]=109; // 878-byte payload + 6-byte header = packet zone
        if(seq==0){auto* h=p+16;h[2]=16;h[7]=16;h[12]=0x0f;h[13]=0xff;h[14]=0xff;h[15]=0xf8;}
        const auto crc=lritCrc(p+6,876);p[882]=uint8_t(crc>>8);p[883]=uint8_t(crc);
        assembler.feed(cadu);
        require(assembler.bufferedBytes()<=LritFileAssembler::kMaxBufferedBytes,"assembler budget exceeded");
    }
    require(assembler.limitDrops>0,"budget stress failed to hit limit");
    assembler.reset();require(assembler.bufferedBytes()==0,"assembler reset retained buffers");
    std::cout<<"110000 CADUs / 32 unfinished files: 64 MiB budget enforced; reset released buffers\n";
}
void jpeg2000Test(){
    QTemporaryDir dir(QDir::current().filePath("work/j2k-XXXXXX"));require(dir.isValid(),"J2K temp dir");const QString path=dir.filePath(QStringLiteral("fixture.j2k"));
    opj_image_cmptparm_t component{};component.dx=component.dy=1;component.w=128;component.h=32;component.prec=10;
    auto* image=opj_image_create(1,&component,OPJ_CLRSPC_GRAY);require(image,"JPEG2000 fixture image");
    image->x1=128;image->y1=32;
    for(int i=0;i<128*32;++i)image->comps[0].data[i]=(i%128)*8;
    auto* codec=opj_create_compress(OPJ_CODEC_J2K); opj_set_error_handler(codec, [](const char* msg, void*){std::cerr<<msg;}, nullptr);
    opj_cparameters_t params;opj_set_default_encoder_parameters(&params);
    params.tcp_numlayers=1;params.tcp_rates[0]=0;params.cp_disto_alloc=1;params.numresolution=4;
    require(opj_setup_encoder(codec,&params,image),"JPEG2000 encoder setup");
    auto* stream=opj_stream_create_default_file_stream(path.toLocal8Bit().constData(),OPJ_FALSE);
    require(stream,"J2K stream"); require(opj_start_compress(codec,image,stream),"J2K start"); require(opj_encode(codec,stream),"J2K encode"); require(opj_end_compress(codec,stream),"J2K end");
    opj_stream_destroy(stream);opj_destroy_codec(codec);opj_image_destroy(image);
    QFile in(path);require(in.open(QIODevice::ReadOnly),"JPEG2000 fixture read");auto raw=in.readAll();
    std::vector<uint8_t> bytes(reinterpret_cast<const uint8_t*>(raw.data()),reinterpret_cast<const uint8_t*>(raw.data())+raw.size());
    QString error;auto img=HritImages::decodePreview(bytes,128,32,10,error);
    if(img.isNull())std::cerr<<error.toStdString()<<"\n";
    require(!img.isNull()&&img.width()==128,"JPEG2000 preview decode");
    require(img.constScanLine(0)[120]>230,"JPEG2000 pixel scale");
    const auto native=HritImages::decodePreview(bytes,128,32,10,error,1,true);
    require(native.size()==QSize(128,32)&&native.format()==QImage::Format_Grayscale16,"native JPEG2000 geometry");
    require(reinterpret_cast<const quint16*>(native.constScanLine(0))[120]==quint16(960u*65535u/1023u),"native JPEG2000 counts changed");
    bytes[8]=0x7f;require(HritImages::decodePreview(bytes,128,32,10,error).isNull(),"malformed JPEG2000 SIZ accepted");
    std::cout<<"10-bit JPEG2000 decode + SIZ allocation guard passed\n";
}
void benchmark(){
    HritViterbi decoder;size_t n=0;const auto start=std::chrono::steady_clock::now();
    for(size_t i=0;i<3000000;++i)decoder.feed(i&1?1.f:-1.f,i&2?1.f:-1.f,[&](uint8_t){++n;});
    const double seconds=std::chrono::duration<double>(std::chrono::steady_clock::now()-start).count();
    std::cout<<"Viterbi throughput "<<3./seconds<<" Msym/s; fixed object "<<sizeof(decoder)<<" bytes, output "<<n<<"\n";
}
void officialTest(const QString& input,const QString& output){
    HritImages worker;const QDir dir(input);
    const auto names=dir.entryList(QStringList{QStringLiteral("*VI006*.hrit")},QDir::Files,QDir::Name);
    require(names.size()==10,"official VIS sample should have ten segments");
    int n=0;
    for(const auto& name:names){
        QFile in(dir.filePath(name));require(in.open(QIODevice::ReadOnly),"official sample read");
        require(in.size()<=qint64(LritFileAssembler::kMaxFileBytes),"official sample exceeds bound");
        auto raw=in.readAll();LritFile file;
        file.bytes.assign(reinterpret_cast<const uint8_t*>(raw.constData()),reinterpret_cast<const uint8_t*>(raw.constData())+raw.size());
        file.primary=parseLritPrimaryHeader(file.bytes,&file.dataOffset,&file.encryptionKey,&file.encrypted);
        file.fileName=name.toStdString();file.complete=true;raw.clear();raw.squeeze();
        require(worker.submit(std::move(file),output,QString()),"official sample queue");
        worker.drain();auto update=worker.takeUpdate();
        std::cout<<update.message.toStdString()<<"\n";
        require(!update.path.isEmpty(),"official VIS preview missing");
        ++n;require(!update.complete,"premature complete flag: an observation needs all five channels");
    }
    const std::array<QString,4> infrared{QStringLiteral("SW038"),QStringLiteral("WV069"),
        QStringLiteral("IR105"),QStringLiteral("IR123")};
    for(int channel=0;channel<4;++channel)for(const auto& name:names){
        QFile in(dir.filePath(name));require(in.open(QIODevice::ReadOnly),"synthetic channel source read");
        const QByteArray raw=in.readAll();LritFile file;
        std::vector<uint8_t> source(reinterpret_cast<const uint8_t*>(raw.constData()),
                                    reinterpret_cast<const uint8_t*>(raw.constData())+raw.size());
        file.primary=parseLritPrimaryHeader(source,&file.dataOffset,&file.encryptionKey,&file.encrypted);
        require(file.primary.valid&&!file.encrypted,"synthetic channel source header");
        file.bytes.assign(source.begin(),source.begin()+std::ptrdiff_t(file.dataOffset));
        for(size_t pos=16;pos+3<=file.dataOffset;){
            const unsigned len=(unsigned(file.bytes[pos+1])<<8)|file.bytes[pos+2];
            require(len>=3&&len<=file.dataOffset-pos,"synthetic channel TLV");
            if(file.bytes[pos]==1&&len==9){
                file.bytes[pos+4]=0x0a;file.bytes[pos+5]=0xbe; // 2750
                file.bytes[pos+6]=0x01;file.bytes[pos+7]=0x13; // 275
            }
            pos+=len;
        }
        const size_t pixels=2750u*275u;
        file.bytes.resize(file.dataOffset+pixels*2);
        for(size_t y=0;y<275;++y)for(size_t x=0;x<2750;++x){
            const size_t src=file.dataOffset+((y*4)*11000+x*4)*2;
            const unsigned value=source[src]|(unsigned(source[src+1])<<8);
            const unsigned adjusted=value*(75+channel*5)/100;
            const size_t dst=file.dataOffset+(y*2750+x)*2;
            file.bytes[dst]=uint8_t(adjusted);file.bytes[dst+1]=uint8_t(adjusted>>8);
        }
        file.fileName=QString(name).replace(QStringLiteral("VI006"),infrared[channel]).toStdString();
        file.complete=true;
        require(worker.submit(std::move(file),output,QString()),"synthetic channel queue");
        worker.drain();auto update=worker.takeUpdate();
        require(!update.path.isEmpty(),"infrared channel preview missing");
        if(channel==3&&name==names.last()){
            require(update.paths.size()==10,"five channels and five derived products missing");
            for(const auto& path:update.paths)require(QFile::exists(path),"derived product file missing");
        }
    }
    worker.reset();require(worker.queuedBytes()==0,"image worker reset retained data");
    std::cout<<"Official NMSC VIS and four synthetic IR channels: five previews and five derived products validated\n";
}
int main(int argc,char** argv){QCoreApplication app(argc,argv);
    try{
        if(argc==4 && QString::fromLatin1(argv[1])==QStringLiteral("--native-observation")){
            HritImages worker;const QDir source(QString::fromLocal8Bit(argv[2]));
            const QString root=QString::fromLocal8Bit(argv[3]);
            for(const QString name:{QStringLiteral("VI006"),QStringLiteral("SW038"),QStringLiteral("WV069"),QStringLiteral("IR105"),QStringLiteral("IR123")}){
                const QDir channel(source.filePath(name));const auto files=channel.entryList(QStringList{QStringLiteral("*.hrit")},QDir::Files,QDir::Name);
                require(!files.isEmpty(),"missing native test channel");QFile input(channel.filePath(files.first()));require(input.open(QIODevice::ReadOnly),"native source open");
                const auto raw=input.readAll();LritFile file;
                file.bytes.assign(reinterpret_cast<const uint8_t*>(raw.constData()),reinterpret_cast<const uint8_t*>(raw.constData())+raw.size());
                file.primary=parseLritPrimaryHeader(file.bytes,&file.dataOffset,&file.encryptionKey,&file.encrypted);
                file.fileName=files.first().toStdString();file.complete=true;
                require(worker.submit(std::move(file),root,QStringLiteral("build/resources/gk2a/EncryptionKeyMessage.bin")),"native submit");
                worker.drain();worker.takeUpdate();
            }
            worker.requestFinalize();worker.drain();const auto completed=worker.takeCompleted();
            require(!completed.empty(),"native observation not finalized");
            const QDir output(QDir(root).filePath(QStringLiteral("GK-2A/HRIT/")+source.dirName()));
            QFile json(output.filePath(QStringLiteral("GK2A_Observation.json")));require(json.open(QIODevice::ReadOnly),"manifest missing");
            const auto manifest=QJsonDocument::fromJson(json.readAll()).object();const auto channels=manifest.value(QStringLiteral("channels")).toArray();
            require(channels.size()==5,"manifest channels missing");
            for(const auto& item:channels){const auto ch=item.toObject();const QString name=ch.value(QStringLiteral("name")).toString();
                const QString path=output.filePath(ch.value(QStringLiteral("file")).toString());QImageReader reader(path);
                const int expected=name==QStringLiteral("VI006")?11000:2750;
                std::cout<<name.toStdString()<<" "<<reader.size().width()<<"x"<<reader.size().height()<<"\n";
                require(reader.size()==QSize(expected,expected),"native output geometry incorrect");}
            return 0;
        }
        if(argc==4 && QString::fromLatin1(argv[1])==QStringLiteral("--seam")){
            HritImages worker;const QDir source(QString::fromLocal8Bit(argv[2]));
            const QString output=QString::fromLocal8Bit(argv[3]);
            const auto names=source.entryList(QStringList{QStringLiteral("*.hrit")},QDir::Files,QDir::Name);
            require(names.size()>=2,"need adjacent HRIT strips");QString previewPath;
            for(const auto& name:names){QFile input(source.filePath(name));require(input.open(QIODevice::ReadOnly),"source open");
                const QByteArray raw=input.readAll();LritFile file;
                file.bytes.assign(reinterpret_cast<const uint8_t*>(raw.constData()),reinterpret_cast<const uint8_t*>(raw.constData())+raw.size());
                file.primary=parseLritPrimaryHeader(file.bytes,&file.dataOffset,&file.encryptionKey,&file.encrypted);
                file.fileName=name.toStdString();file.complete=true;
                require(worker.submit(std::move(file),output,QStringLiteral("build/resources/gk2a/EncryptionKeyMessage.bin")),"submit");
                worker.drain();const auto update=worker.takeUpdate();require(!update.path.isEmpty(),"strip render");previewPath=update.path;
            }
            const QImage preview(previewPath);require(!preview.isNull(),"preview read");
            for(int boundary:{137,275}){if(boundary>=preview.height())continue;
                auto mean=[&](int y){double s=0;for(int x=0;x<preview.width();++x)s+=preview.constScanLine(y)[x];return s/preview.width();};
                const double above=mean(boundary-2),edge=mean(boundary-1),below=mean(boundary);
                std::cout<<boundary<<" "<<above<<" "<<edge<<" "<<below<<"\n";
                require(above<1 || edge>above*0.75,"dark HRIT seam remains");
            }
            return 0;
        }
        if(argc==3 && QString::fromLatin1(argv[1])==QStringLiteral("--strip")){
            QFile input(QString::fromLocal8Bit(argv[2]));require(input.open(QIODevice::ReadOnly),"strip open failed");
            const QByteArray raw=input.readAll();std::vector<uint8_t> b(reinterpret_cast<const uint8_t*>(raw.constData()),reinterpret_cast<const uint8_t*>(raw.constData())+raw.size());
            size_t off=0;uint32_t keyIndex=0;bool encrypted=false;
            auto primary=parseLritPrimaryHeader(b,&off,&keyIndex,&encrypted);require(primary.valid,"strip header invalid");
            int w=0,h=0,c=0;for(size_t p=16;p+3<=off;){const unsigned len=(unsigned(b[p+1])<<8)|b[p+2];if(len<3||p+len>off)break;
                if(b[p]==1&&len==9){w=(b[p+4]<<8)|b[p+5];h=(b[p+6]<<8)|b[p+7];c=b[p+8];}p+=len;}
            std::vector<uint8_t> payload(b.begin()+std::ptrdiff_t(off),b.end());
            if(encrypted){QFile keyFile(QStringLiteral("build/resources/gk2a/EncryptionKeyMessage.bin"));
                require(keyFile.open(QIODevice::ReadOnly),"key file missing");const auto keys=keyFile.readAll();
                const auto* p=reinterpret_cast<const uint8_t*>(keys.constData());const unsigned count=(p[0]<<8)|p[1];bool found=false;
                for(unsigned i=0;i<count;++i)if(((p[2+10*i]<<8)|p[3+10*i])==keyIndex){
                    auto schedule=nadirstation::crypto::desKeySchedule(p+4+10*i);
                    nadirstation::crypto::desEcbDecrypt(payload.data(),payload.data(),payload.size(),schedule);found=true;break;}
                require(found,"key index missing");}
            QString error;QImage img=HritImages::decodePreview(payload,w,h,10,error,c);
            if(img.isNull())std::cerr<<error.toStdString()<<"\n";
            if(w==2750){size_t marker=0;while(marker+3<payload.size() && !(payload[marker]==0xff&&payload[marker+1]==0xd8&&payload[marker+2]==0xff))++marker;
                auto* jpeg=tj3Init(TJINIT_DECOMPRESS);tj3DecompressHeader(jpeg,payload.data()+marker,payload.size()-marker);
                std::vector<int16_t> native(size_t(w)*h);
                if(tj3Decompress12(jpeg,payload.data()+marker,payload.size()-marker,native.data(),w,TJPF_GRAY)==0){
                    std::cout<<"native rows";for(int y:{h-3,h-2,h-1}){double sum=0;for(int x=0;x<w;++x)sum+=native[size_t(y)*w+x];std::cout<<" "<<y<<":"<<sum/w;}std::cout<<"\n";}
                tj3Destroy(jpeg);}
            require(!img.isNull(),"strip decode failed");
            std::cout<<"size "<<img.width()<<"x"<<img.height()<<" rows";
            for(int y: {0,1,img.height()-3,img.height()-2,img.height()-1}){
                double sum=0;for(int x=0;x<img.width();++x)sum+=img.constScanLine(y)[x];
                std::cout<<" "<<y<<":"<<sum/img.width();
            }
            std::cout<<"\n";return 0;
        }
        if(argc==3 && QString::fromLatin1(argv[1])==QStringLiteral("--iq")){
            QFile input(QString::fromLocal8Bit(argv[2]));require(input.open(QIODevice::ReadOnly),"IQ file open failed");
            nadirstation::dsp::PskDemod demod;demod.setQpsk(true);demod.configure(6000000,3000000,.35f,.002f);
            HritDecoder decoder;LritFileAssembler assembler;assembler.setStrict(true);
            size_t files=0;std::vector<std::complex<float>> iq(65536);
            for(int block=0;;++block){
                const QByteArray raw=input.read(qint64(iq.size()*4));if(raw.isEmpty())break;
                const auto* p=reinterpret_cast<const int16_t*>(raw.constData());
                const size_t n=size_t(raw.size()/4);
                for(size_t i=0;i<n;++i)iq[i]={float(p[2*i])/2048.f,float(p[2*i+1])/2048.f};
                auto result=demod.process({iq.begin(),iq.begin()+std::ptrdiff_t(n)});
                decoder.feed(result.symbols,[&](Cadu&& c){if(c.rsOk)files+=assembler.feed(c.bytes).size();});
                if(block%50==0){const auto& s=decoder.stats();
                    std::cout<<"block "<<block<<" offset "<<result.carrierOffsetHz<<" lock "<<demod.lockMeasure()
                             <<" map "<<decoder.mapping()<<" sync "<<s.cadusWithSync<<" RS "<<s.cadusWithGoodRs<<"\n";
                }
            }
            const auto& s=decoder.stats();std::cout<<"FINAL sync "<<s.cadusWithSync<<" RS "<<s.cadusWithGoodRs
                <<" slips "<<s.pairSlips<<" filler "<<assembler.fillerCadus<<" packets "<<assembler.packetsSeen
                <<" crcFail "<<assembler.crcFailures<<" buffered "<<assembler.bufferedBytes()
                <<" files "<<files<<"\n";return 0;
        }
        if(argc==3){officialTest(QString::fromLocal8Bit(argv[1]),QString::fromLocal8Bit(argv[2]));return 0;}
        vitTest();frameTest();threadedFrameTest();fadeHoldTest();desTest();jpegTest();jpeg2000Test();iqTest();boundedAssemblerTest();benchmark();std::cout<<"HRIT TESTS PASS\n";return 0;}
    catch(const std::exception& e){std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
