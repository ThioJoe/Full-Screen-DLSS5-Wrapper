#pragma once
#include "effects/real/console.h"
#include "effects/real/executor.h"
#include "effects/real/panel.h"
#include "effects/real/window.h"
#include "interior/monitors.h"

namespace real {

struct FrameStart
{
    interior::FrameInput input;
};

struct ExecutionReport
{
    interior::FenceValue signaled;
    std::uint32_t stepsExecuted;
};

struct Statistics
{
    interior::Instant lastReport;
    std::uint32_t processed;
    std::uint32_t presented;
};

struct Begun
{
    FrameContext frame;
    interior::FrameInput input;
};

struct EnvironmentSettings
{
    bool captureBorder;
};

class RealEnvironment final
{
public:
    RealEnvironment(Gpu gpu, const interior::SessionPlan& plan, OutputWindow window, std::optional<ControlPanel> panel, const Console& console, std::uint32_t finestPixels, interior::FenceValue fence,
                    interior::Instant start) noexcept;

    [[nodiscard]] infra::Result<FrameStart, Error> BeginFrame(const interior::FrameState& state) noexcept;
    [[nodiscard]] infra::Result<ExecutionReport, Error> Execute(const interior::FramePlan& plan) noexcept;
    // Rebuilds the neural rendering feature when the operator has changed its tuning; the model reads that
    // only while the feature is built. A no-op when nothing changed.
    [[nodiscard]] infra::Result<ExecutionReport, Error> Retuned(const interior::ModelControls& controls) noexcept;
    [[nodiscard]] static Error FromPlanError(interior::PlanFrameError error) noexcept;
    [[nodiscard]] const Gpu& Devices() const noexcept { return gpu_; }
    [[nodiscard]] const OutputWindow& Window() const noexcept { return window_; }
    [[nodiscard]] interior::FenceValue LastFence() const noexcept { return frame_.fence; }

private:
    [[nodiscard]] infra::Result<FrameStart, Error> Accept(const Begun& begun) noexcept;
    [[nodiscard]] infra::Result<ExecutionReport, Error> Ran(const interior::FramePlan& plan) noexcept;

    Gpu gpu_;
    interior::SessionPlan plan_;
    OutputWindow window_;
    std::optional<ControlPanel> panel_;
    Console console_;
    std::uint32_t finestPixels_;
    FrameContext frame_; // WAIVER(R2): per-frame bookkeeping of the effect layer, replaced whole by BeginFrame and Execute.
    Statistics stats_;   // WAIVER(R2): throughput counters, replaced whole once per frame.
};

[[nodiscard]] infra::Result<RealEnvironment, Error> CreateEnvironment(GpuDevice device, std::optional<NgxRuntime> runtime, const interior::SessionPlan& plan, const interior::Geometry& geometry,
                                                                      OutputWindow window, std::optional<ControlPanel> panel, const EnvironmentSettings& settings, const Console& console) noexcept;

} // namespace real
