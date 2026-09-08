#pragma once
#include "infrastructure/array_util.h"
#include "infrastructure/contracts.h"
#include "infrastructure/result.h"

#include <array>
#include <cstddef>
#include <string_view>

namespace infra {

struct StringTooLong
{
    [[nodiscard]] friend constexpr bool operator==(const StringTooLong&, const StringTooLong&) noexcept = default;
};

// GROWTH-SITE: capped at N characters; Parse rejects longer input with StringTooLong.
template <class Char, std::size_t N>
class BoundedString final
{
public:
    static constexpr std::size_t Capacity = N;
    using View = std::basic_string_view<Char>;

    constexpr BoundedString() noexcept = default;

    [[nodiscard]] static constexpr Result<BoundedString, StringTooLong> Parse(View text) noexcept
    {
        if (text.size() > N)
            return Fail(StringTooLong{});
        return BoundedString(text);
    }

    [[nodiscard]] constexpr View Get() const noexcept { return View(chars_.data(), size_); }
    [[nodiscard]] constexpr std::size_t Size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr const Char* CString() const noexcept { return chars_.data(); }

    [[nodiscard]] friend constexpr bool operator==(const BoundedString&, const BoundedString&) noexcept = default;

private:
    explicit constexpr BoundedString(View text) noexcept : chars_(Copied(text)), size_(text.size()) {}

    [[nodiscard]] static constexpr std::array<Char, N + 1> Copied(View text) noexcept
    {
        return CopiedImpl(text, std::make_index_sequence<N + 1>{});
    }

    template <std::size_t... I>
    [[nodiscard]] static constexpr std::array<Char, N + 1> CopiedImpl(View text, std::index_sequence<I...>) noexcept
    {
        return std::array<Char, N + 1>{ (I < text.size() ? text[I] : Char{})... };
    }

    std::array<Char, N + 1> chars_{};
    std::size_t size_ = 0;
};

} // namespace infra
