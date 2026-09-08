#pragma once
#include "effects/real/device.h"
#include "interior/frame.h"

namespace real {

constexpr std::uint32_t kComputeSrvCount = 4;
constexpr std::uint32_t kComputeUavCount = 2;
constexpr std::uint32_t kRootConstantCount = 16;
constexpr std::uint32_t kBlitSrvCount = 2;

struct Pipelines
{
    Com<ID3D12RootSignature> computeRoot;
    Com<ID3D12RootSignature> blitRoot;
    Com<ID3D12PipelineState> convert;
    Com<ID3D12PipelineState> downsample;
    Com<ID3D12PipelineState> match;
    Com<ID3D12PipelineState> finalize;
    Com<ID3D12PipelineState> flowToMv;
    Com<ID3D12PipelineState> blit;
};

[[nodiscard]] infra::Result<Pipelines, Error> CreatePipelines(const GpuDevice& gpu, DXGI_FORMAT swapChainFormat) noexcept;
[[nodiscard]] ID3D12PipelineState* PsoFor(const Pipelines& pipelines, interior::PassId pass) noexcept;

} // namespace real
