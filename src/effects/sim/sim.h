#pragma once
// Seeded simulator of the effect layer: display, capture, GPU and NGX, with failure injection.
// The session loop (app/session.h) is generic over the environment, so it runs unchanged here.
#include "infrastructure/bounded_vector.h"
#include "infrastructure/rng.h"
#include "interior/frame.h"
#include "interior/plan.h"

#include <cstdint>

namespace sim {

enum class SimError : std::uint8_t
{
    DeviceRemoved,
    CaptureLost,
    FenceTimeout,
    NgxSuperResolutionFailed,
    NgxNeuralRenderingFailed,
    IllegalTransition,
    ResourceNotReadable,
    ResourceNotWritable,
    OpticalFlowFailed,
    PresentFailed,
};

struct FailureRates
{
    std::uint32_t deviceRemovedPerMillion;
    std::uint32_t ngxFailurePerMillion;
    std::uint32_t captureLostPerMillion;
    std::uint32_t fenceTimeoutPerMillion;
};

struct FrameStart
{
    interior::FrameInput input;
};

struct ExecutionReport
{
    interior::FenceValue signaled;
    std::uint32_t stepsExecuted;
};

struct SimWorld
{
    infra::RngState rng;
    interior::StateTable states;
    std::uint64_t clockMicroseconds;
    std::uint64_t fence;
    std::uint32_t backBuffer;
    std::uint32_t framesUntilQuit;
    std::array<bool, interior::kFrameSlotCount> readbackWritten;
    std::uint32_t presented;
    std::uint32_t evaluated;
};

class SimEnvironment
{
public:
    SimEnvironment(std::uint64_t seed, const interior::SessionPlan& plan, FailureRates rates, std::uint32_t framesUntilQuit) noexcept;

    [[nodiscard]] infra::Result<FrameStart, SimError> BeginFrame(const interior::FrameState& state) noexcept;
    [[nodiscard]] infra::Result<ExecutionReport, SimError> Execute(const interior::FramePlan& plan) noexcept;
    [[nodiscard]] const SimWorld& World() const noexcept { return world_; }
    [[nodiscard]] static SimError FromPlanError(interior::PlanFrameError) noexcept { return SimError::IllegalTransition; }

private:
    interior::SessionPlan plan_;
    FailureRates rates_;
    SimWorld world_; // WAIVER(R2): the simulated world is the effect layer's state and is replaced whole per call.
};

[[nodiscard]] const char* Describe(SimError error) noexcept;

} // namespace sim
