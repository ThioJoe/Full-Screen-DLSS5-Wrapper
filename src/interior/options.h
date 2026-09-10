#pragma once
#include "infrastructure/bounded_vector.h"
#include "interior/enums.h"
#include "interior/units.h"

#include <optional>
#include <span>
#include <string_view>

namespace interior {

struct NrTuning
{
    NgxPreset preset;
    NrIntensity intensity;
    NrStyle style;
    Strength localStructure;
    Strength localTone;
    SkinStrength skinStructure;
    bool autoMask;
    bool uiCorrection;
    [[nodiscard]] friend constexpr bool operator==(const NrTuning&, const NrTuning&) noexcept = default;
};

// What the operator can change while the session runs, from the panel or the command line. Everything
// else is fixed when the session starts, because it decides which devices and textures exist.
struct LiveSettings
{
    bool neuralRendering;
    NrTuning tuning;
    bool depthInverted;
    MotionScale mvScaleX;
    MotionScale mvScaleY;
    bool vsync;
    Fraction resetThreshold;
    DepthValue depth;
    [[nodiscard]] friend constexpr bool operator==(const LiveSettings&, const LiveSettings&) noexcept = default;
};

// Settings of the window and the capture session rather than of a frame, so the effect layer applies
// them directly and the planner never sees them.
struct SurfaceSettings
{
    CursorMode cursor;
    bool captureBorder;
    bool displayAffinity;
    bool topmost;
    bool clickThrough;
    LogLevel logLevel;
    [[nodiscard]] friend constexpr bool operator==(const SurfaceSettings&, const SurfaceSettings&) noexcept = default;
};

struct SourceSelection
{
    MonitorSelectionKind kind;
    RequestedMonitor index;
    [[nodiscard]] friend constexpr bool operator==(const SourceSelection&, const SourceSelection&) noexcept = default;
};

struct Options
{
    bool showHelp;
    bool listMonitors;
    SourceSelection source;
    WindowTitle window; // when set, one window is the source and the monitor selection is left alone
    std::optional<RequestedMonitor> target;
    bool neuralRendering;
    NrTuning tuning;
    SrMode sr;
    SrPreset srPreset;
    MotionBackend motion;
    LevelIndex motionFinestLevel;
    GridSize nvofGrid;
    PerfLevel nvofPerf;
    DepthValue depthValue;
    bool depthInverted;
    std::optional<MotionScale> mvScaleX; // nothing means the ratio between the model's size and the capture's
    std::optional<MotionScale> mvScaleY;
    Fraction resetThreshold;
    CursorMode cursor;
    bool vsync;
    CompareMode compare;
    ColorFormat format;
    bool captureBorder;
    DirectoryPath ngxPath;
    std::optional<NgxAppId> ngxAppId;
    ProjectIdText ngxProjectId;
    NgxLogLevel ngxLogLevel;
    DirectoryPath appDataPath;
    bool displayAffinity;
    bool topmost;
    bool clickThrough;
    bool redirectionBitmap;
    bool debugLayer;
    std::optional<RequestedAdapter> adapter;
    LogLevel logLevel;
    DirectoryPath logFile;
    bool gui;
    ConsoleMode console;
    bool indicator;
    bool cubinCache;
    bool showInert;         // show the panel page holding the settings that change nothing on a desktop
    bool excludeOwnWindows; // ask the capture to leave our own windows out, rather than hiding them from all capture
    [[nodiscard]] friend constexpr bool operator==(const Options&, const Options&) noexcept = default;
};

enum class OptionsErrorKind : std::uint8_t {
    UnknownOption,
    MissingValue,
    InvalidInteger,
    InvalidNumber,
    InvalidChoice,
    ValueOutOfRange,
    NonAscii,
    TooManyArguments,
    ArgumentTooLong,
    UnexpectedPositional,
    TargetWithAll,
};

struct ArgumentIndexTag;
using ArgumentIndex = infra::Strong<std::uint32_t, ArgumentIndexTag>;
struct ArgumentIndexTag
{
    [[nodiscard]] static constexpr ArgumentIndex Parse(std::uint32_t raw) noexcept { return ArgumentIndex(raw); }
};

struct OptionsError
{
    OptionsErrorKind kind;
    ArgumentIndex argument;
    [[nodiscard]] friend constexpr bool operator==(const OptionsError&, const OptionsError&) noexcept = default;
};

constexpr std::size_t kMaxArguments = 64;

[[nodiscard]] Options DefaultOptions() noexcept;
// The live settings a session starts from, taken from the options it was given.
[[nodiscard]] LiveSettings DefaultLive(const Options& options) noexcept;
[[nodiscard]] Result<Options, OptionsError> ParseOptions(std::span<const std::wstring_view> arguments) noexcept;
[[nodiscard]] std::string_view UsageText() noexcept;
[[nodiscard]] std::string_view Describe(OptionsErrorKind kind) noexcept;

} // namespace interior
