// Verify the additional-data parser against real LRIT files.
//
// The 10 files SatDump decoded from a reference recording are the reference: their headers contain
// the records this parser exists to read, including the look-up table that makes the imagery
// interpretable. Running the parser over them shows whether the record walk lands on the real
// boundaries or invents them.
//
// Usage: NadirStation_AddProbe <file.lrit> [more.lrit ...]

#include "../src/lritadd.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QString>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> readAll(const QString& path) {
    std::ifstream in(path.toLocal8Bit().constData(), std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

const char* typeName(std::uint8_t t) {
    switch (t) {
        case 0:   return "primary";
        case 1:   return "image structure";
        case 2:   return "image navigation";
        case 3:   return "annotation";
        case 4:   return "time stamp";
        case 5:   return "ancillary text";
        case 6:   return "key";
        case 7:   return "GK-2A key header";
        case 128: return "image segmentation";
        default:  return "other";
    }
}

}  // namespace

namespace {

// The routing cannot be exercised with the material on hand: the recording contains only imagery,
// every file having type code 0, so the additional-data path has never actually run. This builds
// what the service would send -- type code 2, named ADD_<category>_... -- and asserts the
// classifier and the layout put it where the specification says.
int checkAddRouting() {
    struct Case { const char* name; std::uint8_t code; const char* category; };
    const Case cases[] = {
        {"ADD_ANT_143_20180627_030000_01.lrit", 2, "ANT"},
        {"ADD_SEA_007_20260101_000000_01.lrit", 2, "SEA"},
        {"ADD_143_20180627_030000_01.lrit", 2, "143"},      // no category field: the type
        {"mystery.lrit", 2, "TEXT"},                        // no name shape: named by its type
        {"IMG_FD_143_VI006_20180627_030000_01.lrit", 0, nullptr},
    };
    int failures = 0;
    std::cout << "=== additional-data routing ===\n";
    for (const Case& c : cases) {
        const nadirstation::gk2a::LritKind kind = nadirstation::gk2a::lritKindOf(c.code);
        const std::string rel = nadirstation::gk2a::lritRelativePath(c.name, kind, "GK-2A", "20261006_093006");
        bool ok = true;
        if (c.category == nullptr) {
            ok = kind == nadirstation::gk2a::LritKind::Image
                 && rel.find("/ADD/") == std::string::npos
                 && rel.find("GK-2A/20261006_093006/") == 0;
        } else {
            const std::string want = std::string("GK-2A/ADD/") + c.category + "/" + c.name;
            ok = kind != nadirstation::gk2a::LritKind::Image && rel == want;
        }
        std::printf("  %-42s -> %-46s %s\n", c.name, rel.c_str(), ok ? "ok" : "WRONG");
        if (!ok) ++failures;
    }
    if (failures == 0)
        std::cout << "  all additional data routes under <spacecraft>/ADD/<category>/\n";
    return failures;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const int routingFailures = checkAddRouting();
    if (argc < 2) {
        std::cout << "\nusage: NadirStation_AddProbe <file.lrit> [...]\n";
        return routingFailures == 0 ? 0 : 1;
    }
    std::cout << "\n";

    int totalPairs = 0;
    for (int a = 1; a < argc; ++a) {
        const QString path = QString::fromLocal8Bit(argv[a]);
        const std::vector<std::uint8_t> bytes = readAll(path);
        if (bytes.empty()) {
            std::cout << QFileInfo(path).fileName().toLocal8Bit().constData()
                      << ": empty or unreadable\n";
            continue;
        }
        const nadirstation::gk2a::LritAddData add = nadirstation::gk2a::parseLritAddData(bytes);
        std::cout << "=== " << QFileInfo(path).fileName().toLocal8Bit().constData()
                  << " (" << bytes.size() << " bytes) ===\n";
        std::cout << "  file type code " << static_cast<int>(add.fileTypeCode)
                  << " -> " << add.kind << "\n";
        std::cout << "  total header   " << add.totalHeaderLength << " B\n";
        std::cout << "  data field     " << add.dataFieldLength << " B\n";
        std::cout << "  spacecraft     " << (add.spacecraft.empty() ? "(none found)"
                                                                    : add.spacecraft) << "\n";

    // The data field is what the file says, as opposed to the records that describe it. Shown here
    // so the reader used by the additional-data page can be checked against a file built to the
    // specification's own example.
    {
        const nadirstation::gk2a::LritDataField field = nadirstation::gk2a::lritDataField(bytes);
        std::cout << "    data field at  " << field.offset << " B, " << field.bytes.size()
                  << " B, " << (field.printable ? "text" : (field.looksLikeImage ? "image" : "neither"))
                  << "\n";
        if (field.printable) {
            std::cout << "    ---- content ----\n";
            std::istringstream lines(field.text);
            std::string line;
            while (std::getline(lines, line)) std::cout << "      " << line << "\n";
            std::cout << "    ----------------\n";
        }
    }
        std::cout << "  summary        " << add.summary << "\n";
        std::cout << "  records        " << add.records.size() << "\n";
        for (const auto& r : add.records) {
            std::printf("    type %3d %-20s len %5u count %5zu %s\n", r.type, typeName(r.type),
                        r.recordLength, r.count, r.textual ? "text" : "binary");
            if (r.textual && !r.text.empty()) {
                const std::string shown = r.text.size() > 150 ? r.text.substr(0, 150) + "..."
                                                              : r.text;
                std::printf("        %s\n", shown.c_str());
            }
        }
        std::cout << "  key/value pairs " << add.pairs.size() << "\n";
        for (std::size_t i = 0; i < add.pairs.size() && i < 12; ++i)
            std::printf("    %-16s = %s\n", add.pairs[i].key.c_str(), add.pairs[i].value.c_str());
        if (add.pairs.size() > 12)
            std::cout << "    ... " << (add.pairs.size() - 12) << " more\n";
        totalPairs += static_cast<int>(add.pairs.size());
        std::cout << "\n";
    }
    std::cout << "total key/value pairs across all files: " << totalPairs << "\n";
    return totalPairs > 0 ? 0 : 1;
}
