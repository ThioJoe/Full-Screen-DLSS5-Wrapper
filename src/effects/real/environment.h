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
    std::optional<PanelReading> reading;
};

struct EnvironmentSettings
{
    interior::SurfaceSettings surface;
    bool captureCursor;                              // what the session resolved for "auto", which the panel's Auto keeps
    std::optional<interior::MonitorHandle> followed; // the window the source is, when the source is one window
};

class RealEnvironment final
{
public:
    RealEnvironment(Gpu gpu, const interior::SessionPlan& plan, OutputWindow window, const ControlPanel* panel, const Console& console, const EnvironmentSettings& settings, std::uint32_t finestPixels,
                    interior::FenceValue fence, interior::Instant start) noexcept;

    [[nodiscard]] infra::Result<FrameStart, Error> BeginFrame(const interior::FrameState& state) noexcept;
    [[nodiscard]] infra::Result<ExecutionReport, Error> Execute(const interior::FramePlan& plan) noexcept;
    // Rebuilds the neural rendering feature when the operator has changed its tuning; the model reads that
    // only while the feature is built. A no-op when nothing changed.
    [[nodiscard]] infra::Result<ExecutionReport, Error> Retuned(const interior::LiveSettings& controls) noexcept;
    [[nodiscard]] static Error FromPlanError(interior::PlanFrameError error) noexcept;
    [[nodiscard]] const Gpu& Devices() const noexcept { return gpu_; }
    [[nodiscard]] const OutputWindow& Window() const noexcept { return window_; }
    [[nodiscard]] interior::FenceValue LastFence() const noexcept { return frame_.fence; }
    // The session the operator asked the panel for, or nothing when they simply left.
    [[nodiscard]] std::optional<interior::CommandLine> Restart(const interior::Options& options) const noexcept;

private:
    [[nodiscard]] infra::Result<FrameStart, Error> Accept(const Begun& begun) noexcept;
    [[nodiscard]] infra::Result<ExecutionReport, Error> Ran(const interior::FramePlan& plan) noexcept;
    [[nodiscard]] infra::Result<FrameStart, Error> Began(const Begun& begun) noexcept;
    [[nodiscard]] infra::Status<Error> Resurfaced(const interior::SurfaceSettings& surface) noexcept;
    [[nodiscard]] infra::Status<Error> SettledIfRead(const std::optional<PanelReading>& reading) noexcept;
    [[nodiscard]] infra::Status<Error> Settled(const PanelReading& reading) noexcept;
    [[nodiscard]] infra::Status<Error> Recleared(interior::DepthValue depth) noexcept;
    // Keeps the overlay over the window the session is working on. The capture follows the window itself,
    // so only where the answer is shown has to be put right.
    void Followed() noexcept;

    Gpu gpu_;
    interior::SessionPlan plan_;
    OutputWindow window_;
    const ControlPanel* panel_; // borrowed: the panel outlives the session, so a rebuilt one keeps its place
    Console console_;
    std::uint32_t finestPixels_;
    FrameContext frame_;                // WAIVER(R2): per-frame bookkeeping of the effect layer, replaced whole by BeginFrame and Execute.
    Statistics stats_;                  // WAIVER(R2): throughput counters, replaced whole once per frame.
    EnvironmentSettings applied_;       // WAIVER(R2): what the window and the capture were last told, replaced whole on a change.
    interior::DepthValue clearedDepth_; // WAIVER(R2): the value the depth plane was last cleared to.
    bool restartWanted_;                // WAIVER(R2): set once, when the operator asks the panel for a new session.
};

[[nodiscard]] infra::Result<RealEnvironment, Error> CreateEnvironment(GpuDevice device, std::optional<NgxRuntime> runtime, const interior::SessionPlan& plan, const interior::Geometry& geometry,
                                                                      OutputWindow window, const ControlPanel* panel, const EnvironmentSettings& settings, const Console& console) noexcept;

} // namespace real
