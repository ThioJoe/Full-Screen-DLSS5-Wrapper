#pragma once
#include "effects/real/device.h"
#include "interior/frame.h"

#include <array>

namespace real {

struct Texture
{
    Com<ID3D12Resource> resource;
    interior::Extent extent;
    DXGI_FORMAT format;
};

struct TextureRequest
{
    interior::Extent extent;
    DXGI_FORMAT format;
    D3D12_RESOURCE_FLAGS flags;
    D3D12_HEAP_FLAGS heapFlags;
    D3D12_RESOURCE_STATES initialState;
    const wchar_t* name;
};

using ResourceTable = std::array<Com<ID3D12Resource>, interior::kSlotCount>;

[[nodiscard]] infra::Result<Texture, Error> CreateTexture(const GpuDevice& gpu, const TextureRequest& request) noexcept;
[[nodiscard]] infra::Result<Texture, Error> CreateClearableTexture(const GpuDevice& gpu, const TextureRequest& request, float clearValue) noexcept;
[[nodiscard]] infra::Result<Com<ID3D12Resource>, Error> CreateBuffer(const GpuDevice& gpu, interior::ByteCount bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                                                                     const wchar_t* name) noexcept;
[[nodiscard]] infra::Status<Error> WriteZeros(ID3D12Resource* upload, interior::ByteCount bytes) noexcept;
[[nodiscard]] infra::Result<std::uint32_t, Error> ReadFirstUInt(ID3D12Resource* readback) noexcept;

void CreateSrv(const GpuDevice& gpu, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;
void CreateUav(const GpuDevice& gpu, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;
void CreateRawUav(const GpuDevice& gpu, ID3D12Resource* buffer, interior::ByteCount bytes, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;
void CreateNullSrv(const GpuDevice& gpu, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;
void CreateNullUav(const GpuDevice& gpu, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;
void CreateRtv(const GpuDevice& gpu, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept;

[[nodiscard]] D3D12_RESOURCE_STATES ToD3D(interior::ResourceState state) noexcept;
void RecordBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, interior::ResourceState from, interior::ResourceState to) noexcept;

[[nodiscard]] ResourceTable WithResource(const ResourceTable& table, const interior::ResourceId& id, const Com<ID3D12Resource>& resource) noexcept;
[[nodiscard]] infra::Result<ID3D12Resource*, Error> Lookup(const ResourceTable& table, const interior::ResourceId& id) noexcept;
[[nodiscard]] DXGI_FORMAT FormatOf(ID3D12Resource* resource) noexcept;

} // namespace real
