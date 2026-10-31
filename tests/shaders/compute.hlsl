// Compute shaders for testing the compute -> geometry shader replay (tests/computetest).
// Each writes deterministic results that are compared bit for bit against native DX11.

cbuffer Params : register(b0)
{
    uint4 offsets;  // xy: source offset, zw: region size
    uint4 target;   // xy: destination offset
    float4 scale;
};

Texture2D<float4> source : register(t0);
SamplerState linearClamp : register(s0);

// Skyrim SE's region copy pattern.
RWTexture2D<float4> copyOut : register(u0);
[numthreads(8, 8, 1)]
void cs_copy(uint3 id : SV_DispatchThreadID)
{
    if (all(id.xy < offsets.zw)) copyOut[id.xy + target.xy] = source.Load(int3(id.xy + offsets.xy, 0));
}

// Per-pixel math with a filtered sample.
RWTexture2D<float4> mathOut : register(u0);
[numthreads(8, 8, 1)]
void cs_math(uint3 id : SV_DispatchThreadID)
{
    float2 uv = (id.xy + 0.5) / 64.0;
    float4 s = source.SampleLevel(linearClamp, uv * scale.xy, 0);
    mathOut[id.xy] = float4(uv, s.r * s.g + 0.25, dot(s.rgb, float3(0.3, 0.6, 0.1)));
}

// Two stores per thread.
RWTexture2D<float4> pairOut : register(u0);
[numthreads(8, 4, 1)]
void cs_two_stores(uint3 id : SV_DispatchThreadID)
{
    pairOut[uint2(id.x * 2, id.y)] = float4(id.x / 64.0, 0, 1, 1);
    pairOut[uint2(id.x * 2 + 1, id.y)] = float4(0, id.y / 64.0, 0.5, 1);
}

// Writes only some texels.
RWTexture2D<float4> condOut : register(u0);
[numthreads(16, 16, 1)]
void cs_conditional(uint3 id : SV_DispatchThreadID, uint3 group : SV_GroupID)
{
    if (((id.x ^ id.y) & 4) != 0) condOut[id.xy] = float4(group.x / 4.0, group.y / 4.0, 1, 1);
}

// Integer output.
RWTexture2D<uint> hashOut : register(u0);
[numthreads(8, 8, 1)]
void cs_uint(uint3 id : SV_DispatchThreadID, uint flat : SV_GroupIndex)
{
    uint h = id.x * 73856093u ^ id.y * 19349663u ^ flat * 83492791u;
    hashOut[id.xy] = h;
}

// Texture array output, slice from the dispatch's z.
RWTexture2DArray<float4> arrayOut : register(u0);
[numthreads(8, 8, 1)]
void cs_array(uint3 id : SV_DispatchThreadID)
{
    arrayOut[id] = float4(id.x / 32.0, id.y / 32.0, id.z / 4.0, 1);
}

// Horizontal blur through groupshared memory, like Skyrim SE's blur.
RWTexture2D<float4> blurOut : register(u0);
groupshared float3 row[68];
[numthreads(68, 1, 1)]
void cs_blur(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    int x = int(group.x) * 64 + int(local.x) - 2;
    float2 uv = (float2(x, group.y) + 0.5) / 64.0;
    row[local.x] = source.SampleLevel(linearClamp, uv, 0).rgb;
    GroupMemoryBarrierWithGroupSync();
    if (local.x >= 2 && local.x < 66)
    {
        float3 sum = row[local.x - 2] * 0.0625 + row[local.x - 1] * 0.25 + row[local.x] * 0.375 +
                     row[local.x + 1] * 0.25 + row[local.x + 2] * 0.0625;
        blurOut[uint2(x, group.y)] = float4(sum, 1);
    }
}

// Read-modify-write of the output.
RWTexture2D<float> rmwOut : register(u0);
[numthreads(8, 8, 1)]
void cs_rmw(uint3 id : SV_DispatchThreadID)
{
    rmwOut[id.xy] = rmwOut[id.xy] * 2.0 + id.x;
}

// Typed buffer output.
RWBuffer<float4> bufferOut : register(u0);
[numthreads(64, 1, 1)]
void cs_buffer(uint3 id : SV_DispatchThreadID)
{
    bufferOut[id.x] = float4(id.x, id.x * 0.5, 1, 2);
}

// Size query.
RWTexture2D<float4> sizeOut : register(u0);
[numthreads(8, 8, 1)]
void cs_size(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    sizeOut.GetDimensions(w, h);
    if (id.x < w && id.y < h) sizeOut[id.xy] = float4(w, h, id.x / float(w), 1);
}

// Vertical blur through groupshared memory: the group runs along y (like Skyrim SE's).
RWTexture2D<float4> columnOut : register(u0);
groupshared float3 column[36];
[numthreads(1, 36, 1)]
void cs_blur_vertical(uint3 group : SV_GroupID, uint3 local : SV_GroupThreadID)
{
    int y = int(group.y) * 32 + int(local.y) - 2;
    float2 uv = (float2(group.x, y) + 0.5) / 64.0;
    column[local.y] = source.SampleLevel(linearClamp, uv, 0).rgb;
    GroupMemoryBarrierWithGroupSync();
    if (local.y >= 2 && local.y < 34)
    {
        float3 sum = column[local.y - 2] * 0.0625 + column[local.y - 1] * 0.25 + column[local.y] * 0.375 +
                     column[local.y + 1] * 0.25 + column[local.y + 2] * 0.0625;
        columnOut[uint2(group.x, y)] = float4(sum, 1);
    }
}

// Control for "shadertool pstest ... fxc": an ordinary pixel shader doing cs_math's work.
float4 ps_control(float4 pos : SV_Position) : SV_Target
{
    float2 uv = (uint2(pos.xy) + 0.5) / 64.0;
    float4 s = source.SampleLevel(linearClamp, uv * scale.xy, 0);
    return float4(uv, s.r * s.g + 0.25, dot(s.rgb, float3(0.3, 0.6, 0.1)));
}
