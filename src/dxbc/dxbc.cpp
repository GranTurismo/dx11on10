#include "dxbc.h"

#include <cstring>

namespace dxbc {
namespace {

const uint32_t kMd5K[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
    0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
    0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
    0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
    0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
    0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
    0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

const int kMd5R[64] = {
    7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
    5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
    4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
    6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

uint32_t Load32(const uint8_t* p) { return p[0] | p[1] << 8 | p[2] << 16 | uint32_t(p[3]) << 24; }

void Store32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v);
    p[1] = uint8_t(v >> 8);
    p[2] = uint8_t(v >> 16);
    p[3] = uint8_t(v >> 24);
}

void Md5Transform(uint32_t state[4], const uint8_t block[64]) {
    uint32_t m[16];
    for (int i = 0; i < 16; i++) m[i] = Load32(block + 4 * i);
    uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        f += a + kMd5K[i] + m[g];
        a = d;
        d = c;
        c = b;
        b += (f << kMd5R[i]) | (f >> (32 - kMd5R[i]));
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
}

void Md5Init(uint32_t state[4]) {
    state[0] = 0x67452301;
    state[1] = 0xefcdab89;
    state[2] = 0x98badcfe;
    state[3] = 0x10325476;
}

}  // namespace

void Md5(const void* data, size_t size, uint8_t out[16]) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t state[4];
    Md5Init(state);
    size_t full = size & ~size_t(63);
    for (size_t i = 0; i < full; i += 64) Md5Transform(state, p + i);
    uint8_t block[128] = {};
    size_t left = size - full;
    memcpy(block, p + full, left);
    block[left] = 0x80;
    size_t blocks = left < 56 ? 1 : 2;
    uint64_t bits = uint64_t(size) * 8;
    Store32(block + blocks * 64 - 8, uint32_t(bits));
    Store32(block + blocks * 64 - 4, uint32_t(bits >> 32));
    for (size_t i = 0; i < blocks; i++) Md5Transform(state, block + 64 * i);
    for (int i = 0; i < 4; i++) Store32(out + 4 * i, state[i]);
}

// MD5 over everything after the checksum field, with a non-standard final block:
// the bit count goes in the first dword and (bits >> 2) | 1 in the last one.
void Checksum(const uint8_t* blob, size_t size, uint32_t out[4]) {
    const uint8_t* data = blob + 20;
    uint32_t length = uint32_t(size - 20);
    uint32_t bits = length * 8;
    uint32_t full = length & ~63u;
    uint32_t left = length - full;
    uint32_t state[4];
    Md5Init(state);
    for (uint32_t i = 0; i < full; i += 64) Md5Transform(state, data + i);
    uint8_t block[64] = {};
    if (left >= 56) {
        memcpy(block, data + full, left);
        block[left] = 0x80;
        Md5Transform(state, block);
        memset(block, 0, sizeof(block));
        Store32(block, bits);
    } else {
        Store32(block, bits);
        memcpy(block + 4, data + full, left);
        block[4 + left] = 0x80;
    }
    Store32(block + 60, (bits >> 2) | 1);
    Md5Transform(state, block);
    for (int i = 0; i < 4; i++) out[i] = state[i];
}

bool ChecksumMatches(const void* blob, size_t size) {
    if (size < 32) return false;
    const uint8_t* p = static_cast<const uint8_t*>(blob);
    uint32_t sum[4];
    Checksum(p, size, sum);
    for (int i = 0; i < 4; i++)
        if (Load32(p + 4 + 4 * i) != sum[i]) return false;
    return true;
}

const Chunk* Container::Find(uint32_t fourcc) const {
    for (const Chunk& c : chunks)
        if (c.fourcc == fourcc) return &c;
    return nullptr;
}

bool Parse(const void* blob, size_t size, Container& out, std::string& error) {
    const uint8_t* p = static_cast<const uint8_t*>(blob);
    out.chunks.clear();
    if (size < 32 || Load32(p) != kDXBC) {
        error = "not a DXBC container";
        return false;
    }
    uint32_t total = Load32(p + 24);
    uint32_t count = Load32(p + 28);
    if (total > size || 32 + size_t(count) * 4 > total) {
        error = "truncated container";
        return false;
    }
    for (uint32_t i = 0; i < count; i++) {
        uint32_t offset = Load32(p + 32 + 4 * i);
        if (offset + 8 > total || Load32(p + offset + 4) > total - offset - 8) {
            error = "chunk out of bounds";
            return false;
        }
        uint32_t chunkSize = Load32(p + offset + 4);
        out.chunks.push_back({Load32(p + offset), std::vector<uint8_t>(p + offset + 8, p + offset + 8 + chunkSize)});
    }
    return true;
}

std::vector<uint8_t> Build(const Container& container) {
    size_t headerSize = 32 + 4 * container.chunks.size();
    size_t total = headerSize;
    for (const Chunk& c : container.chunks) total += 8 + ((c.data.size() + 3) & ~size_t(3));
    std::vector<uint8_t> out(total, 0);
    Store32(&out[0], kDXBC);
    Store32(&out[20], 1);
    Store32(&out[24], uint32_t(total));
    Store32(&out[28], uint32_t(container.chunks.size()));
    size_t offset = headerSize;
    for (size_t i = 0; i < container.chunks.size(); i++) {
        const Chunk& c = container.chunks[i];
        Store32(&out[32 + 4 * i], uint32_t(offset));
        Store32(&out[offset], c.fourcc);
        Store32(&out[offset + 4], uint32_t(c.data.size()));
        if (!c.data.empty()) memcpy(&out[offset + 8], c.data.data(), c.data.size());
        offset += 8 + ((c.data.size() + 3) & ~size_t(3));
    }
    uint32_t sum[4];
    Checksum(out.data(), out.size(), sum);
    for (int i = 0; i < 4; i++) Store32(&out[4 + 4 * i], sum[i]);
    return out;
}

std::string FourCCName(uint32_t fourcc) {
    std::string s;
    for (int i = 0; i < 4; i++) {
        char ch = char(fourcc >> (8 * i));
        s += (ch >= 32 && ch < 127) ? ch : '?';
    }
    return s;
}

}  // namespace dxbc
