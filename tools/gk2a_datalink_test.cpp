// End-to-end check of the LRIT data link layer: CADU stream in, LRIT file out.
//
// The layer below this one is verified to recover a CADU byte for byte. What is not yet shown is
// that a CADU stream can be turned back into the file it carried, which is what has to work before
// anything can be drawn.
//
// This builds a stream the way a conforming transmitter would:
//
//     assemble an LRIT file: 16-byte primary header, a couple of secondary header records, a key
//     header, then the payload
//     split it into CCSDS space packets, 884 bytes of payload space per CADU, with the ten-byte
//     prefix on the first packet, the sequence flags set, and a CRC on each
//     pack those packets into M_PDUs with the VCDU header in front, add filler CADUs on virtual
//     channel 63, and produce 1024-byte CADUs
//
// and then feeds the stream to LritFileAssembler and compares what comes out with what went in.
//
// The payload used is a real JPEG, because the point of this layer is to deliver a file that the
// layer above can draw, and a JPEG is the one payload whose integrity can be checked without
// another library: the marker structure is self-describing.
//
// Usage: NadirStation_GK2ADatalink

#include "../src/gk2adatalink.h"
#include "../src/gk2adecoder.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <vector>

namespace {

void put16(std::vector<std::uint8_t>& v, std::uint16_t x) {
    v.push_back(static_cast<std::uint8_t>(x >> 8));
    v.push_back(static_cast<std::uint8_t>(x & 0xFF));
}

void put16(std::vector<std::uint8_t>& v, std::uint16_t x);

// A structurally valid JPEG: SOI, COM, APP0/JFIF, DQT, SOF0, DHT, SOS, EOI. It is not meant to
// decode to a picture; it is meant to have the segment structure a decoder walks, so that "did the
// payload survive" can be answered by walking it. Each segment's length field counts its own two
// bytes plus the payload, which is where a hand-built version of this got it wrong before.
std::vector<std::uint8_t> buildJpeg(const std::string& comment) {
    std::vector<std::uint8_t> j;
    const auto seg = [&j](std::uint8_t marker, const std::vector<std::uint8_t>& body) {
        j.push_back(0xFF);
        j.push_back(marker);
        put16(j, static_cast<std::uint16_t>(body.size() + 2));
        j.insert(j.end(), body.begin(), body.end());
    };

    j.push_back(0xFF); j.push_back(0xD8);                        // SOI, no length field

    seg(0xFE, std::vector<std::uint8_t>(comment.begin(), comment.end()));   // COM

    {   // APP0 / JFIF
        std::vector<std::uint8_t> b = {'J', 'F', 'I', 'F', 0, 1, 1, 0};
        put16(b, 1); put16(b, 1);
        b.push_back(0); b.push_back(0);
        seg(0xE0, b);
    }
    {   // DQT
        std::vector<std::uint8_t> b = {0};
        b.insert(b.end(), 64, 16);
        seg(0xDB, b);
    }
    {   // SOF0
        std::vector<std::uint8_t> b = {8};
        put16(b, 1); put16(b, 1);
        b.push_back(1);
        b.push_back(1); b.push_back(0x11); b.push_back(0);
        seg(0xC0, b);
    }
    {   // DHT
        std::vector<std::uint8_t> b = {0};
        b.insert(b.end(), 16, 0);
        seg(0xC4, b);
    }
    {   // SOS, then a little entropy data
        std::vector<std::uint8_t> b = {1};
        b.push_back(1); b.push_back(0);
        b.push_back(0); b.push_back(63); b.push_back(0);
        seg(0xDA, b);
        for (int i = 0; i < 32; ++i) j.push_back(static_cast<std::uint8_t>(0x30 + (i % 8)));
    }

    j.push_back(0xFF); j.push_back(0xD9);                        // EOI
    return j;
}

// Walk the segment structure and report how many markers it holds. Once SOS appears the rest of
// the stream is entropy-coded data, not segments, so the walk stops at the first SOS and looks only
// for the final EOI; treating entropy data as segments is what made an earlier version of this
// report zero markers for a perfectly good payload.
std::size_t jpegMarkerCount(const std::vector<std::uint8_t>& j) {
    if (j.size() < 4 || j[0] != 0xFF || j[1] != 0xD8) return 0;
    std::size_t i = 2;
    std::size_t markers = 1;                                    // the SOI
    while (i + 1 < j.size()) {
        if (j[i] != 0xFF) {
            ++i;
            continue;
        }
        const std::uint8_t m = j[i + 1];
        if (m == 0xD9) { ++markers; break; }                    // EOI
        if (m == 0xDA) {                                        // SOS: entropy data follows
            ++markers;
            for (std::size_t k = j.size(); k-- > 0;) {
                if (j[k] == 0xFF && k > 0 && j[k - 1] == 0xFF) continue;
                if (j[k] == 0xD9 && k > 0 && j[k - 1] == 0xFF) { ++markers; break; }
            }
            break;
        }
        if (i + 3 >= j.size()) break;
        const std::size_t len = (static_cast<std::size_t>(j[i + 2]) << 8) | j[i + 3];
        if (len < 2) break;
        ++markers;
        i += 2 + len;
    }
    return markers;
}

}  // namespace

int main() {
    using namespace nadirstation::gk2a;
    std::cout << "=== GK-2A LRIT data link layer, CADU stream to file ===\n\n";

    const std::string name = "test_image.jpg";
    const std::vector<std::uint8_t> jpeg = buildJpeg("GK2A LRIT end to end test");
    std::cout << "  payload: " << jpeg.size() << " bytes, " << jpegMarkerCount(jpeg)
              << " JPEG markers\n";

    // --- build an LRIT file ---------------------------------------------------------------
    // Secondary headers, each: type, 2-byte record length, record count, then the records.
    std::vector<std::uint8_t> headers;
    headers.push_back(0);                                        // placeholder for the type code
    // Type 1: image structure, with a plausible size.
    {
        std::vector<std::uint8_t> rec;
        put16(rec, 0x0001);
        put16(rec, 0x0001);
        rec.push_back(0);                                        // bits per pixel
        put16(rec, 1); put16(rec, 1);                            // columns, rows
        headers.push_back(1);
        put16(headers, static_cast<std::uint16_t>(rec.size() + 4));
        headers.push_back(1);
        headers.insert(headers.end(), rec.begin(), rec.end());
    }
    // Type 3: annotation, carrying the file name.
    {
        std::vector<std::uint8_t> rec;
        rec.push_back(0);
        rec.insert(rec.end(), name.begin(), name.end());
        rec.push_back(0);
        headers.push_back(3);
        put16(headers, static_cast<std::uint16_t>(rec.size() + 4));
        headers.push_back(1);
        headers.insert(headers.end(), rec.begin(), rec.end());
    }
    // Type 7: the GK-2A key header. Zero means not encrypted.
    {
        headers.push_back(7);
        put16(headers, 7);
        headers.push_back(1);
        headers.push_back(0); headers.push_back(0);
        headers.push_back(0); headers.push_back(0);
    }

    const std::size_t totalHeaderLength = headers.size();
    std::cout << "  header block: " << headers.size() << " bytes, JPEG " << jpeg.size()
              << " bytes, expecting a file of " << (headers.size() + jpeg.size())
              << " bytes\n";
    std::vector<std::uint8_t> file;
    // A conforming 16-byte primary header, matching what a real GK-2A file carries:
    //
    //   byte 0       type, 0 for a primary header
    //   bytes 1-2    record_length, the length of this record, which is 16
    //   byte 3       file_type_code, 0 for imagery
    //   bytes 4-7    total_header_length: the whole header, this one included
    //   bytes 8-15   data_field_length: the payload that follows
    //
    // The checker reads that layout. An earlier version of this test wrote a 13-byte header of its
    // own shape, so the fields came back read from offsets it had never written and the mismatch
    // it reported said nothing about the reassembly underneath.
    const std::size_t primaryHeaderBytes = 16;
    const std::size_t wholeHeaderLength = primaryHeaderBytes + totalHeaderLength;
    file.push_back(0);                                           // byte 0: primary header
    put16(file, 16);                                             // bytes 1-2: record length
    file.push_back(0);                                           // byte 3: file type code, image
    file.push_back(static_cast<std::uint8_t>((wholeHeaderLength >> 24) & 0xFF));
    file.push_back(static_cast<std::uint8_t>((wholeHeaderLength >> 16) & 0xFF));
    file.push_back(static_cast<std::uint8_t>((wholeHeaderLength >> 8) & 0xFF));
    file.push_back(static_cast<std::uint8_t>(wholeHeaderLength & 0xFF));
    for (int i = 0; i < 8; ++i)                                  // bytes 8-15: data field length
        file.push_back(static_cast<std::uint8_t>((jpeg.size() >> (8 * (7 - i))) & 0xFF));
    if (file.size() != primaryHeaderBytes)
        std::cout << "    NOTE: primary header is " << file.size() << " bytes, expected "
                  << primaryHeaderBytes << "\n";
    file.insert(file.end(), headers.begin(), headers.end());
    file.insert(file.end(), jpeg.begin(), jpeg.end());
    std::cout << "  LRIT file: " << file.size() << " bytes, header length "
              << totalHeaderLength << "\n";
    // Break the assembled file into its three parts, because a total that does not equal header
    // plus payload means this test built the wrong thing, not that the assembler misbehaved.
    std::cout << "    file is " << file.size() << " = " << primaryHeaderBytes
              << " primary header + " << headers.size() << " secondary + "
              << jpeg.size() << " payload\n";
    if (file.size() != wholeHeaderLength + jpeg.size())
        std::cout << "    BUILD MISMATCH\n";
    std::cout << "    file head, first 20 bytes:";
    for (std::size_t i = 0; i < 20 && i < file.size(); ++i) std::printf(" %02X", file[i]);
    std::cout << "\n    header block, first 20 bytes (held separately):";
    for (std::size_t i = 0; i < 20 && i < headers.size(); ++i) std::printf(" %02X", headers[i]);
    std::cout << "\n    payload starts at file[" << (file.size() - jpeg.size()) << "]";
    std::cout << "\n    tail of file, last 8 bytes:";
    for (std::size_t i = file.size() > 8 ? file.size() - 8 : 0; i < file.size(); ++i)
        std::printf(" %02X", file[i]);
    std::cout << "\n    tail of JPEG, last 8 bytes:";
    for (std::size_t i = jpeg.size() > 8 ? jpeg.size() - 8 : 0; i < jpeg.size(); ++i)
        std::printf(" %02X", jpeg[i]);
    std::cout << "\n";

    // Parse it back with the same routine the assembler uses, as a first check.
    {
        std::size_t off = 0;
        std::uint32_t key = 0;
        bool enc = false;
        const LritPrimaryHeader p = parseLritPrimaryHeader(file, &off, &key, &enc);
        std::cout << "  parsed back: type " << int(p.fileTypeCode) << ", header "
                  << p.totalHeaderLength << ", data " << p.dataLength
                  << ", dataOffset " << off << ", encrypted " << (enc ? "yes" : "no") << "\n";
        // Compare against the whole header, primary included, which is what the field means and
        // what the assembler uses to find the payload.
        if (off != wholeHeaderLength) {
            std::cout << "  HEADER LENGTH MISMATCH: parsed " << off << ", built "
                      << wholeHeaderLength << "\n";
            return 1;
        }
    }

    // --- packetise ------------------------------------------------------------------------
    constexpr std::size_t kMpduSize = 884;
    // Each packet carries: a 6-byte CCSDS header, the body, and a 2-byte CRC. The first packet of
    // a file also carries a 10-byte prefix ahead of the LRIT data.
    constexpr std::size_t kFirstPrefix = 10;
    constexpr std::size_t kBodyFirst = kMpduSize - 6 - 2 - kFirstPrefix - 6;
    constexpr std::size_t kBodyRest = kMpduSize - 6 - 2 - 6;

    struct Pkt { std::vector<std::uint8_t> bytes; };
    std::vector<Pkt> packets;
    std::size_t consumed = 0;
    std::uint16_t seq = 0;
    while (consumed < file.size()) {
        const bool first = packets.empty();
        const std::size_t cap = first ? kBodyFirst : kBodyRest;
        const std::size_t take = std::min(cap, file.size() - consumed);
        const bool last = (consumed + take >= file.size());

        std::vector<std::uint8_t> body;
        if (first)
            for (int i = 0; i < static_cast<int>(kFirstPrefix); ++i) body.push_back(0xAA);
        body.insert(body.end(), file.begin() + static_cast<std::ptrdiff_t>(consumed),
                    file.begin() + static_cast<std::ptrdiff_t>(consumed + take));
        const std::uint16_t crc = lritCrc(body.data(), body.size());
        body.push_back(static_cast<std::uint8_t>(crc >> 8));
        body.push_back(static_cast<std::uint8_t>(crc & 0xFF));

        std::vector<std::uint8_t> pkt;
        const std::uint8_t flag = first ? (last ? 3 : 1) : (last ? 2 : 0);
        const std::uint16_t apid = 100;
        // The length field counts the bytes after it, less one: that is the body together with
        // its CRC, and not the six-byte header the field lives in. An earlier version added the
        // header here, which announced every packet six bytes too long and left the reassembler
        // waiting for bytes that never came.
        const std::uint16_t pktLen = static_cast<std::uint16_t>(body.size() - 1);
        pkt.push_back(static_cast<std::uint8_t>((apid >> 8) & 0x07));
        pkt.push_back(static_cast<std::uint8_t>(apid & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>((flag << 6) | ((seq >> 8) & 0x3F)));
        pkt.push_back(static_cast<std::uint8_t>(seq & 0xFF));
        pkt.push_back(static_cast<std::uint8_t>(pktLen >> 8));
        pkt.push_back(static_cast<std::uint8_t>(pktLen & 0xFF));
        pkt.insert(pkt.end(), body.begin(), body.end());
        packets.push_back({pkt});
        ++seq;
        consumed += take;
    }
    std::cout << "  packetised into " << packets.size() << " CCSDS packets\n";

    // --- pack into CADUs ------------------------------------------------------------------
    std::vector<std::vector<std::uint8_t>> cadus;
    std::vector<std::uint8_t> mpdu;
    std::uint32_t counter = 0;
    const auto flushVcdu = [&](bool filler) {
        if (!filler && mpdu.empty()) return;
        std::vector<std::uint8_t> cadu(1024, 0);
        cadu[0] = 0x1A; cadu[1] = 0xCF; cadu[2] = 0xFC; cadu[3] = 0x1D;
        const std::uint8_t vcid = filler ? 63 : 0;
        cadu[4] = 0x00;                                          // version 0, spacecraft id high
        cadu[5] = static_cast<std::uint8_t>((0x00 << 6) | (vcid & 0x3F));
        cadu[6] = static_cast<std::uint8_t>((counter >> 16) & 0xFF);
        cadu[7] = static_cast<std::uint8_t>((counter >> 8) & 0xFF);
        cadu[8] = static_cast<std::uint8_t>(counter & 0xFF);
        cadu[9] = 0;
        ++counter;
        // The M_PDU header is the two bytes after the VCDU header. A pointer of zero means the
        // first packet header in this frame begins at the start of the M_PDU data, which is what
        // this test builds: one run of complete packets with nothing carried over from a previous
        // frame.
        cadu[10] = 0;
        cadu[11] = 0;
        if (filler) {
            for (std::size_t i = 12; i < 896; ++i) cadu[i] = 0x55;
        } else {
            for (std::size_t i = 0; i < mpdu.size() && i < kMpduSize; ++i) cadu[12 + i] = mpdu[i];
        }
        // Deliberately NOT randomised. This test exercises the reassembler, which sits above the
        // decoder, and LritDecoder already derandomised the frame before handing it on -- it
        // undoes the randomiser over bytes 4 to the end and checks the Reed-Solomon syndromes over
        // the result. Feeding a randomised frame here would be feeding a frame from one layer too
        // low: the VCDU header would read as noise and the packets with it. The randomiser itself
        // is covered by the end-to-end test, which builds a frame, encodes it, and decodes it
        // again.
        //
        // Confirmed against real data: SatDump's own decoded output passes the Reed-Solomon check
        // with no further derandomisation, because its FEC stage already did it. The check is on
        // plaintext.
        cadus.push_back(cadu);
        mpdu.clear();
    };

    cadus.push_back([] {                                          // a filler CADU first, to prove vcid 63 is dropped
        std::vector<std::uint8_t> c(1024, 0x55);
        c[0] = 0x1A; c[1] = 0xCF; c[2] = 0xFC; c[3] = 0x1D;
        c[5] = 63;
        return c;
    }());
    for (const Pkt& p : packets) {
        if (mpdu.size() + p.bytes.size() > kMpduSize) flushVcdu(false);
        mpdu.insert(mpdu.end(), p.bytes.begin(), p.bytes.end());
    }
    flushVcdu(false);
    std::cout << "  packed into " << cadus.size() << " CADUs (one of them filler)\n\n";

    // --- reassemble -----------------------------------------------------------------------
    LritFileAssembler asm_;
    std::vector<LritFile> got;
    for (const auto& c : cadus) {
        const std::vector<LritFile> f = asm_.feed(c);
        got.insert(got.end(), f.begin(), f.end());
    }
    std::cout << "=== assembler ===\n";
    std::cout << "  CADUs fed        " << asm_.cadusFed << "\n";
    std::cout << "  filler dropped   " << asm_.fillerCadus << "\n";
    std::cout << "  packets seen     " << asm_.packetsSeen << "\n";
    std::cout << "  CRC failures     " << asm_.crcFailures << "\n";
    std::cout << "  files recovered  " << got.size() << "\n";

    if (got.empty()) {
        std::cout << "\nNO FILE RECOVERED\n";
        return 1;
    }
    const LritFile& f = got.front();
    std::cout << "\n  file: apid " << f.apid << ", vcid " << int(f.vcid)
              << ", name \"" << f.fileName << "\"\n";
    std::cout << "        type " << int(f.primary.fileTypeCode)
              << ", header " << f.primary.totalHeaderLength
              << ", dataOffset " << f.dataOffset
              << ", encrypted " << (f.encrypted ? "yes" : "no") << "\n";

    const bool sizeOk = f.bytes.size() == file.size();
    const bool bytesOk = sizeOk && std::memcmp(f.bytes.data(), file.data(), file.size()) == 0;
    std::cout << "  size matches: " << (sizeOk ? "yes" : "no") << "\n";
    std::cout << "  bytes match : " << (bytesOk ? "yes" : "no") << "\n";

    std::vector<std::uint8_t> payload(f.bytes.begin() + static_cast<std::ptrdiff_t>(f.dataOffset),
                                      f.bytes.end());
    // Length first: comparing with memcmp before checking the sizes would read past whichever
    // buffer is shorter, and a length mismatch is itself the finding.
    const bool lenOk = payload.size() == jpeg.size();
    const bool jpegOk = lenOk && std::memcmp(payload.data(), jpeg.data(), jpeg.size()) == 0;
    std::cout << "  payload length: " << payload.size() << " (sent " << jpeg.size() << ") "
              << (lenOk ? "ok" : "MISMATCH") << "\n";
    std::cout << "  payload is the JPEG sent, byte for byte: " << (jpegOk ? "yes" : "no")
              << " (" << payload.size() << " bytes, " << jpegMarkerCount(payload)
              << " markers)\n";

    const bool ok = sizeOk && bytesOk && jpegOk && asm_.crcFailures == 0 && asm_.fillerCadus == 1;
    std::cout << "\n" << (ok ? "DATA LINK OK" : "DATA LINK FAILED") << "\n";
    return ok ? 0 : 1;
}
