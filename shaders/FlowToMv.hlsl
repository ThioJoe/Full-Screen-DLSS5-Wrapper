// Converts NVIDIA Optical Flow output (R16G16_SINT, S10.5 fixed point, one
// vector per grid cell, input -> reference direction) into a DLSS motion-vector
// texture in pixels of the motion-vector texture.
#include "Common.hlsli"

cbuffer Constants : register(b0)
{
    uint2 g_OutSize;   // motion-vector texture size
    uint2 g_FlowSize;  // optical-flow grid size
    uint  g_Grid;      // grid cell size in input pixels (1, 2 or 4)
    float g_Scale;     // input pixels -> motion-vector pixels
    uint2 g_Unused;
};

Texture2D<int2>     g_Flow : register(t0);
RWTexture2D<float2> g_Mv   : register(u0);

[numthreads(8, 8, 1)]
void CS_FlowToMv(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_OutSize.x || id.y >= g_OutSize.y)
        return;

    int2 cell = min(int2(id.xy) / int(g_Grid), int2(g_FlowSize) - 1);
    int2 v = g_Flow[cell];
    g_Mv[id.xy] = float2(v) * (1.0 / 32.0) * g_Scale;
}
