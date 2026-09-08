#pragma once
#include "effects/real/com.h"
#include "interior/driver.h"
#include "interior/units.h"

#include <d3d12.h>
#include <dxgi1_6.h>

#include <optional>

namespace real {

struct DeviceSettings
{
    bool debugLayer;
    std::optional<interior::RequestedAdapter> adapter;
};

struct GpuDevice
{
    Com<IDXGIFactory4> factory;
    Com<IDXGIAdapter1> adapter;
    Com<ID3D12Device> device;
    Com<ID3D12CommandQueue> queue;
    Com<ID3D12Fence> fence;
    UniqueHandle fenceEvent;
    Com<ID3D12DescriptorHeap> rtvHeap;
    Com<ID3D12DescriptorHeap> srvHeap;
    std::uint32_t rtvIncrement;
    std::uint32_t srvIncrement;
    bool nvidia;
    interior::AdapterName name;
    std::optional<interior::DriverVersion> driverVersion;
};

constexpr std::uint32_t kRtvSlots = 8;
constexpr std::uint32_t kSrvSlots = interior::kDescriptorsPerFrame * interior::kFrameSlotCount;
constexpr std::uint64_t kFenceTimeoutMicroseconds = 4000000;

[[nodiscard]] infra::Result<GpuDevice, Error> CreateGpuDevice(const DeviceSettings& settings) noexcept;
[[nodiscard]] infra::Result<Com<ID3D12Fence>, Error> CreateFence(ID3D12Device* device, D3D12_FENCE_FLAGS flags) noexcept;
[[nodiscard]] infra::Result<interior::FenceValue, Error> SignalFence(const GpuDevice& gpu, interior::FenceValue previous) noexcept;
[[nodiscard]] infra::Status<Error> WaitForFence(const GpuDevice& gpu, interior::FenceValue value, interior::Microseconds timeout) noexcept;
[[nodiscard]] infra::Result<interior::FenceValue, Error> WaitIdle(const GpuDevice& gpu, interior::FenceValue previous) noexcept;
[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE RtvHandle(const GpuDevice& gpu, std::uint32_t index) noexcept;
[[nodiscard]] D3D12_CPU_DESCRIPTOR_HANDLE SrvCpuHandle(const GpuDevice& gpu, std::uint32_t index) noexcept;
[[nodiscard]] D3D12_GPU_DESCRIPTOR_HANDLE SrvGpuHandle(const GpuDevice& gpu, std::uint32_t index) noexcept;
[[nodiscard]] infra::Result<Com<ID3D12CommandAllocator>, Error> CreateAllocator(const GpuDevice& gpu) noexcept;
[[nodiscard]] infra::Result<Com<ID3D12GraphicsCommandList>, Error> CreateClosedCommandList(const GpuDevice& gpu, ID3D12CommandAllocator* allocator) noexcept;
[[nodiscard]] infra::Status<Error> ExecuteList(const GpuDevice& gpu, ID3D12GraphicsCommandList* list) noexcept;
[[nodiscard]] infra::Status<Error> OpenCommandList(const GpuDevice& gpu, ID3D12GraphicsCommandList* list, ID3D12CommandAllocator* allocator) noexcept;
[[nodiscard]] infra::Result<interior::FenceValue, Error> FlushCommandList(const GpuDevice& gpu, ID3D12GraphicsCommandList* list, interior::FenceValue previous) noexcept;

} // namespace real
