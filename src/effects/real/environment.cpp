#include "effects/real/environment.h"

#include "effects/real/clock.h"
#include "infrastructure/checked.h"
#include "infrastructure/fold.h"
#include "infrastructure/text.h"
#include "interior/pyramid.h"

#include <ranges>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;
using interior::Extent;
using interior::ResourceId;
using interior::ResourceKind;
using interior::SessionPlan;
using interior::SimpleId;

constexpr std::uint64_t kReportIntervalMicroseconds = 5000000;
constexpr auto kZeroSlot = interior::FrameSlotTag::Parse(0);
constexpr auto kZeroSet = interior::SetIndexTag::Parse(0);
static_assert(kZeroSlot.has_value() && kZeroSet.has_value());

using TableResult = Result<ResourceTable, Error>;

[[nodiscard]] Error FromPyramid(interior::PyramidError error) noexcept
{
    return Error{ ApiCall::PlanSession, static_cast<std::uint32_t>(error) };
}

[[nodiscard]] Error FromArithmetic(infra::ArithmeticError error) noexcept
{
    return Error{ ApiCall::PlanSession, 100u + static_cast<std::uint32_t>(error) };
}

[[nodiscard]] DXGI_FORMAT ModelFormatOf(interior::ColorFormat format) noexcept
{
    switch (format)
    {
    case interior::ColorFormat::Rgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case interior::ColorFormat::Rgba16f: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    }
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

[[nodiscard]] TextureRequest UavRequest(const Extent& extent, DXGI_FORMAT format, const wchar_t* name) noexcept
{
    return TextureRequest{ extent, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name };
}

[[nodiscard]] TextureRequest CanvasRequest(const SessionPlan& plan) noexcept
{
    return TextureRequest{ plan.source, DXGI_FORMAT_B8G8R8A8_UNORM, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST, L"Capture canvas" };
}

[[nodiscard]] TextureRequest DepthRequest(const SessionPlan& plan) noexcept
{
    return TextureRequest{ plan.source, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, L"Constant depth plane" };
}

[[nodiscard]] TextureRequest FlowOutputRequest(const SessionPlan& plan) noexcept
{
    return TextureRequest{ plan.flowExtent, DXGI_FORMAT_R16G16_SINT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, L"Optical flow output" };
}

[[nodiscard]] std::optional<TextureRequest> SrOutputRequest(const SessionPlan& plan, DXGI_FORMAT model) noexcept
{
    if (!plan.superResolution.has_value())
        return std::nullopt;
    return UavRequest(plan.target, model, L"DLSS Super Resolution output");
}

[[nodiscard]] std::optional<TextureRequest> NrOutputRequest(const SessionPlan& plan, DXGI_FORMAT model) noexcept
{
    if (!plan.neuralRendering)
        return std::nullopt;
    return UavRequest(plan.work, model, L"DLSS 5 Neural Rendering output");
}

[[nodiscard]] bool UsesOpticalFlow(const SessionPlan& plan) noexcept
{
    return plan.motion == interior::MotionBackend::NvOpticalFlow;
}

[[nodiscard]] std::optional<TextureRequest> OpticalFlowRequest(const SessionPlan& plan) noexcept
{
    if (!UsesOpticalFlow(plan))
        return std::nullopt;
    return FlowOutputRequest(plan);
}

[[nodiscard]] TableResult WithTexture(const ResourceTable& t, const GpuDevice& d, const ResourceId& id, const TextureRequest& r) noexcept
{
    return CreateTexture(d, r).transform([&](const Texture& texture) { return WithResource(t, id, texture.resource); });
}

[[nodiscard]] TableResult WithOptionalTexture(const ResourceTable& t, const GpuDevice& d, const ResourceId& id, const std::optional<TextureRequest>& r) noexcept
{
    if (!r.has_value())
        return t;
    return WithTexture(t, d, id, *r);
}

[[nodiscard]] TableResult WithDepth(const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan) noexcept
{
    return CreateClearableTexture(d, DepthRequest(plan), plan.depth.Get()).transform([&](const Texture& texture) { return WithResource(t, SimpleId(ResourceKind::Depth), texture.resource); });
}

[[nodiscard]] TableResult WithBufferResource(const ResourceTable& t, const GpuDevice& d, const ResourceId& id, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                            D3D12_RESOURCE_FLAGS flags, const wchar_t* name) noexcept
{
    return CreateBuffer(d, interior::ByteCountTag::Parse(interior::kStatsBytes), heap, state, flags, name).transform([&](const Com<ID3D12Resource>& buffer) { return WithResource(t, id, buffer); });
}

[[nodiscard]] TableResult WithZeroBuffer(const ResourceTable& t, const GpuDevice& d) noexcept
{
    return CreateBuffer(d, interior::ByteCountTag::Parse(interior::kStatsBytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"Zero source")
        .and_then([&](const Com<ID3D12Resource>& buffer)
        {
            return WriteZeros(buffer.Get(), interior::ByteCountTag::Parse(interior::kStatsBytes)).transform([&] { return WithResource(t, SimpleId(ResourceKind::ZeroBuffer), buffer); });
        });
}

[[nodiscard]] ResourceId LumaIdOf(std::uint32_t set, std::uint32_t level) noexcept
{
    const Result<interior::SetIndex, interior::UnitError> s = interior::SetIndexTag::Parse(set);
    const Result<interior::LevelIndex, interior::UnitError> l = interior::LevelIndexTag::Parse(level);
    ENSURE(s.has_value() && l.has_value());
    return interior::LumaId(*s, *l);
}

[[nodiscard]] ResourceId FlowIdOf(std::uint32_t level) noexcept
{
    const Result<interior::LevelIndex, interior::UnitError> l = interior::LevelIndexTag::Parse(level);
    ENSURE(l.has_value());
    return interior::FlowId(*l);
}

[[nodiscard]] ResourceId BackBufferIdOf(std::uint32_t index) noexcept
{
    const Result<interior::BackBufferIndex, interior::UnitError> b = interior::BackBufferIndexTag::Parse(index);
    ENSURE(b.has_value());
    return interior::BackBufferId(*b);
}

[[nodiscard]] ResourceId ReadbackIdOf(std::uint32_t slot) noexcept
{
    const Result<interior::FrameSlot, interior::UnitError> s = interior::FrameSlotTag::Parse(slot);
    ENSURE(s.has_value());
    return interior::ReadbackId(*s);
}

[[nodiscard]] std::uint32_t LumaLevels(const SessionPlan& plan) noexcept
{
    switch (plan.motion)
    {
    case interior::MotionBackend::BuiltIn: return plan.levels.Get();
    case interior::MotionBackend::NvOpticalFlow: return 1;
    case interior::MotionBackend::None: return 1;
    }
    return 1;
}

[[nodiscard]] std::uint32_t FlowLevels(const SessionPlan& plan) noexcept
{
    switch (plan.motion)
    {
    case interior::MotionBackend::BuiltIn: return plan.levels.Get();
    case interior::MotionBackend::NvOpticalFlow: return 0;
    case interior::MotionBackend::None: return 0;
    }
    return 0;
}

enum class LevelKind : std::uint8_t { Luma, Flow };

[[nodiscard]] ResourceId LevelIdOf(LevelKind kind, std::uint32_t set, std::uint32_t level) noexcept
{
    switch (kind)
    {
    case LevelKind::Luma: return LumaIdOf(set, level);
    case LevelKind::Flow: return FlowIdOf(level);
    }
    return FlowIdOf(level);
}

[[nodiscard]] TextureRequest LevelRequest(LevelKind kind, const Extent& extent) noexcept
{
    switch (kind)
    {
    case LevelKind::Luma: return UavRequest(extent, DXGI_FORMAT_R8_UNORM, L"Luma pyramid");
    case LevelKind::Flow: return UavRequest(extent, DXGI_FORMAT_R16G16_FLOAT, L"Flow pyramid");
    }
    return UavRequest(extent, DXGI_FORMAT_R16G16_FLOAT, L"Flow pyramid");
}

struct Pyramid
{
    LevelKind kind;
    std::uint32_t set;
    std::uint32_t levels;
};

[[nodiscard]] TableResult WithLevel(const ResourceTable& t, const GpuDevice& d, const interior::LevelExtents& extents, const Pyramid& pyramid, std::uint32_t level) noexcept
{
    return WithTexture(t, d, LevelIdOf(pyramid.kind, pyramid.set, level), LevelRequest(pyramid.kind, extents.At(level)));
}

[[nodiscard]] TableResult WithPyramid(const ResourceTable& t, const GpuDevice& d, const interior::LevelExtents& extents, const Pyramid& pyramid) noexcept
{
    return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, pyramid.levels), TableResult(t),
                             [&](const ResourceTable& acc, std::uint32_t level) { return WithLevel(acc, d, extents, pyramid, level); });
}

[[nodiscard]] TableResult WithLuma(const ResourceTable& t, const GpuDevice& d, const interior::LevelExtents& extents, std::uint32_t levels) noexcept
{
    return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, std::uint32_t{ 2 }), TableResult(t),
                             [&](const ResourceTable& acc, std::uint32_t set) { return WithPyramid(acc, d, extents, Pyramid{ LevelKind::Luma, set, levels }); });
}

[[nodiscard]] ResourceTable WithBackBuffers(const ResourceTable& t, const Presenter& presenter) noexcept
{
    return std::ranges::fold_left(std::views::iota(std::uint32_t{ 0 }, interior::kBackBufferCount), t,
                                  [&](const ResourceTable& acc, std::uint32_t i) { return WithResource(acc, BackBufferIdOf(i), presenter.backBuffers[i]); });
}

[[nodiscard]] TableResult WithReadbacks(const ResourceTable& t, const GpuDevice& d) noexcept
{
    return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, interior::kFrameSlotCount), TableResult(t), [&](const ResourceTable& acc, std::uint32_t slot)
    {
        return WithBufferResource(acc, d, ReadbackIdOf(slot), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"Statistics readback");
    });
}

[[nodiscard]] TableResult CoreTextures(const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan, DXGI_FORMAT model) noexcept
{
    return WithTexture(t, d, SimpleId(ResourceKind::Canvas), CanvasRequest(plan))
        .and_then([&](const ResourceTable& n) { return WithTexture(n, d, SimpleId(ResourceKind::ModelColor), UavRequest(plan.source, model, L"Model colour")); })
        .and_then([&](const ResourceTable& n) { return WithDepth(n, d, plan); })
        .and_then([&](const ResourceTable& n) { return WithTexture(n, d, SimpleId(ResourceKind::MotionVectors), UavRequest(plan.source, DXGI_FORMAT_R16G16_FLOAT, L"Motion vectors")); });
}

[[nodiscard]] TableResult ModelOutputs(const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan, DXGI_FORMAT model) noexcept
{
    return WithOptionalTexture(t, d, SimpleId(ResourceKind::SrOutput), SrOutputRequest(plan, model))
        .and_then([&](const ResourceTable& n) { return WithOptionalTexture(n, d, SimpleId(ResourceKind::NrOutput), NrOutputRequest(plan, model)); })
        .and_then([&](const ResourceTable& n) { return WithOptionalTexture(n, d, SimpleId(ResourceKind::OpticalFlowOutput), OpticalFlowRequest(plan)); });
}

[[nodiscard]] TableResult Buffers(const ResourceTable& t, const GpuDevice& d) noexcept
{
    return WithBufferResource(t, d, SimpleId(ResourceKind::Stats), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, L"Match statistics")
        .and_then([&](const ResourceTable& n) { return WithZeroBuffer(n, d); })
        .and_then([&](const ResourceTable& n) { return WithReadbacks(n, d); });
}

[[nodiscard]] TableResult Pyramids(const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan, const interior::LevelExtents& extents) noexcept
{
    return WithLuma(t, d, extents, LumaLevels(plan)).and_then([&](const ResourceTable& n) { return WithPyramid(n, d, extents, Pyramid{ LevelKind::Flow, 0, FlowLevels(plan) }); });
}

[[nodiscard]] TableResult CreateResources(const GpuDevice& d, const SessionPlan& plan, const Presenter& presenter, const interior::LevelExtents& extents) noexcept
{
    const DXGI_FORMAT model = ModelFormatOf(plan.format);
    return CoreTextures(WithBackBuffers(ResourceTable{}, presenter), d, plan, model)
        .and_then([&](const ResourceTable& n) { return ModelOutputs(n, d, plan, model); })
        .and_then([&](const ResourceTable& n) { return Buffers(n, d); })
        .and_then([&](const ResourceTable& n) { return Pyramids(n, d, plan, extents); });
}

// --- start-up ---------------------------------------------------------------------------------------------

struct Recording
{
    Allocators allocators;
    Com<ID3D12GraphicsCommandList> list;
};

[[nodiscard]] Result<Allocators, Error> CreateAllocators(const GpuDevice& d) noexcept
{
    return infra::FoldResult(std::views::iota(std::size_t{ 0 }, std::tuple_size_v<Allocators>), Result<Allocators, Error>(Allocators{}), [&](const Allocators& acc, std::size_t i)
    {
        return CreateAllocator(d).transform([&](const Com<ID3D12CommandAllocator>& allocator) { return infra::WithElement(acc, i, allocator); });
    });
}

[[nodiscard]] Result<Recording, Error> CreateRecording(const GpuDevice& d) noexcept
{
    return CreateAllocators(d).and_then([&](const Allocators& allocators)
    {
        return CreateClosedCommandList(d, allocators[0].Get()).transform([&](const Com<ID3D12GraphicsCommandList>& list) { return Recording{ allocators, list }; });
    });
}

[[nodiscard]] CaptureSettings CaptureSettingsOf(const SessionPlan& plan, const EnvironmentSettings& settings) noexcept
{
    return CaptureSettings{ plan.captureCursor, settings.captureBorder };
}

[[nodiscard]] Result<Gpu, Error> WithResourcesAndCapture(GpuDevice device, Presenter presenter, const Pipelines& pipelines, const Recording& recording, const SessionPlan& plan,
                                                        const interior::Geometry& geometry, const EnvironmentSettings& settings, const interior::LevelExtents& extents) noexcept
{
    return CreateResources(device, plan, presenter, extents).and_then([&](const ResourceTable& resources)
    {
        return Lookup(resources, SimpleId(ResourceKind::Canvas)).and_then([&](ID3D12Resource* canvas)
        {
            return CreateCapture(device, canvas, geometry.sourceRect, plan.source, geometry.source, CaptureSettingsOf(plan, settings)).transform([&](Capture capture)
            {
                return Gpu{ std::move(device), pipelines, std::move(presenter), std::move(capture), recording.allocators, recording.list, resources, Models{}, OpticalFlowSlot{} };
            });
        });
    });
}

[[nodiscard]] Result<Gpu, Error> AssembledGpu(GpuDevice device, const SessionPlan& plan, const interior::Geometry& geometry, HWND window, const EnvironmentSettings& settings,
                                             const interior::LevelExtents& extents) noexcept
{
    return CreatePresenter(device, window, plan.target).and_then([&](Presenter presenter)
    {
        return CreatePipelines(device, kSwapChainFormat).and_then([&](const Pipelines& pipelines)
        {
            return CreateRecording(device).and_then([&](const Recording& recording)
            {
                return WithResourcesAndCapture(std::move(device), std::move(presenter), pipelines, recording, plan, geometry, settings, extents);
            });
        });
    });
}

void RecordDepthClear(const Gpu& gpu, ID3D12Resource* depth, interior::DepthValue value) noexcept
{
    const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(gpu.device, kDepthRtvSlot);
    const std::array<float, 4> clear{ value.Get(), 0.0f, 0.0f, 0.0f };
    CreateRtv(gpu.device, depth, DXGI_FORMAT_R32_FLOAT, rtv);
    gpu.list->ClearRenderTargetView(rtv, clear.data(), 0, nullptr);
    RecordBarrier(gpu.list.Get(), depth, interior::ResourceState::RenderTarget, interior::ResourceState::ShaderRead);
}

[[nodiscard]] Result<interior::FenceValue, Error> ClearedDepth(const Gpu& gpu, const SessionPlan& plan, interior::FenceValue previous) noexcept
{
    return Lookup(gpu.resources, SimpleId(ResourceKind::Depth)).and_then([&](ID3D12Resource* depth)
    {
        return OpenList(gpu, *kZeroSlot).and_then([&]
        {
            RecordDepthClear(gpu, depth, plan.depth);
            return FlushList(gpu, previous);
        });
    });
}

struct Created
{
    Models models;
    interior::FenceValue fence;
};

[[nodiscard]] Result<Created, Error> WithSuperResolution(const Gpu& gpu, const SessionPlan& plan, Created c) noexcept
{
    if (!plan.superResolution.has_value())
        return Created{ std::move(c.models), c.fence };
    REQUIRE(c.models.runtime.has_value());
    return OpenList(gpu, *kZeroSlot)
        .and_then([&] { return CreateSuperResolution(*c.models.runtime, gpu.list.Get(), *plan.superResolution); })
        .and_then([&](Feature feature)
        {
            return FlushList(gpu, c.fence).transform([&](interior::FenceValue fence) { return Created{ Models{ std::move(c.models.runtime), std::move(feature), std::move(c.models.neuralRendering) }, fence }; });
        });
}

[[nodiscard]] Result<Created, Error> WithNeuralRendering(const Gpu& gpu, const SessionPlan& plan, Created c) noexcept
{
    if (!plan.neuralRendering)
        return Created{ std::move(c.models), c.fence };
    REQUIRE(c.models.runtime.has_value());
    return OpenList(gpu, *kZeroSlot)
        .and_then([&] { return CreateNeuralRendering(*c.models.runtime, gpu.list.Get(), plan.tuning, plan.work); })
        .and_then([&](Feature feature)
        {
            return FlushList(gpu, c.fence).transform([&](interior::FenceValue fence) { return Created{ Models{ std::move(c.models.runtime), std::move(c.models.superResolution), std::move(feature) }, fence }; });
        });
}

[[nodiscard]] Gpu WithModels(Gpu g, Models m) noexcept
{
    return Gpu{ std::move(g.device), std::move(g.pipelines), std::move(g.presenter), std::move(g.capture), std::move(g.allocators), std::move(g.list), g.resources, std::move(m), std::move(g.opticalFlow) };
}

[[nodiscard]] Gpu WithOpticalFlowSlot(Gpu g, OpticalFlowSlot slot) noexcept
{
    return Gpu{ std::move(g.device), std::move(g.pipelines), std::move(g.presenter), std::move(g.capture), std::move(g.allocators), std::move(g.list), g.resources, std::move(g.models), std::move(slot) };
}

#if DSCREEN_HAVE_NVOF
[[nodiscard]] Result<OpticalFlowSlot, Error> OpticalFlowFor(const Gpu& gpu, const SessionPlan& plan) noexcept
{
    if (!UsesOpticalFlow(plan))
        return OpticalFlowSlot{};
    return CreateOpticalFlow(gpu.device, plan, gpu.resources).transform([](OpticalFlow flow) { return OpticalFlowSlot{ std::move(flow) }; });
}
#else
[[nodiscard]] Result<OpticalFlowSlot, Error> OpticalFlowFor(const Gpu&, const SessionPlan& plan) noexcept
{
    if (UsesOpticalFlow(plan))
        return Fail(Error{ ApiCall::OpticalFlowUnavailable, 0 });
    return OpticalFlowSlot{};
}
#endif

struct Ready
{
    Gpu gpu;
    interior::FenceValue fence;
};

[[nodiscard]] Result<Ready, Error> WithOpticalFlow(Gpu gpu, const SessionPlan& plan, interior::FenceValue fence) noexcept
{
    return OpticalFlowFor(gpu, plan).transform([&](OpticalFlowSlot slot) { return Ready{ WithOpticalFlowSlot(std::move(gpu), std::move(slot)), fence }; });
}

[[nodiscard]] Result<Ready, Error> Started(Gpu gpu, std::optional<NgxRuntime> runtime, const SessionPlan& plan) noexcept
{
    return ClearedDepth(gpu, plan, interior::FenceValueTag::Parse(0))
        .and_then([&](interior::FenceValue fence) { return WithSuperResolution(gpu, plan, Created{ Models{ std::move(runtime), std::nullopt, std::nullopt }, fence }); })
        .and_then([&](Created c) { return WithNeuralRendering(gpu, plan, std::move(c)); })
        .and_then([&](Created c) { return WithOpticalFlow(WithModels(std::move(gpu), std::move(c.models)), plan, c.fence); });
}

[[nodiscard]] Result<std::uint32_t, Error> FinestPixels(const SessionPlan& plan, const interior::LevelExtents& extents) noexcept
{
    const Extent finest = extents.At(plan.finestLevel.Get());
    return infra::CheckedMul(finest.width.Get(), finest.height.Get()).transform_error(FromArithmetic);
}

[[nodiscard]] Result<RealEnvironment, Error> Assembled(Ready r, const SessionPlan& plan, OutputWindow window, const Console& console, const interior::LevelExtents& extents) noexcept
{
    return FinestPixels(plan, extents).and_then([&](std::uint32_t finest)
    {
        return Now().transform([&](interior::Instant start) { return RealEnvironment(std::move(r.gpu), plan, std::move(window), console, finest, r.fence, start); });
    });
}

// --- per frame ----------------------------------------------------------------------------------------------

struct Prepared
{
    bool fresh;
    interior::BackBufferIndex backBuffer;
    std::optional<interior::Fraction> unmatched;
    interior::Instant now;
};

[[nodiscard]] Status<Error> AwaitSlot(const Gpu& gpu, const interior::FrameState& state, interior::FrameSlot slot) noexcept
{
    return WaitForFence(gpu.device, state.slotFences[slot.Get()], interior::MicrosecondsTag::Parse(kFenceTimeoutMicroseconds))
        .and_then([&] { return Check(gpu.allocators[slot.Get()]->Reset(), ApiCall::ResetAllocator); });
}

[[nodiscard]] Result<interior::Fraction, Error> FractionOf(std::uint32_t count, std::uint32_t total) noexcept
{
    if (count > total)
        return Fail(Error{ ApiCall::StatsOutOfRange, count });
    return interior::FractionTag::Parse(static_cast<float>(count) / static_cast<float>(total)).transform_error([count](interior::UnitError) { return Error{ ApiCall::StatsOutOfRange, count }; });
}

[[nodiscard]] Result<std::optional<interior::Fraction>, Error> ReadUnmatched(const Gpu& gpu, std::uint32_t finestPixels, const interior::FrameState& state, interior::FrameSlot slot) noexcept
{
    if (!state.statsPending[slot.Get()])
        return std::optional<interior::Fraction>{};
    return Lookup(gpu.resources, interior::ReadbackId(slot))
        .and_then(ReadFirstUInt)
        .and_then([finestPixels](std::uint32_t count) { return FractionOf(count, finestPixels); })
        .transform([](interior::Fraction f) { return std::optional<interior::Fraction>{ f }; });
}

[[nodiscard]] Result<Prepared, Error> Sampled(const Gpu& gpu, std::optional<interior::Fraction> unmatched) noexcept
{
    return AcquireFrames(gpu.capture).and_then([&](bool fresh)
    {
        return Now().and_then([&](interior::Instant now)
        {
            return CurrentBackBuffer(gpu.presenter).transform([&](interior::BackBufferIndex index) { return Prepared{ fresh, index, unmatched, now }; });
        });
    });
}

[[nodiscard]] Result<Prepared, Error> Prepare(const Gpu& gpu, std::uint32_t finestPixels, const interior::FrameState& state, interior::FrameSlot slot) noexcept
{
    return WaitForNextFrame(gpu.presenter)
        .and_then([&] { return AwaitSlot(gpu, state, slot); })
        .and_then([&] { return ReadUnmatched(gpu, finestPixels, state, slot); })
        .and_then([&](std::optional<interior::Fraction> unmatched) { return Sampled(gpu, unmatched); });
}

[[nodiscard]] interior::FrameInput InputOf(const WindowEvents& events, const Prepared& p) noexcept
{
    return interior::FrameInput{ p.fresh, p.backBuffer, p.unmatched, p.now, events.toggleOriginal, events.toggleSplit, events.quit };
}

[[nodiscard]] FrameContext ContextOf(const interior::FrameState& state, interior::FrameSlot slot, interior::FenceValue fence) noexcept
{
    return FrameContext{ state.number, slot, state.currentSet, state.hasPrevious, fence };
}

[[nodiscard]] FrameContext WithFence(const FrameContext& f, interior::FenceValue fence) noexcept
{
    return FrameContext{ f.number, f.slot, f.set, f.hasPrevious, fence };
}

[[nodiscard]] Result<Begun, Error> Begin(const Gpu& gpu, const OutputWindow& window, std::uint32_t finestPixels, interior::FenceValue fence, const interior::FrameState& state) noexcept
{
    const interior::FrameSlot slot = interior::SlotOfFrame(state.number);
    return PumpEvents(window).and_then([&](const WindowEvents& events)
    {
        return Prepare(gpu, finestPixels, state, slot).transform([&](const Prepared& p) { return Begun{ ContextOf(state, slot, fence), InputOf(events, p) }; });
    });
}

[[nodiscard]] bool IsReportDue(const Statistics& s, interior::Instant now) noexcept
{
    return now.Get() - s.lastReport.Get() >= kReportIntervalMicroseconds;
}

[[nodiscard]] std::uint32_t CountOf(bool flag) noexcept
{
    return flag ? 1u : 0u;
}

[[nodiscard]] Statistics Counted(const Statistics& s, bool fresh) noexcept
{
    return Statistics{ s.lastReport, s.processed + CountOf(fresh), s.presented };
}

[[nodiscard]] Statistics Presented(const Statistics& s) noexcept
{
    return Statistics{ s.lastReport, s.processed, s.presented + 1 };
}

[[nodiscard]] Statistics Restarted(interior::Instant now) noexcept
{
    return Statistics{ now, 0, 0 };
}

[[nodiscard]] Status<Error> LogThroughput(const Console& console, const Statistics& s, interior::Instant now) noexcept
{
    const double seconds = static_cast<double>(now.Get() - s.lastReport.Get()) / 1000000.0;
    return Log(console, interior::LogLevel::Info, infra::Formatted<120>("{:.1f} processed fps, {:.1f} presented fps", s.processed / seconds, s.presented / seconds).Get());
}

[[nodiscard]] Result<Statistics, Error> Reported(const Console& console, const Statistics& s, const interior::FrameInput& input) noexcept
{
    if (!IsReportDue(s, input.now))
        return Counted(s, input.freshCapture);
    return LogThroughput(console, s, input.now).transform([&] { return Counted(Restarted(input.now), input.freshCapture); });
}

} // namespace

RealEnvironment::RealEnvironment(Gpu gpu, const SessionPlan& plan, OutputWindow window, const Console& console, std::uint32_t finestPixels, interior::FenceValue fence,
                                 interior::Instant start) noexcept
    : gpu_(std::move(gpu)), plan_(plan), window_(std::move(window)), console_(console), finestPixels_(finestPixels),
      frame_{ interior::FrameNumberTag::Parse(0), *kZeroSlot, *kZeroSet, false, fence }, stats_{ start, 0, 0 }
{
}

Result<FrameStart, Error> RealEnvironment::BeginFrame(const interior::FrameState& state) noexcept
{
    const Result<Begun, Error> begun = Begin(gpu_, window_, finestPixels_, frame_.fence, state);
    if (!begun.has_value())
        return Fail(begun.error());
    return Accept(*begun);
}

Result<FrameStart, Error> RealEnvironment::Accept(const Begun& begun) noexcept
{
    const Result<Statistics, Error> stats = Reported(console_, stats_, begun.input);
    if (!stats.has_value())
        return Fail(stats.error());
    frame_ = begun.frame; // WAIVER(R2): the frame context is the effect layer's state, replaced whole per frame.
    stats_ = *stats;      // WAIVER(R2): throughput counters, replaced whole per frame.
    return FrameStart{ begun.input };
}

Result<ExecutionReport, Error> RealEnvironment::Execute(const interior::FramePlan& plan) noexcept
{
    const Result<interior::FenceValue, Error> fence = ExecuteSteps(gpu_, plan_, frame_, plan.steps);
    if (!fence.has_value())
        return Fail(fence.error());
    frame_ = WithFence(frame_, *fence); // WAIVER(R2): the last signalled fence is effect-layer state, replaced whole per frame.
    stats_ = Presented(stats_);          // WAIVER(R2): throughput counters, replaced whole per frame.
    return ExecutionReport{ *fence, static_cast<std::uint32_t>(plan.steps.Size()) };
}

Error RealEnvironment::FromPlanError(interior::PlanFrameError error) noexcept
{
    return Error{ ApiCall::PlanFrame, static_cast<std::uint32_t>(error) };
}

Result<RealEnvironment, Error> CreateEnvironment(GpuDevice device, std::optional<NgxRuntime> runtime, const SessionPlan& plan, const interior::Geometry& geometry, OutputWindow window,
                                                 const EnvironmentSettings& settings, const Console& console) noexcept
{
    return interior::LevelExtentsOf(plan.source, plan.levels).transform_error(FromPyramid).and_then([&](const interior::LevelExtents& extents)
    {
        return AssembledGpu(std::move(device), plan, geometry, window.handle.get(), settings, extents)
            .and_then([&](Gpu gpu) { return Started(std::move(gpu), std::move(runtime), plan); })
            .and_then([&](Ready r) { return Assembled(std::move(r), plan, std::move(window), console, extents); });
    });
}

} // namespace real
