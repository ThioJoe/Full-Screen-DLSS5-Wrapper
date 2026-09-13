#include "effects/real/window.h"

#include "res/resource.h"

#include "global_common.h"
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

constexpr wchar_t kClassName[] = DSCREEN_WIDE(DSCREEN_FILE_STEM) L"OutputWindow";
constexpr std::uint32_t kMaxMessagesPerPump = 64;

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

[[nodiscard]] HICON IconAt(int size) noexcept
{
    return static_cast<HICON>(::LoadImageW(::GetModuleHandleW(nullptr), MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON, size, size, LR_DEFAULTCOLOR | LR_SHARED));
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

[[nodiscard]] int Width(const interior::ScreenRect& r) noexcept
{
    return r.Right().Get() - r.Left().Get();
}

[[nodiscard]] int Height(const interior::ScreenRect& r) noexcept
{
    return r.Bottom().Get() - r.Top().Get();
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

[[nodiscard]] BOOL CALLBACK CollectMonitor(HMONITOR handle, HDC, LPRECT, LPARAM param)
{
    static constexpr auto QueryMonitor = [] [[nodiscard]] (HMONITOR handle) noexcept -> std::optional<MonitorInfo> {
        static constexpr auto InfoOf = [] [[nodiscard]] (HMONITOR handle, const MONITORINFOEXW& info) noexcept -> std::optional<MonitorInfo> {
            static constexpr auto IsPrimary = [] [[nodiscard]] (const MONITORINFOEXW& info) noexcept -> bool { return (info.dwFlags & MONITORINFOF_PRIMARY) != 0; };
            return infra::AsOptional(interior::MonitorHandleTag::Parse(reinterpret_cast<std::uintptr_t>(handle))).and_then([&](interior::MonitorHandle h) {
                return infra::AsOptional(RectOf(info.rcMonitor)).and_then([&](const interior::ScreenRect& rect) {
                    return infra::AsOptional(interior::DeviceName::Parse(std::wstring_view(info.szDevice))).transform([&](const interior::DeviceName& name) {
                        return MonitorInfo{ h, rect, IsPrimary(info), name, interior::SourceKind::Monitor };
                    });
                });
            });
        };
        MONITORINFOEXW info{};
        info.cbSize = sizeof(info);
        if (::GetMonitorInfoW(handle, &info) == FALSE)
            return std::nullopt;
        return InfoOf(handle, info);
    };

    static constexpr auto Collected = [] [[nodiscard]] (const Collector& c, const std::optional<MonitorInfo>& monitor) noexcept -> Collector {
        static constexpr auto Pushed = [] [[nodiscard]] (const Collector& c, const MonitorInfo& monitor) noexcept -> Collector {
            const Result<MonitorList, infra::CapacityExceeded> pushed = c.list.Push(monitor);
            if (!pushed.has_value())
                return Collector{ c.list, true };
            return Collector{ *pushed, c.overflow };
        };
        if (!monitor.has_value())
            return c;
        return Pushed(c, *monitor);
    };
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

} // namespace

Status<Error> SetDpiAwareness() noexcept
{
    return CheckBool(::SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2), ApiCall::SetProcessDpiAwareness);
}

Result<MonitorList, Error> EnumerateMonitors() noexcept
{
    static constexpr auto ListOf = [] [[nodiscard]] (const Collector& collector) noexcept -> Result<MonitorList, Error> {
        if (collector.overflow)
            return Fail(Error{ ApiCall::EnumDisplayMonitors, 1 });
        return collector.list;
    };
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

// WAIVER(R17): the enumeration callback is called by the OS, which ignores attributes and discards nothing.
BOOL CALLBACK CollectWindow(HWND window, LPARAM parameter) noexcept
{
    static constexpr auto Matches = [] [[nodiscard]] (HWND window, std::wstring_view wanted) noexcept -> bool {
        static constexpr auto IsTopLevel = [] [[nodiscard]] (HWND window) noexcept -> bool { return ::IsWindowVisible(window) != FALSE && ::GetWindow(window, GW_OWNER) == nullptr; };
        if (!IsTopLevel(window))
            return false;
        return infra::ContainsIgnoringCase(std::wstring_view(TitleOf(window).data()), wanted);
    };
    Search* search = reinterpret_cast<Search*>(parameter); // WAIVER(R2): the OS hands the search back one window at a time.
    if (!Matches(window, search->wanted))
        return TRUE;
    search->found = window;
    return FALSE;
}

// Without a desktop manager there is no extended frame, and the whole window is the best answer there is.
[[nodiscard]] std::optional<RECT> VisibleBoundsOf(HWND window) noexcept
{
    // What the window looks like on screen. GetWindowRect includes the invisible resize border the desktop
    // manager keeps around a window, which the capture leaves out.
    static constexpr auto ExtendedFrameOf = [] [[nodiscard]] (HWND window) noexcept -> std::optional<RECT> {
        RECT frame{}; // WAIVER(R2): the answer of one query, read once after it.
        if (IsFailure(::DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &frame, sizeof(frame))))
            return std::nullopt;
        return frame;
    };

    static constexpr auto WholeWindowOf = [] [[nodiscard]] (HWND window) noexcept -> std::optional<RECT> {
        RECT whole{}; // WAIVER(R2): the answer of one query, read once after it.
        if (::GetWindowRect(window, &whole) == FALSE)
            return std::nullopt;
        return whole;
    };
    return ExtendedFrameOf(window).or_else([window] { return WholeWindowOf(window); });
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

Result<MonitorInfo, Error> FindWindowNamed(const interior::WindowTitle& asked) noexcept
{
    static constexpr auto SourceOf = [] [[nodiscard]] (HWND window) noexcept -> std::optional<MonitorInfo> {
        const std::optional<RECT> bounds = VisibleBoundsOf(window);
        if (!bounds.has_value())
            return std::nullopt;
        return infra::AsOptional(interior::MonitorHandleTag::Parse(reinterpret_cast<std::uintptr_t>(window))).and_then([&](interior::MonitorHandle h) {
            return infra::AsOptional(RectOf(*bounds)).transform([&](const interior::ScreenRect& rect) {
                return MonitorInfo{ h, rect, false, interior::DeviceName::Parse(std::wstring_view(TitleOf(window).data()).substr(0, interior::DeviceName::Capacity)).value_or(interior::DeviceName{}),
                                    interior::SourceKind::Window };
            });
        });
    };

    static constexpr auto FoundByTitle = [] [[nodiscard]] (const interior::WindowTitle& title) noexcept -> Result<MonitorInfo, Error> {
        Search search{ title.Get(), nullptr }; // WAIVER(R2): filled by the enumeration, then read once.
        (void)::EnumWindows(&CollectWindow, reinterpret_cast<LPARAM>(&search));
        if (search.found == nullptr)
            return Fail(Error{ ApiCall::WindowNotFound, 0 });
        return infra::AsResult(SourceOf(search.found), Error{ ApiCall::WindowNotFound, 1 });
    };

    static constexpr auto IsHandleText = [] [[nodiscard]] (std::wstring_view asked) noexcept -> bool { return asked.size() > 2 && StartsWithHex(asked); };

    static constexpr auto FoundByHandle = [] [[nodiscard]] (std::wstring_view asked) noexcept -> Result<MonitorInfo, Error> {
        static constexpr auto HandleFrom = [] [[nodiscard]] (std::wstring_view asked) noexcept -> std::optional<HWND> {
            std::uintptr_t value = 0; // WAIVER(R2): the answer of one parse, read once after it.
            const std::array<char, interior::WindowTitle::Capacity + 1> digits = infra::NarrowedChars<interior::WindowTitle::Capacity + 1>(asked.substr(2));
            const std::from_chars_result parsed = std::from_chars(digits.data(), digits.data() + asked.size() - 2, value, 16);
            if (parsed.ec != std::errc{})
                return std::nullopt;
            return reinterpret_cast<HWND>(value);
        };

        static constexpr auto NamesALiveWindow = [] [[nodiscard]] (const std::optional<HWND>& handle) noexcept -> bool { return handle.has_value() && ::IsWindow(*handle) != FALSE; };
        const std::optional<HWND> handle = HandleFrom(asked);
        if (!NamesALiveWindow(handle))
            return Fail(Error{ ApiCall::WindowNotFound, 3 });
        return infra::AsResult(SourceOf(*handle), Error{ ApiCall::WindowNotFound, 4 });
    };
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

[[nodiscard]] bool IsSomeoneElses(HWND window) noexcept
{
    static constexpr auto IsOurs = [] [[nodiscard]] (HWND window) noexcept -> bool {
        DWORD owner = 0; // WAIVER(R2): the answer of one query, read once after it.
        (void)::GetWindowThreadProcessId(window, &owner);
        return owner == ::GetCurrentProcessId();
    };
    return window != nullptr && !IsOurs(window);
}

std::optional<interior::MonitorHandle> WindowUnder(long x, long y) noexcept
{
    static constexpr auto IsPickable = [] [[nodiscard]] (HWND window, HWND desktop) noexcept -> bool {
        static constexpr auto IsTheDesktop = [] [[nodiscard]] (HWND window, HWND desktop) noexcept -> bool {
            // Asking what is under a point on the empty desktop answers with the shell's own window, not with the
            // desktop window, so the wallpaper would be a window to work on like any other unless it is named here.
            static constexpr auto IsDesktopBackground = [] [[nodiscard]] (HWND window) noexcept -> bool {
                static constexpr auto ClassOf = [] [[nodiscard]] (HWND window) noexcept -> std::array<wchar_t, 16> {
                    std::array<wchar_t, 16> name{}; // WAIVER(R2): a local buffer filled once, before use.
                    (void)::GetClassNameW(window, name.data(), static_cast<int>(name.size()));
                    return name;
                };
                const std::array<wchar_t, 16> name = ClassOf(window);
                return std::wstring_view(name.data()) == L"Progman" || std::wstring_view(name.data()) == L"WorkerW";
            };
            return window == desktop || IsDesktopBackground(window);
        };
        return !IsTheDesktop(window, desktop) && IsSomeoneElses(window);
    };
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

bool IsWindowThere(interior::MonitorHandle window) noexcept
{
    return ::IsWindow(reinterpret_cast<HWND>(window.Get())) != FALSE;
}

void BringWindowBack(interior::MonitorHandle window) noexcept
{
    HWND handle = reinterpret_cast<HWND>(window.Get());
    (void)::ShowWindowAsync(handle, SW_SHOWNOACTIVATE);
    (void)::SetWindowPos(handle, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_ASYNCWINDOWPOS);
}

Result<OutputWindow, Error> CreateOutputWindow(const interior::ScreenRect& rect, const WindowSettings& settings) noexcept
{
    // The window paints every pixel it owns from the swap chain, so it asks for no background brush.
    static constexpr auto ClassDescription = [] [[nodiscard]] () noexcept -> WNDCLASSEXW {
        return WNDCLASSEXW{ .cbSize = sizeof(WNDCLASSEXW),
                            .style = 0,
                            .lpfnWndProc = &WindowProc,
                            .cbClsExtra = 0,
                            .cbWndExtra = 0,
                            .hInstance = ::GetModuleHandleW(nullptr),
                            .hIcon = LargeAppIcon(),
                            .hCursor = ::LoadCursorW(nullptr, IDC_ARROW),
                            .hbrBackground = nullptr,
                            .lpszMenuName = nullptr,
                            .lpszClassName = kClassName,
                            .hIconSm = SmallAppIcon() };
    };

    static constexpr auto CreateHandle = [] [[nodiscard]] (const interior::ScreenRect& rect, const WindowSettings& s) noexcept -> Result<UniqueWindow, Error> {
        static constexpr auto ExtendedStyle = [] [[nodiscard]] (const WindowSettings& s) noexcept -> DWORD {
            static constexpr auto RedirectionStyle = [] [[nodiscard]] (const WindowSettings& s) noexcept -> DWORD { return s.redirectionBitmap ? 0u : WS_EX_NOREDIRECTIONBITMAP; };
            return WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | TopmostStyle(s) | ClickThroughStyle(s) | RedirectionStyle(s);
        };
        HWND handle = ::CreateWindowExW(ExtendedStyle(s), kClassName, DSCREEN_WIDE(DSCREEN_PRODUCT_NAME), WS_POPUP, rect.Left().Get(), rect.Top().Get(), Width(rect), Height(rect), nullptr, nullptr,
                                        ::GetModuleHandleW(nullptr), nullptr);
        if (handle == nullptr)
            return Fail(LastError(ApiCall::CreateWindowExW));
        return UniqueWindow(handle);
    };

    static constexpr auto Configure = [] [[nodiscard]] (HWND handle, const interior::ScreenRect& rect, const WindowSettings& s) noexcept -> Status<Error> {
        static constexpr auto Position = [] [[nodiscard]] (HWND handle, const interior::ScreenRect& rect, const WindowSettings& s) noexcept -> Status<Error> {
            return CheckBool(::SetWindowPos(handle, InsertAfter(s), rect.Left().Get(), rect.Top().Get(), Width(rect), Height(rect), SWP_NOACTIVATE | SWP_FRAMECHANGED), ApiCall::CreateWindowExW);
        };
        return ApplyLayering(handle, s).and_then([&] { return ApplyAffinity(handle, s); }).and_then([&] { return Position(handle, rect, s); });
    };
    return RegisterWindowClass(ClassDescription()).and_then([&] { return CreateHandle(rect, settings); }).and_then([&](UniqueWindow handle) {
        return Configure(handle.get(), rect, settings).transform([&] { return OutputWindow{ std::move(handle), rect }; });
    });
}

std::optional<interior::Fraction> SplitRequest(const OutputWindow& window) noexcept
{
    // The same three modifiers the hotkeys use. Read as state, not intercepted: nothing is hooked.
    static constexpr auto AreModifiersHeld = [] [[nodiscard]] () noexcept -> bool {
        static constexpr auto IsKeyDown = [] [[nodiscard]] (int key) noexcept -> bool { return (::GetAsyncKeyState(key) & 0x8000) != 0; };
        return std::ranges::all_of(std::array<int, 3>{ VK_CONTROL, VK_MENU, VK_SHIFT }, IsKeyDown);
    };

    static constexpr auto CursorPosition = [] [[nodiscard]] () noexcept -> std::optional<POINT> {
        POINT cursor{};
        if (::GetCursorPos(&cursor) == 0)
            return std::nullopt;
        return cursor;
    };

    static constexpr auto FractionAcross = [] [[nodiscard]] (const interior::ScreenRect& rect, long x) noexcept -> std::optional<interior::Fraction> {
        const long width = rect.Right().Get() - rect.Left().Get();
        if (width <= 0)
            return std::nullopt;
        const float across = static_cast<float>(x - rect.Left().Get()) / static_cast<float>(width);
        return interior::FractionTag::Parse(std::clamp(across, 0.0f, 1.0f)).transform([](interior::Fraction f) { return std::optional<interior::Fraction>{ f }; }).value_or(std::nullopt);
    };
    if (!AreModifiersHeld())
        return std::nullopt;
    return CursorPosition().and_then([&window](POINT cursor) { return FractionAcross(window.rect, cursor.x); });
}

Status<Error> ApplyWindowSettings(const OutputWindow& window, const WindowSettings& settings) noexcept
{
    // The three flags that can change while the window is up. The style is rewritten whole and the window
    // asked to keep or drop its place above everything; the redirection surface is fixed at creation.
    static constexpr auto LiveStyle = [] [[nodiscard]] (const WindowSettings& s) noexcept -> DWORD { return WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | TopmostStyle(s) | ClickThroughStyle(s); };
    HWND handle = window.handle.get();
    const DWORD kept = static_cast<DWORD>(::GetWindowLongPtrW(handle, GWL_EXSTYLE)) & WS_EX_NOREDIRECTIONBITMAP;
    (void)::SetWindowLongPtrW(handle, GWL_EXSTYLE, static_cast<LONG_PTR>(LiveStyle(settings) | kept));
    return ApplyLayering(handle, settings).and_then([&] { return ApplyAffinity(handle, settings); }).and_then([&] {
        return CheckBool(::SetWindowPos(handle, InsertAfter(settings), 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED), ApiCall::CreateWindowExW);
    });
}

[[nodiscard]] Status<Error> Started(std::array<wchar_t, kCommandCapacity>& line) noexcept
{
    static constexpr auto CloseStarted = [](const PROCESS_INFORMATION& process) noexcept -> void {
        ENSURE(::CloseHandle(process.hThread) != FALSE);
        ENSURE(::CloseHandle(process.hProcess) != FALSE);
    };

    static constexpr auto StartupRecord = [] [[nodiscard]] () noexcept -> STARTUPINFOW {
        STARTUPINFOW startup{}; // WAIVER(R2): a request record filled once, before the call.
        startup.cb = sizeof(STARTUPINFOW);
        return startup;
    };
    STARTUPINFOW startup = StartupRecord();
    PROCESS_INFORMATION process{}; // WAIVER(R2): an answer record filled once by the call.
    if (::CreateProcessW(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process) == FALSE)
        return Fail(LastError(ApiCall::CreateProcess));
    CloseStarted(process);
    return {};
}

Status<Error> StartProcess(std::wstring_view executable, std::wstring_view arguments) noexcept
{
    // The command line is one buffer the call is allowed to write to, so it is built here and handed over.
    static constexpr auto CommandLineFor = [] [[nodiscard]] (std::wstring_view executable, std::wstring_view arguments) noexcept -> std::array<wchar_t, kCommandCapacity> {
        std::array<wchar_t, kCommandCapacity> line{}; // WAIVER(R2): a local buffer filled once, before use.
        const int written =
            ::_snwprintf_s(line.data(), line.size(), _TRUNCATE, L"\"%.*s\" %.*s", static_cast<int>(executable.size()), executable.data(), static_cast<int>(arguments.size()), arguments.data());
        ENSURE(written > 0);
        return line;
    };
    std::array<wchar_t, kCommandCapacity> line = CommandLineFor(executable, arguments);
    return Started(line);
}

HICON LargeAppIcon() noexcept
{
    return IconAt(::GetSystemMetrics(SM_CXICON));
}

HICON SmallAppIcon() noexcept
{
    return IconAt(::GetSystemMetrics(SM_CXSMICON));
}

Status<Error> RegisterWindowClass(const WNDCLASSEXW& description) noexcept
{
    static constexpr auto IsRegistrationFailure = [] [[nodiscard]] (ATOM atom) noexcept -> bool {
        static constexpr auto IsAlreadyRegistered = [] [[nodiscard]] () noexcept -> bool { return ::GetLastError() == ERROR_CLASS_ALREADY_EXISTS; };
        return atom == 0 && !IsAlreadyRegistered();
    };
    if (IsRegistrationFailure(::RegisterClassExW(&description)))
        return Fail(LastError(ApiCall::RegisterClassExW));
    return {};
}

Status<Error> RegisterHotkeys(const OutputWindow& window) noexcept
{
    static constexpr auto RegisterOne = [] [[nodiscard]] (HWND handle, int id, UINT key) noexcept -> Status<Error> {
        return CheckBool(::RegisterHotKey(handle, id, MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_NOREPEAT, key), ApiCall::RegisterHotKey);
    };
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

[[nodiscard]] bool IsAtPoint(const RECT& where, const interior::ScreenRect& rect) noexcept
{
    return where.left == rect.Left().Get() && where.top == rect.Top().Get();
}

[[nodiscard]] bool IsStackedOn(HWND handle, HWND above) noexcept
{
    if (IsTopmostWindow(above))
        return IsTopmostWindow(handle);
    return ::GetWindow(above, GW_HWNDPREV) == handle;
}

// Once in place the overlay is itself the window preceding the one being followed, and no window can be
// inserted after itself: such a call fails whole, move and all. A move alone asks for no restacking.
void MoveOutputWindowAbove(const OutputWindow& window, const interior::ScreenRect& rect, HWND above) noexcept
{
    static constexpr auto MoveFlags = [] [[nodiscard]] (bool stacked) noexcept -> UINT { return stacked ? (SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOZORDER) : (SWP_NOSIZE | SWP_NOACTIVATE); };

    static constexpr auto InsertFor = [] [[nodiscard]] (bool stacked, HWND above) noexcept -> HWND {
        // Nothing can be put directly above a window that is always on top, so the overlay joins that band and
        // the two rise together.
        static constexpr auto JustAbove = [] [[nodiscard]] (HWND above) noexcept -> HWND {
            // SetWindowPos names the window that is to precede the one being positioned, so naming the window being
            // followed puts the overlay behind it. The place above it is asked for by naming whatever is there now.
            static constexpr auto Preceding = [] [[nodiscard]] (HWND window) noexcept -> HWND {
                HWND before = ::GetWindow(window, GW_HWNDPREV);
                return before == nullptr ? HWND_TOP : before;
            };
            return IsTopmostWindow(above) ? HWND_TOPMOST : Preceding(above);
        };
        return stacked ? nullptr : JustAbove(above);
    };
    const bool stacked = IsStackedOn(window.handle.get(), above);
    (void)::SetWindowPos(window.handle.get(), InsertFor(stacked, above), rect.Left().Get(), rect.Top().Get(), 0, 0, MoveFlags(stacked));
}

// Whichever program owns the window being followed can raise it, and raising it puts it over the overlay.
// Windows can also refuse a placement outright. Both are read back from the stack rather than remembered.
bool IsOutputWindowPlaced(const OutputWindow& window, const interior::ScreenRect& rect, HWND above) noexcept
{
    static constexpr auto IsAtTopLeft = [] [[nodiscard]] (HWND handle, const interior::ScreenRect& rect) noexcept -> bool {
        RECT where{}; // WAIVER(R2): the answer of one query, read once after it.
        return ::GetWindowRect(handle, &where) != FALSE && IsAtPoint(where, rect);
    };
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

void RaiseOutputWindow(const OutputWindow& window) noexcept
{
    if (IsTopmostWindow(window.handle.get()))
        return;
    (void)::SetWindowPos(window.handle.get(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void ShowOutputWindow(const OutputWindow& window) noexcept
{
    ::ShowWindow(window.handle.get(), SW_SHOWNOACTIVATE);
}

void HideOutputWindow(const OutputWindow& window) noexcept
{
    ::ShowWindow(window.handle.get(), SW_HIDE);
}

Result<WindowEvents, Error> PumpEvents(const OutputWindow&) noexcept
{
    static constexpr auto PumpOne = [] [[nodiscard]] (const Pump& p) noexcept -> Pump {
        static constexpr auto Peeked = [] [[nodiscard]] (const Pump& p, const MSG& msg, BOOL received) noexcept -> Pump {
            static constexpr auto Dispatched = [] [[nodiscard]] (const Pump& p, const MSG& msg) noexcept -> Pump {
                static constexpr auto Merged = [] [[nodiscard]] (const WindowEvents& a, const WindowEvents& b) noexcept -> WindowEvents {
                    static constexpr auto Either = [] [[nodiscard]] (bool a, bool b) noexcept -> bool { return a || b; };
                    return WindowEvents{ Either(a.quit, b.quit), Either(a.toggleOriginal, b.toggleOriginal), Either(a.toggleSplit, b.toggleSplit) };
                };

                static constexpr auto EventsOf = [] [[nodiscard]] (const MSG& msg) noexcept -> WindowEvents {
                    static constexpr auto EventsOfHotkey = [] [[nodiscard]] (WPARAM id) noexcept -> WindowEvents {
                        return WindowEvents{ id == static_cast<WPARAM>(kHotkeyQuit), id == static_cast<WPARAM>(kHotkeyToggleOriginal), id == static_cast<WPARAM>(kHotkeyToggleSplit) };
                    };
                    if (msg.message == WM_HOTKEY)
                        return EventsOfHotkey(msg.wParam);
                    return WindowEvents{ msg.message == WM_QUIT, false, false };
                };
                ::TranslateMessage(&msg);
                ::DispatchMessageW(&msg);
                return Pump{ Merged(p.events, EventsOf(msg)), false };
            };
            if (received == FALSE)
                return Pump{ p.events, true };
            return Dispatched(p, msg);
        };
        if (p.drained)
            return p;
        MSG msg{};
        const BOOL received = ::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE);
        return Peeked(p, msg, received);
    };
    const Pump pumped =
        std::ranges::fold_left(std::views::iota(std::uint32_t{ 0 }, kMaxMessagesPerPump), Pump{ WindowEvents{ false, false, false }, false }, [](const Pump& p, std::uint32_t) { return PumpOne(p); });
    return pumped.events;
}

} // namespace real
