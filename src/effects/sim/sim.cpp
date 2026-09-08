#include "effects/sim/sim.h"

#include "infrastructure/array_util.h"
#include "infrastructure/fold.h"
#include "infrastructure/overloaded.h"

#include <algorithm>

namespace sim {
namespace {

using infra::Fail;
using infra::Result;
using namespace interior;

struct Draw
{
    std::uint64_t value;
    infra::RngState next;
};

[[nodiscard]] Draw DrawFrom(infra::RngState rng) noexcept
{
    const infra::RngDraw d = infra::NextRandom(rng);
    return Draw{ d.value, d.next };
}

[[nodiscard]] bool Hits(std::uint64_t random, std::uint32_t perMillion) noexcept
{
    return (random % 1000000u) < perMillion;
}

[[nodiscard]] SimWorld WithRng(const SimWorld& w, infra::RngState rng) noexcept
{
    return SimWorld{ rng, w.states, w.clockMicroseconds, w.fence, w.backBuffer, w.framesUntilQuit, w.readbackWritten, w.presented, w.evaluated };
}

[[nodiscard]] SimWorld WithStates(const SimWorld& w, const StateTable& states) noexcept
{
    return SimWorld{ w.rng, states, w.clockMicroseconds, w.fence, w.backBuffer, w.framesUntilQuit, w.readbackWritten, w.presented, w.evaluated };
}

[[nodiscard]] SimWorld WithClock(const SimWorld& w, std::uint64_t clock) noexcept
{
    return SimWorld{ w.rng, w.states, clock, w.fence, w.backBuffer, w.framesUntilQuit, w.readbackWritten, w.presented, w.evaluated };
}

[[nodiscard]] SimWorld WithFence(const SimWorld& w, std::uint64_t fence) noexcept
{
    return SimWorld{ w.rng, w.states, w.clockMicroseconds, fence, w.backBuffer, w.framesUntilQuit, w.readbackWritten, w.presented, w.evaluated };
}

[[nodiscard]] SimWorld WithPresent(const SimWorld& w) noexcept
{
    return SimWorld{ w.rng, w.states, w.clockMicroseconds, w.fence, (w.backBuffer + 1) % kBackBufferCount, w.framesUntilQuit, w.readbackWritten, w.presented + 1, w.evaluated };
}

[[nodiscard]] SimWorld WithEvaluated(const SimWorld& w) noexcept
{
    return SimWorld{ w.rng, w.states, w.clockMicroseconds, w.fence, w.backBuffer, w.framesUntilQuit, w.readbackWritten, w.presented, w.evaluated + 1 };
}

[[nodiscard]] SimWorld WithReadback(const SimWorld& w, FrameSlot slot, bool written) noexcept
{
    return SimWorld{ w.rng, w.states, w.clockMicroseconds, w.fence, w.backBuffer, w.framesUntilQuit, infra::WithElement(w.readbackWritten, slot.Get(), written), w.presented, w.evaluated };
}

[[nodiscard]] SimWorld WithCountdown(const SimWorld& w) noexcept
{
    return SimWorld{ w.rng, w.states, w.clockMicroseconds, w.fence, w.backBuffer, w.framesUntilQuit - std::min(w.framesUntilQuit, 1u), w.readbackWritten, w.presented, w.evaluated };
}

// --- legality checks: the simulated GPU refuses what the real one would render wrong ---

[[nodiscard]] bool IsReadable(const StateTable& states, const ResourceId& id) noexcept
{
    return StateOf(states, id) == ResourceState::ShaderRead;
}

[[nodiscard]] bool IsWritable(const StateTable& states, const ResourceId& id) noexcept
{
    return StateOf(states, id) == ResourceState::UnorderedAccess;
}

[[nodiscard]] bool IsBoundReadable(const StateTable& states, const std::optional<ResourceId>& id) noexcept
{
    return !id.has_value() || IsReadable(states, *id);
}

[[nodiscard]] bool IsBoundWritable(const StateTable& states, const std::optional<ResourceId>& id) noexcept
{
    return !id.has_value() || IsWritable(states, *id);
}

[[nodiscard]] bool AreSrvsReadable(const StateTable& states, const Binding& b) noexcept
{
    return std::ranges::all_of(b.srv, [&states](const std::optional<ResourceId>& id) { return IsBoundReadable(states, id); });
}

[[nodiscard]] bool AreUavsWritable(const StateTable& states, const Binding& b) noexcept
{
    return std::ranges::all_of(b.uav, [&states](const std::optional<ResourceId>& id) { return IsBoundWritable(states, id); });
}

[[nodiscard]] Result<SimWorld, SimError> CheckTransition(const SimWorld& w, const Transition& t) noexcept
{
    if (StateOf(w.states, t.resource) != t.from)
        return Fail(SimError::IllegalTransition);
    return WithStates(w, infra::WithElement(w.states, SlotOf(t.resource), t.to));
}

[[nodiscard]] Result<SimWorld, SimError> CheckUavs(const SimWorld& w, const Dispatch& d) noexcept
{
    if (!AreUavsWritable(w.states, d.binding))
        return Fail(SimError::ResourceNotWritable);
    return w;
}

[[nodiscard]] Result<SimWorld, SimError> CheckDispatch(const SimWorld& w, const Dispatch& d) noexcept
{
    if (!AreSrvsReadable(w.states, d.binding))
        return Fail(SimError::ResourceNotReadable);
    return CheckUavs(w, d);
}

[[nodiscard]] bool IsCopySource(const StateTable& states, const ResourceId& id) noexcept
{
    return StateOf(states, id) == ResourceState::CopySource || StateOf(states, id) == ResourceState::GenericRead;
}

[[nodiscard]] bool IsCopyDest(const StateTable& states, const ResourceId& id) noexcept
{
    return StateOf(states, id) == ResourceState::CopyDest;
}

[[nodiscard]] SimWorld NoteReadback(const SimWorld& w, const CopyBuffer& c) noexcept
{
    return c.destination.kind == ResourceKind::StatsReadback ? WithReadback(w, c.destination.slot, true) : w;
}

[[nodiscard]] Result<SimWorld, SimError> CheckCopyDestination(const SimWorld& w, const CopyBuffer& c) noexcept
{
    if (!IsCopyDest(w.states, c.destination))
        return Fail(SimError::ResourceNotWritable);
    return NoteReadback(w, c);
}

[[nodiscard]] Result<SimWorld, SimError> CheckCopy(const SimWorld& w, const CopyBuffer& c) noexcept
{
    if (!IsCopySource(w.states, c.source))
        return Fail(SimError::ResourceNotReadable);
    return CheckCopyDestination(w, c);
}

[[nodiscard]] bool IsRenderTarget(const StateTable& states, const ResourceId& id) noexcept
{
    return StateOf(states, id) == ResourceState::RenderTarget;
}

[[nodiscard]] Result<SimWorld, SimError> CheckClear(const SimWorld& w, const ClearTarget& c) noexcept
{
    if (!IsRenderTarget(w.states, c.target))
        return Fail(SimError::ResourceNotWritable);
    return w;
}

[[nodiscard]] bool AreGuidesReadable(const StateTable& s, const ResourceId& depth, const ResourceId& mv) noexcept
{
    return IsReadable(s, depth) && IsReadable(s, mv);
}

struct ModelCheck
{
    interior::ModelIo io;
    SimError failure;
};

[[nodiscard]] bool AreInputsReadable(const StateTable& s, const interior::ModelIo& io) noexcept
{
    return IsReadable(s, io.color) && AreGuidesReadable(s, io.depth, io.motionVectors);
}

[[nodiscard]] Result<SimWorld, SimError> Inject(const SimWorld& w, std::uint32_t perMillion, SimError failure) noexcept
{
    const Draw d = DrawFrom(w.rng);
    if (Hits(d.value, perMillion))
        return Fail(failure);
    return WithRng(w, d.next);
}

[[nodiscard]] Result<SimWorld, SimError> InjectNgx(const SimWorld& w, const FailureRates& rates, SimError failure) noexcept
{
    return Inject(w, rates.ngxFailurePerMillion, failure).transform(WithEvaluated);
}

[[nodiscard]] Result<SimWorld, SimError> CheckModelOutput(const SimWorld& w, const ModelCheck& check, const FailureRates& rates) noexcept
{
    if (!IsWritable(w.states, check.io.output))
        return Fail(SimError::ResourceNotWritable);
    return InjectNgx(w, rates, check.failure);
}

[[nodiscard]] Result<SimWorld, SimError> CheckModel(const SimWorld& w, const ModelCheck& check, const FailureRates& rates) noexcept
{
    if (!AreInputsReadable(w.states, check.io))
        return Fail(SimError::ResourceNotReadable);
    return CheckModelOutput(w, check, rates);
}

[[nodiscard]] Result<SimWorld, SimError> CheckSr(const SimWorld& w, const EvaluateSr& e, const FailureRates& rates) noexcept
{
    return CheckModel(w, ModelCheck{ e.io, SimError::NgxSuperResolutionFailed }, rates);
}

[[nodiscard]] Result<SimWorld, SimError> CheckNr(const SimWorld& w, const EvaluateNr& e, const FailureRates& rates) noexcept
{
    return CheckModel(w, ModelCheck{ e.io, SimError::NgxNeuralRenderingFailed }, rates);
}

[[nodiscard]] bool AreDrawSourcesReadable(const StateTable& s, const interior::Draw& d) noexcept
{
    return IsReadable(s, d.processed) && IsReadable(s, d.original);
}

[[nodiscard]] Result<SimWorld, SimError> CheckDrawTarget(const SimWorld& w, const interior::Draw& d) noexcept
{
    if (!IsRenderTarget(w.states, d.target))
        return Fail(SimError::ResourceNotWritable);
    return w;
}

[[nodiscard]] Result<SimWorld, SimError> CheckDraw(const SimWorld& w, const interior::Draw& d) noexcept
{
    if (!AreDrawSourcesReadable(w.states, d))
        return Fail(SimError::ResourceNotReadable);
    return CheckDrawTarget(w, d);
}

[[nodiscard]] Result<SimWorld, SimError> CheckSubmit(const SimWorld& w, const Submit&) noexcept
{
    return WithFence(w, w.fence + 1);
}

[[nodiscard]] bool IsPresentable(const StateTable& states, std::uint32_t backBuffer) noexcept
{
    return BackBufferIndexTag::Parse(backBuffer).transform([&states](BackBufferIndex index) { return StateOf(states, BackBufferId(index)) == ResourceState::Present; }).value_or(false);
}

[[nodiscard]] Result<SimWorld, SimError> CheckPresent(const SimWorld& w, const Present&) noexcept
{
    if (!IsPresentable(w.states, w.backBuffer))
        return Fail(SimError::PresentFailed);
    return WithPresent(w);
}

[[nodiscard]] Result<SimWorld, SimError> ExecuteStep(const SimWorld& w, const Step& step, const FailureRates& rates) noexcept
{
    return std::visit(infra::Overloaded{
                          [&](const Transition& t) { return CheckTransition(w, t); },
                          [&](const Dispatch& d) { return CheckDispatch(w, d); },
                          [&](const CopyBuffer& c) { return CheckCopy(w, c); },
                          [&](const ClearTarget& c) { return CheckClear(w, c); },
                          [&](const EvaluateSr& e) { return CheckSr(w, e, rates); },
                          [&](const EvaluateNr& e) { return CheckNr(w, e, rates); },
                          [&](const interior::Draw& d) { return CheckDraw(w, d); },
                          [&](const Submit& s) { return CheckSubmit(w, s); },
                          [&](const Present& p) { return CheckPresent(w, p); },
                      },
                      step);
}

[[nodiscard]] Result<SimWorld, SimError> InjectDeviceLoss(const SimWorld& w, const FailureRates& rates) noexcept
{
    return Inject(w, rates.deviceRemovedPerMillion, SimError::DeviceRemoved);
}

[[nodiscard]] Result<SimWorld, SimError> InjectFrameStartFailures(const SimWorld& w, const FailureRates& rates) noexcept
{
    return Inject(w, rates.fenceTimeoutPerMillion, SimError::FenceTimeout).and_then([&rates](const SimWorld& n) { return Inject(n, rates.captureLostPerMillion, SimError::CaptureLost); });
}

[[nodiscard]] std::optional<Fraction> FractionIf(bool present, float value) noexcept
{
    if (!present)
        return std::nullopt;
    return FractionTag::Parse(value).transform([](Fraction f) { return std::optional<Fraction>{ f }; }).value_or(std::nullopt);
}

// The operator moving the panel's controls: the model toggles, the intensity moves, the display waits or not.
[[nodiscard]] LiveSettings MovedControls(std::uint64_t value) noexcept
{
    const Options d = DefaultOptions();
    const LiveSettings base = DefaultLive(d);
    const Result<Strength, UnitError> intensity = StrengthTag::Parse(static_cast<float>(value % 400u) / 100.0f);
    ENSURE(intensity.has_value());
    return LiveSettings{
        (value % 2u) == 0u,  NrTuning{ d.tuning.preset, *intensity, d.tuning.style, d.tuning.localStructure, d.tuning.localTone, d.tuning.skinStructure, d.tuning.autoMask, d.tuning.uiCorrection },
        (value % 3u) == 0u,  base.mvScaleX,
        base.mvScaleY,       (value % 5u) != 0u,
        base.resetThreshold, base.depth
    };
}

[[nodiscard]] std::optional<LiveSettings> ControlsIf(bool present, std::uint64_t value) noexcept
{
    if (!present)
        return std::nullopt;
    return MovedControls(value);
}

struct FrameDraws
{
    bool fresh;
    bool toggleOriginal;
    bool toggleSplit;
    std::uint64_t clockJump;
    float unmatched;
    std::optional<Fraction> splitRequest;
    std::optional<LiveSettings> controlRequest;
    SimWorld world;
};

[[nodiscard]] FrameDraws DrawnFrom(const SimWorld& w, const Draw& a, const Draw& b, const Draw& c) noexcept
{
    const Draw d = DrawFrom(c.next);
    const Draw e = DrawFrom(d.next);
    const Draw f = DrawFrom(e.next);
    return FrameDraws{ (a.value % 4u) != 0u,
                       (b.value % 97u) == 0u,
                       (b.value % 89u) == 0u,
                       1000u + (c.value % 3000000u),
                       static_cast<float>(d.value % 1001u) / 1000.0f,
                       FractionIf((e.value % 11u) == 0u, static_cast<float>(e.value % 1001u) / 1000.0f), // the operator dragging the divider
                       ControlsIf((f.value % 13u) == 0u, f.value),
                       WithRng(w, f.next) };
}

[[nodiscard]] FrameDraws DrawFrame(const SimWorld& w) noexcept
{
    const Draw a = DrawFrom(w.rng);
    const Draw b = DrawFrom(a.next);
    return DrawnFrom(w, a, b, DrawFrom(b.next));
}

[[nodiscard]] FrameInput InputFrom(const FrameDraws& draws, const SimWorld& w, bool statsPending, bool quit) noexcept
{
    const Result<BackBufferIndex, UnitError> buffer = BackBufferIndexTag::Parse(w.backBuffer);
    ENSURE(buffer.has_value());
    return FrameInput{
        draws.fresh,          *buffer, FractionIf(statsPending, draws.unmatched), InstantTag::Parse(w.clockMicroseconds), draws.toggleOriginal, draws.toggleSplit, draws.splitRequest, std::nullopt,
        draws.controlRequest, quit
    };
}

} // namespace

SimEnvironment::SimEnvironment(std::uint64_t seed, const SessionPlan& plan, FailureRates rates, std::uint32_t framesUntilQuit) noexcept
    : plan_(plan), rates_(rates), world_{ infra::SeedRng(seed), InitialStates(), 0, 0, 0, framesUntilQuit, { false, false }, 0, 0 }
{
}

[[nodiscard]] bool HasStatsFor(const FrameState& state, const SimWorld& w, FrameSlot slot) noexcept
{
    return state.statsPending[slot.Get()] && w.readbackWritten[slot.Get()];
}

[[nodiscard]] SimWorld AdvancedWorld(const FrameDraws& draws, FrameSlot slot) noexcept
{
    return WithReadback(WithClock(draws.world, draws.world.clockMicroseconds + draws.clockJump), slot, false);
}

[[nodiscard]] FrameStart StartFrom(const SimWorld& checked, const SimWorld& advanced, const FrameDraws& draws, const FrameState& state, FrameSlot slot) noexcept
{
    return FrameStart{ InputFrom(draws, advanced, HasStatsFor(state, checked, slot), checked.framesUntilQuit == 0) };
}

struct Begun
{
    SimWorld world;
    FrameStart start;
};

[[nodiscard]] Begun Begin(const SimWorld& checked, const FrameState& state) noexcept
{
    const FrameSlot slot = SlotOfFrame(state.number);
    const FrameDraws draws = DrawFrame(WithCountdown(checked));
    const SimWorld advanced = AdvancedWorld(draws, slot);
    return Begun{ advanced, StartFrom(checked, advanced, draws, state, slot) };
}

Result<FrameStart, SimError> SimEnvironment::BeginFrame(const FrameState& state) noexcept
{
    const Result<SimWorld, SimError> checked = InjectFrameStartFailures(world_, rates_);
    if (!checked.has_value())
        return Fail(checked.error());
    const Begun begun = Begin(*checked, state);
    world_ = begun.world;
    return begun.start;
}

[[nodiscard]] Result<SimWorld, SimError> ExecuteAll(const SimWorld& start, const FramePlan& plan, const FailureRates& rates) noexcept
{
    return infra::FoldResult(plan.steps.Items(), Result<SimWorld, SimError>(start), [&rates](const SimWorld& w, const Step& step) { return ExecuteStep(w, step, rates); });
}

Result<ExecutionReport, SimError> SimEnvironment::Execute(const FramePlan& plan) noexcept
{
    const Result<SimWorld, SimError> executed = InjectDeviceLoss(world_, rates_).and_then([&](const SimWorld& start) { return ExecuteAll(start, plan, rates_); });
    if (!executed.has_value())
        return Fail(executed.error());
    world_ = *executed;
    return ExecutionReport{ FenceValueTag::Parse(world_.fence), static_cast<std::uint32_t>(plan.steps.Size()) };
}

const char* Describe(SimError error) noexcept
{
    switch (error)
    {
    case SimError::DeviceRemoved: return "simulated device removal";
    case SimError::CaptureLost: return "simulated capture loss";
    case SimError::FenceTimeout: return "simulated fence timeout";
    case SimError::NgxSuperResolutionFailed: return "simulated DLSS Super Resolution failure";
    case SimError::NgxNeuralRenderingFailed: return "simulated DLSS 5 failure";
    case SimError::IllegalTransition: return "transition from a state the resource is not in";
    case SimError::ResourceNotReadable: return "resource bound for reading is not in the shader-read state";
    case SimError::ResourceNotWritable: return "resource bound for writing is not in the writable state";
    case SimError::OpticalFlowFailed: return "simulated optical flow failure";
    case SimError::PresentFailed: return "back buffer not in the present state";
    }
    return "simulation error";
}

} // namespace sim
