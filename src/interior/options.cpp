// WAIVER(R31): the lookups over the parsed option table are generic over the value type; the alternative is a dozen identical copies (R7).
#include "interior/options.h"

#include "infrastructure/fold.h"
#include "infrastructure/text.h"

#include <optional>

#include <algorithm>
#include <array>
#include <charconv>
#include <ranges>
#include <utility>
#include <variant>

namespace interior {
namespace {

using infra::Fail;

enum class OptionId : std::uint8_t {
    Help,
    ListMonitors,
    Monitor,
    Target,
    Nr,
    NrPreset,
    NrIntensity,
    NrStyle,
    NrLocalStructure,
    NrLocalTone,
    NrSkin,
    NrAutoMask,
    NrUiCorrection,
    Sr,
    SrPreset,
    Mv,
    MvLevel,
    NvofGrid,
    NvofPerf,
    DepthValue,
    ResetThreshold,
    Cursor,
    Vsync,
    Compare,
    Format,
    CaptureBorder,
    NgxPath,
    NgxAppId,
    NgxProjectId,
    NgxLog,
    AppData,
    Affinity,
    Topmost,
    ClickThrough,
    RedirectionBitmap,
    DebugLayer,
    Adapter,
    LogLevel,
    LogFile,
    Gui,
    Console,
};

enum class ValueKind : std::uint8_t {
    Flag,
    UInt,
    Float,
    Hex,
    Bool,
    MonitorSel,
    Cursor,
    Sr,
    Motion,
    Compare,
    Format,
    Style,
    Grid,
    Perf,
    NgxLog,
    Log,
    Console,
    Path,
    Text,
};

struct OptionSpec
{
    std::wstring_view name;
    OptionId id;
    ValueKind kind;
};

constexpr std::array<OptionSpec, 41> kSpecs{ {
    { L"help", OptionId::Help, ValueKind::Flag },
    { L"list-monitors", OptionId::ListMonitors, ValueKind::Flag },
    { L"monitor", OptionId::Monitor, ValueKind::MonitorSel },
    { L"target", OptionId::Target, ValueKind::UInt },
    { L"nr", OptionId::Nr, ValueKind::Bool },
    { L"nr-preset", OptionId::NrPreset, ValueKind::UInt },
    { L"nr-intensity", OptionId::NrIntensity, ValueKind::Float },
    { L"nr-style", OptionId::NrStyle, ValueKind::Style },
    { L"nr-local-structure", OptionId::NrLocalStructure, ValueKind::Float },
    { L"nr-local-tone", OptionId::NrLocalTone, ValueKind::Float },
    { L"nr-skin", OptionId::NrSkin, ValueKind::Float },
    { L"nr-automask", OptionId::NrAutoMask, ValueKind::Bool },
    { L"nr-ui-correction", OptionId::NrUiCorrection, ValueKind::Bool },
    { L"sr", OptionId::Sr, ValueKind::Sr },
    { L"sr-preset", OptionId::SrPreset, ValueKind::UInt },
    { L"mv", OptionId::Mv, ValueKind::Motion },
    { L"mv-level", OptionId::MvLevel, ValueKind::UInt },
    { L"nvof-grid", OptionId::NvofGrid, ValueKind::Grid },
    { L"nvof-perf", OptionId::NvofPerf, ValueKind::Perf },
    { L"depth-value", OptionId::DepthValue, ValueKind::Float },
    { L"reset-threshold", OptionId::ResetThreshold, ValueKind::Float },
    { L"cursor", OptionId::Cursor, ValueKind::Cursor },
    { L"vsync", OptionId::Vsync, ValueKind::Bool },
    { L"compare", OptionId::Compare, ValueKind::Compare },
    { L"format", OptionId::Format, ValueKind::Format },
    { L"capture-border", OptionId::CaptureBorder, ValueKind::Bool },
    { L"ngx-path", OptionId::NgxPath, ValueKind::Path },
    { L"ngx-app-id", OptionId::NgxAppId, ValueKind::Hex },
    { L"ngx-project-id", OptionId::NgxProjectId, ValueKind::Text },
    { L"ngx-log", OptionId::NgxLog, ValueKind::NgxLog },
    { L"app-data", OptionId::AppData, ValueKind::Path },
    { L"affinity", OptionId::Affinity, ValueKind::Bool },
    { L"topmost", OptionId::Topmost, ValueKind::Bool },
    { L"click-through", OptionId::ClickThrough, ValueKind::Bool },
    { L"redirection-bitmap", OptionId::RedirectionBitmap, ValueKind::Bool },
    { L"debug-layer", OptionId::DebugLayer, ValueKind::Bool },
    { L"adapter", OptionId::Adapter, ValueKind::UInt },
    { L"log-level", OptionId::LogLevel, ValueKind::Log },
    { L"log-file", OptionId::LogFile, ValueKind::Path },
    { L"gui", OptionId::Gui, ValueKind::Bool },
    { L"console", OptionId::Console, ValueKind::Console },
} };

struct FlagValue
{
    [[nodiscard]] friend constexpr bool operator==(const FlagValue&, const FlagValue&) noexcept = default;
};

struct MonitorSelValue
{
    MonitorSelectionKind kind;
    std::uint32_t index;
};

using AsciiText = infra::BoundedString<char, 64>;

struct NarrowBuffer
{
    std::array<char, AsciiText::Capacity> chars;
    std::size_t size;
};

using OptionValue = std::variant<FlagValue, std::uint32_t, float, std::uint64_t, bool, MonitorSelValue, CursorMode, SrMode, MotionBackend, CompareMode, ColorFormat, NrStyle, GridSize, PerfLevel,
                                 ConsoleMode, NgxLogLevel, LogLevel, DirectoryPath, AsciiText>;

struct ParsedOption
{
    OptionId id;
    ArgumentIndex argument;
    OptionValue value;
};

using ParsedList = infra::BoundedVector<ParsedOption, kMaxArguments>;

struct ParseState
{
    ParsedList parsed;
    std::optional<OptionSpec> pending;
    ArgumentIndex pendingArgument;
};

using ParseResult = Result<ParseState, OptionsError>;

// --- text helpers ---------------------------------------------------------------

[[nodiscard]] Result<AsciiText, OptionsErrorKind> AsciiOf(std::wstring_view text) noexcept
{
    if (!infra::IsAllAscii(text))
        return Fail(OptionsErrorKind::NonAscii);
    return AsciiText::Parse(std::string_view(NarrowBuffer{ infra::NarrowedChars<AsciiText::Capacity>(text), text.size() }.chars.data(), text.size())).transform_error([](infra::StringTooLong) {
        return OptionsErrorKind::ArgumentTooLong;
    });
}

[[nodiscard]] Result<AsciiText, OptionsErrorKind> AsciiOfChecked(std::wstring_view text) noexcept
{
    if (text.size() > AsciiText::Capacity)
        return Fail(OptionsErrorKind::ArgumentTooLong);
    return AsciiOf(text);
}

[[nodiscard]] bool IsConsumed(std::from_chars_result result, const char* end) noexcept
{
    return result.ec == std::errc() && result.ptr == end;
}

// --- value parsers --------------------------------------------------------------

[[nodiscard]] Result<std::uint32_t, OptionsErrorKind> ParseUInt(std::wstring_view text) noexcept
{
    return AsciiOfChecked(text).and_then([](AsciiText ascii) -> Result<std::uint32_t, OptionsErrorKind> {
        std::uint32_t value = 0;
        const std::string_view view = ascii.Get();
        const std::from_chars_result result = std::from_chars(view.data(), view.data() + view.size(), value);
        if (!IsConsumed(result, view.data() + view.size()))
            return Fail(OptionsErrorKind::InvalidInteger);
        return value;
    });
}

[[nodiscard]] Result<float, OptionsErrorKind> ParseFloat(std::wstring_view text) noexcept
{
    return AsciiOfChecked(text).and_then([](AsciiText ascii) -> Result<float, OptionsErrorKind> {
        float value = 0.0f;
        const std::string_view view = ascii.Get();
        const std::from_chars_result result = std::from_chars(view.data(), view.data() + view.size(), value);
        if (!IsConsumed(result, view.data() + view.size()))
            return Fail(OptionsErrorKind::InvalidNumber);
        return value;
    });
}

[[nodiscard]] bool HasHexPrefix(std::string_view text) noexcept
{
    return text.starts_with("0x") || text.starts_with("0X");
}

[[nodiscard]] std::string_view WithoutHexPrefix(std::string_view text) noexcept
{
    return HasHexPrefix(text) ? text.substr(2) : text;
}

[[nodiscard]] Result<std::uint64_t, OptionsErrorKind> ParseHex(std::wstring_view text) noexcept
{
    return AsciiOfChecked(text).and_then([](AsciiText ascii) -> Result<std::uint64_t, OptionsErrorKind> {
        std::uint64_t value = 0;
        const std::string_view view = WithoutHexPrefix(ascii.Get());
        const std::from_chars_result result = std::from_chars(view.data(), view.data() + view.size(), value, 16);
        if (!IsConsumed(result, view.data() + view.size()))
            return Fail(OptionsErrorKind::InvalidInteger);
        return value;
    });
}

template <class T, std::size_t N>
[[nodiscard]] Result<T, OptionsErrorKind> ParseChoice(const std::array<infra::Choice<T>, N>& choices, std::wstring_view text) noexcept
{
    const std::optional<T> found = infra::FindChoice(choices, text);
    if (!found.has_value())
        return Fail(OptionsErrorKind::InvalidChoice);
    return *found;
}

constexpr std::array<infra::Choice<bool>, 8> kBoolChoices{
    { { L"on", true }, { L"1", true }, { L"true", true }, { L"yes", true }, { L"off", false }, { L"0", false }, { L"false", false }, { L"no", false } }
};
constexpr std::array<infra::Choice<ConsoleMode>, 3> kConsoleChoices{ { { L"auto", ConsoleMode::Auto }, { L"on", ConsoleMode::On }, { L"off", ConsoleMode::Off } } };
constexpr std::array<infra::Choice<CursorMode>, 3> kCursorChoices{ { { L"auto", CursorMode::Auto }, { L"on", CursorMode::On }, { L"off", CursorMode::Off } } };
constexpr std::array<infra::Choice<SrMode>, 3> kSrChoices{ { { L"auto", SrMode::Auto }, { L"dlaa", SrMode::Dlaa }, { L"off", SrMode::Off } } };
constexpr std::array<infra::Choice<MotionBackend>, 3> kMotionChoices{ { { L"builtin", MotionBackend::BuiltIn }, { L"nvof", MotionBackend::NvOpticalFlow }, { L"none", MotionBackend::None } } };
constexpr std::array<infra::Choice<CompareMode>, 3> kCompareChoices{ { { L"off", CompareMode::Off }, { L"split", CompareMode::Split }, { L"original", CompareMode::Original } } };
constexpr std::array<infra::Choice<ColorFormat>, 2> kFormatChoices{ { { L"rgba8", ColorFormat::Rgba8 }, { L"rgba16f", ColorFormat::Rgba16f } } };
constexpr std::array<infra::Choice<NrStyle>, 6> kStyleChoices{ { { L"0", NrStyle::Standard },
                                                                 { L"standard", NrStyle::Standard },
                                                                 { L"1", NrStyle::Natural },
                                                                 { L"natural", NrStyle::Natural },
                                                                 { L"2", NrStyle::Cinematic },
                                                                 { L"cinematic", NrStyle::Cinematic } } };
constexpr std::array<infra::Choice<GridSize>, 3> kGridChoices{ { { L"1", GridSize::One }, { L"2", GridSize::Two }, { L"4", GridSize::Four } } };
constexpr std::array<infra::Choice<PerfLevel>, 6> kPerfChoices{
    { { L"slow", PerfLevel::Slow }, { L"5", PerfLevel::Slow }, { L"medium", PerfLevel::Medium }, { L"10", PerfLevel::Medium }, { L"fast", PerfLevel::Fast }, { L"20", PerfLevel::Fast } }
};
constexpr std::array<infra::Choice<NgxLogLevel>, 3> kNgxLogChoices{ { { L"0", NgxLogLevel::Off }, { L"1", NgxLogLevel::On }, { L"2", NgxLogLevel::Verbose } } };
constexpr std::array<infra::Choice<LogLevel>, 8> kLogChoices{ { { L"0", LogLevel::Debug },
                                                                { L"debug", LogLevel::Debug },
                                                                { L"1", LogLevel::Info },
                                                                { L"info", LogLevel::Info },
                                                                { L"2", LogLevel::Warn },
                                                                { L"warn", LogLevel::Warn },
                                                                { L"3", LogLevel::Error },
                                                                { L"error", LogLevel::Error } } };
constexpr std::array<infra::Choice<MonitorSelectionKind>, 2> kMonitorKindChoices{ { { L"primary", MonitorSelectionKind::Primary }, { L"all", MonitorSelectionKind::All } } };

[[nodiscard]] Result<MonitorSelValue, OptionsErrorKind> ParseMonitorSel(std::wstring_view text) noexcept
{
    const Result<MonitorSelectionKind, OptionsErrorKind> named = ParseChoice(kMonitorKindChoices, text);
    if (named.has_value())
        return MonitorSelValue{ *named, 0 };
    return ParseUInt(text).transform([](std::uint32_t index) { return MonitorSelValue{ MonitorSelectionKind::Index, index }; });
}

[[nodiscard]] Result<DirectoryPath, OptionsErrorKind> ParsePath(std::wstring_view text) noexcept
{
    return DirectoryPath::Parse(text).transform_error([](infra::StringTooLong) { return OptionsErrorKind::ArgumentTooLong; });
}

[[nodiscard]] Result<OptionValue, OptionsErrorKind> ParseValue(ValueKind kind, std::wstring_view text) noexcept
{
    switch (kind)
    {
    case ValueKind::Flag: return OptionValue{ FlagValue{} };
    case ValueKind::UInt: return ParseUInt(text).transform([](std::uint32_t v) { return OptionValue{ v }; });
    case ValueKind::Float: return ParseFloat(text).transform([](float v) { return OptionValue{ v }; });
    case ValueKind::Hex: return ParseHex(text).transform([](std::uint64_t v) { return OptionValue{ v }; });
    case ValueKind::Bool: return ParseChoice(kBoolChoices, text).transform([](bool v) { return OptionValue{ v }; });
    case ValueKind::MonitorSel: return ParseMonitorSel(text).transform([](MonitorSelValue v) { return OptionValue{ v }; });
    case ValueKind::Console: return ParseChoice(kConsoleChoices, text).transform([](ConsoleMode v) { return OptionValue{ v }; });
    case ValueKind::Cursor: return ParseChoice(kCursorChoices, text).transform([](CursorMode v) { return OptionValue{ v }; });
    case ValueKind::Sr: return ParseChoice(kSrChoices, text).transform([](SrMode v) { return OptionValue{ v }; });
    case ValueKind::Motion: return ParseChoice(kMotionChoices, text).transform([](MotionBackend v) { return OptionValue{ v }; });
    case ValueKind::Compare: return ParseChoice(kCompareChoices, text).transform([](CompareMode v) { return OptionValue{ v }; });
    case ValueKind::Format: return ParseChoice(kFormatChoices, text).transform([](ColorFormat v) { return OptionValue{ v }; });
    case ValueKind::Style: return ParseChoice(kStyleChoices, text).transform([](NrStyle v) { return OptionValue{ v }; });
    case ValueKind::Grid: return ParseChoice(kGridChoices, text).transform([](GridSize v) { return OptionValue{ v }; });
    case ValueKind::Perf: return ParseChoice(kPerfChoices, text).transform([](PerfLevel v) { return OptionValue{ v }; });
    case ValueKind::NgxLog: return ParseChoice(kNgxLogChoices, text).transform([](NgxLogLevel v) { return OptionValue{ v }; });
    case ValueKind::Log: return ParseChoice(kLogChoices, text).transform([](LogLevel v) { return OptionValue{ v }; });
    case ValueKind::Path: return ParsePath(text).transform([](DirectoryPath v) { return OptionValue{ v }; });
    case ValueKind::Text: return AsciiOfChecked(text).transform([](AsciiText v) { return OptionValue{ v }; });
    }
    return Fail(OptionsErrorKind::InvalidChoice);
}

// --- tokenizer ------------------------------------------------------------------

struct Token
{
    std::wstring_view name;
    std::optional<std::wstring_view> inlineValue;
};

constexpr std::array<std::wstring_view, 3> kHelpAliases{ L"-h", L"-?", L"/?" };

[[nodiscard]] bool IsHelpAlias(std::wstring_view text) noexcept
{
    return std::ranges::find(kHelpAliases, text) != kHelpAliases.end();
}

[[nodiscard]] bool IsOptionToken(std::wstring_view text) noexcept
{
    return text.starts_with(L"--");
}

[[nodiscard]] Token SplitToken(std::wstring_view text) noexcept
{
    const std::wstring_view body = text.substr(2);
    const std::size_t eq = body.find(L'=');
    if (eq == std::wstring_view::npos)
        return Token{ body, std::nullopt };
    return Token{ body.substr(0, eq), body.substr(eq + 1) };
}

[[nodiscard]] std::optional<OptionSpec> FindSpec(std::wstring_view name) noexcept
{
    const auto found = std::ranges::find_if(kSpecs, [name](const OptionSpec& s) { return infra::EqualsIgnoringCase(s.name, name); });
    if (found == kSpecs.end())
        return std::nullopt;
    return *found;
}

[[nodiscard]] bool IsFlag(const OptionSpec& spec) noexcept
{
    return spec.kind == ValueKind::Flag;
}

[[nodiscard]] OptionsError At(OptionsErrorKind kind, ArgumentIndex argument) noexcept
{
    return OptionsError{ kind, argument };
}

[[nodiscard]] ParseResult Record(const ParseState& state, const OptionSpec& spec, ArgumentIndex argument, std::wstring_view text) noexcept
{
    return ParseValue(spec.kind, text).transform_error([argument](OptionsErrorKind kind) { return At(kind, argument); }).and_then([&state, &spec, argument](OptionValue value) -> ParseResult {
        return state.parsed.Push(ParsedOption{ spec.id, argument, value })
            .transform([](ParsedList list) { return ParseState{ list, std::nullopt, ArgumentIndexTag::Parse(0) }; })
            .transform_error([argument](infra::CapacityExceeded) { return At(OptionsErrorKind::TooManyArguments, argument); });
    });
}

[[nodiscard]] ParseResult RecordWithoutValue(const ParseState& state, const OptionSpec& spec, ArgumentIndex argument) noexcept
{
    if (IsFlag(spec))
        return Record(state, spec, argument, std::wstring_view{});
    return ParseState{ state.parsed, spec, argument };
}

[[nodiscard]] ParseResult RecordToken(const ParseState& state, const OptionSpec& spec, const Token& token, ArgumentIndex argument) noexcept
{
    if (token.inlineValue.has_value())
        return Record(state, spec, argument, *token.inlineValue);
    return RecordWithoutValue(state, spec, argument);
}

[[nodiscard]] ParseResult ConsumeOption(const ParseState& state, std::wstring_view text, ArgumentIndex argument) noexcept
{
    const Token token = SplitToken(text);
    const std::optional<OptionSpec> spec = FindSpec(token.name);
    if (!spec.has_value())
        return Fail(At(OptionsErrorKind::UnknownOption, argument));
    return RecordToken(state, *spec, token, argument);
}

[[nodiscard]] ParseResult ConsumeNonAlias(const ParseState& state, std::wstring_view text, ArgumentIndex argument) noexcept
{
    if (!IsOptionToken(text))
        return Fail(At(OptionsErrorKind::UnexpectedPositional, argument));
    return ConsumeOption(state, text, argument);
}

[[nodiscard]] ParseResult ConsumeStandalone(const ParseState& state, std::wstring_view text, ArgumentIndex argument) noexcept
{
    if (IsHelpAlias(text))
        return Record(state, kSpecs[0], argument, std::wstring_view{});
    return ConsumeNonAlias(state, text, argument);
}

[[nodiscard]] ParseResult ConsumeToken(const ParseState& state, std::wstring_view text, ArgumentIndex argument) noexcept
{
    if (state.pending.has_value())
        return Record(state, *state.pending, state.pendingArgument, text);
    return ConsumeStandalone(state, text, argument);
}

[[nodiscard]] ParseResult ConsumeIndexed(const ParseState& state, std::span<const std::wstring_view> arguments, std::uint32_t index) noexcept
{
    return ConsumeToken(state, arguments[index], ArgumentIndexTag::Parse(index));
}

[[nodiscard]] ParseResult RejectPending(const ParseState& state) noexcept
{
    if (state.pending.has_value())
        return Fail(At(OptionsErrorKind::MissingValue, state.pendingArgument));
    return state;
}

// --- building the typed record ------------------------------------------------------

template <class T>
[[nodiscard]] bool IsOptionOf(const ParsedOption& p, OptionId id) noexcept
{
    return p.id == id && std::holds_alternative<T>(p.value);
}

template <class T>
[[nodiscard]] std::optional<ParsedOption> LastOf(const ParsedList& list, OptionId id) noexcept
{
    const auto reversed = std::views::reverse(list.Items());
    const auto found = std::ranges::find_if(reversed, [id](const ParsedOption& p) { return IsOptionOf<T>(p, id); });
    if (found == reversed.end())
        return std::nullopt;
    return *found;
}

template <class T>
[[nodiscard]] T Held(const OptionValue& value, T fallback) noexcept
{
    const T* held = std::get_if<T>(&value);
    return held != nullptr ? *held : fallback;
}

template <class T>
[[nodiscard]] T ValueOr(const ParsedList& list, OptionId id, T fallback) noexcept
{
    const std::optional<ParsedOption> option = LastOf<T>(list, id);
    return option.has_value() ? Held<T>(option->value, fallback) : fallback;
}

[[nodiscard]] bool HasFlag(const ParsedList& list, OptionId id) noexcept
{
    return LastOf<FlagValue>(list, id).has_value();
}

template <class T, class Parser>
[[nodiscard]] Result<T, OptionsError> Validated(const ParsedList& list, OptionId id, T fallback, Parser parse) noexcept
{
    using Raw = decltype(std::declval<T>().Get());
    const std::optional<ParsedOption> option = LastOf<Raw>(list, id);
    if (!option.has_value())
        return fallback;
    return parse(Held<Raw>(option->value, Raw{})).transform_error([&option](UnitError) { return At(OptionsErrorKind::ValueOutOfRange, option->argument); });
}

template <class T>
[[nodiscard]] std::optional<T> OptionalOf(const ParsedList& list, OptionId id) noexcept
{
    const std::optional<ParsedOption> option = LastOf<T>(list, id);
    if (!option.has_value())
        return std::nullopt;
    return Held<T>(option->value, T{});
}

[[nodiscard]] SourceSelection SourceOf(const ParsedList& list) noexcept
{
    const MonitorSelValue value = ValueOr(list, OptionId::Monitor, MonitorSelValue{ MonitorSelectionKind::Primary, 0 });
    return SourceSelection{ value.kind, RequestedMonitorTag::Parse(value.index) };
}

[[nodiscard]] std::optional<RequestedMonitor> RequestedOf(const ParsedList& list, OptionId id) noexcept
{
    return OptionalOf<std::uint32_t>(list, id).transform(RequestedMonitorTag::Parse);
}

[[nodiscard]] std::optional<RequestedAdapter> RequestedAdapterOf(const ParsedList& list) noexcept
{
    return OptionalOf<std::uint32_t>(list, OptionId::Adapter).transform(RequestedAdapterTag::Parse);
}

[[nodiscard]] Result<std::optional<NgxAppId>, OptionsError> AppIdOf(const ParsedList& list) noexcept
{
    const std::optional<ParsedOption> option = LastOf<std::uint64_t>(list, OptionId::NgxAppId);
    if (!option.has_value())
        return std::optional<NgxAppId>{};
    return NgxAppIdTag::Parse(Held<std::uint64_t>(option->value, 0u)).transform([](NgxAppId id) { return std::optional<NgxAppId>{ id }; }).transform_error([&option](UnitError) {
        return At(OptionsErrorKind::ValueOutOfRange, option->argument);
    });
}

[[nodiscard]] Result<ProjectIdText, OptionsError> ProjectIdOf(const ParsedList& list, ProjectIdText fallback) noexcept
{
    const std::optional<ParsedOption> option = LastOf<AsciiText>(list, OptionId::NgxProjectId);
    if (!option.has_value())
        return fallback;
    return ParseProjectId(Held<AsciiText>(option->value, AsciiText{}).Get()).transform_error([&option](UnitError) { return At(OptionsErrorKind::ValueOutOfRange, option->argument); });
}

[[nodiscard]] bool IsTargetWithAll(const Options& options) noexcept
{
    return options.source.kind == MonitorSelectionKind::All && options.target.has_value();
}

[[nodiscard]] Result<Options, OptionsError> RejectConflicts(const Options& options) noexcept
{
    if (IsTargetWithAll(options))
        return Fail(At(OptionsErrorKind::TargetWithAll, ArgumentIndexTag::Parse(0)));
    return options;
}

struct ValidatedTuning
{
    NgxPreset preset;
    Strength intensity;
    Strength localStructure;
    Strength localTone;
    SkinStrength skin;
};

[[nodiscard]] Result<ValidatedTuning, OptionsError> TuningOf(const ParsedList& list, const NrTuning& d) noexcept
{
    return Validated(list, OptionId::NrPreset, d.preset, NgxPresetTag::Parse).and_then([&](NgxPreset preset) {
        return Validated(list, OptionId::NrIntensity, d.intensity, StrengthTag::Parse).and_then([&](Strength intensity) {
            return Validated(list, OptionId::NrLocalStructure, d.localStructure, StrengthTag::Parse).and_then([&](Strength structure) {
                return Validated(list, OptionId::NrLocalTone, d.localTone, StrengthTag::Parse).and_then([&](Strength tone) {
                    return Validated(list, OptionId::NrSkin, d.skinStructure, SkinStrengthTag::Parse).transform([&](SkinStrength skin) {
                        return ValidatedTuning{ preset, intensity, structure, tone, skin };
                    });
                });
            });
        });
    });
}

[[nodiscard]] NrTuning TuningFrom(const ParsedList& list, const ValidatedTuning& v, const NrTuning& d) noexcept
{
    return NrTuning{ v.preset,    v.intensity, ValueOr(list, OptionId::NrStyle, d.style),       v.localStructure,
                     v.localTone, v.skin,      ValueOr(list, OptionId::NrAutoMask, d.autoMask), ValueOr(list, OptionId::NrUiCorrection, d.uiCorrection) };
}

struct ValidatedNumbers
{
    SrPreset srPreset;
    LevelIndex level;
    DepthValue depth;
    Fraction threshold;
};

[[nodiscard]] Result<ValidatedNumbers, OptionsError> NumbersOf(const ParsedList& list, const Options& d) noexcept
{
    return Validated(list, OptionId::SrPreset, d.srPreset, SrPresetTag::Parse).and_then([&](SrPreset srPreset) {
        return Validated(list, OptionId::MvLevel, d.motionFinestLevel, LevelIndexTag::Parse).and_then([&](LevelIndex level) {
            return Validated(list, OptionId::DepthValue, d.depthValue, DepthValueTag::Parse).and_then([&](DepthValue depth) {
                return Validated(list, OptionId::ResetThreshold, d.resetThreshold, FractionTag::Parse).transform([&](Fraction threshold) {
                    return ValidatedNumbers{ srPreset, level, depth, threshold };
                });
            });
        });
    });
}

[[nodiscard]] Options Assemble(const ParsedList& list, const Options& d, const NrTuning& tuning, const ValidatedNumbers& n, std::optional<NgxAppId> appId, ProjectIdText projectId) noexcept
{
    return Options{
        HasFlag(list, OptionId::Help),
        HasFlag(list, OptionId::ListMonitors),
        SourceOf(list),
        RequestedOf(list, OptionId::Target),
        ValueOr(list, OptionId::Nr, d.neuralRendering),
        tuning,
        ValueOr(list, OptionId::Sr, d.sr),
        n.srPreset,
        ValueOr(list, OptionId::Mv, d.motion),
        n.level,
        ValueOr(list, OptionId::NvofGrid, d.nvofGrid),
        ValueOr(list, OptionId::NvofPerf, d.nvofPerf),
        n.depth,
        n.threshold,
        ValueOr(list, OptionId::Cursor, d.cursor),
        ValueOr(list, OptionId::Vsync, d.vsync),
        ValueOr(list, OptionId::Compare, d.compare),
        ValueOr(list, OptionId::Format, d.format),
        ValueOr(list, OptionId::CaptureBorder, d.captureBorder),
        ValueOr(list, OptionId::NgxPath, d.ngxPath),
        appId,
        projectId,
        ValueOr(list, OptionId::NgxLog, d.ngxLogLevel),
        ValueOr(list, OptionId::AppData, d.appDataPath),
        ValueOr(list, OptionId::Affinity, d.displayAffinity),
        ValueOr(list, OptionId::Topmost, d.topmost),
        ValueOr(list, OptionId::ClickThrough, d.clickThrough),
        ValueOr(list, OptionId::RedirectionBitmap, d.redirectionBitmap),
        ValueOr(list, OptionId::DebugLayer, d.debugLayer),
        RequestedAdapterOf(list),
        ValueOr(list, OptionId::LogLevel, d.logLevel),
        ValueOr(list, OptionId::LogFile, d.logFile),
        ValueOr(list, OptionId::Gui, d.gui),
        ValueOr(list, OptionId::Console, d.console),
    };
}

[[nodiscard]] Result<Options, OptionsError> Build(const ParsedList& list) noexcept
{
    const Options d = DefaultOptions();
    return TuningOf(list, d.tuning).and_then([&](ValidatedTuning tuning) {
        return NumbersOf(list, d).and_then([&](ValidatedNumbers numbers) {
            return AppIdOf(list).and_then([&](std::optional<NgxAppId> appId) {
                return ProjectIdOf(list, d.ngxProjectId).and_then([&](ProjectIdText projectId) {
                    return RejectConflicts(Assemble(list, d, TuningFrom(list, tuning, d.tuning), numbers, appId, projectId));
                });
            });
        });
    });
}

[[nodiscard]] bool HasTooManyArguments(std::span<const std::wstring_view> arguments) noexcept
{
    return arguments.size() > kMaxArguments;
}

[[nodiscard]] Result<ParseState, OptionsError> FoldArguments(std::span<const std::wstring_view> arguments) noexcept
{
    return infra::FoldResult(std::views::iota(std::uint32_t{ 0 }, static_cast<std::uint32_t>(arguments.size())), ParseResult(ParseState{ ParsedList{}, std::nullopt, ArgumentIndexTag::Parse(0) }),
                             [arguments](const ParseState& state, std::uint32_t index) { return ConsumeIndexed(state, arguments, index); });
}

constexpr auto kDefaultIntensity = StrengthTag::Parse(1.0f);
constexpr auto kDefaultSkin = SkinStrengthTag::Parse(-1.0f);
constexpr auto kDefaultDepth = DepthValueTag::Parse(0.5f);
constexpr auto kDefaultThreshold = FractionTag::Parse(0.5f);
constexpr auto kDefaultPreset = NgxPresetTag::Parse(kShippedNgxPreset); // asking for the one the model has avoids its fallback warning
constexpr auto kDefaultSrPreset = SrPresetTag::Parse(0);
constexpr auto kDefaultLevel = LevelIndexTag::Parse(1);
constexpr auto kDefaultProjectId = ProjectIdText::Parse("5e9b2a44-7c31-4d0e-9f2b-8d3c1a6e7f10");
static_assert(kDefaultIntensity.has_value() && kDefaultSkin.has_value());
static_assert(kDefaultDepth.has_value() && kDefaultThreshold.has_value());
static_assert(kDefaultPreset.has_value() && kDefaultSrPreset.has_value());
static_assert(kDefaultLevel.has_value() && kDefaultProjectId.has_value());

} // namespace

Options DefaultOptions() noexcept
{
    return Options{
        false,
        false,
        SourceSelection{ MonitorSelectionKind::Primary, RequestedMonitorTag::Parse(0) },
        std::nullopt,
        true,
        NrTuning{ *kDefaultPreset, *kDefaultIntensity, NrStyle::Standard, *kDefaultIntensity, *kDefaultIntensity, *kDefaultSkin, true, true },
        SrMode::Auto,
        *kDefaultSrPreset,
        MotionBackend::BuiltIn,
        *kDefaultLevel,
        GridSize::One,
        PerfLevel::Medium,
        *kDefaultDepth,
        *kDefaultThreshold,
        CursorMode::Auto,
        true,
        CompareMode::Off,
        ColorFormat::Rgba8,
        false,
        DirectoryPath{},
        std::nullopt,
        *kDefaultProjectId,
        NgxLogLevel::On,
        DirectoryPath{},
        true,
        true,
        true,
        false,
        false,
        std::nullopt,
        LogLevel::Info,
        DirectoryPath{},
        true,
        ConsoleMode::Auto,
    };
}

Result<Options, OptionsError> ParseOptions(std::span<const std::wstring_view> arguments) noexcept
{
    if (HasTooManyArguments(arguments))
        return Fail(At(OptionsErrorKind::TooManyArguments, ArgumentIndexTag::Parse(kMaxArguments)));
    return FoldArguments(arguments).and_then(RejectPending).and_then([](const ParseState& state) { return Build(state.parsed); });
}

std::string_view Describe(OptionsErrorKind kind) noexcept
{
    switch (kind)
    {
    case OptionsErrorKind::UnknownOption: return "unknown option";
    case OptionsErrorKind::MissingValue: return "option needs a value";
    case OptionsErrorKind::InvalidInteger: return "not an integer";
    case OptionsErrorKind::InvalidNumber: return "not a number";
    case OptionsErrorKind::InvalidChoice: return "not one of the allowed values";
    case OptionsErrorKind::ValueOutOfRange: return "value out of range";
    case OptionsErrorKind::NonAscii: return "value must be ASCII";
    case OptionsErrorKind::TooManyArguments: return "too many arguments";
    case OptionsErrorKind::ArgumentTooLong: return "argument too long";
    case OptionsErrorKind::UnexpectedPositional: return "unexpected argument";
    case OptionsErrorKind::TargetWithAll: return "--target cannot be combined with --monitor all";
    }
    return "invalid arguments";
}

} // namespace interior
