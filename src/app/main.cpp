// WAIVER(R31): the composition root wires the real effect layer to the pure core; nothing else lives here.
#include "app/session.h"
#include "effects/real/capture.h"
#include "effects/real/console.h"
#include "effects/real/device.h"
#include "effects/real/environment.h"
#include "effects/real/ngx.h"
#include "effects/real/panel.h"
#include "effects/real/trust.h"
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
constexpr std::size_t kCommandCapacity = 300;
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
    interior::MonitorList monitors; // every monitor, not only the ones being captured, so the panel can name them
};

[[nodiscard]] Result<Geometry, Error> ResolvedGeometry(const Console& console, const Options& options, const interior::MonitorList& monitors) noexcept
{
    return interior::ResolveGeometry(monitors, options).transform_error([&console](interior::MonitorError e) { return Logged(console, ExplainMonitor(e)); });
}

[[nodiscard]] Status<Error> LogGeometry(const Console& console, const Geometry& g) noexcept
{
    const Line line = infra::Formatted<kLineCapacity>("Source {}x{} at ({}, {}); output {}x{} at ({}, {})", g.sourceExtent.width.Get(), g.sourceExtent.height.Get(), g.sourceRect.Left().Get(),
                                                      g.sourceRect.Top().Get(), g.targetExtent.width.Get(), g.targetExtent.height.Get(), g.targetRect.Left().Get(), g.targetRect.Top().Get());
    return Log(console, LogLevel::Info, line.Get());
}

[[nodiscard]] Result<Base, Error> BasedOn(const Console& console, const Options& options, const interior::DirectoryPath& directory, const interior::MonitorList& monitors,
                                          const interior::MonitorList& sources) noexcept
{
    return ResolvedGeometry(console, options, sources).and_then([&](const Geometry& g) {
        return LogGeometry(console, g).transform([&] { return Base{ options, directory, g, interior::Ordered(monitors) }; });
    });
}

// One window, if one was asked for, standing in for the monitor list: the geometry then works out to that
// window's own rectangle, and the capture opens an item for the window rather than for a monitor.
[[nodiscard]] Result<interior::MonitorList, Error> SourcesFor(const Options& o, const interior::MonitorList& monitors) noexcept
{
    if (o.window.IsEmpty())
        return monitors;
    return real::FindWindowNamed(o.window).and_then(
        [](const interior::MonitorInfo& found) { return interior::MonitorList{}.Push(found).transform_error([](infra::CapacityExceeded) { return Error{ real::ApiCall::WindowNotFound, 2 }; }); });
}

// A single window is captured whole, so "every source" is what the geometry is asked for.
[[nodiscard]] Options AsWholeSource(const Options& o) noexcept
{
    Options whole = o; // WAIVER(R2): a copy with one answer replaced, made once and read from there on.
    whole.source = interior::SourceSelection{ interior::MonitorSelectionKind::All, interior::RequestedMonitorTag::Parse(0) };
    return whole;
}

[[nodiscard]] Options SelectionFor(const Options& o) noexcept
{
    return o.window.IsEmpty() ? o : AsWholeSource(o);
}

// Settled once for the whole program: how the process reads the display's scaling, and which apartment it
// has. Asking for the scaling twice is refused outright, with an access denied that is nothing of the sort.
[[nodiscard]] Status<Error> PrepareProcess() noexcept
{
    return real::SetDpiAwareness().and_then(real::InitializeRuntime).and_then(real::RequireCaptureSupport);
}

[[nodiscard]] Result<Base, Error> ResolveBase(const Console& console, const Options& options) noexcept
{
    return ExecutableDirectory().and_then([&](const interior::DirectoryPath& directory) {
        return real::EnumerateMonitors().and_then([&](const interior::MonitorList& monitors) {
            return SourcesFor(options, monitors).and_then([&](const interior::MonitorList& sources) { return BasedOn(console, SelectionFor(options), directory, monitors, sources); });
        });
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
    return real::NgxSettings{ o.ngxAppId, o.ngxProjectId, DataPathOf(o, executableDirectory), executableDirectory, o.ngxPath, o.ngxLogLevel, o.indicator, o.cubinCache };
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

[[nodiscard]] Result<std::optional<real::TrustedFile>, Error> Checked(const Console& console, const interior::FilePath& file) noexcept
{
    return real::OpenTrusted(file).and_then([&console](real::TrustedFile model) {
        return Log(console, LogLevel::Info, "nvngx_dlssnr.dll is signed by NVIDIA").transform([&model] { return std::optional<real::TrustedFile>{ std::move(model) }; });
    });
}

[[nodiscard]] std::optional<interior::FilePath> ModelToCheck(const real::NgxSettings& settings, bool wanted) noexcept
{
    return wanted ? real::NeuralRenderingModelFile(settings) : std::nullopt;
}

// The model is a DLL the loader picks up by name from a folder anyone may write to, so it is checked and
// then held open for the life of the session. A missing file is left to the loader, which says so better.
[[nodiscard]] Result<std::optional<real::TrustedFile>, Error> TrustedModel(const Console& console, const real::NgxSettings& settings, bool wanted) noexcept
{
    const std::optional<interior::FilePath> file = ModelToCheck(settings, wanted);
    if (!file.has_value())
        return std::optional<real::TrustedFile>{};
    return Checked(console, *file);
}

struct Devices
{
    real::GpuDevice device;
    std::optional<real::NgxRuntime> runtime;
    std::optional<real::TrustedFile> model; // held open so the file that was checked is the file that loads
};

[[nodiscard]] Result<Devices, Error> WithRuntime(const Console& console, const Base& b, real::GpuDevice device, const real::NgxSettings& settings, bool wantsNgx) noexcept
{
    return TrustedModel(console, settings, wantsNgx && b.options.neuralRendering).and_then([&](std::optional<real::TrustedFile> model) {
        return OptionalRuntime(console, device, b.options, settings, wantsNgx).transform([&](std::optional<real::NgxRuntime> runtime) {
            return Devices{ std::move(device), std::move(runtime), std::move(model) };
        });
    });
}

[[nodiscard]] Result<Devices, Error> CreateDevices(const Console& console, const Base& b) noexcept
{
    const bool wantsNgx = WantsNgx(b.options, b.geometry);
    return real::CreateGpuDevice(real::DeviceSettings{ b.options.debugLayer, b.options.adapter }).and_then([&](real::GpuDevice device) {
        return LogAdapter(console, device).and_then([&] { return RequireNvidia(device, wantsNgx); }).and_then([&] {
            return WithRuntime(console, b, std::move(device), NgxSettingsOf(b.options, b.executableDirectory), wantsNgx);
        });
    });
}

[[nodiscard]] bool OffersSuperResolution(const Devices& d) noexcept
{
    return d.runtime.has_value() && real::OffersSuperResolution(*d.runtime);
}

// A session that asked for super resolution and cannot have it runs without it rather than stopping.
// Nothing else in the picture depends on it, and the panel greys the choice and says why.
[[nodiscard]] Options WithoutSuperResolution(const Options& o) noexcept
{
    Options without = o; // WAIVER(R2): a copy with one answer replaced, made once and read from there on.
    without.sr = interior::SrMode::Off;
    return without;
}

[[nodiscard]] Base Offered(const Base& b, const Devices& d) noexcept
{
    if (OffersSuperResolution(d))
        return b;
    return Base{ WithoutSuperResolution(b.options), b.executableDirectory, b.geometry, b.monitors };
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
    return RequireOpticalFlowBuild(b.options).and_then([&] {
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

// --- naming what the machine turned out to have --------------------------------------------------------

using Caption = real::ChoiceText;

[[nodiscard]] Caption Named(std::wstring_view text) noexcept
{
    return Caption::Parse(text.substr(0, std::min(text.size(), Caption::Capacity))).value_or(Caption{});
}

[[nodiscard]] Caption MonitorCaption(std::size_t index, const interior::MonitorInfo& m) noexcept
{
    const infra::BoundedString<char, Caption::Capacity> line =
        infra::Formatted<Caption::Capacity>("{}: {}x{}{}", index, m.rect.Right().Get() - m.rect.Left().Get(), m.rect.Bottom().Get() - m.rect.Top().Get(), m.primary ? " primary" : "");
    return Named(infra::WidenedChars<Caption::Capacity + 1>(line.Get()).data());
}

[[nodiscard]] real::PanelList WithMonitors(real::PanelList list, const interior::MonitorList& monitors) noexcept
{
    const auto add = [&monitors](const real::PanelList& so, std::size_t i) { return real::PanelList{ so.choices.Push(MonitorCaption(i, monitors.At(i))).value_or(so.choices), so.chosen }; };
    return std::ranges::fold_left(std::views::iota(std::size_t{ 0 }, monitors.Size()), list, add);
}

[[nodiscard]] real::PanelList Started(std::wstring_view first, std::size_t chosen) noexcept
{
    return real::PanelList{ real::PanelList{}.choices.Push(Named(first)).value_or(real::ChoiceTexts{}), chosen };
}

// Which monitor the session is capturing: the two answers that name none come first, so a named one sits
// at its own index plus two.
[[nodiscard]] std::size_t SourceChoice(const Options& o) noexcept
{
    if (o.source.kind != interior::MonitorSelectionKind::Index)
        return static_cast<std::size_t>(o.source.kind);
    return o.source.index.Get() + 2u;
}

[[nodiscard]] real::PanelList SourceList(const Options& o, const interior::MonitorList& monitors) noexcept
{
    const real::PanelList primary = Started(L"Primary monitor", SourceChoice(o));
    const real::PanelList both{ primary.choices.Push(Named(L"All monitors")).value_or(primary.choices), primary.chosen };
    return WithMonitors(both, monitors);
}

[[nodiscard]] real::PanelList TargetList(const Options& o, const interior::MonitorList& monitors) noexcept
{
    return WithMonitors(Started(L"Same as the source", o.target.has_value() ? o.target->Get() + 1u : 0u), monitors);
}

[[nodiscard]] real::PanelList WithAdapters(real::PanelList list, const real::AdapterList& adapters) noexcept
{
    const auto add = [&adapters](const real::PanelList& so, std::size_t i) { return real::PanelList{ so.choices.Push(Named(adapters.At(i).name.Get())).value_or(so.choices), so.chosen }; };
    return std::ranges::fold_left(std::views::iota(std::size_t{ 0 }, adapters.Size()), list, add);
}

[[nodiscard]] real::PanelList AdapterList(const Options& o, const real::AdapterList& adapters) noexcept
{
    return WithAdapters(Started(L"First NVIDIA adapter", o.adapter.has_value() ? o.adapter->Get() + 1u : 0u), adapters);
}

// The presets the model will admit to carrying. Nothing here leaves the choice off the panel entirely.
[[nodiscard]] real::PanelList PresetList(const Options& o, std::uint32_t count) noexcept
{
    const auto add = [](const real::PanelList& so, std::uint32_t i) {
        return real::PanelList{ so.choices.Push(Named(infra::WidenedChars<Caption::Capacity + 1>(infra::Formatted<Caption::Capacity>("Preset {}", i).Get()).data())).value_or(so.choices), so.chosen };
    };
    return std::ranges::fold_left(std::views::iota(std::uint32_t{ 0 }, count), real::PanelList{ real::ChoiceTexts{}, o.tuning.preset.Get() }, add);
}

[[nodiscard]] real::PanelLists ListsFor(const Base& b, const real::AdapterList& adapters, std::uint32_t presets) noexcept
{
    return real::PanelLists{ PresetList(b.options, presets), SourceList(b.options, b.monitors), TargetList(b.options, b.monitors), AdapterList(b.options, adapters) };
}

// The model names its presets or it does not. It does not, so far, and the panel then leaves the choice
// out rather than offering numbers that all fall back to the single set of weights the model carries.
[[nodiscard]] std::uint32_t OfferedPresets(const Devices& d) noexcept
{
    if (!d.runtime.has_value())
        return 0;
    return real::NeuralRenderingPresetCount(*d.runtime).value_or(0);
}

[[nodiscard]] bool CapturesOneWindow(const Base& b) noexcept
{
    return !b.options.window.IsEmpty() && !b.geometry.source.IsEmpty();
}

[[nodiscard]] std::optional<interior::MonitorHandle> FollowedWindow(const Base& b) noexcept
{
    if (!CapturesOneWindow(b))
        return std::nullopt;
    return b.geometry.source.At(0).handle;
}

[[nodiscard]] real::PanelFindings FindingsFor(const Base& b, const Devices& d) noexcept
{
    return real::PanelFindings{ ListsFor(b, real::UsableAdapters(d.device.factory.Get()), OfferedPresets(d)), OffersSuperResolution(d), FollowedWindow(b) };
}

[[nodiscard]] real::EnvironmentSettings SettingsOf(const Base& b, const SessionPlan& plan) noexcept
{
    const Options& o = b.options;
    return real::EnvironmentSettings{ interior::SurfaceSettings{ o.cursor, o.captureBorder, o.displayAffinity, o.topmost, o.clickThrough, o.logLevel }, plan.captureCursor, FollowedWindow(b) };
}

[[nodiscard]] Result<real::RealEnvironment, Error> Environment(const Console& console, const Base& b, Devices d, const SessionPlan& plan, const real::ControlPanel* panel) noexcept
{
    return CreatedWindow(console, b).and_then([&](real::OutputWindow window) {
        return real::CreateEnvironment(std::move(d.device), std::move(d.runtime), plan, b.geometry, std::move(window), panel, SettingsOf(b, plan), b.options, console);
    });
}

[[nodiscard]] Result<interior::FrameNumber, Error> Settled(real::RealEnvironment& env, const Result<app::SessionOutcome, Error>& outcome) noexcept
{
    const Result<interior::FenceValue, Error> idle = real::WaitIdle(env.Devices().device, env.LastFence());
    if (!outcome.has_value())
        return Fail(outcome.error());
    return idle.transform([&outcome](interior::FenceValue) { return outcome->finalState.number; });
}

// The operator asked the start-up page for a session with different settings: this one starts it and
// leaves. Nothing is inherited but the command line, so the new session is exactly what the page says.
[[nodiscard]] Status<Error> Relaunch(const interior::CommandLine& arguments) noexcept
{
    std::array<wchar_t, MAX_PATH> executable{}; // WAIVER(R2): a local buffer filled once, before use.
    if (IsPathFailure(::GetModuleFileNameW(nullptr, executable.data(), MAX_PATH)))
        return Fail(real::LastError(real::ApiCall::GetModuleFileNameW));
    infra::BoundedString<wchar_t, kCommandCapacity> line =
        infra::BoundedString<wchar_t, kCommandCapacity>::Parse(std::wstring_view(executable.data())).value_or(infra::BoundedString<wchar_t, kCommandCapacity>{});
    return real::StartProcess(line.Get(), arguments.Get());
}

// The panel belongs to the operator rather than to any one session, so it is made once and kept while
// sessions are torn down and built again underneath it. Its place on screen, its page and its notice stay.
struct PanelHolder
{
    std::optional<real::ControlPanel> panel;
};

[[nodiscard]] bool AlreadyAnswered(const Base& b, const PanelHolder& held) noexcept
{
    return held.panel.has_value() || !b.options.gui;
}

[[nodiscard]] const real::ControlPanel* Borrowed(const PanelHolder& held) noexcept
{
    return held.panel.has_value() ? &*held.panel : nullptr;
}

[[nodiscard]] Result<const real::ControlPanel*, Error> HeldPanel(const Base& b, const SessionPlan& plan, const real::PanelFindings& findings, PanelHolder& held) noexcept
{
    if (AlreadyAnswered(b, held))
        return Borrowed(held);
    return real::CreateControlPanel(b.options, interior::StartingLive(plan), plan.initialDisplay, findings).transform([&held](real::ControlPanel made) {
        held.panel = std::move(made);
        return &*held.panel;
    });
}

// What one session leaves behind: how far it got, the settings the operator asked the next one for, and
// whether the window it was working on changed size, which asks for the same settings over again.
struct Ended
{
    interior::FrameNumber frames;
    std::optional<interior::CommandLine> again;
    bool resized;
    bool abandoned; // the one window it was working on was closed, minimised or hidden
};

// Whether this machine could keep our windows out of our own capture alone, instead of out of every
// capture on the machine. Reported so it can be answered on real hardware; nothing acts on it yet.
[[nodiscard]] Status<Error> LogExclusion(const Console& console, bool offered) noexcept
{
    if (offered)
        return Log(console, LogLevel::Info, "Per-session window exclusion is offered here: the overlay could be hidden from our capture alone");
    return Log(console, LogLevel::Info, "Per-session window exclusion is not offered here: hiding the overlay from our capture hides it from all capture");
}

[[nodiscard]] Result<Ended, Error> Drive(const Console& console, const Options& options, const SessionPlan& plan, real::RealEnvironment& env) noexcept
{
    real::ShowOutputWindow(env.Window());
    return LogExclusion(console, real::OffersWindowExclusion(env.Devices().capture))
        .and_then([&] { return Log(console, LogLevel::Info, "Running. Hotkeys: Ctrl+Alt+Shift+O original/processed, Ctrl+Alt+Shift+C split view, Ctrl+Alt+Shift+Q quit"); })
        .and_then([&] { return Settled(env, app::RunSession<real::RealEnvironment, Error>(env, plan, interior::InitialFrameState(plan), kFrameLimit)); })
        .transform([&](interior::FrameNumber frames) { return Ended{ .frames = frames, .again = env.Restart(options), .resized = env.Resized(), .abandoned = env.Abandoned() }; });
}

// One session, from the devices up. Everything it makes goes away when it returns, which is what lets the
// next one be made differently; the panel is the operator's and is not part of any of it.
[[nodiscard]] Result<Ended, Error> Staged(const Console& console, const Base& b, Devices d, const SessionPlan& plan, PanelHolder& held) noexcept
{
    const real::PanelFindings findings = FindingsFor(b, d);
    return LogPlan(console, plan).and_then([&] { return HeldPanel(b, plan, findings, held); }).and_then([&](const real::ControlPanel* panel) {
        return Environment(console, b, std::move(d), plan, panel).and_then([&](real::RealEnvironment env) { return Drive(console, b.options, plan, env); });
    });
}

[[nodiscard]] Result<Ended, Error> RunOnce(const Console& console, const Options& options, PanelHolder& held) noexcept
{
    return ResolveBase(console, options).and_then([&](const Base& found) {
        return CreateDevices(console, found).and_then([&](Devices d) {
            const Base b = Offered(found, d);
            return Planned(console, b, d).and_then([&](const SessionPlan& plan) { return Staged(console, b, std::move(d), plan, held); });
        });
    });
}

// The splitter expects a program name in front, as a real command line has, so one is put there and the
// answer starts after it, which is what this process does with the line it was given itself.
[[nodiscard]] std::array<wchar_t, interior::CommandLine::Capacity + 4> WithProgramName(const interior::CommandLine& line) noexcept
{
    std::array<wchar_t, interior::CommandLine::Capacity + 4> whole{}; // WAIVER(R2): a local buffer filled once, before use.
    const int written = ::_snwprintf_s(whole.data(), whole.size(), _TRUNCATE, L"x %.*s", static_cast<int>(line.Get().size()), line.Get().data());
    ENSURE(written > 0);
    return whole;
}

[[nodiscard]] Result<Arguments, Error> SplitArguments(const interior::CommandLine& line) noexcept
{
    int argc = 0; // WAIVER(R2): the answer of one call, read once after it.
    wchar_t** argv = ::CommandLineToArgvW(WithProgramName(line).data(), &argc);
    if (argv == nullptr)
        return Fail(real::LastError(real::ApiCall::CommandLineToArgvW));
    return Collected(ArgumentBlock(argv), argc);
}

// What the start-up page describes is read back through the parser the command line uses, so a session
// built from it is the session a fresh process would have built.
[[nodiscard]] Result<Options, Error> Reread(const interior::CommandLine& asked) noexcept
{
    return SplitArguments(asked).and_then([](const Arguments& a) {
        return interior::ParseOptions(std::span<const std::wstring_view>(a.views.data(), a.count)).transform_error([](const interior::OptionsError&) {
            return Error{ real::ApiCall::CommandLineToArgvW, 1 };
        });
    });
}

// One session after another, each built from what the last one's panel asked for.
struct Cycle
{
    Options wanted;
    Result<Ended, Error> ended;
};

[[nodiscard]] bool AsksForTheSame(const Ended& ended) noexcept
{
    return ended.resized || ended.abandoned;
}

[[nodiscard]] bool AsksAgain(const Ended& ended) noexcept
{
    return ended.again.has_value() || AsksForTheSame(ended);
}

[[nodiscard]] bool Continues(const Cycle& c) noexcept
{
    return c.ended.has_value() && AsksAgain(*c.ended);
}

// The debug layer is the one setting a session cannot take back: Direct3D turns it on for the process and
// there is no turning it off again. Everything else is made afresh below and needs no new process at all.
[[nodiscard]] bool NeedsAFreshProcess(const Options& was, const Options& now) noexcept
{
    return was.debugLayer && !now.debugLayer;
}

// The window a session was following went away, so the next one is not given one: the source that session
// already carries names the monitor, and that is where the model goes back to.
[[nodiscard]] Options WithoutWindow(const Options& o) noexcept
{
    Options next = o; // WAIVER(R2): a copy with one answer replaced, read once after it.
    next.window = interior::WindowTitle{};
    return next;
}

[[nodiscard]] Options WantedNext(const Cycle& c) noexcept
{
    return c.ended->abandoned ? WithoutWindow(c.wanted) : c.wanted;
}

[[nodiscard]] Cycle Again(const Console& console, const Options& o, PanelHolder& held) noexcept
{
    return Cycle{ o, RunOnce(console, o, held) };
}

[[nodiscard]] Cycle Relaunching(const Cycle& c, const Options& now) noexcept
{
    const interior::FrameNumber frames = c.ended->frames;
    return Cycle{ now, Relaunch(*c.ended->again).transform([frames] { return Ended{ .frames = frames, .again = std::nullopt, .resized = false, .abandoned = false }; }) };
}

[[nodiscard]] Cycle Continued(const Console& console, const Cycle& c, const Options& now, PanelHolder& held) noexcept
{
    if (NeedsAFreshProcess(c.wanted, now))
        return Relaunching(c, now);
    return Again(console, now, held);
}

[[nodiscard]] Cycle Asked(const Console& console, const Cycle& c, PanelHolder& held) noexcept
{
    const Result<Options, Error> now = Reread(*c.ended->again);
    if (!now.has_value())
        return Cycle{ c.wanted, Fail(now.error()) };
    return Continued(console, c, *now, held);
}

// A window that changed size asks for the settings it already had: the same window, measured again.
[[nodiscard]] Cycle Next(const Console& console, const Cycle& c, PanelHolder& held) noexcept
{
    if (!c.ended->again.has_value())
        return Again(console, WantedNext(c), held);
    return Asked(console, c, held);
}

// A loop rather than one session calling the next, so asking for a hundred of them costs a hundred
// sessions and not a hundred stack frames.
[[nodiscard]] Result<interior::FrameNumber, Error> Sessions(const Console& console, const Options& options) noexcept
{
    PanelHolder held{};                                      // WAIVER(R2): the panel outlives the sessions, made once when the first asks for it.
    Cycle cycle{ options, RunOnce(console, options, held) }; // WAIVER(R2): one session at a time, replaced whole by the next.
    while (Continues(cycle))                                 // WAIVER(R2): one turn of the loop is one session.
        cycle = Next(console, cycle, held);
    return cycle.ended.transform([](const Ended& e) { return e.frames; });
}

[[nodiscard]] Result<interior::FrameNumber, Error> Run(const Console& console, const Options& options) noexcept
{
    return PrepareProcess().and_then([&] { return Sessions(console, options); });
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
