#pragma once
#include "effects/real/com.h"
#include "interior/monitors.h"

#include <optional>

namespace real {

struct WindowSettings
{
    bool topmost;
    bool clickThrough;
    bool excludeFromCapture;
    bool redirectionBitmap;
};

struct OutputWindow
{
    UniqueWindow handle;
    interior::ScreenRect rect;
};

struct WindowEvents
{
    bool quit;
    bool toggleOriginal;
    bool toggleSplit;
};

constexpr int kHotkeyToggleOriginal = 1;
constexpr int kHotkeyToggleSplit = 2;
constexpr int kHotkeyQuit = 3;

// Registers a window class, treating "already registered" as success. Shared with the control panel.
[[nodiscard]] infra::Status<Error> RegisterWindowClass(const WNDCLASSEXW& description) noexcept;
// Starts this program again with different arguments and leaves it running; used when the operator asks
// the panel for a session the current one cannot become.
[[nodiscard]] infra::Status<Error> StartProcess(std::wstring_view executable, std::wstring_view arguments) noexcept;

[[nodiscard]] infra::Status<Error> SetDpiAwareness() noexcept;
[[nodiscard]] infra::Result<interior::MonitorList, Error> EnumerateMonitors() noexcept;
// The first visible top-level window whose title contains what was asked for, as a source of its own.
[[nodiscard]] infra::Result<interior::MonitorInfo, Error> FindWindowNamed(const interior::WindowTitle& asked) noexcept;
// The title of a window, for showing which one was picked. Empty once the window has gone.
[[nodiscard]] interior::WindowTitle TitleOfWindow(interior::MonitorHandle window) noexcept;
// The top-level window under a point on the screen, skipping this program's own windows. Nothing over the
// desktop, or over anything of ours.
[[nodiscard]] std::optional<interior::MonitorHandle> WindowUnder(long x, long y) noexcept;
// Where that window is now, so the overlay can follow it. Nothing once the window has gone.
[[nodiscard]] std::optional<interior::ScreenRect> BoundsOfWindow(interior::MonitorHandle window) noexcept;

// Whether a window is still something to capture. Closed, hidden and minimised all answer no, and a
// session following such a window is built again for the monitor its source names.
[[nodiscard]] bool IsWindowShowing(interior::MonitorHandle window) noexcept;
// Moves the overlay so its top-left sits where the given point is. Its size is the session's and stays.
void MoveOutputWindow(const OutputWindow& window, const interior::ScreenRect& rect) noexcept;

// Puts a window back into every capture on the machine, for when our own capture excludes it by name.
[[nodiscard]] infra::Status<Error> UncoverWindow(HWND window) noexcept;
[[nodiscard]] infra::Result<OutputWindow, Error> CreateOutputWindow(const interior::ScreenRect& rect, const WindowSettings& settings) noexcept;
[[nodiscard]] infra::Status<Error> RegisterHotkeys(const OutputWindow& window) noexcept;
void ShowOutputWindow(const OutputWindow& window) noexcept;
[[nodiscard]] infra::Result<WindowEvents, Error> PumpEvents(const OutputWindow& window) noexcept;

// Changes how the output window behaves: whether the capture sees it, whether it stays above everything,
// and whether the mouse passes through it. The redirection surface is fixed when the window is made.
[[nodiscard]] infra::Status<Error> ApplyWindowSettings(const OutputWindow& window, const WindowSettings& settings) noexcept;

// Where the split divider should sit, or nothing when it is not being dragged. Holding the hotkey
// modifiers and moving the cursor drags it; no button is involved, so the desktop keeps its clicks.
[[nodiscard]] std::optional<interior::Fraction> SplitRequest(const OutputWindow& window) noexcept;

} // namespace real
