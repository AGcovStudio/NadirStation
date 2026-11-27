// Decrypt a GK-2A LRIT file and write what comes out.
//
// Order matters here. If the decryption is wrong the payload looks like noise, and so does a
// payload encrypted under a different key; the two are indistinguishable by eye. So this runs the
// published DES known-answer test first and refuses to interpret anything if the cipher itself
// fails. Blaming the keys for a wrong S-box would send the next hour in the wrong direction.
//
// The key file format is from the reference's loader: a two-byte big-endian count, then that many
// ten-byte entries of a two-byte big-endian index and eight key bytes taken as they lie.
//
// Usage: NadirStation_Decrypt <file.lrit> [--keys <EncryptionKeyMessage.bin>] [--out <file.bin>]

#include "../src/des.h"
#include "../src/gk2adatalink.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QString>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

std::vector<std::uint8_t> readAll(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::vector<std::uint8_t>(std::istreambuf_iterator<char>(in),
                                     std::istreambuf_iterator<char>());
}

bool writeAll(const std::string& path, const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return out.good();
}

// FIPS PUB 46-2 known-answer test. The key and plaintext are the ones the standard's examples use
// and the ciphertext is the widely published result; a DES implementation that passes this is
// almost certainly right, and one that fails it is certainly wrong.
bool desSelfTest() {
    const std::uint8_t key[8] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
    const std::uint8_t cipher[8] = {0x3F, 0xA4, 0x0E, 0x8A, 0x98, 0x4D, 0x48, 0x15};
    const std::uint8_t plain[8] = {0x4E, 0x6F, 0x77, 0x20, 0x69, 0x73, 0x20, 0x74};

    const auto subkeys = nadirstation::crypto::desKeySchedule(key);
    std::uint8_t got[8] = {0};
    nadirstation::crypto::desEcbDecryptBlock(cipher, got, subkeys);

    const bool ok = std::memcmp(got, plain, 8) == 0;
    std::printf("  DES known-answer test: %s\n", ok ? "PASS" : "FAIL");

    // The intermediate values, so a disagreement with the reference can be located rather than
    // argued about. The Python check reads the same tables out of des.cpp and passes, so whatever
    // differs is in this file's logic.
    std::uint64_t keyValue = 0;
    for (int i = 0; i < 8; ++i) keyValue = (keyValue << 8) | key[i];
    std::printf("  trace, for comparison with the same steps elsewhere:\n");
    std::printf("    key            0x%016llX\n", static_cast<unsigned long long>(keyValue));
    std::printf("    first subkey   0x%012llX  (K1, used last in decryption)\n",
                static_cast<unsigned long long>(subkeys[15]));
    std::printf("    last  subkey   0x%012llX  (K16, used first in decryption)\n",
                static_cast<unsigned long long>(subkeys[0]));
    {
        std::uint64_t c = 0;
        for (int i = 0; i < 8; ++i) c = (c << 8) | cipher[i];
        std::printf("    ciphertext     0x%016llX\n", static_cast<unsigned long long>(c));
    }
    std::printf("    plaintext out  0x%016llX\n", [&] {
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) v = (v << 8) | got[i];
        return static_cast<unsigned long long>(v);
    }());
    std::printf("    plaintext want 0x%016llX\n", static_cast<unsigned long long>(0x4E6F772069732074ULL));
    if (!ok) {
        std::printf("    expected ");
        for (int i = 0; i < 8; ++i) std::printf("%02X", plain[i]);
        std::printf("\n    got      ");
        for (int i = 0; i < 8; ++i) std::printf("%02X", got[i]);
        std::printf("\n");
    }
    return ok;
}

std::map<int, std::array<std::uint8_t, 8>> loadKeys(const std::string& path) {
    std::map<int, std::array<std::uint8_t, 8>> keys;
    const std::vector<std::uint8_t> bytes = readAll(path);
    if (bytes.size() < 2) return keys;
    const int count = (bytes[0] << 8) | bytes[1];
    for (int i = 0; i < count; ++i) {
        const std::size_t at = 2 + static_cast<std::size_t>(i) * 10;
        if (at + 10 > bytes.size()) break;
        const int index = (bytes[at] << 8) | bytes[at + 1];
        std::array<std::uint8_t, 8> key{};
        std::memcpy(key.data(), &bytes[at + 2], 8);
        keys[index] = key;
    }
    return keys;
}

void hexdump(const std::vector<std::uint8_t>& b, std::size_t from, std::size_t count) {
    for (std::size_t i = 0; i < count && from + i < b.size(); ++i) {
        std::printf("%02X ", b[from + i]);
        if (i % 16 == 15) std::printf("\n            ");
    }
    std::printf("\n");
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    std::string path, keyPath, outPath;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        if (a == "--keys" && i + 1 < argc) keyPath = argv[++i];
        else if (a == "--out" && i + 1 < argc) outPath = argv[++i];
        else if (path.empty()) path = a;
    }
    if (path.empty()) {
        std::cout << "usage: NadirStation_Decrypt <file.lrit> [--keys <keyfile>] [--out <file.bin>]\n";
        return 2;
    }
    if (keyPath.empty())
        keyPath = "resources/gk2a/EncryptionKeyMessage.bin";

    std::cout << "=== DES ===\n";
    if (!desSelfTest()) {
        std::cerr << "\nthe cipher is wrong, so nothing below would mean anything\n";
        return 3;
    }

    const auto keys = loadKeys(keyPath);
    std::cout << "\n=== keys ===\n";
    std::cout << "  " << keyPath << "\n  " << keys.size() << " entries\n";
    if (keys.empty()) {
        std::cerr << "no keys loaded\n";
        return 3;
    }
    for (const auto& kv : keys) {
        std::printf("    index 0x%04X  key", kv.first);
        for (std::uint8_t b : kv.second) std::printf(" %02X", b);
        std::printf("\n");
        if (kv.first > 0x0068) break;      // the first few are enough to see the shape
    }

    const std::vector<std::uint8_t> file = readAll(path);
    if (file.size() < 16) {
        std::cerr << "cannot read " << path << "\n";
        return 3;
    }

    std::size_t dataOffset = 0;
    std::uint32_t keyIndex = 0;
    bool encrypted = false;
    const nadirstation::gk2a::LritPrimaryHeader primary =
        nadirstation::gk2a::parseLritPrimaryHeader(file, &dataOffset, &keyIndex, &encrypted);

    std::cout << "\n=== file ===\n";
    std::cout << "  " << QFileInfo(QString::fromStdString(path)).fileName().toStdString()
              << ", " << file.size() << " bytes\n";
    std::cout << "  type code " << static_cast<int>(primary.fileTypeCode)
              << ", header " << primary.totalHeaderLength
              << ", data " << primary.dataLength << "\n";
    std::cout << "  key index " << keyIndex << ", encrypted " << (encrypted ? "yes" : "no") << "\n";

    std::vector<std::uint8_t> payload(file.begin() + static_cast<std::ptrdiff_t>(dataOffset),
                                      file.end());
    std::cout << "  payload " << payload.size() << " bytes\n";
    std::cout << "  payload head: ";
    hexdump(payload, 0, 24);

    const auto found = keys.find(static_cast<int>(keyIndex));
    if (found == keys.end()) {
        std::cerr << "\nno key for index " << keyIndex << ", so this file cannot be read\n";
        return 4;
    }

    // Pad to a block boundary, as the reference does, because the payload length is not a multiple
    // of eight and the cipher has no notion of a partial block.
    const std::size_t pad = (8 - payload.size() % 8) % 8;
    for (std::size_t i = 0; i < pad; ++i) payload.push_back(0x00);

    std::vector<std::uint8_t> plain(payload.size(), 0);
    const auto subkeys = nadirstation::crypto::desKeySchedule(found->second.data());
    nadirstation::crypto::desEcbDecrypt(payload.data(), plain.data(), payload.size(), subkeys);

    std::cout << "\n=== decrypted ===\n";
    std::cout << "  head: ";
    hexdump(plain, 0, 32);

    // What the plaintext should look like. A GK-2A image payload is JPEG, so it begins FF D8 FF;
    // the LRIT compression header may sit ahead of that, and either way the first bytes are the
    // test of whether the key was right.
    const bool jpegAtZero = plain.size() >= 3 && plain[0] == 0xFF && plain[1] == 0xD8
                            && plain[2] == 0xFF;
    std::size_t jpegOffset = plain.size();
    for (std::size_t i = 0; i + 3 <= plain.size() && i < 256; ++i) {
        if (plain[i] == 0xFF && plain[i + 1] == 0xD8 && plain[i + 2] == 0xFF) {
            jpegOffset = i;
            break;
        }
    }
    std::cout << "  JPEG at offset 0: " << (jpegAtZero ? "yes" : "no") << "\n";
    if (jpegOffset < 256)
        std::cout << "  JPEG magic found at offset " << jpegOffset << "\n";
    else
        std::cout << "  no JPEG magic in the first 256 bytes\n";

    if (!outPath.empty()) {
        if (!writeAll(outPath, plain))
            std::cerr << "could not write " << outPath << "\n";
        else
            std::cout << "  wrote " << plain.size() << " bytes to " << outPath << "\n";
    }
    return jpegAtZero || jpegOffset < 256 ? 0 : 5;
}
