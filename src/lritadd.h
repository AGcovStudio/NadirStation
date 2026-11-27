// Additional Data: the textual records LRIT carries in a file's header.
//
// A GK-2A LRIT file is a primary header, a set of secondary header records, and then the payload.
// The secondary records are not only structural: they carry the metadata that makes the payload
// interpretable, and for the additional-data service they are the product.
//
// A decoded file shows the shape. Its primary header announces a
// total_header_length of 3900 bytes for a file whose payload is a single 2200x220 image segment,
// and the header text contains:
//
//     GEOS(128.2)
//     HALFTONE:=8.
//     _NAME:=IR105.
//     _UNIT:=KELVIN.
//     0:=330.05254.
//     1:=329.74910.
//     2:=329.44501.
//     ...
//
// So the bulk of that 3900 bytes is a look-up table mapping stored counts to kelvin, carried as
// ASCII key/value pairs, and the leading entries name the spacecraft, the channel and the unit.
//
// Two things follow, and both matter for the station. The look-up table is the calibration the
// payload needs, so a product assembled without it would be a picture of counts rather than of
// temperature. And the records are text, which means the additional-data service is readable
// without any image decoding at all: it can be displayed as it arrives.
//
// This file parses those records. It does not interpret the keys, because the set is the
// operator's and not something to hard-code; it reports what is there, in order, so a reader sees
// the same thing the transmitter sent.

#pragma once

#include <cstdint>
#include <string>
#include <array>
#include <map>
#include <vector>

namespace nadirstation::gk2a {

// One secondary header record.
struct LritHeaderRecord {
    std::uint8_t type = 0;          // 0 primary, 1 image structure, 2 image navigation,
                                    // 3 annotation, 4 time stamp, 5 ancillary text,
                                    // 6 key, 7 GK-2A key header, 128 segmentation
    std::uint16_t recordLength = 0; // includes the three bytes of type and length
    int offset = 0;                 // where the record starts inside the file
    std::size_t count = 0;          // how many records share this type, from the byte after length
    // The record body, with NULs removed and non-printable bytes replaced, so it can be shown.
    std::string text;
    // Whether the body looked like printable text rather than binary.
    bool textual = false;
};

// A key/value pair lifted out of a record.
struct LritKeyValue {
    std::string key;
    std::string value;
    int recordIndex = 0;            // which record it came from
};

struct LritAddData {
    bool parsed = false;
    std::uint8_t fileTypeCode = 0;
    std::uint32_t totalHeaderLength = 0;
    std::uint64_t dataFieldLength = 0;
    std::vector<LritHeaderRecord> records;
    std::vector<LritKeyValue> pairs;
    // The spacecraft string, when a record names one, e.g. "GEOS(128.2)".
    std::string spacecraft;
    // What the file is, in words, from its type code.
    std::string kind;
    // A one-line summary suitable for a list.
    std::string summary;
};

// What an LRIT file is, from the primary header's file type code (specification Table 4.3).
enum class LritKind {
    Image,      // 0
    GtsMessage, // 1, marked not used by the specification but defined
    Text,       // 2 -- the additional-data service
    KeyMessage, // 3
    AlertMessage, // 4
    Unknown,
};

LritKind lritKindOf(std::uint8_t fileTypeCode);

// The category an additional-data file belongs to, taken from the second field of its name:
// ADD_<category>_<sequence>_<date>_<time>_<part>.lrit. Returns empty when the name does not have
// that shape, because a missing name is not licence to invent one.
std::string lritAddCategory(const std::string& fileName);

// Where a file belongs under an output root, as a relative path:
//
//   images   <spacecraft>/<category-or-session>/<name>
//   ADD      <spacecraft>/ADD/<category>/<name>
//
// `category` falls back to the type code's name when the file name carries none, so nothing is
// filed under an "unknown" heading that hides what it was.
std::string lritRelativePath(const std::string& fileName, LritKind kind,
                             const std::string& spacecraft, const std::string& sessionStamp);

// Parse the header records of a reassembled LRIT file.
//
// `bytes` is the file as the link carried it: primary header, secondary records, payload. Only the
// header portion is read, so a partially received file still yields its records.
LritAddData parseLritAddData(const std::vector<std::uint8_t>& bytes);

// The name of a file type code, from the CGMS LRIT/HRIT Global Specification. Codes that appear in
// practice are named; anything else is reported as unknown rather than guessed at.
std::string lritFileTypeName(std::uint8_t code);

// The part of the file that carries the additional data: everything after the header records.
//
// The records are the file's own bookkeeping -- lengths, sequence numbers, the encryption key index
// -- and none of them is the data. The data field is.
struct LritDataField {
    std::size_t offset = 0;          // where it starts, from the file's announced header length
    std::vector<std::uint8_t> bytes; // its contents
    bool looksLikeImage = false;     // JPEG or PNG, which is what a weather chart arrives as
    std::string text;                // printable text with the padding and control bytes dropped
    bool printable = false;          // whether text is worth showing rather than hex
};

LritDataField lritDataField(const std::vector<std::uint8_t>& bytes);


}  // namespace nadirstation::gk2a
