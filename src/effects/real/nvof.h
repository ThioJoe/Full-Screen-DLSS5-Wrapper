#pragma once
#include "effects/real/resources.h"
#include "interior/plan.h"

#include <nvOpticalFlowCommon.h>
#include <nvOpticalFlowD3D12.h>

#include <array>
#include <memory>
#include <type_traits>

namespace real {

struct ModuleFreer
{
    void operator()(HMODULE module) const noexcept;
};
using UniqueModule = std::unique_ptr<std::remove_pointer_t<HMODULE>, ModuleFreer>;

struct SessionDestroyer
{
    PFNNVOFDESTROY destroy;
    void operator()(NvOFHandle session) const noexcept;
};
using OpticalFlowSession = std::unique_ptr<std::remove_pointer_t<NvOFHandle>, SessionDestroyer>;

struct BufferUnregister
{
    PFNNVOFUNREGISTERRESOURCED3D12 unregister;
    void operator()(NvOFGPUBufferHandle buffer) const noexcept;
};
using RegisteredBuffer = std::unique_ptr<std::remove_pointer_t<NvOFGPUBufferHandle>, BufferUnregister>;

struct OpticalFlow
{
    UniqueModule library;
    NV_OF_D3D12_API_FUNCTION_LIST api;
    OpticalFlowSession session;
    Com<ID3D12Fence> completion;
    std::array<RegisteredBuffer, 2> luma;
    RegisteredBuffer flow;
};

struct OpticalFlowFrame
{
    interior::FrameNumber number;
    interior::SetIndex set;
    bool hasPrevious;
    interior::FenceValue phaseOne;
};

[[nodiscard]] infra::Result<OpticalFlow, Error> CreateOpticalFlow(const GpuDevice& gpu, const interior::SessionPlan& plan, const ResourceTable& resources) noexcept;
[[nodiscard]] infra::Status<Error> ExecuteOpticalFlow(const OpticalFlow& flow, const GpuDevice& gpu, const OpticalFlowFrame& frame) noexcept;

} // namespace real
