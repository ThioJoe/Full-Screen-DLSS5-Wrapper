#include "interior/units.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <ranges>

namespace interior {
namespace {

constexpr std::array<std::size_t, 4> kDashPositions{ 8, 13, 18, 23 };

[[nodiscard]] bool IsDecimalDigit(char c) noexcept
{
    return c >= '0' && c <= '9';
}

[[nodiscard]] bool IsLowerHexLetter(char c) noexcept
{
    return c >= 'a' && c <= 'f';
}

[[nodiscard]] bool IsHexDigit(char c) noexcept
{
    return IsDecimalDigit(c) || IsLowerHexLetter(c);
}

[[nodiscard]] bool IsDashPosition(std::size_t index) noexcept
{
    return std::ranges::find(kDashPositions, index) != kDashPositions.end();
}

[[nodiscard]] bool IsValidGuidChar(char c, std::size_t index) noexcept
{
    return IsDashPosition(index) ? c == '-' : IsHexDigit(c);
}

[[nodiscard]] bool IsGuidShaped(std::string_view text) noexcept
{
    return std::ranges::all_of(std::views::iota(std::size_t{ 0 }, text.size()), [text](std::size_t i) { return IsValidGuidChar(text[i], i); });
}

[[nodiscard]] bool IsGuidLength(std::string_view text) noexcept
{
    return text.size() == ProjectIdText::Capacity;
}

[[nodiscard]] bool IsGuid(std::string_view text) noexcept
{
    return IsGuidLength(text) && IsGuidShaped(text);
}

} // namespace

Result<ProjectIdText, UnitError> ParseProjectId(std::string_view raw) noexcept
{
    if (!IsGuid(raw))
        return infra::Fail(UnitError::ProjectIdMalformed);
    return ProjectIdText::Parse(raw).transform_error([](infra::StringTooLong) { return UnitError::ProjectIdMalformed; });
}

} // namespace interior
