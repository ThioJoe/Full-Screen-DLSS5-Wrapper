#include "effects/real/window.h"

#include "infrastructure/fold.h"

#include <algorithm>
#include <array>
#include <ranges>

namespace real {

constexpr std::size_t kCommandCapacity = 2400;

namespace {

using infra::Fail;
using infra::Result;
using infra::Status;
using interior::MonitorInfo;
using interior::MonitorList;

constexpr wchar_t kClassName[] = L"DlssScreenOutputWindow";
constexpr std::uint32_t kMaxMessagesPerPump = 64;

[[nodiscard]] WNDCLASSEXW WithClassName(WNDCLASSEXW wc) noexcept;

[[nodiscard]] LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    switch (message)
    {
    case WM_NCHITTEST: return HTTRANSPARENT;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    default: return ::DefWindowProcW(window, message, wparam, lparam); // WAIVER(R12): window messages are an open set defined by the OS.
    }
}

[[nodiscard]] WNDCLASSEXW ClassDescription() noexcept
{
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &WindowProc;
    wc.hInstance = ::GetModuleHandleW(nullptr);
    return WithClassName(wc);
}

[[nodiscard]] WNDCLASSEXW WithClassName(WNDCLASSEXW wc) noexcept
{
    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kClassName;
    return wc;
}

[[nodiscard]] bool IsAlreadyRegistered() noexcept
{
    return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

[[nodiscard]] bool IsRegistrationFailure(ATOM atom) noexcept
{
    return atom == 0 && !IsAlreadyRegistered();
}

[[nodiscard]] DWORD TopmostStyle(const WindowSettings& s) noexcept
{
    return s.topmost ? WS_EX_TOPMOST : 0u;
}

[[nodiscard]] DWORD ClickThroughStyle(const WindowSettings& s) noexcept
{
    return s.clickThrough ? (WS_EX_LAYERED | WS_EX_TRANSPARENT) : 0u;
}

[[nodiscard]] DWORD RedirectionStyle(const WindowSettings& s) noexcept
{
    return s.redirectionBitmap ? 0u : WS_EX_NOREDIRECTIONBITMAP;
}

[[nodiscard]] DWORD ExtendedStyle(const WindowSettings& s) noexcept
{
    return WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | TopmostStyle(s) | ClickThroughStyle(s) | RedirectionStyle(s);
}

[[nodiscard]] int Width(const interior::ScreenRect& r) noexcept
{
    return r.Right().Get() - r.Left().Get();
}

[[nodiscard]] int Height(const interior::ScreenRect& r) noexcept
{
    return r.Bottom().Get() - r.Top().Get();
}

[[nodiscard]] Result<UniqueWindow, Error> CreateHandle(const interior::ScreenRect& rect, const WindowSettings& s) noexcept
{
    HWND handle = ::CreateWindowExW(ExtendedStyle(s), kClassName, L"DlssScreen", WS_POPUP, rect.Left().Get(), rect.Top().Get(), Width(rect), Height(rect), nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    if (handle == nullptr)
        return Fail(LastError(ApiCall::CreateWindowExW));
    return UniqueWindow(handle);
}

[[nodiscard]] Status<Error> ApplyLayering(HWND handle, const WindowSettings& s) noexcept
{
    if (!s.clickThrough)
        return {};
    return CheckBool(::SetLayeredWindowAttributes(handle, 0, 255, LWA_ALPHA), ApiCall::CreateWindowExW);
}

// Set either way, so the operator can put the window back into the capture and watch it feed back.
[[nodiscard]] Status<Error> ApplyAffinity(HWND handle, const WindowSettings& s) noexcept
{
    const DWORD affinity = s.excludeFromCapture ? WDA_EXCLUDEFROMCAPTURE : WDA_NONE;
    return CheckBool(::SetWindowDisplayAffinity(handle, affinity), ApiCall::SetWindowDisplayAffinity);
}

[[nodiscard]] HWND InsertAfter(const WindowSettings& s) noexcept
{
    return s.topmost ? HWND_TOPMOST : HWND_TOP;
}

[[nodiscard]] Status<Error> Position(HWND handle, const interior::ScreenRect& rect, const WindowSettings& s) noexcept
{
    return CheckBool(::SetWindowPos(handle, InsertAfter(s), rect.Left().Get(), rect.Top().Get(), Width(rect), Height(rect), SWP_NOACTIVATE | SWP_FRAMECHANGED), ApiCall::CreateWindowExW);
}

[[nodiscard]] Status<Error> Configure(HWND handle, const interior::ScreenRect& rect, const WindowSettings& s) noexcept
{
    return ApplyLayering(handle, s).and_then([&] { return ApplyAffinity(handle, s); }).and_then([&] { return Position(handle, rect, s); });
}

[[nodiscard]] Status<Error> RegisterOne(HWND handle, int id, UINT key) noexcept
{
    return CheckBool(::RegisterHotKey(handle, id, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, key), ApiCall::RegisterHotKey);
}

// --- monitors -----------------------------------------------------------------------------------------

struct Collector
{
    MonitorList list;
    bool overflow;
};

[[nodiscard]] Result<interior::ScreenRect, interior::UnitError> RectOf(const RECT& r) noexcept
{
    return interior::CoordinateTag::Parse(r.left).and_then([&](interior::Coordinate l) {
        return interior::CoordinateTag::Parse(r.top).and_then([&](interior::Coordinate t) {
            return interior::CoordinateTag::Parse(r.right).and_then(
                [&](interior::Coordinate rr) { return interior::CoordinateTag::Parse(r.bottom).and_then([&](interior::Coordinate b) { return interior::ScreenRectTag::Parse(l, t, rr, b); }); });
        });
    });
}

[[nodiscard]] bool IsPrimary(const MONITORINFOEXW& info) noexcept
{
    return (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
}

[[nodiscard]] std::optional<MonitorInfo> InfoOf(HMONITOR handle, const MONITORINFOEXW& info) noexcept
{
    return infra::AsOptional(interior::MonitorHandleTag::Parse(reinterpret_cast<std::uintptr_t>(handle))).and_then([&](interior::MonitorHandle h) {
        return infra::AsOptional(RectOf(info.rcMonitor)).and_then([&](const interior::ScreenRect& rect) {
            return infra::AsOptional(interior::DeviceName::Parse(std::wstring_view(info.szDevice))).transform([&](const interior::DeviceName& name) {
                return MonitorInfo{ h, rect, IsPrimary(info), name };
            });
        });
    });
}

[[nodiscard]] std::optional<MonitorInfo> QueryMonitor(HMONITOR handle) noexcept
{
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (::GetMonitorInfoW(handle, &info) == FALSE)
        return std::nullopt;
    return InfoOf(handle, info);
}

[[nodiscard]] Collector Pushed(const Collector& c, const MonitorInfo& monitor) noexcept
{
    const Result<MonitorList, infra::CapacityExceeded> pushed = c.list.Push(monitor);
    if (!pushed.has_value())
        return Collector{ c.list, true };
    return Collector{ *pushed, c.overflow };
}

[[nodiscard]] Collector Collected(const Collector& c, const std::optional<MonitorInfo>& monitor) noexcept
{
    if (!monitor.has_value())
        return c;
    return Pushed(c, *monitor);
}

[[nodiscard]] BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM param)
{
    auto* collector = reinterpret_cast<Collector*>(param);
    *collector = Collected(*collector, QueryMonitor(handle)); // WAIVER(R2): OS enumeration callback writes its output slot.
    return TRUE;
}

// --- events ---------------------------------------------------------------------------------------------

struct Pump
{
    WindowEvents events;
    bool drained;
};

[[nodiscard]] bool Either(bool a, bool b) noexcept
{
    return a || b;
}

[[nodiscard]] WindowEvents Merged(const WindowEvents& a, const WindowEvents& b) noexcept
{
    return WindowEvents{ Either(a.quit, b.quit), Either(a.toggleOriginal, b.toggleOriginal), Either(a.toggleSplit, b.toggleSplit) };
}

[[nodiscard]] bool IsKeyDown(int key) noexcept
{
    return (::GetAsyncKeyState(key) & 0x8000) != 0;
}

// The same three modifiers the hotkeys use. Read as state, not intercepted: nothing is hooked.
[[nodiscard]] bool AreModifiersHeld() noexcept
{
    return std::ranges::all_of(std::array<int, 3>{ VK_CONTROL, VK_MENU, VK_SHIFT }, IsKeyDown);
}

[[nodiscard]] std::optional<POINT> CursorPosition() noexcept
{
    POINT cursor{};
    if (::GetCursorPos(&cursor) == 0)
        return std::nullopt;
    return cursor;
}

[[nodiscard]] std::optional<interior::Fraction> FractionAcross(const interior::ScreenRect& rect, long x) noexcept
{
    const long width = rect.Right().Get() - rect.Left().Get();
    if (width <= 0)
        return std::nullopt;
    const float across = static_cast<float>(x - rect.Left().Get()) / static_cast<float>(width);
    return interior::FractionTag::Parse(std::clamp(across, 0.0f, 1.0f)).transform([](interior::Fraction f) { return std::optional<interior::Fraction>{ f }; }).value_or(std::nullopt);
}

[[nodiscard]] WindowEvents EventsOfHotkey(WPARAM id) noexcept
{
    return WindowEvents{ id == static_cast<WPARAM>(kHotkeyQuit), id == static_cast<WPARAM>(kHotkeyToggleOriginal), id == static_cast<WPARAM>(kHotkeyToggleSplit) };
}

[[nodiscard]] WindowEvents EventsOf(const MSG& msg) noexcept
{
    if (msg.message == WM_HOTKEY)
        return EventsOfHotkey(msg.wParam);
    return WindowEvents{ msg.message == WM_QUIT, false, false };
}

[[nodiscard]] Pump Dispatched(const Pump& p, const MSG& msg) noexcept
{
    ::TranslateMessage(&msg);
    ::DispatchMessageW(&msg);
    return Pump{ Merged(p.events, EventsOf(msg)), false };
}

[[nodiscard]] Pump Peeked(const Pump& p, const MSG& msg, BOOL received) noexcept
{
    if (received == FALSE)
        return Pump{ p.events, true };
    return Dispatched(p, msg);
}

[[nodiscard]] Pump PumpOne(const Pump& p) noexcept
{
    if (p.drained)
        return p;
    MSG msg{};
    const BOOL received = ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
    return Peeked(p, msg, received);
}

} // namespace

Status<Error> SetDpiAwareness() noexcept
{
    return CheckBool(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), ApiCall::SetProcessDpiAwareness);
}

[[nodiscard]] Result<MonitorList, Error> ListOf(const Collector& collector) noexcept
{
    if (collector.overflow)
        return Fail(Error{ ApiCall::EnumDisplayMonitors, 1 });
    return collector.list;
}

Result<MonitorList, Error> EnumerateMonitors() noexcept
{
    Collector collector{ MonitorList{}, false };
    if (::EnumDisplayMonitors(nullptr, nullptr, &CollectMonitor, reinterpret_cast<LPARAM>(&collector)) == FALSE)
        return Fail(LastError(ApiCall::EnumDisplayMonitors));
    return ListOf(collector);
}

Result<OutputWindow, Error> CreateOutputWindow(const interior::ScreenRect& rect, const WindowSettings& settings) noexcept
{
    return RegisterWindowClass(ClassDescription()).and_then([&] { return CreateHandle(rect, settings); }).and_then([&](UniqueWindow handle) {
        return Configure(handle.get(), rect, settings).transform([&] { return OutputWindow{ std::move(handle), rect }; });
    });
}

std::optional<interior::Fraction> SplitRequest(const OutputWindow& window) noexcept
{
    if (!AreModifiersHeld())
        return std::nullopt;
    return CursorPosition().and_then([&window](POINT cursor) { return FractionAcross(window.rect, cursor.x); });
}

// The three flags that can change while the window is up. The style is rewritten whole and the window
// asked to keep or drop its place above everything; the redirection surface is fixed at creation.
[[nodiscard]] DWORD LiveStyle(const WindowSettings& s) noexcept
{
    return WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | TopmostStyle(s) | ClickThroughStyle(s);
}

Status<Error> ApplyWindowSettings(const OutputWindow& window, const WindowSettings& settings) noexcept
{
    HWND handle = window.handle.get();
    const DWORD kept = static_cast<DWORD>(::GetWindowLongPtrW(handle, GWL_EXSTYLE)) & WS_EX_NOREDIRECTIONBITMAP;
    (void)::SetWindowLongPtrW(handle, GWL_EXSTYLE, static_cast<LONG_PTR>(LiveStyle(settings) | kept));
    return ApplyLayering(handle, settings).and_then([&] { return ApplyAffinity(handle, settings); }).and_then([&] {
        return CheckBool(::SetWindowPos(handle, InsertAfter(settings), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED), ApiCall::CreateWindowExW);
    });
}

// The command line is one buffer the call is allowed to write to, so it is built here and handed over.
[[nodiscard]] std::array<wchar_t, kCommandCapacity> CommandLineFor(std::wstring_view executable, std::wstring_view arguments) noexcept
{
    std::array<wchar_t, kCommandCapacity> line{}; // WAIVER(R2): a local buffer filled once, before use.
    const int written =
        ::_snwprintf_s(line.data(), line.size(), _TRUNCATE, L"\"%.*s\" %.*s", static_cast<int>(executable.size()), executable.data(), static_cast<int>(arguments.size()), arguments.data());
    ENSURE(written > 0);
    return line;
}

void CloseStarted(const PROCESS_INFORMATION& process) noexcept
{
    ENSURE(::CloseHandle(process.hThread) != FALSE);
    ENSURE(::CloseHandle(process.hProcess) != FALSE);
}

[[nodiscard]] STARTUPINFOW StartupRecord() noexcept
{
    STARTUPINFOW startup{}; // WAIVER(R2): a request record filled once, before the call.
    startup.cb = sizeof(STARTUPINFOW);
    return startup;
}

[[nodiscard]] Status<Error> Started(std::array<wchar_t, kCommandCapacity>& line) noexcept
{
    STARTUPINFOW startup = StartupRecord();
    PROCESS_INFORMATION process{}; // WAIVER(R2): an answer record filled once by the call.
    if (::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) == FALSE)
        return Fail(LastError(ApiCall::CreateProcess));
    CloseStarted(process);
    return {};
}

Status<Error> StartProcess(std::wstring_view executable, std::wstring_view arguments) noexcept
{
    std::array<wchar_t, kCommandCapacity> line = CommandLineFor(executable, arguments);
    return Started(line);
}

Status<Error> RegisterWindowClass(const WNDCLASSEXW& description) noexcept
{
    if (IsRegistrationFailure(::RegisterClassExW(&description)))
        return Fail(LastError(ApiCall::RegisterClassExW));
    return {};
}

Status<Error> RegisterHotkeys(const OutputWindow& window) noexcept
{
    return RegisterOne(window.handle.get(), kHotkeyToggleOriginal, 'O').and_then([&] { return RegisterOne(window.handle.get(), kHotkeyToggleSplit, 'C'); }).and_then([&] {
        return RegisterOne(window.handle.get(), kHotkeyQuit, 'Q');
    });
}

void ShowOutputWindow(const OutputWindow& window) noexcept
{
    ::ShowWindow(window.handle.get(), SW_SHOWNOACTIVATE);
}

Result<WindowEvents, Error> PumpEvents(const OutputWindow&) noexcept
{
    const Pump pumped =
        std::ranges::fold_left(std::views::iota(std::uint32_t{ 0 }, kMaxMessagesPerPump), Pump{ WindowEvents{ false, false, false }, false }, [](const Pump& p, std::uint32_t) { return PumpOne(p); });
    return pumped.events;
}

} // namespace real
