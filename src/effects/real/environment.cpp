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

[[nodiscard]] TableResult WithBufferResource(const ResourceTable& t, const GpuDevice& d, const ResourceId& id, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags,
                                             const wchar_t* name) noexcept
{
    return CreateBuffer(d, interior::ByteCountTag::Parse(interior::kStatsBytes), heap, state, flags, name).transform([&](const Com<ID3D12Resource>& buffer) { return WithResource(t, id, buffer); });
}

[[nodiscard]] TableResult WithZeroBuffer(const ResourceTable& t, const GpuDevice& d) noexcept
{
    return CreateBuffer(d, interior::ByteCountTag::Parse(interior::kStatsBytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE, L"Zero source")
        .and_then([&](const Com<ID3D12Resource>& buffer) {
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
    return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, interior::kFrameSlotCount), TableResult(t), [&](const ResourceTable& acc, std::uint32_t slot) {
        return WithBufferResource(acc, d, ReadbackIdOf(slot), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"Statistics readback");
    });
}

// The canvas is the capture device's texture, opened on this device; everything else is created here.
[[nodiscard]] TableResult CoreTextures(const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan, DXGI_FORMAT model, ID3D12Resource* canvas) noexcept
{
    return TableResult(WithResource(t, SimpleId(ResourceKind::Canvas), canvas))
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

[[nodiscard]] TableResult CreateResources(const GpuDevice& d, const SessionPlan& plan, const Presenter& presenter, const interior::LevelExtents& extents, ID3D12Resource* canvas) noexcept
{
    const DXGI_FORMAT model = ModelFormatOf(plan.format);
    return CoreTextures(WithBackBuffers(ResourceTable{}, presenter), d, plan, model, canvas)
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
    return infra::FoldResult(std::views::iota(std::size_t{ 0 }, std::tuple_size_v<Allocators>), Result<Allocators, Error>(Allocators{}), [&](const Allocators& acc, std::size_t i) {
        return CreateAllocator(d).transform([&](const Com<ID3D12CommandAllocator>& allocator) { return infra::WithElement(acc, i, allocator); });
    });
}

[[nodiscard]] Result<Recording, Error> CreateRecording(const GpuDevice& d) noexcept
{
    return CreateAllocators(d).and_then([&](const Allocators& allocators) {
        return CreateClosedCommandList(d, allocators[0].Get()).transform([&](const Com<ID3D12GraphicsCommandList>& list) { return Recording{ allocators, list }; });
    });
}

[[nodiscard]] CaptureSettings CaptureSettingsOf(const SessionPlan& plan, const EnvironmentSettings& settings) noexcept
{
    return CaptureSettings{ plan.captureCursor, settings.surface.captureBorder };
}

[[nodiscard]] Result<Gpu, Error> WithResourcesAndCapture(GpuDevice device, Presenter presenter, const Pipelines& pipelines, const Recording& recording, const SessionPlan& plan,
                                                         const interior::Geometry& geometry, const EnvironmentSettings& settings, const interior::LevelExtents& extents) noexcept
{
    return CreateCapture(device, geometry.sourceRect, plan.source, geometry.source, CaptureSettingsOf(plan, settings)).and_then([&](Capture capture) {
        return CreateResources(device, plan, presenter, extents, capture.sharedCanvas.Get()).transform([&](const ResourceTable& resources) {
            return Gpu{ std::move(device), pipelines, std::move(presenter), std::move(capture), recording.allocators, recording.list, resources, Models{}, OpticalFlowSlot{} };
        });
    });
}

[[nodiscard]] Result<Gpu, Error> AssembledGpu(GpuDevice device, const SessionPlan& plan, const interior::Geometry& geometry, HWND window, const EnvironmentSettings& settings,
                                              const interior::LevelExtents& extents) noexcept
{
    return CreatePresenter(device, window, plan.target).and_then([&](Presenter presenter) {
        return CreatePipelines(device, kSwapChainFormat).and_then([&](const Pipelines& pipelines) {
            return CreateRecording(device).and_then(
                [&](const Recording& recording) { return WithResourcesAndCapture(std::move(device), std::move(presenter), pipelines, recording, plan, geometry, settings, extents); });
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

[[nodiscard]] SessionPlan WithDepthValue(const SessionPlan& plan, interior::DepthValue depth) noexcept
{
    SessionPlan next = plan; // WAIVER(R2): a copy adjusted once, to clear the plane to a new value.
    next.depth = depth;
    return next;
}

[[nodiscard]] Result<interior::FenceValue, Error> ClearedDepth(const Gpu& gpu, const SessionPlan& plan, interior::FenceValue previous) noexcept
{
    return Lookup(gpu.resources, SimpleId(ResourceKind::Depth)).and_then([&](ID3D12Resource* depth) {
        return OpenList(gpu, *kZeroSlot).and_then([&] {
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
    return OpenList(gpu, *kZeroSlot).and_then([&] { return CreateSuperResolution(*c.models.runtime, gpu.list.Get(), *plan.superResolution); }).and_then([&](Feature feature) {
        return FlushList(gpu, c.fence).transform([&](interior::FenceValue fence) {
            return Created{ Models{ std::move(c.models.runtime), std::move(feature), std::move(c.models.neuralRendering), c.models.builtWith }, fence };
        });
    });
}

// Builds the neural rendering feature at the given tuning, replacing whatever was there. The model reads
// its tuning while the feature is built, so a value the operator changes is only honoured by rebuilding.
[[nodiscard]] Result<Created, Error> BuiltNeuralRendering(const Gpu& gpu, const SessionPlan& plan, const interior::NrTuning& tuning, Created c) noexcept
{
    REQUIRE(c.models.runtime.has_value());
    return OpenList(gpu, *kZeroSlot).and_then([&] { return CreateNeuralRendering(*c.models.runtime, gpu.list.Get(), tuning, plan.work); }).and_then([&](Feature feature) {
        return FlushList(gpu, c.fence).transform([&](interior::FenceValue fence) {
            return Created{ Models{ std::move(c.models.runtime), std::move(c.models.superResolution), std::move(feature), tuning }, fence };
        });
    });
}

[[nodiscard]] Result<Created, Error> WithNeuralRendering(const Gpu& gpu, const SessionPlan& plan, Created c) noexcept
{
    if (!plan.neuralRendering)
        return Created{ std::move(c.models), c.fence };
    return BuiltNeuralRendering(gpu, plan, plan.tuning, std::move(c));
}

[[nodiscard]] Gpu WithModels(Gpu g, Models m) noexcept
{
    return Gpu{ std::move(g.device), std::move(g.pipelines), std::move(g.presenter),  std::move(g.capture), std::move(g.allocators), std::move(g.list),
                g.resources,         std::move(m),           std::move(g.opticalFlow) };
}

[[nodiscard]] Gpu WithOpticalFlowSlot(Gpu g, OpticalFlowSlot slot) noexcept
{
    return Gpu{
        std::move(g.device), std::move(g.pipelines), std::move(g.presenter), std::move(g.capture), std::move(g.allocators), std::move(g.list), g.resources, std::move(g.models), std::move(slot)
    };
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
        .and_then([&](interior::FenceValue fence) { return WithSuperResolution(gpu, plan, Created{ Models{ std::move(runtime), std::nullopt, std::nullopt, std::nullopt }, fence }); })
        .and_then([&](Created c) { return WithNeuralRendering(gpu, plan, std::move(c)); })
        .and_then([&](Created c) { return WithOpticalFlow(WithModels(std::move(gpu), std::move(c.models)), plan, c.fence); });
}

[[nodiscard]] Result<std::uint32_t, Error> FinestPixels(const SessionPlan& plan, const interior::LevelExtents& extents) noexcept
{
    const Extent finest = extents.At(plan.finestLevel.Get());
    return infra::CheckedMul(finest.width.Get(), finest.height.Get()).transform_error(FromArithmetic);
}

[[nodiscard]] Result<RealEnvironment, Error> Assembled(Ready r, const SessionPlan& plan, OutputWindow window, std::optional<ControlPanel> panel, const Console& console,
                                                       const EnvironmentSettings& settings, const interior::LevelExtents& extents) noexcept
{
    return FinestPixels(plan, extents).and_then([&](std::uint32_t finest) {
        return Now().transform([&](interior::Instant start) { return RealEnvironment(std::move(r.gpu), plan, std::move(window), std::move(panel), console, settings, finest, r.fence, start); });
    });
}

// --- per frame ----------------------------------------------------------------------------------------------

// What the frame is read from besides the GPU: the output window and, when there is one, the panel.
struct Surroundings
{
    const OutputWindow& window;
    const std::optional<ControlPanel>& panel;
};

struct Prepared
{
    bool fresh;
    interior::BackBufferIndex backBuffer;
    std::optional<interior::Fraction> unmatched;
    interior::Instant now;
    std::optional<interior::Fraction> splitRequest;
    std::optional<PanelReading> reading;
    bool panelClosed;
};

[[nodiscard]] Status<Error> AwaitSlot(const Gpu& gpu, const interior::FrameState& state, interior::FrameSlot slot) noexcept
{
    return WaitForFence(gpu.device, state.slotFences[slot.Get()], interior::MicrosecondsTag::Parse(kFenceTimeoutMicroseconds)).and_then([&] {
        return Check(gpu.allocators[slot.Get()]->Reset(), ApiCall::ResetAllocator);
    });
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

// The panel is the one place a setting lives while it runs, so the hotkeys and the divider drag move its
// controls first and are then read back out of it. Without a panel they go straight into the frame.
[[nodiscard]] bool TogglesTheView(const WindowEvents& events) noexcept
{
    return events.toggleOriginal || events.toggleSplit;
}

void SteerDisplay(const ControlPanel& panel, const WindowEvents& events, const interior::FrameState& state) noexcept
{
    if (TogglesTheView(events))
        ApplyDisplay(panel, interior::NextDisplay(state.display, events.toggleOriginal, events.toggleSplit));
}

void SteerPanel(const ControlPanel& panel, const WindowEvents& events, const std::optional<interior::Fraction>& drag, const interior::FrameState& state) noexcept
{
    SteerDisplay(panel, events, state);
    if (drag.has_value())
        ApplySplit(panel, *drag);
}

[[nodiscard]] std::optional<PanelReading> ReadingOf(const std::optional<ControlPanel>& panel, const WindowEvents& events, const std::optional<interior::Fraction>& drag,
                                                    const interior::FrameState& state) noexcept
{
    if (!panel.has_value())
        return std::nullopt;
    SteerPanel(*panel, events, drag, state);
    return ReadControlPanel(*panel, state.controls);
}

[[nodiscard]] bool PanelWasClosed(const std::optional<ControlPanel>& panel) noexcept
{
    return panel.has_value() && IsPanelClosed(*panel);
}

[[nodiscard]] Result<Prepared, Error> Sampled(const Gpu& gpu, const Surroundings& s, const WindowEvents& events, std::optional<interior::Fraction> unmatched,
                                              const interior::FrameState& state) noexcept
{
    const std::optional<interior::Fraction> drag = SplitRequest(s.window);
    const std::optional<PanelReading> reading = ReadingOf(s.panel, events, drag, state);
    return AcquireFrames(gpu.capture, state.number).and_then([&](bool fresh) {
        return Now().and_then([&](interior::Instant now) {
            return CurrentBackBuffer(gpu.presenter).transform([&](interior::BackBufferIndex index) { return Prepared{ fresh, index, unmatched, now, drag, reading, PanelWasClosed(s.panel) }; });
        });
    });
}

[[nodiscard]] Result<Prepared, Error> Prepare(const Gpu& gpu, const Surroundings& s, const WindowEvents& events, std::uint32_t finestPixels, const interior::FrameState& state,
                                              interior::FrameSlot slot) noexcept
{
    return WaitForNextFrame(gpu.presenter)
        .and_then([&] { return AwaitSlot(gpu, state, slot); })
        .and_then([&] { return ReadUnmatched(gpu, finestPixels, state, slot); })
        .and_then([&](std::optional<interior::Fraction> unmatched) { return Sampled(gpu, s, events, unmatched, state); });
}

[[nodiscard]] std::optional<interior::DisplayMode> DisplayFrom(const std::optional<PanelReading>& reading) noexcept
{
    if (!reading.has_value())
        return std::nullopt;
    return reading->display;
}

[[nodiscard]] std::optional<interior::Fraction> SplitFrom(const Prepared& p) noexcept
{
    if (!p.reading.has_value())
        return p.splitRequest;
    return p.reading->split;
}

[[nodiscard]] std::optional<interior::LiveSettings> ControlsFrom(const std::optional<PanelReading>& reading) noexcept
{
    if (!reading.has_value())
        return std::nullopt;
    return reading->live;
}

[[nodiscard]] bool AsksForANewSession(const std::optional<PanelReading>& reading) noexcept
{
    return reading.has_value() && reading->restartWanted;
}

// Asking for a new session ends this one, which is what starts the new one: the command line the panel
// describes is read and launched once the loop has stopped and the device is idle.
[[nodiscard]] bool LeftThePanel(const Prepared& p) noexcept
{
    return p.panelClosed || AsksForANewSession(p.reading);
}

[[nodiscard]] bool AsksToStop(const WindowEvents& events, const Prepared& p) noexcept
{
    return events.quit || LeftThePanel(p);
}

[[nodiscard]] interior::FrameInput InputOf(const WindowEvents& events, const Prepared& p) noexcept
{
    return interior::FrameInput{
        p.fresh, p.backBuffer, p.unmatched, p.now, events.toggleOriginal, events.toggleSplit, SplitFrom(p), DisplayFrom(p.reading), ControlsFrom(p.reading), AsksToStop(events, p)
    };
}

[[nodiscard]] FrameContext ContextOf(const interior::FrameState& state, interior::FrameSlot slot, interior::FenceValue fence) noexcept
{
    return FrameContext{ state.number, slot, state.currentSet, state.hasPrevious, fence };
}

[[nodiscard]] FrameContext WithFence(const FrameContext& f, interior::FenceValue fence) noexcept
{
    return FrameContext{ f.number, f.slot, f.set, f.hasPrevious, fence };
}

[[nodiscard]] Result<Begun, Error> Begin(const Gpu& gpu, const Surroundings& s, std::uint32_t finestPixels, interior::FenceValue fence, const interior::FrameState& state) noexcept
{
    const interior::FrameSlot slot = interior::SlotOfFrame(state.number);
    return PumpEvents(s.window).and_then([&](const WindowEvents& events) {
        return Prepare(gpu, s, events, finestPixels, state, slot).transform([&](const Prepared& p) { return Begun{ ContextOf(state, slot, fence), InputOf(events, p), p.reading }; });
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

RealEnvironment::RealEnvironment(Gpu gpu, const SessionPlan& plan, OutputWindow window, std::optional<ControlPanel> panel, const Console& console, const EnvironmentSettings& settings,
                                 std::uint32_t finestPixels, interior::FenceValue fence, interior::Instant start) noexcept
    : gpu_(std::move(gpu)), plan_(plan), window_(std::move(window)), panel_(std::move(panel)), console_(console), finestPixels_(finestPixels),
      frame_{ interior::FrameNumberTag::Parse(0), *kZeroSlot, *kZeroSet, false, fence }, stats_{ start, 0, 0 }, applied_(settings), clearedDepth_(plan.depth), restartWanted_(false)
{
}

void MoveIfFound(const OutputWindow& window, const std::optional<interior::ScreenRect>& bounds) noexcept
{
    if (bounds.has_value())
        MoveOutputWindow(window, *bounds);
}

// The capture of a window follows the window itself, so only the overlay showing the answer has to move.
// A resized window is no longer the size the session was planned for, and is cropped to it until restarted.
void RealEnvironment::Followed() noexcept
{
    if (!applied_.followed.has_value())
        return;
    MoveIfFound(window_, BoundsOfWindow(*applied_.followed));
}

Result<FrameStart, Error> RealEnvironment::Began(const Begun& begun) noexcept
{
    Followed();
    return SettledIfRead(begun.reading).and_then([this, &begun] { return Accept(begun); });
}

Result<FrameStart, Error> RealEnvironment::BeginFrame(const interior::FrameState& state) noexcept
{
    return Begin(gpu_, Surroundings{ window_, panel_ }, finestPixels_, frame_.fence, state).and_then([this](const Begun& begun) { return Began(begun); });
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

// Auto keeps whatever the session resolved for the cursor when it started.
[[nodiscard]] bool CursorWanted(const interior::SurfaceSettings& s, bool resolved) noexcept
{
    if (s.cursor == interior::CursorMode::Auto)
        return resolved;
    return s.cursor == interior::CursorMode::On;
}

[[nodiscard]] WindowSettings WindowSettingsOf(const interior::SurfaceSettings& s) noexcept
{
    return WindowSettings{ s.topmost, s.clickThrough, s.displayAffinity, false };
}

[[nodiscard]] infra::Status<Error> ApplySurface(const Gpu& gpu, const OutputWindow& window, const EnvironmentSettings& settings) noexcept
{
    return ApplyCaptureSettings(gpu.capture, CaptureSettings{ CursorWanted(settings.surface, settings.captureCursor), settings.surface.captureBorder }).and_then([&] {
        return ApplyWindowSettings(window, WindowSettingsOf(settings.surface));
    });
}

[[nodiscard]] bool HasBuiltModel(const Models& models) noexcept
{
    return models.neuralRendering.has_value();
}

[[nodiscard]] bool NeedsRebuild(const Models& models, const interior::LiveSettings& controls) noexcept
{
    return HasBuiltModel(models) && models.builtWith != controls.tuning;
}

// The depth plane is a texture cleared once, so a new value means clearing it again.
[[nodiscard]] Status<Error> RealEnvironment::Recleared(interior::DepthValue depth) noexcept
{
    if (depth == clearedDepth_)
        return {};
    return WaitIdle(gpu_.device, frame_.fence)
        .and_then([&](interior::FenceValue idle) { return ClearedDepth(gpu_, WithDepthValue(plan_, depth), idle); })
        .transform([this, depth](interior::FenceValue cleared) {
            frame_ = WithFence(frame_, cleared); // WAIVER(R2): the last signalled fence, replaced whole.
            clearedDepth_ = depth;               // WAIVER(R2): what the plane now holds, replaced whole.
        });
}

// Everything the operator changed since the last frame, put into effect before this one is drawn.
Status<Error> RealEnvironment::SettledIfRead(const std::optional<PanelReading>& reading) noexcept
{
    if (!reading.has_value())
        return {};
    return Settled(*reading);
}

Status<Error> RealEnvironment::Resurfaced(const interior::SurfaceSettings& surface) noexcept
{
    if (surface == applied_.surface)
        return {};
    applied_ = EnvironmentSettings{ surface, applied_.captureCursor, applied_.followed }; // WAIVER(R2): what has been applied, replaced whole.
    return ApplySurface(gpu_, window_, applied_);
}

Status<Error> RealEnvironment::Settled(const PanelReading& reading) noexcept
{
    restartWanted_ = restartWanted_ || reading.restartWanted; // WAIVER(R2): set once, and never unset.
    return Resurfaced(reading.surface).and_then([this, &reading] { return Recleared(reading.live.depth); });
}

[[nodiscard]] bool AsksForANewSession(bool wanted, const std::optional<ControlPanel>& panel) noexcept
{
    return wanted && panel.has_value();
}

std::optional<interior::CommandLine> RealEnvironment::Restart(const interior::Options& options) const noexcept
{
    if (!AsksForANewSession(restartWanted_, panel_))
        return std::nullopt;
    return RestartCommandLine(*panel_, options);
}

Result<ExecutionReport, Error> RealEnvironment::Retuned(const interior::LiveSettings& controls) noexcept
{
    if (!NeedsRebuild(gpu_.models, controls))
        return ExecutionReport{ frame_.fence, 0 };
    return WaitIdle(gpu_.device, frame_.fence)
        .and_then([&](interior::FenceValue idle) { return BuiltNeuralRendering(gpu_, plan_, controls.tuning, Created{ std::move(gpu_.models), idle }); })
        .transform([this](Created rebuilt) {
            gpu_ = WithModels(std::move(gpu_), std::move(rebuilt.models)); // WAIVER(R2): the built model is effect-layer state, replaced whole when the operator retunes.
            frame_ = WithFence(frame_, rebuilt.fence);                     // WAIVER(R2): the last signalled fence, replaced whole.
            return ExecutionReport{ rebuilt.fence, 0 };
        });
}

Result<ExecutionReport, Error> RealEnvironment::Ran(const interior::FramePlan& plan) noexcept
{
    const Result<interior::FenceValue, Error> fence = ExecuteSteps(gpu_, frame_, plan.steps);
    if (!fence.has_value())
        return Fail(fence.error());
    frame_ = WithFence(frame_, *fence); // WAIVER(R2): the last signalled fence is effect-layer state, replaced whole per frame.
    stats_ = Presented(stats_);         // WAIVER(R2): throughput counters, replaced whole per frame.
    return ExecutionReport{ *fence, static_cast<std::uint32_t>(plan.steps.Size()) };
}

Result<ExecutionReport, Error> RealEnvironment::Execute(const interior::FramePlan& plan) noexcept
{
    return Retuned(plan.next.controls).and_then([this, &plan](const ExecutionReport&) { return Ran(plan); });
}

Error RealEnvironment::FromPlanError(interior::PlanFrameError error) noexcept
{
    return Error{ ApiCall::PlanFrame, static_cast<std::uint32_t>(error) };
}

Result<RealEnvironment, Error> CreateEnvironment(GpuDevice device, std::optional<NgxRuntime> runtime, const SessionPlan& plan, const interior::Geometry& geometry, OutputWindow window,
                                                 std::optional<ControlPanel> panel, const EnvironmentSettings& settings, const Console& console) noexcept
{
    return interior::LevelExtentsOf(plan.source, plan.levels).transform_error(FromPyramid).and_then([&](const interior::LevelExtents& extents) {
        return AssembledGpu(std::move(device), plan, geometry, window.handle.get(), settings, extents)
            .and_then([&](Gpu gpu) { return Started(std::move(gpu), std::move(runtime), plan); })
            .and_then([&](Ready r) { return Assembled(std::move(r), plan, std::move(window), std::move(panel), console, settings, extents); });
    });
}

} // namespace real
