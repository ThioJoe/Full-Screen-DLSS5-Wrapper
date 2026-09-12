#include "interior/capture_name.h"

#include "infrastructure/array_util.h"
#include "infrastructure/text.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

namespace interior {
namespace {

using Part = infra::BoundedString<char, 32>;

struct Kept
{
    std::array<char, CaptureLabel::Capacity> chars;
    std::size_t count;
};

[[nodiscard]] bool IsLetterOrDigit(wchar_t c) noexcept
{
    return (c >= L'0' && c <= L'9') || (c >= L'A' && c <= L'Z') || (c >= L'a' && c <= L'z');
}

[[nodiscard]] std::string_view StyleWord(NrStyle style) noexcept
{
    switch (style)
    {
    case NrStyle::Standard: return "standard";
    case NrStyle::Natural: return "natural";
    case NrStyle::Cinematic: return "cinematic";
    }
    return "standard";
}

[[nodiscard]] long PercentOf(NrIntensity intensity) noexcept
{
    return std::lround(100.0 * static_cast<double>(intensity.Get()));
}

// Skin structure counts only while the mask is on and skin is not following local structure, which the
// model reads a negative value as.
[[nodiscard]] Part SkinPart(const NrTuning& t) noexcept
{
    if (!t.autoMask || t.skinStructure.Get() < 0.0f)
        return Part{};
    return infra::Formatted<Part::Capacity>("_skinstruc-{:.2f}", static_cast<double>(t.skinStructure.Get()));
}

[[nodiscard]] Part MaskPart(const NrTuning& t, bool everything) noexcept
{
    if (!t.autoMask)
        return infra::Formatted<Part::Capacity>("_nomask");
    if (everything)
        return infra::Formatted<Part::Capacity>("_automask");
    return Part{};
}

[[nodiscard]] Part IntensityPart(const NrTuning& t, bool everything) noexcept
{
    const long percent = PercentOf(t.intensity);
    if (percent == 100 && !everything) // TEST BUILD: past 100 is not the default either, so it is named
        return Part{};
    return infra::Formatted<Part::Capacity>("_Intensity-{}", percent);
}

[[nodiscard]] Part PassesPart(PassCount passes, bool everything) noexcept
{
    if (passes.Get() <= 1 && !everything)
        return Part{};
    return infra::Formatted<Part::Capacity>("_{}x", passes.Get());
}

} // namespace

CaptureLabel CaptureLabelOf(std::wstring_view title) noexcept
{
    // The letters and digits met so far, as many as the label holds.
    static constexpr auto Adding = [] [[nodiscard]] (const Kept& so, wchar_t c) noexcept -> Kept {
        if (!IsLetterOrDigit(c) || so.count == so.chars.size())
            return so;
        return Kept{ infra::WithElement(so.chars, so.count, static_cast<char>(c)), so.count + 1 };
    };
    const Kept kept = std::ranges::fold_left(title, Kept{ {}, 0 }, Adding);
    const std::string_view text(kept.chars.data(), kept.count);
    if (text.empty())
        return CaptureLabel::Parse(kWindowLabel).value_or(CaptureLabel{});
    return CaptureLabel::Parse(text).value_or(CaptureLabel{});
}

CaptureStem CaptureStemOf(const CaptureLabel& label, const LiveSettings& live, bool everything, const CaptureMoment& when) noexcept
{
    const NrTuning& t = live.tuning;
    return CaptureStem::Parse(infra::Formatted<CaptureStem::Capacity>("{}_struc-{:.2f}_tone-{:.2f}_{}{}{}{}{}_{}-{}--{}-{}", label.Get(), static_cast<double>(t.localStructure.Get()),
                                                                      static_cast<double>(t.localTone.Get()), StyleWord(t.style), SkinPart(t).Get(), MaskPart(t, everything).Get(),
                                                                      IntensityPart(t, everything).Get(), PassesPart(live.passes, everything).Get(), when.month, when.day, when.hour, when.minute)
                                  .Get())
        .value_or(CaptureStem{});
}

CaptureStem PlainStemOf(const CaptureLabel& label, const CaptureMoment& when) noexcept
{
    return CaptureStem::Parse(infra::Formatted<CaptureStem::Capacity>("{}_{}-{}--{}-{}", label.Get(), when.month, when.day, when.hour, when.minute).Get()).value_or(CaptureStem{});
}

CaptureStem ComparisonFolderStemOf(const CaptureLabel& label, const CaptureMoment& when) noexcept
{
    return CaptureStem::Parse(infra::Formatted<CaptureStem::Capacity>("{}_multicapture_{}-{}--{}-{}", label.Get(), when.month, when.day, when.hour, when.minute).Get()).value_or(CaptureStem{});
}

CaptureStem NumberedStem(const CaptureStem& stem, std::uint32_t attempt) noexcept
{
    if (attempt <= 1)
        return stem;
    return CaptureStem::Parse(infra::Formatted<CaptureStem::Capacity>("{}_{}", stem.Get(), attempt).Get()).value_or(stem);
}

DirectoryPath DefaultCaptureFolder(const DirectoryPath& executableDirectory) noexcept
{
    std::array<wchar_t, DirectoryPath::Capacity + 1> path{}; // WAIVER(R2): a local buffer filled once, before use.
    const std::format_to_n_result<wchar_t*> written = std::format_to_n(path.data(), static_cast<std::ptrdiff_t>(DirectoryPath::Capacity), L"{}\\Captures", executableDirectory.Get());
    return DirectoryPath::Parse(std::wstring_view(path.data(), infra::ClampedLength(written.size, DirectoryPath::Capacity))).value_or(DirectoryPath{});
}

} // namespace interior
