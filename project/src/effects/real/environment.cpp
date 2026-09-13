#include "effects/real/environment.h"

#include "effects/real/clock.h"
#include "effects/real/exclusion.h"
#include "infrastructure/checked.h"
#include "infrastructure/fold.h"
#include "infrastructure/text.h"
#include "interior/pyramid.h"

#include <algorithm>
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
constexpr auto kOneSet = interior::SetIndexTag::Parse(1);
static_assert(kZeroSlot.has_value() && kZeroSet.has_value() && kOneSet.has_value());

using TableResult = Result<ResourceTable, Error>;

[[nodiscard]] bool UsesOpticalFlow(const SessionPlan& plan) noexcept
{
    return plan.motion == interior::MotionBackend::NvOpticalFlow;
}

enum class LevelKind : std::uint8_t { Luma, Flow };

struct Pyramid
{
    LevelKind kind;
    std::uint32_t set;
    std::uint32_t levels;
};

// --- start-up ---------------------------------------------------------------------------------------------

struct Recording
{
    Allocators allocators;
    Com<ID3D12GraphicsCommandList> list;
};

[[nodiscard]] Result<interior::FenceValue, Error> ClearedDepth(const Gpu& gpu, const SessionPlan& plan, interior::FenceValue previous) noexcept
{
    static constexpr auto RecordDepthClear = [](const Gpu& gpu, ID3D12Resource* depth, interior::DepthValue value) noexcept -> void {
        const D3D12_CPU_DESCRIPTOR_HANDLE rtv = RtvHandle(gpu.device, kDepthRtvSlot);
        const std::array<float, 4> clear{ value.Get(), 0.0f, 0.0f, 0.0f };
        CreateRtv(gpu.device, depth, DXGI_FORMAT_R32_FLOAT, rtv);
        gpu.list->ClearRenderTargetView(rtv, clear.data(), 0, nullptr);
        RecordBarrier(gpu.list.Get(), depth, interior::ResourceState::RenderTarget, interior::ResourceState::ShaderRead);
    };
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

struct BuiltPass
{
    Feature feature;
    interior::FenceValue fence;
};

struct Building
{
    Passes passes;
    interior::FenceValue fence;
};

// Builds the neural rendering instances at the given tuning, one per pass, replacing whatever was there.
// The model reads its tuning while an instance is built, so a value the operator changes is only honoured
// by rebuilding, and a changed count is a different number of instances.
[[nodiscard]] Result<Created, Error> BuiltNeuralRendering(const Gpu& gpu, const SessionPlan& plan, const BuiltModel& wanted, Created c) noexcept
{
    static constexpr auto BuiltPassOf = [] [[nodiscard]] (const Gpu& gpu, const NgxRuntime& runtime, const SessionPlan& plan, const interior::NrTuning& tuning,
                                                          interior::FenceValue after) noexcept -> Result<BuiltPass, Error> {
        return OpenList(gpu, *kZeroSlot).and_then([&] { return CreateNeuralRendering(runtime, gpu.list.Get(), tuning, plan.work); }).and_then([&](Feature feature) {
            return FlushList(gpu, after).transform([&feature](interior::FenceValue fence) { return BuiltPass{ std::move(feature), fence }; });
        });
    };

    static constexpr auto Holding = [] [[nodiscard]] (Passes so, Feature feature) noexcept -> Passes {
        so.push_back(std::move(feature)); // WAIVER(R2): the list is this call's own, grown by one and handed back.
        return so;
    };

    static constexpr auto WithPass = [] [[nodiscard]] (const Gpu& gpu, const NgxRuntime& runtime, const SessionPlan& plan, const interior::NrTuning& tuning,
                                                       Building so) noexcept -> Result<Building, Error> {
        return BuiltPassOf(gpu, runtime, plan, tuning, so.fence).transform([&](BuiltPass built) { return Building{ Holding(std::move(so.passes), std::move(built.feature)), built.fence }; });
    };
    REQUIRE(c.models.runtime.has_value());
    const NgxRuntime& runtime = *c.models.runtime;
    return infra::FoldOwned(std::views::iota(std::uint32_t{ 0 }, wanted.passes.Get()), Result<Building, Error>(Building{ Passes{}, c.fence }),
                            [&](Building so, std::uint32_t) { return WithPass(gpu, runtime, plan, wanted.tuning, std::move(so)); })
        .transform([&](Building built) { return Created{ Models{ std::move(c.models.runtime), std::move(c.models.superResolution), std::move(built.passes), wanted }, built.fence }; });
}

[[nodiscard]] Gpu WithModels(Gpu g, Models m) noexcept
{
    return Gpu{ std::move(g.device), std::move(g.pipelines), std::move(g.presenter),  std::move(g.capture), std::move(g.allocators), std::move(g.list),
                g.resources,         std::move(m),           std::move(g.opticalFlow) };
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

[[nodiscard]] Result<Ready, Error> Started(Gpu gpu, std::optional<NgxRuntime> runtime, const SessionPlan& plan) noexcept
{
    static constexpr auto WithSuperResolution = [] [[nodiscard]] (const Gpu& gpu, const SessionPlan& plan, Created c) noexcept -> Result<Created, Error> {
        if (!plan.superResolution.has_value())
            return Created{ std::move(c.models), c.fence };
        REQUIRE(c.models.runtime.has_value());
        return OpenList(gpu, *kZeroSlot).and_then([&] { return CreateSuperResolution(*c.models.runtime, gpu.list.Get(), *plan.superResolution); }).and_then([&](Feature feature) {
            return FlushList(gpu, c.fence).transform([&](interior::FenceValue fence) {
                return Created{ Models{ std::move(c.models.runtime), std::move(feature), std::move(c.models.neuralRendering), c.models.builtWith }, fence };
            });
        });
    };

    static constexpr auto WithNeuralRendering = [] [[nodiscard]] (const Gpu& gpu, const SessionPlan& plan, Created c) noexcept -> Result<Created, Error> {
        if (!plan.neuralRendering)
            return Created{ std::move(c.models), c.fence };
        return BuiltNeuralRendering(gpu, plan, BuiltModel{ plan.tuning, plan.passes }, std::move(c));
    };

    static constexpr auto WithOpticalFlow = [] [[nodiscard]] (Gpu gpu, const SessionPlan& plan, interior::FenceValue fence) noexcept -> Result<Ready, Error> {
        static constexpr auto WithOpticalFlowSlot = [] [[nodiscard]] (Gpu g, OpticalFlowSlot slot) noexcept -> Gpu {
            return Gpu{ std::move(g.device), std::move(g.pipelines), std::move(g.presenter), std::move(g.capture), std::move(g.allocators), std::move(g.list),
                        g.resources,         std::move(g.models),    std::move(slot) };
        };
        return OpticalFlowFor(gpu, plan).transform([&](OpticalFlowSlot slot) { return Ready{ WithOpticalFlowSlot(std::move(gpu), std::move(slot)), fence }; });
    };
    return ClearedDepth(gpu, plan, interior::FenceValueTag::Parse(0))
        .and_then([&](interior::FenceValue fence) { return WithSuperResolution(gpu, plan, Created{ Models{ std::move(runtime), std::nullopt, Passes{}, std::nullopt }, fence }); })
        .and_then([&](Created c) { return WithNeuralRendering(gpu, plan, std::move(c)); })
        .and_then([&](Created c) { return WithOpticalFlow(WithModels(std::move(gpu), std::move(c.models)), plan, c.fence); });
}

// --- per frame ----------------------------------------------------------------------------------------------

// What the frame is read from besides the GPU: the output window and, when there is one, the panel.
struct Surroundings
{
    const OutputWindow& window;
    const ControlPanel* panel;
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

[[nodiscard]] FrameContext WithFence(const FrameContext& f, interior::FenceValue fence) noexcept
{
    return FrameContext{ f.number, f.slot, f.set, f.hasPrevious, fence };
}

[[nodiscard]] interior::CommandLine ShapeOf(const ControlPanel* panel) noexcept
{
    return panel == nullptr ? interior::CommandLine{} : SessionShape(*panel);
}

} // namespace

RealEnvironment::RealEnvironment(HeldFiles held, Gpu gpu, const SessionPlan& plan, OutputWindow window, const ControlPanel* panel, const Console& console, const EnvironmentSettings& settings,
                                 const interior::Options& options, std::uint32_t finestPixels, interior::FenceValue fence, interior::Instant start) noexcept
    : held_(std::move(held)), gpu_(std::move(gpu)), plan_(plan), window_(std::move(window)), panel_(panel), console_(console), finestPixels_(finestPixels),
      frame_{ interior::FrameNumberTag::Parse(0), *kZeroSlot, *kZeroSet, false, fence }, stats_{ start, 0, 0 }, applied_(settings), clearedDepth_(plan.depth), restartWanted_(false), resized_(false),
      pending_(plan.source), since_(start), options_(options), built_(ShapeOf(panel)), wanted_(built_), asked_(start), abandoned_(false), waiting_(false), snapshot_(std::nullopt),
      recording_(std::nullopt), comparison_(std::nullopt), cursor_(std::nullopt), writer_(std::make_unique<PngWriter>()), now_(start)
{
}

// A window dragged by its corner changes size many times a second, and none of those is a session worth
// building. The session is built again once the size has stopped changing and stayed still for a moment.
constexpr std::uint64_t kSettleMicroseconds = 300000;

bool RealEnvironment::HasSettled(interior::Instant now) const noexcept
{
    return now.Get() - since_.Get() >= kSettleMicroseconds;
}

void RealEnvironment::Noticed(const interior::Extent& size, interior::Instant now) noexcept
{
    pending_ = size; // WAIVER(R2): the size last seen, replaced whole.
    since_ = now;    // WAIVER(R2): when it was first seen, replaced whole with it.
}

[[nodiscard]] bool RealEnvironment::AsksForARebuild(const interior::Extent& size, interior::Instant now) const noexcept
{
    return size != plan_.source && HasSettled(now);
}

void RealEnvironment::Held(const interior::Extent& size, interior::Instant now) noexcept
{
    if (AsksForARebuild(size, now))
        resized_ = true; // WAIVER(R2): set once, and never unset.
}

void RealEnvironment::Settling(const interior::Extent& size, interior::Instant now) noexcept
{
    if (size != pending_)
        Noticed(size, now);
    else
        Held(size, now);
}

// Where the overlay belongs is asked of the stack rather than remembered: the program that owns the
// window can raise it over the overlay whenever it likes, and a refused placement is then tried again.
void RealEnvironment::Placed(const interior::ScreenRect& bounds) noexcept
{
    static constexpr auto FollowedHandle = [] [[nodiscard]] (const std::optional<interior::MonitorHandle>& followed) noexcept -> HWND {
        return followed.has_value() ? reinterpret_cast<HWND>(followed->Get()) : nullptr;
    };
    HWND above = FollowedHandle(applied_.followed);
    if (IsOutputWindowPlaced(window_, bounds, above))
        return;
    MoveOutputWindowAbove(window_, bounds, above);
}

// A window that only moves needs the overlay moved and nothing else, because the capture follows it. Every
// size below this was settled when the session was planned, so a resize asks for the session to be rebuilt.
void RealEnvironment::Moved(const interior::ScreenRect& bounds, interior::Instant now) noexcept
{
    static constexpr auto SizeOf = [] [[nodiscard]] (const interior::ScreenRect& bounds, const interior::Extent& absent) noexcept -> interior::Extent {
        return interior::ExtentOf(bounds).value_or(absent);
    };
    Placed(bounds);
    Settling(SizeOf(bounds, plan_.source), now);
}

void RealEnvironment::FollowedTo(const std::optional<interior::ScreenRect>& bounds, interior::Instant now) noexcept
{
    if (bounds.has_value())
        Moved(*bounds, now);
}

void RealEnvironment::Abandon() noexcept
{
    // The panel is the operator's, so what it shows has to agree: with the window gone, the crosshair holds
    // nothing and the source it names is what the next session is built from.
    static constexpr auto LetGoOfWindow = [](const ControlPanel* panel) noexcept -> void {
        if (panel == nullptr)
            return;
        ReleaseWindow(*panel);
    };
    abandoned_ = true; // WAIVER(R2): set once, and never unset.
    LetGoOfWindow(panel_);
}

// A window closed has nothing left to capture and never will, so the session ends and the next takes the
// monitor the source names. One minimised or hidden may come back, so the session waits for it with the
// overlay out of sight rather than leaving it over where the window was, and carries on when it shows.
Status<Error> RealEnvironment::Watched(interior::MonitorHandle window, interior::Instant now) noexcept
{
    if (!IsWindowThere(window))
    {
        Abandon();
        return {};
    }
    if (!IsWindowShowing(window))
        return Waiting();
    return Resumed().transform([this, window, now] { FollowedTo(BoundsOfWindow(window), now); });
}

Status<Error> RealEnvironment::Waiting() noexcept
{
    if (waiting_)
        return {};
    waiting_ = true; // WAIVER(R2): set while the window is away, cleared when it is back.
    HideOutputWindow(window_);
    return Log(console_, interior::LogLevel::Info, "the window is minimised or hidden; waiting for it to come back, which the panel's Bring the window back button does");
}

Status<Error> RealEnvironment::Resumed() noexcept
{
    if (!waiting_)
        return {};
    waiting_ = false; // WAIVER(R2): cleared when the window is back.
    ShowOutputWindow(window_);
    return Log(console_, interior::LogLevel::Info, "the window is back");
}

Status<Error> RealEnvironment::BroughtBack(const PanelReading& reading) noexcept
{
    if (!reading.restoreWindow || !applied_.followed.has_value())
        return {};
    BringWindowBack(*applied_.followed);
    return Log(console_, interior::LogLevel::Info, "bringing the window back");
}

void RealEnvironment::ShowFollowing() noexcept
{
    if (panel_ != nullptr)
        ApplyFollowing(*panel_, applied_.followed.has_value());
}

[[nodiscard]] HWND PanelWindow(const ControlPanel* panel) noexcept;

void RealEnvironment::Behind(HWND front) noexcept
{
    static constexpr auto IsAlreadyBehind = [] [[nodiscard]] (const OutputWindow& window, HWND front) noexcept -> bool { return front == nullptr || IsOutputWindowBehind(window, front); };
    if (IsAlreadyBehind(window_, front))
        return;
    KeepOutputWindowBehind(window_, front);
}

// The panel is another of our windows, and an overlay covering the monitor is in front of everything and
// left out of the capture that draws over it: on one screen the panel would be nowhere at all. So the
// overlay is kept just behind the panel while the panel is above everything itself, or is the window the
// operator is using; the rest of the time it is above everything, and the panel is out of the way.
void RealEnvironment::Fronted() noexcept
{
    static constexpr auto IsInUse = [] [[nodiscard]] (HWND panel) noexcept -> bool { return panel != nullptr && (IsTopmostWindow(panel) || ::GetForegroundWindow() == panel); };
    HWND panel = PanelWindow(panel_);
    if (IsInUse(panel))
        Behind(panel);
    else
        RaiseOutputWindow(window_);
}

Status<Error> RealEnvironment::Followed(interior::Instant now) noexcept
{
    if (!applied_.followed.has_value())
    {
        Fronted();
        return {};
    }
    return Watched(*applied_.followed, now);
}

// What a session cannot follow while it runs, it is rebuilt for, once the panel has settled on it. Settling
// first is what keeps a walk through three choices from building three sessions.
void RealEnvironment::Asked(const interior::CommandLine& shape, interior::Instant now) noexcept
{
    wanted_ = shape; // WAIVER(R2): the shape last described, replaced whole.
    asked_ = now;    // WAIVER(R2): when it was first described, replaced whole with it.
}

[[nodiscard]] bool RealEnvironment::HasWaited(interior::Instant now) const noexcept
{
    return now.Get() - asked_.Get() >= kSettleMicroseconds;
}

// The crosshair takes its window when the button comes up, which is already the operator saying they
// have chosen. Waiting for that to settle would only be waiting.
[[nodiscard]] bool RealEnvironment::AsksForAnotherWindow() const noexcept
{
    return panel_ != nullptr && PickedWindow(*panel_) != applied_.followed;
}

[[nodiscard]] bool RealEnvironment::IsWorthBuilding(interior::Instant now) const noexcept
{
    return HasWaited(now) || AsksForAnotherWindow();
}

[[nodiscard]] bool RealEnvironment::AsksForAnother(const interior::CommandLine& shape, interior::Instant now) const noexcept
{
    return shape != built_ && IsWorthBuilding(now);
}

void RealEnvironment::HeldSettings(const interior::CommandLine& shape, interior::Instant now) noexcept
{
    if (AsksForAnother(shape, now))
        restartWanted_ = true; // WAIVER(R2): set once, and never unset.
}

void RealEnvironment::Considering(const interior::CommandLine& shape, interior::Instant now) noexcept
{
    if (shape != wanted_)
        Asked(shape, now);
    else
        HeldSettings(shape, now);
}

void RealEnvironment::Reconsidered(interior::Instant now) noexcept
{
    if (panel_ == nullptr)
        return;
    Considering(SessionShape(*panel_), now);
}

// The three ways a session ends short of the operator quitting: the window it was working on changed size,
// the panel settled on other settings, or the window it was working on went away.
bool RealEnvironment::AsksForSettings() const noexcept
{
    return restartWanted_ || abandoned_;
}

bool RealEnvironment::AsksToEnd() const noexcept
{
    return resized_ || AsksForSettings();
}

Result<FrameStart, Error> RealEnvironment::Began(const Begun& begun) noexcept
{
    static constexpr auto StoppedIf = [] [[nodiscard]] (const Begun& begun, bool ending) noexcept -> Begun {
        static constexpr auto Stopping = [] [[nodiscard]] (const Begun& begun) noexcept -> Begun {
            interior::FrameInput input = begun.input; // WAIVER(R2): a copy with one answer replaced, read once after.
            input.quit = true;
            return Begun{ begun.frame, input, begun.reading };
        };
        return ending ? Stopping(begun) : begun;
    };
    // While a comparison capture runs, the frame runs the combination it asks for rather than what the panel says.
    static constexpr auto Overridden = [] [[nodiscard]] (const Begun& begun, const std::optional<Comparison>& comparison) noexcept -> Begun {
        static constexpr auto ControlsOf = [] [[nodiscard]] (const std::optional<Comparison>& comparison) noexcept -> std::optional<interior::LiveSettings> {
            if (!comparison.has_value())
                return std::nullopt;
            return ComparisonControls(*comparison);
        };
        const std::optional<interior::LiveSettings> controls = ControlsOf(comparison);
        if (!controls.has_value())
            return begun;
        interior::FrameInput input = begun.input; // WAIVER(R2): a copy with one answer replaced, read once after.
        input.controlRequest = controls;
        return Begun{ begun.frame, input, begun.reading };
    };
    now_ = begun.input.now; // WAIVER(R2): this frame's clock reading, replaced whole per frame.
    return Followed(begun.input.now)
        .and_then([this, &begun] {
            Reconsidered(begun.input.now);
            return DrainedRecording(begun.frame.slot);
        })
        .and_then([this, &begun] { return SettledIfRead(begun.reading); })
        .and_then([this] { return ShowRecording(); })
        .and_then([this, &begun] { return Accept(Overridden(StoppedIf(begun, AsksToEnd()), comparison_)); });
}

Result<FrameStart, Error> RealEnvironment::BeginFrame(const interior::FrameState& state) noexcept
{
    static constexpr auto Begin = [] [[nodiscard]] (const Gpu& gpu, const Surroundings& s, std::uint32_t finestPixels, interior::FenceValue fence, const interior::FrameState& state,
                                                    bool held) noexcept -> Result<Begun, Error> {
        static constexpr auto Prepare = [] [[nodiscard]] (const Gpu& gpu, const Surroundings& s, const WindowEvents& events, std::uint32_t finestPixels, const interior::FrameState& state,
                                                          interior::FrameSlot slot, bool held) noexcept -> Result<Prepared, Error> {
            static constexpr auto AwaitSlot = [] [[nodiscard]] (const Gpu& gpu, const interior::FrameState& state, interior::FrameSlot slot) noexcept -> Status<Error> {
                return WaitForFence(gpu.device, state.slotFences[slot.Get()], interior::MicrosecondsTag::Parse(kFenceTimeoutMicroseconds)).and_then([&] {
                    return Check(gpu.allocators[slot.Get()]->Reset(), ApiCall::ResetAllocator);
                });
            };

            static constexpr auto ReadUnmatched = [] [[nodiscard]] (const Gpu& gpu, std::uint32_t finestPixels, const interior::FrameState& state,
                                                                    interior::FrameSlot slot) noexcept -> Result<std::optional<interior::Fraction>, Error> {
                static constexpr auto FractionOf = [] [[nodiscard]] (std::uint32_t count, std::uint32_t total) noexcept -> Result<interior::Fraction, Error> {
                    if (count > total)
                        return Fail(Error{ ApiCall::StatsOutOfRange, count });
                    return interior::FractionTag::Parse(static_cast<float>(count) / static_cast<float>(total)).transform_error([count](interior::UnitError) {
                        return Error{ ApiCall::StatsOutOfRange, count };
                    });
                };
                if (!state.statsPending[slot.Get()])
                    return std::optional<interior::Fraction>{};
                return Lookup(gpu.resources, interior::ReadbackId(slot))
                    .and_then(ReadFirstUInt)
                    .and_then([finestPixels](std::uint32_t count) { return FractionOf(count, finestPixels); })
                    .transform([](interior::Fraction f) { return std::optional<interior::Fraction>{ f }; });
            };

            static constexpr auto Sampled = [] [[nodiscard]] (const Gpu& gpu, const Surroundings& s, const WindowEvents& events, std::optional<interior::Fraction> unmatched,
                                                              const interior::FrameState& state, bool held) noexcept -> Result<Prepared, Error> {
                // A comparison capture works on the frame it started with, so no new one is taken while it runs: the
                // canvas keeps that frame, and the frames arriving meanwhile are left with the capture.
                static constexpr auto AcquiredUnlessHeld = [] [[nodiscard]] (const Gpu& gpu, interior::FrameNumber number, bool held) noexcept -> Result<bool, Error> {
                    if (held)
                        return false;
                    return AcquireFrames(gpu.capture, number);
                };

                static constexpr auto ReadingOf = [] [[nodiscard]] (const ControlPanel* panel, const WindowEvents& events, const std::optional<interior::Fraction>& drag,
                                                                    const interior::FrameState& state) noexcept -> std::optional<PanelReading> {
                    static constexpr auto SteerPanel = [](const ControlPanel& panel, const WindowEvents& events, const std::optional<interior::Fraction>& drag,
                                                          const interior::FrameState& state) noexcept -> void {
                        static constexpr auto SteerDisplay = [](const ControlPanel& panel, const WindowEvents& events, const interior::FrameState& state) noexcept -> void {
                            // The panel is the one place a setting lives while it runs, so the hotkeys and the divider drag move its
                            // controls first and are then read back out of it. Without a panel they go straight into the frame.
                            static constexpr auto TogglesTheView = [] [[nodiscard]] (const WindowEvents& events) noexcept -> bool { return events.toggleOriginal || events.toggleSplit; };
                            if (TogglesTheView(events))
                                ApplyDisplay(panel, interior::NextDisplay(state.display, events.toggleOriginal, events.toggleSplit));
                        };
                        SteerDisplay(panel, events, state);
                        if (drag.has_value())
                            ApplySplit(panel, *drag);
                    };
                    if (panel == nullptr)
                        return std::nullopt;
                    SteerPanel(*panel, events, drag, state);
                    return ReadControlPanel(*panel, state.controls);
                };

                static constexpr auto PanelWasClosed = [] [[nodiscard]] (const ControlPanel* panel) noexcept -> bool { return panel != nullptr && IsPanelClosed(*panel); };
                const std::optional<interior::Fraction> drag = SplitRequest(s.window);
                const std::optional<PanelReading> reading = ReadingOf(s.panel, events, drag, state);
                return AcquiredUnlessHeld(gpu, state.number, held).and_then([&](bool fresh) {
                    return Now().and_then([&](interior::Instant now) {
                        return CurrentBackBuffer(gpu.presenter).transform([&](interior::BackBufferIndex index) {
                            return Prepared{ fresh, index, unmatched, now, drag, reading, PanelWasClosed(s.panel) };
                        });
                    });
                });
            };
            return WaitForNextFrame(gpu.presenter)
                .and_then([&] { return AwaitSlot(gpu, state, slot); })
                .and_then([&] { return ReadUnmatched(gpu, finestPixels, state, slot); })
                .and_then([&](std::optional<interior::Fraction> unmatched) { return Sampled(gpu, s, events, unmatched, state, held); });
        };

        static constexpr auto InputOf = [] [[nodiscard]] (const WindowEvents& events, const Prepared& p) noexcept -> interior::FrameInput {
            static constexpr auto DisplayFrom = [] [[nodiscard]] (const std::optional<PanelReading>& reading) noexcept -> std::optional<interior::DisplayMode> {
                if (!reading.has_value())
                    return std::nullopt;
                return reading->display;
            };

            static constexpr auto SplitFrom = [] [[nodiscard]] (const Prepared& p) noexcept -> std::optional<interior::Fraction> {
                if (!p.reading.has_value())
                    return p.splitRequest;
                return p.reading->split;
            };

            static constexpr auto ControlsFrom = [] [[nodiscard]] (const std::optional<PanelReading>& reading) noexcept -> std::optional<interior::LiveSettings> {
                if (!reading.has_value())
                    return std::nullopt;
                return reading->live;
            };

            // Asking for a new session ends this one, which is what starts the new one: the command line the panel
            // describes is read and launched once the loop has stopped and the device is idle.
            static constexpr auto AsksToStop = [] [[nodiscard]] (const WindowEvents& events, const Prepared& p) noexcept -> bool { return events.quit || p.panelClosed; };
            return interior::FrameInput{
                p.fresh, p.backBuffer, p.unmatched, p.now, events.toggleOriginal, events.toggleSplit, SplitFrom(p), DisplayFrom(p.reading), ControlsFrom(p.reading), AsksToStop(events, p)
            };
        };

        static constexpr auto ContextOf = [] [[nodiscard]] (const interior::FrameState& state, interior::FrameSlot slot, interior::FenceValue fence) noexcept -> FrameContext {
            return FrameContext{ state.number, slot, state.currentSet, state.hasPrevious, fence };
        };
        const interior::FrameSlot slot = interior::SlotOfFrame(state.number);
        return PumpEvents(s.window).and_then([&](const WindowEvents& events) {
            return Prepare(gpu, s, events, finestPixels, state, slot, held).transform([&](const Prepared& p) { return Begun{ ContextOf(state, slot, fence), InputOf(events, p), p.reading }; });
        });
    };
    return Begin(gpu_, Surroundings{ window_, panel_ }, finestPixels_, frame_.fence, state, comparison_.has_value()).and_then([this](const Begun& begun) { return Began(begun); });
}

Result<FrameStart, Error> RealEnvironment::Accept(const Begun& begun) noexcept
{
    static constexpr auto Reported = [] [[nodiscard]] (const Console& console, const Statistics& s, const interior::FrameInput& input) noexcept -> Result<Statistics, Error> {
        static constexpr auto IsReportDue = [] [[nodiscard]] (const Statistics& s, interior::Instant now) noexcept -> bool { return now.Get() - s.lastReport.Get() >= kReportIntervalMicroseconds; };

        static constexpr auto Counted = [] [[nodiscard]] (const Statistics& s, bool fresh) noexcept -> Statistics {
            static constexpr auto CountOf = [] [[nodiscard]] (bool flag) noexcept -> std::uint32_t { return flag ? 1u : 0u; };
            return Statistics{ s.lastReport, s.processed + CountOf(fresh), s.presented };
        };

        static constexpr auto Restarted = [] [[nodiscard]] (interior::Instant now) noexcept -> Statistics { return Statistics{ now, 0, 0 }; };

        static constexpr auto LogThroughput = [] [[nodiscard]] (const Console& console, const Statistics& s, interior::Instant now) noexcept -> Status<Error> {
            const double seconds = static_cast<double>(now.Get() - s.lastReport.Get()) / 1000000.0;
            return Log(console, interior::LogLevel::Info, infra::Formatted<120>("{:.1f} processed fps, {:.1f} presented fps", s.processed / seconds, s.presented / seconds).Get());
        };
        if (!IsReportDue(s, input.now))
            return Counted(s, input.freshCapture);
        return LogThroughput(console, s, input.now).transform([&] { return Counted(Restarted(input.now), input.freshCapture); });
    };
    const Result<Statistics, Error> stats = Reported(console_, stats_, begun.input);
    if (!stats.has_value())
        return Fail(stats.error());
    frame_ = begun.frame; // WAIVER(R2): the frame context is the effect layer's state, replaced whole per frame.
    stats_ = *stats;      // WAIVER(R2): throughput counters, replaced whole per frame.
    return FrameStart{ begun.input };
}

// The depth plane is a texture cleared once, so a new value means clearing it again.
[[nodiscard]] Status<Error> RealEnvironment::Recleared(interior::DepthValue depth) noexcept
{
    static constexpr auto WithDepthValue = [] [[nodiscard]] (const SessionPlan& plan, interior::DepthValue depth) noexcept -> SessionPlan {
        SessionPlan next = plan; // WAIVER(R2): a copy adjusted once, to clear the plane to a new value.
        next.depth = depth;
        return next;
    };
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

namespace {

// Whether the capture is told to include the cursor. Auto keeps whatever the session resolved when it started.
[[nodiscard]] bool CursorCaptured(const interior::SurfaceSettings& surface, bool resolved) noexcept
{
    if (surface.cursor == interior::CursorMode::Auto)
        return resolved;
    return surface.cursor == interior::CursorMode::On;
}

} // namespace

Status<Error> RealEnvironment::Resurfaced(const interior::SurfaceSettings& surface) noexcept
{
    static constexpr auto ApplySurface = [] [[nodiscard]] (const Gpu& gpu, const OutputWindow& window, const EnvironmentSettings& settings) noexcept -> infra::Status<Error> {
        // The overlay is above everything when it covers a monitor, and just above the window it follows otherwise.
        static constexpr auto WindowSettingsOf = [] [[nodiscard]] (const EnvironmentSettings& settings) noexcept -> WindowSettings {
            return WindowSettings{
                .topmost = !settings.followed.has_value(), .clickThrough = settings.surface.clickThrough, .excludeFromCapture = settings.surface.displayAffinity, .redirectionBitmap = false
            };
        };
        return ApplyCaptureSettings(gpu.capture, CaptureSettings{ CursorCaptured(settings.surface, settings.captureCursor), settings.surface.captureBorder }).and_then([&] {
            return ApplyWindowSettings(window, WindowSettingsOf(settings));
        });
    };
    if (surface == applied_.surface)
        return {};
    // WAIVER(R2): what has been applied, replaced whole.
    applied_ = EnvironmentSettings{ surface, applied_.captureCursor, applied_.followed, applied_.asksToBeLeftOut, applied_.outsideTheSource, applied_.source };
    return ApplySurface(gpu_, window_, applied_);
}

// The two files a capture became, for the log.
[[nodiscard]] Status<Error> NotedFiles(const Console& console, std::string_view what, const CaptureFiles& files) noexcept
{
    const std::array<char, interior::FilePath::Capacity + 1> original = infra::NarrowedChars<interior::FilePath::Capacity + 1>(files.original.Get());
    const std::array<char, interior::FilePath::Capacity + 1> processed = infra::NarrowedChars<interior::FilePath::Capacity + 1>(files.processed.Get());
    return Log(console, interior::LogLevel::Info, infra::Formatted<720>("{}: {} and {}", what, original.data(), processed.data()).Get());
}

[[nodiscard]] bool NothingToHideFrom(const Gpu& gpu, const EnvironmentSettings& settings) noexcept
{
    // A window capture holds that window's own content and nothing stacked in front, so the overlay was never
    // going to be in it; an overlay on a monitor other than the captured one is not in the picture either.
    static constexpr auto NotInThePicture = [] [[nodiscard]] (const EnvironmentSettings& settings) noexcept -> bool { return settings.followed.has_value() || settings.outsideTheSource; };
    return gpu.capture.excludesOurWindows || NotInThePicture(settings);
}

Status<Error> RealEnvironment::Settled(const PanelReading& reading) noexcept
{
    // The panel still carries the setting the session started with, so a change to any other surface setting
    // would put the blanket back over windows the capture is already leaving out by name.
    static constexpr auto AsExcluded = [] [[nodiscard]] (const interior::SurfaceSettings& s, bool excluding) noexcept -> interior::SurfaceSettings {
        static constexpr auto WithoutAffinity = [] [[nodiscard]] (const interior::SurfaceSettings& s) noexcept -> interior::SurfaceSettings {
            interior::SurfaceSettings next = s; // WAIVER(R2): a copy with one answer replaced, read once after it.
            next.displayAffinity = false;
            return next;
        };
        return excluding ? WithoutAffinity(s) : s;
    };
    // A capture is named for what is being captured: the window followed, or the desktop.
    static constexpr auto OrderOf = [] [[nodiscard]] (const PanelReading& reading, const std::optional<interior::MonitorHandle>& followed) noexcept -> SnapshotOrder {
        static constexpr auto LabelOf = [] [[nodiscard]] (const std::optional<interior::MonitorHandle>& followed) noexcept -> interior::CaptureLabel {
            if (!followed.has_value())
                return interior::CaptureLabel::Parse(interior::kDesktopLabel).value_or(interior::CaptureLabel{});
            return interior::CaptureLabelOf(TitleOfWindow(*followed).Get());
        };
        return SnapshotOrder{ reading.capture.folder, LabelOf(followed), reading.live, reading.capture.everything, MomentNow(), Pictures::Both };
    };

    static constexpr auto ScreenshotOf = [] [[nodiscard]] (const PanelReading& reading, const SnapshotOrder& order) noexcept -> std::optional<SnapshotOrder> {
        if (!reading.capture.screenshot)
            return std::nullopt;
        return order;
    };

    // The cursor is drawn into captures only when it was asked for and the capture itself leaves it out; one
    // that carries it already needs nothing drawn. A picture starts where the followed window is now, or
    // where the source is; a window that has gone gives no cursor this frame.
    static constexpr auto OverlayOf = [] [[nodiscard]] (const PanelReading& reading, const EnvironmentSettings& applied) noexcept -> std::optional<CursorOverlay> {
        static constexpr auto TopLeftOf = [] [[nodiscard]] (const interior::ScreenRect& rect) noexcept -> POINT {
            return POINT{ .x = static_cast<LONG>(rect.Left().Get()), .y = static_cast<LONG>(rect.Top().Get()) };
        };

        static constexpr auto OriginOf = [] [[nodiscard]] (const EnvironmentSettings& applied) noexcept -> std::optional<POINT> {
            if (!applied.followed.has_value())
                return TopLeftOf(applied.source);
            return BoundsOfWindow(*applied.followed).transform([](const interior::ScreenRect& rect) { return TopLeftOf(rect); });
        };
        if (!reading.capture.cursor || CursorCaptured(applied.surface, applied.captureCursor))
            return std::nullopt;
        return OriginOf(applied).transform([](const POINT& origin) { return CursorOverlay{ SampleCursor(), origin }; });
    };
    const interior::SurfaceSettings surface = AsExcluded(reading.surface, NothingToHideFrom(gpu_, applied_));
    const SnapshotOrder order = OrderOf(reading, applied_.followed);
    return Resurfaced(surface)
        .and_then([this, &reading] { return Recleared(reading.live.depth); })
        .and_then([this, &reading, &order] { return ToggledRecording(reading, order); })
        .and_then([this, &reading, &order] {
            snapshot_ = ScreenshotOf(reading, order); // WAIVER(R2): what this frame's reading asked for, replaced whole each frame.
            cursor_ = OverlayOf(reading, applied_);   // WAIVER(R2): the cursor this frame's captures get, replaced whole each frame.
            return ToggledComparison(reading, order);
        })
        .and_then([this, &reading] { return BroughtBack(reading); })
        .transform([this, &reading] {
            ShowComparison(reading);
            ShowFollowing();
        });
}

namespace {

// How far a comparison capture has got and where its pictures are, for the log.
[[nodiscard]] Status<Error> NotedComparison(const Console& console, std::string_view what, const Comparison& c, interior::Instant now) noexcept
{
    const std::array<char, interior::DirectoryPath::Capacity + 1> folder = infra::NarrowedChars<interior::DirectoryPath::Capacity + 1>(c.base.folder.Get());
    const double seconds = static_cast<double>(now.Get() - std::min(now.Get(), c.started.Get())) / 1000000.0;
    return Log(console, interior::LogLevel::Info, infra::Formatted<420>("{}: {} of {} pictures in {} after {:.1f} s", what, c.done, c.total, folder.data(), seconds).Get());
}

} // namespace

// The button starts a comparison capture when none is running and stops the one that is. One with nothing
// checked would take no pictures, so it is not started.
Status<Error> RealEnvironment::ToggledComparison(const PanelReading& reading, const SnapshotOrder& order) noexcept
{
    if (!reading.capture.comparison.start)
        return {};
    if (comparison_.has_value())
        return EndedComparison("comparison capture stopped");
    if (interior::SweepCount(reading.capture.comparison.axes) == 0)
        return {};
    return StartComparison(order, reading.capture.comparison.axes, cursor_, now_).and_then([this](const Comparison& started) {
        comparison_ = started; // WAIVER(R2): the comparison under way, replaced whole.
        return NotedComparison(console_, "comparison capture started", started, now_);
    });
}

Status<Error> RealEnvironment::EndedComparison(std::string_view what) noexcept
{
    if (!comparison_.has_value())
        return {};
    const Comparison ended = *comparison_;
    comparison_ = std::nullopt; // WAIVER(R2): the comparison ended, replaced whole.
    return NotedComparison(console_, what, ended, now_);
}

// Done means on disk: the writer is drained before the log says so.
Status<Error> RealEnvironment::FinishedComparison() noexcept
{
    if (!comparison_.has_value() || !IsComparisonDone(*comparison_))
        return {};
    return writer_->Drain().and_then([this] { return EndedComparison("comparison capture done"); });
}

void RealEnvironment::ShowComparison(const PanelReading& reading) noexcept
{
    static constexpr auto RunningOf = [] [[nodiscard]] (const std::optional<Comparison>& comparison) noexcept -> std::optional<interior::SweepProgress> {
        if (!comparison.has_value())
            return std::nullopt;
        return ProgressOf(*comparison);
    };
    if (panel_ != nullptr)
        ApplyComparison(*panel_, interior::SweepCount(reading.capture.comparison.axes), RunningOf(comparison_));
}

Status<Error> RealEnvironment::ToggledRecording(const PanelReading& reading, const SnapshotOrder& order) noexcept
{
    if (!reading.capture.record)
        return {};
    if (recording_.has_value())
        return StoppedRecording();
    return StartRecording(order, now_).transform([this](VideoRecording started) {
        recording_ = std::move(started); // WAIVER(R2): the recording under way, replaced whole.
    });
}

Status<Error> RealEnvironment::StoppedRecording() noexcept
{
    if (!recording_.has_value())
        return {};
    return StopRecording(gpu_, frame_.fence, std::move(*recording_)).and_then([this](const Stopped& stopped) {
        recording_ = std::nullopt;                 // WAIVER(R2): the recording ended, replaced whole.
        frame_ = WithFence(frame_, stopped.fence); // WAIVER(R2): the last signalled fence, replaced whole.
        return NotedFiles(console_, "recording saved", stopped.files);
    });
}

Status<Error> RealEnvironment::Finished() noexcept
{
    return StoppedRecording().and_then([this] { return EndedComparison("comparison capture ended with the session"); }).and_then([this] { return writer_->Drain(); });
}

Status<Error> RealEnvironment::DrainedRecording(interior::FrameSlot slot) noexcept
{
    if (!recording_.has_value())
        return {};
    return DrainedSlot(std::move(*recording_), slot).transform([this](VideoRecording drained) {
        recording_ = std::move(drained); // WAIVER(R2): the recording under way, replaced whole.
    });
}

Status<Error> RealEnvironment::ShowRecording() noexcept
{
    static constexpr auto ElapsedOf = [] [[nodiscard]] (const std::optional<VideoRecording>& recording, interior::Instant now) noexcept -> std::optional<interior::Microseconds> {
        if (!recording.has_value())
            return std::nullopt;
        return Elapsed(*recording, now);
    };
    if (panel_ != nullptr)
        ApplyRecording(*panel_, ElapsedOf(recording_, now_));
    return {};
}

std::optional<interior::CommandLine> RealEnvironment::Restart(const interior::Options& options) const noexcept
{
    static constexpr auto AsksForANewSession = [] [[nodiscard]] (bool wanted, const ControlPanel* panel) noexcept -> bool { return wanted && panel != nullptr; };
    if (!AsksForANewSession(AsksForSettings(), panel_))
        return std::nullopt;
    const interior::CommandLine line = RestartCommandLine(*panel_, options);
    NoteExclusion("--- this session is asking for another, built from:");
    NoteExclusionWide(line.CString());
    return line;
}

Result<ExecutionReport, Error> RealEnvironment::Retuned(const interior::LiveSettings& controls) noexcept
{
    static constexpr auto Wanted = [] [[nodiscard]] (const interior::LiveSettings& controls) noexcept -> BuiltModel { return BuiltModel{ controls.tuning, controls.passes }; };

    static constexpr auto NeedsRebuild = [] [[nodiscard]] (const Models& models, const interior::LiveSettings& controls) noexcept -> bool {
        return models.builtWith.has_value() && *models.builtWith != Wanted(controls);
    };
    if (!NeedsRebuild(gpu_.models, controls))
        return ExecutionReport{ frame_.fence, 0 };
    return WaitIdle(gpu_.device, frame_.fence)
        .and_then([&](interior::FenceValue idle) { return BuiltNeuralRendering(gpu_, plan_, Wanted(controls), Created{ std::move(gpu_.models), idle }); })
        .transform([this](Created rebuilt) {
            gpu_ = WithModels(std::move(gpu_), std::move(rebuilt.models)); // WAIVER(R2): the built model is effect-layer state, replaced whole when the operator retunes.
            frame_ = WithFence(frame_, rebuilt.fence);                     // WAIVER(R2): the last signalled fence, replaced whole.
            return ExecutionReport{ rebuilt.fence, 0 };
        });
}

Result<ExecutionReport, Error> RealEnvironment::Ran(const interior::FramePlan& plan) noexcept
{
    static constexpr auto Presented = [] [[nodiscard]] (const Statistics& s) noexcept -> Statistics { return Statistics{ s.lastReport, s.processed, s.presented + 1 }; };
    const Result<interior::FenceValue, Error> fence = ExecuteSteps(gpu_, frame_, plan.steps);
    if (!fence.has_value())
        return Fail(fence.error());
    frame_ = WithFence(frame_, *fence); // WAIVER(R2): the last signalled fence is effect-layer state, replaced whole per frame.
    stats_ = Presented(stats_);         // WAIVER(R2): throughput counters, replaced whole per frame.
    return ExecutionReport{ *fence, static_cast<std::uint32_t>(plan.steps.Size()) };
}

Result<ExecutionReport, Error> RealEnvironment::Captured(const interior::FramePlan& plan, const ExecutionReport& report) noexcept
{
    if (!snapshot_.has_value())
        return report;
    return SaveSnapshot(gpu_, frame_, plan.next, *snapshot_, cursor_, *writer_).and_then([this, &report](const Snapshot& s) {
        frame_ = WithFence(frame_, s.fence); // WAIVER(R2): the last signalled fence, replaced whole.
        snapshot_ = std::nullopt;            // WAIVER(R2): the order was taken, so nothing waits.
        return NotedFiles(console_, "screenshot writing", CaptureFiles{ s.original, s.processed }).transform([&] { return ExecutionReport{ s.fence, report.stepsExecuted }; });
    });
}

Result<ExecutionReport, Error> RealEnvironment::ComparedFrame(const interior::FramePlan& plan, const ExecutionReport& report) noexcept
{
    if (!comparison_.has_value())
        return report;
    return CapturedComparison(gpu_, frame_, plan.next, *comparison_, *writer_).and_then([this, &report](const Compared& c) {
        comparison_ = c.comparison;          // WAIVER(R2): the comparison under way, replaced whole as pictures are saved.
        frame_ = WithFence(frame_, c.fence); // WAIVER(R2): the last signalled fence, replaced whole.
        return FinishedComparison().transform([&] { return ExecutionReport{ c.fence, report.stepsExecuted }; });
    });
}

Result<ExecutionReport, Error> RealEnvironment::RecordedFrame(const interior::FramePlan& plan, const ExecutionReport& report) noexcept
{
    if (!recording_.has_value())
        return report;
    return RecordFrame(gpu_, frame_, plan.next, now_, cursor_, std::move(*recording_)).transform([this, &report](Recorded recorded) {
        recording_ = std::move(recorded.recording); // WAIVER(R2): the recording under way, replaced whole.
        frame_ = WithFence(frame_, recorded.fence); // WAIVER(R2): the last signalled fence, replaced whole.
        return ExecutionReport{ recorded.fence, report.stepsExecuted };
    });
}

Result<ExecutionReport, Error> RealEnvironment::Execute(const interior::FramePlan& plan) noexcept
{
    return Retuned(plan.next.controls)
        .and_then([this, &plan](const ExecutionReport&) { return Ran(plan); })
        .and_then([this, &plan](const ExecutionReport& report) { return Captured(plan, report); })
        .and_then([this, &plan](const ExecutionReport& report) { return ComparedFrame(plan, report); })
        .and_then([this, &plan](const ExecutionReport& report) { return RecordedFrame(plan, report); });
}

Error RealEnvironment::FromPlanError(interior::PlanFrameError error) noexcept
{
    return Error{ ApiCall::PlanFrame, static_cast<std::uint32_t>(error) };
}

[[nodiscard]] HWND PanelWindow(const ControlPanel* panel) noexcept
{
    return panel == nullptr ? nullptr : panel->window.get();
}

[[nodiscard]] std::span<const HWND> Present(const std::array<HWND, 2>& ours) noexcept
{
    return std::span<const HWND>(ours.data(), ours[1] == nullptr ? 1u : 2u);
}

// Asking for nothing to be left out is how the capture is told not to try, which leaves the windows
// hidden from every capture, as they were before there was another way.
[[nodiscard]] std::span<const HWND> Asked(const std::array<HWND, 2>& ours, bool wanted) noexcept
{
    return wanted ? Present(ours) : std::span<const HWND>{};
}

Result<RealEnvironment, Error> CreateEnvironment(HeldFiles held, GpuDevice device, std::optional<NgxRuntime> runtime, const SessionPlan& plan, const interior::Geometry& geometry, OutputWindow window,
                                                 const ControlPanel* panel, const EnvironmentSettings& settings, const interior::Options& options, const Console& console) noexcept
{
    static constexpr auto FromPyramid = [] [[nodiscard]] (interior::PyramidError error) noexcept -> Error { return Error{ ApiCall::PlanSession, static_cast<std::uint32_t>(error) }; };

    static constexpr auto AssembledGpu = [] [[nodiscard]] (GpuDevice device, const SessionPlan& plan, const interior::Geometry& geometry, HWND window, const EnvironmentSettings& settings,
                                                           const interior::LevelExtents& extents, std::span<const HWND> ours) noexcept -> Result<Gpu, Error> {
        static constexpr auto CreateRecording = [] [[nodiscard]] (const GpuDevice& d) noexcept -> Result<Recording, Error> {
            static constexpr auto CreateAllocators = [] [[nodiscard]] (const GpuDevice& d) noexcept -> Result<Allocators, Error> {
                return infra::FoldResult(std::views::iota(std::size_t{ 0 }, std::tuple_size_v<Allocators>), Result<Allocators, Error>(Allocators{}), [&](const Allocators& acc, std::size_t i) {
                    return CreateAllocator(d).transform([&](const Com<ID3D12CommandAllocator>& allocator) { return infra::WithElement(acc, i, allocator); });
                });
            };
            return CreateAllocators(d).and_then([&](const Allocators& allocators) {
                return CreateClosedCommandList(d, allocators[0].Get()).transform([&](const Com<ID3D12GraphicsCommandList>& list) { return Recording{ allocators, list }; });
            });
        };

        static constexpr auto WithResourcesAndCapture = [] [[nodiscard]] (GpuDevice device, Presenter presenter, const Pipelines& pipelines, const Recording& recording, const SessionPlan& plan,
                                                                          const interior::Geometry& geometry, const EnvironmentSettings& settings, const interior::LevelExtents& extents,
                                                                          std::span<const HWND> ours) noexcept -> Result<Gpu, Error> {
            static constexpr auto CreateResources = [] [[nodiscard]] (const GpuDevice& d, const SessionPlan& plan, const Presenter& presenter, const interior::LevelExtents& extents,
                                                                      ID3D12Resource* canvas) noexcept -> TableResult {
                static constexpr auto ModelFormatOf = [] [[nodiscard]] (interior::ColorFormat format) noexcept -> DXGI_FORMAT {
                    switch (format)
                    {
                    case interior::ColorFormat::Rgba8: return DXGI_FORMAT_R8G8B8A8_UNORM;
                    case interior::ColorFormat::Rgba16f: return DXGI_FORMAT_R16G16B16A16_FLOAT;
                    }
                    return DXGI_FORMAT_R8G8B8A8_UNORM;
                };

                static constexpr auto UavRequest = [] [[nodiscard]] (const Extent& extent, DXGI_FORMAT format, const wchar_t* name) noexcept -> TextureRequest {
                    return TextureRequest{ extent, format, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, name };
                };

                static constexpr auto WithTexture = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const ResourceId& id, const TextureRequest& r) noexcept -> TableResult {
                    return CreateTexture(d, r).transform([&](const Texture& texture) { return WithResource(t, id, texture.resource); });
                };

                static constexpr auto WithBackBuffers = [] [[nodiscard]] (const ResourceTable& t, const Presenter& presenter) noexcept -> ResourceTable {
                    static constexpr auto BackBufferIdOf = [] [[nodiscard]] (std::uint32_t index) noexcept -> ResourceId {
                        const Result<interior::BackBufferIndex, interior::UnitError> b = interior::BackBufferIndexTag::Parse(index);
                        ENSURE(b.has_value());
                        return interior::BackBufferId(*b);
                    };
                    return std::ranges::fold_left(std::views::iota(std::uint32_t{ 0 }, interior::kBackBufferCount), t,
                                                  [&](const ResourceTable& acc, std::uint32_t i) { return WithResource(acc, BackBufferIdOf(i), presenter.backBuffers[i]); });
                };

                // The canvas is the capture device's texture, opened on this device; everything else is created here.
                static constexpr auto CoreTextures = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan, DXGI_FORMAT model,
                                                                       ID3D12Resource* canvas) noexcept -> TableResult {
                    static constexpr auto WithDepth = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan) noexcept -> TableResult {
                        static constexpr auto DepthRequest = [] [[nodiscard]] (const SessionPlan& plan) noexcept -> TextureRequest {
                            return TextureRequest{ plan.source, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, L"Constant depth plane" };
                        };
                        return CreateClearableTexture(d, DepthRequest(plan), plan.depth.Get()).transform([&](const Texture& texture) {
                            return WithResource(t, SimpleId(ResourceKind::Depth), texture.resource);
                        });
                    };
                    return TableResult(WithResource(t, SimpleId(ResourceKind::Canvas), canvas))
                        .and_then([&](const ResourceTable& n) { return WithTexture(n, d, SimpleId(ResourceKind::ModelColor), UavRequest(plan.source, model, L"Model colour")); })
                        .and_then([&](const ResourceTable& n) { return WithDepth(n, d, plan); })
                        .and_then(
                            [&](const ResourceTable& n) { return WithTexture(n, d, SimpleId(ResourceKind::MotionVectors), UavRequest(plan.source, DXGI_FORMAT_R16G16_FLOAT, L"Motion vectors")); });
                };

                static constexpr auto ModelOutputs = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan, DXGI_FORMAT model) noexcept -> TableResult {
                    static constexpr auto SrOutputRequest = [] [[nodiscard]] (const SessionPlan& plan, DXGI_FORMAT model) noexcept -> std::optional<TextureRequest> {
                        if (!plan.superResolution.has_value())
                            return std::nullopt;
                        return UavRequest(plan.target, model, L"DLSS Super Resolution output");
                    };

                    static constexpr auto NrOutputRequest = [] [[nodiscard]] (const SessionPlan& plan, DXGI_FORMAT model) noexcept -> std::optional<TextureRequest> {
                        if (!plan.neuralRendering)
                            return std::nullopt;
                        return UavRequest(plan.work, model, L"DLSS 5 Neural Rendering output");
                    };

                    // The two pictures the model's passes hand each other. Both exist whenever the model runs, so the
                    // number of passes can change without the textures being rebuilt.
                    static constexpr auto NrPassRequest = [] [[nodiscard]] (const SessionPlan& plan, DXGI_FORMAT model) noexcept -> std::optional<TextureRequest> {
                        if (!plan.neuralRendering)
                            return std::nullopt;
                        return UavRequest(plan.work, model, L"DLSS 5 Neural Rendering pass");
                    };

                    static constexpr auto OpticalFlowRequest = [] [[nodiscard]] (const SessionPlan& plan) noexcept -> std::optional<TextureRequest> {
                        static constexpr auto FlowOutputRequest = [] [[nodiscard]] (const SessionPlan& plan) noexcept -> TextureRequest {
                            return TextureRequest{ plan.flowExtent, DXGI_FORMAT_R16G16_SINT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COMMON, L"Optical flow output" };
                        };
                        if (!UsesOpticalFlow(plan))
                            return std::nullopt;
                        return FlowOutputRequest(plan);
                    };

                    static constexpr auto WithOptionalTexture = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const ResourceId& id,
                                                                                  const std::optional<TextureRequest>& r) noexcept -> TableResult {
                        if (!r.has_value())
                            return t;
                        return WithTexture(t, d, id, *r);
                    };
                    return WithOptionalTexture(t, d, SimpleId(ResourceKind::SrOutput), SrOutputRequest(plan, model))
                        .and_then([&](const ResourceTable& n) { return WithOptionalTexture(n, d, SimpleId(ResourceKind::NrOutput), NrOutputRequest(plan, model)); })
                        .and_then([&](const ResourceTable& n) { return WithOptionalTexture(n, d, interior::NrPassId(*kZeroSet), NrPassRequest(plan, model)); })
                        .and_then([&](const ResourceTable& n) { return WithOptionalTexture(n, d, interior::NrPassId(*kOneSet), NrPassRequest(plan, model)); })
                        .and_then([&](const ResourceTable& n) { return WithOptionalTexture(n, d, SimpleId(ResourceKind::OpticalFlowOutput), OpticalFlowRequest(plan)); });
                };

                static constexpr auto Buffers = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d) noexcept -> TableResult {
                    static constexpr auto WithBufferResource = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const ResourceId& id, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state,
                                                                                 D3D12_RESOURCE_FLAGS flags, const wchar_t* name) noexcept -> TableResult {
                        return CreateBuffer(d, interior::ByteCountTag::Parse(interior::kStatsBytes), heap, state, flags, name).transform([&](const Com<ID3D12Resource>& buffer) {
                            return WithResource(t, id, buffer);
                        });
                    };

                    static constexpr auto WithZeroBuffer = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d) noexcept -> TableResult {
                        return CreateBuffer(d, interior::ByteCountTag::Parse(interior::kStatsBytes), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE,
                                            L"Zero source")
                            .and_then([&](const Com<ID3D12Resource>& buffer) {
                                return WriteZeros(buffer.Get(), interior::ByteCountTag::Parse(interior::kStatsBytes)).transform([&] {
                                    return WithResource(t, SimpleId(ResourceKind::ZeroBuffer), buffer);
                                });
                            });
                    };

                    static constexpr auto WithReadbacks = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d) noexcept -> TableResult {
                        static constexpr auto ReadbackIdOf = [] [[nodiscard]] (std::uint32_t slot) noexcept -> ResourceId {
                            const Result<interior::FrameSlot, interior::UnitError> s = interior::FrameSlotTag::Parse(slot);
                            ENSURE(s.has_value());
                            return interior::ReadbackId(*s);
                        };
                        return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, interior::kFrameSlotCount), TableResult(t), [&](const ResourceTable& acc, std::uint32_t slot) {
                            return WithBufferResource(acc, d, ReadbackIdOf(slot), D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE, L"Statistics readback");
                        });
                    };
                    return WithBufferResource(t, d, SimpleId(ResourceKind::Stats), D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,
                                              L"Match statistics")
                        .and_then([&](const ResourceTable& n) { return WithZeroBuffer(n, d); })
                        .and_then([&](const ResourceTable& n) { return WithReadbacks(n, d); });
                };

                static constexpr auto Pyramids = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const SessionPlan& plan, const interior::LevelExtents& extents) noexcept -> TableResult {
                    static constexpr auto LumaLevels = [] [[nodiscard]] (const SessionPlan& plan) noexcept -> std::uint32_t {
                        switch (plan.motion)
                        {
                        case interior::MotionBackend::BuiltIn: return plan.levels.Get();
                        case interior::MotionBackend::NvOpticalFlow: return 1;
                        case interior::MotionBackend::None: return 1;
                        }
                        return 1;
                    };

                    static constexpr auto FlowLevels = [] [[nodiscard]] (const SessionPlan& plan) noexcept -> std::uint32_t {
                        switch (plan.motion)
                        {
                        case interior::MotionBackend::BuiltIn: return plan.levels.Get();
                        case interior::MotionBackend::NvOpticalFlow: return 0;
                        case interior::MotionBackend::None: return 0;
                        }
                        return 0;
                    };

                    static constexpr auto WithPyramid = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const interior::LevelExtents& extents,
                                                                          const Pyramid& pyramid) noexcept -> TableResult {
                        static constexpr auto WithLevel = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const interior::LevelExtents& extents, const Pyramid& pyramid,
                                                                            std::uint32_t level) noexcept -> TableResult {
                            static constexpr auto LevelIdOf = [] [[nodiscard]] (LevelKind kind, std::uint32_t set, std::uint32_t level) noexcept -> ResourceId {
                                static constexpr auto LumaIdOf = [] [[nodiscard]] (std::uint32_t set, std::uint32_t level) noexcept -> ResourceId {
                                    const Result<interior::SetIndex, interior::UnitError> s = interior::SetIndexTag::Parse(set);
                                    const Result<interior::LevelIndex, interior::UnitError> l = interior::LevelIndexTag::Parse(level);
                                    ENSURE(s.has_value() && l.has_value());
                                    return interior::LumaId(*s, *l);
                                };

                                static constexpr auto FlowIdOf = [] [[nodiscard]] (std::uint32_t level) noexcept -> ResourceId {
                                    const Result<interior::LevelIndex, interior::UnitError> l = interior::LevelIndexTag::Parse(level);
                                    ENSURE(l.has_value());
                                    return interior::FlowId(*l);
                                };
                                switch (kind)
                                {
                                case LevelKind::Luma: return LumaIdOf(set, level);
                                case LevelKind::Flow: return FlowIdOf(level);
                                }
                                return FlowIdOf(level);
                            };

                            static constexpr auto LevelRequest = [] [[nodiscard]] (LevelKind kind, const Extent& extent) noexcept -> TextureRequest {
                                switch (kind)
                                {
                                case LevelKind::Luma: return UavRequest(extent, DXGI_FORMAT_R8_UNORM, L"Luma pyramid");
                                case LevelKind::Flow: return UavRequest(extent, DXGI_FORMAT_R16G16_FLOAT, L"Flow pyramid");
                                }
                                return UavRequest(extent, DXGI_FORMAT_R16G16_FLOAT, L"Flow pyramid");
                            };
                            return WithTexture(t, d, LevelIdOf(pyramid.kind, pyramid.set, level), LevelRequest(pyramid.kind, extents.At(level)));
                        };
                        return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, pyramid.levels), TableResult(t),
                                                 [&](const ResourceTable& acc, std::uint32_t level) { return WithLevel(acc, d, extents, pyramid, level); });
                    };

                    static constexpr auto WithLuma = [] [[nodiscard]] (const ResourceTable& t, const GpuDevice& d, const interior::LevelExtents& extents,
                                                                       std::uint32_t levels) noexcept -> TableResult {
                        return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, std::uint32_t{ 2 }), TableResult(t),
                                                 [&](const ResourceTable& acc, std::uint32_t set) { return WithPyramid(acc, d, extents, Pyramid{ LevelKind::Luma, set, levels }); });
                    };
                    return WithLuma(t, d, extents, LumaLevels(plan)).and_then([&](const ResourceTable& n) { return WithPyramid(n, d, extents, Pyramid{ LevelKind::Flow, 0, FlowLevels(plan) }); });
                };
                const DXGI_FORMAT model = ModelFormatOf(plan.format);
                return CoreTextures(WithBackBuffers(ResourceTable{}, presenter), d, plan, model, canvas)
                    .and_then([&](const ResourceTable& n) { return ModelOutputs(n, d, plan, model); })
                    .and_then([&](const ResourceTable& n) { return Buffers(n, d); })
                    .and_then([&](const ResourceTable& n) { return Pyramids(n, d, plan, extents); });
            };

            static constexpr auto CaptureSettingsOf = [] [[nodiscard]] (const SessionPlan& plan, const EnvironmentSettings& settings) noexcept -> CaptureSettings {
                return CaptureSettings{ plan.captureCursor, settings.surface.captureBorder };
            };
            return CreateCapture(device, geometry.sourceRect, plan.source, geometry.source, CaptureSettingsOf(plan, settings), ours).and_then([&](Capture capture) {
                return CreateResources(device, plan, presenter, extents, capture.sharedCanvas.Get()).transform([&](const ResourceTable& resources) {
                    return Gpu{ std::move(device), pipelines, std::move(presenter), std::move(capture), recording.allocators, recording.list, resources, Models{}, OpticalFlowSlot{} };
                });
            });
        };

        // A window Windows has never had to compose is not one it can be asked to leave out of a capture, and
        // the overlay would otherwise stay hidden until the first frame is ready, which is after the asking.
        static constexpr auto ShownBeforeCapture = [](HWND window, const EnvironmentSettings& settings) noexcept -> void {
            if (!settings.asksToBeLeftOut)
                return;
            NoteExclusion("putting the overlay on screen before the capture is asked to leave it out");
            (void)::ShowWindow(window, SW_SHOWNOACTIVATE);
        };
        NoteExclusion(settings.asksToBeLeftOut ? "--- new session: asking to be left out of the capture" : "--- new session: hidden from every capture");
        return CreatePresenter(device, window, plan.target).and_then([&](Presenter presenter) {
            ShownBeforeCapture(window, settings);
            return CreatePipelines(device, kSwapChainFormat).and_then([&](const Pipelines& pipelines) {
                return CreateRecording(device).and_then(
                    [&](const Recording& recording) { return WithResourcesAndCapture(std::move(device), std::move(presenter), pipelines, recording, plan, geometry, settings, extents, ours); });
            });
        });
    };

    static constexpr auto Assembled = [] [[nodiscard]] (HeldFiles held, Ready r, const SessionPlan& plan, OutputWindow window, const ControlPanel* panel, const Console& console,
                                                        const EnvironmentSettings& settings, const interior::Options& options,
                                                        const interior::LevelExtents& extents) noexcept -> Result<RealEnvironment, Error> {
        static constexpr auto FinestPixels = [] [[nodiscard]] (const SessionPlan& plan, const interior::LevelExtents& extents) noexcept -> Result<std::uint32_t, Error> {
            static constexpr auto FromArithmetic = [] [[nodiscard]] (infra::ArithmeticError error) noexcept -> Error {
                return Error{ ApiCall::PlanSession, 100u + static_cast<std::uint32_t>(error) };
            };
            const Extent finest = extents.At(plan.finestLevel.Get());
            return infra::CheckedMul(finest.width.Get(), finest.height.Get()).transform_error(FromArithmetic);
        };
        return FinestPixels(plan, extents).and_then([&](std::uint32_t finest) {
            return Now().transform(
                [&](interior::Instant start) { return RealEnvironment(std::move(held), std::move(r.gpu), plan, std::move(window), panel, console, settings, options, finest, r.fence, start); });
        });
    };

    // The windows of this program, which the capture is asked to leave out by name. A window that is not
    // there is passed as nothing and skipped, so the list is as long as the program has windows.
    static constexpr auto OurWindows = [] [[nodiscard]] (const OutputWindow& window, const ControlPanel* panel) noexcept -> std::array<HWND, 2> { return { window.handle.get(), PanelWindow(panel) }; };

    // Starting hidden from every capture is the only safe order: nothing can photograph the overlay before a
    // session exists to be told about it. Once one has taken the list, they go back to ordinary windows.
    static constexpr auto Uncovered = [] [[nodiscard]] (const Gpu& gpu, const EnvironmentSettings& settings, std::span<const HWND> ours) noexcept -> Status<Error> {
        if (!NothingToHideFrom(gpu, settings))
            return {};
        return infra::ForEach(ours, Status<Error>{}, [](HWND window) { return UncoverWindow(window); });
    };
    const std::array<HWND, 2> ours = OurWindows(window, panel);
    return interior::LevelExtentsOf(plan.source, plan.levels).transform_error(FromPyramid).and_then([&](const interior::LevelExtents& extents) {
        return AssembledGpu(std::move(device), plan, geometry, window.handle.get(), settings, extents, Asked(ours, settings.asksToBeLeftOut))
            .and_then([&](Gpu gpu) { return Uncovered(gpu, settings, Present(ours)).transform([&] { return std::move(gpu); }); })
            .and_then([&](Gpu gpu) { return Started(std::move(gpu), std::move(runtime), plan); })
            .and_then([&](Ready r) { return Assembled(std::move(held), std::move(r), plan, std::move(window), panel, console, settings, options, extents); });
    });
}

} // namespace real
