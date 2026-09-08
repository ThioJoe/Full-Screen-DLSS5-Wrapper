// Shared declarations for DlssScreen shaders.
#ifndef DSCREEN_COMMON_HLSLI
#define DSCREEN_COMMON_HLSLI

// Every compute pass shares one root signature:
//   b0  : 16 root constants
//   t0-3: SRV descriptor table
//   u0-1: UAV descriptor table
//   s0  : linear clamp sampler, s1: point clamp sampler
SamplerState g_LinearClamp : register(s0);
SamplerState g_PointClamp  : register(s1);

static const float3 kLumaWeights = float3(0.299, 0.587, 0.114);

#endif
