// WAIVER(R2): test suites accumulate failure counts and drive generated sequences with loops.
#include "interior/options.h"
#include "tests/test_registry.h"

#include <array>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace tests {
namespace {

using namespace interior;

constexpr std::array<std::wstring_view, 12> kVocabulary{ L"--monitor", L"all",  L"--target",  L"1",       L"--nr-intensity", L"1.5",
                                                         L"--sr",      L"dlaa", L"--mv=nvof", L"--bogus", L"positional",     L"--nr-style=cinematic" };

[[nodiscard]] std::vector<std::wstring> RandomArguments(infra::RngState& rng) noexcept
{
    const std::uint32_t count = proptest::DrawBelow(rng, 8);
    std::vector<std::wstring> args;
    for (std::uint32_t i = 0; i < count; ++i) // WAIVER(R2): test generator.
    {
        if (proptest::DrawBool(rng))
            args.emplace_back(kVocabulary[proptest::DrawBelow(rng, static_cast<std::uint32_t>(kVocabulary.size()))]);
        else
            args.emplace_back(std::wstring(proptest::DrawBelow(rng, 70), static_cast<wchar_t>(1 + proptest::DrawBelow(rng, 300))));
    }
    return args;
}

[[nodiscard]] std::vector<std::wstring_view> Views(const std::vector<std::wstring>& args) noexcept
{
    return std::vector<std::wstring_view>(args.begin(), args.end());
}

[[nodiscard]] bool ParserNeverPanicsAndErrorsAreEnumerated(infra::RngState& rng) noexcept
{
    const std::vector<std::wstring> args = RandomArguments(rng);
    const std::vector<std::wstring_view> views = Views(args);
    const auto parsed = ParseOptions(views);
    return parsed.has_value() || static_cast<std::uint8_t>(parsed.error().kind) <= static_cast<std::uint8_t>(OptionsErrorKind::TargetWithAll);
}

[[nodiscard]] bool EmptyArgumentsGiveDefaults(infra::RngState&) noexcept
{
    const auto parsed = ParseOptions(std::span<const std::wstring_view>{});
    return parsed.has_value() && *parsed == DefaultOptions();
}

[[nodiscard]] bool MonitorIndexRoundTrips(infra::RngState& rng) noexcept
{
    const std::uint32_t index = proptest::DrawBelow(rng, 100);
    const std::wstring text = std::to_wstring(index);
    const std::array<std::wstring_view, 2> args{ L"--monitor", text };
    const auto parsed = ParseOptions(args);
    return parsed.has_value() && parsed->source.kind == MonitorSelectionKind::Index && parsed->source.index.Get() == index;
}

// The model publishes no range for its strengths, so the parser imposes none: every finite value is
// the operator's to choose, and only a value the model could not act on is refused.
[[nodiscard]] bool IntensityAcceptsEveryFiniteValue(infra::RngState& rng) noexcept
{
    const float value = (proptest::DrawUnit(rng) - 0.5f) * 200.0f;
    const std::wstring joined = L"--nr-intensity=" + std::to_wstring(value);
    const std::array<std::wstring_view, 1> real{ joined };
    return ParseOptions(real).has_value();
}

[[nodiscard]] bool IntensityRefusesWhatIsNotANumber(infra::RngState&) noexcept
{
    const std::array<std::wstring_view, 1> nan{ L"--nr-intensity=nan" };
    const std::array<std::wstring_view, 1> text{ L"--nr-intensity=strong" };
    return !ParseOptions(nan).has_value() && !ParseOptions(text).has_value();
}

[[nodiscard]] bool TargetWithAllIsRejected(infra::RngState&) noexcept
{
    const std::array<std::wstring_view, 3> args{ L"--monitor=all", L"--target", L"0" };
    const auto parsed = ParseOptions(args);
    return !parsed.has_value() && parsed.error().kind == OptionsErrorKind::TargetWithAll;
}

[[nodiscard]] bool LastOccurrenceWins(infra::RngState& rng) noexcept
{
    const bool last = proptest::DrawBool(rng);
    const std::array<std::wstring_view, 2> args{ last ? L"--vsync=off" : L"--vsync=on", last ? L"--vsync=on" : L"--vsync=off" };
    const auto parsed = ParseOptions(args);
    return parsed.has_value() && parsed->vsync == last;
}

[[nodiscard]] bool MissingValueIsReported(infra::RngState&) noexcept
{
    const std::array<std::wstring_view, 1> args{ L"--target" };
    const auto parsed = ParseOptions(args);
    return !parsed.has_value() && parsed.error().kind == OptionsErrorKind::MissingValue;
}

[[nodiscard]] bool HelpAliasesWork(infra::RngState& rng) noexcept
{
    const std::array<std::wstring_view, 3> aliases{ L"-h", L"--help", L"/?" };
    const std::array<std::wstring_view, 1> args{ aliases[proptest::DrawBelow(rng, 3)] };
    const auto parsed = ParseOptions(args);
    return parsed.has_value() && parsed->showHelp;
}

constexpr std::array<std::pair<std::wstring_view, bool>, 8> kSpellings{
    { { L"on", true }, { L"1", true }, { L"true", true }, { L"yes", true }, { L"off", false }, { L"0", false }, { L"false", false }, { L"no", false } }
};

[[nodiscard]] bool BooleanSpellingsParse(infra::RngState& rng) noexcept
{
    const auto& [text, expected] = kSpellings[proptest::DrawBelow(rng, static_cast<std::uint32_t>(kSpellings.size()))];
    const std::array<std::wstring_view, 2> args{ L"--vsync", text };
    const auto parsed = ParseOptions(args);
    return parsed.has_value() && parsed->vsync == expected;
}

[[nodiscard]] bool HexAppIdRoundTrips(infra::RngState& rng) noexcept
{
    const std::uint64_t id = 1 + proptest::Draw(rng) % 0xFFFFFFFFull;
    const bool prefixed = proptest::DrawBool(rng);
    const std::wstring text = prefixed ? std::format(L"0x{:X}", id) : std::format(L"{:x}", id);
    const std::array<std::wstring_view, 2> args{ L"--ngx-app-id", text };
    const auto parsed = ParseOptions(args);
    return parsed.has_value() && parsed->ngxAppId.has_value() && parsed->ngxAppId->Get() == id;
}

[[nodiscard]] bool DefaultModelsMatchTheDocumentation(const Options& d) noexcept
{
    const NrTuning& t = d.tuning;
    const bool tuning = t.preset.Get() == 0 && t.intensity.Get() == 1.0f && t.style == NrStyle::Standard && t.localStructure.Get() == 1.0f && t.localTone.Get() == 1.0f &&
                        t.skinStructure.Get() == -1.0f && t.autoMask && t.uiCorrection;
    return d.neuralRendering && d.sr == SrMode::Auto && d.srPreset.Get() == 0 && tuning;
}

[[nodiscard]] bool DefaultMotionMatchesTheDocumentation(const Options& d) noexcept
{
    return d.motion == MotionBackend::BuiltIn && d.motionFinestLevel.Get() == 1 && d.nvofGrid == GridSize::One && d.nvofPerf == PerfLevel::Medium && d.depthValue.Get() == 0.5f &&
           d.resetThreshold.Get() == 0.5f;
}

[[nodiscard]] bool DefaultCaptureMatchesTheDocumentation(const Options& d) noexcept
{
    return d.source.kind == MonitorSelectionKind::Primary && !d.target.has_value() && d.cursor == CursorMode::Auto && d.vsync && d.compare == CompareMode::Off && d.format == ColorFormat::Rgba8 &&
           !d.captureBorder && d.ngxLogLevel == NgxLogLevel::On && !d.ngxAppId.has_value() && d.ngxPath.IsEmpty() && d.appDataPath.IsEmpty();
}

[[nodiscard]] bool DefaultWindowMatchesTheDocumentation(const Options& d) noexcept
{
    return d.displayAffinity && d.topmost && d.clickThrough && !d.redirectionBitmap && !d.debugLayer && !d.adapter.has_value() && d.logLevel == LogLevel::Info && d.logFile.IsEmpty() && !d.showHelp &&
           !d.listMonitors;
}

[[nodiscard]] bool DefaultsMatchTheDocumentation(infra::RngState&) noexcept
{
    const Options d = DefaultOptions();
    return DefaultModelsMatchTheDocumentation(d) && DefaultMotionMatchesTheDocumentation(d) && DefaultCaptureMatchesTheDocumentation(d) && DefaultWindowMatchesTheDocumentation(d);
}

[[nodiscard]] bool ArgumentCountIsBoundedExactly(infra::RngState&) noexcept
{
    const std::vector<std::wstring_view> full(kMaxArguments, L"--vsync=on");
    const std::vector<std::wstring_view> over(kMaxArguments + 1, L"--vsync=on");
    const auto accepted = ParseOptions(full);
    const auto rejected = ParseOptions(over);
    return accepted.has_value() && !rejected.has_value() && rejected.error().kind == OptionsErrorKind::TooManyArguments;
}

} // namespace

std::uint32_t OptionsSuite(std::uint64_t seed) noexcept
{
    std::uint32_t failures = 0;
    failures += Failures(proptest::ForAll("the defaults match the documentation", seed, 1, DefaultsMatchTheDocumentation));
    failures += Failures(proptest::ForAll("the argument count is bounded exactly", seed, 1, ArgumentCountIsBoundedExactly));
    failures += Failures(proptest::ForAll("boolean spellings parse", seed, 40, BooleanSpellingsParse));
    failures += Failures(proptest::ForAll("--ngx-app-id round-trips", seed, 200, HexAppIdRoundTrips));
    failures += Failures(proptest::ForAll("option parser never panics on random input", seed, 3000, ParserNeverPanicsAndErrorsAreEnumerated));
    failures += Failures(proptest::ForAll("empty arguments give the defaults", seed, 1, EmptyArgumentsGiveDefaults));
    failures += Failures(proptest::ForAll("--monitor N round-trips", seed, 200, MonitorIndexRoundTrips));
    failures += Failures(proptest::ForAll("--nr-intensity accepts every finite value", seed, 300, IntensityAcceptsEveryFiniteValue));
    failures += Failures(proptest::ForAll("--nr-intensity refuses what is not a number", seed, 1, IntensityRefusesWhatIsNotANumber));
    failures += Failures(proptest::ForAll("--target with --monitor all is rejected", seed, 1, TargetWithAllIsRejected));
    failures += Failures(proptest::ForAll("last occurrence wins", seed, 20, LastOccurrenceWins));
    failures += Failures(proptest::ForAll("missing value is reported", seed, 1, MissingValueIsReported));
    failures += Failures(proptest::ForAll("help aliases work", seed, 10, HelpAliasesWork));
    return failures;
}

} // namespace tests
