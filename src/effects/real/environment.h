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
    bool ownContent;                                 // the overlay shows its own content rather than a composition over it
};

class RealEnvironment final
{
public:
    RealEnvironment(Gpu gpu, const interior::SessionPlan& plan, OutputWindow window, const ControlPanel* panel, const Console& console, const EnvironmentSettings& settings,
                    const interior::Options& options, std::uint32_t finestPixels, interior::FenceValue fence, interior::Instant start) noexcept;

    [[nodiscard]] infra::Result<FrameStart, Error> BeginFrame(const interior::FrameState& state) noexcept;
    [[nodiscard]] infra::Result<ExecutionReport, Error> Execute(const interior::FramePlan& plan) noexcept;
    // Rebuilds the neural rendering feature when the operator has changed its tuning; the model reads that
    // only while the feature is built. A no-op when nothing changed.
    [[nodiscard]] infra::Result<ExecutionReport, Error> Retuned(const interior::LiveSettings& controls) noexcept;
    [[nodiscard]] static Error FromPlanError(interior::PlanFrameError error) noexcept;
    [[nodiscard]] const Gpu& Devices() const noexcept { return gpu_; }
    [[nodiscard]] const OutputWindow& Window() const noexcept { return window_; }
    [[nodiscard]] interior::FenceValue LastFence() const noexcept { return frame_.fence; }
    // The session the panel now describes, once it has settled on it, or nothing when nothing changed.
    [[nodiscard]] std::optional<interior::CommandLine> Restart(const interior::Options& options) const noexcept;
    // Whether the window being worked on changed size, which the sizes of a built session cannot follow.
    [[nodiscard]] bool Resized() const noexcept { return resized_; }
    // Whether the window being worked on was closed, minimised or hidden, which leaves nothing to capture.
    [[nodiscard]] bool Abandoned() const noexcept { return abandoned_; }

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
    void Followed(interior::Instant now) noexcept;
    void Watched(interior::MonitorHandle window, interior::Instant now) noexcept;
    void Abandon() noexcept;
    [[nodiscard]] bool AsksForSettings() const noexcept;
    [[nodiscard]] bool AsksToEnd() const noexcept;
    void Moved(const interior::ScreenRect& bounds, interior::Instant now) noexcept;
    void Placed(const interior::ScreenRect& bounds) noexcept;
    void FollowedTo(const std::optional<interior::ScreenRect>& bounds, interior::Instant now) noexcept;
    void Settling(const interior::Extent& size, interior::Instant now) noexcept;
    void Reconsidered(interior::Instant now) noexcept;
    void Considering(const interior::CommandLine& shape, interior::Instant now) noexcept;
    void Asked(const interior::CommandLine& shape, interior::Instant now) noexcept;
    [[nodiscard]] bool AsksForAnother(const interior::CommandLine& shape, interior::Instant now) const noexcept;
    void HeldSettings(const interior::CommandLine& shape, interior::Instant now) noexcept;
    void Noticed(const interior::Extent& size, interior::Instant now) noexcept;
    [[nodiscard]] bool HasSettled(interior::Instant now) const noexcept;
    [[nodiscard]] bool AsksForARebuild(const interior::Extent& size, interior::Instant now) const noexcept;
    void Held(const interior::Extent& size, interior::Instant now) noexcept;

    Gpu gpu_;
    interior::SessionPlan plan_;
    OutputWindow window_;
    const ControlPanel* panel_; // borrowed: the panel outlives the session, so a rebuilt one keeps its place
    Console console_;
    std::uint32_t finestPixels_;
    FrameContext frame_;                         // WAIVER(R2): per-frame bookkeeping of the effect layer, replaced whole by BeginFrame and Execute.
    Statistics stats_;                           // WAIVER(R2): throughput counters, replaced whole once per frame.
    EnvironmentSettings applied_;                // WAIVER(R2): what the window and the capture were last told, replaced whole on a change.
    interior::DepthValue clearedDepth_;          // WAIVER(R2): the value the depth plane was last cleared to.
    bool restartWanted_;                         // WAIVER(R2): set once, when the operator asks the panel for a new session.
    bool resized_;                               // WAIVER(R2): set once, when the window being followed has settled at another size.
    interior::Extent pending_;                   // WAIVER(R2): the size the window was last seen at, replaced whole as it changes.
    interior::Instant since_;                    // WAIVER(R2): when it was first seen at that size.
    interior::Options options_;                  // what this session was built from, which the panel is compared against
    interior::CommandLine built_;                // the shape it was built with
    interior::CommandLine wanted_;               // WAIVER(R2): the shape the panel now describes, replaced whole as it changes.
    interior::Instant asked_;                    // WAIVER(R2): when it first described it.
    bool abandoned_;                             // WAIVER(R2): set once, when the window being followed stopped being on screen.
    std::optional<interior::ScreenRect> placed_; // WAIVER(R2): where the overlay was last put, replaced whole as the window moves.
};

[[nodiscard]] infra::Result<RealEnvironment, Error> CreateEnvironment(GpuDevice device, std::optional<NgxRuntime> runtime, const interior::SessionPlan& plan, const interior::Geometry& geometry,
                                                                      OutputWindow window, const ControlPanel* panel, const EnvironmentSettings& settings, const interior::Options& options,
                                                                      const Console& console) noexcept;

} // namespace real
