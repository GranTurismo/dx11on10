// Pixel-shader versions of compute shaders that DX10 hardware can't run.
// Compiled as Shader Model 4.0 by tools/build_emulation_shaders.sh into emulation_shaders.h.

float4 vs_fullscreen(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

// ---- Generic compute replay (see src/dxbc/compute.h) ----
// One point per thread: the vertex shader passes the thread number on, a geometry shader
// generated from the compute shader emits one point per store, these pixel shaders write
// the stored value. The gs_signature_* shaders only provide the stage signatures the
// generated geometry shaders copy, so all stages link.

struct ThreadPoint
{
    uint id : ID;
};

ThreadPoint vs_thread(uint id : SV_VertexID)
{
    ThreadPoint p;
    p.id = id;
    return p;
}

#define STORE_TYPES(Name, Type)                                                              \
    struct Store##Name                                                                       \
    {                                                                                        \
        float4 pos : SV_Position;                                                            \
        nointerpolation Type##4 value : VALUE;                                               \
        uint slice : SV_RenderTargetArrayIndex;                                              \
    };                                                                                       \
    struct Stored##Name                                                                      \
    {                                                                                        \
        float4 pos : SV_Position;                                                            \
        nointerpolation Type##4 value : VALUE;                                               \
    };                                                                                       \
    [maxvertexcount(1)] void gs_signature_##Name(point ThreadPoint p[1], inout PointStream<Store##Name> s) \
    {                                                                                        \
        Store##Name o;                                                                       \
        o.pos = float4(asfloat(p[0].id), 0, 0, 1);                                           \
        o.value = (Type##4)0;                                                                \
        o.slice = 0;                                                                         \
        s.Append(o);                                                                         \
    }                                                                                        \
    Type##4 ps_store_##Name(Stored##Name i) : SV_Target { return i.value; }

STORE_TYPES(float, float)
STORE_TYPES(uint, uint)
STORE_TYPES(sint, int)

// Fast path: one pixel per thread. These only provide the signatures (position in,
// one typed target out) that the generated pixel shaders copy.
float4 ps_signature_float(float4 pos : SV_Position) : SV_Target { return pos; }
uint4 ps_signature_uint(float4 pos : SV_Position) : SV_Target { return uint4(pos); }
int4 ps_signature_sint(float4 pos : SV_Position) : SV_Target { return int4(pos); }
