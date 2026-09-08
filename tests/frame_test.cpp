// WAIVER(R2): test suites accumulate failure counts and drive generated sequences with loops.
#include "infrastructure/overloaded.h"
#include "interior/frame.h"
#include "interior/plan.h"
#include "interior/pyramid.h"
#include "tests/test_registry.h"

#include <algorithm>
#include <ranges>
#include <variant>
#include <vector>

namespace tests {
namespace {

using namespace interior;

// A pure checker that replays a plan's transitions and verifies every access precondition.
struct Replay
{
    StateTable states;
    bool valid;
    std::uint32_t submits;
    std::uint32_t presents;
};

[[nodiscard]] Replay Apply(const Replay& r, const Transition& t) noexcept
{
    const bool ok = StateOf(r.states, t.resource) == t.from;
    return Replay{ infra::WithElement(r.states, SlotOf(t.resource), t.to), r.valid && ok, r.submits, r.presents };
}

[[nodiscard]] bool Readable(const StateTable& s, const std::optional<ResourceId>& id) noexcept
{
    return !id.has_value() || StateOf(s, *id) == ResourceState::ShaderRead;
}

[[nodiscard]] bool Writable(const StateTable& s, const std::optional<ResourceId>& id) noexcept
{
    return !id.has_value() || StateOf(s, *id) == ResourceState::UnorderedAccess;
}

[[nodiscard]] Replay Apply(const Replay& r, const Dispatch& d) noexcept
{
    const bool srvs = std::ranges::all_of(d.binding.srv, [&](const auto& id) { return Readable(r.states, id); });
    const bool uavs = std::ranges::all_of(d.binding.uav, [&](const auto& id) { return Writable(r.states, id); });
    return Replay{ r.states, r.valid && srvs && uavs && d.constants.count <= 16, r.submits, r.presents };
}

[[nodiscard]] Replay Apply(const Replay& r, const CopyBuffer& c) noexcept
{
    const ResourceState src = StateOf(r.states, c.source);
    const bool ok = (src == ResourceState::CopySource || src == ResourceState::GenericRead) && StateOf(r.states, c.destination) == ResourceState::CopyDest;
    return Replay{ r.states, r.valid && ok, r.submits, r.presents };
}

[[nodiscard]] Replay Apply(const Replay& r, const ClearTarget& c) noexcept
{
    return Replay{ r.states, r.valid && StateOf(r.states, c.target) == ResourceState::RenderTarget, r.submits, r.presents };
}

[[nodiscard]] bool ModelIoValid(const StateTable& states, const ModelIo& io) noexcept
{
    return Readable(states, io.color) && Readable(states, io.depth) && Readable(states, io.motionVectors) && Writable(states, io.output);
}

[[nodiscard]] Replay Apply(const Replay& r, const EvaluateSr& e) noexcept
{
    return Replay{ r.states, r.valid && ModelIoValid(r.states, e.io), r.submits, r.presents };
}

[[nodiscard]] Replay Apply(const Replay& r, const EvaluateNr& e) noexcept
{
    return Replay{ r.states, r.valid && ModelIoValid(r.states, e.io) && e.io.color != e.io.output, r.submits, r.presents };
}

[[nodiscard]] Replay Apply(const Replay& r, const Draw& d) noexcept
{
    const bool ok = Readable(r.states, d.processed) && Readable(r.states, d.original) && StateOf(r.states, d.target) == ResourceState::RenderTarget;
    return Replay{ r.states, r.valid && ok, r.submits, r.presents };
}

[[nodiscard]] Replay Apply(const Replay& r, const Submit&) noexcept
{
    return Replay{ r.states, r.valid, r.submits + 1, r.presents };
}

[[nodiscard]] Replay Apply(const Replay& r, const Present&) noexcept
{
    return Replay{ r.states, r.valid && r.submits > 0, r.submits, r.presents + 1 };
}

[[nodiscard]] Replay ReplayPlan(const StateTable& start, const FramePlan& plan) noexcept
{
    return std::ranges::fold_left(plan.steps.Items(), Replay{ start, true, 0, 0 }, [](const Replay& r, const Step& step) { return std::visit([&r](const auto& s) { return Apply(r, s); }, step); });
}

[[nodiscard]] Extent RandomExtent(infra::RngState& rng) noexcept
{
    const auto w = PixelCountTag::Parse(proptest::DrawBetween(rng, 16, 4096));
    const auto h = PixelCountTag::Parse(proptest::DrawBetween(rng, 16, 2160));
    REQUIRE(w.has_value() && h.has_value());
    return Extent{ *w, *h };
}

[[nodiscard]] Extent ScaledExtent(const Extent& source, std::uint32_t factor) noexcept
{
    const auto w = PixelCountTag::Parse(std::min(kMaxPixelCount, source.width.Get() * factor));
    const auto h = PixelCountTag::Parse(std::min(kMaxPixelCount, source.height.Get() * factor));
    REQUIRE(w.has_value() && h.has_value());
    return Extent{ *w, *h };
}

[[nodiscard]] SessionPlan RandomPlan(infra::RngState& rng) noexcept
{
    const Extent source = RandomExtent(rng);
    const bool sr = proptest::DrawBool(rng);
    const Extent target = sr ? ScaledExtent(source, proptest::DrawBetween(rng, 1, 3)) : source;
    const Options d = DefaultOptions();
    const LevelCount levels = LevelCountFor(source);
    const auto finest = LevelIndexTag::Parse(std::min(proptest::DrawBelow(rng, 3), levels.Get() - 1));
    const auto scaleX = ScaleTag::Parse(static_cast<float>(target.width.Get()) / static_cast<float>(source.width.Get()));
    const auto scaleY = ScaleTag::Parse(static_cast<float>(target.height.Get()) / static_cast<float>(source.height.Get()));
    const auto flow = GridExtent(source, 1);
    REQUIRE(finest.has_value() && scaleX.has_value() && scaleY.has_value() && flow.has_value());
    const std::array<MotionBackend, 3> backends{ MotionBackend::BuiltIn, MotionBackend::NvOpticalFlow, MotionBackend::None };
    return SessionPlan{ source,
                        target,
                        target,
                        sr ? std::optional<SrChoice>{ SrChoice{ SrQuality::Quality, source, target, d.srPreset, false } } : std::nullopt,
                        proptest::DrawBool(rng),
                        d.tuning,
                        backends[proptest::DrawBelow(rng, 3)],
                        levels,
                        *finest,
                        GridSize::One,
                        PerfLevel::Medium,
                        *flow,
                        *scaleX,
                        *scaleY,
                        d.depthValue,
                        d.resetThreshold,
                        ColorFormat::Rgba8,
                        DisplayMode::Processed,
                        false,
                        true };
}

[[nodiscard]] FrameInput RandomInput(infra::RngState& rng, std::uint64_t clock, std::uint32_t backBuffer) noexcept
{
    const auto buffer = BackBufferIndexTag::Parse(backBuffer % kBackBufferCount);
    const auto unmatched = FractionTag::Parse(proptest::DrawUnit(rng));
    REQUIRE(buffer.has_value() && unmatched.has_value());
    return FrameInput{ proptest::DrawBelow(rng, 4) != 0,
                       *buffer,
                       proptest::DrawBool(rng) ? std::optional<Fraction>{ *unmatched } : std::nullopt,
                       InstantTag::Parse(clock),
                       proptest::DrawBelow(rng, 20) == 0,
                       proptest::DrawBelow(rng, 20) == 0,
                       proptest::DrawBelow(rng, 8) == 0 ? std::optional<Fraction>{ *FractionTag::Parse(static_cast<float>(proptest::DrawBelow(rng, 1001)) / 1000.0f) } : std::nullopt,
                       std::nullopt,
                       false };
}

[[nodiscard]] bool PlansAreValidOverRandomSequences(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    FrameState state = InitialFrameState(plan);
    const std::uint32_t frames = proptest::DrawBetween(rng, 1, 12);
    std::uint64_t clock = 0;
    for (std::uint32_t i = 0; i < frames; ++i) // WAIVER(R2): test drives a sequence of frames.
    {
        clock += proptest::DrawBelow(rng, 2000000);
        const FrameInput input = RandomInput(rng, clock, i);
        const auto framePlan = PlanFrame(plan, state, input);
        if (!framePlan.has_value())
            return false;
        const Replay replay = ReplayPlan(state.states, *framePlan);
        if (!replay.valid || replay.presents != 1 || replay.states != framePlan->next.states)
            return false;
        state = framePlan->next;
    }
    return true;
}

[[nodiscard]] bool FreshFrameEvaluatesConfiguredModels(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState state = InitialFrameState(plan);
    const FrameInput input = FrameInput{ true, *BackBufferIndexTag::Parse(0), std::nullopt, InstantTag::Parse(0), false, false, std::nullopt, std::nullopt, false };
    const auto framePlan = PlanFrame(plan, state, input);
    if (!framePlan.has_value())
        return false;
    const auto count = [&](auto pred) { return std::ranges::count_if(framePlan->steps.Items(), pred); };
    const auto srs = count([](const Step& s) { return std::holds_alternative<EvaluateSr>(s); });
    const auto nrs = count([](const Step& s) { return std::holds_alternative<EvaluateNr>(s); });
    return srs == (plan.superResolution.has_value() ? 1 : 0) && nrs == (plan.neuralRendering ? 1 : 0);
}

[[nodiscard]] bool FirstFreshFrameResetsHistory(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState state = InitialFrameState(plan);
    const FrameInput input = FrameInput{ true, *BackBufferIndexTag::Parse(0), std::nullopt, InstantTag::Parse(0), false, false, std::nullopt, std::nullopt, false };
    const auto framePlan = PlanFrame(plan, state, input);
    if (!framePlan.has_value())
        return false;
    return std::ranges::all_of(framePlan->steps.Items(), [](const Step& s) {
        return std::visit(infra::Overloaded{ [](const EvaluateSr& e) { return e.reset; }, [](const EvaluateNr& e) { return e.reset; }, [](const auto&) { return true; } }, s);
    });
}

[[nodiscard]] bool RepeatFrameOnlyBlits(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState state = InitialFrameState(plan);
    const FrameInput input = FrameInput{ false, *BackBufferIndexTag::Parse(1), std::nullopt, InstantTag::Parse(0), false, false, std::nullopt, std::nullopt, false };
    const auto framePlan = PlanFrame(plan, state, input);
    if (!framePlan.has_value())
        return false;
    const bool noDispatch = std::ranges::none_of(framePlan->steps.Items(), [](const Step& s) { return std::holds_alternative<Dispatch>(s) || std::holds_alternative<EvaluateNr>(s); });
    return noDispatch && framePlan->next.currentSet == state.currentSet && !framePlan->next.hasOutput;
}

[[nodiscard]] bool QuitStopsWithoutSideEffects(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState state = InitialFrameState(plan);
    const FrameInput input = FrameInput{ true, *BackBufferIndexTag::Parse(2), std::nullopt, InstantTag::Parse(0), false, false, std::nullopt, std::nullopt, true };
    const auto framePlan = PlanFrame(plan, state, input);
    return framePlan.has_value() && framePlan->stop;
}

[[nodiscard]] bool PlanIsDeterministic(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState state = InitialFrameState(plan);
    const FrameInput input = RandomInput(rng, 5, 0);
    return PlanFrame(plan, state, input) == PlanFrame(plan, state, input);
}

[[nodiscard]] bool DisplayToggleIsInvolutive(infra::RngState& rng) noexcept
{
    const std::array<DisplayMode, 3> modes{ DisplayMode::Processed, DisplayMode::Original, DisplayMode::Split };
    const DisplayMode start = modes[proptest::DrawBelow(rng, 3)];
    const bool original = proptest::DrawBool(rng);
    const DisplayMode once = NextDisplay(start, original, !original);
    const bool changes = once != start;
    const bool returnsFromProcessed = NextDisplay(NextDisplay(DisplayMode::Processed, original, !original), original, !original) == DisplayMode::Processed;
    return changes && returnsFromProcessed && NextDisplay(start, false, false) == start;
}

[[nodiscard]] bool StatsReadbackRequiresBuiltInMotion(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState state = InitialFrameState(plan);
    const FrameInput input = FrameInput{ true, *BackBufferIndexTag::Parse(0), std::nullopt, InstantTag::Parse(0), false, false, std::nullopt, std::nullopt, false };
    const auto framePlan = PlanFrame(plan, state, input);
    if (!framePlan.has_value())
        return false;
    const bool pending = framePlan->next.statsPending[0];
    return pending == (plan.motion == MotionBackend::BuiltIn);
}

[[nodiscard]] bool IsBlank(const FrameState& s) noexcept
{
    return !s.hasOutput && !s.hasPrevious && !s.resetPending && !s.zeroMotionWritten && !s.lastCapture.has_value() && !s.statsPending[0] && !s.statsPending[1];
}

[[nodiscard]] bool InitialStateIsBlank(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState s = InitialFrameState(plan);
    return IsBlank(s) && s.number.Get() == 0 && s.currentSet.Get() == 0 && s.display == plan.initialDisplay && s.states == InitialStates();
}

[[nodiscard]] FrameState WithLastCapture(const FrameState& s, Instant at) noexcept
{
    return FrameState{ s.number, s.currentSet, s.states,     s.hasOutput,    s.hasPrevious,   s.resetPending, s.zeroMotionWritten,
                       at,       s.display,    s.slotFences, s.statsPending, s.displaySource, s.split,        s.controls };
}

[[nodiscard]] bool LongPauseNeedsACaptureAndMoreThanTheLimit(infra::RngState& rng) noexcept
{
    const FrameState blank = InitialFrameState(RandomPlan(rng));
    const std::uint64_t at = proptest::Draw(rng) % 1000000000000ull;
    const FrameState captured = WithLastCapture(blank, InstantTag::Parse(at));
    const bool none = !IsLongPause(blank, InstantTag::Parse(at + kPauseResetMicroseconds + 1));
    const bool atLimit = !IsLongPause(captured, InstantTag::Parse(at + kPauseResetMicroseconds)) && !IsLongPause(captured, InstantTag::Parse(at));
    return none && atLimit && IsLongPause(captured, InstantTag::Parse(at + kPauseResetMicroseconds + 1));
}

[[nodiscard]] bool ThresholdIsExclusiveAndNeedsAValue(infra::RngState& rng) noexcept
{
    const auto threshold = FractionTag::Parse(static_cast<float>(proptest::DrawBelow(rng, 1000)) / 1000.0f);
    const auto above = FractionTag::Parse(std::min(1.0f, threshold->Get() + 0.001f));
    const bool exclusive = !ExceedsThreshold(*threshold, *threshold) && ExceedsThreshold(*above, *threshold) == (above->Get() > threshold->Get());
    return threshold.has_value() && above.has_value() && exclusive && !ExceedsThreshold(std::nullopt, *threshold);
}

[[nodiscard]] std::vector<std::size_t> AllSlots() noexcept
{
    std::vector<std::size_t> slots;
    for (const ResourceKind kind : { ResourceKind::Canvas, ResourceKind::ModelColor, ResourceKind::Depth, ResourceKind::MotionVectors, ResourceKind::Stats, ResourceKind::ZeroBuffer,
                                     ResourceKind::SrOutput, ResourceKind::NrOutput, ResourceKind::OpticalFlowOutput })
        slots.push_back(SlotOf(SimpleId(kind)));
    for (std::uint32_t i = 0; i < kBackBufferCount; ++i)
        slots.push_back(SlotOf(BackBufferId(*BackBufferIndexTag::Parse(i))));
    for (std::uint32_t i = 0; i < kFrameSlotCount; ++i)
        slots.push_back(SlotOf(ReadbackId(*FrameSlotTag::Parse(i))));
    for (std::uint32_t set = 0; set < 2; ++set)
        for (std::uint32_t level = 0; level < kMaxLevels; ++level)
            slots.push_back(SlotOf(LumaId(*SetIndexTag::Parse(set), *LevelIndexTag::Parse(level))));
    for (std::uint32_t level = 0; level < kMaxLevels; ++level)
        slots.push_back(SlotOf(FlowId(*LevelIndexTag::Parse(level))));
    return slots;
}

[[nodiscard]] bool SlotsCoverTheTableExactlyOnce(infra::RngState&) noexcept
{
    std::vector<std::size_t> slots = AllSlots();
    std::ranges::sort(slots);
    return slots.size() == kSlotCount && std::ranges::equal(slots, std::views::iota(std::size_t{ 0 }, kSlotCount));
}

[[nodiscard]] bool PredictsFromTheCoarserFlow(const Dispatch& d, const SessionPlan& plan) noexcept
{
    if (d.pass != PassId::Match || !d.binding.uav[0].has_value())
        return d.pass != PassId::Match;
    const std::uint32_t level = d.binding.uav[0]->level.Get();
    if (level + 1 == plan.levels.Get())
        return !d.binding.srv[2].has_value();
    return d.binding.srv[2].has_value() && d.binding.srv[2]->kind == ResourceKind::Flow && d.binding.srv[2]->level.Get() == level + 1;
}

[[nodiscard]] FrameInput FreshInput() noexcept
{
    return FrameInput{ true, *BackBufferIndexTag::Parse(0), std::nullopt, InstantTag::Parse(0), false, false, std::nullopt, std::nullopt, false };
}

[[nodiscard]] FrameInput RepeatInput() noexcept
{
    return FrameInput{ false, *BackBufferIndexTag::Parse(1), std::nullopt, InstantTag::Parse(1000), false, false, std::nullopt, std::nullopt, false };
}

[[nodiscard]] bool MatchDispatchesPredictFromTheCoarserFlow(infra::RngState& rng) noexcept
{
    SessionPlan plan = RandomPlan(rng);
    plan.motion = MotionBackend::BuiltIn;
    const auto planned = PlanFrame(plan, InitialFrameState(plan), FreshInput());
    if (!planned.has_value())
        return false;
    const auto matches = std::ranges::count_if(planned->steps.Items(), [](const Step& s) { return std::holds_alternative<Dispatch>(s) && std::get<Dispatch>(s).pass == PassId::Match; });
    const bool predicted = std::ranges::all_of(planned->steps.Items(), [&](const Step& s) { return !std::holds_alternative<Dispatch>(s) || PredictsFromTheCoarserFlow(std::get<Dispatch>(s), plan); });
    return predicted && static_cast<std::uint32_t>(matches) == plan.levels.Get() - plan.finestLevel.Get();
}

[[nodiscard]] std::uint32_t Draws(const StepList& steps) noexcept
{
    return static_cast<std::uint32_t>(std::ranges::count_if(steps.Items(), [](const Step& s) { return std::holds_alternative<Draw>(s); }));
}

[[nodiscard]] std::uint32_t Clears(const StepList& steps) noexcept
{
    return static_cast<std::uint32_t>(std::ranges::count_if(steps.Items(), [](const Step& s) { return std::holds_alternative<ClearTarget>(s); }));
}

[[nodiscard]] bool FreshFramesDrawAndBlankRepeatsClear(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState initial = InitialFrameState(plan);
    const auto blank = PlanFrame(plan, initial, RepeatInput());
    const auto fresh = PlanFrame(plan, initial, FreshInput());
    if (!blank.has_value() || !fresh.has_value())
        return false;
    const auto repeat = PlanFrame(plan, fresh->next, RepeatInput());
    const bool blankClears = Clears(blank->steps) == 1 && Draws(blank->steps) == 0;
    const bool freshDraws = Draws(fresh->steps) == 1 && Clears(fresh->steps) == 0 && fresh->next.hasOutput;
    return blankClears && freshDraws && repeat.has_value() && Draws(repeat->steps) == 1 && Clears(repeat->steps) == 0;
}

[[nodiscard]] FrameState WithSplitPosition(const FrameState& s, Fraction split) noexcept
{
    return FrameState{ s.number,      s.currentSet, s.states,     s.hasOutput,    s.hasPrevious,   s.resetPending, s.zeroMotionWritten,
                       s.lastCapture, s.display,    s.slotFences, s.statsPending, s.displaySource, split,          s.controls };
}

[[nodiscard]] const Draw* DrawIn(const StepList& steps) noexcept
{
    // WAIVER(R2): a search over the emitted steps; the cursor is the loop's own.
    for (std::size_t i = 0; i < steps.Size(); ++i)
        if (const Draw* draw = std::get_if<Draw>(&steps.At(i)); draw != nullptr)
            return draw;
    return nullptr;
}

// The random plan may or may not run the model; these properties need one that does.
[[nodiscard]] SessionPlan WithNeuralRendering(const SessionPlan& p) noexcept
{
    SessionPlan plan = p; // WAIVER(R2): a test fixture, copied and adjusted before use.
    plan.neuralRendering = true;
    return plan;
}

[[nodiscard]] const EvaluateNr* NeuralStepIn(const StepList& steps) noexcept
{
    // WAIVER(R2): a search over the emitted steps; the cursor is the loop's own.
    for (std::size_t i = 0; i < steps.Size(); ++i)
        if (const EvaluateNr* step = std::get_if<EvaluateNr>(&steps.At(i)); step != nullptr)
            return step;
    return nullptr;
}

[[nodiscard]] NrTuning WithIntensity(const NrTuning& t, Strength intensity) noexcept
{
    return NrTuning{ t.preset, intensity, t.style, t.localStructure, t.localTone, t.skinStructure, t.autoMask, t.uiCorrection };
}

[[nodiscard]] FrameInput RequestingControls(const ModelControls& controls) noexcept
{
    return FrameInput{ true, *BackBufferIndexTag::Parse(0), std::nullopt, InstantTag::Parse(0), false, false, std::nullopt, controls, false };
}

// A value moved on the panel has to reach the step that evaluates the model, not just the state.
[[nodiscard]] bool ANewIntensityReachesTheEvaluatedStep(infra::RngState& rng) noexcept
{
    const SessionPlan plan = WithNeuralRendering(RandomPlan(rng));
    const Strength intensity = *StrengthTag::Parse(static_cast<float>(proptest::DrawBelow(rng, 1000)) / 100.0f);
    const ModelControls controls = ModelControls{ true, WithIntensity(plan.tuning, intensity) };
    const auto framePlan = PlanFrame(plan, InitialFrameState(plan), RequestingControls(controls));
    if (!framePlan.has_value())
        return false;
    const EvaluateNr* step = NeuralStepIn(framePlan->steps);
    return step != nullptr && step->tuning.intensity == intensity && framePlan->next.controls == controls;
}

// Switching the model off has to drop its step and show the picture that skips it.
[[nodiscard]] bool SwitchingTheModelOffDropsItsStep(infra::RngState& rng) noexcept
{
    const SessionPlan plan = WithNeuralRendering(RandomPlan(rng));
    const ModelControls off = ModelControls{ false, plan.tuning };
    const auto framePlan = PlanFrame(plan, InitialFrameState(plan), RequestingControls(off));
    if (!framePlan.has_value())
        return false;
    const Draw* draw = DrawIn(framePlan->steps);
    return NeuralStepIn(framePlan->steps) == nullptr && draw != nullptr && draw->processed.kind != ResourceKind::NrOutput && !framePlan->next.controls.neuralRendering;
}

// Dragging the divider must reach the blit, not merely be recorded in the state.
[[nodiscard]] bool ADragMovesTheDividerInTheDrawnStep(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const FrameState state = InitialFrameState(plan);
    const Fraction requested = *FractionTag::Parse(static_cast<float>(proptest::DrawBelow(rng, 1001)) / 1000.0f);
    const FrameInput input = FrameInput{ true, *BackBufferIndexTag::Parse(0), std::nullopt, InstantTag::Parse(0), false, false, requested, std::nullopt, false };
    const auto framePlan = PlanFrame(plan, state, input);
    if (!framePlan.has_value())
        return false;
    const Draw* draw = DrawIn(framePlan->steps);
    return draw != nullptr && draw->split == requested && framePlan->next.split == requested;
}

// With no drag the divider stays where it was, whatever else the frame does.
[[nodiscard]] bool WithoutADragTheDividerHolds(infra::RngState& rng) noexcept
{
    const SessionPlan plan = RandomPlan(rng);
    const Fraction held = *FractionTag::Parse(static_cast<float>(proptest::DrawBelow(rng, 1001)) / 1000.0f);
    const FrameState state = WithSplitPosition(InitialFrameState(plan), held);
    const FrameInput input = FrameInput{ true, *BackBufferIndexTag::Parse(0), std::nullopt, InstantTag::Parse(0), false, false, std::nullopt, std::nullopt, false };
    const auto framePlan = PlanFrame(plan, state, input);
    if (!framePlan.has_value())
        return false;
    const Draw* draw = DrawIn(framePlan->steps);
    return draw != nullptr && draw->split == held && framePlan->next.split == held;
}

} // namespace

std::uint32_t FrameSuite(std::uint64_t seed) noexcept
{
    std::uint32_t failures = 0;
    failures += Failures(proptest::ForAll("slots cover the resource table exactly once", seed, 1, SlotsCoverTheTableExactlyOnce));
    failures += Failures(proptest::ForAll("match dispatches predict from the coarser flow", seed, 300, MatchDispatchesPredictFromTheCoarserFlow));
    failures += Failures(proptest::ForAll("fresh frames draw and blank repeats clear", seed, 300, FreshFramesDrawAndBlankRepeatsClear));
    failures += Failures(proptest::ForAll("the initial state is blank", seed, 100, InitialStateIsBlank));
    failures += Failures(proptest::ForAll("a long pause needs a capture and more than the limit", seed, 200, LongPauseNeedsACaptureAndMoreThanTheLimit));
    failures += Failures(proptest::ForAll("the reset threshold is exclusive and needs a value", seed, 200, ThresholdIsExclusiveAndNeedsAValue));
    failures += Failures(proptest::ForAll("frame plans are valid over random sequences", seed, 800, PlansAreValidOverRandomSequences));
    failures += Failures(proptest::ForAll("fresh frames evaluate exactly the configured models", seed, 400, FreshFrameEvaluatesConfiguredModels));
    failures += Failures(proptest::ForAll("the first fresh frame resets history", seed, 300, FirstFreshFrameResetsHistory));
    failures += Failures(proptest::ForAll("repeat frames only blit", seed, 300, RepeatFrameOnlyBlits));
    failures += Failures(proptest::ForAll("quit stops the session", seed, 100, QuitStopsWithoutSideEffects));
    failures += Failures(proptest::ForAll("planning is deterministic", seed, 200, PlanIsDeterministic));
    failures += Failures(proptest::ForAll("display toggles are involutive", seed, 100, DisplayToggleIsInvolutive));
    failures += Failures(proptest::ForAll("stats readback only with built-in motion", seed, 300, StatsReadbackRequiresBuiltInMotion));
    failures += Failures(proptest::ForAll("a drag moves the divider in the drawn step", seed, 200, ADragMovesTheDividerInTheDrawnStep));
    failures += Failures(proptest::ForAll("without a drag the divider holds", seed, 200, WithoutADragTheDividerHolds));
    failures += Failures(proptest::ForAll("a new intensity reaches the evaluated step", seed, 200, ANewIntensityReachesTheEvaluatedStep));
    failures += Failures(proptest::ForAll("switching the model off drops its step", seed, 200, SwitchingTheModelOffDropsItsStep));
    return failures;
}

} // namespace tests
