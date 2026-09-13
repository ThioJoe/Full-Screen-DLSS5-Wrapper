#pragma once
#include "effects/real/comparison.h"
#include "effects/real/console.h"
#include "effects/real/executor.h"
#include "effects/real/panel.h"
#include "effects/real/recorder.h"
#include "effects/real/snapshot.h"
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
    bool asksToBeLeftOut;                            // the capture is asked to leave our own windows out of it by name
    bool outsideTheSource;                           // the overlay covers something other than what is being captured
    interior::ScreenRect source;                     // where what is captured is on the screen, which is where a picture's top-left pixel is
};

class RealEnvironment final
{
public:
    RealEnvironment(HeldFiles held, Gpu gpu, const interior::SessionPlan& plan, OutputWindow window, const ControlPanel* panel, const Console& console, const EnvironmentSettings& settings,
                    const interior::Options& options, std::uint32_t finestPixels, interior::FenceValue fence, interior::Instant start) noexcept;

    [[nodiscard]] infra::Result<FrameStart, Error> BeginFrame(const interior::FrameState& state) noexcept;
    [[nodiscard]] infra::Result<ExecutionReport, Error> Execute(const interior::FramePlan& plan) noexcept;
    // Rebuilds the neural rendering feature when the operator has changed its tuning; the model reads that
    // only while the feature is built. A no-op when nothing changed.
    [[nodiscard]] infra::Result<ExecutionReport, Error> Retuned(const interior::LiveSettings& controls) noexcept;
    // Ends a recording under way, once the session's last frame is done: a file left open would not play.
    [[nodiscard]] infra::Status<Error> Finished() noexcept;
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
    // Takes the screenshot the panel asked for, once the frame it belongs to has been submitted.
    [[nodiscard]] infra::Result<ExecutionReport, Error> Captured(const interior::FramePlan& plan, const ExecutionReport& report) noexcept;
    // Records the frame's two pictures for a recording under way, once the frame has been submitted.
    [[nodiscard]] infra::Result<ExecutionReport, Error> RecordedFrame(const interior::FramePlan& plan, const ExecutionReport& report) noexcept;
    // Encodes the frames whose copies have landed, which is what the slot about to be reused holds.
    [[nodiscard]] infra::Status<Error> DrainedRecording(interior::FrameSlot slot) noexcept;
    [[nodiscard]] infra::Status<Error> ToggledRecording(const PanelReading& reading, const SnapshotOrder& order) noexcept;
    // Saves the comparison capture's picture of the frame just submitted, and ends it after the last.
    [[nodiscard]] infra::Result<ExecutionReport, Error> ComparedFrame(const interior::FramePlan& plan, const ExecutionReport& report) noexcept;
    [[nodiscard]] infra::Status<Error> ToggledComparison(const PanelReading& reading, const SnapshotOrder& order) noexcept;
    [[nodiscard]] infra::Status<Error> FinishedComparison() noexcept;
    [[nodiscard]] infra::Status<Error> EndedComparison(std::string_view what) noexcept;
    void ShowComparison(const PanelReading& reading) noexcept;
    [[nodiscard]] infra::Status<Error> StoppedRecording() noexcept;
    [[nodiscard]] infra::Status<Error> ShowRecording() noexcept;
    [[nodiscard]] infra::Result<FrameStart, Error> Began(const Begun& begun) noexcept;
    [[nodiscard]] infra::Status<Error> Resurfaced(const interior::SurfaceSettings& surface) noexcept;
    [[nodiscard]] infra::Status<Error> SettledIfRead(const std::optional<PanelReading>& reading) noexcept;
    [[nodiscard]] infra::Status<Error> Settled(const PanelReading& reading) noexcept;
    [[nodiscard]] infra::Status<Error> Recleared(interior::DepthValue depth) noexcept;
    // Keeps the overlay over the window the session is working on. The capture follows the window itself,
    // so only where the answer is shown has to be put right.
    [[nodiscard]] infra::Status<Error> Followed(interior::Instant now) noexcept;
    void Fronted() noexcept;
    void Behind(HWND front) noexcept;
    [[nodiscard]] infra::Status<Error> Watched(interior::MonitorHandle window, interior::Instant now) noexcept;
    // While the window being followed is minimised or hidden the overlay is out of sight and the session
    // waits; when it shows again the overlay comes back. Each says so in the log once.
    [[nodiscard]] infra::Status<Error> Waiting() noexcept;
    [[nodiscard]] infra::Status<Error> Resumed() noexcept;
    // The panel's button restores the window and puts it on top without giving it the focus.
    [[nodiscard]] infra::Status<Error> BroughtBack(const PanelReading& reading) noexcept;
    void ShowFollowing() noexcept;
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
    [[nodiscard]] bool AsksForAnotherWindow() const noexcept;
    [[nodiscard]] bool HasWaited(interior::Instant now) const noexcept;
    [[nodiscard]] bool IsWorthBuilding(interior::Instant now) const noexcept;
    void HeldSettings(const interior::CommandLine& shape, interior::Instant now) noexcept;
    void Noticed(const interior::Extent& size, interior::Instant now) noexcept;
    [[nodiscard]] bool HasSettled(interior::Instant now) const noexcept;
    [[nodiscard]] bool AsksForARebuild(const interior::Extent& size, interior::Instant now) const noexcept;
    void Held(const interior::Extent& size, interior::Instant now) noexcept;

    HeldFiles held_; // the checked model and runtime files, open for as long as the session runs; first, so they are let go last, after the runtime that may have loaded them
    Gpu gpu_;
    interior::SessionPlan plan_;
    OutputWindow window_;
    const ControlPanel* panel_; // borrowed: the panel outlives the session, so a rebuilt one keeps its place
    Console console_;
    std::uint32_t finestPixels_;
    FrameContext frame_;                      // WAIVER(R2): per-frame bookkeeping of the effect layer, replaced whole by BeginFrame and Execute.
    Statistics stats_;                        // WAIVER(R2): throughput counters, replaced whole once per frame.
    EnvironmentSettings applied_;             // WAIVER(R2): what the window and the capture were last told, replaced whole on a change.
    interior::DepthValue clearedDepth_;       // WAIVER(R2): the value the depth plane was last cleared to.
    bool restartWanted_;                      // WAIVER(R2): set once, when the operator asks the panel for a new session.
    bool resized_;                            // WAIVER(R2): set once, when the window being followed has settled at another size.
    interior::Extent pending_;                // WAIVER(R2): the size the window was last seen at, replaced whole as it changes.
    interior::Instant since_;                 // WAIVER(R2): when it was first seen at that size.
    interior::Options options_;               // what this session was built from, which the panel is compared against
    interior::CommandLine built_;             // the shape it was built with
    interior::CommandLine wanted_;            // WAIVER(R2): the shape the panel now describes, replaced whole as it changes.
    interior::Instant asked_;                 // WAIVER(R2): when it first described it.
    bool abandoned_;                          // WAIVER(R2): set once, when the window being followed was closed.
    bool waiting_;                            // WAIVER(R2): set while the window being followed is minimised or hidden, cleared when it is back.
    std::optional<SnapshotOrder> snapshot_;   // WAIVER(R2): the screenshot the panel asked for this frame, taken after the frame and cleared then.
    std::optional<VideoRecording> recording_; // WAIVER(R2): the recording under way, replaced whole as frames are added and cleared when it stops.
    std::optional<Comparison> comparison_;    // WAIVER(R2): the comparison capture under way, replaced whole as pictures are saved and cleared when it ends.
    std::optional<CursorOverlay> cursor_;     // WAIVER(R2): the cursor to draw into this frame's captures, replaced whole each frame.
    std::unique_ptr<PngWriter> writer_;       // writes the captures' files on a thread of its own; held by pointer so the environment can be moved
    interior::Instant now_;                   // WAIVER(R2): this frame's clock reading, replaced whole per frame.
};

[[nodiscard]] infra::Result<RealEnvironment, Error> CreateEnvironment(HeldFiles held, GpuDevice device, std::optional<NgxRuntime> runtime, const interior::SessionPlan& plan,
                                                                      const interior::Geometry& geometry, OutputWindow window, const ControlPanel* panel, const EnvironmentSettings& settings,
                                                                      const interior::Options& options, const Console& console) noexcept;

} // namespace real
