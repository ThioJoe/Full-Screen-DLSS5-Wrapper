#pragma once
#include "infrastructure/array_util.h"
#include "infrastructure/contracts.h"
#include "infrastructure/result.h"

#include <algorithm>
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
        std::array<Char, N + 1> chars{};
        std::copy_n(text.data(), text.size(), chars.data()); // WAIVER(R2): the fresh buffer is filled once before it is returned.
        return chars;
    }

    std::array<Char, N + 1> chars_{};
    std::size_t size_ = 0;
};

} // namespace infra
