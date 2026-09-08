// WAIVER(R31): the composition root wires the real effect layer to the pure core; nothing else lives here.
#include "app/session.h"
#include "effects/real/capture.h"
#include "effects/real/console.h"
#include "effects/real/device.h"
#include "effects/real/environment.h"
#include "effects/real/ngx.h"
#include "effects/real/panel.h"
#include "effects/real/window.h"
#include "infrastructure/array_util.h"
#include "infrastructure/fold.h"
#include "infrastructure/text.h"
#include "interior/driver.h"
#include "interior/monitors.h"
#include "interior/options.h"
#include "interior/plan.h"

#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <memory>
#include <ranges>
#include <span>
#include <string_view>

namespace {

using infra::Fail;
using infra::Result;
using infra::Status;
using interior::Geometry;
using interior::LogLevel;
using interior::Options;
using interior::SessionPlan;
using real::Console;
using real::Error;
using real::Log;

// R21: the presentation loop is bounded; 2^40 frames is centuries at any refresh rate.
constexpr std::uint64_t kFrameLimit = std::uint64_t{ 1 } << 40;
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitFailure = 3;
constexpr std::size_t kLineCapacity = 240;
constexpr bool kHasOpticalFlow = DSCREEN_HAVE_NVOF != 0;

using Line = infra::BoundedString<char, kLineCapacity>;

struct LocalFreer
{
    void operator()(wchar_t** block) const noexcept { ENSURE(::LocalFree(block) == nullptr); }
};
using ArgumentBlock = std::unique_ptr<wchar_t*, LocalFreer>;

struct Arguments
{
    ArgumentBlock block;
    std::array<std::wstring_view, interior::kMaxArguments> views;
    std::size_t count;
};

[[nodiscard]] std::wstring_view ArgumentAt(wchar_t** argv, std::size_t count, std::size_t index) noexcept
{
    if (index >= count)
        return std::wstring_view{};
    return std::wstring_view(argv[index + 1]);
}

[[nodiscard]] Result<Arguments, Error> Collected(ArgumentBlock block, int argc) noexcept
{
    const std::size_t count = static_cast<std::size_t>(std::max(argc - 1, 0));
    if (count > interior::kMaxArguments)
        return Fail(Error{ real::ApiCall::ArgumentCount, static_cast<std::uint32_t>(count) });
    wchar_t** argv = block.get();
    return Arguments{ std::move(block), infra::Generated<std::wstring_view, interior::kMaxArguments>([argv, count](std::size_t i) { return ArgumentAt(argv, count, i); }), count };
}

[[nodiscard]] Result<Arguments, Error> ReadArguments() noexcept
{
    int argc = 0;
    wchar_t** argv = ::CommandLineToArgvW(::GetCommandLineW(), &argc);
    if (argv == nullptr)
        return Fail(real::LastError(real::ApiCall::CommandLineToArgvW));
    return Collected(ArgumentBlock(argv), argc);
}

[[nodiscard]] int ReportEarly(const Error& error) noexcept
{
    const Line line = infra::Formatted<kLineCapacity>("DlssScreen: {}\n", real::Describe(error).Get());
    real::ShowMessage(line.Get());
    return real::WriteText(stderr, line.Get()).transform([] { return kExitFailure; }).value_or(kExitFailure);
}

[[nodiscard]] int ExitCodeOf(const Result<int, Error>& result) noexcept
{
    if (!result.has_value())
        return ReportEarly(result.error());
    return *result;
}

[[nodiscard]] int UsageFailure(const interior::OptionsError& error) noexcept
{
    const Line line = infra::Formatted<kLineCapacity>("argument {}: {}\n", error.argument.Get(), interior::Describe(error.kind));
    return real::WriteText(stderr, interior::UsageText()).and_then([&line] { return real::WriteText(stderr, line.Get()); }).transform([] { return kExitUsage; }).value_or(kExitUsage);
}

[[nodiscard]] int PrintUsage() noexcept
{
    return real::WriteText(stdout, interior::UsageText()).transform([] { return kExitOk; }).value_or(kExitFailure);
}

[[nodiscard]] Status<Error> PrintMonitor(std::size_t index, const interior::MonitorInfo& m) noexcept
{
    const std::array<char, interior::DeviceName::Capacity + 1> name = infra::NarrowedChars<interior::DeviceName::Capacity + 1>(m.name.Get());
    const Line line = infra::Formatted<kLineCapacity>("{}: {} {}x{} at ({}, {}){}\n", index, name.data(), m.rect.Right().Get() - m.rect.Left().Get(), m.rect.Bottom().Get() - m.rect.Top().Get(),
                                                      m.rect.Left().Get(), m.rect.Top().Get(), m.primary ? " primary" : "");
    return real::WriteText(stdout, line.Get());
}

[[nodiscard]] Status<Error> PrintMonitors(const interior::MonitorList& monitors) noexcept
{
    const interior::MonitorList ordered = interior::Ordered(monitors);
    return infra::ForEach(std::views::iota(std::size_t{ 0 }, ordered.Size()), Status<Error>{}, [&ordered](std::size_t i) { return PrintMonitor(i, ordered.At(i)); });
}

[[nodiscard]] Result<int, Error> ListMonitors() noexcept
{
    return real::SetDpiAwareness().and_then(real::EnumerateMonitors).and_then(PrintMonitors).transform([] { return kExitOk; });
}

// --- start-up stages -----------------------------------------------------------------------------------------

[[nodiscard]] Error FromMonitorError(interior::MonitorError error) noexcept
{
    return Error{ real::ApiCall::ResolveGeometry, static_cast<std::uint32_t>(error) };
}

[[nodiscard]] Error FromPlanError(interior::PlanError error) noexcept
{
    return Error{ real::ApiCall::PlanSession, static_cast<std::uint32_t>(error) };
}

struct Explanation
{
    std::string_view text;
    Error error;
};

[[nodiscard]] Explanation ExplainMonitor(interior::MonitorError error) noexcept
{
    return Explanation{ interior::Describe(error), FromMonitorError(error) };
}

[[nodiscard]] Explanation ExplainPlan(interior::PlanError error) noexcept
{
    return Explanation{ interior::Describe(error), FromPlanError(error) };
}

[[nodiscard]] Error Logged(const Console& console, const Explanation& explanation) noexcept
{
    return Log(console, LogLevel::Error, explanation.text).error_or(explanation.error);
}

[[nodiscard]] bool IsPathFailure(DWORD length) noexcept
{
    return length == 0 || length >= MAX_PATH;
}

[[nodiscard]] Result<interior::DirectoryPath, Error> DirectoryOf(std::wstring_view path) noexcept
{
    const std::size_t slash = path.find_last_of(L"\\/");
    if (slash == std::wstring_view::npos)
        return Fail(Error{ real::ApiCall::ExecutableDirectory, 0 });
    return interior::DirectoryPath::Parse(path.substr(0, slash)).transform_error([](infra::StringTooLong) { return Error{ real::ApiCall::ExecutableDirectory, 1 }; });
}

[[nodiscard]] Result<interior::DirectoryPath, Error> ExecutableDirectory() noexcept
{
    std::array<wchar_t, MAX_PATH> buffer{};
    const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), MAX_PATH);
    if (IsPathFailure(length))
        return Fail(real::LastError(real::ApiCall::GetModuleFileNameW));
    return DirectoryOf(std::wstring_view(buffer.data(), length));
}

struct Base
{
    Options options;
    interior::DirectoryPath executableDirectory;
    Geometry geometry;
};

[[nodiscard]] Result<Geometry, Error> ResolvedGeometry(const Console& console, const Options& options) noexcept
{
    return real::EnumerateMonitors().and_then([&](const interior::MonitorList& monitors) {
        return interior::ResolveGeometry(monitors, options).transform_error([&console](interior::MonitorError e) { return Logged(console, ExplainMonitor(e)); });
    });
}

[[nodiscard]] Status<Error> LogGeometry(const Console& console, const Geometry& g) noexcept
{
    const Line line = infra::Formatted<kLineCapacity>("Source {}x{} at ({}, {}); output {}x{} at ({}, {})", g.sourceExtent.width.Get(), g.sourceExtent.height.Get(), g.sourceRect.Left().Get(),
                                                      g.sourceRect.Top().Get(), g.targetExtent.width.Get(), g.targetExtent.height.Get(), g.targetRect.Left().Get(), g.targetRect.Top().Get());
    return Log(console, LogLevel::Info, line.Get());
}

[[nodiscard]] Result<Base, Error> ResolveBase(const Console& console, const Options& options) noexcept
{
    return real::SetDpiAwareness().and_then(real::InitializeRuntime).and_then(real::RequireCaptureSupport).and_then(ExecutableDirectory).and_then([&](const interior::DirectoryPath& directory) {
        return ResolvedGeometry(console, options).and_then([&](const Geometry& g) { return LogGeometry(console, g).transform([&] { return Base{ options, directory, g }; }); });
    });
}

[[nodiscard]] bool WantsNgx(const Options& o, const Geometry& g) noexcept
{
    return o.neuralRendering || interior::WantsSuperResolution(o, g.sourceExtent, g.targetExtent);
}

[[nodiscard]] bool LacksNvidia(const real::GpuDevice& device, bool wanted) noexcept
{
    return wanted && !device.nvidia;
}

[[nodiscard]] Status<Error> RequireNvidia(const real::GpuDevice& device, bool wanted) noexcept
{
    if (LacksNvidia(device, wanted))
        return Fail(Error{ real::ApiCall::NotNvidia, 0 });
    return {};
}

[[nodiscard]] interior::DirectoryPath DataPathOf(const Options& o, const interior::DirectoryPath& executableDirectory) noexcept
{
    return o.appDataPath.IsEmpty() ? executableDirectory : o.appDataPath;
}

[[nodiscard]] real::NgxSettings NgxSettingsOf(const Options& o, const interior::DirectoryPath& executableDirectory) noexcept
{
    return real::NgxSettings{ o.ngxAppId, o.ngxProjectId, DataPathOf(o, executableDirectory), executableDirectory, o.ngxPath, o.ngxLogLevel };
}

[[nodiscard]] Line SupportText(std::uint32_t mask) noexcept
{
    if (mask == 0)
        return infra::Formatted<kLineCapacity>("supported");
    return infra::Formatted<kLineCapacity>("not supported (mask 0x{:X}: 1 check missing, 2 driver too old, 4 adapter, 8 OS, 16 unimplemented)", mask);
}

[[nodiscard]] Line RequirementText(const real::Requirement& r) noexcept
{
    if (NVSDK_NGX_FAILED(r.result))
        return infra::Formatted<kLineCapacity>("requirements query failed with 0x{:08X}", static_cast<unsigned int>(r.result));
    return SupportText(r.supportMask);
}

[[nodiscard]] Status<Error> LogRequirement(const Console& console, const real::GpuDevice& device, const real::NgxSettings& settings, NVSDK_NGX_Feature feature, std::string_view name) noexcept
{
    const real::Requirement requirement = real::RequirementOf(device, settings, feature);
    return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("NGX feature {} ({}): {}", static_cast<int>(feature), name, RequirementText(requirement).Get()).Get());
}

[[nodiscard]] Status<Error> LogRequirements(const Console& console, const real::GpuDevice& device, const real::NgxSettings& settings) noexcept
{
    return LogRequirement(console, device, settings, NVSDK_NGX_Feature_SuperSampling, "DLSS Super Resolution").and_then([&] {
        return LogRequirement(console, device, settings, real::kNeuralRenderingFeature, "DLSS 5 Neural Rendering");
    });
}

[[nodiscard]] Line NvidiaDriverText(const interior::DriverVersion& v) noexcept
{
    const std::uint32_t number = interior::NvidiaDriverNumber(v);
    return infra::Formatted<kLineCapacity>("NVIDIA driver {}.{:02} ({}.{}.{}.{})", interior::NvidiaDriverMajor(number), interior::NvidiaDriverMinor(number), v.product, v.version, v.subVersion,
                                           v.build);
}

[[nodiscard]] Line VersionText(bool nvidia, const interior::DriverVersion& v) noexcept
{
    if (!nvidia)
        return infra::Formatted<kLineCapacity>("driver {}.{}.{}.{}", v.product, v.version, v.subVersion, v.build);
    return NvidiaDriverText(v);
}

[[nodiscard]] Line DriverText(const real::GpuDevice& device) noexcept
{
    if (!device.driverVersion.has_value())
        return infra::Formatted<kLineCapacity>("driver version unknown");
    return VersionText(device.nvidia, *device.driverVersion);
}

[[nodiscard]] Status<Error> LogAdapter(const Console& console, const real::GpuDevice& device) noexcept
{
    const std::array<char, interior::AdapterName::Capacity + 1> name = infra::NarrowedChars<interior::AdapterName::Capacity + 1>(device.name.Get());
    return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("Direct3D 12 device on '{}', {}", name.data(), DriverText(device).Get()).Get());
}

[[nodiscard]] Line RequiredDriverText() noexcept
{
    return infra::Formatted<kLineCapacity>("{}.{:02}", interior::NvidiaDriverMajor(interior::kFirstNeuralRenderingDriver), interior::NvidiaDriverMinor(interior::kFirstNeuralRenderingDriver));
}

[[nodiscard]] Line MissingAvailabilityText(const real::GpuDevice& device) noexcept
{
    return infra::Formatted<kLineCapacity>("DLSS 5 Neural Rendering is not offered by this driver's NGX loader (it has no DLSSNR.Available); NVIDIA driver {} or newer is required and this is {}",
                                           RequiredDriverText().Get(), DriverText(device).Get());
}

// The loader builds feature 18 from nvngx_dlssnr.dll in the executable folder or --ngx-path; a zero
// with no such file means there is nothing to build from, a zero with the file means it was rejected.
[[nodiscard]] Line ZeroAvailabilityText(const std::optional<interior::DirectoryPath>& model) noexcept
{
    if (!model.has_value())
        return infra::Formatted<kLineCapacity>("the NGX loader reports DLSSNR.Available = 0 and there is no nvngx_dlssnr.dll next to DlssScreen.exe or in --ngx-path; put NVIDIA's DLSS 5 model there");
    const std::array<char, interior::DirectoryPath::Capacity + 1> directory = infra::NarrowedChars<interior::DirectoryPath::Capacity + 1>(model->Get());
    return infra::Formatted<kLineCapacity>("the NGX loader reports DLSSNR.Available = 0 although nvngx_dlssnr.dll is in {}; the loader rejected that build, see nvngx.log", directory.data());
}

[[nodiscard]] Status<Error> CheckAvailabilityValue(const Console& console, const real::NgxSettings& settings, std::uint32_t available) noexcept
{
    if (available == 0)
        return Fail(Logged(console, Explanation{ ZeroAvailabilityText(real::NeuralRenderingModelLocation(settings)).Get(), Error{ real::ApiCall::NgxNeuralRenderingUnavailable, 2 } }));
    return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("DLSSNR.Available = {}", available).Get());
}

// The loader's capability block names DLSSNR.Available only when it can build feature 18 itself.
[[nodiscard]] Status<Error> CheckNeuralRendering(const Console& console, const real::GpuDevice& device, const real::NgxSettings& settings, std::optional<std::uint32_t> available) noexcept
{
    if (!available.has_value())
        return Fail(Logged(console, Explanation{ MissingAvailabilityText(device).Get(), Error{ real::ApiCall::NgxNeuralRenderingUnavailable, 1 } }));
    return CheckAvailabilityValue(console, settings, *available);
}

[[nodiscard]] Status<Error> RequireNeuralRendering(const Console& console, const real::GpuDevice& device, const real::NgxSettings& settings, const real::NgxRuntime& runtime,
                                                   bool neuralRendering) noexcept
{
    if (!neuralRendering)
        return {};
    return CheckNeuralRendering(console, device, settings, real::NeuralRenderingAvailability(runtime));
}

[[nodiscard]] Result<std::optional<real::NgxRuntime>, Error> OptionalRuntime(const Console& console, const real::GpuDevice& device, const Options& o, const real::NgxSettings& settings,
                                                                             bool wanted) noexcept
{
    if (!wanted)
        return std::optional<real::NgxRuntime>{};
    return LogRequirements(console, device, settings).and_then([&] { return real::CreateNgxRuntime(device, settings); }).and_then([&](real::NgxRuntime runtime) {
        return RequireNeuralRendering(console, device, settings, runtime, o.neuralRendering).transform([&runtime] { return std::optional<real::NgxRuntime>{ std::move(runtime) }; });
    });
}

struct Devices
{
    real::GpuDevice device;
    std::optional<real::NgxRuntime> runtime;
};

[[nodiscard]] Result<Devices, Error> CreateDevices(const Console& console, const Base& b) noexcept
{
    const bool wantsNgx = WantsNgx(b.options, b.geometry);
    return real::CreateGpuDevice(real::DeviceSettings{ b.options.debugLayer, b.options.adapter }).and_then([&](real::GpuDevice device) {
        return LogAdapter(console, device)
            .and_then([&] { return RequireNvidia(device, wantsNgx); })
            .and_then([&] { return OptionalRuntime(console, device, b.options, NgxSettingsOf(b.options, b.executableDirectory), wantsNgx); })
            .transform([&](std::optional<real::NgxRuntime> runtime) { return Devices{ std::move(device), std::move(runtime) }; });
    });
}

[[nodiscard]] Status<Error> RequireSuperResolutionIf(const std::optional<real::NgxRuntime>& runtime, bool wanted) noexcept
{
    if (!wanted)
        return {};
    REQUIRE(runtime.has_value());
    return real::RequireSuperResolution(*runtime);
}

[[nodiscard]] interior::QualityTable TableFor(const std::optional<real::NgxRuntime>& runtime, const interior::Extent& target) noexcept
{
    if (!runtime.has_value())
        return interior::QualityTable{};
    return real::QualityTableFor(*runtime, target);
}

[[nodiscard]] bool LacksOpticalFlowBuild(const Options& o) noexcept
{
    return !kHasOpticalFlow && o.motion == interior::MotionBackend::NvOpticalFlow;
}

[[nodiscard]] Status<Error> RequireOpticalFlowBuild(const Options& o) noexcept
{
    if (LacksOpticalFlowBuild(o))
        return Fail(Error{ real::ApiCall::OpticalFlowUnavailable, 0 });
    return {};
}

[[nodiscard]] Result<SessionPlan, Error> Planned(const Console& console, const Base& b, const Devices& d) noexcept
{
    const bool wantsSr = interior::WantsSuperResolution(b.options, b.geometry.sourceExtent, b.geometry.targetExtent);
    return RequireOpticalFlowBuild(b.options).and_then([&] { return RequireSuperResolutionIf(d.runtime, wantsSr); }).and_then([&] {
        return interior::PlanSession(b.options, b.geometry, TableFor(d.runtime, b.geometry.targetExtent)).transform_error([&console](interior::PlanError e) {
            return Logged(console, ExplainPlan(e));
        });
    });
}

[[nodiscard]] std::string_view MotionName(interior::MotionBackend motion) noexcept
{
    switch (motion)
    {
    case interior::MotionBackend::BuiltIn: return "block matching";
    case interior::MotionBackend::NvOpticalFlow: return "NVIDIA Optical Flow";
    case interior::MotionBackend::None: return "zero motion";
    }
    return "";
}

[[nodiscard]] Line SuperResolutionText(const SessionPlan& p) noexcept
{
    if (!p.superResolution.has_value())
        return infra::Formatted<kLineCapacity>("no super resolution");
    return infra::Formatted<kLineCapacity>("DLSS {} {}x{} -> {}x{}", interior::Describe(p.superResolution->quality), p.superResolution->input.width.Get(), p.superResolution->input.height.Get(),
                                           p.superResolution->output.width.Get(), p.superResolution->output.height.Get());
}

[[nodiscard]] std::string_view NeuralRenderingText(const SessionPlan& p) noexcept
{
    return p.neuralRendering ? "DLSS 5 Neural Rendering" : "neural rendering off";
}

[[nodiscard]] Status<Error> LogTuning(const Console& console, const interior::NrTuning& t) noexcept
{
    const Line line =
        infra::Formatted<kLineCapacity>("Neural rendering tuning: preset {}, intensity {:.2f}, style {}, local structure {:.2f}, local tone {:.2f}, skin {:.2f}, auto mask {}, UI correction {}",
                                        t.preset.Get(), t.intensity.Get(), interior::StyleCode(t.style), t.localStructure.Get(), t.localTone.Get(), t.skinStructure.Get(), t.autoMask, t.uiCorrection);
    return Log(console, LogLevel::Info, line.Get());
}

[[nodiscard]] Status<Error> LogTuningIf(const Console& console, const SessionPlan& p) noexcept
{
    if (!p.neuralRendering)
        return {};
    return LogTuning(console, p.tuning);
}

[[nodiscard]] Status<Error> LogPlan(const Console& console, const SessionPlan& p) noexcept
{
    const Line line = infra::Formatted<kLineCapacity>("Pipeline: capture {}x{} -> {} -> {} -> {} -> present {}x{}", p.source.width.Get(), p.source.height.Get(), MotionName(p.motion),
                                                      SuperResolutionText(p).Get(), NeuralRenderingText(p), p.target.width.Get(), p.target.height.Get());
    return Log(console, LogLevel::Info, line.Get()).and_then([&] { return LogTuningIf(console, p); });
}

[[nodiscard]] bool OverlapsSource(const Options& o, const Geometry& g) noexcept
{
    return interior::IsSameRect(g.sourceRect, g.targetRect) || o.source.kind == interior::MonitorSelectionKind::All;
}

[[nodiscard]] bool FeedsBack(const Options& o, const Geometry& g) noexcept
{
    return !o.displayAffinity && OverlapsSource(o, g);
}

[[nodiscard]] Status<Error> WarnFeedback(const Console& console, const Options& o, const Geometry& g) noexcept
{
    if (!FeedsBack(o, g))
        return {};
    return Log(console, LogLevel::Warn, "The output window is not excluded from capture and overlaps the source: expect a feedback loop");
}

[[nodiscard]] real::WindowSettings WindowSettingsOf(const Options& o) noexcept
{
    return real::WindowSettings{ o.topmost, o.clickThrough, o.displayAffinity, o.redirectionBitmap };
}

[[nodiscard]] Result<real::OutputWindow, Error> CreatedWindow(const Console& console, const Base& b) noexcept
{
    return WarnFeedback(console, b.options, b.geometry).and_then([&] { return real::CreateOutputWindow(b.geometry.targetRect, WindowSettingsOf(b.options)); }).and_then([](real::OutputWindow window) {
        return real::RegisterHotkeys(window).transform([&window] { return std::move(window); });
    });
}

// The panel is the ordinary way in: it opens unless --gui off asks for the overlay alone.
[[nodiscard]] Result<std::optional<real::ControlPanel>, Error> CreatedPanel(const Options& o, const SessionPlan& plan) noexcept
{
    if (!o.gui)
        return std::optional<real::ControlPanel>{};
    return real::CreateControlPanel(interior::ModelControls{ plan.neuralRendering, plan.tuning }, plan.initialDisplay).transform([](real::ControlPanel panel) {
        return std::optional<real::ControlPanel>{ std::move(panel) };
    });
}

[[nodiscard]] Result<real::RealEnvironment, Error> Environment(const Console& console, const Base& b, Devices d, const SessionPlan& plan) noexcept
{
    return CreatedWindow(console, b).and_then([&](real::OutputWindow window) {
        return CreatedPanel(b.options, plan).and_then([&](std::optional<real::ControlPanel> panel) {
            return real::CreateEnvironment(std::move(d.device), std::move(d.runtime), plan, b.geometry, std::move(window), std::move(panel), real::EnvironmentSettings{ b.options.captureBorder },
                                           console);
        });
    });
}

[[nodiscard]] Result<interior::FrameNumber, Error> Settled(real::RealEnvironment& env, const Result<app::SessionOutcome, Error>& outcome) noexcept
{
    const Result<interior::FenceValue, Error> idle = real::WaitIdle(env.Devices().device, env.LastFence());
    if (!outcome.has_value())
        return Fail(outcome.error());
    return idle.transform([&outcome](interior::FenceValue) { return outcome->finalState.number; });
}

[[nodiscard]] Result<interior::FrameNumber, Error> Drive(const Console& console, const SessionPlan& plan, real::RealEnvironment& env) noexcept
{
    real::ShowOutputWindow(env.Window());
    return Log(console, LogLevel::Info, "Running. Hotkeys: Ctrl+Alt+Shift+O original/processed, Ctrl+Alt+Shift+C split view, Ctrl+Alt+Shift+Q quit").and_then([&] {
        return Settled(env, app::RunSession<real::RealEnvironment, Error>(env, plan, interior::InitialFrameState(plan), kFrameLimit));
    });
}

[[nodiscard]] Result<interior::FrameNumber, Error> Run(const Console& console, const Options& options) noexcept
{
    return ResolveBase(console, options).and_then([&](const Base& b) {
        return CreateDevices(console, b).and_then([&](Devices d) {
            return Planned(console, b, d).and_then([&](const SessionPlan& plan) {
                return LogPlan(console, plan).and_then([&] { return Environment(console, b, std::move(d), plan); }).and_then([&](real::RealEnvironment env) { return Drive(console, plan, env); });
            });
        });
    });
}

[[nodiscard]] int Failed(const Console& console, const Error& error) noexcept
{
    const real::ErrorText text = real::Describe(error);
    if (!console.attached)
        real::ShowMessage(text.Get());
    return Log(console, LogLevel::Error, text.Get()).transform([] { return kExitFailure; }).value_or(kExitFailure);
}

[[nodiscard]] int Finish(const Console& console, const Result<interior::FrameNumber, Error>& result) noexcept
{
    if (!result.has_value())
        return Failed(console, result.error());
    return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("Stopped after {} frames", result->Get()).Get()).transform([] { return kExitOk; }).value_or(kExitFailure);
}

[[nodiscard]] int RunWithConsole(const Options& options) noexcept
{
    const Result<Console, Error> console = real::OpenConsole(options.logLevel, options.logFile, options.console);
    if (!console.has_value())
        return ReportEarly(console.error());
    return Finish(*console, Run(*console, options));
}

// Printing to a console the operator does not have helps nobody, so these two ask for one.
[[nodiscard]] Result<Console, Error> ConsoleForReading(const Options& options) noexcept
{
    return real::OpenConsole(options.logLevel, options.logFile, interior::ConsoleMode::On);
}

[[nodiscard]] int ServeOrRun(const Options& options) noexcept
{
    if (!options.listMonitors)
        return RunWithConsole(options);
    return ConsoleForReading(options).transform([](const Console&) { return ExitCodeOf(ListMonitors()); }).value_or(kExitFailure);
}

[[nodiscard]] int Serve(const Options& options) noexcept
{
    if (!options.showHelp)
        return ServeOrRun(options);
    return ConsoleForReading(options).transform([](const Console&) { return PrintUsage(); }).value_or(kExitFailure);
}

[[nodiscard]] int Dispatch(const Result<Options, interior::OptionsError>& parsed) noexcept
{
    if (!parsed.has_value())
        return UsageFailure(parsed.error());
    return Serve(*parsed);
}

} // namespace

int main()
{
    const infra::Result<Arguments, real::Error> arguments = ReadArguments();
    if (!arguments.has_value())
        return ReportEarly(arguments.error());
    return Dispatch(interior::ParseOptions(std::span<const std::wstring_view>(arguments->views.data(), arguments->count)));
}
