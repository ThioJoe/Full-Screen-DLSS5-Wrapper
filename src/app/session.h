#pragma once
// WAIVER(R31): the session loop is the effect interface (R10), generic over the real and simulated environments.
// Both environments expose BeginFrame and Execute returning results; the loop never branches on which one it is.
#include "infrastructure/result.h"
#include "interior/frame.h"
#include "interior/plan.h"

#include <cstdint>

namespace app {

enum class StopReason : std::uint8_t { Quit, FrameLimit };

struct SessionOutcome
{
    StopReason reason;
    interior::FrameState finalState;
};

[[nodiscard]] inline interior::FrameState WithSlotFence(const interior::FrameState& state, interior::FrameSlot slot, interior::FenceValue value) noexcept
{
    return interior::FrameState{ state.number,       state.currentSet,        state.states,      state.hasOutput, state.hasPrevious,
                                 state.resetPending, state.zeroMotionWritten, state.lastCapture, state.display,   infra::WithElement(state.slotFences, slot.Get(), value),
                                 state.statsPending, state.displaySource,     state.split,       state.controls };
}

template <class Environment, class Error>
struct SessionStep
{
    [[nodiscard]] static infra::Result<interior::FramePlan, Error> Plan(Environment& env, const interior::SessionPlan& plan, const interior::FrameState& state) noexcept
    {
        return env.BeginFrame(state).and_then(
            [&](const auto& start) { return interior::PlanFrame(plan, state, start.input).transform_error([](interior::PlanFrameError e) { return Environment::FromPlanError(e); }); });
    }
};

// WAIVER(R1): the session loop is the one loop (R2 waiver below); its body is the bounded iteration itself.
template <class Environment, class Error>
[[nodiscard]] infra::Result<SessionOutcome, Error> RunSession(Environment& env, const interior::SessionPlan& plan, interior::FrameState state, std::uint64_t frameLimit) noexcept
{
    // WAIVER(R2): the presentation loop runs until the operator quits or frameLimit (R21) is reached; the frame
    // state record is the only mutable binding and is replaced whole each iteration.
    while (state.number.Get() < frameLimit)
    {
        const infra::Result<interior::FramePlan, Error> framePlan = SessionStep<Environment, Error>::Plan(env, plan, state);
        if (!framePlan.has_value())
            return infra::Fail(framePlan.error());
        if (framePlan->stop)
            return SessionOutcome{ StopReason::Quit, state };
        const infra::Result<interior::FrameState, Error> next =
            env.Execute(*framePlan).transform([&](const auto& report) { return WithSlotFence(framePlan->next, interior::SlotOfFrame(state.number), report.signaled); });
        if (!next.has_value())
            return infra::Fail(next.error());
        state = *next;
    }
    return SessionOutcome{ StopReason::FrameLimit, state };
}

} // namespace app
