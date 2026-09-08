#include "infrastructure/checked.h"

#if defined(_MSC_VER)
#include <intsafe.h>
#endif

namespace infra {

namespace {

[[nodiscard]] bool IsZero(std::uint32_t value) noexcept
{
    return value == 0;
}

#if defined(_MSC_VER)
template <class T>
[[nodiscard]] Result<T, ArithmeticError> FromIntsafe(HRESULT status, T value) noexcept
{
    if (FAILED(status))
        return Fail(ArithmeticError::Overflow);
    return value;
}
#endif

} // namespace

#if defined(_MSC_VER)

Result<std::uint32_t, ArithmeticError> CheckedAdd(std::uint32_t a, std::uint32_t b) noexcept
{
    std::uint32_t out = 0;
    const auto status = UIntAdd(a, b, &out);
    return FromIntsafe(status, out);
}

Result<std::uint32_t, ArithmeticError> CheckedSub(std::uint32_t a, std::uint32_t b) noexcept
{
    std::uint32_t out = 0;
    const auto status = UIntSub(a, b, &out);
    return FromIntsafe(status, out);
}

Result<std::uint32_t, ArithmeticError> CheckedMul(std::uint32_t a, std::uint32_t b) noexcept
{
    std::uint32_t out = 0;
    const auto status = UIntMult(a, b, &out);
    return FromIntsafe(status, out);
}

Result<std::uint64_t, ArithmeticError> CheckedAdd(std::uint64_t a, std::uint64_t b) noexcept
{
    std::uint64_t out = 0;
    const auto status = ULongLongAdd(a, b, &out);
    return FromIntsafe(status, out);
}

Result<std::uint64_t, ArithmeticError> CheckedSub(std::uint64_t a, std::uint64_t b) noexcept
{
    std::uint64_t out = 0;
    const auto status = ULongLongSub(a, b, &out);
    return FromIntsafe(status, out);
}

Result<std::uint64_t, ArithmeticError> CheckedMul(std::uint64_t a, std::uint64_t b) noexcept
{
    std::uint64_t out = 0;
    const auto status = ULongLongMult(a, b, &out);
    return FromIntsafe(status, out);
}

Result<std::int32_t, ArithmeticError> CheckedAdd(std::int32_t a, std::int32_t b) noexcept
{
    std::int32_t out = 0;
    const auto status = IntAdd(a, b, &out);
    return FromIntsafe(status, out);
}

Result<std::int32_t, ArithmeticError> CheckedSub(std::int32_t a, std::int32_t b) noexcept
{
    std::int32_t out = 0;
    const auto status = IntSub(a, b, &out);
    return FromIntsafe(status, out);
}

Result<std::int32_t, ArithmeticError> CheckedMul(std::int32_t a, std::int32_t b) noexcept
{
    std::int32_t out = 0;
    const auto status = IntMult(a, b, &out);
    return FromIntsafe(status, out);
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
