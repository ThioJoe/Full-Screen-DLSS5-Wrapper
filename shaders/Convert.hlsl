// Converts the captured desktop canvas (B8G8R8A8_UNORM, display-referred sRGB)
// into the model's colour input and a luma plane for motion estimation.
#include "Common.hlsli"

cbuffer Constants : register(b0)
{
    uint2 g_Size;   // canvas size in pixels
    uint2 g_Unused;
};

Texture2D<float4>   g_Source : register(t0); // capture canvas
RWTexture2D<float4> g_Color  : register(u0); // model colour input (opaque)
RWTexture2D<float>  g_Luma   : register(u1); // R8_UNORM luma, level 0 of the pyramid

[numthreads(8, 8, 1)]
void CS_Convert(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Size.x || id.y >= g_Size.y)
        return;

    float4 c = g_Source[id.xy];
    c.a = 1.0;
    g_Color[id.xy] = c;
    g_Luma[id.xy] = dot(c.rgb, kLumaWeights);
}
