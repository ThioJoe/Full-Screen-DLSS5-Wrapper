// Coarse-to-fine block matching between the current and previous luma pyramids.
// WAIVER(R2): GPU kernel loops are the vectorised fold over the search window.
//
// Output convention (the DLSS one): for a pixel p of the current frame the
// vector v satisfies  previous_position = p + v, in pixels of the level being
// matched. CS_Finalize rescales to the motion-vector texture's own pixels.
#include "Common.hlsli"

cbuffer Constants : register(b0)
{
    uint2  g_Size;          // size of this level
    uint2  g_OutSize;       // CS_Finalize: size of the motion-vector texture
    int    g_Radius;        // search radius around the prediction, in level pixels
    uint   g_Flags;         // bit0: prediction available, bit1: count unmatched pixels, bit2: sub-pixel refinement
    float  g_Lambda;        // cost per level pixel of deviating from the prediction
    float  g_ZeroBias;      // zero motion wins when within this cost margin of the best candidate
    float  g_BadThreshold;  // mean absolute difference above which a pixel counts as unmatched
    float  g_OutScale;      // CS_Finalize: level pixels -> motion-vector pixels
    float2 g_Unused;
};

Texture2D<float>     g_Cur   : register(t0); // current luma, this level
Texture2D<float>     g_Prev  : register(t1); // previous luma, this level
Texture2D<float2>    g_Pred  : register(t2); // flow of the coarser level (or, for CS_Finalize, of the finest level)
RWTexture2D<float2>  g_Flow  : register(u0); // flow written for this level (or the motion-vector texture)
RWByteAddressBuffer  g_Stats : register(u1); // dword 0: number of unmatched pixels

static const int kWindow = 3; // (2*3+1)^2 = 49 taps

float MeanAbsDiff(int2 p, int2 d)
{
    int2 maxP = int2(g_Size) - 1;
    float s = 0.0;
    [unroll]
    for (int y = -kWindow; y <= kWindow; ++y)
    {
        [unroll]
        for (int x = -kWindow; x <= kWindow; ++x)
        {
            int2 a = clamp(p + int2(x, y), int2(0, 0), maxP);
            int2 b = clamp(p + d + int2(x, y), int2(0, 0), maxP);
            s += abs(g_Cur[a] - g_Prev[b]);
        }
    }
    return s * (1.0 / 49.0);
}

[numthreads(8, 8, 1)]
void CS_Match(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_Size.x || id.y >= g_Size.y)
        return;

    int2 p = int2(id.xy);

    float2 predicted = float2(0.0, 0.0);
    if (g_Flags & 1u)
    {
        float2 uv = (float2(p) + 0.5) / float2(g_Size);
        predicted = g_Pred.SampleLevel(g_LinearClamp, uv, 0) * 2.0; // coarser level pixels -> this level
    }
    int2 pred = int2(round(predicted));

    float zeroCost = MeanAbsDiff(p, int2(0, 0));
    float best = zeroCost;
    int2 bestD = int2(0, 0);

    for (int dy = -g_Radius; dy <= g_Radius; ++dy)
    {
        for (int dx = -g_Radius; dx <= g_Radius; ++dx)
        {
            int2 d = pred + int2(dx, dy);
            float c = MeanAbsDiff(p, d) + g_Lambda * float(abs(dx) + abs(dy));
            if (c < best)
            {
                best = c;
                bestD = d;
            }
        }
    }

    // Static content must come out as exactly zero, or text shimmers.
    if (zeroCost <= best + g_ZeroBias)
    {
        best = zeroCost;
        bestD = int2(0, 0);
    }

    float2 result = float2(bestD);

    if ((g_Flags & 4u) && any(bestD != int2(0, 0)))
    {
        // Parabolic sub-pixel refinement from the two neighbours on each axis.
        float cx0 = MeanAbsDiff(p, bestD + int2(-1, 0));
        float cx1 = MeanAbsDiff(p, bestD + int2( 1, 0));
        float cy0 = MeanAbsDiff(p, bestD + int2(0, -1));
        float cy1 = MeanAbsDiff(p, bestD + int2(0,  1));
        float denX = cx0 - 2.0 * best + cx1;
        float denY = cy0 - 2.0 * best + cy1;
        if (denX > 1e-5)
            result.x += clamp(0.5 * (cx0 - cx1) / denX, -0.5, 0.5);
        if (denY > 1e-5)
            result.y += clamp(0.5 * (cy0 - cy1) / denY, -0.5, 0.5);
    }

    g_Flow[id.xy] = result;

    if ((g_Flags & 2u) && best > g_BadThreshold)
    {
        uint previous;
        g_Stats.InterlockedAdd(0, 1u, previous);
    }
}

// Upsamples the finest computed flow level to the motion-vector texture and
// converts level pixels into motion-vector pixels.
[numthreads(8, 8, 1)]
void CS_Finalize(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_OutSize.x || id.y >= g_OutSize.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(g_OutSize);
    float2 flow = g_Pred.SampleLevel(g_LinearClamp, uv, 0);
    g_Flow[id.xy] = flow * g_OutScale;
}
