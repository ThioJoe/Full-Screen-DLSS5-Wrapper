// 2x2 box downsample of a luma plane, one pyramid level down.
#include "Common.hlsli"

cbuffer Constants : register(b0)
{
    uint2 g_DstSize;
    uint2 g_SrcSize;
};

Texture2D<float>   g_Src : register(t0);
RWTexture2D<float> g_Dst : register(u0);

[numthreads(8, 8, 1)]
void CS_Downsample(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_DstSize.x || id.y >= g_DstSize.y)
        return;

    int2 p = int2(id.xy) * 2;
    int2 maxP = int2(g_SrcSize) - 1;
    float s = g_Src[min(p, maxP)]
            + g_Src[min(p + int2(1, 0), maxP)]
            + g_Src[min(p + int2(0, 1), maxP)]
            + g_Src[min(p + int2(1, 1), maxP)];
    g_Dst[id.xy] = s * 0.25;
}
