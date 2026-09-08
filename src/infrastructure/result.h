#pragma once
#include <expected>
#include <optional>

namespace infra {

template <class T, class E>
using Result = std::expected<T, E>;

template <class E>
using Status = std::expected<void, E>;

template <class E>
[[nodiscard]] constexpr std::unexpected<E> Fail(E error) noexcept
{
    return std::unexpected<E>(error);
}

template <class T, class E>
[[nodiscard]] constexpr std::optional<T> AsOptional(const Result<T, E>& result) noexcept
{
    if (!result.has_value())
        return std::nullopt;
    return *result;
}

} // namespace infra
