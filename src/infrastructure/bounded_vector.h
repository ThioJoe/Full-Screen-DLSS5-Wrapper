#pragma once
#include "infrastructure/array_util.h"
#include "infrastructure/contracts.h"
#include "infrastructure/result.h"

#include <array>
#include <cstddef>
#include <optional>
#include <ranges>
#include <span>

namespace infra {

struct CapacityExceeded
{
    [[nodiscard]] friend constexpr bool operator==(const CapacityExceeded&, const CapacityExceeded&) noexcept = default;
};

namespace detail {

struct DerefOptional
{
    template <class T>
    [[nodiscard]] constexpr const T& operator()(const std::optional<T>& item) const noexcept
    {
        return *item;
    }
};

} // namespace detail

// GROWTH-SITE: every instance is capped at N elements; Push rejects with CapacityExceeded.
template <class T, std::size_t N>
class BoundedVector final
{
public:
    static constexpr std::size_t Capacity = N;

    constexpr BoundedVector() noexcept = default;

    [[nodiscard]] constexpr std::size_t Size() const noexcept { return size_; }
    [[nodiscard]] constexpr bool IsEmpty() const noexcept { return size_ == 0; }
    [[nodiscard]] constexpr bool IsFull() const noexcept { return size_ == N; }
    [[nodiscard]] constexpr bool HasIndex(std::size_t index) const noexcept { return index < size_; }

    [[nodiscard]] constexpr const T& At(std::size_t index) const noexcept
    {
        REQUIRE(HasIndex(index));
        return *items_[index];
    }

    [[nodiscard]] constexpr const T& Last() const noexcept
    {
        REQUIRE(!IsEmpty());
        return *items_[size_ - 1];
    }

    [[nodiscard]] constexpr auto Items() const noexcept
    {
        return std::span<const std::optional<T>>(items_.data(), size_) | std::views::transform(detail::DerefOptional{});
    }

    [[nodiscard]] constexpr Result<BoundedVector, CapacityExceeded> Push(const T& item) const noexcept
    {
        if (IsFull())
            return Fail(CapacityExceeded{});
        return BoundedVector(WithElement(items_, size_, std::optional<T>{ item }), size_ + 1);
    }

    [[nodiscard]] friend constexpr bool operator==(const BoundedVector&, const BoundedVector&) noexcept = default;

private:
    constexpr BoundedVector(const std::array<std::optional<T>, N>& items, std::size_t size) noexcept : items_(items), size_(size) {}

    std::array<std::optional<T>, N> items_{};
    std::size_t size_ = 0;
};

} // namespace infra
