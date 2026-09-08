#include "interior/frame.h"

#include "infrastructure/array_util.h"
#include "infrastructure/fold.h"
#include "interior/pyramid.h"

#include <algorithm>
#include <array>
#include <bit>
#include <ranges>

namespace interior {
namespace {

using infra::Fail;

struct Builder
{
    StepList steps;
    StateTable states;
};

using BuildResult = Result<Builder, PlanFrameError>;

constexpr float kLambda = 0.004f;
constexpr float kZeroBias = 0.006f;
constexpr float kBadThreshold = 0.12f;
constexpr std::uint32_t kCoarseRadius = 3;
constexpr std::uint32_t kFineRadius = 1;
constexpr std::uint32_t kFlagHasPrediction = 1;
constexpr std::uint32_t kFlagCountUnmatched = 2;
constexpr std::uint32_t kFlagSubpixel = 4;

constexpr auto kZeroLevel = LevelIndexTag::Parse(0);
constexpr auto kZeroSet = SetIndexTag::Parse(0);
constexpr auto kZeroBuffer = BackBufferIndexTag::Parse(0);
constexpr auto kZeroSlot = FrameSlotTag::Parse(0);
static_assert(kZeroLevel.has_value() && kZeroSet.has_value() && kZeroBuffer.has_value() && kZeroSlot.has_value());

[[nodiscard]] PlanFrameError FromCapacity(infra::CapacityExceeded) noexcept
{
    return PlanFrameError::Capacity;
}

[[nodiscard]] PlanFrameError FromPyramid(PyramidError error) noexcept
{
    switch (error)
    {
    case PyramidError::Arithmetic: return PlanFrameError::Arithmetic;
    case PyramidError::Capacity: return PlanFrameError::Capacity;
    case PyramidError::Unit: return PlanFrameError::Unit;
    }
    return PlanFrameError::Unit;
}

[[nodiscard]] PlanFrameError FromUnit(UnitError) noexcept
{
    return PlanFrameError::Unit;
}

[[nodiscard]] std::uint32_t Bits(float value) noexcept
{
    return std::bit_cast<std::uint32_t>(value);
}

[[nodiscard]] Result<LevelIndex, PlanFrameError> Level(std::uint32_t raw) noexcept
{
    return LevelIndexTag::Parse(raw).transform_error(FromUnit);
}

[[nodiscard]] SetIndex OtherSet(SetIndex set) noexcept
{
    const Result<SetIndex, UnitError> other = SetIndexTag::Parse(set.Get() ^ 1u);
    ENSURE(other.has_value());
    return *other;
}

// --- step emission -----------------------------------------------------------------

[[nodiscard]] BuildResult Emit(const Builder& b, const Step& step) noexcept
{
    return b.steps.Push(step).transform([&b](const StepList& steps) { return Builder{ steps, b.states }; }).transform_error(FromCapacity);
}

[[nodiscard]] Builder WithState(const Builder& b, const ResourceId& id, ResourceState state) noexcept
{
    return Builder{ b.steps, infra::WithElement(b.states, SlotOf(id), state) };
}

[[nodiscard]] BuildResult MoveTo(const Builder& b, const ResourceId& id, ResourceState to) noexcept
{
    const ResourceState from = StateOf(b.states, id);
    if (from == to)
        return b;
    return Emit(b, Step{ Transition{ id, from, to } }).transform([&id, to](const Builder& next) { return WithState(next, id, to); });
}

[[nodiscard]] Binding Bind(std::optional<ResourceId> s0, std::optional<ResourceId> s1, std::optional<ResourceId> s2, std::optional<ResourceId> u0,
                           std::optional<ResourceId> u1) noexcept
{
    return Binding{ { s0, s1, s2, std::nullopt }, { u0, u1 } };
}

[[nodiscard]] Constants Consts(std::array<std::uint32_t, 16> values, std::uint32_t count) noexcept
{
    return Constants{ values, count };
}

[[nodiscard]] BuildResult EmitDispatch(const Builder& b, PassId pass, const Binding& binding, const Constants& constants, const Extent& extent) noexcept
{
    return GroupsFor(extent).transform_error(FromPyramid).and_then([&](ThreadGroups groups) { return Emit(b, Step{ Dispatch{ pass, binding, constants, groups } }); });
}

// --- phase one: convert and pyramid ---------------------------------------------------

[[nodiscard]] Constants ConvertConstants(const Extent& e) noexcept
{
    return Consts({ e.width.Get(), e.height.Get(), 0u, 0u }, 4);
}

[[nodiscard]] Constants DownsampleConstants(const Extent& dst, const Extent& src) noexcept
{
    return Consts({ dst.width.Get(), dst.height.Get(), src.width.Get(), src.height.Get() }, 4);
}

[[nodiscard]] BuildResult ConvertDispatch(const Builder& b, const SessionPlan& plan, SetIndex set) noexcept
{
    const Binding binding = Bind(SimpleId(ResourceKind::Canvas), std::nullopt, std::nullopt, SimpleId(ResourceKind::ModelColor), LumaId(set, *kZeroLevel));
    return EmitDispatch(b, PassId::Convert, binding, ConvertConstants(plan.source), plan.source);
}

[[nodiscard]] BuildResult ConvertSteps(const Builder& b, const SessionPlan& plan, SetIndex set) noexcept
{
    return MoveTo(b, SimpleId(ResourceKind::Canvas), ResourceState::ShaderRead)
        .and_then([set](const Builder& n) { return MoveTo(n, LumaId(set, *kZeroLevel), ResourceState::UnorderedAccess); })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::ModelColor), ResourceState::UnorderedAccess); })
        .and_then([&plan, set](const Builder& n) { return ConvertDispatch(n, plan, set); })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::Canvas), ResourceState::CopyDest); })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::ModelColor), ResourceState::ShaderRead); });
}

[[nodiscard]] BuildResult DownsampleLevel(const Builder& b, const LevelExtents& extents, SetIndex set, std::uint32_t level) noexcept
{
    return Level(level).and_then([&](LevelIndex dst)
    {
        return Level(level - 1).and_then([&](LevelIndex src)
        {
            return MoveTo(b, LumaId(set, src), ResourceState::ShaderRead)
                .and_then([&](const Builder& n) { return MoveTo(n, LumaId(set, dst), ResourceState::UnorderedAccess); })
                .and_then([&](const Builder& n)
                {
                    return EmitDispatch(n, PassId::Downsample, Bind(LumaId(set, src), std::nullopt, std::nullopt, LumaId(set, dst), std::nullopt),
                                        DownsampleConstants(extents.At(level), extents.At(level - 1)), extents.At(level));
                });
        });
    });
}

[[nodiscard]] BuildResult PyramidSteps(const Builder& b, const SessionPlan& plan, const LevelExtents& extents, SetIndex set) noexcept
{
    return infra::FoldResult(std::views::iota(std::uint32_t{ 1 }, plan.levels.Get()), BuildResult(b),
                             [&](const Builder& acc, std::uint32_t level) { return DownsampleLevel(acc, extents, set, level); })
        .and_then([&](const Builder& n) { return Level(plan.levels.Get() - 1).and_then([&](LevelIndex last) { return MoveTo(n, LumaId(set, last), ResourceState::ShaderRead); }); });
}

[[nodiscard]] BuildResult OpticalFlowHandoff(const Builder& b, SetIndex set) noexcept
{
    return MoveTo(b, LumaId(set, *kZeroLevel), ResourceState::Common)
        .and_then([set](const Builder& n) { return MoveTo(n, LumaId(OtherSet(set), *kZeroLevel), ResourceState::Common); })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::OpticalFlowOutput), ResourceState::Common); });
}

[[nodiscard]] BuildResult MotionPhaseOne(const Builder& b, const SessionPlan& plan, const LevelExtents& extents, SetIndex set) noexcept
{
    switch (plan.motion)
    {
    case MotionBackend::BuiltIn: return PyramidSteps(b, plan, extents, set);
    case MotionBackend::NvOpticalFlow: return OpticalFlowHandoff(b, set);
    case MotionBackend::None: return MoveTo(b, LumaId(set, *kZeroLevel), ResourceState::ShaderRead);
    }
    return b;
}

[[nodiscard]] BuildResult PhaseOne(const Builder& b, const SessionPlan& plan, const LevelExtents& extents, SetIndex set) noexcept
{
    return ConvertSteps(b, plan, set)
        .and_then([&](const Builder& n) { return MotionPhaseOne(n, plan, extents, set); })
        .and_then([](const Builder& n) { return Emit(n, Step{ Submit{ Phase::One } }); });
}

// --- phase two: matching ------------------------------------------------------------------

[[nodiscard]] bool IsCoarsest(const SessionPlan& plan, std::uint32_t level) noexcept
{
    return level + 1 == plan.levels.Get();
}

[[nodiscard]] bool IsFinest(const SessionPlan& plan, std::uint32_t level) noexcept
{
    return level == plan.finestLevel.Get();
}

[[nodiscard]] std::uint32_t PredictionFlag(const SessionPlan& plan, std::uint32_t level) noexcept
{
    return IsCoarsest(plan, level) ? 0u : kFlagHasPrediction;
}

[[nodiscard]] std::uint32_t FinestFlags(const SessionPlan& plan, std::uint32_t level) noexcept
{
    return IsFinest(plan, level) ? (kFlagCountUnmatched | kFlagSubpixel) : 0u;
}

[[nodiscard]] std::uint32_t MatchFlags(const SessionPlan& plan, std::uint32_t level) noexcept
{
    return PredictionFlag(plan, level) | FinestFlags(plan, level);
}

[[nodiscard]] std::uint32_t RadiusFor(const SessionPlan& plan, std::uint32_t level) noexcept
{
    return IsCoarsest(plan, level) ? kCoarseRadius : kFineRadius;
}

[[nodiscard]] Constants MatchConstants(const SessionPlan& plan, const Extent& e, std::uint32_t level) noexcept
{
    return Consts({ e.width.Get(), e.height.Get(), 0u, 0u, RadiusFor(plan, level), MatchFlags(plan, level), Bits(kLambda), Bits(kZeroBias), Bits(kBadThreshold), Bits(1.0f) }, 10);
}

[[nodiscard]] Constants FinalizeConstants(const Extent& from, const Extent& to, float scale) noexcept
{
    return Consts({ from.width.Get(), from.height.Get(), to.width.Get(), to.height.Get(), 0u, 0u, 0u, 0u, 0u, Bits(scale) }, 10);
}

[[nodiscard]] std::optional<ResourceId> PredictionOf(const SessionPlan& plan, std::uint32_t level) noexcept
{
    if (IsCoarsest(plan, level))
        return std::nullopt;
    return Level(level + 1).transform([](LevelIndex coarser) { return std::optional<ResourceId>{ FlowId(coarser) }; }).value_or(std::nullopt);
}

[[nodiscard]] BuildResult PredictionReady(const Builder& b, std::optional<ResourceId> prediction) noexcept
{
    if (!prediction.has_value())
        return b;
    return MoveTo(b, *prediction, ResourceState::ShaderRead);
}

[[nodiscard]] BuildResult MatchDispatch(const Builder& b, const SessionPlan& plan, const LevelExtents& extents, SetIndex set, std::uint32_t level, LevelIndex index) noexcept
{
    const std::optional<ResourceId> prediction = PredictionOf(plan, level);
    const Binding binding = Bind(LumaId(set, index), LumaId(OtherSet(set), index), prediction, FlowId(index), SimpleId(ResourceKind::Stats));
    return PredictionReady(b, prediction).and_then([&](const Builder& n) { return EmitDispatch(n, PassId::Match, binding, MatchConstants(plan, extents.At(level), level), extents.At(level)); });
}

[[nodiscard]] BuildResult MatchLevel(const Builder& b, const SessionPlan& plan, const LevelExtents& extents, SetIndex set, std::uint32_t level) noexcept
{
    return Level(level).and_then([&](LevelIndex index)
    {
        return MoveTo(b, LumaId(set, index), ResourceState::ShaderRead)
            .and_then([&](const Builder& n) { return MoveTo(n, LumaId(OtherSet(set), index), ResourceState::ShaderRead); })
            .and_then([&](const Builder& n) { return MoveTo(n, FlowId(index), ResourceState::UnorderedAccess); })
            .and_then([&](const Builder& n) { return MatchDispatch(n, plan, extents, set, level, index); });
    });
}

[[nodiscard]] std::uint32_t DescendingLevel(const SessionPlan& plan, std::uint32_t offset) noexcept
{
    return plan.levels.Get() - 1 - offset;
}

[[nodiscard]] BuildResult MatchAllLevels(const Builder& b, const SessionPlan& plan, const LevelExtents& extents, SetIndex set) noexcept
{
    const std::uint32_t count = plan.levels.Get() - plan.finestLevel.Get();
    return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, count), BuildResult(b),
                             [&](const Builder& acc, std::uint32_t offset) { return MatchLevel(acc, plan, extents, set, DescendingLevel(plan, offset)); });
}

[[nodiscard]] BuildResult StatsClear(const Builder& b) noexcept
{
    return MoveTo(b, SimpleId(ResourceKind::Stats), ResourceState::CopyDest)
        .and_then([](const Builder& n) { return Emit(n, Step{ CopyBuffer{ SimpleId(ResourceKind::ZeroBuffer), SimpleId(ResourceKind::Stats), ByteCountTag::Parse(kStatsBytes) } }); })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::Stats), ResourceState::UnorderedAccess); });
}

[[nodiscard]] BuildResult StatsReadback(const Builder& b, FrameSlot slot) noexcept
{
    return MoveTo(b, SimpleId(ResourceKind::Stats), ResourceState::CopySource)
        .and_then([slot](const Builder& n) { return Emit(n, Step{ CopyBuffer{ SimpleId(ResourceKind::Stats), ReadbackId(slot), ByteCountTag::Parse(4) } }); });
}

[[nodiscard]] BuildResult FinalizeSteps(const Builder& b, const SessionPlan& plan, const LevelExtents& extents) noexcept
{
    const Extent finest = extents.At(plan.finestLevel.Get());
    const float scale = static_cast<float>(1u << plan.finestLevel.Get());
    return MoveTo(b, FlowId(plan.finestLevel), ResourceState::ShaderRead)
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::MotionVectors), ResourceState::UnorderedAccess); })
        .and_then([&](const Builder& n)
        {
            return EmitDispatch(n, PassId::Finalize, Bind(std::nullopt, std::nullopt, FlowId(plan.finestLevel), SimpleId(ResourceKind::MotionVectors), std::nullopt),
                                FinalizeConstants(finest, plan.source, scale), plan.source);
        })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::MotionVectors), ResourceState::ShaderRead); });
}

[[nodiscard]] BuildResult BlockMatchPhaseTwo(const Builder& b, const SessionPlan& plan, const LevelExtents& extents, SetIndex set, FrameSlot slot) noexcept
{
    return StatsClear(b)
        .and_then([&](const Builder& n) { return MatchAllLevels(n, plan, extents, set); })
        .and_then([&](const Builder& n) { return FinalizeSteps(n, plan, extents); })
        .and_then([slot](const Builder& n) { return StatsReadback(n, slot); });
}

[[nodiscard]] Constants FlowToMvConstants(const SessionPlan& plan, float scale) noexcept
{
    return Consts({ plan.source.width.Get(), plan.source.height.Get(), plan.flowExtent.width.Get(), plan.flowExtent.height.Get(), GridCells(plan.nvofGrid), Bits(scale), 0u, 0u }, 8);
}

[[nodiscard]] BuildResult OpticalFlowPhaseTwo(const Builder& b, const SessionPlan& plan, bool hasPrevious) noexcept
{
    return MoveTo(b, SimpleId(ResourceKind::OpticalFlowOutput), ResourceState::ShaderRead)
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::MotionVectors), ResourceState::UnorderedAccess); })
        .and_then([&](const Builder& n)
        {
            return EmitDispatch(n, PassId::FlowToMv, Bind(SimpleId(ResourceKind::OpticalFlowOutput), std::nullopt, std::nullopt, SimpleId(ResourceKind::MotionVectors), std::nullopt),
                                FlowToMvConstants(plan, hasPrevious ? 1.0f : 0.0f), plan.source);
        })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::MotionVectors), ResourceState::ShaderRead); })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::OpticalFlowOutput), ResourceState::Common); });
}

[[nodiscard]] BuildResult ZeroMotionPhaseTwo(const Builder& b, const SessionPlan& plan, bool written) noexcept
{
    if (written)
        return b;
    return MoveTo(b, SimpleId(ResourceKind::MotionVectors), ResourceState::UnorderedAccess)
        .and_then([&](const Builder& n)
        {
            return EmitDispatch(n, PassId::Finalize, Bind(std::nullopt, std::nullopt, std::nullopt, SimpleId(ResourceKind::MotionVectors), std::nullopt),
                                FinalizeConstants(plan.source, plan.source, 0.0f), plan.source);
        })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::MotionVectors), ResourceState::ShaderRead); });
}

[[nodiscard]] BuildResult MotionPhaseTwo(const Builder& b, const SessionPlan& plan, const LevelExtents& extents, const FrameState& state, FrameSlot slot) noexcept
{
    switch (plan.motion)
    {
    case MotionBackend::BuiltIn: return BlockMatchPhaseTwo(b, plan, extents, state.currentSet, slot);
    case MotionBackend::NvOpticalFlow: return OpticalFlowPhaseTwo(b, plan, state.hasPrevious);
    case MotionBackend::None: return ZeroMotionPhaseTwo(b, plan, state.zeroMotionWritten);
    }
    return b;
}

// --- phase two: models ------------------------------------------------------------------------

[[nodiscard]] ModelIo ModelIoOf(ResourceKind color, ResourceKind output) noexcept
{
    return ModelIo{ SimpleId(color), SimpleId(ResourceKind::Depth), SimpleId(ResourceKind::MotionVectors), SimpleId(output) };
}

[[nodiscard]] EvaluateSr SrStep(const SessionPlan& plan, bool reset) noexcept
{
    return EvaluateSr{ ModelIoOf(ResourceKind::ModelColor, ResourceKind::SrOutput), plan.source, reset };
}

[[nodiscard]] BuildResult SuperResolutionSteps(const Builder& b, const SessionPlan& plan, bool reset) noexcept
{
    if (!plan.superResolution.has_value())
        return b;
    return MoveTo(b, SimpleId(ResourceKind::SrOutput), ResourceState::UnorderedAccess)
        .and_then([&](const Builder& n) { return Emit(n, Step{ SrStep(plan, reset) }); })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::SrOutput), ResourceState::ShaderRead); });
}

[[nodiscard]] ResourceKind ColorSourceOf(const SessionPlan& plan) noexcept
{
    return plan.superResolution.has_value() ? ResourceKind::SrOutput : ResourceKind::ModelColor;
}

[[nodiscard]] ResourceKind DisplaySourceOf(const SessionPlan& plan) noexcept
{
    return plan.neuralRendering ? ResourceKind::NrOutput : ColorSourceOf(plan);
}

[[nodiscard]] EvaluateNr NrStep(const SessionPlan& plan, bool reset) noexcept
{
    return EvaluateNr{ ModelIoOf(ColorSourceOf(plan), ResourceKind::NrOutput), plan.work, plan.source, plan.mvScaleX, plan.mvScaleY, reset };
}

[[nodiscard]] BuildResult NeuralRenderingSteps(const Builder& b, const SessionPlan& plan, bool reset) noexcept
{
    if (!plan.neuralRendering)
        return b;
    return MoveTo(b, SimpleId(ResourceKind::NrOutput), ResourceState::UnorderedAccess)
        .and_then([&](const Builder& n) { return Emit(n, Step{ NrStep(plan, reset) }); })
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::NrOutput), ResourceState::ShaderRead); });
}

// --- blit and present --------------------------------------------------------------------------

[[nodiscard]] BuildResult DrawSteps(const Builder& b, ResourceKind source, DisplayMode mode, BackBufferIndex index) noexcept
{
    return MoveTo(b, SimpleId(source), ResourceState::ShaderRead)
        .and_then([](const Builder& n) { return MoveTo(n, SimpleId(ResourceKind::ModelColor), ResourceState::ShaderRead); })
        .and_then([&](const Builder& n) { return Emit(n, Step{ Draw{ SimpleId(source), SimpleId(ResourceKind::ModelColor), BackBufferId(index), mode } }); });
}

[[nodiscard]] BuildResult ClearSteps(const Builder& b, BackBufferIndex index) noexcept
{
    return Emit(b, Step{ ClearTarget{ BackBufferId(index) } });
}

[[nodiscard]] BuildResult TargetContent(const Builder& b, bool hasOutput, ResourceKind source, DisplayMode mode, BackBufferIndex index) noexcept
{
    return hasOutput ? DrawSteps(b, source, mode, index) : ClearSteps(b, index);
}

[[nodiscard]] BuildResult BlitSteps(const Builder& b, bool hasOutput, ResourceKind source, DisplayMode mode, BackBufferIndex index) noexcept
{
    return MoveTo(b, BackBufferId(index), ResourceState::RenderTarget)
        .and_then([&](const Builder& n) { return TargetContent(n, hasOutput, source, mode, index); })
        .and_then([index](const Builder& n) { return MoveTo(n, BackBufferId(index), ResourceState::Present); })
        .and_then([](const Builder& n) { return Emit(n, Step{ Submit{ Phase::Two } }); })
        .and_then([](const Builder& n) { return Emit(n, Step{ Present{} }); });
}

// --- state bookkeeping ---------------------------------------------------------------------------

[[nodiscard]] bool IsHistoryStale(const FrameState& state, Instant now) noexcept
{
    return state.resetPending || IsLongPause(state, now);
}

[[nodiscard]] bool NeedsReset(const FrameState& state, Instant now) noexcept
{
    return !state.hasPrevious || IsHistoryStale(state, now);
}

[[nodiscard]] bool EmitsStats(const SessionPlan& plan, bool fresh) noexcept
{
    return fresh && plan.motion == MotionBackend::BuiltIn;
}

[[nodiscard]] SetIndex NextSet(const FrameState& state, bool fresh) noexcept
{
    return fresh ? OtherSet(state.currentSet) : state.currentSet;
}

[[nodiscard]] std::optional<Instant> NextCapture(const FrameState& state, const FrameInput& input) noexcept
{
    return input.freshCapture ? std::optional<Instant>{ input.now } : state.lastCapture;
}

[[nodiscard]] bool OrFresh(bool flag, const FrameInput& input) noexcept
{
    return flag || input.freshCapture;
}

[[nodiscard]] FrameState NextState(const SessionPlan& plan, const FrameState& state, const FrameInput& input, const StateTable& states, FrameSlot slot) noexcept
{
    return FrameState{ FrameNumberTag::Parse(state.number.Get() + 1), NextSet(state, input.freshCapture), states, OrFresh(state.hasOutput, input),
                       OrFresh(state.hasPrevious, input), ExceedsThreshold(input.unmatched, plan.resetThreshold), OrFresh(state.zeroMotionWritten, input),
                       NextCapture(state, input), NextDisplay(state.display, input.toggleOriginal, input.toggleSplit), state.slotFences,
                       infra::WithElement(state.statsPending, slot.Get(), EmitsStats(plan, input.freshCapture)), DisplaySourceOf(plan) };
}

[[nodiscard]] BuildResult FreshSteps(const SessionPlan& plan, const FrameState& state, const FrameInput& input, const LevelExtents& extents, FrameSlot slot) noexcept
{
    const bool reset = NeedsReset(state, input.now);
    return PhaseOne(Builder{ StepList{}, state.states }, plan, extents, state.currentSet)
        .and_then([&](const Builder& n) { return MotionPhaseTwo(n, plan, extents, state, slot); })
        .and_then([&](const Builder& n) { return SuperResolutionSteps(n, plan, reset); })
        .and_then([&](const Builder& n) { return NeuralRenderingSteps(n, plan, reset); })
        .and_then([&](const Builder& n) { return BlitSteps(n, true, DisplaySourceOf(plan), NextDisplay(state.display, input.toggleOriginal, input.toggleSplit), input.backBuffer); });
}

[[nodiscard]] BuildResult RepeatSteps(const SessionPlan& plan, const FrameState& state, const FrameInput& input) noexcept
{
    return BlitSteps(Builder{ StepList{}, state.states }, state.hasOutput, DisplaySourceOf(plan), NextDisplay(state.display, input.toggleOriginal, input.toggleSplit), input.backBuffer);
}

[[nodiscard]] BuildResult StepsFor(const SessionPlan& plan, const FrameState& state, const FrameInput& input, const LevelExtents& extents, FrameSlot slot) noexcept
{
    return input.freshCapture ? FreshSteps(plan, state, input, extents, slot) : RepeatSteps(plan, state, input);
}

} // namespace

ResourceId SimpleId(ResourceKind kind) noexcept
{
    return ResourceId{ kind, *kZeroSet, *kZeroLevel, *kZeroBuffer, *kZeroSlot };
}

ResourceId LumaId(SetIndex set, LevelIndex level) noexcept
{
    return ResourceId{ ResourceKind::Luma, set, level, *kZeroBuffer, *kZeroSlot };
}

ResourceId FlowId(LevelIndex level) noexcept
{
    return ResourceId{ ResourceKind::Flow, *kZeroSet, level, *kZeroBuffer, *kZeroSlot };
}

ResourceId BackBufferId(BackBufferIndex index) noexcept
{
    return ResourceId{ ResourceKind::BackBuffer, *kZeroSet, *kZeroLevel, index, *kZeroSlot };
}

ResourceId ReadbackId(FrameSlot slot) noexcept
{
    return ResourceId{ ResourceKind::StatsReadback, *kZeroSet, *kZeroLevel, *kZeroBuffer, slot };
}

std::size_t SlotOf(const ResourceId& id) noexcept
{
    switch (id.kind)
    {
    case ResourceKind::Canvas: return 0;
    case ResourceKind::ModelColor: return 1;
    case ResourceKind::Depth: return 2;
    case ResourceKind::MotionVectors: return 3;
    case ResourceKind::Stats: return 4;
    case ResourceKind::ZeroBuffer: return 5;
    case ResourceKind::SrOutput: return 6;
    case ResourceKind::NrOutput: return 7;
    case ResourceKind::OpticalFlowOutput: return 8;
    case ResourceKind::BackBuffer: return 9 + id.buffer.Get();
    case ResourceKind::StatsReadback: return 12 + id.slot.Get();
    case ResourceKind::Luma: return 14 + id.set.Get() * kMaxLevels + id.level.Get();
    case ResourceKind::Flow: return 30 + id.level.Get();
    }
    return 0;
}

ResourceState StateOf(const StateTable& table, const ResourceId& id) noexcept
{
    return table[SlotOf(id)];
}

struct InitialEntry
{
    std::size_t slot;
    ResourceState state;
};

constexpr std::array<InitialEntry, 10> kInitialEntries{ { { 0, ResourceState::CopyDest }, { 2, ResourceState::ShaderRead }, { 4, ResourceState::CopyDest },
                                                          { 5, ResourceState::GenericRead }, { 8, ResourceState::Common }, { 9, ResourceState::Present },
                                                          { 10, ResourceState::Present }, { 11, ResourceState::Present }, { 12, ResourceState::CopyDest },
                                                          { 13, ResourceState::CopyDest } } };

[[nodiscard]] StateTable WithEntry(const StateTable& table, const InitialEntry& entry) noexcept
{
    return infra::WithElement(table, entry.slot, entry.state);
}

StateTable InitialStates() noexcept
{
    return std::ranges::fold_left(kInitialEntries, infra::Filled<ResourceState, kSlotCount>(ResourceState::UnorderedAccess), WithEntry);
}

FrameState InitialFrameState(const SessionPlan& plan) noexcept
{
    return FrameState{ FrameNumberTag::Parse(0), *kZeroSet, InitialStates(), false, false, false, false, std::nullopt, plan.initialDisplay,
                       { FenceValueTag::Parse(0), FenceValueTag::Parse(0) }, { false, false }, DisplaySourceOf(plan) };
}

FrameSlot SlotOfFrame(FrameNumber number) noexcept
{
    const Result<FrameSlot, UnitError> slot = FrameSlotTag::Parse(static_cast<std::uint32_t>(number.Get() % kFrameSlotCount));
    ENSURE(slot.has_value());
    return *slot;
}

[[nodiscard]] DisplayMode Toggled(DisplayMode current, DisplayMode mode) noexcept
{
    return current == mode ? DisplayMode::Processed : mode;
}

[[nodiscard]] DisplayMode ToggledIf(DisplayMode current, DisplayMode mode, bool toggle) noexcept
{
    return toggle ? Toggled(current, mode) : current;
}

DisplayMode NextDisplay(DisplayMode current, bool toggleOriginal, bool toggleSplit) noexcept
{
    return ToggledIf(ToggledIf(current, DisplayMode::Original, toggleOriginal), DisplayMode::Split, toggleSplit);
}

bool IsLongPause(const FrameState& state, Instant now) noexcept
{
    return state.lastCapture.has_value() && now.Get() - state.lastCapture->Get() > kPauseResetMicroseconds;
}

bool ExceedsThreshold(std::optional<Fraction> unmatched, Fraction threshold) noexcept
{
    return unmatched.has_value() && unmatched->Get() > threshold.Get();
}

Result<FramePlan, PlanFrameError> PlanFrame(const SessionPlan& plan, const FrameState& state, const FrameInput& input) noexcept
{
    const FrameSlot slot = SlotOfFrame(state.number);
    return LevelExtentsOf(plan.source, plan.levels).transform_error(FromPyramid).and_then([&](const LevelExtents& extents)
    {
        return StepsFor(plan, state, input, extents, slot)
            .transform([&](const Builder& built) { return FramePlan{ built.steps, NextState(plan, state, input, built.states, slot), input.quit }; });
    });
}

std::string_view Describe(PlanFrameError error) noexcept
{
    switch (error)
    {
    case PlanFrameError::Capacity: return "frame plan exceeded its step capacity";
    case PlanFrameError::Arithmetic: return "arithmetic overflow while planning a frame";
    case PlanFrameError::Unit: return "invalid level or slot while planning a frame";
    }
    return "frame planning error";
}

} // namespace interior
