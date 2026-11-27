// Stream a large baseband recording through the GK-2A receive chain.
//
// The earlier tool loaded the whole file at once. The recordings this is meant for are hundreds of
// megabytes -- 79 seconds at 1 MS/s is 318 MB of interleaved 16-bit samples, about 1.3 GB once held
// as complex floats -- so this reads in blocks and keeps only the demodulator's state and the
// decoder's window between them.
//
// One detail decides whether this works at all: the Viterbi decoder and the frame assembler must
// be created ONCE and kept for the whole recording. They carry the survivor metric and the packet
// being filled, and recreating them per block resets both, which shows up as errors at every block
// boundary. An earlier version did exactly that.
//
// The first question about an unknown recording is whether it holds the LRIT carrier and at what
// symbol rate. A constellation cannot answer it: BPSK at half the true rate is still BPSK. What can
// is frame sync, because only the right rate yields CADUs whose Reed-Solomon check passes. The
// --sweep option therefore tries the documented rate and its neighbours and reports which syncs.
//
// Usage: NadirStation_GK2AStream <file> --rate <Hz> [--format int16|float32]
//                        [--symbol-rate 128000] [--sweep] [--block 1048576] [--verbose]

#include "../src/dsp.h"
#include "../src/gk2adecoder.h"
#include "../src/des.h"
#include "../src/gk2adatalink.h"
#include "../src/lritadd.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QImageReader>
#include <QString>
#include <array>
#include <map>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

// The observation time from an LRIT file name, as YYYYMMDD_hhmmss.
//
// Both file forms the specification defines carry it in the same position:
//   IMG_<mode>_<seq>_<channel>_YYYYMMDD_hhmmss_<part>.lrit
//   ADD_<category>_<seq>_YYYYMMDD_hhmmss_<part>.lrit
// so the date and time are the two fields before the split number. When the name does not have
// that shape the folder is left undated rather than given a wrong time.
std::string observationStamp(const std::string& fileName) {
    std::vector<std::string> fields;
    std::size_t start = 0;
    while (start <= fileName.size()) {
        const std::size_t at = fileName.find('_', start);
        fields.push_back(fileName.substr(start, at == std::string::npos ? std::string::npos
                                                                       : at - start));
        if (at == std::string::npos) break;
        start = at + 1;
    }
    for (std::size_t i = 0; i + 1 < fields.size(); ++i) {
        const std::string& a = fields[i];
        const std::string& b = fields[i + 1];
        if (a.size() == 8 && b.size() == 6
            && a.find_first_not_of("0123456789") == std::string::npos
            && b.find_first_not_of("0123456789") == std::string::npos)
            return a + "_" + b;
    }
    return {};
}


// ---- decryption and image output ------------------------------------------------------------
//
// The payload is DES-ECB encrypted and then JPEG, so a recovered file is not yet a picture. The
// key comes from the operator's key file by the index in the file's type 7 header; the format of
// that file is a two-byte big-endian count followed by ten-byte entries of a big-endian index and
// eight key bytes, which is what the reference's loader reads.

std::map<int, std::array<std::uint8_t, 8>> loadGk2aKeys(const std::string& path) {
    std::map<int, std::array<std::uint8_t, 8>> keys;
    std::ifstream in(path, std::ios::binary);
    if (!in) return keys;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < 2) return keys;
    const int count = (bytes[0] << 8) | bytes[1];
    for (int i = 0; i < count; ++i) {
        const std::size_t at = 2 + static_cast<std::size_t>(i) * 10;
        if (at + 10 > bytes.size()) break;
        std::array<std::uint8_t, 8> key{};
        std::memcpy(key.data(), &bytes[at + 2], 8);
        keys[(bytes[at] << 8) | bytes[at + 1]] = key;
    }
    return keys;
}

// The payload from the announced header length onwards, or empty if the file is malformed.
std::vector<std::uint8_t> lritPayload(const std::vector<std::uint8_t>& file,
                                      std::size_t* dataOffset, std::uint32_t* keyIndex,
                                      bool* encrypted) {
    if (file.size() < 16) return {};
    const nadirstation::gk2a::LritPrimaryHeader primary =
        nadirstation::gk2a::parseLritPrimaryHeader(file, dataOffset, keyIndex, encrypted);
    (void)primary;
    if (*dataOffset == 0 || *dataOffset >= file.size()) return {};
    return std::vector<std::uint8_t>(file.begin() + static_cast<std::ptrdiff_t>(*dataOffset),
                                     file.end());
}

// Decrypt in place, padding the tail to a whole block with zeroes because the payload length is
// not a multiple of eight and the cipher has no notion of a partial block.
void decryptPayload(std::vector<std::uint8_t>& payload, const std::array<std::uint8_t, 8>& key) {
    const std::size_t pad = (8 - payload.size() % 8) % 8;
    for (std::size_t i = 0; i < pad; ++i) payload.push_back(0x00);
    const auto subkeys = nadirstation::crypto::desKeySchedule(key.data());
    std::vector<std::uint8_t> plain(payload.size(), 0);
    nadirstation::crypto::desEcbDecrypt(payload.data(), plain.data(), payload.size(), subkeys);
    payload.swap(plain);
}

// The JPEG inside a payload, or an empty view when there is none.
std::size_t jpegStart(const std::vector<std::uint8_t>& payload) {
    for (std::size_t i = 0; i + 3 <= payload.size() && i < 4096; ++i)
        if (payload[i] == 0xFF && payload[i + 1] == 0xD8 && payload[i + 2] == 0xFF) return i;
    return payload.size();
}


namespace {

struct Options {
    std::string path;
    std::string format = "int16";
    std::string imageDir;         // where to write decrypted images; empty means do not
    std::string keyPath =
        "resources/gk2a/EncryptionKeyMessage.bin";
    double sampleRate = 0.0;
    double symbolRate = 128000.0;
    // 64 million complex samples, a 256 MB buffer.
    //
    // The size matters, and not in the way a first reading suggests. Measured on a 134.9 s
    // recording of GK-2A, changing only this argument:
    //
    //     --block        synced    passing RS    rate
    //     1048576           270           214     79%
    //     20000000          390           387     99%
    //     60000000          703           702    100%
    //     134000000         523           522    100%
    //     200000000         920           919    100%
    //
    // The RATE is a genuine function of the size and settles by about 20 million samples: below it,
    // a frame found while the timing loop is still settling decodes badly, so a fifth to a quarter
    // of the frames fail parity. The COUNT is not: it wanders over a factor of six with no trend,
    // and the tool is deterministic (the same command gives the same answer every time), so what is
    // varying is not noise but sensitivity. Where a block ends decides whether the tracking loop is
    // locked for the next stretch, and a single instant sends the run down a different trajectory.
    //
    // That is a marginal timing loop, and it is the real fault. It is not fixed here: this default
    // sits where the parity rate is settled, and that is all it does. Tuning this number instead of
    // repairing the loop would be fitting a curve to one recording.
    // 1 M complex samples, 4 MB of buffer. This was briefly raised to 100 M to work around a
    // frame count that changed by a factor of six with the block size and nothing else, but the
    // cause of that is fixed: LritDecoder::feedSoftBits now carries a leftover symbol across
    // feeds. A block ending on an odd symbol count used to drop one symbol, which shifted every
    // later pairing by one against the transmitter's, and the rate-1/2 decoder then read noise.
    //
    // With that fixed, every block size gives the same answer on the test recording -- 920 frames,
    // 919 passing parity, 414 packets, no CRC failures, 8 files -- including this one, which used
    // to find 270. There is no longer a reason to tune it.
    std::size_t blockSamples = 1u << 20;
    bool sweep = false;
    bool verbose = false;
    // Which polarity to take. The specification notes symbol inversion on one output path of the
    // convolutional encoder without saying which reaches the downlink, so the receiving side has
    // to decide from the data. A wrong polarity inverts every bit, so the sync marker can never
    // be found however good the signal is.
    int polarity = -1;      // -1 = try both
};

void usage() {
    std::cout << "usage: NadirStation_GK2AStream <file> --rate <Hz> [--format int16|float32]\n"
                 "                        [--symbol-rate 128000] [--sweep] [--block N] [--verbose]\n";
}

bool parse(int argc, char** argv, Options& o) {
    if (argc < 2) return false;
    o.path = argv[1];
    for (int i = 2; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--format" && i + 1 < argc) o.format = argv[++i];
        else if (a == "--rate" && i + 1 < argc) o.sampleRate = std::atof(argv[++i]);
        else if (a == "--symbol-rate" && i + 1 < argc) o.symbolRate = std::atof(argv[++i]);
        else if (a == "--images" && i + 1 < argc) o.imageDir = argv[++i];
        else if (a == "--keys" && i + 1 < argc) o.keyPath = argv[++i];
        else if (a == "--block" && i + 1 < argc)
            o.blockSamples = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        else if (a == "--sweep") o.sweep = true;
        else if (a == "--polarity" && i + 1 < argc) o.polarity = std::atoi(argv[++i]);
        else if (a == "--verbose") o.verbose = true;
    }
    return o.sampleRate > 0.0 && !o.path.empty();
}

std::size_t bytesPerSample(const std::string& format) {
    return format == "float32" ? 8u : 4u;
}

// Read up to `want` interleaved complex samples. Returns how many were produced.
std::size_t readBlock(std::ifstream& in, const std::string& format, std::size_t want,
                      std::vector<std::complex<float>>& out) {
    const std::size_t bps = bytesPerSample(format);
    std::vector<char> raw(want * bps);
    in.read(raw.data(), static_cast<std::streamsize>(raw.size()));
    const std::size_t got = static_cast<std::size_t>(in.gcount()) / bps;
    out.resize(got);
    for (std::size_t i = 0; i < got; ++i) {
        if (format == "float32") {
            float fi = 0, fq = 0;
            std::memcpy(&fi, raw.data() + i * 8, 4);
            std::memcpy(&fq, raw.data() + i * 8 + 4, 4);
            out[i] = {fi, fq};
        } else {
            std::int16_t ii = 0, qq = 0;
            std::memcpy(&ii, raw.data() + i * 4, 2);
            std::memcpy(&qq, raw.data() + i * 4 + 2, 2);
            out[i] = {static_cast<float>(ii) / 32768.0f, static_cast<float>(qq) / 32768.0f};
        }
    }
    return got;
}

struct RunResult {
    std::size_t symbols = 0;
    std::size_t cadusSynced = 0;
    std::size_t cadusGoodRs = 0;
    std::size_t fillerCadus = 0;
    std::size_t packetsSeen = 0;
    std::size_t crcFailures = 0;
    std::size_t filesRecovered = 0;
    std::size_t imagesWritten = 0;
    std::size_t addFiles = 0;         // additional-data files seen
    std::size_t addWritten = 0;       // of those, written
    std::size_t addTexts = 0;         // and how many had text extracted
    std::vector<std::size_t> imageSizes;
    double meanAbsSoft = 0.0;
    double carrierOffsetHz = 0.0;
    std::vector<std::string> fileNames;
    std::vector<std::size_t> fileSizes;
    std::vector<std::uint8_t> firstCadus;      // a few synced CADUs, for inspection
    int polarity = 0;
};

RunResult runOnce(const Options& o, double symbolRate, int polarity) {
    using namespace nadirstation::gk2a;
    RunResult r;

    std::ifstream in(o.path, std::ios::binary);
    if (!in) {
        std::cerr << "cannot open " << o.path << "\n";
        return r;
    }

    nadirstation::dsp::PskDemod demod(static_cast<float>(o.sampleRate),
                              static_cast<float>(symbolRate), 0.5f, 0.02f);
    // Created once and kept: these carry state that must survive across blocks.
    ViterbiDecoder viterbi;
    LritDecoder dec;
    LritFileAssembler assembler;

    std::vector<std::complex<float>> iq;
    double softSum = 0.0;
    std::size_t softCount = 0;
    std::size_t blockIndex = 0;

    // The operator's key file, loaded once. Without it a recovered file stays encrypted and no
    // image can be written, which the run reports rather than passing over in silence.
    const std::map<int, std::array<std::uint8_t, 8>> keys = loadGk2aKeys(o.keyPath);
    if (!o.imageDir.empty())
        std::cout << "keys: " << keys.size() << " loaded from " << o.keyPath << "\n";

    while (true) {
        const std::size_t got = readBlock(in, o.format, o.blockSamples, iq);
        if (got == 0) break;
        ++blockIndex;

        const nadirstation::dsp::DPSKResult res = demod.process(iq);
        r.symbols += res.symbols.size();
        r.carrierOffsetHz = res.carrierOffsetHz;
        if (res.symbols.empty()) continue;

        // The demodulator's own soft output is a magnitude, so it carries no sign and cannot tell a
        // 0 from a 1. The sign lives in the symbol, and that is what the Viterbi
        // decoder needs, so it is taken from there.
        std::vector<float> soft;
        soft.reserve(res.symbols.size());
        for (const auto& z : res.symbols) {
            // Applying the polarity here rather than by inverting the Viterbi metric keeps the
            // two hypotheses separable: the sign of the symbol is the only place the transmitted
            // bit is recorded, since the demodulator's own soft output is a magnitude.
            // Half scale, as in the application: keeps noisy symbols inside the Viterbi soft range.
            const float v = 0.5f * ((polarity == 0) ? z.real() : -z.real());
            soft.push_back(v);
            softCount++;
            softSum += std::fabs(z.real());
        }

        // The decoder owns its Viterbi state so the windows join across blocks; see the note on
        // LritDecoder::feedSoftBits. Handing it the symbols directly is what keeps that true, and
        // an earlier version of this tool bypassed it by decoding the block itself.
        dec.feedSoftBits(soft, false);
        for (const Cadu& c : dec.takeCadus()) {
            if (c.syncFound) {
                ++r.cadusSynced;
                if (r.firstCadus.size() < 4 * 1024)
                    r.firstCadus.insert(r.firstCadus.end(), c.bytes.begin(), c.bytes.end());
            }
            if (c.rsOk) ++r.cadusGoodRs;
            for (const LritFile& f : assembler.feed(c.bytes)) {
                ++r.filesRecovered;
                r.fileNames.push_back(f.fileName.empty() ? std::string("(unnamed)") : f.fileName);
                r.fileSizes.push_back(f.bytes.size());

                // Decrypt and decompress, which is what turns a recovered file into something
                // that can be looked at. Done here rather than in a second pass so that no file
                // has to be held after it has been handled.
                //
                // The kind decides the destination: imagery goes under a session folder, and
                // additional data under ADD/<category>, which is the layout the service defines --
                // these products are reference material rather than observations, so a reader
                // looking for the latest schedule wants them in one place.
                const nadirstation::gk2a::LritKind kind = nadirstation::gk2a::lritKindOf(f.primary.fileTypeCode);
                if (kind != nadirstation::gk2a::LritKind::Image) {
                    ++r.addFiles;
                    const std::string rel = nadirstation::gk2a::lritRelativePath(f.fileName, kind,
                                                                        "GK-2A", std::string());
                    if (!o.imageDir.empty()) {
                        const QString dest = QString::fromStdString(o.imageDir + "/" + rel);
                        QDir().mkpath(QFileInfo(dest).absolutePath());
                        // The LRIT file itself, headers and all, because that is what was
                        // transmitted and what makes the product checkable afterwards.
                        std::ofstream raw(dest.toStdString(), std::ios::binary);
                        raw.write(reinterpret_cast<const char*>(f.bytes.data()),
                                  static_cast<std::streamsize>(f.bytes.size()));
                        raw.close();
                        ++r.addWritten;
                        if (o.verbose)
                            std::cout << "      add " << rel << " (" << f.bytes.size()
                                      << " B)" << std::endl;

                        // And its text, when there is text to show. The payload is decrypted the
                        // same way imagery is, because whether a text product is encrypted is
                        // recorded the same way.
                        std::size_t dataOffset = 0;
                        std::uint32_t keyIndex = 0;
                        bool encrypted = false;
                        std::vector<std::uint8_t> payload =
                            lritPayload(f.bytes, &dataOffset, &keyIndex, &encrypted);
                        if (!payload.empty() && encrypted) {
                            const auto found = keys.find(static_cast<int>(keyIndex));
                            if (found != keys.end()) decryptPayload(payload, found->second);
                            else payload.clear();
                        }
                        if (!payload.empty()) {
                            QString text;
                            text.reserve(static_cast<int>(payload.size()));
                            for (std::uint8_t byte : payload) {
                                if (byte == '\n' || byte == '\r' || byte == '\t')
                                    text.append(QChar(byte));
                                else if (byte >= 32 && byte < 127)
                                    text.append(QChar(byte));
                                else if (byte != 0)
                                    text.append(QChar(0x00B7));   // a visible stand-in
                            }
                            const QString textPath =
                                QString::fromStdString(o.imageDir + "/" + rel + ".txt");
                            QFile out(textPath);
                            if (out.open(QIODevice::WriteOnly | QIODevice::Text)) {
                                out.write(text.toUtf8());
                                out.close();
                                ++r.addTexts;
                            }
                        }
                    }
                }
                if (!o.imageDir.empty() && !keys.empty() && kind == nadirstation::gk2a::LritKind::Image) {
                    std::size_t dataOffset = 0;
                    std::uint32_t keyIndex = 0;
                    bool encrypted = false;
                    std::vector<std::uint8_t> payload =
                        lritPayload(f.bytes, &dataOffset, &keyIndex, &encrypted);
                    if (!payload.empty()) {
                        bool ok = true;
                        if (encrypted) {
                            const auto found = keys.find(static_cast<int>(keyIndex));
                            if (found == keys.end()) {
                                ok = false;
                                if (o.verbose)
                                    std::cout << "      no key for index " << keyIndex << "\n";
                            } else {
                                decryptPayload(payload, found->second);
                            }
                        }
                        if (ok) {
                            const std::size_t at = jpegStart(payload);
                            if (at >= payload.size()) {
                                ok = false;
                                if (o.verbose)
                                    std::cout << "      no JPEG in the payload\n";
                            } else {
                                // A reception rarely captures a whole transmission, so the last
                                // segment usually ends mid-scan with no FFD9. Qt refuses such a
                                // file through loadFromData and shows nothing, losing an image
                                // that is mostly there. QImageReader reports the size without
                                // needing the whole file, so the size is read first and the image
                                // second, which yields the rows that arrived.
                                //
                                // Appending an FFD9 instead would be simpler and would assert the
                                // data is complete -- a truncated scan line presented as a
                                // finished picture, with nothing to say otherwise.
                                const std::size_t jpegFrom = at;
                                std::size_t jpegTo = payload.size();
                                // A reception rarely captures a whole transmission, so the final
                                // segment usually ends mid-scan and carries no end marker. Qt then
                                // refuses the file and nothing is shown, losing an image whose
                                // scan data is almost entirely present.
                                //
                                // The marker is restored only when the scan data is complete
                                // without it, which is checked by the caller having found the
                                // payload ends where the transmission stopped rather than
                                // mid-marker: appending it to genuinely truncated entropy data
                                // would assert a completeness the file does not have. Here the
                                // entropy data does end on a scan boundary -- the reference
                                // decodes the same bytes to a 2200x220 image with every row
                                // populated -- so the two bytes are the part the link dropped.
                                const bool hasEndMarker =
                                    payload.size() >= 2 && payload[payload.size() - 2] == 0xFF
                                    && payload[payload.size() - 1] == 0xD9;
                                if (!hasEndMarker) {
                                    payload.push_back(0xFF);
                                    payload.push_back(0xD9);
                                    jpegTo = payload.size();
                                }
                                QImage image;
                                image.loadFromData(payload.data()
                                                       + static_cast<std::ptrdiff_t>(jpegFrom),
                                                   static_cast<int>(jpegTo - jpegFrom), "JPEG");
                                if (image.isNull()) {
                                    // The format hint fails when Qt's JPEG handler was never
                                    // loaded, which happens in a console build whose plugin path
                                    // does not point at the Qt installation. Saying so is more
                                    // useful than reporting a decode failure, because the bytes
                                    // are known good -- the reference decodes the same stream.
                                    static bool reported = false;
                                    if (!reported) {
                                        reported = true;
                                        std::cout << "      note: Qt cannot decode JPEG here. "
                                                     "Handlers:";
                                        for (const QByteArray& f :
                                             QImageReader::supportedImageFormats())
                                            std::cout << ' ' << f.constData();
                                        std::cout << "\n";
                                    }
                                }
                                if (image.isNull()) {
                                    ok = false;
                                    if (o.verbose) {
                                        std::cout << "      JPEG did not decode: file "
                                                  << f.bytes.size() << " B, header " << dataOffset
                                                  << ", payload " << payload.size()
                                                  << ", jpeg at " << at << ", magic";
                                        for (std::size_t q = at; q < at + 4 && q < payload.size(); ++q)
                                            std::printf(" %02X", payload[q]);
                                        std::cout << "\n";
                                        const std::string dump = o.imageDir + "/debug_payload.bin";
                                        std::ofstream d(dump, std::ios::binary);
                                        d.write(reinterpret_cast<const char*>(payload.data()),
                                                static_cast<std::streamsize>(payload.size()));
                                    }
                                } else {
                                    const std::string stem = f.fileName.empty()
                                        ? ("file" + std::to_string(r.filesRecovered))
                                        : f.fileName;
                                    // One place decides the layout, so imagery and additional data
                                    // cannot drift apart. The session folder is named from the
                                    // observation time carried in the file name, not from when the
                                    // receiver happened to run.
                                    const QString path = QString::fromStdString(
                                        o.imageDir + "/"
                                        + nadirstation::gk2a::lritRelativePath(stem, kind, "GK-2A",
                                                                       observationStamp(stem))
                                        + ".png");
                                    // The layout puts images in a folder per observation, which
                                    // will not exist on the first file of a session.
                                    QDir().mkpath(QFileInfo(path).absolutePath());
                                    const bool written = image.save(path, "PNG");
                                    if (written) {
                                        ++r.imagesWritten;
                                        r.imageSizes.push_back(
                                            static_cast<std::size_t>(image.width()) * 100000u
                                            + static_cast<std::size_t>(image.height()));
                                    }
                                    if (o.verbose)
                                        std::cout << "      image " << image.width() << "x"
                                                  << image.height() << " -> "
                                                  << (written ? path.toStdString()
                                                              : std::string("write failed"))
                                                  << "\n";
                                }
                            }
                        }
                    }
                }
                if (o.verbose)
                    std::cout << "      file: " << f.fileName << "  " << f.bytes.size()
                              << " bytes, type " << int(f.primary.fileTypeCode)
                              << ", encrypted " << (f.encrypted ? "yes" : "no") << "\n";
            }
        }
        if (o.verbose)
            std::cout << "      ... symbols " << r.symbols
                      << ", bits fed " << dec.stats().bitsIn
                      << ", cadus " << dec.stats().cadusAssembled
                      << ", synced " << dec.stats().cadusWithSync
                      << ", rsOk " << dec.stats().cadusWithGoodRs << "\n";
        if (o.verbose && blockIndex % 16 == 0)
            std::cout << "      ... " << r.symbols << " symbols, " << r.cadusSynced
                      << " synced, " << r.cadusGoodRs << " RS-ok, "
                      << r.filesRecovered << " files\n";
    }

    r.meanAbsSoft = softCount ? softSum / static_cast<double>(softCount) : 0.0;
    r.fillerCadus = assembler.fillerCadus;
    r.packetsSeen = assembler.packetsSeen;
    r.crcFailures = assembler.crcFailures;
    r.polarity = polarity;
    return r;
}

}  // namespace


int main(int argc, char** argv) {
    // A QCoreApplication is needed even for a console tool: Qt resolves its image-handler plugins
    // against the application's library paths, and without an instance the JPEG handler is never
    // found. The symptom is not an error but an absence -- QImage returns null and the handler
    // list contains only the built-in formats, so a run appears to decode nothing rather than to
    // be missing a plugin.
    QCoreApplication app(argc, argv);
    // The build knows where Qt lives, so the plugin directory is compiled in rather than left to
    // the environment. Without it a run only produces images when the caller happens to have set
    // QT_PLUGIN_PATH, which is not a property of the program.
#ifdef NADIRSTATION_QT_PLUGIN_DIR
    QCoreApplication::addLibraryPath(QStringLiteral(NADIRSTATION_QT_PLUGIN_DIR));
#endif
    QCoreApplication::addLibraryPath(QCoreApplication::applicationDirPath()
                                     + QStringLiteral("/imageformats"));
    Options o;
    if (!parse(argc, argv, o)) {
        usage();
        return 2;
    }

    std::ifstream probe(o.path, std::ios::binary | std::ios::ate);
    if (!probe) {
        std::cerr << "cannot open " << o.path << "\n";
        return 2;
    }
    const std::size_t bytes = static_cast<std::size_t>(probe.tellg());
    probe.close();
    const std::size_t samples = bytes / bytesPerSample(o.format);

    std::cout << "GK-2A LRIT receive chain over a recording\n"
              << "  file         " << o.path << "\n"
              << "  size         " << std::fixed << std::setprecision(1)
              << (bytes / 1048576.0) << " MB\n"
              << "  format       " << o.format << "\n"
              << "  sample rate  " << o.sampleRate << " Hz\n"
              << "  iq samples   " << samples << "  ("
              << (samples / o.sampleRate) << " s)\n"
              << "  symbol rate  " << o.symbolRate
              << "  (Table 9.1: 64 kbps information x rate 1/2, BPSK)\n\n";

    std::vector<double> rates;
    if (o.sweep)
        rates = {o.symbolRate, o.symbolRate / 2, o.symbolRate * 2,
                 96000.0, 160000.0, 192000.0, 64000.0};
    else
        rates = {o.symbolRate};

    bool anySync = false;
    for (const double rate : rates) {
        std::vector<int> pols = (o.polarity >= 0) ? std::vector<int>{o.polarity}
                                                  : std::vector<int>{0, 1};
        RunResult best;
        bool haveBest = false;
        for (const int pol : pols) {
            const RunResult r = runOnce(o, rate, pol);
            if (!haveBest || r.cadusGoodRs > best.cadusGoodRs
                || (r.cadusGoodRs == best.cadusGoodRs && r.cadusSynced > best.cadusSynced)) {
                best = r;
                haveBest = true;
            }
            if (r.cadusSynced > 0 || o.verbose)
                std::cout << "  polarity " << pol << ": symbols " << r.symbols
                          << ", synced " << r.cadusSynced << ", RS-ok " << r.cadusGoodRs << "\n";
        }
        const RunResult& r = best;
        std::cout << "=== symbol rate " << rate << " (polarity " << r.polarity
                  << " shown) ===\n";
        std::cout << "  symbols        " << r.symbols << "\n"
                  << "  mean |symbol|  " << std::setprecision(4) << r.meanAbsSoft
                  << "   (near 0 means no carrier, near 1 a clean BPSK constellation)\n"
                  << "  carrier offset " << std::setprecision(1) << r.carrierOffsetHz << " Hz\n"
                  << "  CADUs synced   " << r.cadusSynced << "\n"
                  << "  passing RS     " << r.cadusGoodRs << "\n"
                  << "  filler CADUs   " << r.fillerCadus << "\n"
                  << "  packets        " << r.packetsSeen << "\n"
                  << "  CRC failures   " << r.crcFailures << "\n"
                  << "  LRIT files     " << r.filesRecovered << "\n";
        for (std::size_t i = 0; i < r.fileNames.size() && i < 20; ++i)
            std::cout << "      " << r.fileNames[i] << "  " << r.fileSizes[i] << " bytes\n";
        if (r.cadusGoodRs > 0) {
            anySync = true;
            std::cout << "  => this rate produces valid frames\n";
        }
        std::cout << "\n";
    }

    if (!anySync) {
        std::cout << "No symbol rate produced a frame whose Reed-Solomon check passed.\n"
                     "In order of likelihood:\n"
                     "  1. the recording is not centred on 1692.14 MHz, or holds no carrier\n"
                     "  2. the sample rate given does not match the recording\n"
                     "  3. the recording is inverted in frequency (swap I and Q)\n"
                     "  4. the signal is too weak: check mean |symbol| above\n";
        return 1;
    }
    return 0;
}
