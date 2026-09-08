#include "infrastructure/trace.h"

#include "infrastructure/contracts.h"

#include <array>
#include <cstdio>
#include <cstdlib>

// WAIVER(R11): the trace ring is written by compiler-inserted hooks that carry no context; it is the one global.
// WAIVER(R2): the ring index and entries are the hooks' output channel and are overwritten in place.
namespace infra {
namespace {

constexpr std::uint64_t kTraceCapacity = 65536; // GROWTH-SITE: ring of 65536 entries, oldest evicted.

std::array<TraceEntry, kTraceCapacity> g_ring{};
std::uint64_t g_sequence = 0;

void Record(const void* function, bool enter) noexcept
{
    const std::uint64_t index = g_sequence % kTraceCapacity;
    g_ring[index] = TraceEntry{ function, g_sequence, enter };
    g_sequence = g_sequence + 1;
}

[[nodiscard]] const char* Direction(bool enter) noexcept
{
    return enter ? "enter" : "exit ";
}

void PrintEntry(std::FILE* sink, const TraceEntry& entry) noexcept
{
    if (entry.function != nullptr)
        std::fprintf(sink, "%llu %s %p\n", static_cast<unsigned long long>(entry.sequence), Direction(entry.enter), entry.function);
}

} // namespace

void DumpTrace() noexcept
{
    std::FILE* sink = std::fopen("dlssscreen-trace.txt", "w");
    if (sink == nullptr)
        return;
    std::fprintf(sink, "--- trace (%llu calls recorded, newest last) ---\n", static_cast<unsigned long long>(g_sequence));
    for (const TraceEntry& entry : g_ring) // WAIVER(R2): dumping the ring during a panic.
        PrintEntry(sink, entry);
    std::fclose(sink);
}

void ContractViolation(const char* predicate, const char* file, int line) noexcept
{
    std::fflush(stdout);
    std::fprintf(stderr, "CONTRACT VIOLATION: %s at %s:%d (trace written to dlssscreen-trace.txt)\n", predicate, file, line);
    DumpTrace();
    std::fflush(stderr);
    std::abort();
}

} // namespace infra

extern "C" {

void TraceEnter(const void* function) noexcept
{
    infra::Record(function, true);
}

void TraceExit(const void* function) noexcept
{
    infra::Record(function, false);
}

#if !defined(_MSC_VER)
__attribute__((no_instrument_function)) void __cyg_profile_func_enter(void* function, void*) noexcept
{
    infra::Record(function, true);
}

__attribute__((no_instrument_function)) void __cyg_profile_func_exit(void* function, void*) noexcept
{
    infra::Record(function, false);
}
#endif
}
