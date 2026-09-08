// Draws the processed frame into the swap chain, with optional comparison views.
#include "Common.hlsli"

cbuffer Constants : register(b0)
{
    uint   g_Mode;   // 0: processed, 1: original, 2: split (original left, processed right)
    float  g_Split;  // split position in [0,1]
    float2 g_Unused;
};

Texture2D<float4> g_Processed : register(t0);
Texture2D<float4> g_Original  : register(t1);

struct VsOut
{
    float4 position : SV_Position;
    float2 uv       : TEXCOORD0;
};

VsOut VS_Blit(uint vertexId : SV_VertexID)
{
    VsOut o;
    float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    o.position = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    o.uv = uv;
    return o;
}

float4 PS_Blit(VsOut i) : SV_Target
{
    float3 processed = g_Processed.SampleLevel(g_LinearClamp, i.uv, 0).rgb;
    if (g_Mode == 0u)
        return float4(processed, 1.0);

    float3 original = g_Original.SampleLevel(g_LinearClamp, i.uv, 0).rgb;
    if (g_Mode == 1u)
        return float4(original, 1.0);

    if (abs(i.uv.x - g_Split) < 0.0008)
        return float4(1.0, 0.8, 0.0, 1.0);
    return float4(i.uv.x < g_Split ? original : processed, 1.0);
}
