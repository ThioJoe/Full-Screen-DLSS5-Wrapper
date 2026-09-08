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
};

[[nodiscard]] infra::Result<Console, Error> OpenConsole(interior::LogLevel minimum, const interior::DirectoryPath& logFile) noexcept;
[[nodiscard]] infra::Status<Error> Log(const Console& console, interior::LogLevel level, std::string_view text) noexcept;
[[nodiscard]] infra::Status<Error> WriteText(std::FILE* sink, std::string_view text) noexcept;

} // namespace real
