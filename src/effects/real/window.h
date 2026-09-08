#pragma once
#include "effects/real/com.h"
#include "interior/monitors.h"

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

[[nodiscard]] infra::Status<Error> SetDpiAwareness() noexcept;
[[nodiscard]] infra::Result<interior::MonitorList, Error> EnumerateMonitors() noexcept;
[[nodiscard]] infra::Result<OutputWindow, Error> CreateOutputWindow(const interior::ScreenRect& rect, const WindowSettings& settings) noexcept;
[[nodiscard]] infra::Status<Error> RegisterHotkeys(const OutputWindow& window) noexcept;
void ShowOutputWindow(const OutputWindow& window) noexcept;
[[nodiscard]] infra::Result<WindowEvents, Error> PumpEvents(const OutputWindow& window) noexcept;

} // namespace real
