// WAIVER(R31): the composition root wires the real effect layer to the pure core; nothing else lives here.
#include "app/session.h"
#include "effects/real/capture.h"
#include "effects/real/console.h"
#include "effects/real/device.h"
#include "effects/real/environment.h"
#include "effects/real/exclusion.h"
#include "effects/real/ngx.h"
#include "effects/real/panel.h"
#include "effects/real/trust.h"
#include "effects/real/window.h"
#include "global_common.h"
#include "infrastructure/array_util.h"
#include "infrastructure/fold.h"
#include "infrastructure/text.h"
#include "interior/capture_name.h"
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

[[nodiscard]] Result<Arguments, Error> Collected(ArgumentBlock block, int argc) noexcept
{
    static constexpr auto ArgumentAt = [] [[nodiscard]] (wchar_t * *argv, std::size_t count, std::size_t index) noexcept -> std::wstring_view {
        if (index >= count)
            return std::wstring_view{};
        return std::wstring_view(argv[index + 1]);
    };
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
    const Line line = infra::Formatted<kLineCapacity>(DSCREEN_PRODUCT_NAME ": {}\n", real::Describe(error).Get());
    real::ShowMessage(line.Get());
    return real::WriteText(stderr, line.Get()).transform([] { return kExitFailure; }).value_or(kExitFailure);
}

// --- start-up stages -----------------------------------------------------------------------------------------

[[nodiscard]] Error FromPlanError(interior::PlanError error) noexcept
{
    return Error{ real::ApiCall::PlanSession, static_cast<std::uint32_t>(error) };
}

struct Explanation
{
    std::string_view text;
    Error error;
};

[[nodiscard]] bool IsPathFailure(DWORD length) noexcept
{
    return length == 0 || length >= MAX_PATH;
}

[[nodiscard]] Result<interior::DirectoryPath, Error> ExecutableDirectory() noexcept
{
    static constexpr auto DirectoryOf = [] [[nodiscard]] (std::wstring_view path) noexcept -> Result<interior::DirectoryPath, Error> {
        const std::size_t slash = path.find_last_of(L"\\/");
        if (slash == std::wstring_view::npos)
            return Fail(Error{ real::ApiCall::ExecutableDirectory, 0 });
        return interior::DirectoryPath::Parse(path.substr(0, slash)).transform_error([](infra::StringTooLong) { return Error{ real::ApiCall::ExecutableDirectory, 1 }; });
    };
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

// The slots of the checked files are the ones LoadableFilesOf lists; these two are the DLSS 5 model beside
// the executable, which the loader takes first, and under --ngx-path.
constexpr std::size_t kModelBeside = 0;
constexpr std::size_t kModelInPath = 1;

// What each slot holds: the kind a refusal is named under, and the name the log says.
struct Loadable
{
    real::ModelKind kind;
    std::string_view name;
};

constexpr std::array<Loadable, real::kLoadableCount> kLoadables{ { { real::ModelKind::NeuralRendering, "nvngx_dlssnr.dll" },
                                                                   { real::ModelKind::NeuralRendering, "nvngx_dlssnr.dll" },
                                                                   { real::ModelKind::SuperResolution, "nvngx_dlss.dll" },
                                                                   { real::ModelKind::SuperResolution, "nvngx_dlss.dll" },
                                                                   { real::ModelKind::Runtime, "_nvngx.dll" },
                                                                   { real::ModelKind::Runtime, "_nvngx.dll" },
                                                                   { real::ModelKind::Runtime, "nvngx.dll" },
                                                                   { real::ModelKind::Runtime, "nvngx.dll" } } };

struct Devices
{
    real::GpuDevice device;
    std::optional<real::NgxRuntime> runtime;
    real::HeldFiles held; // checked and open, handed to the environment, which keeps them open for as long as it runs
};

[[nodiscard]] bool OffersSuperResolution(const Devices& d) noexcept
{
    return d.runtime.has_value() && real::OffersSuperResolution(*d.runtime);
}

// Whether the model file calls itself what the model is called: the copy beside the executable, which the
// loader takes first, or else the one under --ngx-path. With no file of ours in play there is nothing to doubt.
[[nodiscard]] bool ModelAsNamed(const Devices& d) noexcept
{
    const std::optional<real::TrustedFile>& model = d.held[kModelBeside].has_value() ? d.held[kModelBeside] : d.held[kModelInPath];
    return !model.has_value() || model->product.Get() == real::kNeuralRenderingProduct;
}

[[nodiscard]] bool OverlapsSource(const Options& o, const Geometry& g) noexcept
{
    return interior::IsSameRect(g.sourceRect, g.targetRect) || o.source.kind == interior::MonitorSelectionKind::All;
}

// A window capture never holds what is stacked in front of the window, and an output on a monitor that
// is not being captured is not in the picture either. Only where it is has anything to be left out.
[[nodiscard]] bool IsInItsOwnCapture(const Options& o, const Geometry& g) noexcept
{
    return o.window.IsEmpty() && OverlapsSource(o, g);
}

// --- naming what the machine turned out to have --------------------------------------------------------

using Caption = real::ChoiceText;

[[nodiscard]] Caption Named(std::wstring_view text) noexcept
{
    return Caption::Parse(text.substr(0, std::min(text.size(), Caption::Capacity))).value_or(Caption{});
}

[[nodiscard]] real::PanelList Started(std::wstring_view first, std::size_t chosen) noexcept
{
    return real::PanelList{ real::PanelList{}.choices.Push(Named(first)).value_or(real::ChoiceTexts{}), chosen };
}

[[nodiscard]] real::PanelList AdapterList(const Options& o, const real::AdapterList& adapters) noexcept
{
    static constexpr auto WithAdapters = [] [[nodiscard]] (real::PanelList list, const real::AdapterList& adapters) noexcept -> real::PanelList {
        const auto add = [&adapters](const real::PanelList& so, std::size_t i) { return real::PanelList{ so.choices.Push(Named(adapters.At(i).name.Get())).value_or(so.choices), so.chosen }; };
        return std::ranges::fold_left(std::views::iota(std::size_t{ 0 }, adapters.Size()), list, add);
    };
    return WithAdapters(Started(L"First NVIDIA adapter", o.adapter.has_value() ? o.adapter->Get() + 1u : 0u), adapters);
}

[[nodiscard]] std::optional<interior::MonitorHandle> FollowedWindow(const Base& b) noexcept
{
    static constexpr auto CapturesOneWindow = [] [[nodiscard]] (const Base& b) noexcept -> bool { return !b.options.window.IsEmpty() && !b.geometry.source.IsEmpty(); };
    if (!CapturesOneWindow(b))
        return std::nullopt;
    return b.geometry.source.At(0).handle;
}

[[nodiscard]] Result<real::RealEnvironment, Error> Environment(const Console& console, const Base& b, Devices d, const SessionPlan& plan, const real::ControlPanel* panel) noexcept
{
    static constexpr auto CreatedWindow = [] [[nodiscard]] (const Console& console, const Base& b) noexcept -> Result<real::OutputWindow, Error> {
        static constexpr auto WarnFeedback = [] [[nodiscard]] (const Console& console, const Options& o, const Geometry& g) noexcept -> Status<Error> {
            static constexpr auto FeedsBack = [] [[nodiscard]] (const Options& o, const Geometry& g) noexcept -> bool { return !o.displayAffinity && OverlapsSource(o, g); };
            if (!FeedsBack(o, g))
                return {};
            return Log(console, LogLevel::Warn, "The output window is not excluded from capture and overlaps the source: expect a feedback loop");
        };

        static constexpr auto WindowSettingsOf = [] [[nodiscard]] (const Options& o) noexcept -> real::WindowSettings {
            // The overlay is above everything when it covers a monitor. Over one window it stays just above that
            // window instead, so a window raised in front of the one being worked on covers the overlay too.
            return real::WindowSettings{ .topmost = o.window.IsEmpty(), .clickThrough = o.clickThrough, .excludeFromCapture = o.displayAffinity, .redirectionBitmap = o.redirectionBitmap };
        };
        return WarnFeedback(console, b.options, b.geometry)
            .and_then([&] { return real::CreateOutputWindow(b.geometry.targetRect, WindowSettingsOf(b.options)); })
            .and_then([](real::OutputWindow window) { return real::RegisterHotkeys(window).transform([&window] { return std::move(window); }); });
    };

    static constexpr auto SettingsOf = [] [[nodiscard]] (const Base& b, const SessionPlan& plan) noexcept -> real::EnvironmentSettings {
        static constexpr auto AsksToBeLeftOut = [] [[nodiscard]] (const Options& o, const Geometry& g) noexcept -> bool { return o.excludeOwnWindows && IsInItsOwnCapture(o, g); };
        const Options& o = b.options;
        return real::EnvironmentSettings{ .surface = interior::SurfaceSettings{ o.cursor, o.captureBorder, o.displayAffinity, o.clickThrough, o.logLevel },
                                          .captureCursor = plan.captureCursor,
                                          .followed = FollowedWindow(b),
                                          .asksToBeLeftOut = AsksToBeLeftOut(o, b.geometry),
                                          .outsideTheSource = !OverlapsSource(o, b.geometry),
                                          .source = b.geometry.sourceRect };
    };
    return CreatedWindow(console, b).and_then([&](real::OutputWindow window) {
        return real::CreateEnvironment(std::move(d.held), std::move(d.device), std::move(d.runtime), plan, b.geometry, std::move(window), panel, SettingsOf(b, plan), b.options, console);
    });
}

[[nodiscard]] Result<interior::FrameNumber, Error> Settled(real::RealEnvironment& env, const Result<app::SessionOutcome, Error>& outcome) noexcept
{
    const Result<interior::FenceValue, Error> idle = real::WaitIdle(env.Devices().device, env.LastFence());
    if (!outcome.has_value())
        return Fail(outcome.error());
    return idle.transform([&outcome](interior::FenceValue) { return outcome->finalState.number; });
}

// The panel belongs to the operator rather than to any one session, so it is made once and kept while
// sessions are torn down and built again underneath it. Its place on screen, its page and its notice stay.
struct PanelHolder
{
    std::optional<real::ControlPanel> panel;
};

[[nodiscard]] const real::ControlPanel* Borrowed(const PanelHolder& held) noexcept
{
    return held.panel.has_value() ? &*held.panel : nullptr;
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

[[nodiscard]] Result<Ended, Error> RunOnce(const Console& console, const Options& options, PanelHolder& held) noexcept
{
    static constexpr auto Logged = [] [[nodiscard]] (const Console& console, const Explanation& explanation) noexcept -> Error {
        return Log(console, LogLevel::Error, explanation.text).error_or(explanation.error);
    };

    static constexpr auto ResolveBase = [] [[nodiscard]] (const Console& console, const Options& options) noexcept -> Result<Base, Error> {
        static constexpr auto BasedOn = [] [[nodiscard]] (const Console& console, const Options& options, const interior::DirectoryPath& directory, const interior::MonitorList& monitors,
                                                          const interior::MonitorList& sources) noexcept -> Result<Base, Error> {
            static constexpr auto ResolvedGeometry = [] [[nodiscard]] (const Console& console, const Options& options, const interior::MonitorList& monitors) noexcept -> Result<Geometry, Error> {
                static constexpr auto ExplainMonitor = [] [[nodiscard]] (interior::MonitorError error) noexcept -> Explanation {
                    static constexpr auto FromMonitorError = [] [[nodiscard]] (interior::MonitorError error) noexcept -> Error {
                        return Error{ real::ApiCall::ResolveGeometry, static_cast<std::uint32_t>(error) };
                    };
                    return Explanation{ interior::Describe(error), FromMonitorError(error) };
                };
                return interior::ResolveGeometry(monitors, options).transform_error([&console](interior::MonitorError e) { return Logged(console, ExplainMonitor(e)); });
            };

            static constexpr auto LogGeometry = [] [[nodiscard]] (const Console& console, const Geometry& g) noexcept -> Status<Error> {
                const Line line =
                    infra::Formatted<kLineCapacity>("Source {}x{} at ({}, {}); output {}x{} at ({}, {})", g.sourceExtent.width.Get(), g.sourceExtent.height.Get(), g.sourceRect.Left().Get(),
                                                    g.sourceRect.Top().Get(), g.targetExtent.width.Get(), g.targetExtent.height.Get(), g.targetRect.Left().Get(), g.targetRect.Top().Get());
                return Log(console, LogLevel::Info, line.Get());
            };
            return ResolvedGeometry(console, options, sources).and_then([&](const Geometry& g) {
                return LogGeometry(console, g).transform([&] { return Base{ options, directory, g, interior::Ordered(monitors) }; });
            });
        };

        // One window, if one was asked for, standing in for the monitor list: the geometry then works out to that
        // window's own rectangle, and the capture opens an item for the window rather than for a monitor.
        static constexpr auto SourcesFor = [] [[nodiscard]] (const Options& o, const interior::MonitorList& monitors) noexcept -> Result<interior::MonitorList, Error> {
            if (o.window.IsEmpty())
                return monitors;
            return real::FindWindowNamed(o.window).and_then([](const interior::MonitorInfo& found) {
                return interior::MonitorList{}.Push(found).transform_error([](infra::CapacityExceeded) { return Error{ real::ApiCall::WindowNotFound, 2 }; });
            });
        };

        static constexpr auto SelectionFor = [] [[nodiscard]] (const Options& o) noexcept -> Options {
            // A single window is captured whole, so "every source" is what the geometry is asked for.
            static constexpr auto AsWholeSource = [] [[nodiscard]] (const Options& o) noexcept -> Options {
                Options whole = o; // WAIVER(R2): a copy with one answer replaced, made once and read from there on.
                whole.source = interior::SourceSelection{ interior::MonitorSelectionKind::All, interior::RequestedMonitorTag::Parse(0) };
                return whole;
            };
            return o.window.IsEmpty() ? o : AsWholeSource(o);
        };
        return ExecutableDirectory().and_then([&](const interior::DirectoryPath& directory) {
            return real::EnumerateMonitors().and_then([&](const interior::MonitorList& monitors) {
                return SourcesFor(options, monitors).and_then([&](const interior::MonitorList& sources) { return BasedOn(console, SelectionFor(options), directory, monitors, sources); });
            });
        });
    };

    static constexpr auto CreateDevices = [] [[nodiscard]] (const Console& console, const Base& b) noexcept -> Result<Devices, Error> {
        static constexpr auto WantsNgx = [] [[nodiscard]] (const Options& o, const Geometry& g) noexcept -> bool {
            return o.neuralRendering || interior::WantsSuperResolution(o, g.sourceExtent, g.targetExtent);
        };

        static constexpr auto RequireNvidia = [] [[nodiscard]] (const real::GpuDevice& device, bool wanted) noexcept -> Status<Error> {
            static constexpr auto LacksNvidia = [] [[nodiscard]] (const real::GpuDevice& device, bool wanted) noexcept -> bool { return wanted && !device.nvidia; };
            if (LacksNvidia(device, wanted))
                return Fail(Error{ real::ApiCall::NotNvidia, 0 });
            return {};
        };

        static constexpr auto NgxSettingsOf = [] [[nodiscard]] (const Options& o, const interior::DirectoryPath& executableDirectory) noexcept -> real::NgxSettings {
            static constexpr auto DataPathOf = [] [[nodiscard]] (const Options& o, const interior::DirectoryPath& executableDirectory) noexcept -> interior::DirectoryPath {
                return o.appDataPath.IsEmpty() ? executableDirectory : o.appDataPath;
            };
            return real::NgxSettings{ o.ngxAppId, o.ngxProjectId, DataPathOf(o, executableDirectory), executableDirectory, o.ngxPath, o.ngxLogLevel, o.indicator, o.cubinCache };
        };

        static constexpr auto DriverText = [] [[nodiscard]] (const real::GpuDevice& device) noexcept -> Line {
            static constexpr auto VersionText = [] [[nodiscard]] (bool nvidia, const interior::DriverVersion& v) noexcept -> Line {
                static constexpr auto NvidiaDriverText = [] [[nodiscard]] (const interior::DriverVersion& v) noexcept -> Line {
                    const std::uint32_t number = interior::NvidiaDriverNumber(v);
                    return infra::Formatted<kLineCapacity>("NVIDIA driver {}.{:02} ({}.{}.{}.{})", interior::NvidiaDriverMajor(number), interior::NvidiaDriverMinor(number), v.product, v.version,
                                                           v.subVersion, v.build);
                };
                if (!nvidia)
                    return infra::Formatted<kLineCapacity>("driver {}.{}.{}.{}", v.product, v.version, v.subVersion, v.build);
                return NvidiaDriverText(v);
            };
            if (!device.driverVersion.has_value())
                return infra::Formatted<kLineCapacity>("driver version unknown");
            return VersionText(device.nvidia, *device.driverVersion);
        };

        static constexpr auto LogAdapter = [] [[nodiscard]] (const Console& console, const real::GpuDevice& device) noexcept -> Status<Error> {
            const std::array<char, interior::AdapterName::Capacity + 1> name = infra::NarrowedChars<interior::AdapterName::Capacity + 1>(device.name.Get());
            return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("Direct3D 12 device on '{}', {}", name.data(), DriverText(device).Get()).Get());
        };

        static constexpr auto WithRuntime = [] [[nodiscard]] (const Console& console, const Base& b, real::GpuDevice device, const real::NgxSettings& settings,
                                                              bool wantsNgx) noexcept -> Result<Devices, Error> {
            static constexpr auto OptionalRuntime = [] [[nodiscard]] (const Console& console, const real::GpuDevice& device, const Options& o, const real::NgxSettings& settings,
                                                                      bool wanted) noexcept -> Result<std::optional<real::NgxRuntime>, Error> {
                static constexpr auto LogRequirements = [] [[nodiscard]] (const Console& console, const real::GpuDevice& device, const real::NgxSettings& settings) noexcept -> Status<Error> {
                    static constexpr auto LogRequirement = [] [[nodiscard]] (const Console& console, const real::GpuDevice& device, const real::NgxSettings& settings, NVSDK_NGX_Feature feature,
                                                                             std::string_view name) noexcept -> Status<Error> {
                        static constexpr auto RequirementText = [] [[nodiscard]] (const real::Requirement& r) noexcept -> Line {
                            static constexpr auto SupportText = [] [[nodiscard]] (std::uint32_t mask) noexcept -> Line {
                                if (mask == 0)
                                    return infra::Formatted<kLineCapacity>("supported");
                                return infra::Formatted<kLineCapacity>("not supported (mask 0x{:X}: 1 check missing, 2 driver too old, 4 adapter, 8 OS, 16 unimplemented)", mask);
                            };
                            if (NVSDK_NGX_FAILED(r.result))
                                return infra::Formatted<kLineCapacity>("requirements query failed with 0x{:08X}", static_cast<unsigned int>(r.result));
                            return SupportText(r.supportMask);
                        };
                        const real::Requirement requirement = real::RequirementOf(device, settings, feature);
                        return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("NGX feature {} ({}): {}", static_cast<int>(feature), name, RequirementText(requirement).Get()).Get());
                    };
                    return LogRequirement(console, device, settings, NVSDK_NGX_Feature_SuperSampling, "DLSS Super Resolution").and_then([&] {
                        return LogRequirement(console, device, settings, real::kNeuralRenderingFeature, "DLSS 5 Neural Rendering");
                    });
                };

                static constexpr auto RequireNeuralRendering = [] [[nodiscard]] (const Console& console, const real::GpuDevice& device, const real::NgxSettings& settings,
                                                                                 const real::NgxRuntime& runtime, bool neuralRendering) noexcept -> Status<Error> {
                    // The loader's capability block names DLSSNR.Available only when it can build feature 18 itself.
                    static constexpr auto CheckNeuralRendering = [] [[nodiscard]] (const Console& console, const real::GpuDevice& device, const real::NgxSettings& settings,
                                                                                   std::optional<std::uint32_t> available) noexcept -> Status<Error> {
                        // The driver's own number travels with the error, so the message names it; nothing means it was not read.
                        static constexpr auto DriverNumberOf = [] [[nodiscard]] (const real::GpuDevice& device) noexcept -> std::uint32_t {
                            if (!device.nvidia || !device.driverVersion.has_value())
                                return 0;
                            return interior::NvidiaDriverNumber(*device.driverVersion);
                        };

                        static constexpr auto MissingAvailabilityText = [] [[nodiscard]] (const real::GpuDevice& device) noexcept -> Line {
                            static constexpr auto RequiredDriverText = [] [[nodiscard]] () noexcept -> Line {
                                return infra::Formatted<kLineCapacity>("{}.{:02}", interior::NvidiaDriverMajor(interior::kFirstNeuralRenderingDriver),
                                                                       interior::NvidiaDriverMinor(interior::kFirstNeuralRenderingDriver));
                            };
                            return infra::Formatted<kLineCapacity>(
                                "DLSS 5 Neural Rendering is not offered by this driver's NGX loader (it has no DLSSNR.Available); NVIDIA driver {} or newer is required and this is {}",
                                RequiredDriverText().Get(), DriverText(device).Get());
                        };

                        static constexpr auto CheckAvailabilityValue = [] [[nodiscard]] (const Console& console, const real::NgxSettings& settings, std::uint32_t available) noexcept -> Status<Error> {
                            // The loader builds feature 18 from nvngx_dlssnr.dll in the executable folder or --ngx-path; a zero
                            // with no such file means there is nothing to build from, a zero with the file means it was rejected.
                            // Each is its own error, so the message the operator sees says which; the log adds where the file was.
                            static constexpr auto ZeroAvailabilityText = [] [[nodiscard]] (const std::optional<interior::DirectoryPath>& model) noexcept -> Line {
                                if (!model.has_value())
                                    return infra::Formatted<kLineCapacity>("the NGX loader reports DLSSNR.Available = 0 and there is no nvngx_dlssnr.dll next to " DSCREEN_FILE_STEM ".exe or in "
                                                                           "--ngx-path");
                                const std::array<char, interior::DirectoryPath::Capacity + 1> directory = infra::NarrowedChars<interior::DirectoryPath::Capacity + 1>(model->Get());
                                return infra::Formatted<kLineCapacity>("the NGX loader reports DLSSNR.Available = 0 although nvngx_dlssnr.dll is in {}; the loader rejected that build",
                                                                       directory.data());
                            };

                            static constexpr auto ZeroAvailabilityError = [] [[nodiscard]] (const std::optional<interior::DirectoryPath>& model) noexcept -> Error {
                                return Error{ model.has_value() ? real::ApiCall::NgxNeuralRenderingUnavailable : real::ApiCall::NgxModelMissing, 0 };
                            };
                            if (available == 0)
                                return Fail(Logged(console, Explanation{ ZeroAvailabilityText(real::NeuralRenderingModelLocation(settings)).Get(),
                                                                         ZeroAvailabilityError(real::NeuralRenderingModelLocation(settings)) }));
                            return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("DLSSNR.Available = {}", available).Get());
                        };
                        if (!available.has_value())
                            return Fail(Logged(console, Explanation{ MissingAvailabilityText(device).Get(), Error{ real::ApiCall::NgxDriverTooOld, DriverNumberOf(device) } }));
                        return CheckAvailabilityValue(console, settings, *available);
                    };
                    if (!neuralRendering)
                        return {};
                    return CheckNeuralRendering(console, device, settings, real::NeuralRenderingAvailability(runtime));
                };
                if (!wanted)
                    return std::optional<real::NgxRuntime>{};
                return LogRequirements(console, device, settings).and_then([&] { return real::CreateNgxRuntime(device, settings); }).and_then([&](real::NgxRuntime runtime) {
                    return RequireNeuralRendering(console, device, settings, runtime, o.neuralRendering).transform([&runtime] { return std::optional<real::NgxRuntime>{ std::move(runtime) }; });
                });
            };

            // A model is a DLL the loader picks up by name from a folder anyone may write to, so what is found there
            // is checked and then held open for the life of the session, whether or not the session will use it: the
            // loader may open it all the same. A missing file is left to the loader, which says so better: neural
            // rendering stops without one, and super resolution has the driver's own copy.
            static constexpr auto TrustedModel = [] [[nodiscard]] (const Console& console, const std::optional<interior::FilePath>& file, real::ModelKind kind,
                                                                   std::string_view name) noexcept -> Result<std::optional<real::TrustedFile>, Error> {
                // Only the DLSS 5 model's product name is judged; the super resolution file's is only said.
                static constexpr auto ProductOf = [] [[nodiscard]] (real::ModelKind kind) noexcept -> std::optional<std::wstring_view> {
                    if (kind != real::ModelKind::NeuralRendering)
                        return std::nullopt;
                    return real::kNeuralRenderingProduct;
                };

                // A file that passed the signature check is used whatever it calls itself; what it calls itself is said,
                // and said as a warning when it is not what was expected, since it may then be some other file of NVIDIA's.
                static constexpr auto Reported = [] [[nodiscard]] (const Console& console, const real::TrustedFile& model, std::string_view name,
                                                                   std::optional<std::wstring_view> product) noexcept -> Status<Error> {
                    static constexpr auto Warned = [] [[nodiscard]] (const Console& console, std::string_view name, const char* called, std::wstring_view wanted) noexcept -> Status<Error> {
                        const std::array<char, real::ProductName::Capacity + 1> expected = infra::NarrowedChars<real::ProductName::Capacity + 1>(wanted);
                        return Log(console, LogLevel::Warn,
                                   infra::Formatted<kLineCapacity>("{} is signed by NVIDIA but calls its product '{}' rather than '{}', so it may not be the DLSS 5 model; it is used anyway", name,
                                                                   called, expected.data())
                                       .Get());
                    };
                    const std::array<char, real::ProductName::Capacity + 1> called = infra::NarrowedChars<real::ProductName::Capacity + 1>(model.product.Get());
                    if (product.has_value() && model.product.Get() != *product)
                        return Warned(console, name, called.data(), *product);
                    return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("{} is signed by NVIDIA and calls its product '{}'", name, called.data()).Get());
                };

                static constexpr auto Checked = [] [[nodiscard]] (const Console& console, const interior::FilePath& file, real::ModelKind kind,
                                                                  std::string_view name) noexcept -> Result<std::optional<real::TrustedFile>, Error> {
                    return real::OpenTrusted(file, kind).and_then([&console, kind, name](real::TrustedFile model) {
                        return Reported(console, model, name, ProductOf(kind)).transform([&model] { return std::optional<real::TrustedFile>{ std::move(model) }; });
                    });
                };
                if (!file.has_value())
                    return std::optional<real::TrustedFile>{};
                return Checked(console, *file, kind, name);
            };

            static constexpr auto ModelToCheck = [] [[nodiscard]] (const std::optional<interior::FilePath>& found, bool wanted) noexcept -> std::optional<interior::FilePath> {
                return wanted ? found : std::nullopt;
            };

            // Every loadable file that is there, checked in turn and held in its slot, whenever NGX is started at all.
            static constexpr auto TrustedFiles = [] [[nodiscard]] (const Console& console, const real::LoadableFiles& files, bool wanted) noexcept -> Result<real::HeldFiles, Error> {
                return std::ranges::fold_left(
                    std::views::iota(std::size_t{ 0 }, real::kLoadableCount), Result<real::HeldFiles, Error>{ real::HeldFiles{} },
                    [&console, &files, wanted](Result<real::HeldFiles, Error> held, std::size_t i) {
                        return std::move(held).and_then([&console, &files, wanted, i](real::HeldFiles slots) {
                            return TrustedModel(console, ModelToCheck(files[i], wanted), kLoadables[i].kind, kLoadables[i].name).transform([&slots, i](std::optional<real::TrustedFile> file) {
                                slots[i] = std::move(file); // WAIVER(R2): each slot is filled once, in order, by the one file it is for.
                                return std::move(slots);
                            });
                        });
                    });
            };
            return TrustedFiles(console, real::LoadableFilesOf(settings), wantsNgx).and_then([&](real::HeldFiles held) {
                return OptionalRuntime(console, device, b.options, settings, wantsNgx).transform([&](std::optional<real::NgxRuntime> runtime) {
                    return Devices{ std::move(device), std::move(runtime), std::move(held) };
                });
            });
        };
        const bool wantsNgx = WantsNgx(b.options, b.geometry);
        return real::CreateGpuDevice(real::DeviceSettings{ b.options.debugLayer, b.options.adapter }).and_then([&](real::GpuDevice device) {
            return LogAdapter(console, device).and_then([&] { return RequireNvidia(device, wantsNgx); }).and_then([&] {
                return WithRuntime(console, b, std::move(device), NgxSettingsOf(b.options, b.executableDirectory), wantsNgx);
            });
        });
    };

    static constexpr auto Offered = [] [[nodiscard]] (const Base& b, const Devices& d) noexcept -> Base {
        // A session that asked for super resolution and cannot have it runs without it rather than stopping.
        // Nothing else in the picture depends on it, and the panel greys the choice and says why.
        static constexpr auto WithoutSuperResolution = [] [[nodiscard]] (const Options& o) noexcept -> Options {
            Options without = o; // WAIVER(R2): a copy with one answer replaced, made once and read from there on.
            without.sr = interior::SrMode::Off;
            return without;
        };
        if (OffersSuperResolution(d))
            return b;
        return Base{ WithoutSuperResolution(b.options), b.executableDirectory, b.geometry, b.monitors };
    };

    static constexpr auto Planned = [] [[nodiscard]] (const Console& console, const Base& b, const Devices& d) noexcept -> Result<SessionPlan, Error> {
        static constexpr auto ExplainPlan = [] [[nodiscard]] (interior::PlanError error) noexcept -> Explanation { return Explanation{ interior::Describe(error), FromPlanError(error) }; };

        static constexpr auto TableFor = [] [[nodiscard]] (const std::optional<real::NgxRuntime>& runtime, const interior::Extent& target) noexcept -> interior::QualityTable {
            if (!runtime.has_value())
                return interior::QualityTable{};
            return real::QualityTableFor(*runtime, target);
        };

        static constexpr auto RequireOpticalFlowBuild = [] [[nodiscard]] (const Options& o) noexcept -> Status<Error> {
            static constexpr auto LacksOpticalFlowBuild = [] [[nodiscard]] (const Options& o) noexcept -> bool { return !kHasOpticalFlow && o.motion == interior::MotionBackend::NvOpticalFlow; };
            if (LacksOpticalFlowBuild(o))
                return Fail(Error{ real::ApiCall::OpticalFlowUnavailable, 0 });
            return {};
        };
        return RequireOpticalFlowBuild(b.options).and_then([&] {
            return interior::PlanSession(b.options, b.geometry, TableFor(d.runtime, b.geometry.targetExtent)).transform_error([&console](interior::PlanError e) {
                return Logged(console, ExplainPlan(e));
            });
        });
    };

    // One session, from the devices up. Everything it makes goes away when it returns, which is what lets the
    // next one be made differently; the panel is the operator's and is not part of any of it.
    static constexpr auto Staged = [] [[nodiscard]] (const Console& console, const Base& b, Devices d, const SessionPlan& plan, PanelHolder& held) noexcept -> Result<Ended, Error> {
        static constexpr auto LogPlan = [] [[nodiscard]] (const Console& console, const SessionPlan& p) noexcept -> Status<Error> {
            static constexpr auto MotionName = [] [[nodiscard]] (interior::MotionBackend motion) noexcept -> std::string_view {
                switch (motion)
                {
                case interior::MotionBackend::BuiltIn: return "block matching";
                case interior::MotionBackend::NvOpticalFlow: return "NVIDIA Optical Flow";
                case interior::MotionBackend::None: return "zero motion";
                }
                return "";
            };

            static constexpr auto SuperResolutionText = [] [[nodiscard]] (const SessionPlan& p) noexcept -> Line {
                if (!p.superResolution.has_value())
                    return infra::Formatted<kLineCapacity>("no super resolution");
                return infra::Formatted<kLineCapacity>("DLSS {} {}x{} -> {}x{}", interior::Describe(p.superResolution->quality), p.superResolution->input.width.Get(),
                                                       p.superResolution->input.height.Get(), p.superResolution->output.width.Get(), p.superResolution->output.height.Get());
            };

            static constexpr auto NeuralRenderingText = [] [[nodiscard]] (const SessionPlan& p) noexcept -> Line {
                static constexpr auto PassesText = [] [[nodiscard]] (const SessionPlan& p) noexcept -> Line {
                    if (p.passes.Get() == 1)
                        return infra::Formatted<kLineCapacity>("DLSS 5 Neural Rendering");
                    return infra::Formatted<kLineCapacity>("DLSS 5 Neural Rendering, {} passes", p.passes.Get());
                };
                return p.neuralRendering ? PassesText(p) : infra::Formatted<kLineCapacity>("neural rendering off");
            };

            static constexpr auto LogTuningIf = [] [[nodiscard]] (const Console& console, const SessionPlan& p) noexcept -> Status<Error> {
                static constexpr auto LogTuning = [] [[nodiscard]] (const Console& console, const interior::NrTuning& t) noexcept -> Status<Error> {
                    const Line line = infra::Formatted<kLineCapacity>(
                        "Neural rendering tuning: preset {}, intensity {:.2f}, style {}, local structure {:.2f}, local tone {:.2f}, skin {:.2f}, auto mask {}, UI correction {}", t.preset.Get(),
                        t.intensity.Get(), interior::StyleCode(t.style), t.localStructure.Get(), t.localTone.Get(), t.skinStructure.Get(), t.autoMask, t.uiCorrection);
                    return Log(console, LogLevel::Info, line.Get());
                };
                if (!p.neuralRendering)
                    return {};
                return LogTuning(console, p.tuning);
            };
            const Line line = infra::Formatted<kLineCapacity>("Pipeline: capture {}x{} -> {} -> {} -> {} -> present {}x{}", p.source.width.Get(), p.source.height.Get(), MotionName(p.motion),
                                                              SuperResolutionText(p).Get(), NeuralRenderingText(p).Get(), p.target.width.Get(), p.target.height.Get());
            return Log(console, LogLevel::Info, line.Get()).and_then([&] { return LogTuningIf(console, p); });
        };

        static constexpr auto FindingsFor = [] [[nodiscard]] (const Base& b, const Devices& d) noexcept -> real::PanelFindings {
            static constexpr auto ListsFor = [] [[nodiscard]] (const Base& b, const real::AdapterList& adapters, std::uint32_t presets) noexcept -> real::PanelLists {
                static constexpr auto WithMonitors = [] [[nodiscard]] (real::PanelList list, const interior::MonitorList& monitors) noexcept -> real::PanelList {
                    static constexpr auto MonitorCaption = [] [[nodiscard]] (std::size_t index, const interior::MonitorInfo& m) noexcept -> Caption {
                        const infra::BoundedString<char, Caption::Capacity> line = infra::Formatted<Caption::Capacity>("{}: {}x{}{}", index, m.rect.Right().Get() - m.rect.Left().Get(),
                                                                                                                       m.rect.Bottom().Get() - m.rect.Top().Get(), m.primary ? " primary" : "");
                        return Named(infra::WidenedChars<Caption::Capacity + 1>(line.Get()).data());
                    };
                    const auto add = [&monitors](const real::PanelList& so, std::size_t i) {
                        return real::PanelList{ so.choices.Push(MonitorCaption(i, monitors.At(i))).value_or(so.choices), so.chosen };
                    };
                    return std::ranges::fold_left(std::views::iota(std::size_t{ 0 }, monitors.Size()), list, add);
                };

                static constexpr auto SourceList = [] [[nodiscard]] (const Options& o, const interior::MonitorList& monitors) noexcept -> real::PanelList {
                    // Which monitor the session is capturing: the two answers that name none come first, so a named one sits
                    // at its own index plus two.
                    static constexpr auto SourceChoice = [] [[nodiscard]] (const Options& o) noexcept -> std::size_t {
                        if (o.source.kind != interior::MonitorSelectionKind::Index)
                            return static_cast<std::size_t>(o.source.kind);
                        return o.source.index.Get() + 2u;
                    };
                    const real::PanelList primary = Started(L"Primary monitor", SourceChoice(o));
                    const real::PanelList both{ primary.choices.Push(Named(L"All monitors")).value_or(primary.choices), primary.chosen };
                    return WithMonitors(both, monitors);
                };

                static constexpr auto TargetList = [] [[nodiscard]] (const Options& o, const interior::MonitorList& monitors) noexcept -> real::PanelList {
                    return WithMonitors(Started(L"Same as the source", o.target.has_value() ? o.target->Get() + 1u : 0u), monitors);
                };

                // The presets the model will admit to carrying. Nothing here leaves the choice off the panel entirely.
                static constexpr auto PresetList = [] [[nodiscard]] (const Options& o, std::uint32_t count) noexcept -> real::PanelList {
                    const auto add = [](const real::PanelList& so, std::uint32_t i) {
                        return real::PanelList{
                            so.choices.Push(Named(infra::WidenedChars<Caption::Capacity + 1>(infra::Formatted<Caption::Capacity>("Preset {}", i).Get()).data())).value_or(so.choices), so.chosen
                        };
                    };
                    return std::ranges::fold_left(std::views::iota(std::uint32_t{ 0 }, count), real::PanelList{ real::ChoiceTexts{}, o.tuning.preset.Get() }, add);
                };
                return real::PanelLists{ PresetList(b.options, presets), SourceList(b.options, b.monitors), TargetList(b.options, b.monitors), AdapterList(b.options, adapters) };
            };

            // The model names its presets or it does not. It does not, so far, and the panel then leaves the choice
            // out rather than offering numbers that all fall back to the single set of weights the model carries.
            static constexpr auto OfferedPresets = [] [[nodiscard]] (const Devices& d) noexcept -> std::uint32_t {
                if (!d.runtime.has_value())
                    return 0;
                return real::NeuralRenderingPresetCount(*d.runtime).value_or(0);
            };
            return real::PanelFindings{ .lists = ListsFor(b, real::UsableAdapters(d.device.factory.Get()), OfferedPresets(d)),
                                        .superResolution = OffersSuperResolution(d),
                                        .opticalFlow = kHasOpticalFlow,
                                        .modelAsNamed = ModelAsNamed(d),
                                        .captureFolder = interior::DefaultCaptureFolder(b.executableDirectory),
                                        .window = FollowedWindow(b) };
        };

        static constexpr auto HeldPanel = [] [[nodiscard]] (const Base& b, const SessionPlan& plan, const real::PanelFindings& findings,
                                                            PanelHolder& held) noexcept -> Result<const real::ControlPanel*, Error> {
            static constexpr auto AlreadyAnswered = [] [[nodiscard]] (const Base& b, const PanelHolder& held) noexcept -> bool { return held.panel.has_value() || !b.options.gui; };
            if (AlreadyAnswered(b, held))
                return Borrowed(held);
            return real::CreateControlPanel(b.options, interior::StartingLive(plan), plan.initialDisplay, findings).transform([&held](real::ControlPanel made) {
                held.panel = std::move(made);
                return &*held.panel;
            });
        };

        static constexpr auto Drive = [] [[nodiscard]] (const Console& console, const Base& b, const SessionPlan& plan, real::RealEnvironment& env) noexcept -> Result<Ended, Error> {
            // Nothing of ours can reach a capture of somewhere our windows are not, so neither way is needed there.
            static constexpr auto LogExclusion = [] [[nodiscard]] (const Console& console, const Base& b, bool excluding) noexcept -> Status<Error> {
                // Which of the two ways our own windows are being kept out of our own capture, since one of them also
                // keeps them out of everyone else's and is the reason the overlay cannot be screenshotted or recorded.
                static constexpr auto LogHiding = [] [[nodiscard]] (const Console& console, bool excluding) noexcept -> Status<Error> {
                    if (excluding)
                        return Log(console, LogLevel::Info, "Our windows are left out of our own capture by name, and are in everyone else's: screenshots and recordings hold them");
                    return Log(console, LogLevel::Info, "Our windows are hidden from every capture, screenshots included (--exclude-own-windows on asks for the other way)");
                };
                if (!OverlapsSource(b.options, b.geometry))
                    return Log(console, LogLevel::Info, "The output is not on what is being captured, so nothing of ours is hidden: screenshots and recordings hold both windows");
                return LogHiding(console, excluding);
            };
            // Said only where it cannot be had, since having it is what the setting is for and needs no remark.
            static constexpr auto LogBorder = [] [[nodiscard]] (const Console& console, bool controls) noexcept -> Status<Error> {
                if (controls)
                    return {};
                return Log(
                    console, LogLevel::Warn,
                    "This Windows has no setting for the capture border, which arrived in build 20348, so the system draws its own around what is captured and --capture-border cannot change it");
            };
            real::ShowOutputWindow(env.Window());
            return LogExclusion(console, b, env.Devices().capture.excludesOurWindows)
                .and_then([&] { return LogBorder(console, env.Devices().capture.controlsBorder); })
                .and_then([&] { return Log(console, LogLevel::Info, "Running. Hotkeys: Ctrl+Alt+Shift+O original/processed, Ctrl+Alt+Shift+C split view, Ctrl+Alt+Shift+Q quit"); })
                .and_then([&] { return Settled(env, app::RunSession<real::RealEnvironment, Error>(env, plan, interior::InitialFrameState(plan), kFrameLimit)); })
                .and_then([&](interior::FrameNumber frames) { return env.Finished().transform([frames] { return frames; }); })
                .transform([&](interior::FrameNumber frames) { return Ended{ .frames = frames, .again = env.Restart(b.options), .resized = env.Resized(), .abandoned = env.Abandoned() }; });
        };
        const real::PanelFindings findings = FindingsFor(b, d);
        return LogPlan(console, plan).and_then([&] { return HeldPanel(b, plan, findings, held); }).and_then([&](const real::ControlPanel* panel) {
            return Environment(console, b, std::move(d), plan, panel).and_then([&](real::RealEnvironment env) { return Drive(console, b, plan, env); });
        });
    };
    return ResolveBase(console, options).and_then([&](const Base& found) {
        return CreateDevices(console, found).and_then([&](Devices d) {
            const Base b = Offered(found, d);
            return Planned(console, b, d).and_then([&](const SessionPlan& plan) { return Staged(console, b, std::move(d), plan, held); });
        });
    });
}

// One session after another, each built from what the last one's panel asked for.
struct Cycle
{
    Options wanted;
    Result<Ended, Error> ended;
};

[[nodiscard]] bool AsksAgain(const Ended& ended) noexcept
{
    static constexpr auto AsksForTheSame = [] [[nodiscard]] (const Ended& ended) noexcept -> bool { return ended.resized || ended.abandoned; };
    return ended.again.has_value() || AsksForTheSame(ended);
}

constexpr std::string_view kWindowGoneText = "The window could not be captured, most likely because it has closed; going back to the monitor";

[[nodiscard]] Cycle Again(const Console& console, const Options& o, PanelHolder& held) noexcept
{
    return Cycle{ o, RunOnce(console, o, held) };
}

[[nodiscard]] Cycle Asked(const Console& console, const Cycle& c, PanelHolder& held) noexcept
{
    // What the start-up page describes is read back through the parser the command line uses, so a session
    // built from it is the session a fresh process would have built.
    static constexpr auto Reread = [] [[nodiscard]] (const interior::CommandLine& asked) noexcept -> Result<Options, Error> {
        static constexpr auto SplitArguments = [] [[nodiscard]] (const interior::CommandLine& line) noexcept -> Result<Arguments, Error> {
            // The splitter expects a program name in front, as a real command line has, so one is put there and the
            // answer starts after it, which is what this process does with the line it was given itself.
            static constexpr auto WithProgramName = [] [[nodiscard]] (const interior::CommandLine& line) noexcept -> std::array<wchar_t, interior::CommandLine::Capacity + 4> {
                std::array<wchar_t, interior::CommandLine::Capacity + 4> whole{}; // WAIVER(R2): a local buffer filled once, before use.
                const int written = ::_snwprintf_s(whole.data(), whole.size(), _TRUNCATE, L"x %.*s", static_cast<int>(line.Get().size()), line.Get().data());
                ENSURE(written > 0);
                return whole;
            };
            int argc = 0; // WAIVER(R2): the answer of one call, read once after it.
            wchar_t** argv = ::CommandLineToArgvW(WithProgramName(line).data(), &argc);
            if (argv == nullptr)
                return Fail(real::LastError(real::ApiCall::CommandLineToArgvW));
            return Collected(ArgumentBlock(argv), argc);
        };
        return SplitArguments(asked).and_then([](const Arguments& a) {
            return interior::ParseOptions(std::span<const std::wstring_view>(a.views.data(), a.count)).transform_error([](const interior::OptionsError&) {
                return Error{ real::ApiCall::CommandLineToArgvW, 1 };
            });
        });
    };

    static constexpr auto Continued = [] [[nodiscard]] (const Console& console, const Cycle& c, const Options& now, PanelHolder& held) noexcept -> Cycle {
        // The debug layer is the one setting a session cannot take back: Direct3D turns it on for the process and
        // there is no turning it off again. Everything else is made afresh below and needs no new process at all.
        static constexpr auto NeedsAFreshProcess = [] [[nodiscard]] (const Options& was, const Options& now) noexcept -> bool { return was.debugLayer && !now.debugLayer; };

        static constexpr auto Relaunching = [] [[nodiscard]] (const Cycle& c, const Options& now) noexcept -> Cycle {
            // The operator asked the start-up page for a session with different settings: this one starts it and
            // leaves. Nothing is inherited but the command line, so the new session is exactly what the page says.
            static constexpr auto Relaunch = [] [[nodiscard]] (const interior::CommandLine& arguments) noexcept -> Status<Error> {
                std::array<wchar_t, MAX_PATH> executable{}; // WAIVER(R2): a local buffer filled once, before use.
                if (IsPathFailure(::GetModuleFileNameW(nullptr, executable.data(), MAX_PATH)))
                    return Fail(real::LastError(real::ApiCall::GetModuleFileNameW));
                infra::BoundedString<wchar_t, kCommandCapacity> line =
                    infra::BoundedString<wchar_t, kCommandCapacity>::Parse(std::wstring_view(executable.data())).value_or(infra::BoundedString<wchar_t, kCommandCapacity>{});
                return real::StartProcess(line.Get(), arguments.Get());
            };
            const interior::FrameNumber frames = c.ended->frames;
            return Cycle{ now, Relaunch(*c.ended->again).transform([frames] { return Ended{ .frames = frames, .again = std::nullopt, .resized = false, .abandoned = false }; }) };
        };
        if (NeedsAFreshProcess(c.wanted, now))
            return Relaunching(c, now);
        return Again(console, now, held);
    };
    const Result<Options, Error> now = Reread(*c.ended->again);
    if (!now.has_value())
        return Cycle{ c.wanted, Fail(now.error()) };
    return Continued(console, c, *now, held);
}

[[nodiscard]] int Dispatch(const Result<Options, interior::OptionsError>& parsed) noexcept
{
    static constexpr auto UsageFailure = [] [[nodiscard]] (const interior::OptionsError& error) noexcept -> int {
        const Line line = infra::Formatted<kLineCapacity>("argument {}: {}\n", error.argument.Get(), interior::Describe(error.kind));
        return real::WriteText(stderr, interior::UsageText()).and_then([&line] { return real::WriteText(stderr, line.Get()); }).transform([] { return kExitUsage; }).value_or(kExitUsage);
    };

    static constexpr auto Serve = [] [[nodiscard]] (const Options& options) noexcept -> int {
        static constexpr auto PrintUsage = [] [[nodiscard]] () noexcept -> int { return real::WriteText(stdout, interior::UsageText()).transform([] { return kExitOk; }).value_or(kExitFailure); };

        // Printing to a console the operator does not have helps nobody, so these two ask for one.
        static constexpr auto ConsoleForReading = [] [[nodiscard]] (const Options& options) noexcept -> Result<Console, Error> {
            return real::OpenConsole(options.logLevel, options.logFile, interior::ConsoleMode::On);
        };

        static constexpr auto ServeOrRun = [] [[nodiscard]] (const Options& options) noexcept -> int {
            static constexpr auto ExitCodeOf = [] [[nodiscard]] (const Result<int, Error>& result) noexcept -> int {
                if (!result.has_value())
                    return ReportEarly(result.error());
                return *result;
            };

            static constexpr auto ListMonitors = [] [[nodiscard]] () noexcept -> Result<int, Error> {
                static constexpr auto PrintMonitors = [] [[nodiscard]] (const interior::MonitorList& monitors) noexcept -> Status<Error> {
                    static constexpr auto PrintMonitor = [] [[nodiscard]] (std::size_t index, const interior::MonitorInfo& m) noexcept -> Status<Error> {
                        const std::array<char, interior::DeviceName::Capacity + 1> name = infra::NarrowedChars<interior::DeviceName::Capacity + 1>(m.name.Get());
                        const Line line = infra::Formatted<kLineCapacity>("{}: {} {}x{} at ({}, {}){}\n", index, name.data(), m.rect.Right().Get() - m.rect.Left().Get(),
                                                                          m.rect.Bottom().Get() - m.rect.Top().Get(), m.rect.Left().Get(), m.rect.Top().Get(), m.primary ? " primary" : "");
                        return real::WriteText(stdout, line.Get());
                    };
                    const interior::MonitorList ordered = interior::Ordered(monitors);
                    return infra::ForEach(std::views::iota(std::size_t{ 0 }, ordered.Size()), Status<Error>{}, [&ordered](std::size_t i) { return PrintMonitor(i, ordered.At(i)); });
                };
                return real::SetDpiAwareness().and_then(real::EnumerateMonitors).and_then(PrintMonitors).transform([] { return kExitOk; });
            };

            static constexpr auto RunWithConsole = [] [[nodiscard]] (const Options& options) noexcept -> int {
                static constexpr auto Run = [] [[nodiscard]] (const Console& console, const Options& options) noexcept -> Result<interior::FrameNumber, Error> {
                    // Settled once for the whole program: how the process reads the display's scaling, and which apartment it
                    // has. Asking for the scaling twice is refused outright, with an access denied that is nothing of the sort.
                    static constexpr auto PrepareProcess = [] [[nodiscard]] () noexcept -> Status<Error> {
                        return real::SetDpiAwareness().and_then(real::InitializeRuntime).and_then(real::RequireCaptureSupport);
                    };

                    // A loop rather than one session calling the next, so asking for a hundred of them costs a hundred
                    // sessions and not a hundred stack frames.
                    static constexpr auto Sessions = [] [[nodiscard]] (const Console& console, const Options& options) noexcept -> Result<interior::FrameNumber, Error> {
                        // A session built on one window can fail outright because that window went away while it was being
                        // built: nothing can be captured from a window that is closing. The monitor is what is left.
                        static constexpr auto FellWithAWindow = [] [[nodiscard]] (const Cycle& c) noexcept -> bool { return !c.ended.has_value() && !c.wanted.window.IsEmpty(); };

                        static constexpr auto Continues = [] [[nodiscard]] (const Cycle& c) noexcept -> bool {
                            static constexpr auto AsksAgainIfEnded = [] [[nodiscard]] (const Cycle& c) noexcept -> bool { return c.ended.has_value() && AsksAgain(*c.ended); };
                            return FellWithAWindow(c) || AsksAgainIfEnded(c);
                        };

                        static constexpr auto Next = [] [[nodiscard]] (const Console& console, const Cycle& c, PanelHolder& held) noexcept -> Cycle {
                            // The window a session was following went away, so the next one is not given one: the source that session
                            // already carries names the monitor, and that is where the model goes back to.
                            static constexpr auto WithoutWindow = [] [[nodiscard]] (const Options& o) noexcept -> Options {
                                Options next = o; // WAIVER(R2): a copy with one answer replaced, read once after it.
                                next.window = interior::WindowTitle{};
                                return next;
                            };

                            // A window that changed size asks for the settings it already had: the same window, measured again.
                            static constexpr auto Finished = [] [[nodiscard]] (const Console& console, const Cycle& c, PanelHolder& held) noexcept -> Cycle {
                                static constexpr auto WantedNext = [] [[nodiscard]] (const Cycle& c) noexcept -> Options { return c.ended->abandoned ? WithoutWindow(c.wanted) : c.wanted; };
                                if (!c.ended->again.has_value())
                                    return Again(console, WantedNext(c), held);
                                return Asked(console, c, held);
                            };

                            // The crosshair is emptied along with the session, so what the panel shows and what is being worked on
                            // go on agreeing.
                            static constexpr auto Retried = [] [[nodiscard]] (const Console& console, const Cycle& c, PanelHolder& held) noexcept -> Cycle {
                                static constexpr auto LetGoOfPickedWindow = [](const PanelHolder& held) noexcept -> void {
                                    if (Borrowed(held) != nullptr)
                                        real::ReleaseWindow(*Borrowed(held));
                                };
                                const Options next = WithoutWindow(c.wanted);
                                LetGoOfPickedWindow(held);
                                return Cycle{ next, Log(console, LogLevel::Warn, kWindowGoneText).and_then([&] { return RunOnce(console, next, held); }) };
                            };
                            if (FellWithAWindow(c))
                                return Retried(console, c, held);
                            return Finished(console, c, held);
                        };
                        PanelHolder held{};                                      // WAIVER(R2): the panel outlives the sessions, made once when the first asks for it.
                        Cycle cycle{ options, RunOnce(console, options, held) }; // WAIVER(R2): one session at a time, replaced whole by the next.
                        while (Continues(cycle))                                 // WAIVER(R2): one turn of the loop is one session.
                            cycle = Next(console, cycle, held);
                        return cycle.ended.transform([](const Ended& e) { return e.frames; });
                    };
                    return PrepareProcess().and_then([&] { return Sessions(console, options); });
                };

                static constexpr auto Finish = [] [[nodiscard]] (const Console& console, const Result<interior::FrameNumber, Error>& result) noexcept -> int {
                    static constexpr auto Failed = [] [[nodiscard]] (const Console& console, const Error& error) noexcept -> int {
                        const real::ErrorText text = real::Describe(error);
                        if (!console.attached)
                            real::ShowMessage(text.Get());
                        return Log(console, LogLevel::Error, text.Get()).transform([] { return kExitFailure; }).value_or(kExitFailure);
                    };
                    if (!result.has_value())
                        return Failed(console, result.error());
                    return Log(console, LogLevel::Info, infra::Formatted<kLineCapacity>("Stopped after {} frames", result->Get()).Get()).transform([] { return kExitOk; }).value_or(kExitFailure);
                };
                const Result<Console, Error> console = real::OpenConsole(options.logLevel, options.logFile, options.console);
                if (!console.has_value())
                    return ReportEarly(console.error());
                real::NoteExclusionsTo(options.exclusionLog);
                return Finish(*console, Run(*console, options));
            };
            if (!options.listMonitors)
                return RunWithConsole(options);
            return ConsoleForReading(options).transform([](const Console&) { return ExitCodeOf(ListMonitors()); }).value_or(kExitFailure);
        };
        if (!options.showHelp)
            return ServeOrRun(options);
        return ConsoleForReading(options).transform([](const Console&) { return PrintUsage(); }).value_or(kExitFailure);
    };
    if (!parsed.has_value())
        return UsageFailure(parsed.error());
    return Serve(*parsed);
}

} // namespace

int main()
{
    // Before anything that could load a library: from here on a name found in the system folder is taken from there.
    const infra::Status<real::Error> loading = real::PreferSystemLibraries();
    if (!loading.has_value())
        return ReportEarly(loading.error());
    const infra::Result<Arguments, real::Error> arguments = ReadArguments();
    if (!arguments.has_value())
        return ReportEarly(arguments.error());
    return Dispatch(interior::ParseOptions(std::span<const std::wstring_view>(arguments->views.data(), arguments->count)));
}
