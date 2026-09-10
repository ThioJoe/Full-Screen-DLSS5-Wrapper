#include "effects/real/exclusion.h"

#include "infrastructure/text.h"

#include <windows.ui.h>

#include <algorithm>
#include <array>
#include <optional>
#include <ranges>
#include <string_view>

namespace real {
namespace {

using ABI::Windows::Graphics::Capture::IGraphicsCaptureSession;
using ABI::Windows::UI::WindowId;

// Every step is written to a file of its own: it fails in a way that reads as success.
constexpr wchar_t kNotesFile[] = L"dlssscreen-exclusion.log";

[[nodiscard]] HANDLE Notes() noexcept
{
    // WAIVER(R11): one file for the program, opened once and left open until it ends.
    static const HANDLE file = ::CreateFileW(kNotesFile, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    return file;
}

void Note(std::string_view line) noexcept
{
    if (Notes() == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0; // WAIVER(R2): what the write reports, which nothing reads.
    (void)::WriteFile(Notes(), line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    (void)::WriteFile(Notes(), "\r\n", 2, &written, nullptr);
}

void NoteOne(const char* what, unsigned long long value) noexcept
{
    Note(infra::Formatted<192>("{} {:#x}", what, value).Get());
}

void NoteTwo(const char* what, unsigned long long first, unsigned long long second) noexcept
{
    Note(infra::Formatted<192>("{} {:#x} {:#x}", what, first, second).Get());
}

// IDisplayGraphicsCaptureSession as Windows metadata declares it. No Windows SDK this builds against
// projects the type, so its identity and the order of its two methods are written out here.
constexpr GUID kDisplaySessionIid{ 0xBB91F61B, 0x218A, 0x587D, { 0x85, 0x80, 0x27, 0x01, 0xA7, 0x4C, 0x05, 0x25 } };

// The identity of a parameterised interface is a name-based UUID over its signature, so these two are not
// in any header: they were computed from the signatures Windows would use and checked against known ones.
constexpr GUID kIterableIid{ 0x745698BF, 0x22AD, 0x5C0D, { 0xB0, 0xE0, 0x07, 0xD3, 0x5A, 0x1C, 0x97, 0x19 } };
constexpr GUID kIteratorIid{ 0xBA0A30A1, 0xC082, 0x5671, { 0xAC, 0x07, 0x7A, 0xAA, 0x4F, 0x26, 0x96, 0x70 } };

// The reader is past its end. Written out because the headers the compile check uses lack the name.
constexpr HRESULT kOutOfBounds = static_cast<HRESULT>(0x8000000BL);

constexpr std::size_t kMaxExcluded = 4;
using WindowIds = std::array<WindowId, kMaxExcluded>;

struct IWindowIdIterator : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE get_Current(WindowId* value) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean* value) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE MoveNext(boolean* value) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32 capacity, WindowId* items, UINT32* taken) noexcept = 0;
};

struct IWindowIdIterable : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE First(IWindowIdIterator** first) noexcept = 0;
};

struct IWindowIdVectorView : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE GetAt(UINT32 index, WindowId* item) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE get_Size(UINT32* size) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE IndexOf(WindowId item, UINT32* index, boolean* found) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetMany(UINT32 start, UINT32 capacity, WindowId* items, UINT32* taken) noexcept = 0;
};

struct IDisplaySession : IInspectable
{
    virtual HRESULT STDMETHODCALLTYPE SetWindowExclusionList(IWindowIdIterable* windows, UINT64* iteration) noexcept = 0;
    virtual HRESULT STDMETHODCALLTYPE GetWindowExclusionList(IWindowIdVectorView** windows) noexcept = 0;
};

// An interface we did not think of is the likeliest way a computed identity is wrong, and Windows asking
// for one it cannot get is invisible from the outside, so every refusal says what was asked for.
[[nodiscard]] HRESULT NotedRefusal(REFIID asked) noexcept
{
    NoteTwo("refused an interface", asked.Data1, asked.Data2);
    return E_NOINTERFACE;
}

[[nodiscard]] bool IsOwnOrUnknown(REFIID asked, const GUID& own) noexcept
{
    return ::IsEqualGUID(asked, own) || ::IsEqualGUID(asked, IID_IUnknown);
}

[[nodiscard]] bool Knows(REFIID asked, const GUID& own) noexcept
{
    return IsOwnOrUnknown(asked, own) || ::IsEqualGUID(asked, IID_IInspectable);
}

[[nodiscard]] HRESULT Answer(REFIID asked, const GUID& own, IUnknown* self, void** out) noexcept
{
    if (!Knows(asked, own))
        return NotedRefusal(asked);
    *out = self;
    return S_OK;
}

[[nodiscard]] HRESULT NoIids(ULONG* count, IID** iids) noexcept
{
    *count = 0;
    *iids = nullptr;
    return S_OK;
}

[[nodiscard]] HRESULT BaseTrust(TrustLevel* level) noexcept
{
    *level = ::TrustLevel::BaseTrust;
    return S_OK;
}

class Iterator final : public IWindowIdIterator
{
public:
    void Reset(const WindowIds& ids, std::size_t count) noexcept;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID asked, void** out) noexcept override { return Answer(asked, kIteratorIid, this, out); }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 2; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) noexcept override { return NoIids(count, iids); }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) noexcept override;
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) noexcept override { return BaseTrust(level); }
    HRESULT STDMETHODCALLTYPE get_Current(WindowId* value) noexcept override;
    HRESULT STDMETHODCALLTYPE get_HasCurrent(boolean* value) noexcept override;
    HRESULT STDMETHODCALLTYPE MoveNext(boolean* value) noexcept override;
    HRESULT STDMETHODCALLTYPE GetMany(UINT32 capacity, WindowId* items, UINT32* taken) noexcept override;

private:
    WindowIds ids_{};     // WAIVER(R2): the list being handed over, replaced whole before each call.
    std::size_t count_{}; // WAIVER(R2): how much of it is in use, replaced whole with it.
    std::size_t at_{};    // WAIVER(R2): how far the reader has got, which is what an iterator is.
};

void Iterator::Reset(const WindowIds& ids, std::size_t count) noexcept
{
    ids_ = ids;
    count_ = count;
    at_ = 0;
}

// WAIVER(R17): a vtable entry called by Windows, which discards nothing and ignores attributes.
HRESULT Iterator::GetRuntimeClassName(HSTRING* name) noexcept
{
    *name = nullptr;
    return E_NOTIMPL;
}

// WAIVER(R17): a vtable entry called by Windows, which discards nothing and ignores attributes.
HRESULT Iterator::get_Current(WindowId* value) noexcept
{
    if (at_ >= count_)
        return kOutOfBounds;
    *value = ids_[at_];
    return S_OK;
}

// WAIVER(R17): a vtable entry called by Windows, which discards nothing and ignores attributes.
HRESULT Iterator::get_HasCurrent(boolean* value) noexcept
{
    *value = at_ < count_ ? TRUE : FALSE;
    return S_OK;
}

// WAIVER(R17): a vtable entry called by Windows, which discards nothing and ignores attributes.
HRESULT Iterator::MoveNext(boolean* value) noexcept
{
    at_ = std::min(at_ + 1, count_); // WAIVER(R2): one step of the reader, which is the whole of its job.
    return get_HasCurrent(value);
}

// WAIVER(R17): a vtable entry called by Windows, which discards nothing and ignores attributes.
HRESULT Iterator::GetMany(UINT32 capacity, WindowId* items, UINT32* taken) noexcept
{
    const std::size_t many = std::min(static_cast<std::size_t>(capacity), count_ - at_);
    std::ranges::copy_n(ids_.begin() + static_cast<std::ptrdiff_t>(at_), static_cast<std::ptrdiff_t>(many), items);
    at_ += many; // WAIVER(R2): the reader has got that much further.
    *taken = static_cast<UINT32>(many);
    return S_OK;
}

class Iterable final : public IWindowIdIterable
{
public:
    void Reset(const WindowIds& ids, std::size_t count) noexcept;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID asked, void** out) noexcept override { return Answer(asked, kIterableIid, this, out); }
    ULONG STDMETHODCALLTYPE AddRef() noexcept override { return 2; }
    ULONG STDMETHODCALLTYPE Release() noexcept override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) noexcept override { return NoIids(count, iids); }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) noexcept override;
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) noexcept override { return BaseTrust(level); }
    HRESULT STDMETHODCALLTYPE First(IWindowIdIterator** first) noexcept override;

private:
    WindowIds ids_{};     // WAIVER(R2): the list being handed over, replaced whole before each call.
    std::size_t count_{}; // WAIVER(R2): how much of it is in use, replaced whole with it.
    Iterator reader_{};   // WAIVER(R2): the one reader it hands out, wound back for each walk.
};

void Iterable::Reset(const WindowIds& ids, std::size_t count) noexcept
{
    ids_ = ids;
    count_ = count;
}

// WAIVER(R17): a vtable entry called by Windows, which discards nothing and ignores attributes.
HRESULT Iterable::GetRuntimeClassName(HSTRING* name) noexcept
{
    *name = nullptr;
    return E_NOTIMPL;
}

// WAIVER(R17): a vtable entry called by Windows, which discards nothing and ignores attributes.
HRESULT Iterable::First(IWindowIdIterator** first) noexcept
{
    Note("Windows walked our list");
    reader_.Reset(ids_, count_);
    *first = &reader_;
    return S_OK;
}

// One list for the program, so a pointer Windows kept stays good however many sessions are built.
[[nodiscard]] Iterable& TheList() noexcept
{
    // WAIVER(R11): one per program, kept at a fixed address in case Windows holds the pointer past the call.
    static Iterable list;
    return list;
}

[[nodiscard]] Com<IDisplaySession> DisplaySessionOf(IGraphicsCaptureSession* session) noexcept
{
    Com<IDisplaySession> display;
    (void)session->QueryInterface(kDisplaySessionIid, reinterpret_cast<void**>(display.GetAddressOf()));
    return display;
}

// A WindowId is not a window handle, so one has to be asked for. These two live in an API set rather than
// in any library this links against, and are absent on a Windows too old to know them.
using WindowIdOfWindow = HRESULT(WINAPI*)(HWND, WindowId*);
using WindowOfWindowId = HRESULT(WINAPI*)(WindowId, HWND*);

constexpr wchar_t kWindowingSet[] = L"ext-ms-win-windowing-external-l1-1-0.dll";

struct Interop
{
    WindowIdOfWindow toId;
    WindowOfWindowId toWindow;
};

[[nodiscard]] Interop ResolvedIn(HMODULE library) noexcept
{
    return Interop{ .toId = reinterpret_cast<WindowIdOfWindow>(reinterpret_cast<void*>(::GetProcAddress(library, "GetWindowIdFromWindow"))),
                    .toWindow = reinterpret_cast<WindowOfWindowId>(reinterpret_cast<void*>(::GetProcAddress(library, "GetWindowFromWindowId"))) };
}

[[nodiscard]] Interop Noted(const Interop& found) noexcept
{
    NoteTwo("GetWindowIdFromWindow / GetWindowFromWindowId", reinterpret_cast<std::uintptr_t>(found.toId), reinterpret_cast<std::uintptr_t>(found.toWindow));
    return found;
}

[[nodiscard]] Interop Resolved() noexcept
{
    const HMODULE library = ::LoadLibraryExW(kWindowingSet, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    NoteOne("apiset loaded", reinterpret_cast<std::uintptr_t>(library));
    if (library == nullptr)
        return Interop{ .toId = nullptr, .toWindow = nullptr };
    return Noted(ResolvedIn(library));
}

[[nodiscard]] const Interop& TheInterop() noexcept
{
    // WAIVER(R11): resolved once for the program, from a library that is never let go of again.
    static const Interop interop = Resolved();
    return interop;
}

// An id is only believed when Windows turns it back into the window it was made from. An id that means
// nothing would go into the list, read back out of it, and be taken for success.
[[nodiscard]] bool NamesTheWindow(const Interop& interop, WindowId id, HWND window) noexcept
{
    HWND back = nullptr;
    const HRESULT hr = interop.toWindow(id, &back);
    NoteTwo("id back to window: hwnd, came back as", reinterpret_cast<std::uintptr_t>(window), reinterpret_cast<std::uintptr_t>(back));
    if (FAILED(hr))
        return false;
    return back == window;
}

[[nodiscard]] std::optional<WindowId> CheckedAgainst(const Interop& interop, WindowId id, HWND window) noexcept
{
    return NamesTheWindow(interop, id, window) ? std::optional<WindowId>{ id } : std::nullopt;
}

[[nodiscard]] std::optional<WindowId> ConvertedBy(const Interop& interop, HWND window) noexcept
{
    WindowId id{};
    const HRESULT hr = interop.toId(window, &id);
    NoteTwo("window to id: hr, id", static_cast<unsigned long>(hr), id.Value);
    if (FAILED(hr))
        return std::nullopt;
    return CheckedAgainst(interop, id, window);
}

[[nodiscard]] bool HasBoth(const Interop& interop) noexcept
{
    return interop.toId != nullptr && interop.toWindow != nullptr;
}

[[nodiscard]] std::optional<WindowId> IdOf(HWND window) noexcept
{
    if (!HasBoth(TheInterop()))
        return std::nullopt;
    return ConvertedBy(TheInterop(), window);
}

[[nodiscard]] bool IsThere(const std::optional<WindowId>& id) noexcept
{
    return id.has_value();
}

// All of them or none: a list missing one of our windows would leave that one in the capture, and the
// windows are uncovered on the strength of the list being complete.
[[nodiscard]] std::size_t Filled(WindowIds& ids, std::span<const HWND> windows) noexcept
{
    const std::size_t many = std::min(windows.size(), kMaxExcluded);
    const auto converted = windows.first(many) | std::views::transform([](HWND w) { return IdOf(w); });
    if (!std::ranges::all_of(converted, IsThere))
        return 0;
    std::ranges::transform(converted, ids.begin(), [](const std::optional<WindowId>& id) { return *id; });
    return many;
}

[[nodiscard]] bool Names(IWindowIdVectorView* held, WindowId wanted) noexcept
{
    UINT32 index = 0;
    boolean found = FALSE;
    const HRESULT hr = held->IndexOf(wanted, &index, &found);
    NoteTwo("IndexOf: hr, found", static_cast<unsigned long>(hr), found);
    return SUCCEEDED(hr) && found != FALSE;
}

void NoteAt(IWindowIdVectorView* held, UINT32 at) noexcept
{
    WindowId item{};
    const HRESULT hr = held->GetAt(at, &item);
    NoteTwo("  holds", static_cast<unsigned long>(hr), item.Value);
}

// What the session says it holds, read out one at a time rather than trusted. A list that comes back
// empty, or short, or holding something else, is the difference between our fault and Windows's.
void NoteHeld(IWindowIdVectorView* held) noexcept
{
    UINT32 size = 0;
    NoteOne("GetWindowExclusionList size hr", static_cast<unsigned long>(held->get_Size(&size)));
    NoteOne("  size", size);
    std::ranges::for_each(std::views::iota(0u, size), [held](UINT32 at) { NoteAt(held, at); });
}

[[nodiscard]] bool NamesAll(IWindowIdVectorView* held, const WindowIds& ids, std::size_t count) noexcept
{
    return std::ranges::all_of(std::span<const WindowId>(ids.data(), count), [held](WindowId id) { return Names(held, id); });
}

// Read back what the session says it is excluding and look for our own windows in it. Uncovering a window
// on the strength of a call that quietly did nothing would put the overlay back into its own capture.
[[nodiscard]] bool CameBack(HRESULT hr, const Com<IWindowIdVectorView>& held) noexcept
{
    return SUCCEEDED(hr) && held != nullptr;
}

[[nodiscard]] bool Searched(const Com<IWindowIdVectorView>& held, const WindowIds& ids, std::size_t count) noexcept
{
    NoteHeld(held.Get());
    return NamesAll(held.Get(), ids, count);
}

[[nodiscard]] bool HoldsAll(IDisplaySession* display, const WindowIds& ids, std::size_t count) noexcept
{
    Com<IWindowIdVectorView> held;
    const HRESULT hr = display->GetWindowExclusionList(held.GetAddressOf());
    NoteTwo("GetWindowExclusionList: hr, view", static_cast<unsigned long>(hr), reinterpret_cast<std::uintptr_t>(held.Get()));
    if (!CameBack(hr, held))
        return false;
    return Searched(held, ids, count);
}

[[nodiscard]] HRESULT Handing(IDisplaySession* display, const WindowIds& ids, std::size_t count) noexcept
{
    UINT64 iteration = 0;
    TheList().Reset(ids, count);
    const HRESULT hr = display->SetWindowExclusionList(&TheList(), &iteration);
    NoteTwo("SetWindowExclusionList: hr, iteration", static_cast<unsigned long>(hr), iteration);
    return hr;
}

[[nodiscard]] bool Told(IDisplaySession* display, const WindowIds& ids, std::size_t count) noexcept
{
    if (FAILED(Handing(display, ids, count)))
        return false;
    return HoldsAll(display, ids, count);
}

[[nodiscard]] bool ToldIfAny(IDisplaySession* display, const WindowIds& ids, std::size_t count) noexcept
{
    return count != 0 && Told(display, ids, count);
}

} // namespace

bool SessionCanExcludeWindows(IGraphicsCaptureSession* session) noexcept
{
    return DisplaySessionOf(session) != nullptr;
}

void NoteHeldIfAny(IDisplaySession* display, Com<IWindowIdVectorView>& held) noexcept
{
    NoteOne("list now: hr", static_cast<unsigned long>(display->GetWindowExclusionList(held.GetAddressOf())));
    if (held == nullptr)
        return;
    NoteHeld(held.Get());
}

[[nodiscard]] bool Listed(IDisplaySession* display, std::span<const HWND> windows) noexcept
{
    WindowIds ids{}; // WAIVER(R2): a local list filled once, before it is handed over.
    const std::size_t count = Filled(ids, windows);
    NoteOne("windows converted", count);
    return ToldIfAny(display, ids, count);
}

void NoteExclusion(const char* line) noexcept
{
    Note(line);
}

void NoteExclusionWide(const wchar_t* text) noexcept
{
    std::array<char, 1024> narrow{}; // WAIVER(R2): a local buffer filled once, before it is written out.
    const auto room = std::views::iota(std::size_t{ 0 }, narrow.size() - 1) | std::views::take_while([text](std::size_t at) { return text[at] != L'\0'; });
    std::ranges::for_each(room, [&](std::size_t at) { narrow[at] = static_cast<char>(text[at]); });
    Note(narrow.data());
}

void NoteExclusionList(IGraphicsCaptureSession* session, const char* when) noexcept
{
    const Com<IDisplaySession> display = DisplaySessionOf(session);
    if (display == nullptr)
        return;
    Com<IWindowIdVectorView> held;
    Note(when);
    NoteHeldIfAny(display.Get(), held);
}

bool ExcludeWindowsFrom(IGraphicsCaptureSession* session, std::span<const HWND> windows) noexcept
{
    const Com<IDisplaySession> display = DisplaySessionOf(session);
    NoteTwo("--- session, display interface", reinterpret_cast<std::uintptr_t>(session), reinterpret_cast<std::uintptr_t>(display.Get()));
    if (display == nullptr)
        return false;
    return Listed(display.Get(), windows);
}

} // namespace real
