#pragma once
#include "effects/real/com.h"

#include <windows.graphics.capture.h>

#include <span>

namespace real {

// A capture session can be told to leave certain windows out of what it captures. That is scoped to the
// one session, unlike a window's display affinity, which hides it from every capture on the machine.
[[nodiscard]] bool SessionCanExcludeWindows(ABI::Windows::Graphics::Capture::IGraphicsCaptureSession* session) noexcept;

// Leaves the given windows out of this session. Answers whether the session took the list, read back from
// the session itself rather than assumed, because what follows a yes is uncovering the windows.
[[nodiscard]] bool ExcludeWindowsFrom(ABI::Windows::Graphics::Capture::IGraphicsCaptureSession* session, std::span<const HWND> windows) noexcept;

// Adds a line to the log the exclusion writes, so what was tried is all in one place.
void NoteExclusion(const char* line) noexcept;

// Adds a wide line to that log, narrowed as it goes; for command lines, which are ASCII bar their paths.
void NoteExclusionWide(const wchar_t* text) noexcept;

// Writes the session's exclusion list to that log as it stands, for asking when it stops being held.
void NoteExclusionList(ABI::Windows::Graphics::Capture::IGraphicsCaptureSession* session, const char* when) noexcept;

} // namespace real
