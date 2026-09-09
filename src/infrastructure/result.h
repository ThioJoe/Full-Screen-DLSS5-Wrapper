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

// The other way about: something that may be missing, with a reason to give when it is.
template <class T, class E>
[[nodiscard]] constexpr Result<T, E> AsResult(const std::optional<T>& value, const E& absent) noexcept
{
    if (!value.has_value())
        return Fail(absent);
    return *value;
}

} // namespace infra
