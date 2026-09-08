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
