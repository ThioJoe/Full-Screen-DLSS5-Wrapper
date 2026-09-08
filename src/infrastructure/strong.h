#pragma once
#include <compare>

namespace infra {

template <class T, class Tag>
class Strong final
{
public:
    using Value = T;

    [[nodiscard]] constexpr T Get() const noexcept { return value_; }

    [[nodiscard]] friend constexpr bool operator==(const Strong&, const Strong&) noexcept = default;
    [[nodiscard]] friend constexpr auto operator<=>(const Strong&, const Strong&) noexcept = default;

private:
    friend Tag;
    explicit constexpr Strong(T value) noexcept : value_(value) {}
    T value_;
};

} // namespace infra
