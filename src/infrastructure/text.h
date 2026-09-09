#pragma once
#include "infrastructure/bounded_string.h"
#include "infrastructure/contracts.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <format>
#include <optional>
#include <string_view>
#include <utility>

namespace infra {

[[nodiscard]] constexpr bool IsUpperAscii(wchar_t c) noexcept
{
    return c >= L'A' && c <= L'Z';
}

[[nodiscard]] constexpr wchar_t LowerAscii(wchar_t c) noexcept
{
    return IsUpperAscii(c) ? static_cast<wchar_t>(c + (L'a' - L'A')) : c;
}

[[nodiscard]] constexpr bool IsAscii(wchar_t c) noexcept
{
    return c > 0 && c < 128;
}

[[nodiscard]] inline bool IsAllAscii(std::wstring_view text) noexcept
{
    return std::ranges::all_of(text, IsAscii);
}

[[nodiscard]] inline bool EqualsIgnoringCase(std::wstring_view a, std::wstring_view b) noexcept
{
    return std::ranges::equal(a, b, [](wchar_t x, wchar_t y) { return LowerAscii(x) == LowerAscii(y); });
}

template <std::size_t N, std::size_t... I>
[[nodiscard]] constexpr std::array<char, N> NarrowedCharsImpl(std::wstring_view text, std::index_sequence<I...>) noexcept
{
    return std::array<char, N>{ (I < text.size() ? static_cast<char>(text[I]) : char{})... };
}

template <std::size_t N>
[[nodiscard]] constexpr std::array<char, N> NarrowedChars(std::wstring_view text) noexcept
{
    return NarrowedCharsImpl<N>(text, std::make_index_sequence<N>{});
}

// Whether one piece of text appears in another, ignoring the case of the ASCII letters in both.
[[nodiscard]] inline bool ContainsIgnoringCase(std::wstring_view text, std::wstring_view wanted) noexcept
{
    const auto same = [](wchar_t x, wchar_t y) { return LowerAscii(x) == LowerAscii(y); };
    return std::ranges::search(text, wanted, same).begin() != text.end();
}

template <std::size_t N, std::size_t... I>
[[nodiscard]] constexpr std::array<wchar_t, N> WidenedCharsImpl(std::string_view text, std::index_sequence<I...>) noexcept
{
    return std::array<wchar_t, N>{ (I < text.size() ? static_cast<wchar_t>(static_cast<unsigned char>(text[I])) : wchar_t{})... };
}

// Widening is a character-for-character copy, which is only right for text that is already ASCII.
template <std::size_t N>
[[nodiscard]] constexpr std::array<wchar_t, N> WidenedChars(std::string_view text) noexcept
{
    return WidenedCharsImpl<N>(text, std::make_index_sequence<N>{});
}

template <class T>
struct Choice
{
    std::wstring_view name;
    T value;
};

template <class T, std::size_t N>
[[nodiscard]] std::optional<T> FindChoice(const std::array<Choice<T>, N>& choices, std::wstring_view text) noexcept
{
    const auto found = std::ranges::find_if(choices, [text](const Choice<T>& c) { return EqualsIgnoringCase(c.name, text); });
    if (found == choices.end())
        return std::nullopt;
    return found->value;
}

[[nodiscard]] constexpr std::size_t ClampedLength(std::ptrdiff_t written, std::size_t capacity) noexcept
{
    return std::min(static_cast<std::size_t>(std::max<std::ptrdiff_t>(written, 0)), capacity);
}

// Formats into a bounded string; output beyond N characters is cut, never allocated.
template <std::size_t N, class... Args>
[[nodiscard]] BoundedString<char, N> Formatted(std::format_string<Args...> format, Args&&... args) noexcept
{
    std::array<char, N> buffer{};
    const std::format_to_n_result<char*> written = std::format_to_n(buffer.data(), static_cast<std::ptrdiff_t>(N), format, std::forward<Args>(args)...);
    const Result<BoundedString<char, N>, StringTooLong> text = BoundedString<char, N>::Parse(std::string_view(buffer.data(), ClampedLength(written.size, N)));
    ENSURE(text.has_value());
    return *text;
}

} // namespace infra
