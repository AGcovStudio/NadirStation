// Additional Data parser. See lritadd.h for why the records matter and what they look like.

#include "lritadd.h"

#include "des.h"

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace nadirstation::gk2a {
namespace {

std::uint16_t readU16(const std::vector<std::uint8_t>& b, std::size_t i) {
    if (i + 1 >= b.size()) return 0;
    return static_cast<std::uint16_t>((b[i] << 8) | b[i + 1]);
}

std::uint32_t readU32(const std::vector<std::uint8_t>& b, std::size_t i) {
    if (i + 3 >= b.size()) return 0;
    return (static_cast<std::uint32_t>(b[i]) << 24)
           | (static_cast<std::uint32_t>(b[i + 1]) << 16)
           | (static_cast<std::uint32_t>(b[i + 2]) << 8)
           | static_cast<std::uint32_t>(b[i + 3]);
}

std::uint64_t readU64(const std::vector<std::uint8_t>& b, std::size_t i) {
    if (i + 7 >= b.size()) return 0;
    std::uint64_t v = 0;
    for (std::size_t k = 0; k < 8; ++k) v = (v << 8) | b[i + k];
    return v;
}

// Render a record body for display: keep printable ASCII, turn runs of NUL into nothing, and
// describe anything else by a dot. The purpose is to show what the transmitter sent without
// pretending binary fields are text.
std::string renderBody(const std::vector<std::uint8_t>& bytes, std::size_t from, std::size_t to) {
    std::string out;
    out.reserve(to > from ? to - from : 0);
    for (std::size_t i = from; i < to && i < bytes.size(); ++i) {
        const std::uint8_t c = bytes[i];
        if (c == 0) continue;                       // padding, not content
        if (c >= 32 && c < 127) out.push_back(static_cast<char>(c));
        else if (c == '\n') out.push_back('\n');    // structural: it separates fields
        else if (c == '\r' || c == '\t') continue;
        else out.push_back('.');
    }
    // Collapse the runs of spaces that pad fixed-width records, without touching newlines, which
    // carry the field boundaries.
    std::string collapsed;
    collapsed.reserve(out.size());
    int spaces = 0;
    for (char c : out) {
        if (c == ' ') {
            if (++spaces <= 2) collapsed.push_back(c);
        } else {
            spaces = 0;
            collapsed.push_back(c);
        }
    }
    while (!collapsed.empty() && collapsed.back() == ' ') collapsed.pop_back();
    return collapsed;
}

// A record is textual when most of its bytes are printable. The threshold is deliberately
// permissive: a record with a few binary fields among text is still worth showing.
bool looksTextual(const std::string& rendered) {
    if (rendered.size() < 4) return false;
    std::size_t printable = 0;
    for (char c : rendered)
        if (c != '.' || c == '\n') ++printable;    // a newline is text, not an unprintable byte
    return printable * 10 >= rendered.size() * 8;
}

// Lift "KEY:=VALUE" pairs out of a record, one per line.
//
// The fields are separated by newlines -- the annotation record holds 258 of them, delimiting 255
// look-up entries plus three leading fields. A first attempt treated '.' as the terminator because
// a hex dump showed dots between the fields; that dump had replaced every non-printable byte with a
// dot, so the dots were an artefact of looking rather than content. The result was one pair holding
// the whole record.
void extractPairs(const std::string& text, int recordIndex, std::vector<LritKeyValue>& out) {
    std::size_t lineStart = 0;
    while (lineStart <= text.size()) {
        std::size_t lineEnd = text.find('\n', lineStart);
        if (lineEnd == std::string::npos) lineEnd = text.size();
        const std::string line = text.substr(lineStart, lineEnd - lineStart);
        const std::size_t sep = line.find(":=");
        if (sep != std::string::npos && sep > 0) {
            LritKeyValue kv;
            kv.key = line.substr(0, sep);
            kv.value = line.substr(sep + 2);
            kv.recordIndex = recordIndex;
            // Trim the padding a fixed-width record may carry.
            while (!kv.key.empty() && kv.key.front() == ' ') kv.key.erase(kv.key.begin());
            while (!kv.key.empty() && kv.key.back() == ' ') kv.key.pop_back();
            while (!kv.value.empty() && kv.value.back() == ' ') kv.value.pop_back();
            if (!kv.key.empty()) out.push_back(kv);
        }
        if (lineEnd == text.size()) break;
        lineStart = lineEnd + 1;
    }
}

// The spacecraft string appears as a bare token like "GEOS(128.2)" rather than as a key/value
// pair, so it is recognised by shape.
std::string findSpacecraft(const std::vector<LritHeaderRecord>& records) {
    for (const auto& r : records) {
        if (!r.textual) continue;
        const std::size_t open = r.text.find('(');
        if (open == std::string::npos || open == 0) continue;
        const std::size_t close = r.text.find(')', open);
        if (close == std::string::npos) continue;
        std::size_t start = open;
        while (start > 0) {
            const char c = r.text[start - 1];
            if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) --start;
            else break;
        }
        if (open - start >= 3) return r.text.substr(start, close - start + 1);
    }
    return {};
}

}  // namespace

std::string lritFileTypeName(std::uint8_t code) {
    // CGMS LRIT/HRIT Global Specification, file type codes.
    switch (code) {
        case 0:   return "image";
        case 1:   return "GTS message";
        case 2:   return "alphanumeric text";
        case 3:   return "encryption key message";
        case 4:   return "alert message";
        case 128: return "image, reduced";
        case 129: return "alphanumeric text, reduced";
        case 130: return "GTS message, reduced";
        case 131: return "encryption key, reduced";
        case 132: return "alert message, reduced";
        case 255: return "additional data (type code 255)";
        default:  return "unknown type " + std::to_string(static_cast<int>(code));
    }
}

LritKind lritKindOf(std::uint8_t fileTypeCode) {
    switch (fileTypeCode) {
        case 0: return LritKind::Image;
        case 1: return LritKind::GtsMessage;
        case 2: return LritKind::Text;
        case 3: return LritKind::KeyMessage;
        case 4: return LritKind::AlertMessage;
        case 128: return LritKind::Image;
        case 129: return LritKind::Text;
        case 130: return LritKind::GtsMessage;
        case 131: return LritKind::KeyMessage;
        case 132: return LritKind::AlertMessage;
        // The specification's Table 4.3 gives 2 for an alphanumeric text file, and the reference
        // implementation routes additional data on 255. Which one the satellite uses cannot be
        // settled here -- no recording available contains additional data, so every file in all of
        // them carries type code 0. Accepting both costs nothing if one never appears, and missing
        // the one that does costs the whole service, so both are accepted and the log names the
        // code of every file filed this way.
        case 255: return LritKind::Text;
        default: return LritKind::Unknown;
    }
}

std::string lritAddCategory(const std::string& fileName) {
    // ADD_<category>_... -- the category is everything between the first two underscores.
    const std::string prefix = "ADD_";
    if (fileName.size() <= prefix.size()) return {};
    if (fileName.compare(0, prefix.size(), prefix) != 0) return {};
    const std::size_t start = prefix.size();
    const std::size_t end = fileName.find('_', start);
    if (end == std::string::npos || end == start) return {};
    return fileName.substr(start, end - start);
}

std::string lritRelativePath(const std::string& fileName, LritKind kind,
                             const std::string& spacecraft, const std::string& sessionStamp) {
    const std::string stem = fileName.empty() ? std::string("unnamed.lrit") : fileName;
    std::string root = spacecraft.empty() ? std::string("UNKNOWN") : spacecraft;
    if (kind == LritKind::Image) {
        root += "/";
        root += sessionStamp.empty() ? std::string("undated") : sessionStamp;
        return root + "/" + stem;
    }
    // Everything that is not imagery is additional data, which the service files by category
    // rather than by time: the products are reference material, and a reader looking for the
    // latest schedule wants it in one place rather than in a folder per reception.
    std::string category = lritAddCategory(fileName);
    if (category.empty()) {
        switch (kind) {
            case LritKind::GtsMessage: category = "GTS"; break;
            case LritKind::Text: category = "TEXT"; break;
            case LritKind::KeyMessage: category = "KEY"; break;
            case LritKind::AlertMessage: category = "ALERT"; break;
            default: category = "OTHER"; break;
        }
    }
    return root + "/ADD/" + category + "/" + stem;
}

LritAddData parseLritAddData(const std::vector<std::uint8_t>& bytes) {
    LritAddData out;
    if (bytes.size() < 16) return out;

    out.fileTypeCode = bytes[3];
    out.totalHeaderLength = readU32(bytes, 4);
    out.dataFieldLength = readU64(bytes, 8);
    out.kind = lritFileTypeName(out.fileTypeCode);

    // Walk the records the way the reference does: each begins with a type byte and a two-byte
    // length that includes those three bytes.
    const std::size_t limit = std::min<std::size_t>(
        bytes.size(), out.totalHeaderLength > 0 ? out.totalHeaderLength : bytes.size());
    std::size_t i = 0;
    int index = 0;
    while (i + 3 <= limit) {
        LritHeaderRecord record;
        record.type = bytes[i];
        record.recordLength = readU16(bytes, i + 1);
        record.offset = static_cast<int>(i);
        if (record.recordLength < 3 || i + record.recordLength > limit) {
            // A record that claims to run past the header cannot be trusted; stop rather than
            // report fields invented from whatever follows.
            break;
        }
        if (i + 6 <= limit) record.count = readU16(bytes, i + 4);
        record.text = renderBody(bytes, i + 3, i + record.recordLength);
        record.textual = looksTextual(record.text);
        if (record.textual) extractPairs(record.text, index, out.pairs);
        out.records.push_back(record);
        i += record.recordLength;
        ++index;
    }

    out.spacecraft = findSpacecraft(out.records);

    std::ostringstream summary;
    summary << out.kind;
    if (!out.spacecraft.empty()) summary << " from " << out.spacecraft;
    if (!out.pairs.empty()) summary << ", " << out.pairs.size() << " fields";
    if (out.totalHeaderLength > 0) summary << ", header " << out.totalHeaderLength << " B";
    out.summary = summary.str();

    out.parsed = !out.records.empty();
    return out;
}

LritDataField lritDataField(const std::vector<std::uint8_t>& bytes) {
    LritDataField field;
    if (bytes.size() < 16) return field;

    std::uint32_t total = (static_cast<std::uint32_t>(bytes[4]) << 24)
                          | (static_cast<std::uint32_t>(bytes[5]) << 16)
                          | (static_cast<std::uint32_t>(bytes[6]) << 8)
                          | static_cast<std::uint32_t>(bytes[7]);
    if (total == 0 || total > bytes.size()) total = static_cast<std::uint32_t>(bytes.size());
    field.offset = total;
    if (field.offset >= bytes.size()) return field;
    field.bytes.assign(bytes.begin() + static_cast<std::ptrdiff_t>(field.offset), bytes.end());

    // A weather chart arrives as an image in the data field. JPEG and PNG are what the service
    // uses, so those two are looked for and nothing more is guessed at.
    if (field.bytes.size() > 4) {
        const std::uint8_t a = field.bytes[0], b = field.bytes[1], d = field.bytes[2],
                            e = field.bytes[3];
        // JPEG, PNG and GIF -- the three the reference checks, less its JPEG, because imagery of
        // this service is JPEG and a weather chart may be sent the same way.
        field.looksLikeImage = (a == 0xFF && b == 0xD8 && d == 0xFF) ||
                               (a == 0x89 && b == 0x50 && d == 0x4E && e == 0x47) ||
                               (a == 'G' && b == 'I' && d == 'F');
    }

    // Text, with the trailing padding dropped. The specification pads the data field to a
    // sixty-four bit boundary for the cipher, so runs of zero at the end mean nothing and are not
    // part of what the file says.
    std::size_t end = field.bytes.size();
    while (end > 0 && (field.bytes[end - 1] == 0x00 || field.bytes[end - 1] == 0x20)) --end;
    std::size_t printable = 0;
    for (std::size_t i = 0; i < end; ++i) {
        const std::uint8_t ch = field.bytes[i];
        const bool ok = ch == '\n' || ch == '\r' || ch == '\t' || (ch >= 32 && ch < 127);
        if (ok) ++printable;
    }
    field.printable = end > 0 && printable * 10 >= end * 9;   // nine in ten, so a stray byte is fine
    if (field.printable) {
        for (std::size_t i = 0; i < end; ++i) {
            const char ch = static_cast<char>(field.bytes[i]);
            field.text.push_back(ch == '\r' ? '\n' : ch);
        }
    }
    return field;
}


}  // namespace nadirstation::gk2a
