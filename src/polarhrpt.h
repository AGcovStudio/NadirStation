#pragma once
// Polar-orbiting HRPT downlinks: MetOp AHRPT and METEOR-M HRPT.
//
//   MetOp AHRPT   1701.3 MHz, QPSK 2.3333 Msym/s, RRC 0.5, CCSDS K=7 r=1/2 (soft-decision Viterbi),
//                 randomised 1024-byte CADUs, RS(255,223) depth 4, AOS VCDU with a 2-byte insert zone.
//                 AVHRR/3 on VCID 9, CCSDS packets APID 103 (3A active) / 104 (3B active), one scan
//                 line per packet: 2048 pixels x 5 channels x 10 bit, CDS time in the secondary header.
//                 The physical and link layers are the ones GK-2A HRIT uses (QpskDemod + HritDecoder);
//                 this file adds the packet layer and the AVHRR line reader.
//
//   METEOR HRPT   1700 MHz, residual-carrier PM, Manchester (bi-phase L) 665.4 kbit/s, no FEC.
//                 1024-byte frames with marker 0x1ACFFC1D (either polarity). MSU-MR occupies bytes
//                 22..259, 278..515, 534..771, 790..1023 of each frame; concatenated, they carry
//                 11850-byte scan lines that start with the 64-bit marker 0x0218A7A392DD9ABF:
//                 6 channels x 1572 pixels x 10 bit, packed four pixels in five bytes, channel c of
//                 group g at byte 50 + 30 g + 5 c; hour/minute/second (Moscow time) at bytes 8..10.
//
// Both layouts were checked against SatDump's output for our MetOp-B (2026-08-21) and
// METEOR-M2-4 (2026-08-27) passes: the AVHRR and MSU-MR channel images agree pixel for pixel.
//
// Soft decisions. MetOp: 8-bit soft symbols into the Viterbi decoder (the GK-2A HRIT path), with
// Reed-Solomon as the arbiter. METEOR: the Manchester matched filter (integrate the first half-bit,
// subtract the second, at the tracked bit clock) gives each bit's soft value; the frame marker is
// found by soft correlation in both polarities and frames are held on a flywheel through a damaged
// marker; the MSU-MR line marker is matched with a tolerance and also flywheeled.
//
// Channel numbering. AVHRR: 1, 2, 3A, 3B, 4, 5. MSU-MR: 1 .. 6. They line up spectrally
// (0.6, 0.8, 1.6, 3.7, 10.8, 12.0 um), so the six image slots are the same for both instruments.

#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>
#include <array>
#include <complex>
#include <cstdint>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace nadirstation::polar {

// ---------------------------------------------------------------------------------------------
// CCSDS packet layer (AOS, M_PDU) for one virtual channel.
struct Packet {
    int apid = 0;
    std::vector<std::uint8_t> bytes;   // primary header (6 bytes) + data field
};

class VcidDemux {
public:
    // dataOffset: first M_PDU data byte within the 1024-byte CADU (ASM included). MetOp: 14.
    explicit VcidDemux(int dataOffset = 14, int dataSize = 882) : off_(dataOffset), size_(dataSize) {}
    // An RS-checked CADU of this virtual channel; returns the packets it completed.
    std::vector<Packet> feed(const std::uint8_t* cadu);
    void reset() { buf_.clear(); synced_ = false; haveCount_ = false; }
    std::size_t gaps() const { return gaps_; }
private:
    int off_, size_;
    std::vector<std::uint8_t> buf_;
    bool synced_ = false;
    bool haveCount_ = false;
    std::uint32_t lastCount_ = 0;
    std::size_t gaps_ = 0;
};

// ---------------------------------------------------------------------------------------------
// The six image slots and the live / final pictures made from them.
class Imager {
public:
    // instrument: "AVHRR" or "MSU-MR"; width 2048 / 1572.
    Imager(QString instrument, int width);
    // One scan line; channels[c] is null when the instrument did not send slot c on this line.
    void addLine(const std::array<const std::uint16_t*, 6>& channels, double unixTime);
    int lines() const;
    // Channel slot names as the user writes them in RGB formulas.
    static QStringList slotNames(const QString& instrument);
    // 8-bit picture of the newest `maxLines` lines (all when 0), half resolution when `half`:
    // a 221 false-colour picture by day, the 10.8 um channel (cold bright) by night.
    QImage preview(int maxLines, bool half, QString* what) const;
    // Writes every channel (16-bit counts x 64, as SatDump does, plus an 8-bit stretched copy) and
    // the composites into `dir`; returns the files written.
    QStringList save(const QString& dir, const QString& stem) const;
    bool slotPresent(int slot) const;
    QDateTime firstTime() const;
    int width() const { return width_; }
    const QString& instrument() const { return instrument_; }
    // Copies for the map products: the time of each line (Unix s, UTC) and one slot's counts,
    // lines x width (0 where the line did not carry the slot).
    std::vector<double> lineTimes() const;
    std::vector<std::uint16_t> slotCounts(int slot) const;
private:
    QImage channel8(int slot, bool invert) const;
    QImage channel16(int slot) const;
    QString instrument_;
    int width_;
    mutable std::mutex m_;
    std::array<std::vector<std::uint16_t>, 6> data_;
    std::array<std::size_t, 6> present_{};
    std::vector<double> times_;
};

// ---------------------------------------------------------------------------------------------
// MetOp AVHRR/3 line reader on top of the VCID 9 demultiplexer.
class AvhrrReader {
public:
    // An RS-checked CADU (any VCID); AVHRR lines go to `imager`.
    void feed(const std::uint8_t* cadu, Imager& imager);
    void reset() { vc9_.reset(); }
    std::size_t lines() const { return lines_; }
    int spacecraftId() const { return scid_; }
private:
    VcidDemux vc9_{14, 882};
    std::size_t lines_ = 0;
    int scid_ = -1;
};

// ---------------------------------------------------------------------------------------------
// METEOR HRPT: residual-carrier PM, Manchester at 665.4 kbit/s, soft bits out.
class MeteorDemod {
public:
    void configure(double sampleRate, double bitRate = 665400.0);
    void reset();
    // Soft bits (positive = 1 before the polarity the deframer settles), scaled so mean |b| = 1.
    void process(const std::vector<std::complex<float>>& iq, std::vector<float>& soft,
                 std::vector<std::complex<float>>* constellation = nullptr);
    double carrierOffsetHz() const { return freq_ * fs_ / 6.283185307179586; }
    double lockMeasure() const { return lock_; }      // 0..1, from the bit-decision SNR
    double snrDb() const { return snrDb_; }
    bool acquired() const { return !acquiring_; }
    int acquisitions() const { return acquisitions_; }
    void setSearchSpanHz(double hz) { searchHz_ = hz; }
private:
    void acquire();
    void runSamples(const std::complex<float>* x, std::size_t n, std::vector<float>& soft,
                    std::vector<std::complex<float>>* constellation);
    double fs_ = 3e6, rb_ = 665400.0, T_ = 4.5;
    double searchHz_ = 150000.0;
    // carrier
    double phase_ = 0.0, freq_ = 0.0, kp_ = 0.0, ki_ = 0.0;
    double amp_ = 0.0;            // carrier amplitude (mean of the in-phase part)
    double pow_ = 0.0;            // input power, for the AGC
    // bit clock: cumulative sum of the detected (quadrature) signal, sample-indexed
    std::vector<double> cum_;     // cum_[i] = integral up to sample base_ + i
    double base_ = 0.0;
    double next_ = 0.0;           // end of the next bit, absolute samples
    double period_ = 4.5;
    double tKp_ = 0.0, tKi_ = 0.0;
    double prevY_ = 0.0;
    double yAvg_ = 1.0, yVar_ = 1.0;
    double lock_ = 0.0, snrDb_ = 0.0;
    double sum_ = 0.0;
    bool acquiring_ = true;
    int acquisitions_ = 0;
    std::vector<std::complex<float>> acqBuf_;
    std::size_t acqLength_ = 1u << 16;
    std::uint64_t sampleIndex_ = 0;
    std::vector<std::complex<float>> pending_;
};

// Frame sync on soft bits: 32-bit marker, 8192-bit frames, both polarities, flywheel.
class MeteorDeframer {
public:
    std::vector<std::vector<std::uint8_t>> feed(const std::vector<float>& soft);
    void reset();
    bool locked() const { return state_ == 2; }
    std::size_t frames() const { return frames_; }
    std::size_t flywheelFrames() const { return flywheel_; }
    std::size_t markerErrorsTotal() const { return markerErrors_; }
private:
    std::deque<float> win_;          // the last bits, newest at the back
    std::vector<float> bits_;        // bits of the frame being collected
    int state_ = 0;                  // 0 search, 1 verify, 2 locked
    bool inverted_ = false;
    int misses_ = 0, good_ = 0;
    long untilMarker_ = 0;           // bits until the next expected marker end
    bool collecting_ = false;
    std::size_t frames_ = 0, flywheel_ = 0, markerErrors_ = 0;
    std::uint32_t reg_ = 0;          // hard bits, newest in bit 0
    void startFrame();
};

// MSU-MR scan lines out of the HRPT frames.
class MsuMrReader {
public:
    void feedFrame(const std::uint8_t* frame, Imager& imager);
    void reset();
    std::size_t lines() const { return lines_; }
    // Serial number of the MSU-MR (line byte 12, high nibble; the most common one of the pass):
    // 0 = METEOR-M N2, 2 = N2-2, 3 = N2-3, 4 = N2-4; -1 before any line.
    int serial() const;
    // Time of the newest line (Unix s, UTC; 0 when it carried none). MTVZA has no clock of its
    // own and takes this one.
    double lastTime() const { return lastTime_; }
private:
    void takeLine(const std::vector<std::uint8_t>& line, Imager& imager);
    std::vector<std::uint8_t> bits_;   // unpacked stream, one bit per byte
    std::size_t pos_ = 0;
    bool locked_ = false;
    int misses_ = 0;
    std::size_t lines_ = 0;
    QDate day_;
    std::array<int, 16> serialVotes_{};
    double lastTime_ = 0.0;
};

// RGB composite from channel expressions such as "ch2,ch2,ch1" or "ch3a, ch2, ch1" or
// "ch4-ch5, ch4, 1-ch5". Each component is evaluated on channels normalised to 0..1; numbers,
// + - * / and parentheses, and the functions inv(x) = 1 - x, abs, min, max, clamp(x,a,b), pow(x,y).
// The channels are looked up in `channels` by name (case-insensitive); all are resampled to the
// largest one's size. When `stretch` is set every component is stretched between its 0.5 and 99.5
// percentiles. A single expression gives a grey picture.
QImage composeRgb(const QString& formula, const std::map<QString, QImage>& channels, bool stretch, QString* error);

}  // namespace nadirstation::polar
