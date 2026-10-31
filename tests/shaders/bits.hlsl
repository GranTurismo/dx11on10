// Shaders for "shadertool compare": each pixel shader turns its position and a buffer
// of random numbers into integer results, so native SM5 output and converted SM4
// output can be compared bit for bit.

cbuffer Inputs : register(b0)
{
    uint4 data[16];
};

float4 vs_fullscreen(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

uint4 Seed(float4 pos)
{
    uint2 p = uint2(pos.xy);
    uint h = p.x * 73856093u ^ p.y * 19349663u;
    return data[(p.x + p.y) & 15] ^ uint4(h, h * 3u, h * 7u, h * 13u);
}

// Bit packing with constant widths and offsets, the way Skyrim's shaders use bfi.
uint4 ps_bfi_const(float4 pos : SV_Position) : SV_Target
{
    uint4 s = Seed(pos);
    uint r = 0;
    r = (r & ~0xCu) | ((s.x & 3u) << 2);
    r = (r & ~0x3u) | (s.y & 3u);
    uint q = (s.z & ~0x00FF0000u) | ((s.w & 0xFFu) << 16);
    uint t = (s.x & ~(0x7u << 5)) | ((s.y << 5) & (0x7u << 5));
    return uint4(r, q, t, (s.w & 0x80000000u) | (s.z & 0x7FFFFFFFu));
}

// Width and offset known only at run time.
uint4 ps_bfi_dynamic(float4 pos : SV_Position) : SV_Target
{
    uint4 s = Seed(pos);
    uint w = s.x & 31u, o = s.y & 31u;
    uint mask = ((1u << w) - 1u) << o;
    return uint4((s.z << o & mask) | (s.w & ~mask), w, o, mask);
}
