// ELEKTRO-L GGAK (heliogeophysical instrument complex) telemetry downlink.
//
// The frame layout and the instrument channel tables below are facts about the downlink. The
// SKL / SKIF record maps and scalings were reverse engineered by others (see
// THIRD_PARTY_NOTICES.md). The demodulator and the frame synchroniser are this project's own
// design, made from recordings of the signal.
//
//   carrier        1693.0 MHz (ARKTIKA-M N1: 1703.0 MHz)
//   modulation     phase modulation by a 5 kHz square subcarrier carrying the data (bi-phase);
//                  measured modulation index about 90 degrees, so the residual carrier is weak
//   symbol rate    5000 Bd; no convolutional code, no Reed-Solomon, no NRZ-M, no randomiser
//   frame          224 bytes: ASM 1ACFFC1D | id | master ctr (BE16) | channel ctr (BE16) |
//                  time (BE32, s since 2000-01-01 Moscow time) | payload 209 | checksum (BE16)
//   checksum       16-bit sum of bytes 0..221
//   ids            48 = SKL, 32 = SKIF, 119 = filler. Other ids carry instruments whose formats
//                  are not published; they are kept raw.
//
// The SKL / SKIF scalings are reverse engineered and partly guesses ("prob also log"), so the
// raw counts are always kept beside the values.
#pragma once

#include <array>
#include <complex>
#include <cstdint>
#include <string>
#include <vector>

namespace nadirstation::ggak {

constexpr int kFrameBytes = 224;
constexpr std::uint32_t kAsm = 0x1ACFFC1DU;
constexpr double kSymbolRate = 5000.0;
constexpr double kDownlinkHz = 1693.0e6;
constexpr int kIdSkif = 32;
constexpr int kIdSkl = 48;
constexpr int kIdFiller = 119;

struct Frame {
    std::array<std::uint8_t, kFrameBytes> bytes{};
    bool checksumOk = false;
    int id = 0;
    std::uint16_t masterCounter = 0;
    std::uint16_t channelCounter = 0;
    std::uint32_t timeRaw = 0;
    double unixTime = 0.0;     // UTC seconds since 1970
};

Frame parseFrame(const std::uint8_t* bytes);
const char* frameIdName(int id);

// SKL: 14 records of 14 bytes per frame; counts at offsets 3..13 of each record.
constexpr int kSklChannels = 12;
struct SklRecord {
    double unixTime = 0.0;
    std::array<int, 11> raw{};
    std::array<double, kSklChannels> value{};   // channel 11 (index 10) is not transmitted: NaN
};
const char* sklChannelName(int index);
std::vector<SklRecord> parseSkl(const Frame& frame);

// SKIF: 13 records of 15 bytes; byte 3 is a record id, bytes 5..14 ten values whose meaning
// depends on (record id, position).
enum SkifChannel {
    Skif1, Skif2, Skif3,
    SkifE1, SkifE2, SkifE3, SkifE4, SkifE5, SkifE6, SkifE7, SkifE8, SkifE9, SkifE10,
    SkifP1, SkifP2, SkifP3, SkifP4, SkifP5, SkifP6, SkifP7, SkifP8, SkifP9, SkifP10,
    SkifUnknown,
    SkifChannelCount = SkifUnknown
};
struct SkifRecord {
    double unixTime = 0.0;
    int recordId = 0;
    int position = 0;    // 1..10
    SkifChannel channel = SkifUnknown;
    int raw = 0;
    double value = 0.0;
};
const char* skifChannelName(int channel);
bool skifScalingUncertain(int channel);
std::vector<SkifRecord> parseSkif(const Frame& frame);

// Frame synchroniser on hard bits. A marker with at most 2 wrong bits (either polarity) is a
// candidate; it is accepted when the marker one frame later also matches (at most 6 wrong bits)
// or when the frame's own checksum is right. Once locked, frames are cut every 1792 bits and the
// lock is dropped after three consecutive frames whose marker has more than 8 wrong bits.
class Deframer {
public:
    // Feeds hard bits (0/1); appends every complete 224-byte frame (ASM included) to out.
    void push(const std::vector<std::uint8_t>& bits, std::vector<std::array<std::uint8_t, kFrameBytes>>& out);
    void reset();
    int state() const { return state_; }   // 0 searching, 1 candidate, 2 locked
    bool inverted() const { return inverted_; }
private:
    static int markerErrors(const std::vector<std::uint8_t>& b, std::size_t at, bool inverted);
    void cut(std::size_t at, std::array<std::uint8_t, kFrameBytes>& frame) const;
    std::vector<std::uint8_t> bits_;   // pending bits; bits_[0] is the oldest kept
    std::size_t pos_ = 0;              // next position to examine (search) or next frame start (locked)
    bool inverted_ = false;
    int state_ = 0;
    int misses_ = 0;
};

// Demodulator for the GGAK downlink: phase modulation of the carrier by a 5 kHz square
// subcarrier that carries the 5 kBd data (bi-phase), with a residual carrier.
//
// 1. Acquisition. The spectrum of this signal is mirror-symmetric about the carrier (two data
//    lobes at +-5 kHz, odd subcarrier harmonics further out), while the strongest single line is
//    often a data line, not the carrier. The carrier is therefore taken as the frequency about
//    which the averaged spectrum is most symmetric over 1..9 kHz: first coarsely at the input
//    rate (+-150 kHz), then finely after decimation (+-400 Hz, about 3 Hz resolution).
// 2. Mixing and decimation to about 50 kS/s (10 samples per symbol).
// 3. Carrier tracking with a decision-directed loop for phase modulation of any index: the
//    sample y = c + j s m (c, s the carrier and data amplitudes, m = +-1) is compared with its
//    reference c + j s m^, so the loop works whether the residual carrier is strong or nearly
//    suppressed. The estimated modulation index is atan(s / c).
// 4. The data is the quadrature component. Each symbol is integrated against the bi-phase
//    pulse (first half minus second half) using a running sum, so the integration window can
//    sit at any fractional sample; an early-late gate on the integrator output keeps it aligned.
// 5. Eb/N0 from the second and fourth moments of the symbol integrals (Pauluzzi & Beaulieu,
//    IEEE Trans. Commun. 48(10), 2000), which needs no decisions and holds at low SNR.
struct DemodStats {
    double coarseOffsetHz = 0.0;     // carrier offset found by acquisition
    double pllOffsetHz = 0.0;        // further offset tracked by the carrier loop
    double carrierToNoiseDb = 0.0;   // strongest part of the signal above the noise floor
    bool carrierFound = false;
    bool tracking = false;           // acquisition done, loops running
    double snrDb = 0.0;              // Eb/N0 estimate, dB
    double modulationIndexDeg = 0.0; // atan(s / c)
    double symbolsPerSecond = 0.0;   // recovered symbol clock
    // True sample rate / declared sample rate. 1 normally; 0.5 or 2 when the signal only fits at
    // half or twice the rate the source declared (a recording whose header has the wrong rate).
    double rateScale = 1.0;
};

class Demodulator {
public:
    void configure(double sampleRate);
    double sampleRate() const { return inRate_; }
    // Returns soft symbols (positive = bit 1 before polarity resolution), scaled so that a clean
    // symbol is about +-1.
    std::vector<float> process(const std::vector<std::complex<float>>& iq);
    void requestSearch() { mode_ = Mode::Coarse; resetAcquisition(); }
    const DemodStats& stats() const { return stats_; }
    const std::vector<std::complex<float>>& lastSymbols() const { return lastSymbols_; }
private:
    enum class Mode { Coarse, Fine, Track };
    void resetAcquisition();
    void coarseSearch(const std::vector<std::complex<float>>& iq);
    void decimate(const std::vector<std::complex<float>>& iq, std::vector<std::complex<float>>& out);
    void fineSearch(const std::vector<std::complex<float>>& base);
    void track(const std::vector<std::complex<float>>& base, std::vector<float>& soft);
    void setMixer(double offsetHz);
    void configureAt(double trueRate);

    Mode mode_ = Mode::Coarse;
    double inRate_ = 0.0;
    double declaredRate_ = 0.0;  // the rate the caller passes to configure()
    bool rateChecked_ = false;   // the declared rate has been checked against the signal once
    int decim_ = 1;
    double rate_ = 0.0;   // after decimation
    double sps_ = 10.0;   // samples per symbol after decimation
    bool configured_ = false;

    // acquisition
    std::vector<float> accum_;
    int accumBlocks_ = 0;
    std::size_t coarseN_ = 16384;   // carrier-search FFT size, set from the input rate
    std::vector<std::complex<float>> pending_;
    std::vector<std::complex<float>> leftover_;
    std::vector<std::complex<float>> heldFine_;   // decimated samples used by the fine search   // input after the coarse search, for the next stage
    std::vector<std::complex<float>> fineBuf_;

    // mixer and decimator
    double mixOffsetHz_ = 0.0;
    std::complex<double> lo_{1.0, 0.0}, loStep_{1.0, 0.0};
    int loRenorm_ = 0;
    std::vector<float> lpf_;
    std::vector<std::complex<float>> hist_;
    std::size_t histPos_ = 0;
    int decimPhase_ = 0;

    // AGC and carrier loop
    float power_ = 1.0f;
    double phase_ = 0.0, freq_ = 0.0, kp_ = 0.0, ki_ = 0.0;
    double cAmp_ = 0.5, sAmp_ = 0.5;

    // symbol integration and timing
    std::vector<double> csum_;          // running sum of the quadrature samples; csum_[0] = 0
    std::vector<double> csumRe_;        // the same for the in-phase residual (constellation only)
    double tau_ = 0.0;                  // start of the next symbol, in csum_ index units
    double period_ = 10.0;              // symbol period in samples (tracked)
    double absAvg_ = 1.0;
    double onAcc_ = 0.0, offAcc_ = 0.0; // half-symbol ambiguity check
    int ambCount_ = 0;
    // Eb/N0 moments
    double m2_ = 0.0, m4_ = 0.0;
    int moments_ = 0;

    std::vector<std::complex<float>> lastSymbols_;
    DemodStats stats_;
};


// ---------------------------------------------------------------------------------------------
// ELEKTRO-L N4 instruments: record layouts read from received frames (2026-10-18), NOT from any
// documentation. The record lengths, counters and field positions are measured; which instrument
// each frame ID belongs to is an inference and is labelled as such wherever it is shown.
//
//   ID   record  records/frame  layout                                       inferred instrument
//   12   12 B    17             0x84, counter, multiplexed bytes             solar UV / X-ray sensor?
//   28   12 B    17             0x8C, counter (same as ID 12), mixed data    solar UV / X-ray sensor?
//   44   69 B    3              0x94, BE16 counter, BE16 words, status       particle spectrometer (electrons?)
//   60   69 B    3              0x9C, same counter as ID 44, count words     particle spectrometer (protons?)
//   64   19 B    11             0xA0, BE16 counter (+2), 7 BE16 words         GALS-E cosmic-ray counts?
//   80   104 B   2              0xA8, BE16 counter, 3 x signed 24-bit, deltas FM-E magnetometer?
//   112  15 B    13             7 BE16 words + status, very stable            ISP-2M or housekeeping?
// ---------------------------------------------------------------------------------------------
struct L4Layout {
    int id;
    int recordBytes;
    const char* guess;          // inferred instrument, shown with a question mark
};
const L4Layout* l4Layout(int id);   // nullptr for IDs outside the N4 set

struct L4Record {
    double unixTime = 0.0;      // frame time plus the record's position (one record a second)
    int id = 0;
    int index = 0;              // position in the frame
    int counter = -1;           // record counter where the layout has one
    std::vector<std::uint8_t> bytes;
};
std::vector<L4Record> splitL4Records(const Frame& frame);

// One named quantity of an L4 record, for plots and CSV columns.
struct L4Field {
    int id;
    const char* name;
    int offset;                 // byte offset in the record
    int kind;                   // 0 u8, 1 u16 BE, 3 s24 BE
};
const std::vector<L4Field>& l4Fields();
double l4FieldValue(const L4Record& record, const L4Field& field);   // NaN when not applicable

// Which ELEKTRO-L unit the frames come from, judged from the set of instrument IDs seen. No
// published GGAK header carries a spacecraft identifier, so the fingerprint is all there is:
//   - the N4 set (12, 28, 44, 60, 64, 80, 112), measured on N4 at 165.8 E;
//   - IDs 32 / 48, the published layout, which is not what N4 sends (another unit);
//   - anything else: unknown (N5's set has not been seen).
class UnitIdentifier {
public:
    void note(int id, bool checksumOk);
    void reset() { *this = UnitIdentifier{}; }
    std::string label() const;      // short, e.g. "ELEKTRO-L N4 (165.8E)"
    std::string reason() const;     // what the label rests on
    int confidence() const;         // 0..100
private:
    std::array<std::uint32_t, 256> count_{};
    std::uint32_t total_ = 0;
};

}  // namespace nadirstation::ggak
