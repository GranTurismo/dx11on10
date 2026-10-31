// Shaders for dx11test and shadertool. All compiled as Shader Model 5.0, like a DX11-only game.

cbuffer Frame : register(b0)
{
    float4x4 worldViewProj;
    float4x4 world;
    float4 lightDir;
    float4 tint;
    float4 params;  // x = frame, y = particle size
};

Texture2D albedo : register(t0);
SamplerState linearWrap : register(s0);

struct MeshIn
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct MeshOut
{
    float4 pos : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

MeshOut vs_mesh(MeshIn i)
{
    MeshOut o;
    o.pos = mul(float4(i.pos, 1), worldViewProj);
    o.normal = mul(float4(i.normal, 0), world).xyz;
    o.uv = i.uv;
    o.worldPos = mul(float4(i.pos, 1), world).xyz;
    return o;
}

float4 ps_mesh(MeshOut i) : SV_Target
{
    float3 n = normalize(i.normal);
    float ndl = saturate(dot(n, -lightDir.xyz));
    float4 tex = albedo.Sample(linearWrap, i.uv);
    float fade = rcp(1.0 + 0.05 * length(i.worldPos));
    float grid = saturate(abs(ddx_fine(i.uv.x)) * 60.0 + abs(ddy_coarse(i.uv.y)) * 60.0);
    float3 color = tex.rgb * (0.3 + 0.7 * ndl) * tint.rgb;
    return float4(lerp(color, color * fade, 0.5) + grid * 0.05, 1);
}

struct DepthIn
{
    noperspective centroid float4 pos : SV_Position;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
    float3 worldPos : TEXCOORD1;
};

// Writes depth with a conservative-depth semantic (SM5 only).
float4 ps_depth_ge(DepthIn i, out float depth : SV_DepthGreaterEqual) : SV_Target
{
    depth = i.pos.z;
    float4 tex = albedo.Sample(linearWrap, i.uv * 2);
    return float4(tex.rgb * float3(0.4, 0.9, 0.5), 1);
}

struct ParticleIn
{
    float3 pos : POSITION;
    float4 color : COLOR;
};

struct ParticleVS
{
    float4 pos : SV_Position;
    float4 color : COLOR;
};

struct ParticleGS
{
    float4 pos : SV_Position;
    float4 color : COLOR;
    float2 uv : TEXCOORD0;
};

ParticleVS vs_particle(ParticleIn i)
{
    ParticleVS o;
    o.pos = mul(float4(i.pos, 1), worldViewProj);
    o.color = i.color;
    return o;
}

[maxvertexcount(4)]
void gs_particle(point ParticleVS p[1], inout TriangleStream<ParticleGS> stream)
{
    const float2 corners[4] = { float2(-1, -1), float2(-1, 1), float2(1, -1), float2(1, 1) };
    [unroll]
    for (int k = 0; k < 4; k++)
    {
        ParticleGS o;
        o.pos = p[0].pos + float4(corners[k] * params.y * float2(0.5625, 1), 0, 0) * p[0].pos.w;
        o.color = p[0].color;
        o.uv = corners[k];
        stream.Append(o);
    }
}

float4 ps_particle(ParticleGS i) : SV_Target
{
    float d = saturate(1 - dot(i.uv, i.uv));
    return float4(i.color.rgb * d, d);
}

// Fog from the depth buffer, read while that buffer stays bound read-only (deferred-style).
Texture2D<float> sceneDepth : register(t1);

float4 vs_fullscreen(uint id : SV_VertexID) : SV_Position
{
    float2 uv = float2((id << 1) & 2, id & 2);
    return float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
}

float4 ps_fog(float4 pos : SV_Position) : SV_Target
{
    float d = sceneDepth.Load(int3(pos.xy, 0));
    float fog = saturate((d - 0.985) * 60);
    return float4(0.55, 0.62, 0.75, fog * 0.85);
}

// ---- Things DX10 hardware can't run (used to test the fallbacks) ----

struct PatchPoint
{
    float3 pos : POSITION;
    float3 normal : NORMAL;
    float2 uv : TEXCOORD0;
};

struct PatchConstants
{
    float edges[3] : SV_TessFactor;
    float inside : SV_InsideTessFactor;
};

MeshIn vs_patch(MeshIn i) { return i; }

PatchConstants hs_constants(InputPatch<MeshIn, 3> patch)
{
    PatchConstants c;
    c.edges[0] = c.edges[1] = c.edges[2] = 8;
    c.inside = 8;
    return c;
}

[domain("tri")]
[partitioning("integer")]
[outputtopology("triangle_cw")]
[outputcontrolpoints(3)]
[patchconstantfunc("hs_constants")]
MeshIn hs_main(InputPatch<MeshIn, 3> patch, uint id : SV_OutputControlPointID) { return patch[id]; }

[domain("tri")]
MeshOut ds_main(PatchConstants c, float3 bary : SV_DomainLocation, const OutputPatch<MeshIn, 3> patch)
{
    float3 pos = patch[0].pos * bary.x + patch[1].pos * bary.y + patch[2].pos * bary.z;
    float3 normal = normalize(patch[0].normal * bary.x + patch[1].normal * bary.y + patch[2].normal * bary.z);
    pos = normalize(pos) * 0.75;
    MeshOut o;
    o.pos = mul(float4(pos, 1), worldViewProj);
    o.normal = mul(float4(normal, 0), world).xyz;
    o.uv = patch[0].uv * bary.x + patch[1].uv * bary.y + patch[2].uv * bary.z;
    o.worldPos = mul(float4(pos, 1), world).xyz;
    return o;
}

RWTexture2D<float4> csOutput : register(u0);

[numthreads(8, 8, 1)]
void cs_main(uint3 id : SV_DispatchThreadID)
{
    float2 uv = id.xy / 256.0;
    csOutput[id.xy] = float4(uv, 0.5 + 0.5 * sin(params.x * 0.1), 1);
}

// Bit operations that need emulation on SM4 (not supported by the converter yet).
float4 ps_bits(MeshOut i) : SV_Target
{
    uint v = asuint(i.uv.x * 1000);
    return float4(countbits(v) / 32.0, firstbithigh(v) / 32.0, 0, 1);
}
