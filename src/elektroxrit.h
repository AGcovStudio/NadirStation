#pragma once
// ELEKTRO-L LRIT / HRIT: from reassembled xRIT files to images.
//
// The link layers are shared with GK-2A (gk2adecoder / gk2adatalink): CCSDS rate-1/2 K=7 code,
// RS(255,223) depth 4, 1024-byte CADU, randomised, packets of 8192 bytes on APID 0. What arrives
// here is one xRIT file at a time, laid out as the CGMS LRIT/HRIT Global Specification describes
// (primary header, image structure, navigation, annotation, time stamp, segment identification,
// then the data field), which is the layout Roshydromet uses for ELEKTRO-L.
//
// Image data fields are compressed (JPEG for the recordings seen so far; wavelet and T.4 are
// handled too) and are expanded with EUMETSAT's PublicDecompWT (third_party, Apache-2.0).
// Segments of one channel and one observation are placed in a canvas by segment number; a channel
// is written as a PNG as soon as all its segments are in, and whatever has arrived is written when
// the reception ends.
//
// Work is done on a thread of its own so decompression never holds up the demodulator.

#include "gk2adatalink.h"
#include <QDateTime>
#include <QImage>
#include <QString>
#include <QStringList>
#include <condition_variable>
#include <cstdint>
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace nadirstation::elektro {

struct XritHeader {
    bool valid = false;
    int fileType = -1;                 // 0 image, 1 GTS message, 2 text, 3 encryption key, 128+ mission
    std::uint32_t headerLength = 0;
    std::uint64_t dataBits = 0;
    int nb = 0, nc = 0, nl = 0, compression = 0;   // image structure (type 1)
    std::string projection;            // image navigation (type 2)
    std::int32_t cfac = 0, lfac = 0, coff = 0, loff = 0;
    std::string annotation;            // type 4: the file's name
    QDateTime time;                    // type 5, UTC
    bool hasSegment = false;           // type 128
    int spacecraft = 0, channel = 0, segment = 0, firstSegment = 0, lastSegment = 0, representation = 0;
    std::vector<int> recordTypes;      // every header record seen, for the log
};

XritHeader parseXrit(const std::vector<std::uint8_t>& bytes);

// Expands an image data field to NB-bit samples (one std::uint16_t per pixel, nc x nl).
// Returns false and fills `error` when the field cannot be decoded.
bool decompressImage(const XritHeader& header, const std::uint8_t* data, std::size_t size,
                     std::vector<std::uint16_t>& pixels, std::string& error);

class XritProducts {
public:
    // service: "LRIT" or "HRIT", used for folder names and messages.
    explicit XritProducts(QString service);
    ~XritProducts();
    XritProducts(const XritProducts&) = delete;
    XritProducts& operator=(const XritProducts&) = delete;

    void submit(gk2a::LritFile&& file, const QString& outputRoot);
    // Write every channel that has any segment (end of reception), then wait for the queue.
    void finalize();
    void reset();

    struct Update {
        QString message;               // for the status bar / event log
        QString stamp;                 // observation time, yyyyMMdd_HHmm
        QImage preview;                // latest channel canvas, 8-bit
        QString previewInfo;
        std::map<QString, QStringList> completed;  // PNGs grouped by observation time
    };
    Update take();

    std::size_t segmentsDecoded() const { return segments_.load(); }
    std::size_t skippedImages() const { return skippedImages_.load(); }
    std::size_t archiveFailures() const { return archiveFailures_.load(); }

private:
    struct Canvas {
        QString stamp, channelName, dir;
        int nc = 0, nl = 0, segments = 0, first = 1, last = 1, nb = 10;
        std::vector<std::uint16_t> pixels;
        std::vector<bool> have;        // per segment
        bool written = false;
        double micrometres = 0.0;      // band centre from the file name, 0 when unknown
        bool complete() const { return !have.empty() && std::all_of(have.begin(), have.end(), [](bool b) { return b; }); }
        bool any() const { return std::any_of(have.begin(), have.end(), [](bool b) { return b; }); }
    };
    // Level-2 style products of one observation, made from the channels as counts (the downlink
    // carries no calibration): written once all their inputs are complete, or as partial at the end.
    void makeProducts(const QString& stamp, bool final);
    // A channel as 8-bit, stretched; infrared bands inverted so cold cloud tops are white.
    QImage channelImage(const Canvas& c) const;
    const Canvas* band(const QString& stamp, double lo, double hi) const;
    std::map<QString, bool> productsWritten_;   // stamp|product -> written complete
    // Calibration tables from each observation's prologue (stamp -> 10 bands x 1024 counts).
    std::map<QString, std::array<std::array<float, 1024>, 10>> prologue_;
    void makeL2Products(const QString& stamp, bool final);
    std::map<QString, QStringList> outputs_;    // stamp -> every picture written for it
    void addOutputLocked(const QString& stamp, const QString& path);
    void run();
    struct Job {
        gk2a::LritFile file;
        XritHeader header;
        QDateTime when;
        QString name, base, band;
    };
    static constexpr std::size_t kQueueBytes = 128u * 1024u * 1024u;
    static constexpr std::size_t kCanvasBytes = 384u * 1024u * 1024u;
    static constexpr std::size_t kSingleCanvasBytes = 64u * 1024u * 1024u;
    void process(const Job& job);
    bool makeRoomForCanvas(const QString& stamp, std::size_t bytes);
    void closeObservation(const QString& stamp);
    void skipImage(const QString& reason);
    void writeCanvas(Canvas& c, bool partial);
    QImage toImage(const Canvas& c) const;
    void post(const QString& message);

    QString service_;
    std::thread worker_;
    std::mutex m_;
    std::condition_variable cv_, idle_;
    std::deque<Job> queue_;
    std::size_t queuedBytes_ = 0;
    std::size_t canvasBytes_ = 0;
    QString evictedThrough_;
    bool stop_ = false, busy_ = false;
    std::map<QString, Canvas> canvases_;   // key: stamp + channel
    Update pending_;
    std::atomic<std::size_t> filesSeen_{0}, segments_{0}, skippedImages_{0}, archiveFailures_{0};
};

}  // namespace nadirstation::elektro
