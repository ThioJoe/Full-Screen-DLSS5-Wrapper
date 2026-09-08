#include "effects/real/resources.h"

#include "infrastructure/array_util.h"

#include <algorithm>
#include <cstring>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;

[[nodiscard]] D3D12_RESOURCE_DESC TextureDescription(const TextureRequest& r) noexcept
{
    return D3D12_RESOURCE_DESC{ D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, r.extent.width.Get(), r.extent.height.Get(), 1, 1, r.format, { 1, 0 }, D3D12_TEXTURE_LAYOUT_UNKNOWN, r.flags };
}

[[nodiscard]] D3D12_HEAP_PROPERTIES HeapOf(D3D12_HEAP_TYPE type) noexcept
{
    return D3D12_HEAP_PROPERTIES{ type, D3D12_CPU_PAGE_PROPERTY_UNKNOWN, D3D12_MEMORY_POOL_UNKNOWN, 0, 0 };
}

[[nodiscard]] bool HasExtent(const D3D12_RESOURCE_DESC& actual, const interior::Extent& extent) noexcept
{
    return actual.Width == extent.width.Get() && actual.Height == extent.height.Get();
}

[[nodiscard]] bool MatchesRequest(ID3D12Resource* resource, const TextureRequest& r) noexcept
{
    const D3D12_RESOURCE_DESC actual = resource->GetDesc();
    return HasExtent(actual, r.extent) && actual.Format == r.format;
}

[[nodiscard]] Result<Texture, Error> Verified(const Com<ID3D12Resource>& resource, const TextureRequest& r) noexcept
{
    if (!MatchesRequest(resource.Get(), r))
        return Fail(Error{ ApiCall::TextureDescriptionMismatch, 0 });
    return Texture{ resource, r.extent, r.format };
}

[[nodiscard]] Result<Com<ID3D12Resource>, Error> Committed(const GpuDevice& gpu, const D3D12_RESOURCE_DESC& desc, D3D12_HEAP_TYPE heapType, D3D12_RESOURCE_STATES state, const D3D12_CLEAR_VALUE* clear,
                                                           const wchar_t* name) noexcept
{
    const D3D12_HEAP_PROPERTIES heap = HeapOf(heapType);
    Com<ID3D12Resource> resource;
    const HRESULT hr = gpu.device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, state, clear, IID_PPV_ARGS(&resource));
    return Check(hr, ApiCall::CreateCommittedResource).and_then([&] { return Check(resource->SetName(name), ApiCall::CreateCommittedResource); }).transform([&resource] { return resource; });
}

[[nodiscard]] D3D12_RESOURCE_DESC BufferDescription(interior::ByteCount bytes, D3D12_RESOURCE_FLAGS flags) noexcept
{
    return D3D12_RESOURCE_DESC{ D3D12_RESOURCE_DIMENSION_BUFFER, 0, bytes.Get(), 1, 1, 1, DXGI_FORMAT_UNKNOWN, { 1, 0 }, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, flags };
}

[[nodiscard]] bool IsAllZero(const void* memory, std::size_t bytes) noexcept
{
    const auto* p = static_cast<const unsigned char*>(memory);
    return std::all_of(p, p + bytes, [](unsigned char c) { return c == 0; });
}

[[nodiscard]] Status<Error> FillAndVerify(ID3D12Resource* upload, void* mapped, std::size_t bytes) noexcept
{
    std::memset(mapped, 0, bytes);
    const bool verified = IsAllZero(mapped, bytes);
    upload->Unmap(0, nullptr);
    if (!verified)
        return Fail(Error{ ApiCall::MapResource, 1 });
    return {};
}

[[nodiscard]] std::uint32_t CopiedThenUnmapped(ID3D12Resource* readback, const void* mapped) noexcept
{
    std::uint32_t value = 0;
    std::memcpy(&value, mapped, sizeof(value));
    const D3D12_RANGE noWrite{ 0, 0 };
    readback->Unmap(0, &noWrite);
    return value;
}

[[nodiscard]] D3D12_SHADER_RESOURCE_VIEW_DESC SrvDescription(DXGI_FORMAT format) noexcept
{
    return D3D12_SHADER_RESOURCE_VIEW_DESC{
        .Format = format, .ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D, .Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING, .Texture2D = D3D12_TEX2D_SRV{ 0, 1, 0, 0.0f }
    };
}

[[nodiscard]] D3D12_UNORDERED_ACCESS_VIEW_DESC RawUavDescription(interior::ByteCount bytes) noexcept
{
    return D3D12_UNORDERED_ACCESS_VIEW_DESC{ .Format = DXGI_FORMAT_R32_TYPELESS,
                                             .ViewDimension = D3D12_UAV_DIMENSION_BUFFER,
                                             .Buffer = D3D12_BUFFER_UAV{ 0, bytes.Get() / 4, 0, 0, D3D12_BUFFER_UAV_FLAG_RAW } };
}

} // namespace

Result<Texture, Error> CreateTexture(const GpuDevice& gpu, const TextureRequest& request) noexcept
{
    return Committed(gpu, TextureDescription(request), D3D12_HEAP_TYPE_DEFAULT, request.initialState, nullptr, request.name).and_then([&request](const Com<ID3D12Resource>& resource) {
        return Verified(resource, request);
    });
}

Result<Texture, Error> CreateClearableTexture(const GpuDevice& gpu, const TextureRequest& request, float clearValue) noexcept
{
    const D3D12_CLEAR_VALUE clear{ request.format, { { clearValue, 0.0f, 0.0f, 0.0f } } };
    return Committed(gpu, TextureDescription(request), D3D12_HEAP_TYPE_DEFAULT, request.initialState, &clear, request.name).and_then([&request](const Com<ID3D12Resource>& resource) {
        return Verified(resource, request);
    });
}

Result<Com<ID3D12Resource>, Error> CreateBuffer(const GpuDevice& gpu, interior::ByteCount bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                                                const wchar_t* name) noexcept
{
    return Committed(gpu, BufferDescription(bytes, flags), heap, state, nullptr, name);
}

Status<Error> WriteZeros(ID3D12Resource* upload, interior::ByteCount bytes) noexcept
{
    void* mapped = nullptr;
    const D3D12_RANGE noRead{ 0, 0 };
    return Check(upload->Map(0, &noRead, &mapped), ApiCall::MapResource).and_then([&] { return FillAndVerify(upload, mapped, bytes.Get()); });
}

Result<std::uint32_t, Error> ReadFirstUInt(ID3D12Resource* readback) noexcept
{
    void* mapped = nullptr;
    const D3D12_RANGE range{ 0, sizeof(std::uint32_t) };
    return Check(readback->Map(0, &range, &mapped), ApiCall::MapResource).transform([&] { return CopiedThenUnmapped(readback, mapped); });
}

void CreateSrv(const GpuDevice& gpu, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    const D3D12_SHADER_RESOURCE_VIEW_DESC desc = SrvDescription(format);
    gpu.device->CreateShaderResourceView(resource, &desc, handle);
}

void CreateUav(const GpuDevice& gpu, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    D3D12_UNORDERED_ACCESS_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    gpu.device->CreateUnorderedAccessView(resource, nullptr, &desc, handle);
}

void CreateRawUav(const GpuDevice& gpu, ID3D12Resource* buffer, interior::ByteCount bytes, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    const D3D12_UNORDERED_ACCESS_VIEW_DESC desc = RawUavDescription(bytes);
    gpu.device->CreateUnorderedAccessView(buffer, nullptr, &desc, handle);
}

void CreateNullSrv(const GpuDevice& gpu, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    CreateSrv(gpu, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, handle);
}

void CreateNullUav(const GpuDevice& gpu, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    CreateUav(gpu, nullptr, DXGI_FORMAT_R8G8B8A8_UNORM, handle);
}

void CreateRtv(const GpuDevice& gpu, ID3D12Resource* resource, DXGI_FORMAT format, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    D3D12_RENDER_TARGET_VIEW_DESC desc{};
    desc.Format = format;
    desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
    gpu.device->CreateRenderTargetView(resource, &desc, handle);
}

D3D12_RESOURCE_STATES ToD3D(interior::ResourceState state) noexcept
{
    switch (state)
    {
    case interior::ResourceState::CopyDest: return D3D12_RESOURCE_STATE_COPY_DEST;
    case interior::ResourceState::CopySource: return D3D12_RESOURCE_STATE_COPY_SOURCE;
    case interior::ResourceState::ShaderRead: return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    case interior::ResourceState::UnorderedAccess: return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    case interior::ResourceState::RenderTarget: return D3D12_RESOURCE_STATE_RENDER_TARGET;
    case interior::ResourceState::Present: return D3D12_RESOURCE_STATE_PRESENT;
    case interior::ResourceState::Common: return D3D12_RESOURCE_STATE_COMMON;
    case interior::ResourceState::GenericRead: return D3D12_RESOURCE_STATE_GENERIC_READ;
    }
    return D3D12_RESOURCE_STATE_COMMON;
}

void RecordBarrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, interior::ResourceState from, interior::ResourceState to) noexcept
{
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = D3D12_RESOURCE_TRANSITION_BARRIER{ resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, ToD3D(from), ToD3D(to) };
    list->ResourceBarrier(1, &barrier);
}

ResourceTable WithResource(const ResourceTable& table, const interior::ResourceId& id, const Com<ID3D12Resource>& resource) noexcept
{
    return infra::WithElement(table, interior::SlotOf(id), resource);
}

Result<ID3D12Resource*, Error> Lookup(const ResourceTable& table, const interior::ResourceId& id) noexcept
{
    ID3D12Resource* resource = table[interior::SlotOf(id)].Get();
    if (resource == nullptr)
        return Fail(Error{ ApiCall::ResourceMissing, static_cast<std::uint32_t>(interior::SlotOf(id)) });
    return resource;
}

DXGI_FORMAT FormatOf(ID3D12Resource* resource) noexcept
{
    return resource->GetDesc().Format;
}

} // namespace real
