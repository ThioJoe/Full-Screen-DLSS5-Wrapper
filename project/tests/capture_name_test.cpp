// WAIVER(R2): test suites accumulate failure counts and drive generated sequences with loops.
#include "interior/capture_name.h"
#include "tests/test_registry.h"

#include <array>
#include <string>
#include <string_view>

namespace tests {
namespace {

using namespace interior;

[[nodiscard]] bool Contains(std::string_view text, std::string_view part) noexcept
{
    return text.find(part) != std::string_view::npos;
}

[[nodiscard]] bool EndsWith(std::string_view text, std::string_view part) noexcept
{
    return text.size() >= part.size() && text.substr(text.size() - part.size()) == part;
}

// Every kind of character a title might hold, so the label's filtering is exercised on all of them.
[[nodiscard]] std::wstring RandomTitle(infra::RngState& rng) noexcept
{
    constexpr std::array<wchar_t, 12> alphabet{ L'a', L'Z', L'7', L' ', L'-', L'_', L'.', L':', L'\\', L'/', L'é', L'中' };
    std::wstring title;
    const std::uint32_t length = proptest::DrawBelow(rng, 60);
    for (std::uint32_t i = 0; i < length; ++i)
        title.push_back(alphabet[proptest::DrawBelow(rng, static_cast<std::uint32_t>(alphabet.size()))]);
    return title;
}

[[nodiscard]] bool IsLetterOrDigit(char c) noexcept
{
    return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

[[nodiscard]] bool TheLabelKeepsOnlyLettersAndDigits(infra::RngState& rng) noexcept
{
    const std::wstring title = RandomTitle(rng);
    const CaptureLabel label = CaptureLabelOf(title);
    std::string expected;
    for (const wchar_t c : title)
        if (c < 128 && IsLetterOrDigit(static_cast<char>(c)) && expected.size() < CaptureLabel::Capacity)
            expected.push_back(static_cast<char>(c));
    if (expected.empty())
        return label.Get() == kWindowLabel;
    return label.Get() == expected;
}

[[nodiscard]] LiveSettings RandomLive(infra::RngState& rng) noexcept
{
    const LiveSettings base = DefaultLive(DefaultOptions());
    const NrTuning t = base.tuning;
    const float skin = proptest::DrawBool(rng) ? -1.0f : proptest::DrawUnit(rng) * 3.0f;
    const NrTuning tuning{ t.preset,
                           NrIntensityTag::Parse(proptest::DrawUnit(rng)).value_or(t.intensity),
                           static_cast<NrStyle>(proptest::DrawBelow(rng, 3)),
                           StrengthTag::Parse(proptest::DrawUnit(rng) * 3.0f).value_or(t.localStructure),
                           StrengthTag::Parse(proptest::DrawUnit(rng) * 3.0f).value_or(t.localTone),
                           SkinStrengthTag::Parse(skin).value_or(t.skinStructure),
                           proptest::DrawBool(rng),
                           t.uiCorrection };
    return LiveSettings{ base.neuralRendering, tuning,    PassCountTag::Parse(proptest::DrawBetween(rng, 1, 6)).value_or(base.passes), base.depthInverted, base.mvScaleX, base.mvScaleY, base.vsync,
                         base.resetThreshold,  base.depth };
}

[[nodiscard]] CaptureMoment RandomMoment(infra::RngState& rng) noexcept
{
    return CaptureMoment{ proptest::DrawBetween(rng, 1, 12), proptest::DrawBetween(rng, 1, 31), proptest::DrawBelow(rng, 24), proptest::DrawBelow(rng, 60) };
}

[[nodiscard]] bool TheStemNamesTheMaskAsItIs(infra::RngState& rng) noexcept
{
    const LiveSettings live = RandomLive(rng);
    const bool everything = proptest::DrawBool(rng);
    const CaptureStem stem = CaptureStemOf(CaptureLabelOf(L"Desktop"), live, everything, RandomMoment(rng));
    const bool noMask = Contains(stem.Get(), "_nomask");
    const bool autoMask = Contains(stem.Get(), "_automask");
    return noMask == !live.tuning.autoMask && autoMask == (live.tuning.autoMask && everything);
}

[[nodiscard]] bool TheStemGivesSkinOnlyWhenItCounts(infra::RngState& rng) noexcept
{
    const LiveSettings live = RandomLive(rng);
    const CaptureStem stem = CaptureStemOf(CaptureLabelOf(L"Desktop"), live, proptest::DrawBool(rng), RandomMoment(rng));
    const bool counts = live.tuning.autoMask && live.tuning.skinStructure.Get() >= 0.0f;
    return Contains(stem.Get(), "_skinstruc-") == counts;
}

[[nodiscard]] bool TheStemCountsPassesAndIntensityWhenTheyAreNotTheDefault(infra::RngState& rng) noexcept
{
    const LiveSettings live = RandomLive(rng);
    const bool everything = proptest::DrawBool(rng);
    const CaptureStem stem = CaptureStemOf(CaptureLabelOf(L"Desktop"), live, everything, RandomMoment(rng));
    const bool passes = Contains(stem.Get(), "x_") && Contains(stem.Get(), std::string("_") + std::to_string(live.passes.Get()) + "x_");
    const bool intensity = Contains(stem.Get(), "_Intensity-");
    const long percent = static_cast<long>(live.tuning.intensity.Get() * 100.0f + 0.5f);
    return passes == (live.passes.Get() > 1 || everything) && intensity == (percent != 100 || everything);
}

[[nodiscard]] bool TheStemBeginsWithTheLabelAndEndsWithTheMoment(infra::RngState& rng) noexcept
{
    const LiveSettings live = RandomLive(rng);
    const CaptureMoment when = RandomMoment(rng);
    const CaptureStem stem = CaptureStemOf(CaptureLabelOf(L"Cyberpunk 2077"), live, false, when);
    const std::string ending = "_" + std::to_string(when.month) + "-" + std::to_string(when.day) + "--" + std::to_string(when.hour) + "-" + std::to_string(when.minute);
    return stem.Get().starts_with("Cyberpunk2077_struc-") && EndsWith(stem.Get(), ending);
}

[[nodiscard]] bool NumberingAddsACountFromTwo(infra::RngState& rng) noexcept
{
    const CaptureStem stem = CaptureStemOf(CaptureLabelOf(L"Desktop"), RandomLive(rng), false, RandomMoment(rng));
    const std::uint32_t attempt = proptest::DrawBetween(rng, 1, 40);
    const CaptureStem numbered = NumberedStem(stem, attempt);
    if (attempt <= 1)
        return numbered == stem;
    return numbered.Get() == std::string(stem.Get()) + "_" + std::to_string(attempt);
}

[[nodiscard]] bool ThePlainStemIsTheLabelAndTheMoment(infra::RngState& rng) noexcept
{
    const CaptureMoment when = RandomMoment(rng);
    const CaptureLabel label = CaptureLabelOf(RandomTitle(rng));
    const std::string moment = std::to_string(when.month) + "-" + std::to_string(when.day) + "--" + std::to_string(when.hour) + "-" + std::to_string(when.minute);
    const bool plain = PlainStemOf(label, when).Get() == std::string(label.Get()) + "_" + moment;
    const bool folder = ComparisonFolderStemOf(label, when).Get() == std::string(label.Get()) + "_multicapture_" + moment;
    return plain && folder;
}

[[nodiscard]] bool TheDefaultFolderIsCapturesBesideTheExecutable(infra::RngState& rng) noexcept
{
    const std::wstring directory = std::wstring(L"C:\\Games\\") + std::wstring(proptest::DrawBelow(rng, 20), L'x');
    const DirectoryPath folder = DefaultCaptureFolder(DirectoryPath::Parse(directory).value_or(DirectoryPath{}));
    return folder.Get() == directory + L"\\Captures";
}

} // namespace

std::uint32_t CaptureNameSuite(std::uint64_t seed) noexcept
{
    std::uint32_t failures = 0;
    failures += Failures(proptest::ForAll("The default folder is Captures beside the executable", seed, 100, TheDefaultFolderIsCapturesBesideTheExecutable));
    failures += Failures(proptest::ForAll("The label keeps only letters and digits", seed, 300, TheLabelKeepsOnlyLettersAndDigits));
    failures += Failures(proptest::ForAll("The stem names the mask as it is", seed, 300, TheStemNamesTheMaskAsItIs));
    failures += Failures(proptest::ForAll("The stem gives skin only when it counts", seed, 300, TheStemGivesSkinOnlyWhenItCounts));
    failures += Failures(proptest::ForAll("The stem counts passes and intensity when they are not the default", seed, 300, TheStemCountsPassesAndIntensityWhenTheyAreNotTheDefault));
    failures += Failures(proptest::ForAll("The stem begins with the label and ends with the moment", seed, 300, TheStemBeginsWithTheLabelAndEndsWithTheMoment));
    failures += Failures(proptest::ForAll("Numbering adds a count from two", seed, 300, NumberingAddsACountFromTwo));
    failures += Failures(proptest::ForAll("The plain stem is the label and the moment", seed, 300, ThePlainStemIsTheLabelAndTheMoment));
    return failures;
}

} // namespace tests
