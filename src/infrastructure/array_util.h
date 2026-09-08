#pragma once
#include "infrastructure/contracts.h"

#include <array>
#include <cstddef>
#include <utility>

namespace infra {

namespace detail {

template <class T, std::size_t N, class F, std::size_t... I>
[[nodiscard]] constexpr std::array<T, N> GeneratedImpl(F generator, std::index_sequence<I...>) noexcept
{
    return std::array<T, N>{ generator(I)... };
}

template <class T, std::size_t N, std::size_t... I>
[[nodiscard]] constexpr std::array<T, N> FilledImpl(const T& value, std::index_sequence<I...>) noexcept
{
    return std::array<T, N>{ ((void)I, value)... };
}

} // namespace detail

// A pack expansion over N elements makes MSVC's optimiser take minutes for large arrays of variants,
// so the replacement copies the array and writes the one slot before the copy escapes.
template <class T, std::size_t N>
[[nodiscard]] constexpr std::array<T, N> WithElement(const std::array<T, N>& source, std::size_t index, const T& value) noexcept
{
    REQUIRE(index < N);
    std::array<T, N> copy = source;
    copy[index] = value; // WAIVER(R2): the fresh copy is written exactly once before it is returned.
    return copy;
}

template <class T, std::size_t N>
[[nodiscard]] constexpr std::array<T, N> Filled(const T& value) noexcept
{
    return detail::FilledImpl<T, N>(value, std::make_index_sequence<N>{});
}

template <class T, std::size_t N, class F>
[[nodiscard]] constexpr std::array<T, N> Generated(F generator) noexcept
{
    return detail::GeneratedImpl<T, N>(generator, std::make_index_sequence<N>{});
}

} // namespace infra
