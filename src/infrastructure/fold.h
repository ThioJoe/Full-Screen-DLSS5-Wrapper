#pragma once
#include "infrastructure/bounded_vector.h"
#include "infrastructure/result.h"

#include <algorithm>
#include <ranges>
#include <span>

namespace infra {

// Fold that stops at the first error: a step returning unexpected short-circuits the rest.
template <class Range, class State, class E, class Step>
[[nodiscard]] constexpr Result<State, E> FoldResult(const Range& range, Result<State, E> initial, Step step) noexcept
{
    return std::ranges::fold_left(range, initial, [&step](Result<State, E> acc, const auto& item) -> Result<State, E> { return acc.and_then([&](const State& state) { return step(state, item); }); });
}

// Status fold: runs step on every item until the first error.
template <class Range, class E, class Step>
[[nodiscard]] constexpr Status<E> ForEach(const Range& range, Status<E> initial, Step step) noexcept
{
    return std::ranges::fold_left(range, initial, [&step](Status<E> acc, const auto& item) -> Status<E> { return acc.and_then([&] { return step(item); }); });
}

} // namespace infra
