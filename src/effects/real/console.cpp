#include "effects/real/console.h"

#include <array>
#include <cstring>

namespace real {
namespace {

using infra::Fail;
using infra::Result;
using infra::Status;

[[nodiscard]] const char* Tag(interior::LogLevel level) noexcept
{
    switch (level)
    {
    case interior::LogLevel::Debug: return "[debug] ";
    case interior::LogLevel::Info: return "[info]  ";
    case interior::LogLevel::Warn: return "[warn]  ";
    case interior::LogLevel::Error: return "[error] ";
    }
    return "";
}

[[nodiscard]] Status<Error> Flushed(std::FILE* sink) noexcept
{
    if (std::fflush(sink) != 0)
        return Fail(Error{ ApiCall::WriteLog, 1 });
    return {};
}

[[nodiscard]] Status<Error> WriteLine(std::FILE* sink, interior::LogLevel level, std::string_view text) noexcept
{
    return WriteText(sink, Tag(level)).and_then([&] { return WriteText(sink, text); }).and_then([&] { return WriteText(sink, "\n"); }).and_then([&] { return Flushed(sink); });
}

[[nodiscard]] std::FILE* StreamFor(interior::LogLevel level) noexcept
{
    return level >= interior::LogLevel::Warn ? stderr : stdout;
}

[[nodiscard]] Status<Error> WriteMirror(const Console& console, interior::LogLevel level, std::string_view text) noexcept
{
    if (!console.mirror)
        return {};
    return WriteLine(console.mirror.get(), level, text);
}

[[nodiscard]] Result<std::shared_ptr<std::FILE>, Error> OpenedFile(const interior::DirectoryPath& logFile) noexcept
{
    std::FILE* file = _wfopen(logFile.CString(), L"a");
    if (file == nullptr)
        return Fail(Error{ ApiCall::OpenLogFile, static_cast<std::uint32_t>(errno) });
    return std::shared_ptr<std::FILE>(file, FileCloser{});
}

[[nodiscard]] Result<std::shared_ptr<std::FILE>, Error> OpenMirror(const interior::DirectoryPath& logFile) noexcept
{
    if (logFile.IsEmpty())
        return std::shared_ptr<std::FILE>{};
    return OpenedFile(logFile);
}

} // namespace

Result<Console, Error> OpenConsole(interior::LogLevel minimum, const interior::DirectoryPath& logFile) noexcept
{
    return OpenMirror(logFile).transform([minimum](const std::shared_ptr<std::FILE>& mirror) { return Console{ minimum, mirror }; });
}

Status<Error> Log(const Console& console, interior::LogLevel level, std::string_view text) noexcept
{
    if (level < console.minimum)
        return {};
    return WriteLine(StreamFor(level), level, text).and_then([&] { return WriteMirror(console, level, text); });
}

Status<Error> WriteText(std::FILE* sink, std::string_view text) noexcept
{
    const std::size_t written = std::fwrite(text.data(), 1, text.size(), sink);
    if (written != text.size())
        return Fail(Error{ ApiCall::WriteLog, static_cast<std::uint32_t>(written) });
    return {};
}

} // namespace real
