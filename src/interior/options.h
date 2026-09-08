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
    Strength intensity;
    NrStyle style;
    Strength localStructure;
    Strength localTone;
    SkinStrength skinStructure;
    bool autoMask;
    bool uiCorrection;
    [[nodiscard]] friend constexpr bool operator==(const NrTuning&, const NrTuning&) noexcept = default;
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
[[nodiscard]] Result<Options, OptionsError> ParseOptions(std::span<const std::wstring_view> arguments) noexcept;
[[nodiscard]] std::string_view UsageText() noexcept;
[[nodiscard]] std::string_view Describe(OptionsErrorKind kind) noexcept;

} // namespace interior
