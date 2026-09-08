#pragma once
#include <cstdint>

namespace infra {

struct TraceEntry
{
    const void* function = nullptr;
    std::uint64_t sequence = 0;
    bool enter = false;
};

// Writes every recorded entry to the trace sink; called only by ContractViolation.
void DumpTrace() noexcept;

} // namespace infra
