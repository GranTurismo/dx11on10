// DXBC container: parsing, building and the checksum the D3D runtime validates.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxbc {

constexpr uint32_t FourCC(char a, char b, char c, char d) {
    return uint32_t(uint8_t(a)) | uint32_t(uint8_t(b)) << 8 | uint32_t(uint8_t(c)) << 16 | uint32_t(uint8_t(d)) << 24;
}

constexpr uint32_t kDXBC = FourCC('D', 'X', 'B', 'C');
constexpr uint32_t kSHDR = FourCC('S', 'H', 'D', 'R');
constexpr uint32_t kSHEX = FourCC('S', 'H', 'E', 'X');
constexpr uint32_t kISGN = FourCC('I', 'S', 'G', 'N');
constexpr uint32_t kOSGN = FourCC('O', 'S', 'G', 'N');
constexpr uint32_t kOSG5 = FourCC('O', 'S', 'G', '5');
constexpr uint32_t kPCSG = FourCC('P', 'C', 'S', 'G');
constexpr uint32_t kRDEF = FourCC('R', 'D', 'E', 'F');

struct Chunk {
    uint32_t fourcc;
    std::vector<uint8_t> data;
};

struct Container {
    std::vector<Chunk> chunks;
    const Chunk* Find(uint32_t fourcc) const;
};

bool Parse(const void* blob, size_t size, Container& out, std::string& error);
std::vector<uint8_t> Build(const Container& container);

// The runtime rejects a container whose stored checksum doesn't match this.
void Checksum(const uint8_t* blob, size_t size, uint32_t out[4]);
bool ChecksumMatches(const void* blob, size_t size);

// Plain MD5, exposed for self-tests of the shared transform.
void Md5(const void* data, size_t size, uint8_t out[16]);

std::string FourCCName(uint32_t fourcc);

}  // namespace dxbc
