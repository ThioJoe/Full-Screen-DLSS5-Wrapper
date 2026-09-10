#include "effects/real/window.h"

#include "infrastructure/fold.h"
#include "infrastructure/text.h"

#include <dwmapi.h>

#include <algorithm>
#include <array>
#include <charconv>
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

// Returning HTTRANSPARENT from the window procedure only passes a click to a window on the same thread,
// so reaching another program's window underneath needs WS_EX_LAYERED as well.
[[nodiscard]] DWORD ClickThroughStyle(const WindowSettings& s) noexcept
{
    return s.clickThrough ? (WS_EX_TRANSPARENT | WS_EX_LAYERED) : 0u;
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
                return MonitorInfo{ h, rect, IsPrimary(info), name, interior::SourceKind::Monitor };
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

// --- finding one window to work on ---------------------------------------------------------------------

// What the operator typed has to appear in the title, ignoring case. Only a visible top-level window with
// a title of its own can be picked, which leaves out the invisible message windows every process has.
struct Search
{
    std::wstring_view wanted;
    HWND found;
};

[[nodiscard]] std::array<wchar_t, interior::WindowTitle::Capacity + 1> TitleOf(HWND window) noexcept
{
    std::array<wchar_t, interior::WindowTitle::Capacity + 1> title{}; // WAIVER(R2): a local buffer filled once, before use.
    (void)::GetWindowTextW(window, title.data(), static_cast<int>(title.size()));
    return title;
}

[[nodiscard]] bool IsTopLevel(HWND window) noexcept
{
    return ::IsWindowVisible(window) != FALSE && ::GetWindow(window, GW_OWNER) == nullptr;
}

[[nodiscard]] bool Matches(HWND window, std::wstring_view wanted) noexcept
{
    if (!IsTopLevel(window))
        return false;
    return infra::ContainsIgnoringCase(std::wstring_view(TitleOf(window).data()), wanted);
}

// WAIVER(R17): the enumeration callback is called by the OS, which ignores attributes and discards nothing.
BOOL CALLBACK CollectWindow(HWND window, LPARAM parameter) noexcept
{
    Search* search = reinterpret_cast<Search*>(parameter); // WAIVER(R2): the OS hands the search back one window at a time.
    if (!Matches(window, search->wanted))
        return TRUE;
    search->found = window;
    return FALSE;
}

// What the window looks like on screen. GetWindowRect includes the invisible resize border the desktop
// manager keeps around a window, which the capture leaves out.
[[nodiscard]] std::optional<RECT> ExtendedFrameOf(HWND window) noexcept
{
    RECT frame{}; // WAIVER(R2): the answer of one query, read once after it.
    if (IsFailure(::DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
        return std::nullopt;
    return frame;
}

[[nodiscard]] std::optional<RECT> WholeWindowOf(HWND window) noexcept
{
    RECT whole{}; // WAIVER(R2): the answer of one query, read once after it.
    if (::GetWindowRect(window, &whole) == FALSE)
        return std::nullopt;
    return whole;
}

// Without a desktop manager there is no extended frame, and the whole window is the best answer there is.
[[nodiscard]] std::optional<RECT> VisibleBoundsOf(HWND window) noexcept
{
    return ExtendedFrameOf(window).or_else([window] { return WholeWindowOf(window); });
}

[[nodiscard]] std::optional<MonitorInfo> SourceOf(HWND window) noexcept
{
    const std::optional<RECT> bounds = VisibleBoundsOf(window);
    if (!bounds.has_value())
        return std::nullopt;
    return infra::AsOptional(interior::MonitorHandleTag::Parse(reinterpret_cast<std::uintptr_t>(window))).and_then([&](interior::MonitorHandle h) {
        return infra::AsOptional(RectOf(*bounds)).transform([&](const interior::ScreenRect& rect) {
            return MonitorInfo{ h, rect, false, interior::DeviceName::Parse(std::wstring_view(TitleOf(window).data()).substr(0, interior::DeviceName::Capacity)).value_or(interior::DeviceName{}),
                                interior::SourceKind::Window };
        });
    });
}

[[nodiscard]] Result<MonitorInfo, Error> FoundByTitle(const interior::WindowTitle& title) noexcept
{
    Search search{ title.Get(), nullptr }; // WAIVER(R2): filled by the enumeration, then read once.
    (void)::EnumWindows(&CollectWindow, reinterpret_cast<LPARAM>(&search));
    if (search.found == nullptr)
        return Fail(Error{ ApiCall::WindowNotFound, 0 });
    return infra::AsResult(SourceOf(search.found), Error{ ApiCall::WindowNotFound, 1 });
}

// The panel picks a window with the mouse and has its handle, so that is what it writes; a person typing a
// command line has a title and not a handle, so both are accepted and the leading 0x tells them apart.
[[nodiscard]] bool IsHexMarker(wchar_t c) noexcept
{
    return c == L'x' || c == L'X';
}

[[nodiscard]] bool StartsWithHex(std::wstring_view asked) noexcept
{
    return asked.starts_with(L'0') && IsHexMarker(asked[1]);
}

[[nodiscard]] bool IsHandleText(std::wstring_view asked) noexcept
{
    return asked.size() > 2 && StartsWithHex(asked);
}

[[nodiscard]] std::optional<HWND> HandleFrom(std::wstring_view asked) noexcept
{
    std::uintptr_t value = 0; // WAIVER(R2): the answer of one parse, read once after it.
    const std::array<char, interior::WindowTitle::Capacity + 1> digits = infra::NarrowedChars<interior::WindowTitle::Capacity + 1>(asked.substr(2));
    const std::from_chars_result parsed = std::from_chars(digits.data(), digits.data() + asked.size() - 2, value, 16);
    if (parsed.ec != std::errc{})
        return std::nullopt;
    return reinterpret_cast<HWND>(value);
}

[[nodiscard]] bool NamesALiveWindow(const std::optional<HWND>& handle) noexcept
{
    return handle.has_value() && ::IsWindow(*handle) != FALSE;
}

[[nodiscard]] Result<MonitorInfo, Error> FoundByHandle(std::wstring_view asked) noexcept
{
    const std::optional<HWND> handle = HandleFrom(asked);
    if (!NamesALiveWindow(handle))
        return Fail(Error{ ApiCall::WindowNotFound, 3 });
    return infra::AsResult(SourceOf(*handle), Error{ ApiCall::WindowNotFound, 4 });
}

Result<MonitorInfo, Error> FindWindowNamed(const interior::WindowTitle& asked) noexcept
{
    if (IsHandleText(asked.Get()))
        return FoundByHandle(asked.Get());
    return FoundByTitle(asked);
}

interior::WindowTitle TitleOfWindow(interior::MonitorHandle window) noexcept
{
    HWND handle = reinterpret_cast<HWND>(window.Get());
    if (::IsWindow(handle) == FALSE)
        return interior::WindowTitle{};
    return interior::WindowTitle::Parse(std::wstring_view(TitleOf(handle).data()).substr(0, interior::WindowTitle::Capacity)).value_or(interior::WindowTitle{});
}

[[nodiscard]] bool IsOurs(HWND window) noexcept
{
    DWORD owner = 0; // WAIVER(R2): the answer of one query, read once after it.
    (void)::GetWindowThreadProcessId(window, &owner);
    return owner == ::GetCurrentProcessId();
}

[[nodiscard]] bool IsSomeoneElses(HWND window) noexcept
{
    return window != nullptr && !IsOurs(window);
}

[[nodiscard]] std::array<wchar_t, 16> ClassOf(HWND window) noexcept
{
    std::array<wchar_t, 16> name{}; // WAIVER(R2): a local buffer filled once, before use.
    (void)::GetClassNameW(window, name.data(), static_cast<int>(name.size()));
    return name;
}

// Asking what is under a point on the empty desktop answers with the shell's own window, not with the
// desktop window, so the wallpaper would be a window to work on like any other unless it is named here.
[[nodiscard]] bool IsDesktopBackground(HWND window) noexcept
{
    const std::array<wchar_t, 16> name = ClassOf(window);
    return std::wstring_view(name.data()) == L"Progman" || std::wstring_view(name.data()) == L"WorkerW";
}

[[nodiscard]] bool IsTheDesktop(HWND window, HWND desktop) noexcept
{
    return window == desktop || IsDesktopBackground(window);
}

[[nodiscard]] bool IsPickable(HWND window, HWND desktop) noexcept
{
    return !IsTheDesktop(window, desktop) && IsSomeoneElses(window);
}

std::optional<interior::MonitorHandle> WindowUnder(long x, long y) noexcept
{
    const POINT point{ x, y };
    HWND under = ::GetAncestor(::WindowFromPoint(point), GA_ROOT);
    if (!IsPickable(under, ::GetDesktopWindow()))
        return std::nullopt;
    return infra::AsOptional(interior::MonitorHandleTag::Parse(reinterpret_cast<std::uintptr_t>(under)));
}

std::optional<interior::ScreenRect> BoundsOfWindow(interior::MonitorHandle window) noexcept
{
    return VisibleBoundsOf(reinterpret_cast<HWND>(window.Get())).and_then([](const RECT& bounds) { return infra::AsOptional(RectOf(bounds)); });
}

// A minimised window keeps its handle and its style but has nothing on screen to capture, and a hidden one
// is the same; both answer no here, as a window that has been closed does.
[[nodiscard]] bool IsOnScreen(HWND window) noexcept
{
    return ::IsWindowVisible(window) != FALSE && ::IsIconic(window) == FALSE;
}

bool IsWindowShowing(interior::MonitorHandle window) noexcept
{
    HWND handle = reinterpret_cast<HWND>(window.Get());
    return ::IsWindow(handle) != FALSE && IsOnScreen(handle);
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

// Puts a window back into every capture on the machine. Only for when our own capture has been told to
// leave it out by name, which is the whole point: seen by recorders, unseen by the model.
Status<Error> UncoverWindow(HWND window) noexcept
{
    return CheckBool(::SetWindowDisplayAffinity(window, WDA_NONE), ApiCall::SetWindowDisplayAffinity);
}

void MoveOutputWindow(const OutputWindow& window, const interior::ScreenRect& rect) noexcept
{
    (void)::SetWindowPos(window.handle.get(), nullptr, rect.Left().Get(), rect.Top().Get(), 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
}

[[nodiscard]] bool IsTopmostWindow(HWND window) noexcept
{
    return (static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE)) & WS_EX_TOPMOST) != 0;
}

// SetWindowPos names the window that is to precede the one being positioned, so naming the window being
// followed puts the overlay behind it. The place above it is asked for by naming whatever is there now.
[[nodiscard]] HWND Preceding(HWND window) noexcept
{
    HWND before = ::GetWindow(window, GW_HWNDPREV);
    return before == nullptr ? HWND_TOP : before;
}

// Nothing can be put directly above a window that is always on top, so the overlay joins that band and
// the two rise together.
[[nodiscard]] HWND JustAbove(HWND above) noexcept
{
    return IsTopmostWindow(above) ? HWND_TOPMOST : Preceding(above);
}

void MoveOutputWindowAbove(const OutputWindow& window, const interior::ScreenRect& rect, HWND above) noexcept
{
    (void)::SetWindowPos(window.handle.get(), JustAbove(above), rect.Left().Get(), rect.Top().Get(), 0, 0, SWP_NOSIZE | SWP_NOACTIVATE);
}

[[nodiscard]] bool IsAtPoint(const RECT& where, const interior::ScreenRect& rect) noexcept
{
    return where.left == rect.Left().Get() && where.top == rect.Top().Get();
}

[[nodiscard]] bool IsAtTopLeft(HWND handle, const interior::ScreenRect& rect) noexcept
{
    RECT where{}; // WAIVER(R2): the answer of one query, read once after it.
    return ::GetWindowRect(handle, &where) != FALSE && IsAtPoint(where, rect);
}

[[nodiscard]] bool IsStackedOn(HWND handle, HWND above) noexcept
{
    if (IsTopmostWindow(above))
        return IsTopmostWindow(handle);
    return ::GetWindow(above, GW_HWNDPREV) == handle;
}

// Whichever program owns the window being followed can raise it, and raising it puts it over the overlay.
// Windows can also refuse a placement outright. Both are read back from the stack rather than remembered.
bool IsOutputWindowPlaced(const OutputWindow& window, const interior::ScreenRect& rect, HWND above) noexcept
{
    return IsAtTopLeft(window.handle.get(), rect) && IsStackedOn(window.handle.get(), above);
}

bool IsOutputWindowBehind(const OutputWindow& window, HWND front) noexcept
{
    return ::GetWindow(front, GW_HWNDNEXT) == window.handle.get();
}

// The overlay is moved rather than the panel raised, so nothing the operator is holding on to shifts.
void KeepOutputWindowBehind(const OutputWindow& window, HWND front) noexcept
{
    (void)::SetWindowPos(window.handle.get(), front, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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
