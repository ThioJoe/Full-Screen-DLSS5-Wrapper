// WAIVER(R2): test suites accumulate failure counts and drive generated sequences with loops.
#include "app/session.h"
#include "effects/sim/sim.h"
#include "interior/plan.h"
#include "interior/pyramid.h"
#include "tests/test_registry.h"

namespace tests {
namespace {

using namespace interior;

[[nodiscard]] SessionPlan PlanFor(infra::RngState& rng) noexcept
{
    const auto w = PixelCountTag::Parse(proptest::DrawBetween(rng, 64, 2048));
    const auto h = PixelCountTag::Parse(proptest::DrawBetween(rng, 64, 1024));
    REQUIRE(w.has_value() && h.has_value());
    const Extent source{ *w, *h };
    const Options d = DefaultOptions();
    const LevelCount levels = LevelCountFor(source);
    const auto scale = ScaleTag::Parse(1.0f);
    const auto flow = GridExtent(source, 1);
    REQUIRE(scale.has_value() && flow.has_value());
    const std::array<MotionBackend, 3> backends{ MotionBackend::BuiltIn, MotionBackend::NvOpticalFlow, MotionBackend::None };
    return SessionPlan{ source, source, source, std::nullopt, proptest::DrawBool(rng), d.tuning, backends[proptest::DrawBelow(rng, 3)], levels, d.motionFinestLevel,
                        GridSize::One, PerfLevel::Medium, *flow, *scale, *scale, d.depthValue, d.resetThreshold, ColorFormat::Rgba8, DisplayMode::Processed, false, true };
}

[[nodiscard]] bool SessionEndsCleanlyUnderInjectedFailures(infra::RngState& rng) noexcept
{
    const std::uint64_t seed = proptest::Draw(rng);
    const SessionPlan plan = PlanFor(rng);
    const sim::FailureRates rates{ proptest::DrawBelow(rng, 20000), proptest::DrawBelow(rng, 20000), proptest::DrawBelow(rng, 20000), proptest::DrawBelow(rng, 20000) };
    sim::SimEnvironment env(seed, plan, rates, proptest::DrawBetween(rng, 1, 200));
    const auto outcome = app::RunSession<sim::SimEnvironment, sim::SimError>(env, plan, InitialFrameState(plan), 500);
    if (outcome.has_value())
        return outcome->reason == app::StopReason::Quit || outcome->finalState.number.Get() == 500;
    const sim::SimError e = outcome.error();
    return e == sim::SimError::DeviceRemoved || e == sim::SimError::CaptureLost || e == sim::SimError::FenceTimeout || e == sim::SimError::NgxSuperResolutionFailed ||
           e == sim::SimError::NgxNeuralRenderingFailed;
}

[[nodiscard]] bool WithoutFailuresEveryFrameIsPresented(infra::RngState& rng) noexcept
{
    const std::uint64_t seed = proptest::Draw(rng);
    const SessionPlan plan = PlanFor(rng);
    const std::uint32_t frames = proptest::DrawBetween(rng, 1, 100);
    sim::SimEnvironment env(seed, plan, sim::FailureRates{ 0, 0, 0, 0 }, frames);
    const auto outcome = app::RunSession<sim::SimEnvironment, sim::SimError>(env, plan, InitialFrameState(plan), 1000);
    return outcome.has_value() && outcome->reason == app::StopReason::Quit && env.World().presented == frames;
}

[[nodiscard]] bool SimulationIsReproducibleFromSeed(infra::RngState& rng) noexcept
{
    const std::uint64_t seed = proptest::Draw(rng);
    const SessionPlan plan = PlanFor(rng);
    const sim::FailureRates rates{ 5000, 5000, 5000, 5000 };
    sim::SimEnvironment a(seed, plan, rates, 60);
    sim::SimEnvironment b(seed, plan, rates, 60);
    const auto first = app::RunSession<sim::SimEnvironment, sim::SimError>(a, plan, InitialFrameState(plan), 200);
    const auto second = app::RunSession<sim::SimEnvironment, sim::SimError>(b, plan, InitialFrameState(plan), 200);
    return first.has_value() == second.has_value() && a.World().presented == b.World().presented && a.World().evaluated == b.World().evaluated;
}

} // namespace

std::uint32_t SimulationSuite(std::uint64_t seed) noexcept
{
    std::uint32_t failures = 0;
    failures += Failures(proptest::ForAll("sessions end cleanly under injected failures", seed, 400, SessionEndsCleanlyUnderInjectedFailures));
    failures += Failures(proptest::ForAll("without failures every frame is presented", seed, 300, WithoutFailuresEveryFrameIsPresented));
    failures += Failures(proptest::ForAll("simulation is reproducible from its seed", seed, 200, SimulationIsReproducibleFromSeed));
    return failures;
}

} // namespace tests
