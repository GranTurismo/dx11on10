// Shader Model 5 -> Shader Model 4 bytecode conversion.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dxbc {

enum class Stage { Pixel = 0, Vertex = 1, Geometry = 2, Hull = 3, Domain = 4, Compute = 5, Unknown = 99 };

struct ConvertResult {
    enum Status {
        Converted,    // blob holds SM4 bytecode
        AlreadySM4,   // input is already SM4 or older, use it unchanged
        Unsupported,  // uses something SM4 hardware can't do; message lists what
        Invalid,      // not parseable
    };
    Status status = Invalid;
    Stage stage = Stage::Unknown;
    int major = 0, minor = 0;  // version of the input
    std::vector<uint8_t> blob;
    std::string message;
};

// targetMinor: 0 for feature level 10_0 (vs_4_0...), 1 for 10_1 (vs_4_1...).
ConvertResult ToSM4(const void* bytecode, size_t size, int targetMinor);

const char* StageName(Stage stage);
const char* OpcodeName(uint32_t opcode);

}  // namespace dxbc
