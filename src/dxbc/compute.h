// Compute shader -> geometry shader rewriting, so compute work runs on hardware without
// compute shaders. Each thread becomes one point drawn through a geometry shader that
// runs the thread's code and emits one point per UAV store, at the stored texel, with
// the stored value. A pixel shader writes that value into a render target view of the
// texture the UAV pointed at.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace dxbc {

enum class ValueType { Float = 0, Uint = 1, Sint = 2 };

// One draw replaying the stores to one UAV slot.
struct ComputePass {
    unsigned uavSlot = 0;
    unsigned dimension = 0;  // D3D10_SB_RESOURCE_DIMENSION of the UAV
    ValueType type = ValueType::Float;
    unsigned maxPoints = 0;   // points a thread may emit
    std::vector<uint8_t> gs;  // Shader Model 4 geometry shader
    // Fast path, when the pass's one store writes texel "thread id.xy + values equal for all
    // threads" of a 2D texture: a pixel shader run over the texture, one pixel per thread.
    std::vector<uint8_t> ps;
};

struct ComputeProgram {
    bool ok = false;
    std::string message;              // why not, or what was done
    unsigned threads[3] = {1, 1, 1};  // thread group size
    unsigned dispatchSlot = 0;        // constant buffer slot holding our dispatch constants
    std::vector<std::pair<unsigned, unsigned>> uavReads;  // UAV slot read -> SRV slot of its snapshot
    unsigned srvSlots = 0, cbSlots = 0, samplerSlots = 0;  // highest slot the shader uses, plus one
    std::vector<ComputePass> passes;
};

// Layout of the dispatch constant buffer (16-byte rows):
//   [0] uint  groups x, y, z
//   [1] float 2 / width, -2 / height, -1, 1   (texel -> clip space)
//   [2] uint  slices (array size or depth)
//   [3] uint  width, height, slices, 1        (what GetDimensions returns)
constexpr unsigned kDispatchConstantsSize = 64;

// signatures[type]: a gs_4_0 container whose ISGN/OSGN chunks the passes reuse
// (thread id in, position + value + render target array index out); psSignatures[type]:
// the same for the pixel shader fast path (position in, one typed target out).
ComputeProgram ComputeToGS(const void* code, size_t size, int targetMinor, const std::vector<uint8_t> signatures[3],
                           const std::vector<uint8_t> psSignatures[3]);

}  // namespace dxbc
