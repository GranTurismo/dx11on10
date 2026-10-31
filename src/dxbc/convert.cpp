#include "convert.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "dxbc.h"
#include "sbformat.h"

namespace dxbc {
namespace {

const char* const kOpcodeNames[] = {
#include "opnames.inc"
};

// D3D_NAME values used in signature elements.
constexpr uint32_t kNameDepth = 65;
constexpr uint32_t kNameDepthGreaterEqual = 67;
constexpr uint32_t kNameDepthLessEqual = 68;

constexpr uint32_t kControlsMask = 0x00fff800;  // opcode-specific bits [23:11]
constexpr uint32_t kSaturate = 1u << 13;

struct State {
    int targetMinor = 0;
    bool needs41 = false;
    std::set<std::string> unsupported;
    int resourceTags = 0, rcp = 0, derivatives = 0, depth = 0, streams = 0, eval = 0, droppedStreams = 0, bfi = 0;

    // Temp registers: emulations use registers after the shader's own ones.
    uint32_t tempCount = 0;
    long tempsAt = -1;            // output index of dcl_temps
    long firstInstructionAt = -1;  // output index of the first non-declaration
    uint32_t extraTemps = 0;

    void Unsupported(const std::string& what) { unsupported.insert(what); }
};

uint32_t TempOperand(bool source, uint32_t mask = 0) {
    uint32_t token = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
                     ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_TEMP) |
                     ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D) |
                     ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32);
    if (source)
        return token | ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) |
               D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE;
    return token | ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) | mask;
}

std::vector<uint32_t> Immediate(uint32_t v) {
    return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
                ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32),
            v, v, v, v};
}

// A 32-bit immediate operand with the same value in every component.
bool ScalarImmediate(const std::vector<uint32_t>& ins, std::pair<size_t, size_t> span, uint32_t& value) {
    uint32_t token = ins[span.first];
    if (DECODE_D3D10_SB_OPERAND_TYPE(token) != D3D10_SB_OPERAND_TYPE_IMMEDIATE32 || (token >> 31)) return false;
    size_t n = span.second - span.first - 1;
    if (n != 1 && n != 4) return false;
    for (size_t i = 1; i < n; i++)
        if (ins[span.first + 1 + i] != ins[span.first + 1]) return false;
    value = ins[span.first + 1];
    return true;
}

const char* OperandTypeName(uint32_t type) {
    switch (type) {
        case D3D11_SB_OPERAND_TYPE_STREAM: return "stream";
        case D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW: return "UAV";
        case D3D11_SB_OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY: return "groupshared memory";
        case D3D11_SB_OPERAND_TYPE_INPUT_COVERAGE_MASK: return "SV_Coverage input";
        case D3D11_SB_OPERAND_TYPE_INPUT_GS_INSTANCE_ID: return "GS instancing";
        case D3D11_SB_OPERAND_TYPE_CYCLE_COUNTER: return "cycle counter";
        case D3D11_SB_OPERAND_TYPE_OUTPUT_STENCIL_REF: return "SV_StencilRef";
        case D3D11_SB_OPERAND_TYPE_INNER_COVERAGE: return "SV_InnerCoverage";
        case D3D11_SB_OPERAND_TYPE_THIS_POINTER:
        case D3D11_SB_OPERAND_TYPE_FUNCTION_BODY:
        case D3D11_SB_OPERAND_TYPE_FUNCTION_TABLE:
        case D3D11_SB_OPERAND_TYPE_INTERFACE: return "interfaces";
        default: return "SM5 operand";
    }
}

// Walks one operand at toks[i], rewriting SM5-only encodings in place.
bool FixOperand(uint32_t* toks, size_t end, size_t& i, State& s) {
    if (i >= end) return false;
    uint32_t& token = toks[i++];
    bool extended = token >> 31;
    while (extended) {
        if (i >= end) return false;
        uint32_t& ext = toks[i++];
        extended = ext >> 31;
        if ((ext & D3D10_SB_EXTENDED_OPERAND_TYPE_MASK) == D3D10_SB_EXTENDED_OPERAND_MODIFIER)
            ext &= ~(D3D11_SB_OPERAND_MIN_PRECISION_MASK | D3D12_SB_OPERAND_NON_UNIFORM_MASK);
    }
    uint32_t type = DECODE_D3D10_SB_OPERAND_TYPE(token);
    switch (type) {
        case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_GREATER_EQUAL:
        case D3D11_SB_OPERAND_TYPE_OUTPUT_DEPTH_LESS_EQUAL:
            token = (token & ~D3D10_SB_OPERAND_TYPE_MASK) | ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_OUTPUT_DEPTH);
            s.depth++;
            break;
        case D3D10_SB_OPERAND_TYPE_RASTERIZER:
        case D3D10_SB_OPERAND_TYPE_OUTPUT_COVERAGE_MASK:
            s.needs41 = true;
            break;
        case D3D11_SB_OPERAND_TYPE_STREAM:
            break;  // only legal in the stream instructions, which handle it
        default:
            if (type >= D3D11_SB_OPERAND_TYPE_STREAM) s.Unsupported(OperandTypeName(type));
            break;
    }
    uint32_t components = token & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK;
    if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32) {
        i += components == D3D10_SB_OPERAND_1_COMPONENT ? 1 : components == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 0;
    } else if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64) {
        i += components == D3D10_SB_OPERAND_1_COMPONENT ? 2 : 8;
        s.Unsupported("double precision");
    }
    uint32_t dims = DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(token);
    for (uint32_t d = 0; d < dims; d++) {
        switch (DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(d, token)) {
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE32: i += 1; break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE64: i += 2; break;
            case D3D10_SB_OPERAND_INDEX_RELATIVE:
                if (!FixOperand(toks, end, i, s)) return false;
                break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE:
                i += 1;
                if (!FixOperand(toks, end, i, s)) return false;
                break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE64_PLUS_RELATIVE:
                i += 2;
                if (!FixOperand(toks, end, i, s)) return false;
                break;
            default: return false;
        }
    }
    return i <= end;
}

void Emit(std::vector<uint32_t>& out, uint32_t opcode, uint32_t controls, const std::vector<uint32_t>& exts,
          const std::vector<uint32_t>& operands) {
    uint32_t length = uint32_t(1 + exts.size() + operands.size());
    out.push_back(opcode | (controls & kControlsMask) | (length << 24) | (exts.empty() ? 0 : 0x80000000u));
    for (size_t i = 0; i < exts.size(); i++) out.push_back(exts[i] | (i + 1 < exts.size() ? 0x80000000u : 0));
    out.insert(out.end(), operands.begin(), operands.end());
}

bool IsDeclaration(uint32_t op) {
    return (op >= D3D10_SB_OPCODE_DCL_RESOURCE && op <= D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS) ||
           (op >= D3D11_SB_OPCODE_DCL_STREAM && op <= D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED) ||
           op == D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT;
}

void ConvertDeclaration(std::vector<uint32_t>& ins, State& s, std::vector<uint32_t>& out) {
    uint32_t& token = ins[0];
    uint32_t op = DECODE_D3D10_SB_OPCODE_TYPE(token);
    size_t i = 1;
    switch (op) {
        case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS: {
            uint32_t flags = DECODE_D3D10_SB_GLOBAL_FLAGS(token);
            if (flags & (D3D11_SB_GLOBAL_FLAG_ENABLE_DOUBLE_PRECISION_FLOAT_OPS | D3D11_1_SB_GLOBAL_FLAG_ENABLE_DOUBLE_EXTENSIONS))
                s.Unsupported("double precision");
            token = (token & ~D3D10_SB_GLOBAL_FLAGS_MASK) | (flags & D3D10_SB_GLOBAL_FLAG_REFACTORING_ALLOWED);
            break;
        }
        case D3D10_SB_OPCODE_DCL_INPUT_PS:
        case D3D10_SB_OPCODE_DCL_INPUT_PS_SGV:
        case D3D10_SB_OPCODE_DCL_INPUT_PS_SIV:
            if (DECODE_D3D10_SB_INPUT_INTERPOLATION_MODE(token) >= D3D10_SB_INTERPOLATION_LINEAR_SAMPLE) s.needs41 = true;
            [[fallthrough]];
        case D3D10_SB_OPCODE_DCL_INPUT:
        case D3D10_SB_OPCODE_DCL_INPUT_SGV:
        case D3D10_SB_OPCODE_DCL_INPUT_SIV:
        case D3D10_SB_OPCODE_DCL_OUTPUT:
        case D3D10_SB_OPCODE_DCL_OUTPUT_SGV:
        case D3D10_SB_OPCODE_DCL_OUTPUT_SIV:
            if (!FixOperand(ins.data(), ins.size(), i, s)) s.Unsupported("malformed declaration");
            break;
        case D3D10_SB_OPCODE_DCL_RESOURCE:
            if (DECODE_D3D10_SB_RESOURCE_DIMENSION(token) == D3D10_SB_RESOURCE_DIMENSION_TEXTURECUBEARRAY) s.needs41 = true;
            break;
        case D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE:
            if (DECODE_D3D10_SB_GS_INPUT_PRIMITIVE(token) >= D3D11_SB_PRIMITIVE_1_CONTROL_POINT_PATCH)
                s.Unsupported("GS with patch input");
            break;
        case D3D11_SB_OPCODE_DCL_STREAM:
            if (ins.size() < 3 || ins[2] != 0) s.Unsupported("multiple GS streams");
            s.streams++;
            return;  // SM4 has a single implicit stream
        case D3D10_SB_OPCODE_DCL_TEMPS:
            if (ins.size() >= 2) {
                s.tempsAt = long(out.size());
                s.tempCount = ins[1];
            }
            break;
        case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER:
        case D3D10_SB_OPCODE_DCL_SAMPLER:
        case D3D10_SB_OPCODE_DCL_INDEX_RANGE:
        case D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY:
        case D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT:
        case D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP:
            break;
        default:
            s.Unsupported(OpcodeName(op));
            break;
    }
    out.insert(out.end(), ins.begin(), ins.end());
}

void ConvertInstruction(std::vector<uint32_t>& ins, State& s, std::vector<uint32_t>& out) {
    uint32_t token = ins[0];
    uint32_t op = DECODE_D3D10_SB_OPCODE_TYPE(token);
    uint32_t controls = token & kControlsMask & ~D3D11_SB_INSTRUCTION_PRECISE_VALUES_MASK;
    size_t n = ins.size(), i = 1;
    if (s.firstInstructionAt < 0) s.firstInstructionAt = long(out.size());

    std::vector<uint32_t> exts;
    bool extended = token >> 31;
    while (extended) {
        if (i >= n) return s.Unsupported("malformed instruction");
        uint32_t ext = ins[i++];
        extended = ext >> 31;
        switch (DECODE_D3D10_SB_EXTENDED_OPCODE_TYPE(ext)) {
            case D3D10_SB_EXTENDED_OPCODE_SAMPLE_CONTROLS: exts.push_back(ext & 0x7fffffff); break;
            case D3D11_SB_EXTENDED_OPCODE_RESOURCE_DIM:
            case D3D11_SB_EXTENDED_OPCODE_RESOURCE_RETURN_TYPE: s.resourceTags++; break;
            default: s.Unsupported("extended opcode"); break;
        }
    }

    std::vector<std::pair<size_t, size_t>> spans;
    while (i < n) {
        size_t start = i;
        if (!FixOperand(ins.data(), n, i, s)) return s.Unsupported("malformed operand");
        spans.push_back({start, i});
    }
    auto operands = [&](std::initializer_list<int> which) {
        std::vector<uint32_t> v;
        for (int k : which)
            if (k < int(spans.size())) v.insert(v.end(), ins.begin() + spans[k].first, ins.begin() + spans[k].second);
        return v;
    };
    std::vector<uint32_t> all(ins.begin() + (spans.empty() ? n : spans[0].first), ins.end());

    if (op < D3D10_SB_OPCODE_RESERVED0) return Emit(out, op, controls, exts, all);

    switch (op) {
        case D3D10_1_SB_OPCODE_LOD:
        case D3D10_1_SB_OPCODE_GATHER4:
        case D3D10_1_SB_OPCODE_SAMPLE_POS:
        case D3D10_1_SB_OPCODE_SAMPLE_INFO:
            s.needs41 = true;
            return Emit(out, op, controls, exts, all);

        case D3D11_SB_OPCODE_RCP: {
            // rcp dst, src -> div dst, l(1, 1, 1, 1), src
            std::vector<uint32_t> v = operands({0});
            uint32_t one = 0x3f800000;
            v.insert(v.end(), {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
                                   ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32),
                               one, one, one, one});
            std::vector<uint32_t> src = operands({1});
            v.insert(v.end(), src.begin(), src.end());
            s.rcp++;
            return Emit(out, D3D10_SB_OPCODE_DIV, controls, exts, v);
        }

        case D3D11_SB_OPCODE_DERIV_RTX_COARSE:
        case D3D11_SB_OPCODE_DERIV_RTX_FINE:
            s.derivatives++;
            return Emit(out, D3D10_SB_OPCODE_DERIV_RTX, controls, exts, all);
        case D3D11_SB_OPCODE_DERIV_RTY_COARSE:
        case D3D11_SB_OPCODE_DERIV_RTY_FINE:
            s.derivatives++;
            return Emit(out, D3D10_SB_OPCODE_DERIV_RTY, controls, exts, all);

        case D3D11_SB_OPCODE_EMIT_STREAM:
        case D3D11_SB_OPCODE_CUT_STREAM:
        case D3D11_SB_OPCODE_EMITTHENCUT_STREAM: {
            if (spans.size() != 1 || spans[0].second - spans[0].first != 2) return s.Unsupported("stream instruction");
            if (ins[spans[0].first + 1] != 0) {
                s.droppedStreams++;
                return;
            }
            uint32_t sm4 = op == D3D11_SB_OPCODE_EMIT_STREAM ? D3D10_SB_OPCODE_EMIT
                         : op == D3D11_SB_OPCODE_CUT_STREAM  ? D3D10_SB_OPCODE_CUT
                                                             : D3D10_SB_OPCODE_EMITTHENCUT;
            return Emit(out, sm4, 0, {}, {});
        }

        case D3D11_SB_OPCODE_BFI: {
            // bfi dst, width, offset, insert, base:
            //   mask = ((1 << width) - 1) << offset;  dst = ((insert << offset) & mask) | (base & ~mask)
            if (spans.size() != 5) return s.Unsupported("malformed bfi");
            uint32_t writeMask = ins[spans[0].first] & D3D10_SB_OPERAND_4_COMPONENT_MASK_MASK;
            uint32_t t0 = s.tempCount, t1 = s.tempCount + 1;
            s.extraTemps = std::max(s.extraTemps, 2u);
            s.bfi++;
            auto dst = [&](uint32_t reg) { return std::vector<uint32_t>{TempOperand(false, writeMask), reg}; };
            auto src = [&](uint32_t reg) { return std::vector<uint32_t>{TempOperand(true), reg}; };
            auto cat = [](std::initializer_list<std::vector<uint32_t>> parts) {
                std::vector<uint32_t> v;
                for (const auto& p : parts) v.insert(v.end(), p.begin(), p.end());
                return v;
            };
            uint32_t width, offset;
            if (ScalarImmediate(ins, spans[1], width) && ScalarImmediate(ins, spans[2], offset)) {
                width &= 31;
                offset &= 31;
                uint32_t mask = uint32_t((1ull << width) - 1) << offset;
                Emit(out, D3D10_SB_OPCODE_ISHL, 0, {}, cat({dst(t0), operands({3}), Immediate(offset)}));
                Emit(out, D3D10_SB_OPCODE_AND, 0, {}, cat({dst(t0), src(t0), Immediate(mask)}));
                Emit(out, D3D10_SB_OPCODE_AND, 0, {}, cat({dst(t1), operands({4}), Immediate(~mask)}));
            } else {
                Emit(out, D3D10_SB_OPCODE_ISHL, 0, {}, cat({dst(t1), Immediate(1), operands({1})}));
                Emit(out, D3D10_SB_OPCODE_IADD, 0, {}, cat({dst(t1), src(t1), Immediate(0xffffffffu)}));
                Emit(out, D3D10_SB_OPCODE_ISHL, 0, {}, cat({dst(t1), src(t1), operands({2})}));
                Emit(out, D3D10_SB_OPCODE_ISHL, 0, {}, cat({dst(t0), operands({3}), operands({2})}));
                Emit(out, D3D10_SB_OPCODE_AND, 0, {}, cat({dst(t0), src(t0), src(t1)}));
                Emit(out, D3D10_SB_OPCODE_NOT, 0, {}, cat({dst(t1), src(t1)}));
                Emit(out, D3D10_SB_OPCODE_AND, 0, {}, cat({dst(t1), operands({4}), src(t1)}));
            }
            return Emit(out, D3D10_SB_OPCODE_OR, 0, {}, cat({operands({0}), src(t0), src(t1)}));
        }

        case D3D11_SB_OPCODE_EVAL_CENTROID:
        case D3D11_SB_OPCODE_EVAL_SAMPLE_INDEX:
        case D3D11_SB_OPCODE_EVAL_SNAPPED:
            // Approximation: use the value interpolated at the pixel's usual position.
            s.eval++;
            return Emit(out, D3D10_SB_OPCODE_MOV, controls & kSaturate, {}, operands({0, 1}));

        default:
            return s.Unsupported(OpcodeName(op));
    }
}

uint32_t Load32(const std::vector<uint8_t>& d, size_t at) {
    return d[at] | d[at + 1] << 8 | d[at + 2] << 16 | uint32_t(d[at + 3]) << 24;
}

void Append32(std::vector<uint8_t>& d, uint32_t v) {
    for (int i = 0; i < 4; i++) d.push_back(uint8_t(v >> (8 * i)));
}

// Rewrites a signature chunk (ISGN/OSGN, or OSG5 with stream indices) as an SM4 one.
bool ConvertSignature(const std::vector<uint8_t>& in, bool hasStream, std::vector<uint8_t>& out, State& s) {
    if (in.size() < 8) return false;
    uint32_t count = Load32(in, 0), offset = Load32(in, 4);
    size_t stride = hasStream ? 28 : 24;
    if (offset + count * stride > in.size()) return false;
    struct Element {
        std::string name;
        uint32_t index, systemValue, componentType, reg, masks;
    };
    std::vector<Element> elements;
    for (uint32_t e = 0; e < count; e++) {
        size_t at = offset + e * stride;
        if (hasStream) {
            if (Load32(in, at) != 0) continue;
            at += 4;
        }
        uint32_t nameOffset = Load32(in, at);
        if (nameOffset >= in.size()) return false;
        const char* name = reinterpret_cast<const char*>(in.data() + nameOffset);
        Element el{std::string(name, strnlen(name, in.size() - nameOffset)), Load32(in, at + 4), Load32(in, at + 8),
                   Load32(in, at + 12), Load32(in, at + 16), Load32(in, at + 20)};
        std::string lower;
        for (char ch : el.name) lower += char(ch >= 'A' && ch <= 'Z' ? ch + 32 : ch);
        if (el.systemValue == kNameDepthGreaterEqual || el.systemValue == kNameDepthLessEqual ||
            lower == "sv_depthgreaterequal" || lower == "sv_depthlessequal") {
            if (el.systemValue) el.systemValue = kNameDepth;
            el.name = "SV_Depth";
        }
        elements.push_back(el);
    }
    out.clear();
    Append32(out, uint32_t(elements.size()));
    Append32(out, 8);
    size_t strings = 8 + elements.size() * 24;
    std::string table;
    for (const Element& el : elements) {
        Append32(out, uint32_t(strings + table.size()));
        Append32(out, el.index);
        Append32(out, el.systemValue);
        Append32(out, el.componentType);
        Append32(out, el.reg);
        Append32(out, el.masks);
        table += el.name;
        table += '\0';
    }
    out.insert(out.end(), table.begin(), table.end());
    while (out.size() % 4) out.push_back(0xAB);  // fxc pads string tables with 0xAB
    (void)s;
    return true;
}

}  // namespace

const char* OpcodeName(uint32_t opcode) {
    return opcode < sizeof(kOpcodeNames) / sizeof(kOpcodeNames[0]) ? kOpcodeNames[opcode] : "unknown";
}

const char* StageName(Stage stage) {
    switch (stage) {
        case Stage::Pixel: return "PS";
        case Stage::Vertex: return "VS";
        case Stage::Geometry: return "GS";
        case Stage::Hull: return "HS";
        case Stage::Domain: return "DS";
        case Stage::Compute: return "CS";
        default: return "??";
    }
}

ConvertResult ToSM4(const void* bytecode, size_t size, int targetMinor) {
    ConvertResult r;
    Container in;
    if (!Parse(bytecode, size, in, r.message)) return r;
    const Chunk* code = in.Find(kSHEX);
    if (!code) code = in.Find(kSHDR);
    if (!code || code->data.size() < 8) {
        r.message = "no shader code chunk";
        return r;
    }
    const std::vector<uint8_t>& raw = code->data;
    std::vector<uint32_t> program(raw.size() / 4);
    memcpy(program.data(), raw.data(), program.size() * 4);
    uint32_t version = program[0];
    r.stage = Stage(DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(version));
    r.major = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MAJOR_VERSION(version);
    r.minor = DECODE_D3D10_SB_TOKENIZED_PROGRAM_MINOR_VERSION(version);
    if (r.major < 5) {
        r.status = ConvertResult::AlreadySM4;
        return r;
    }
    if (r.stage == Stage::Hull || r.stage == Stage::Domain) {
        r.status = ConvertResult::Unsupported;
        r.message = "tessellation needs DX11 hardware";
        return r;
    }
    if (r.stage == Stage::Compute) {
        r.status = ConvertResult::Unsupported;
        r.message = "compute shader 5.0";
        return r;
    }
    if (r.stage != Stage::Pixel && r.stage != Stage::Vertex && r.stage != Stage::Geometry) {
        r.message = "unknown shader type";
        return r;
    }
    size_t length = program[1];
    if (length > program.size() || length < 2) {
        r.message = "bad program length";
        return r;
    }

    State s;
    s.targetMinor = targetMinor;
    std::vector<uint32_t> out = {0, 0};
    for (size_t pos = 2; pos < length;) {
        uint32_t token = program[pos];
        uint32_t op = DECODE_D3D10_SB_OPCODE_TYPE(token);
        size_t n;
        if (op == D3D10_SB_OPCODE_CUSTOMDATA) {
            n = pos + 1 < length ? program[pos + 1] : 0;
            if (n < 2 || pos + n > length) {
                r.message = "bad customdata block";
                return r;
            }
            out.insert(out.end(), program.begin() + pos, program.begin() + pos + n);
            pos += n;
            continue;
        }
        n = DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(token);
        if (n == 0 || pos + n > length) {
            r.message = std::string("bad instruction length at ") + OpcodeName(op);
            return r;
        }
        std::vector<uint32_t> ins(program.begin() + pos, program.begin() + pos + n);
        pos += n;
        if (IsDeclaration(op))
            ConvertDeclaration(ins, s, out);
        else
            ConvertInstruction(ins, s, out);
    }

    if (s.needs41 && targetMinor < 1) s.Unsupported("shader model 4.1 features on feature level 10_0");
    if (!s.unsupported.empty()) {
        r.status = ConvertResult::Unsupported;
        for (const std::string& u : s.unsupported) r.message += (r.message.empty() ? "" : ", ") + u;
        return r;
    }

    if (s.extraTemps) {
        uint32_t total = s.tempCount + s.extraTemps;
        if (s.tempsAt >= 0) {
            out[s.tempsAt + 1] = total;
        } else {
            size_t at = s.firstInstructionAt >= 0 ? size_t(s.firstInstructionAt) : out.size();
            out.insert(out.begin() + at, {ENCODE_D3D10_SB_OPCODE_TYPE(D3D10_SB_OPCODE_DCL_TEMPS) |
                                              ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(2),
                                          total});
        }
    }
    out[0] = ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(uint32_t(r.stage), 4, uint32_t(targetMinor));
    out[1] = uint32_t(out.size());

    Container result;
    const Chunk* isgn = in.Find(kISGN);
    const Chunk* osgn = in.Find(kOSGN);
    const Chunk* osg5 = in.Find(kOSG5);
    if (isgn) result.chunks.push_back(*isgn);
    if (osgn || osg5) {
        Chunk c{kOSGN, {}};
        if (!ConvertSignature(osgn ? osgn->data : osg5->data, !osgn, c.data, s)) {
            r.message = "bad output signature";
            return r;
        }
        result.chunks.push_back(c);
    }
    Chunk shdr{kSHDR, std::vector<uint8_t>(out.size() * 4)};
    memcpy(shdr.data.data(), out.data(), shdr.data.size());
    result.chunks.push_back(shdr);
    r.blob = Build(result);
    r.status = ConvertResult::Converted;

    std::string notes;
    auto note = [&](int count, const char* what) {
        if (count) notes += (notes.empty() ? "" : ", ") + std::to_string(count) + " " + what;
    };
    note(s.resourceTags, "resource tags stripped");
    note(s.rcp, "rcp->div");
    note(s.derivatives, "coarse/fine derivatives");
    note(s.depth, "conservative depth->SV_Depth");
    note(s.streams, "stream decls dropped");
    note(s.droppedStreams, "non-zero stream emits dropped");
    note(s.eval, "eval_* approximated");
    note(s.bfi, "bfi emulated");
    r.message = notes;
    return r;
}

}  // namespace dxbc
