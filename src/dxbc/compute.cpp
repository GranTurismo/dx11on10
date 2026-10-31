#include "compute.h"

#include <algorithm>
#include <cstring>
#include <map>
#include <set>

#include "convert.h"
#include "dxbc.h"
#include "sbformat.h"

namespace dxbc {
namespace {

using Tokens = std::vector<uint32_t>;

struct Instr {
    Tokens t;
    uint32_t op = 0;
    bool decl = false;
    std::vector<std::pair<size_t, size_t>> spans;  // operands of non-declarations, as [begin, end) in t
};

bool IsDecl(uint32_t op) {
    return (op >= D3D10_SB_OPCODE_DCL_RESOURCE && op <= D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS) ||
           (op >= D3D11_SB_OPCODE_DCL_STREAM && op <= D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED) ||
           op == D3D11_SB_OPCODE_DCL_GS_INSTANCE_COUNT || op == D3D10_SB_OPCODE_CUSTOMDATA;
}

// Index just past the operand at i, or 0 when malformed.
size_t SkipOperand(const uint32_t* t, size_t end, size_t i) {
    if (i >= end) return 0;
    uint32_t tok = t[i++];
    bool ext = tok >> 31;
    while (ext) {
        if (i >= end) return 0;
        ext = t[i++] >> 31;
    }
    uint32_t type = DECODE_D3D10_SB_OPERAND_TYPE(tok);
    uint32_t comps = tok & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK;
    if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
        i += comps == D3D10_SB_OPERAND_1_COMPONENT ? 1 : comps == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 0;
    else if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64)
        i += comps == D3D10_SB_OPERAND_1_COMPONENT ? 2 : 8;
    for (uint32_t d = 0; d < DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(tok); d++) {
        switch (DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(d, tok)) {
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE32: i += 1; break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE64: i += 2; break;
            case D3D10_SB_OPERAND_INDEX_RELATIVE: i = SkipOperand(t, end, i); break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE: i = SkipOperand(t, end, i + 1); break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE64_PLUS_RELATIVE: i = SkipOperand(t, end, i + 2); break;
            default: return 0;
        }
        if (!i) return 0;
    }
    return i <= end ? i : 0;
}

bool ParseProgram(const Tokens& prog, std::vector<Instr>& out, std::string& error) {
    size_t length = prog[1];
    if (length > prog.size()) {
        error = "bad program length";
        return false;
    }
    for (size_t pos = 2; pos < length;) {
        uint32_t tok = prog[pos];
        uint32_t op = DECODE_D3D10_SB_OPCODE_TYPE(tok);
        size_t n = op == D3D10_SB_OPCODE_CUSTOMDATA ? (pos + 1 < length ? prog[pos + 1] : 0)
                                                     : DECODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(tok);
        if (!n || pos + n > length) {
            error = std::string("bad instruction length at ") + OpcodeName(op);
            return false;
        }
        Instr ins;
        ins.t.assign(prog.begin() + pos, prog.begin() + pos + n);
        ins.op = op;
        ins.decl = IsDecl(op);
        if (!ins.decl) {
            size_t i = 1;
            bool ext = tok >> 31;
            while (ext && i < n) ext = ins.t[i++] >> 31;
            while (i < n) {
                size_t next = SkipOperand(ins.t.data(), n, i);
                if (!next) {
                    error = std::string("bad operand in ") + OpcodeName(op);
                    return false;
                }
                ins.spans.push_back({i, next});
                i = next;
            }
        }
        out.push_back(std::move(ins));
        pos += n;
    }
    return true;
}

uint32_t TypeOf(const uint32_t* operand) { return DECODE_D3D10_SB_OPERAND_TYPE(operand[0]); }

// Register number of a simple operand (r5, u1, g0, t3...): the first immediate index.
uint32_t IndexOf(const uint32_t* operand) {
    size_t i = 1;
    bool ext = operand[0] >> 31;
    while (ext) ext = operand[i++] >> 31;
    return operand[i];
}

Tokens Span(const Instr& ins, size_t k) {
    return Tokens(ins.t.begin() + ins.spans[k].first, ins.t.begin() + ins.spans[k].second);
}

// ---- operand builders ----

constexpr uint32_t kSwizzleXYZW = D3D10_SB_OPERAND_4_COMPONENT_NOSWIZZLE;

uint32_t Swizzle(int x, int y, int z, int w) { return ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE(x, y, z, w); }

Tokens Register(uint32_t type, uint32_t index, uint32_t selection) {
    return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | selection |
                ENCODE_D3D10_SB_OPERAND_TYPE(type) | ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D) |
                ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32),
            index};
}

uint32_t Mask(uint32_t mask) {
    return ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_MASK_MODE) | (mask << 4);
}
uint32_t Swz(uint32_t swizzle) {
    return ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) | swizzle;
}
uint32_t Sel(int comp) {
    return ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) |
           ENCODE_D3D10_SB_OPERAND_4_COMPONENT_SELECT_1(comp);
}

Tokens TempDst(uint32_t r, uint32_t mask) { return Register(D3D10_SB_OPERAND_TYPE_TEMP, r, Mask(mask)); }
Tokens TempSrc(uint32_t r, uint32_t swizzle = kSwizzleXYZW) { return Register(D3D10_SB_OPERAND_TYPE_TEMP, r, Swz(swizzle)); }
Tokens TempSel(uint32_t r, int comp) { return Register(D3D10_SB_OPERAND_TYPE_TEMP, r, Sel(comp)); }
Tokens OutDst(uint32_t o, uint32_t mask) { return Register(D3D10_SB_OPERAND_TYPE_OUTPUT, o, Mask(mask)); }

Tokens Imm(uint32_t x, uint32_t y, uint32_t z, uint32_t w) {
    return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) |
                ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_IMMEDIATE32),
            x, y, z, w};
}
Tokens Imm(uint32_t v) { return Imm(v, v, v, v); }
uint32_t F(float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    return u;
}

// cb<slot>[row] with a swizzle.
Tokens Cb(uint32_t slot, uint32_t row, uint32_t selection) {
    return {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | selection |
                ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER) |
                ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_2D) |
                ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32) |
                ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, D3D10_SB_OPERAND_INDEX_IMMEDIATE32),
            slot, row};
}

// The same operand reduced to one of its (swizzled) components.
Tokens Component(Tokens op, int comp) {
    uint32_t& tok = op[0];
    if (DECODE_D3D10_SB_OPERAND_TYPE(tok) == D3D10_SB_OPERAND_TYPE_IMMEDIATE32 && !(tok >> 31)) {
        if ((tok & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK) == D3D10_SB_OPERAND_4_COMPONENT) return Imm(op[1 + comp]);
        return Imm(op[1]);
    }
    if ((tok & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK) != D3D10_SB_OPERAND_4_COMPONENT) return op;
    uint32_t mode = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(tok);
    int source = comp;
    if (mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) source = (tok >> (4 + 2 * comp)) & 3;
    else if (mode == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) source = (tok >> 4) & 3;
    tok = (tok & ~0xffcu) | Sel(source);
    return op;
}

// The same operand with its swizzle composed with `swizzle` (component k reads the
// operand's component swizzle[k]).
Tokens Reswizzle(Tokens op, const int swizzle[4]) {
    uint32_t& tok = op[0];
    if ((tok & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK) != D3D10_SB_OPERAND_4_COMPONENT) return op;
    uint32_t mode = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(tok);
    int src[4];
    for (int k = 0; k < 4; k++) {
        if (mode == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE) src[k] = (tok >> (4 + 2 * swizzle[k])) & 3;
        else if (mode == D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE) src[k] = (tok >> 4) & 3;
        else src[k] = swizzle[k];
    }
    tok = (tok & ~0xffcu) | Swz(Swizzle(src[0], src[1], src[2], src[3]));
    return op;
}

void Emit(Tokens& out, uint32_t op, std::initializer_list<Tokens> operands, uint32_t controls = 0) {
    size_t length = 1;
    for (const Tokens& o : operands) length += o.size();
    out.push_back(ENCODE_D3D10_SB_OPCODE_TYPE(op) | controls | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(length));
    for (const Tokens& o : operands) out.insert(out.end(), o.begin(), o.end());
}

// ---- operand rewriting ----

struct Rewrite {
    uint32_t ids[4];          // temps holding vThreadID, vThreadGroupID, vThreadIDInGroup, vThreadIDInGroupFlattened
    uint32_t tempOffset = 0;  // added to r# (copies of the shader's start, see InlineProducer)
};

int ThreadInput(uint32_t type) {
    switch (type) {
        case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID: return 0;
        case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_GROUP_ID: return 1;
        case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP: return 2;
        case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED: return 3;
        default: return -1;
    }
}

void RewriteOperand(const uint32_t* t, size_t& i, const Rewrite& rw, Tokens& out) {
    uint32_t tok = t[i++];
    Tokens exts;
    for (bool ext = tok >> 31; ext; ext = t[i - 1] >> 31) exts.push_back(t[i++]);
    uint32_t type = DECODE_D3D10_SB_OPERAND_TYPE(tok);
    int id = ThreadInput(type);
    if (id >= 0) {
        uint32_t selection = tok & 0xfff;
        if ((tok & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK) != D3D10_SB_OPERAND_4_COMPONENT)
            selection = ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | Sel(0);
        out.push_back(selection | ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_TEMP) |
                      ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D) |
                      ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32) |
                      (tok & 0x80000000u));
        out.insert(out.end(), exts.begin(), exts.end());
        out.push_back(rw.ids[id]);
        return;
    }
    out.push_back(tok);
    out.insert(out.end(), exts.begin(), exts.end());
    uint32_t comps = tok & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK;
    size_t values = 0;
    if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
        values = comps == D3D10_SB_OPERAND_1_COMPONENT ? 1 : comps == D3D10_SB_OPERAND_4_COMPONENT ? 4 : 0;
    else if (type == D3D10_SB_OPERAND_TYPE_IMMEDIATE64)
        values = comps == D3D10_SB_OPERAND_1_COMPONENT ? 2 : 8;
    out.insert(out.end(), t + i, t + i + values);
    i += values;
    for (uint32_t d = 0; d < DECODE_D3D10_SB_OPERAND_INDEX_DIMENSION(tok); d++) {
        switch (DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(d, tok)) {
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE32:
                out.push_back(t[i++] + (type == D3D10_SB_OPERAND_TYPE_TEMP && d == 0 ? rw.tempOffset : 0));
                break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE64:
                out.push_back(t[i++]);
                out.push_back(t[i++]);
                break;
            case D3D10_SB_OPERAND_INDEX_RELATIVE: RewriteOperand(t, i, rw, out); break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE32_PLUS_RELATIVE:
                out.push_back(t[i++]);
                RewriteOperand(t, i, rw, out);
                break;
            case D3D10_SB_OPERAND_INDEX_IMMEDIATE64_PLUS_RELATIVE:
                out.push_back(t[i++]);
                out.push_back(t[i++]);
                RewriteOperand(t, i, rw, out);
                break;
        }
    }
}

Tokens RewriteSpan(const Instr& ins, size_t k, const Rewrite& rw) {
    Tokens out;
    size_t i = ins.spans[k].first;
    RewriteOperand(ins.t.data(), i, rw, out);
    return out;
}

// Copies an instruction with its operands rewritten.
void CopyInstruction(const Instr& ins, const Rewrite& rw, Tokens& out) {
    Tokens body(ins.t.begin() + 1, ins.t.begin() + (ins.spans.empty() ? ins.t.size() : ins.spans[0].first));
    for (size_t k = 0; k < ins.spans.size(); k++) {
        Tokens o = RewriteSpan(ins, k, rw);
        body.insert(body.end(), o.begin(), o.end());
    }
    uint32_t tok = ins.t[0];
    tok = (tok & ~D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH_MASK) | ENCODE_D3D10_SB_TOKENIZED_INSTRUCTION_LENGTH(body.size() + 1);
    out.push_back(tok);
    out.insert(out.end(), body.begin(), body.end());
}

bool IsAtomic(uint32_t op) {
    return (op >= D3D11_SB_OPCODE_ATOMIC_AND && op <= D3D11_SB_OPCODE_IMM_ATOMIC_UMIN);
}

struct Analysis {
    uint32_t temps = 0;
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> uavs;  // slot -> (dimension, return type token)
    std::map<uint32_t, uint32_t> tgsmStride;                  // structured g# -> stride
    std::set<uint32_t> cbSlots;
    uint32_t srvSlots = 0, samplerSlots = 0;
    std::vector<size_t> code;  // indices of non-declarations
    long sync = -1;            // index into `code` of the single sync, if any
    std::map<uint32_t, unsigned> stores;  // UAV slot -> number of store instructions
    std::set<uint32_t> storesInLoop;
    std::set<uint32_t> reads;  // UAV slots loaded from
    std::vector<size_t> sharedStores, sharedLoads;  // indices into `code`
};

std::string Analyze(const std::vector<Instr>& prog, ComputeProgram& r, Analysis& a) {
    for (size_t k = 0; k < prog.size(); k++) {
        const Instr& ins = prog[k];
        const Tokens& t = ins.t;
        if (!ins.decl) {
            a.code.push_back(k);
            continue;
        }
        switch (ins.op) {
            case D3D11_SB_OPCODE_DCL_THREAD_GROUP:
                r.threads[0] = t[1], r.threads[1] = t[2], r.threads[2] = t[3];
                break;
            case D3D10_SB_OPCODE_DCL_TEMPS: a.temps = t[1]; break;
            case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_TYPED:
                a.uavs[IndexOf(&t[1])] = {DECODE_D3D10_SB_RESOURCE_DIMENSION(t[0]), t.back()};
                break;
            case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_STRUCTURED: a.tgsmStride[IndexOf(&t[1])] = t[t.size() - 2]; break;
            case D3D11_SB_OPCODE_DCL_THREAD_GROUP_SHARED_MEMORY_RAW: return "raw groupshared memory";
            case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_RAW:
            case D3D11_SB_OPCODE_DCL_UNORDERED_ACCESS_VIEW_STRUCTURED: return "raw/structured UAVs";
            case D3D11_SB_OPCODE_DCL_RESOURCE_RAW:
            case D3D11_SB_OPCODE_DCL_RESOURCE_STRUCTURED: return "raw/structured buffer inputs";
            case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER: a.cbSlots.insert(IndexOf(&t[1])); break;
            case D3D10_SB_OPCODE_DCL_SAMPLER: a.samplerSlots = std::max(a.samplerSlots, IndexOf(&t[1]) + 1); break;
            case D3D10_SB_OPCODE_DCL_RESOURCE: a.srvSlots = std::max(a.srvSlots, IndexOf(&t[1]) + 1); break;
            case D3D10_SB_OPCODE_DCL_INPUT:
            case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS:
            case D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP:
            case D3D10_SB_OPCODE_CUSTOMDATA:
                break;
            default: return std::string("declaration ") + OpcodeName(ins.op);
        }
    }

    int depth = 0, loops = 0;
    for (size_t c = 0; c < a.code.size(); c++) {
        const Instr& ins = prog[a.code[c]];
        uint32_t op = ins.op;
        if (op == D3D10_SB_OPCODE_IF || op == D3D10_SB_OPCODE_SWITCH) depth++;
        if (op == D3D10_SB_OPCODE_LOOP) depth++, loops++;
        if (op == D3D10_SB_OPCODE_ENDIF || op == D3D10_SB_OPCODE_ENDSWITCH) depth--;
        if (op == D3D10_SB_OPCODE_ENDLOOP) depth--, loops--;
        if (IsAtomic(op)) return "atomics";
        if (op == D3D11_SB_OPCODE_SYNC) {
            if (a.sync >= 0) return "more than one barrier";
            if (depth) return "barrier inside flow control";
            a.sync = long(c);
            continue;
        }
        if (op == D3D11_SB_OPCODE_STORE_UAV_TYPED) {
            uint32_t slot = IndexOf(&ins.t[ins.spans[0].first]);
            a.stores[slot]++;
            if (loops) a.storesInLoop.insert(slot);
        }
        if (op == D3D11_SB_OPCODE_LD_UAV_TYPED) a.reads.insert(IndexOf(&ins.t[ins.spans[2].first]));
        if (op == D3D11_SB_OPCODE_STORE_STRUCTURED || op == D3D11_SB_OPCODE_STORE_RAW) {
            if (TypeOf(&ins.t[ins.spans[0].first]) != D3D11_SB_OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY)
                return "raw/structured UAV writes";
            if (op == D3D11_SB_OPCODE_STORE_RAW) return "raw groupshared memory";
            a.sharedStores.push_back(c);
        }
        if (op == D3D11_SB_OPCODE_LD_STRUCTURED || op == D3D11_SB_OPCODE_LD_RAW) {
            const uint32_t* res = &ins.t[ins.spans.back().first];
            if (TypeOf(res) == D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW) return "raw/structured UAV reads";
            if (TypeOf(res) == D3D11_SB_OPERAND_TYPE_THREAD_GROUP_SHARED_MEMORY) {
                if (op == D3D11_SB_OPCODE_LD_RAW) return "raw groupshared memory";
                a.sharedLoads.push_back(c);
            }
        }
        if (op == D3D11_SB_OPCODE_EMIT_STREAM || op == D3D11_SB_OPCODE_INTERFACE_CALL) return OpcodeName(op);
    }
    if (a.stores.empty()) return "no typed UAV writes";
    for (const auto& s : a.stores)
        if (!a.uavs.count(s.first)) return "store to an undeclared UAV";

    // Shared memory: only "every thread stores, barrier, threads read" with each thread
    // storing at its own index, so a read can recompute what the other thread stored.
    if (!a.sharedStores.empty() || !a.sharedLoads.empty()) {
        if (a.sync < 0) return "groupshared memory without a barrier";
        for (size_t c : a.sharedStores) {
            if (long(c) > a.sync) return "groupshared stores after the barrier";
            const Instr& ins = prog[a.code[c]];
            const uint32_t* index = &ins.t[ins.spans[1].first];
            uint32_t type = TypeOf(index);
            // The index must be the thread's own flattened id, or the one component of its
            // id in the group along which the group extends (then the two are the same).
            Tokens one = Component(Span(ins, 1), 0);
            int comp = (one[0] & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK) != D3D10_SB_OPERAND_4_COMPONENT
                           ? 0
                           : int((one[0] >> 4) & 3);
            bool alongComp = comp < 3;
            for (int k = 0; k < 3; k++)
                if (k != comp && r.threads[k] != 1) alongComp = false;
            bool own = type == D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP_FLATTENED ||
                       (type == D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID_IN_GROUP && alongComp);
            if (!own) return "groupshared store at another thread's index";
            if (TypeOf(&ins.t[ins.spans[2].first]) != D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
                return "groupshared store at a computed offset";
        }
        for (size_t c : a.sharedLoads) {
            if (long(c) < a.sync) return "groupshared loads before the barrier";
            if (TypeOf(&prog[a.code[c]].t[prog[a.code[c]].spans[2].first]) != D3D10_SB_OPERAND_TYPE_IMMEDIATE32)
                return "groupshared load at a computed offset";
        }
        for (long c = 0; c < a.sync; c++) {
            uint32_t op = prog[a.code[c]].op;
            if (op == D3D10_SB_OPCODE_RET || op == D3D10_SB_OPCODE_RETC || op == D3D10_SB_OPCODE_DISCARD ||
                op == D3D11_SB_OPCODE_STORE_UAV_TYPED || op == D3D11_SB_OPCODE_LD_UAV_TYPED)
                return "side effects before the barrier";
            for (const auto& span : prog[a.code[c]].spans)
                if (TypeOf(&prog[a.code[c]].t[span.first]) == D3D10_SB_OPERAND_TYPE_INDEXABLE_TEMP)
                    return "indexable temps before the barrier";
        }
    }
    return "";
}

// Which component of the register a source operand's logical component `comp` reads.
int SourceComponent(uint32_t tok, int comp) {
    if ((tok & D3D10_SB_OPERAND_NUM_COMPONENTS_MASK) != D3D10_SB_OPERAND_4_COMPONENT) return 0;
    switch (DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(tok)) {
        case D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE: return (tok >> (4 + 2 * comp)) & 3;
        case D3D10_SB_OPERAND_4_COMPONENT_SELECT_1_MODE: return (tok >> 4) & 3;
        default: return comp;
    }
}

// A value per thread of the form "thread id component + values equal for all threads".
struct Affine {
    int tid = -2;               // -2: unknown, -1: the same for every thread, 0/1: vThreadID.x/.y plus terms
    std::vector<Tokens> terms;  // scalar operands added (immediates, constant buffer values)
};

Affine AffineOf(const Tokens& op, int comp, const std::map<uint32_t, Affine>& regs) {
    uint32_t tok = op[0];
    if (tok >> 31) return {};  // operand modifiers
    int src = SourceComponent(tok, comp);
    switch (DECODE_D3D10_SB_OPERAND_TYPE(tok)) {
        case D3D10_SB_OPERAND_TYPE_IMMEDIATE32: return {-1, {Component(op, comp)}};
        case D3D10_SB_OPERAND_TYPE_CONSTANT_BUFFER:
            if (DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, tok) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32 ||
                DECODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, tok) != D3D10_SB_OPERAND_INDEX_IMMEDIATE32)
                return {};
            return {-1, {Component(op, comp)}};
        case D3D11_SB_OPERAND_TYPE_INPUT_THREAD_ID:
            if (src < 2) return {src, {}};
            return {};
        case D3D10_SB_OPERAND_TYPE_TEMP: {
            auto it = regs.find((IndexOf(op.data()) << 2) | uint32_t(src));
            return it == regs.end() ? Affine{} : it->second;
        }
        default: return {};
    }
}

bool WritesFirstOperandOnly(uint32_t op) {
    switch (op) {
        case D3D10_SB_OPCODE_IF:
        case D3D10_SB_OPCODE_BREAKC:
        case D3D10_SB_OPCODE_CONTINUEC:
        case D3D10_SB_OPCODE_DISCARD:
        case D3D10_SB_OPCODE_RETC:
        case D3D10_SB_OPCODE_SWITCH:
        case D3D10_SB_OPCODE_CASE:
        case D3D10_SB_OPCODE_CALLC: return false;  // the first operand is read
        default: return true;
    }
}

// True when the one store to `slot` writes texel (thread id.xy + offsets) of a 2D texture:
// then thread (pixel - offsets) can run as a full-screen pixel shader, one pixel each.
bool DenseOffsets(const std::vector<Instr>& prog, const Analysis& a, const ComputeProgram& r, uint32_t slot,
                  std::vector<Tokens> offsets[2]) {
    if (a.stores.at(slot) != 1 || a.storesInLoop.count(slot) || r.threads[2] != 1) return false;
    if (a.uavs.at(slot).first != D3D10_SB_RESOURCE_DIMENSION_TEXTURE2D) return false;
    // Values are followed into branches; after a branch, what it wrote is unknown (it may
    // not have run). Inside loops everything written is unknown.
    struct Frame {
        std::map<uint32_t, Affine> before;
        std::set<uint32_t> written;
        bool loop;
    };
    std::vector<Frame> frames;
    std::map<uint32_t, Affine> regs;
    for (size_t c : a.code) {
        const Instr& ins = prog[c];
        uint32_t op = ins.op;
        if (op == D3D11_SB_OPCODE_STORE_UAV_TYPED && IndexOf(&ins.t[ins.spans[0].first]) == slot) {
            Tokens address = Span(ins, 1);
            for (int k = 0; k < 2; k++) {
                Affine f = AffineOf(address, k, regs);
                if (f.tid != k) return false;
                offsets[k] = f.terms;
            }
            return true;
        }
        if (op == D3D10_SB_OPCODE_IF || op == D3D10_SB_OPCODE_LOOP || op == D3D10_SB_OPCODE_SWITCH)
            frames.push_back({regs, {}, op == D3D10_SB_OPCODE_LOOP});
        if ((op == D3D10_SB_OPCODE_ELSE || op == D3D10_SB_OPCODE_CASE || op == D3D10_SB_OPCODE_DEFAULT) && !frames.empty())
            regs = frames.back().before;
        if ((op == D3D10_SB_OPCODE_ENDIF || op == D3D10_SB_OPCODE_ENDLOOP || op == D3D10_SB_OPCODE_ENDSWITCH) &&
            !frames.empty()) {
            Frame f = std::move(frames.back());
            frames.pop_back();
            regs = std::move(f.before);
            for (uint32_t w : f.written) regs[w] = Affine{};
            if (!frames.empty()) frames.back().written.insert(f.written.begin(), f.written.end());
        }
        bool inLoop = false;
        for (const Frame& f : frames) inLoop |= f.loop;
        if (ins.spans.empty() || !WritesFirstOperandOnly(op)) continue;
        bool twoResults = op == D3D10_SB_OPCODE_UDIV || op == D3D10_SB_OPCODE_UMUL || op == D3D10_SB_OPCODE_IMUL ||
                          op == D3D11_SB_OPCODE_UADDC || op == D3D11_SB_OPCODE_USUBB || op == D3D11_SB_OPCODE_SWAPC ||
                          op == D3D10_SB_OPCODE_SINCOS;
        for (size_t d = 0; d < (twoResults ? 2u : 1u) && d < ins.spans.size(); d++) {
            Tokens dst = Span(ins, d);
            if (DECODE_D3D10_SB_OPERAND_TYPE(dst[0]) != D3D10_SB_OPERAND_TYPE_TEMP) continue;
            uint32_t reg = IndexOf(dst.data());
            uint32_t mask = (dst[0] & D3D10_SB_OPERAND_4_COMPONENT_MASK_MASK) >> 4;
            for (int k = 0; k < 4; k++) {
                if (!(mask & (1 << k))) continue;
                Affine v;
                if (!inLoop && !twoResults && op == D3D10_SB_OPCODE_MOV) {
                    v = AffineOf(Span(ins, 1), k, regs);
                } else if (!inLoop && op == D3D10_SB_OPCODE_IADD) {
                    Affine x = AffineOf(Span(ins, 1), k, regs), y = AffineOf(Span(ins, 2), k, regs);
                    if (x.tid >= -1 && y.tid >= -1 && (x.tid < 0 || y.tid < 0)) {
                        v.tid = std::max(x.tid, y.tid);
                        v.terms = x.terms;
                        v.terms.insert(v.terms.end(), y.terms.begin(), y.terms.end());
                    }
                }
                regs[(reg << 2) | uint32_t(k)] = v;
                if (!frames.empty()) frames.back().written.insert((reg << 2) | uint32_t(k));
            }
        }
    }
    return false;
}

struct Builder {
    const std::vector<Instr>& prog;
    const ComputeProgram& r;
    const Analysis& a;
    uint32_t slot, dim;  // the UAV this pass replays
    uint32_t N, ids, scratch, nextTemp;
    Tokens body;

    // Thread ids from the point's index: linear -> group, local, global.
    void Prologue() {
        uint32_t tx = r.threads[0], ty = r.threads[1], tz = r.threads[2];
        uint32_t T0 = ids, T1 = ids + 1, T2 = ids + 2, T3 = ids + 3;
        Tokens v = {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | Sel(0) |
                        ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_INPUT) |
                        ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_2D) |
                        ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32) |
                        ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, D3D10_SB_OPERAND_INDEX_IMMEDIATE32),
                    0, 0};
        Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(T3, 1), v});
        Emit(body, D3D10_SB_OPCODE_UDIV, {TempDst(T1, 8), TempDst(T3, 1), TempSel(T3, 0), Imm(tx * ty * tz)});
        LocalIds(T3, T2);
        Emit(body, D3D10_SB_OPCODE_UDIV, {TempDst(T0, 8), TempDst(T1, 1), TempSel(T1, 3), Cb(r.dispatchSlot, 0, Sel(0))});
        Emit(body, D3D10_SB_OPCODE_UDIV, {TempDst(T1, 4), TempDst(T1, 2), TempSel(T0, 3), Cb(r.dispatchSlot, 0, Sel(1))});
        Emit(body, D3D10_SB_OPCODE_IMAD, {TempDst(T0, 7), TempSrc(T1, Swizzle(0, 1, 2, 0)), Imm(tx, ty, tz, 0),
                                          TempSrc(T2, Swizzle(0, 1, 2, 0))});
    }

    // Fast path: thread ids from the pixel: id = pixel - offsets; pixels past the launched
    // threads are discarded.
    void PixelPrologue(const std::vector<Tokens> offsets[2]) {
        uint32_t tx = r.threads[0], ty = r.threads[1];
        uint32_t T0 = ids, T1 = ids + 1, T2 = ids + 2, T3 = ids + 3, S0 = scratch, S1 = scratch + 1;
        // Read only the declared components (.xy): some drivers reject reading undeclared ones.
        Emit(body, D3D10_SB_OPCODE_FTOU, {TempDst(T0, 3), Register(D3D10_SB_OPERAND_TYPE_INPUT, 0, Swz(Swizzle(0, 1, 0, 0)))});
        for (int k = 0; k < 2; k++)
            for (const Tokens& term : offsets[k]) {
                Emit(body, D3D10_SB_OPCODE_INEG, {TempDst(S0, 1u << k), term});
                Emit(body, D3D10_SB_OPCODE_IADD, {TempDst(T0, 1u << k), TempSel(T0, k), TempSel(S0, k)});
            }
        // threads launched = groups * group size (imad: plain instructions are safest on old drivers)
        Emit(body, D3D10_SB_OPCODE_IMAD, {TempDst(S1, 3), Cb(r.dispatchSlot, 0, Swz(kSwizzleXYZW)), Imm(tx, ty, 0, 0), Imm(0)});
        Emit(body, D3D10_SB_OPCODE_UGE, {TempDst(S1, 3), TempSrc(T0), TempSrc(S1)});
        Emit(body, D3D10_SB_OPCODE_OR, {TempDst(S1, 1), TempSel(S1, 0), TempSel(S1, 1)});
        Emit(body, D3D10_SB_OPCODE_DISCARD, {TempSel(S1, 0)},
             ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(D3D10_SB_INSTRUCTION_TEST_NONZERO));
        Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(T0, 4), Imm(0)});
        Emit(body, D3D10_SB_OPCODE_UDIV, {TempDst(T1, 3), TempDst(T2, 3), TempSrc(T0), Imm(tx, ty, 1, 1)});
        Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(T1, 4), Imm(0)});
        Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(T2, 4), Imm(0)});
        Emit(body, D3D10_SB_OPCODE_IMAD, {TempDst(T3, 1), TempSel(T2, 1), Imm(tx), TempSel(T2, 0)});
        Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(S0, 8), Imm(0)});  // S0.w: the thread has written
    }

    // local.xyz from a flattened index in flat.x (uses local.w as scratch).
    void LocalIds(uint32_t flat, uint32_t local) {
        Emit(body, D3D10_SB_OPCODE_UDIV, {TempDst(local, 8), TempDst(local, 1), TempSel(flat, 0), Imm(r.threads[0])});
        Emit(body, D3D10_SB_OPCODE_UDIV, {TempDst(local, 4), TempDst(local, 2), TempSel(local, 3), Imm(r.threads[1])});
    }

    // store_uav_typed u#, address, value -> one point at the texel.
    void Store(const Instr& ins, const Rewrite& rw) {
        Tokens address = RewriteSpan(ins, 1, rw), value = RewriteSpan(ins, 2, rw);
        uint32_t S0 = scratch;
        bool oneD = dim == D3D10_SB_RESOURCE_DIMENSION_BUFFER || dim == D3D10_SB_RESOURCE_DIMENSION_TEXTURE1D ||
                    dim == D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY;
        int sliceComp = dim == D3D10_SB_RESOURCE_DIMENSION_TEXTURE1DARRAY ? 1
                        : (dim == D3D10_SB_RESOURCE_DIMENSION_TEXTURE2DARRAY || dim == D3D10_SB_RESOURCE_DIMENSION_TEXTURE3D) ? 2
                                                                                                                       : -1;
        if (oneD) {
            Emit(body, D3D10_SB_OPCODE_UTOF, {TempDst(S0, 1), Component(address, 0)});
            Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(S0, 2), Imm(0)});
        } else {
            Emit(body, D3D10_SB_OPCODE_UTOF, {TempDst(S0, 3), address});
        }
        Emit(body, D3D10_SB_OPCODE_ADD, {TempDst(S0, 3), TempSrc(S0), Imm(F(0.5f))});
        Emit(body, D3D10_SB_OPCODE_MAD, {OutDst(0, 3), TempSrc(S0), Cb(r.dispatchSlot, 1, Swz(Swizzle(0, 1, 0, 1))),
                                         Cb(r.dispatchSlot, 1, Swz(Swizzle(2, 3, 2, 3)))});
        Emit(body, D3D10_SB_OPCODE_MOV, {OutDst(0, 12), Imm(0, 0, 0, F(1.0f))});
        Emit(body, D3D10_SB_OPCODE_MOV, {OutDst(1, 15), value});
        if (sliceComp >= 0) {
            Emit(body, D3D10_SB_OPCODE_MOV, {OutDst(2, 1), Component(address, sliceComp)});
            // Slices past the end are dropped like out-of-range UAV writes.
            Emit(body, D3D10_SB_OPCODE_ULT, {TempDst(S0, 4), Component(address, sliceComp), Cb(r.dispatchSlot, 2, Sel(0))});
            Emit(body, D3D10_SB_OPCODE_IF,
                 {TempSel(S0, 2)}, ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(D3D10_SB_INSTRUCTION_TEST_NONZERO));
            Emit(body, D3D10_SB_OPCODE_EMIT, {});
            Emit(body, D3D10_SB_OPCODE_ENDIF, {});
        } else {
            Emit(body, D3D10_SB_OPCODE_MOV, {OutDst(2, 1), Imm(0)});
            Emit(body, D3D10_SB_OPCODE_EMIT, {});
        }
    }

    // ld_uav_typed dst, address, u# -> ld from a snapshot of the UAV's texture.
    void Load(const Instr& ins, const Rewrite& rw, uint32_t srv) {
        uint32_t S1 = scratch + 1;
        Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(S1, 15), RewriteSpan(ins, 1, rw)});
        uint32_t u = IndexOf(&ins.t[ins.spans[2].first]);
        if (a.uavs.at(u).first != D3D10_SB_RESOURCE_DIMENSION_BUFFER)
            Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(S1, 8), Imm(0)});
        Tokens resource = Span(ins, 2);
        resource[0] = (resource[0] & ~D3D10_SB_OPERAND_TYPE_MASK) | ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_RESOURCE);
        resource.back() = srv;
        Emit(body, D3D10_SB_OPCODE_LD, {RewriteSpan(ins, 0, rw), TempSrc(S1), resource});
    }

    // resinfo / bufinfo on a UAV -> the size we put in the dispatch constants.
    void Size(const Instr& ins, const Rewrite& rw) {
        Tokens dst = RewriteSpan(ins, 0, rw);
        Tokens resource = Span(ins, ins.spans.size() - 1);
        uint32_t swizzle = resource[0] & D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MASK;
        Tokens size = Cb(r.dispatchSlot, 3, Swz(ins.op == D3D11_SB_OPCODE_BUFINFO ? Swizzle(0, 0, 0, 0) : swizzle));
        bool asUint = ins.op == D3D11_SB_OPCODE_BUFINFO ||
                      DECODE_D3D10_SB_RESINFO_INSTRUCTION_RETURN_TYPE(ins.t[0]) == D3D10_SB_RESINFO_INSTRUCTION_RETURN_UINT;
        Emit(body, asUint ? D3D10_SB_OPCODE_MOV : D3D10_SB_OPCODE_UTOF, {dst, size});
    }

    // ld_structured from groupshared memory -> rerun the start of the shader for the
    // thread that stored the element, and take what it stored.
    void InlineProducer(const Instr& ins, const Rewrite& rw, unsigned blockNo, unsigned captures) {
        uint32_t base = nextTemp + blockNo * (N + 4 + captures);
        uint32_t P0 = base + N, P1 = P0 + 1, P2 = P0 + 2, P3 = P0 + 3, C = P0 + 4;
        Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(P3, 1), Component(RewriteSpan(ins, 1, rw), 0)});
        LocalIds(P3, P2);
        Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(P1, 7), TempSrc(ids + 1)});
        Emit(body, D3D10_SB_OPCODE_IMAD, {TempDst(P0, 7), TempSrc(P1, Swizzle(0, 1, 2, 0)),
                                          Imm(r.threads[0], r.threads[1], r.threads[2], 0), TempSrc(P2, Swizzle(0, 1, 2, 0))});
        Rewrite producer = {{P0, P1, P2, P3}, base};
        unsigned captureNo = 0;
        struct Captured {
            uint32_t g, offset, mask, reg;
        };
        std::vector<Captured> captured;
        for (long c = 0; c < a.sync; c++) {
            const Instr& p = prog[a.code[c]];
            if (p.op == D3D11_SB_OPCODE_STORE_STRUCTURED) {
                const uint32_t* g = &p.t[p.spans[0].first];
                uint32_t mask = (g[0] & D3D10_SB_OPERAND_4_COMPONENT_MASK_MASK) >> 4;
                uint32_t reg = C + captureNo++;
                Emit(body, D3D10_SB_OPCODE_MOV, {TempDst(reg, mask), RewriteSpan(p, 3, producer)});
                captured.push_back({IndexOf(g), p.t[p.spans[2].first + 1], mask, reg});
                continue;
            }
            CopyInstruction(p, producer, body);
        }
        // Destination component k reads dword offset/4 + swizzle[k] of the element.
        const uint32_t* g = &ins.t[ins.spans[3].first];
        uint32_t gIndex = IndexOf(g), offset = ins.t[ins.spans[2].first + 1];
        Tokens dst = RewriteSpan(ins, 0, rw);
        uint32_t dstMask = (dst[0] & D3D10_SB_OPERAND_4_COMPONENT_MASK_MASK) >> 4;
        for (const Captured& cap : captured) {
            if (cap.g != gIndex) continue;
            uint32_t mask = 0;
            int swizzle[4] = {0, 0, 0, 0};
            for (int k = 0; k < 4; k++) {
                if (!(dstMask & (1 << k))) continue;
                int sel = DECODE_D3D10_SB_OPERAND_4_COMPONENT_SELECTION_MODE(g[0]) == D3D10_SB_OPERAND_4_COMPONENT_SWIZZLE_MODE
                              ? (g[0] >> (4 + 2 * k)) & 3
                              : (g[0] >> 4) & 3;
                int dword = int(offset / 4) + sel - int(cap.offset / 4);
                if (dword < 0 || dword > 3 || !(cap.mask & (1 << dword))) continue;
                mask |= 1 << k;
                swizzle[k] = dword;
            }
            if (!mask) continue;
            Tokens part = dst;
            part[0] = (part[0] & ~D3D10_SB_OPERAND_4_COMPONENT_MASK_MASK) | (mask << 4);
            Emit(body, D3D10_SB_OPCODE_MOV, {part, Reswizzle(TempSrc(cap.reg), swizzle)});
        }
    }
};

}  // namespace

ComputeProgram ComputeToGS(const void* code, size_t size, int targetMinor, const std::vector<uint8_t> signatures[3],
                           const std::vector<uint8_t> psSignatures[3]) {
    ComputeProgram r;
    Container in;
    if (!Parse(code, size, in, r.message)) return r;
    const Chunk* shex = in.Find(kSHEX);
    if (!shex || shex->data.size() < 8) {
        r.message = "no shader code";
        return r;
    }
    Tokens program(shex->data.size() / 4);
    memcpy(program.data(), shex->data.data(), program.size() * 4);
    if (DECODE_D3D10_SB_TOKENIZED_PROGRAM_TYPE(program[0]) != D3D11_SB_COMPUTE_SHADER) {
        r.message = "not a compute shader";
        return r;
    }
    std::vector<Instr> prog;
    if (!ParseProgram(program, prog, r.message)) return r;
    Analysis a;
    r.message = Analyze(prog, r, a);
    if (!r.message.empty()) return r;

    r.srvSlots = a.srvSlots;
    r.samplerSlots = a.samplerSlots;
    r.cbSlots = a.cbSlots.empty() ? 0 : *a.cbSlots.rbegin() + 1;
    // The lowest free slot: some drivers mishandle high constant buffer slots in pixel shaders.
    r.dispatchSlot = 0;
    while (a.cbSlots.count(r.dispatchSlot) && r.dispatchSlot < 13) r.dispatchSlot++;
    if (a.cbSlots.count(r.dispatchSlot)) {
        r.message = "no free constant buffer slot";
        return r;
    }
    uint32_t nextSrv = a.srvSlots;
    std::map<uint32_t, uint32_t> snapshot;
    for (uint32_t u : a.reads) {
        snapshot[u] = nextSrv;
        r.uavReads.push_back({u, nextSrv++});
    }
    if (nextSrv > 128) {
        r.message = "no free texture slot for a UAV read";
        return r;
    }

    unsigned captures = unsigned(a.sharedStores.size());
    unsigned blocks = unsigned(a.sharedLoads.size());
    uint32_t N = a.temps;
    uint32_t totalTemps = N + 6 + blocks * (N + 4 + captures);
    if (totalTemps > 4096) {
        r.message = "too many registers after recomputing groupshared reads";
        return r;
    }

    for (const auto& store : a.stores) {
        uint32_t slot = store.first;
        uint32_t dim = a.uavs.at(slot).first, returnType = a.uavs.at(slot).second;
        ComputePass pass;
        pass.uavSlot = slot;
        pass.dimension = dim;
        uint32_t component = DECODE_D3D10_SB_RESOURCE_RETURN_TYPE(returnType, D3D10_SB_4_COMPONENT_X);
        pass.type = component == D3D10_SB_RETURN_TYPE_UINT ? ValueType::Uint
                    : component == D3D10_SB_RETURN_TYPE_SINT ? ValueType::Sint
                                                             : ValueType::Float;
        // Three outputs (position, value, slice) of up to 9 scalars: at most 113 points.
        pass.maxPoints = a.storesInLoop.count(slot) ? 100 : std::min(100u, store.second);
        std::vector<Tokens> offsets[2];
        bool dense = DenseOffsets(prog, a, r, slot, offsets);

        for (int pixel = 0; pixel < (dense ? 2 : 1); pixel++) {
            Builder b{prog, r, a, slot, dim, N, N, N + 4, N + 6, {}};
            Tokens& out = b.body;
            out = {ENCODE_D3D10_SB_TOKENIZED_PROGRAM_VERSION_TOKEN(pixel ? D3D10_SB_PIXEL_SHADER : D3D10_SB_GEOMETRY_SHADER, 5, 0),
                   0};
            // Declarations: the shader's own resources, then the new stage's.
            for (const Instr& ins : prog) {
                if (!ins.decl) continue;
                switch (ins.op) {
                    case D3D10_SB_OPCODE_DCL_GLOBAL_FLAGS:
                    case D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER:
                    case D3D10_SB_OPCODE_DCL_SAMPLER:
                    case D3D10_SB_OPCODE_DCL_RESOURCE:
                    case D3D10_SB_OPCODE_DCL_INDEXABLE_TEMP:
                    case D3D10_SB_OPCODE_CUSTOMDATA:
                        out.insert(out.end(), ins.t.begin(), ins.t.end());
                        break;
                    default: break;
                }
            }
            Emit(out, D3D10_SB_OPCODE_DCL_CONSTANT_BUFFER, {Cb(r.dispatchSlot, 4, Swz(kSwizzleXYZW))});
            for (const auto& sn : snapshot) {
                const auto& uav = a.uavs.at(sn.first);
                Tokens t = {ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_0_COMPONENT) |
                                ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_RESOURCE) |
                                ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_1D) |
                                ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32),
                            sn.second, uav.second};
                Emit(out, D3D10_SB_OPCODE_DCL_RESOURCE, {t}, ENCODE_D3D10_SB_RESOURCE_DIMENSION(uav.first));
            }
            if (pixel) {
                Emit(out, D3D10_SB_OPCODE_DCL_INPUT_PS_SIV,
                     {Register(D3D10_SB_OPERAND_TYPE_INPUT, 0, Mask(3)), {ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)}},
                     ENCODE_D3D10_SB_INPUT_INTERPOLATION_MODE(D3D10_SB_INTERPOLATION_LINEAR_NOPERSPECTIVE));
                Emit(out, D3D10_SB_OPCODE_DCL_OUTPUT, {OutDst(0, 15)});
                Emit(out, D3D10_SB_OPCODE_DCL_TEMPS, {{totalTemps}});
                b.PixelPrologue(offsets);
            } else {
                Emit(out, D3D10_SB_OPCODE_DCL_GS_INPUT_PRIMITIVE, {}, ENCODE_D3D10_SB_GS_INPUT_PRIMITIVE(D3D10_SB_PRIMITIVE_POINT));
                Emit(out, D3D10_SB_OPCODE_DCL_GS_OUTPUT_PRIMITIVE_TOPOLOGY, {},
                     ENCODE_D3D10_SB_GS_OUTPUT_PRIMITIVE_TOPOLOGY(D3D10_SB_PRIMITIVE_TOPOLOGY_POINTLIST));
                Emit(out, D3D10_SB_OPCODE_DCL_INPUT,
                     {{ENCODE_D3D10_SB_OPERAND_NUM_COMPONENTS(D3D10_SB_OPERAND_4_COMPONENT) | Mask(1) |
                           ENCODE_D3D10_SB_OPERAND_TYPE(D3D10_SB_OPERAND_TYPE_INPUT) |
                           ENCODE_D3D10_SB_OPERAND_INDEX_DIMENSION(D3D10_SB_OPERAND_INDEX_2D) |
                           ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(0, D3D10_SB_OPERAND_INDEX_IMMEDIATE32) |
                           ENCODE_D3D10_SB_OPERAND_INDEX_REPRESENTATION(1, D3D10_SB_OPERAND_INDEX_IMMEDIATE32),
                       1, 0}});
                Emit(out, D3D10_SB_OPCODE_DCL_TEMPS, {{totalTemps}});
                Emit(out, D3D10_SB_OPCODE_DCL_OUTPUT_SIV, {OutDst(0, 15), {ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_POSITION)}});
                Emit(out, D3D10_SB_OPCODE_DCL_OUTPUT, {OutDst(1, 15)});
                Emit(out, D3D10_SB_OPCODE_DCL_OUTPUT_SIV,
                     {OutDst(2, 1), {ENCODE_D3D10_SB_NAME(D3D10_SB_NAME_RENDER_TARGET_ARRAY_INDEX)}});
                Emit(out, D3D10_SB_OPCODE_DCL_MAX_OUTPUT_VERTEX_COUNT, {{pass.maxPoints}});
                b.Prologue();
            }

            Rewrite main = {{N, N + 1, N + 2, N + 3}, 0};
            unsigned blockNo = 0;
            for (size_t c = 0; c < a.code.size(); c++) {
                const Instr& ins = prog[a.code[c]];
                switch (ins.op) {
                    case D3D11_SB_OPCODE_SYNC: continue;
                    case D3D11_SB_OPCODE_STORE_STRUCTURED: continue;  // groupshared: replaced by recomputation
                    case D3D11_SB_OPCODE_LD_STRUCTURED: b.InlineProducer(ins, main, blockNo++, captures); continue;
                    case D3D11_SB_OPCODE_STORE_UAV_TYPED:
                        if (IndexOf(&ins.t[ins.spans[0].first]) != slot) continue;
                        if (pixel) {
                            // The thread's one write is this pixel's color.
                            Emit(out, D3D10_SB_OPCODE_MOV, {OutDst(0, 15), RewriteSpan(ins, 2, main)});
                            Emit(out, D3D10_SB_OPCODE_MOV, {TempDst(N + 4, 8), Imm(~0u)});
                        } else {
                            b.Store(ins, main);
                        }
                        continue;
                    case D3D11_SB_OPCODE_LD_UAV_TYPED:
                        b.Load(ins, main, snapshot.at(IndexOf(&ins.t[ins.spans[2].first])));
                        continue;
                    case D3D10_SB_OPCODE_RESINFO:
                    case D3D11_SB_OPCODE_BUFINFO:
                        if (TypeOf(&ins.t[ins.spans.back().first]) == D3D11_SB_OPERAND_TYPE_UNORDERED_ACCESS_VIEW) {
                            b.Size(ins, main);
                            continue;
                        }
                        break;
                    case D3D10_SB_OPCODE_RET:
                        // A pixel whose thread ends without writing keeps the texel unchanged.
                        if (pixel) Emit(out, D3D10_SB_OPCODE_DISCARD, {TempSel(N + 4, 3)},
                                        ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(D3D10_SB_INSTRUCTION_TEST_ZERO));
                        break;
                    case D3D10_SB_OPCODE_RETC:
                        if (pixel) {
                            Emit(out, D3D10_SB_OPCODE_IF, {RewriteSpan(ins, 0, main)},
                                 ins.t[0] & D3D10_SB_INSTRUCTION_TEST_BOOLEAN_MASK);
                            Emit(out, D3D10_SB_OPCODE_DISCARD, {TempSel(N + 4, 3)},
                                 ENCODE_D3D10_SB_INSTRUCTION_TEST_BOOLEAN(D3D10_SB_INSTRUCTION_TEST_ZERO));
                            Emit(out, D3D10_SB_OPCODE_ENDIF, {});
                        }
                        break;
                    default: break;
                }
                CopyInstruction(ins, main, out);
            }
            out[1] = uint32_t(out.size());

            Container stage;
            Container sig;
            std::string error;
            const std::vector<uint8_t>& signature = (pixel ? psSignatures : signatures)[int(pass.type)];
            if (!Parse(signature.data(), signature.size(), sig, error) || !sig.Find(kISGN) || !sig.Find(kOSGN)) {
                r.message = "bad signature template";
                return r;
            }
            stage.chunks.push_back(*sig.Find(kISGN));
            stage.chunks.push_back(*sig.Find(kOSGN));
            Chunk shdr{kSHEX, std::vector<uint8_t>(out.size() * 4)};
            memcpy(shdr.data.data(), out.data(), shdr.data.size());
            stage.chunks.push_back(shdr);
            std::vector<uint8_t> sm5 = Build(stage);
            ConvertResult converted = ToSM4(sm5.data(), sm5.size(), targetMinor);
            if (converted.status != ConvertResult::Converted) {
                if (pixel) break;  // the geometry shader path still works
                r.message = "geometry shader for u" + std::to_string(slot) + ": " + converted.message;
                return r;
            }
            (pixel ? pass.ps : pass.gs) = std::move(converted.blob);
        }
        r.passes.push_back(std::move(pass));
    }
    r.ok = true;
    unsigned fast = 0;
    for (const ComputePass& p : r.passes) fast += !p.ps.empty();
    r.message = std::to_string(r.passes.size()) + " pass(es), " + std::to_string(fast) + " as full-screen pixel shaders" +
                (a.sharedLoads.empty() ? "" : ", groupshared reads recomputed");
    return r;
}

}  // namespace dxbc
