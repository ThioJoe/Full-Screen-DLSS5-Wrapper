#include "effects/real/executor.h"

#include "infrastructure/fold.h"
#include "infrastructure/overloaded.h"

#include <bit>
#include <ranges>
#include <span>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;
using interior::ResourceId;

constexpr std::uint32_t kDispatchDescriptors = kComputeSrvCount + kComputeUavCount;
constexpr std::array<float, 4> kBlack{ 0.0f, 0.0f, 0.0f, 1.0f };
constexpr float kSplitPosition = 0.5f;

struct Cursor
{
    std::uint32_t descriptor;
    interior::FenceValue fence;
};

using StepResult = Result<Cursor, Error>;

enum class ViewKind : std::uint8_t { Srv, Uav };

[[nodiscard]] Status<Error> RequireBudget(const Cursor& c, std::uint32_t count) noexcept
{
    if (c.descriptor + count > interior::kDescriptorsPerFrame)
        return Fail(Error{ ApiCall::DescriptorBudget, c.descriptor });
    return {};
}

[[nodiscard]] Cursor Advanced(const Cursor& c, std::uint32_t count) noexcept
{
    return Cursor{ c.descriptor + count, c.fence };
}

[[nodiscard]] Cursor WithFence(const Cursor& c, interior::FenceValue fence) noexcept
{
    return Cursor{ c.descriptor, fence };
}

[[nodiscard]] std::uint32_t HeapIndex(interior::FrameSlot slot, std::uint32_t offset) noexcept
{
    return slot.Get() * interior::kDescriptorsPerFrame + offset;
}

[[nodiscard]] bool IsBuffer(ID3D12Resource* resource) noexcept
{
    return resource->GetDesc().Dimension == D3D12_RESOURCE_DIMENSION_BUFFER;
}

[[nodiscard]] bool IsBackBuffer(const ResourceId& id) noexcept
{
    return id.kind == interior::ResourceKind::BackBuffer;
}

[[nodiscard]] interior::ByteCount SizeOf(ID3D12Resource* buffer) noexcept
{
    return interior::ByteCountTag::Parse(static_cast<std::uint32_t>(buffer->GetDesc().Width));
}

void CreateNullView(const GpuDevice& device, ViewKind kind, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    switch (kind)
    {
    case ViewKind::Srv: return CreateNullSrv(device, handle);
    case ViewKind::Uav: return CreateNullUav(device, handle);
    }
}

void CreateAnyUav(const GpuDevice& device, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    if (IsBuffer(resource))
        return CreateRawUav(device, resource, SizeOf(resource), handle);
    return CreateUav(device, resource, FormatOf(resource), handle);
}

void CreateResourceView(const GpuDevice& device, ViewKind kind, ID3D12Resource* resource, D3D12_CPU_DESCRIPTOR_HANDLE handle) noexcept
{
    switch (kind)
    {
    case ViewKind::Srv: return CreateSrv(device, resource, FormatOf(resource), handle);
    case ViewKind::Uav: return CreateAnyUav(device, resource, handle);
    }
}

[[nodiscard]] Status<Error> WriteView(const Gpu& gpu, ViewKind kind, const std::optional<ResourceId>& id, std::uint32_t heapIndex) noexcept
{
    const D3D12_CPU_DESCRIPTOR_HANDLE handle = SrvCpuHandle(gpu.device, heapIndex);
    if (!id.has_value())
    {
        CreateNullView(gpu.device, kind, handle);
        return {};
    }
    return Lookup(gpu.resources, *id).transform([&](ID3D12Resource* resource) { CreateResourceView(gpu.device, kind, resource, handle); });
}

[[nodiscard]] Status<Error> WriteViews(const Gpu& gpu, ViewKind kind, std::span<const std::optional<ResourceId>> ids, std::uint32_t base) noexcept
{
    return infra::ForEach(std::views::iota(std::size_t{ 0 }, ids.size()), Status<Error>{}, [&](std::size_t i) { return WriteView(gpu, kind, ids[i], base + static_cast<std::uint32_t>(i)); });
}

[[nodiscard]] Status<Error> WriteBinding(const Gpu& gpu, const interior::Binding& b, std::uint32_t base) noexcept
{
    return WriteViews(gpu, ViewKind::Srv, b.srv, base).and_then([&] { return WriteViews(gpu, ViewKind::Uav, b.uav, base + kComputeSrvCount); });
}

void SetComputeTables(const Gpu& gpu, std::uint32_t base) noexcept
{
    gpu.list->SetComputeRootDescriptorTable(1, SrvGpuHandle(gpu.device, base));
    gpu.list->SetComputeRootDescriptorTable(2, SrvGpuHandle(gpu.device, base + kComputeSrvCount));
}

void SetComputeState(const Gpu& gpu, const interior::Dispatch& d, std::uint32_t base) noexcept
{
    gpu.list->SetComputeRootSignature(gpu.pipelines.computeRoot.Get());
    gpu.list->SetPipelineState(PsoFor(gpu.pipelines, d.pass));
    gpu.list->SetComputeRoot32BitConstants(0, d.constants.count, d.constants.values.data(), 0);
    SetComputeTables(gpu, base);
}

[[nodiscard]] Cursor Dispatched(const Gpu& gpu, const interior::Dispatch& d, std::uint32_t base, const Cursor& c) noexcept
{
    SetComputeState(gpu, d, base);
    gpu.list->Dispatch(d.groups.x.Get(), d.groups.y.Get(), 1);
    return Advanced(c, kDispatchDescriptors);
}

[[nodiscard]] StepResult RecordDispatch(const Gpu& gpu, interior::FrameSlot slot, const interior::Dispatch& d, const Cursor& c) noexcept
{
    const std::uint32_t base = HeapIndex(slot, c.descriptor);
    return RequireBudget(c, kDispatchDescriptors).and_then([&] { return WriteBinding(gpu, d.binding, base); }).transform([&] { return Dispatched(gpu, d, base, c); });
}

[[nodiscard]] StepResult RecordTransition(const Gpu& gpu, const interior::Transition& t, const Cursor& c) noexcept
{
    return Lookup(gpu.resources, t.resource).transform([&](ID3D12Resource* resource) {
        RecordBarrier(gpu.list.Get(), resource, t.from, t.to);
        return c;
    });
}

[[nodiscard]] StepResult RecordCopy(const Gpu& gpu, const interior::CopyBuffer& copy, const Cursor& c) noexcept
{
    return Lookup(gpu.resources, copy.source).and_then([&](ID3D12Resource* source) {
        return Lookup(gpu.resources, copy.destination).transform([&](ID3D12Resource* destination) {
            gpu.list->CopyBufferRegion(destination, 0, source, 0, copy.bytes.Get());
            return c;
        });
    });
}

[[nodiscard]] StepResult RecordClear(const Gpu& gpu, const interior::ClearTarget& clear, const Cursor& c) noexcept
{
    REQUIRE(IsBackBuffer(clear.target));
    gpu.list->ClearRenderTargetView(RtvHandle(gpu.device, clear.target.buffer.Get()), kBlack.data(), 0, nullptr);
    return c;
}

[[nodiscard]] Result<ModelIo, Error> ResolvedIo(const ResourceTable& table, const interior::ModelIo& io) noexcept
{
    return Lookup(table, io.color).and_then([&](ID3D12Resource* color) {
        return Lookup(table, io.depth).and_then([&](ID3D12Resource* depth) {
            return Lookup(table, io.motionVectors).and_then([&](ID3D12Resource* motionVectors) {
                return Lookup(table, io.output).transform([&](ID3D12Resource* output) { return ModelIo{ color, depth, motionVectors, output }; });
            });
        });
    });
}

[[nodiscard]] StepResult RecordSuperResolution(const Gpu& gpu, const interior::EvaluateSr& e, const Cursor& c) noexcept
{
    REQUIRE(gpu.models.runtime.has_value());
    REQUIRE(gpu.models.superResolution.has_value());
    return ResolvedIo(gpu.resources, e.io)
        .and_then([&](const ModelIo& io) { return EvaluateSuperResolution(*gpu.models.runtime, *gpu.models.superResolution, gpu.list.Get(), SrInputs{ io, e.render, e.reset }); })
        .transform([&c] { return c; });
}

[[nodiscard]] StepResult RecordNeuralRendering(const Gpu& gpu, const interior::SessionPlan& plan, const interior::EvaluateNr& e, const Cursor& c) noexcept
{
    REQUIRE(gpu.models.runtime.has_value());
    REQUIRE(gpu.models.neuralRendering.has_value());
    return EvaluateNeuralRendering(*gpu.models.runtime, *gpu.models.neuralRendering, gpu.list.Get(), plan.tuning, e, gpu.resources).transform([&c] { return c; });
}

[[nodiscard]] std::uint32_t ModeCode(interior::DisplayMode mode) noexcept
{
    switch (mode)
    {
    case interior::DisplayMode::Processed: return 0;
    case interior::DisplayMode::Original: return 1;
    case interior::DisplayMode::Split: return 2;
    }
    return 0;
}

[[nodiscard]] std::array<std::uint32_t, 4> BlitConstants(interior::DisplayMode mode) noexcept
{
    return { ModeCode(mode), std::bit_cast<std::uint32_t>(kSplitPosition), 0u, 0u };
}

[[nodiscard]] D3D12_VIEWPORT ViewportOf(const interior::Extent& e) noexcept
{
    return D3D12_VIEWPORT{ 0.0f, 0.0f, static_cast<float>(e.width.Get()), static_cast<float>(e.height.Get()), 0.0f, 1.0f };
}

[[nodiscard]] D3D12_RECT ScissorOf(const interior::Extent& e) noexcept
{
    return D3D12_RECT{ 0, 0, static_cast<LONG>(e.width.Get()), static_cast<LONG>(e.height.Get()) };
}

void SetViewport(const Gpu& gpu) noexcept
{
    const D3D12_VIEWPORT viewport = ViewportOf(gpu.presenter.extent);
    const D3D12_RECT scissor = ScissorOf(gpu.presenter.extent);
    gpu.list->RSSetViewports(1, &viewport);
    gpu.list->RSSetScissorRects(1, &scissor);
}

void SetBlitTarget(const Gpu& gpu, const interior::Draw& d) noexcept
{
    REQUIRE(IsBackBuffer(d.target));
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(gpu.device, d.target.buffer.Get());
    gpu.list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    SetViewport(gpu);
}

void SetBlitPipeline(const Gpu& gpu, interior::DisplayMode mode, std::uint32_t base) noexcept
{
    const std::array<std::uint32_t, 4> constants = BlitConstants(mode);
    gpu.list->SetGraphicsRootSignature(gpu.pipelines.blitRoot.Get());
    gpu.list->SetPipelineState(gpu.pipelines.blit.Get());
    gpu.list->SetGraphicsRoot32BitConstants(0, static_cast<UINT>(constants.size()), constants.data(), 0);
    gpu.list->SetGraphicsRootDescriptorTable(1, SrvGpuHandle(gpu.device, base));
}

[[nodiscard]] Cursor Drawn(const Gpu& gpu, const interior::Draw& d, std::uint32_t base, const Cursor& c) noexcept
{
    SetBlitTarget(gpu, d);
    SetBlitPipeline(gpu, d.mode, base);
    gpu.list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    gpu.list->DrawInstanced(3, 1, 0, 0);
    return Advanced(c, kBlitSrvCount);
}

[[nodiscard]] StepResult RecordDraw(const Gpu& gpu, interior::FrameSlot slot, const interior::Draw& d, const Cursor& c) noexcept
{
    const std::uint32_t base = HeapIndex(slot, c.descriptor);
    return RequireBudget(c, kBlitSrvCount)
        .and_then([&] { return WriteView(gpu, ViewKind::Srv, d.processed, base); })
        .and_then([&] { return WriteView(gpu, ViewKind::Srv, d.original, base + 1); })
        .transform([&] { return Drawn(gpu, d, base, c); });
}

[[nodiscard]] StepResult SubmitList(const Gpu& gpu, const Cursor& c) noexcept
{
    return Check(gpu.list->Close(), ApiCall::CloseCommandList)
        .and_then([&] { return ExecuteList(gpu.device, gpu.list.Get()); })
        .and_then([&] { return SignalFence(gpu.device, c.fence); })
        .transform([&c](interior::FenceValue signaled) { return WithFence(c, signaled); });
}

#if DSCREEN_HAVE_NVOF
[[nodiscard]] Status<Error> RunOpticalFlow(const Gpu& gpu, const FrameContext& f, const Cursor& c) noexcept
{
    if (!gpu.opticalFlow.has_value())
        return {};
    return ExecuteOpticalFlow(*gpu.opticalFlow, gpu.device, OpticalFlowFrame{ f.number, f.set, f.hasPrevious, c.fence });
}
#else
[[nodiscard]] Status<Error> RunOpticalFlow(const Gpu&, const FrameContext&, const Cursor&) noexcept
{
    return {};
}
#endif

[[nodiscard]] StepResult BetweenPhases(const Gpu& gpu, const FrameContext& f, const Cursor& c) noexcept
{
    return RunOpticalFlow(gpu, f, c).and_then([&] { return OpenList(gpu, f.slot); }).transform([&c] { return c; });
}

[[nodiscard]] StepResult RecordSubmit(const Gpu& gpu, const FrameContext& f, const interior::Submit& s, const Cursor& c) noexcept
{
    switch (s.phase)
    {
    case interior::Phase::One: return SubmitList(gpu, c).and_then([&](const Cursor& n) { return BetweenPhases(gpu, f, n); });
    case interior::Phase::Two: return SubmitList(gpu, c);
    }
    return c;
}

[[nodiscard]] StepResult RecordPresent(const Gpu& gpu, const interior::SessionPlan& plan, const Cursor& c) noexcept
{
    return PresentFrame(gpu.presenter, plan.vsync).and_then([&] { return SignalFence(gpu.device, c.fence); }).transform([&c](interior::FenceValue v) { return WithFence(c, v); });
}

// WAIVER(R7): the real and the simulated interpreter dispatch the same step variant; every arm differs.
[[nodiscard]] StepResult ExecuteStep(const Gpu& gpu, const interior::SessionPlan& plan, const FrameContext& f, const interior::Step& step, const Cursor& c) noexcept
{
    return std::visit(infra::Overloaded{
                          [&](const interior::Transition& t) { return RecordTransition(gpu, t, c); },
                          [&](const interior::Dispatch& d) { return RecordDispatch(gpu, f.slot, d, c); },
                          [&](const interior::CopyBuffer& copy) { return RecordCopy(gpu, copy, c); },
                          [&](const interior::ClearTarget& clear) { return RecordClear(gpu, clear, c); },
                          [&](const interior::EvaluateSr& e) { return RecordSuperResolution(gpu, e, c); },
                          [&](const interior::EvaluateNr& e) { return RecordNeuralRendering(gpu, plan, e, c); },
                          [&](const interior::Draw& d) { return RecordDraw(gpu, f.slot, d, c); },
                          [&](const interior::Submit& s) { return RecordSubmit(gpu, f, s, c); },
                          [&](const interior::Present&) { return RecordPresent(gpu, plan, c); },
                      },
                      step);
}

} // namespace

Status<Error> OpenList(const Gpu& gpu, interior::FrameSlot slot) noexcept
{
    return OpenCommandList(gpu.device, gpu.list.Get(), gpu.allocators[slot.Get()].Get());
}

Result<interior::FenceValue, Error> FlushList(const Gpu& gpu, interior::FenceValue previous) noexcept
{
    return FlushCommandList(gpu.device, gpu.list.Get(), previous);
}

Result<interior::FenceValue, Error> ExecuteSteps(const Gpu& gpu, const interior::SessionPlan& plan, const FrameContext& f, const interior::StepList& steps) noexcept
{
    return OpenList(gpu, f.slot)
        .and_then([&] { return infra::FoldResult(steps.Items(), StepResult(Cursor{ 0, f.fence }), [&](const Cursor& c, const interior::Step& s) { return ExecuteStep(gpu, plan, f, s, c); }); })
        .transform([](const Cursor& c) { return c.fence; });
}

} // namespace real
