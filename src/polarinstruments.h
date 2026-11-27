#pragma once
// The other instruments of the polar HRPT downlinks.
//
// MetOp AHRPT (CCSDS packets, one VcidDemux per virtual channel; payload offsets count from the
// first byte after the 6-byte primary header):
//   MHS      VCID 12, APID 34, 1308-byte packets, one scan per 8/3 s: 90 Earth views x 5 channels
//            (89, 157, 183.3+-1, 183.3+-3, 190.3 GHz), 16-bit counts at data + 49 + 12 i + 2 c.
//   AMSU-A   VCID 3, APID 39 (A1: channels 3..15) / 40 (A2: channels 1, 2), one scan per 8 s, 30
//            views. The 16-bit words from payload byte 14 on carry 0x0001 pad words, which are
//            removed before the scan record is read.
//   IASI IIS VCID 10, APID 150: the integrated imager, 64 x 64 pixels per field (12-bit, packed),
//            36 fields per scan (6 calibration views + 30 Earth views); APID 180 carries the
//            blackbody temperature. Calibrated here with the cold-space and blackbody views of
//            each scan, pixel by pixel, to brightness temperature at an effective 11.4 um.
//   IASI     VCID 10, APIDs 130/135/140/145: the interferometer's spectra, 2 x 2 detectors x 30
//            positions per scan; each spectrum packed per the BRD table (523 segments of 16
//            samples, a bit length per segment, LSB first in big-endian 16-bit words from payload
//            byte 314). Kept for a handful of channels (window, ozone, water vapour, CO2, 4 um).
//   ASCAT    VCID 15, APIDs 208..213: echo of the six antenna beams, 256 samples per pulse as a
//            16-bit float (1 sign, 8 exponent, 7 fraction bits) from payload byte 140.
//   GOME-2   VCID 24, APID 384: four detectors x 1024 spectral pixels, two readouts per packet,
//            16 packets per scan (32 positions). Quick looks only; no wavelength calibration.
//   SEM      VCID 3, APID 37: 40 particle counters, 16 two-second samples per packet (inverted
//            bytes from payload byte 15).
//   Admin messages (VCID 34) are counted.
//
// METEOR-M HRPT:
//   MTVZA-GY  8 bytes at frame offsets 14, 270, 526, 782 of every 1024-byte frame form a byte
//            stream of 248-byte MTVZA frames (marker 0xFB386A45, or 0x38FB456A byte-swapped);
//            the scan position (2..26) and 30 channels (5 low, 2 full, 23 medium resolution)
//            give 200 samples per channel per scan, all Earth views (no calibration views in this
//            stream). No clock of its own: each scan takes the MSU-MR line time. The counts fall as
//            the scene warms. Mapped as a conical scan (mtvza geometry in metopmaps.cpp).
//
// Formats as documented by the SatDump project (GPL); this is an independent implementation.
// MHS and AMSU-A swath pictures are counts (their maps are calibrated); MTVZA stays uncalibrated.

#include <QString>
#include <QStringList>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "polarhrpt.h"
#include "ascatl2.h"

namespace nadirstation::polar {

class MetopInstruments {
public:
    // An RS-checked CADU of any virtual channel.
    void feed(const std::uint8_t* cadu);
    void reset();
    // Writes the products into `dir`; returns the files written.
    QStringList save(const QString& dir) const;
    QString summary() const;
    int mhsLines() const { return int(mhs_.size()); }
    // MetOp spacecraft id from the frames (11 = MetOp-B, 12 = MetOp-A, 13 = MetOp-C), -1 before any.
    int spacecraftId() const { return scid_; }
    // For the ASCAT map products: the TLE of this MetOp and the folder with the coastline file.
    void setAscatContext(const std::string& tleName, const std::string& l1, const std::string& l2, const QString& mapsDir) {
        ascatTleName_ = tleName; ascatTle1_ = l1; ascatTle2_ = l2; mapsDir_ = mapsDir;
    }
    QString ascatLog() const { return ascatLog_; }

private:
public:
    struct MhsScan {
        std::array<std::array<std::uint16_t, 90>, 5> ch{};
        double time = 0.0;                                        // packet time, Unix s (0 = none)
        std::array<std::uint16_t, 3> prtCal{};                    // PRT calibration resistor counts
        std::array<std::uint16_t, 5> prt{};                       // blackbody PRT counts
        std::array<std::uint8_t, 39> hk{};                        // housekeeping (instrument temperatures)
        std::array<std::array<std::uint16_t, 2>, 5> views{};      // per channel: space, warm (0 = none usable)
        int pie = 0;
    };
    struct AmsuScan {
        std::array<std::array<std::uint16_t, 30>, 15> ch{};
        bool a1 = false, a2 = false;
        double time = 0.0;
        std::array<std::array<std::uint16_t, 2>, 13> a1Views{};   // blackbody, space (channels 3..15)
        std::array<std::uint16_t, 45> a1Temps{};
        std::array<std::array<std::uint16_t, 2>, 2> a2Views{};    // blackbody, space (channels 1, 2)
        std::array<std::uint16_t, 19> a2Temps{};
    };
    QString mapsLog() const { return mapsLog_; }
private:
    // Map products of MHS, AMSU-A and the IASI imager (metopmaps.cpp); needs the TLE.
    QStringList saveMaps(const QString& dir) const;
    mutable QString mapsLog_;
    std::vector<std::array<double, 36>> iisFieldTimes_;
    std::array<double, 36> iisCurTimes_{};
    void mhsPacket(const Packet& p);
    void amsuPacket(const Packet& p);
    void iasiPacket(const Packet& p);
    void iasiSpectrum(const Packet& p);
    void ascatPacket(const Packet& p);
    void gomePacket(const Packet& p);
    void semPacket(const Packet& p);

    VcidDemux vc3_{14, 882}, vc10_{14, 882}, vc12_{14, 882}, vc15_{14, 882}, vc24_{14, 882};
    // IASI spectra: selected channels, 60 positions per row, two rows per scan
    static constexpr int kIasiSel = 6;
    std::vector<std::array<std::array<std::uint16_t, 60>, kIasiSel>> iasiRows_;
    int iasiRowBase_ = 0;
    // ASCAT echo per beam: rows of 256 samples
    std::array<std::vector<std::array<float, 256>>, 6> ascat6_;
    std::vector<AscatPulse> ascatPulses_;
    int scid_ = -1;
    std::string ascatTleName_, ascatTle1_, ascatTle2_;
    QString mapsDir_;
    mutable QString ascatLog_;
    // GOME-2: per scan, 4 detectors x 1024 pixels x 32 positions
    struct GomeScan { std::vector<std::uint16_t> px = std::vector<std::uint16_t>(4 * 1024 * 32, 0); };
    std::vector<GomeScan> gomeScans_;
    int gomeLastCounter_ = -1;
    // SEM: 40 channels per 2 s sample
    std::vector<std::array<std::uint8_t, 40>> semSamples_;
    std::vector<MhsScan> mhs_;
    std::vector<AmsuScan> amsu_;
    // IASI IIS: one 64 x (36*64) block per scan, in field order, 12-bit counts (0 = missing)
    std::vector<std::vector<std::uint16_t>> iisScans_;
    std::vector<double> iisBbt_;
    std::vector<std::uint16_t> iisCur_;
    int iisLastField_ = 0;
    double lastBbt_ = 0.0;
    std::size_t iasiSpectra_ = 0, ascat_ = 0, gome_ = 0, sem_ = 0, admin_ = 0;
};

class MtvzaReader {
public:
    // One 1024-byte METEOR HRPT frame (marker included) and the time of the newest MSU-MR line
    // (Unix s; 0 = unknown): MTVZA has no clock of its own.
    void feedFrame(const std::uint8_t* frame, double msuMrTime = 0.0);
    void reset();
    // For the map products: the MSU-MR serial number (which METEOR-M), its TLE, the coastline folder.
    void setContext(int serial, const std::string& tleName, const std::string& l1, const std::string& l2, const QString& mapsDir) {
        serial_ = serial; tleName_ = tleName; tle1_ = l1; tle2_ = l2; mapsDir_ = mapsDir;
    }
    QStringList save(const QString& dir) const;
    int scans() const { return int(scans_.size()); }
    bool hasSignal() const;
    QString mapsLog() const { return mapsLog_; }
    // Scan geometry used for the maps (per satellite): cone half-angle and the azimuth of sample 0.
    static void geometry(int serial, double* coneDeg, double* azimuth0Deg);

    using Scan = std::array<std::array<std::int16_t, 200>, 30>;
    // Channel layout of the stream: 1..5 low resolution (one value per 4 samples), 6..7 full,
    // 8..30 medium (one value per 2 samples).
    static int sampleStep(int ch) { return ch < 5 ? 4 : ch < 7 ? 1 : 2; }

private:
    void frame248(const std::uint8_t* f, bool swapped);
    // Counts with bit-error spikes removed (NaN), sign flipped so that warmer scenes read higher.
    std::vector<std::array<std::array<float, 200>, 30>> cleaned(int* spikes) const;
    QStringList saveMaps(const QString& dir, const std::vector<std::array<std::array<float, 200>, 30>>& v) const;
    std::vector<std::uint8_t> stream_;
    std::vector<Scan> scans_;
    std::vector<double> scanTimes_;            // MSU-MR time at the end of each scan (0 = none)
    Scan cur_{};
    bool curAny_ = false;
    double msuTime_ = 0.0;
    int lastCounter_ = 0;
    std::size_t otherFrames_ = 0;              // frames with a scan position outside 2..26
    int serial_ = -1;
    std::string tleName_, tle1_, tle2_;
    QString mapsDir_;
    mutable QString mapsLog_;
};

}  // namespace nadirstation::polar
