#pragma once
#include "effects/real/com.h"
#include "interior/enums.h"
#include "interior/units.h"

#include <cstdio>
#include <memory>
#include <string_view>

namespace real {

struct FileCloser
{
    void operator()(std::FILE* file) const noexcept { ENSURE(std::fclose(file) == 0); }
};
using UniqueFile = std::unique_ptr<std::FILE, FileCloser>;

struct Console
{
    interior::LogLevel minimum;
    std::shared_ptr<std::FILE> mirror;
    bool attached; // false when the program has no console: launched by a double-click, or --console off
};

// Attaches to the console the program was launched from, makes one, or goes without, per the mode. A
// program started from Explorer has no parent console and, on Auto, gets no window of its own.
[[nodiscard]] infra::Result<Console, Error> OpenConsole(interior::LogLevel minimum, const interior::DirectoryPath& logFile, interior::ConsoleMode mode) noexcept;

// Says something to an operator who has no console to read. Used for the failures that would otherwise
// leave a double-clicked program closing in silence.
void ShowMessage(std::string_view text) noexcept;
[[nodiscard]] infra::Status<Error> Log(const Console& console, interior::LogLevel level, std::string_view text) noexcept;
[[nodiscard]] infra::Status<Error> WriteText(std::FILE* sink, std::string_view text) noexcept;

} // namespace real
