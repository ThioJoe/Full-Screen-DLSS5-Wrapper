#include "infrastructure/checked.h"

#include <cstdint>
#include <limits>

namespace infra {

namespace {

[[nodiscard]] bool IsZero(std::uint32_t value) noexcept
{
    return value == 0;
}

} // namespace

#if defined(_MSC_VER)

namespace {

[[nodiscard]] bool IsOutsideInt32(std::int64_t value) noexcept
{
    return value < std::numeric_limits<std::int32_t>::min() || value > std::numeric_limits<std::int32_t>::max();
}

[[nodiscard]] Result<std::uint32_t, ArithmeticError> NarrowedUnsigned(std::uint64_t value) noexcept
{
    if (value > std::numeric_limits<std::uint32_t>::max())
        return Fail(ArithmeticError::Overflow);
    return static_cast<std::uint32_t>(value);
}

[[nodiscard]] Result<std::int32_t, ArithmeticError> NarrowedSigned(std::int64_t value) noexcept
{
    if (IsOutsideInt32(value))
        return Fail(ArithmeticError::Overflow);
    return static_cast<std::int32_t>(value);
}

[[nodiscard]] bool WouldWrapAdding(std::uint64_t a, std::uint64_t b) noexcept
{
    return a > std::numeric_limits<std::uint64_t>::max() - b;
}

[[nodiscard]] bool WouldWrapMultiplying(std::uint64_t a, std::uint64_t b) noexcept
{
    return a != 0 && b > std::numeric_limits<std::uint64_t>::max() / a;
}

} // namespace

Result<std::uint32_t, ArithmeticError> CheckedAdd(std::uint32_t a, std::uint32_t b) noexcept
{
    return NarrowedUnsigned(static_cast<std::uint64_t>(a) + b);
}

Result<std::uint32_t, ArithmeticError> CheckedSub(std::uint32_t a, std::uint32_t b) noexcept
{
    if (b > a)
        return Fail(ArithmeticError::Overflow);
    return a - b;
}

Result<std::uint32_t, ArithmeticError> CheckedMul(std::uint32_t a, std::uint32_t b) noexcept
{
    return NarrowedUnsigned(static_cast<std::uint64_t>(a) * b);
}

Result<std::uint64_t, ArithmeticError> CheckedAdd(std::uint64_t a, std::uint64_t b) noexcept
{
    if (WouldWrapAdding(a, b))
        return Fail(ArithmeticError::Overflow);
    return a + b;
}

Result<std::uint64_t, ArithmeticError> CheckedSub(std::uint64_t a, std::uint64_t b) noexcept
{
    if (b > a)
        return Fail(ArithmeticError::Overflow);
    return a - b;
}

Result<std::uint64_t, ArithmeticError> CheckedMul(std::uint64_t a, std::uint64_t b) noexcept
{
    if (WouldWrapMultiplying(a, b))
        return Fail(ArithmeticError::Overflow);
    return a * b;
}

Result<std::int32_t, ArithmeticError> CheckedAdd(std::int32_t a, std::int32_t b) noexcept
{
    return NarrowedSigned(static_cast<std::int64_t>(a) + b);
}

Result<std::int32_t, ArithmeticError> CheckedSub(std::int32_t a, std::int32_t b) noexcept
{
    return NarrowedSigned(static_cast<std::int64_t>(a) - b);
}

Result<std::int32_t, ArithmeticError> CheckedMul(std::int32_t a, std::int32_t b) noexcept
{
    return NarrowedSigned(static_cast<std::int64_t>(a) * b);
}

#else

namespace {

template <class T>
[[nodiscard]] Result<T, ArithmeticError> FromBuiltin(bool overflowed, T value) noexcept
{
    if (overflowed)
        return Fail(ArithmeticError::Overflow);
    return value;
}

} // namespace

Result<std::uint32_t, ArithmeticError> CheckedAdd(std::uint32_t a, std::uint32_t b) noexcept
{
    std::uint32_t out = 0;
    const auto status = __builtin_add_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

Result<std::uint32_t, ArithmeticError> CheckedSub(std::uint32_t a, std::uint32_t b) noexcept
{
    std::uint32_t out = 0;
    const auto status = __builtin_sub_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

Result<std::uint32_t, ArithmeticError> CheckedMul(std::uint32_t a, std::uint32_t b) noexcept
{
    std::uint32_t out = 0;
    const auto status = __builtin_mul_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

Result<std::uint64_t, ArithmeticError> CheckedAdd(std::uint64_t a, std::uint64_t b) noexcept
{
    std::uint64_t out = 0;
    const auto status = __builtin_add_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

Result<std::uint64_t, ArithmeticError> CheckedSub(std::uint64_t a, std::uint64_t b) noexcept
{
    std::uint64_t out = 0;
    const auto status = __builtin_sub_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

Result<std::uint64_t, ArithmeticError> CheckedMul(std::uint64_t a, std::uint64_t b) noexcept
{
    std::uint64_t out = 0;
    const auto status = __builtin_mul_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

Result<std::int32_t, ArithmeticError> CheckedAdd(std::int32_t a, std::int32_t b) noexcept
{
    std::int32_t out = 0;
    const auto status = __builtin_add_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

Result<std::int32_t, ArithmeticError> CheckedSub(std::int32_t a, std::int32_t b) noexcept
{
    std::int32_t out = 0;
    const auto status = __builtin_sub_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

Result<std::int32_t, ArithmeticError> CheckedMul(std::int32_t a, std::int32_t b) noexcept
{
    std::int32_t out = 0;
    const auto status = __builtin_mul_overflow(a, b, &out);
    return FromBuiltin(status, out);
}

#endif

Result<std::uint32_t, ArithmeticError> CheckedDiv(std::uint32_t a, std::uint32_t b) noexcept
{
    if (IsZero(b))
        return Fail(ArithmeticError::DivisionByZero);
    return a / b;
}

} // namespace infra
