#pragma once
#include "infrastructure/bounded_vector.h"
#include "interior/plan.h"
#include "interior/units.h"

#include <array>
#include <optional>
#include <variant>

namespace interior {

enum class ResourceKind : std::uint8_t
{
    Canvas, ModelColor, Depth, MotionVectors, Stats, ZeroBuffer, SrOutput, NrOutput, OpticalFlowOutput, BackBuffer, StatsReadback, Luma, Flow,
};

enum class ResourceState : std::uint8_t { CopyDest, CopySource, ShaderRead, UnorderedAccess, RenderTarget, Present, Common, GenericRead };

struct ResourceId
{
    ResourceKind kind;
    SetIndex set;
    LevelIndex level;
    BackBufferIndex buffer;
    FrameSlot slot;
    [[nodiscard]] friend constexpr bool operator==(const ResourceId&, const ResourceId&) noexcept = default;
};

constexpr std::size_t kSlotCount = 38;
using StateTable = std::array<ResourceState, kSlotCount>;

[[nodiscard]] ResourceId SimpleId(ResourceKind kind) noexcept;
[[nodiscard]] ResourceId LumaId(SetIndex set, LevelIndex level) noexcept;
[[nodiscard]] ResourceId FlowId(LevelIndex level) noexcept;
[[nodiscard]] ResourceId BackBufferId(BackBufferIndex index) noexcept;
[[nodiscard]] ResourceId ReadbackId(FrameSlot slot) noexcept;
[[nodiscard]] std::size_t SlotOf(const ResourceId& id) noexcept;
[[nodiscard]] ResourceState StateOf(const StateTable& table, const ResourceId& id) noexcept;

struct Transition
{
    ResourceId resource;
    ResourceState from;
    ResourceState to;
    [[nodiscard]] friend constexpr bool operator==(const Transition&, const Transition&) noexcept = default;
};

enum class PassId : std::uint8_t { Convert, Downsample, Match, Finalize, FlowToMv };

struct Binding
{
    std::array<std::optional<ResourceId>, 4> srv;
    std::array<std::optional<ResourceId>, 2> uav;
    [[nodiscard]] friend constexpr bool operator==(const Binding&, const Binding&) noexcept = default;
};

struct Constants
{
    std::array<std::uint32_t, 16> values;
    std::uint32_t count;
    [[nodiscard]] friend constexpr bool operator==(const Constants&, const Constants&) noexcept = default;
};

struct Dispatch
{
    PassId pass;
    Binding binding;
    Constants constants;
    ThreadGroups groups;
    [[nodiscard]] friend constexpr bool operator==(const Dispatch&, const Dispatch&) noexcept = default;
};

struct CopyBuffer
{
    ResourceId source;
    ResourceId destination;
    ByteCount bytes;
    [[nodiscard]] friend constexpr bool operator==(const CopyBuffer&, const CopyBuffer&) noexcept = default;
};

struct ClearTarget
{
    ResourceId target;
    [[nodiscard]] friend constexpr bool operator==(const ClearTarget&, const ClearTarget&) noexcept = default;
};

struct ModelIo
{
    ResourceId color;
    ResourceId depth;
    ResourceId motionVectors;
    ResourceId output;
    [[nodiscard]] friend constexpr bool operator==(const ModelIo&, const ModelIo&) noexcept = default;
};

struct EvaluateSr
{
    ModelIo io;
    Extent render;
    bool reset;
    [[nodiscard]] friend constexpr bool operator==(const EvaluateSr&, const EvaluateSr&) noexcept = default;
};

struct EvaluateNr
{
    ModelIo io;
    Extent work;
    Extent guide;
    Scale mvScaleX;
    Scale mvScaleY;
    bool reset;
    [[nodiscard]] friend constexpr bool operator==(const EvaluateNr&, const EvaluateNr&) noexcept = default;
};

struct Draw
{
    ResourceId processed;
    ResourceId original;
    ResourceId target;
    DisplayMode mode;
    [[nodiscard]] friend constexpr bool operator==(const Draw&, const Draw&) noexcept = default;
};

enum class Phase : std::uint8_t { One, Two };

struct Submit
{
    Phase phase;
    [[nodiscard]] friend constexpr bool operator==(const Submit&, const Submit&) noexcept = default;
};

struct Present
{
    [[nodiscard]] friend constexpr bool operator==(const Present&, const Present&) noexcept = default;
};

using Step = std::variant<Transition, Dispatch, CopyBuffer, ClearTarget, EvaluateSr, EvaluateNr, Draw, Submit, Present>;

constexpr std::size_t kMaxSteps = 192;
using StepList = infra::BoundedVector<Step, kMaxSteps>;

struct FrameState
{
    FrameNumber number;
    SetIndex currentSet;
    StateTable states;
    bool hasOutput;
    bool hasPrevious;
    bool resetPending;
    bool zeroMotionWritten;
    std::optional<Instant> lastCapture;
    DisplayMode display;
    std::array<FenceValue, kFrameSlotCount> slotFences;
    std::array<bool, kFrameSlotCount> statsPending;
    ResourceKind displaySource;
    [[nodiscard]] friend constexpr bool operator==(const FrameState&, const FrameState&) noexcept = default;
};

struct FrameInput
{
    bool freshCapture;
    BackBufferIndex backBuffer;
    std::optional<Fraction> unmatched;
    Instant now;
    bool toggleOriginal;
    bool toggleSplit;
    bool quit;
    [[nodiscard]] friend constexpr bool operator==(const FrameInput&, const FrameInput&) noexcept = default;
};

struct FramePlan
{
    StepList steps;
    FrameState next;
    bool stop;
    [[nodiscard]] friend constexpr bool operator==(const FramePlan&, const FramePlan&) noexcept = default;
};

enum class PlanFrameError : std::uint8_t { Capacity, Arithmetic, Unit };

constexpr std::uint32_t kStatsBytes = 256;
constexpr std::uint64_t kPauseResetMicroseconds = 1000000;

[[nodiscard]] StateTable InitialStates() noexcept;
[[nodiscard]] FrameState InitialFrameState(const SessionPlan& plan) noexcept;
[[nodiscard]] FrameSlot SlotOfFrame(FrameNumber number) noexcept;
[[nodiscard]] Result<FramePlan, PlanFrameError> PlanFrame(const SessionPlan& plan, const FrameState& state, const FrameInput& input) noexcept;
[[nodiscard]] DisplayMode NextDisplay(DisplayMode current, bool toggleOriginal, bool toggleSplit) noexcept;
[[nodiscard]] bool IsLongPause(const FrameState& state, Instant now) noexcept;
[[nodiscard]] bool ExceedsThreshold(std::optional<Fraction> unmatched, Fraction threshold) noexcept;
[[nodiscard]] std::string_view Describe(PlanFrameError error) noexcept;

} // namespace interior
