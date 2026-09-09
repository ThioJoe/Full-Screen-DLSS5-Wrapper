#include "effects/real/panel.h"

#include "effects/real/window.h"
#include "infrastructure/array_util.h"
#include "infrastructure/text.h"
#include "interior/ngx_params.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <ranges>
#include <span>
#include <string_view>

namespace real {
namespace {

using infra::Fail;
using infra::Result;

constexpr wchar_t kPanelClass[] = L"DlssScreenControlPanel";
constexpr int kReferenceDpi = 96;
constexpr int kTextCapacity = 32;
constexpr int kPathCapacity = 260;
constexpr float kDefaultSplit = 0.5f;
constexpr auto kCentre = interior::FractionTag::Parse(kDefaultSplit);
static_assert(kCentre.has_value());

// The layout in reference pixels, two columns of rows per page.
constexpr int kMargin = 12;
constexpr int kColumnWidth = 384;
constexpr int kColumns = 2;
constexpr int kPanelWidth = kColumns * kColumnWidth + (kColumns + 1) * kMargin;
constexpr int kSliderWidth = 196;
constexpr int kBoxOffset = 214;
constexpr int kBoxWidth = 78;
constexpr int kResetOffset = 302;
constexpr int kResetWidth = 28;
constexpr int kChoiceWidth = 122;
constexpr int kTabHeight = 30;
// A runtime list carries names rather than words, so its buttons are wider and fewer to a line.
constexpr int kListChoiceWidth = 186;
constexpr std::size_t kListPerLine = 2;
constexpr wchar_t kChevronGlyph[] = L"\uE70D";
constexpr wchar_t kNoticeLine[] = L"NOTICE: this is not representative of a native DLSS 5 implementation";
constexpr wchar_t kNoticeBody[] =
    L"A game hands the model its own motion vectors, its own depth buffer and the sub-pixel jitter it rendered with, frame by frame, before anything is composited. DlssScreen has none of "
    L"that. It captures the finished desktop and makes substitutes: one flat depth plane, and motion guessed by matching blocks between two pictures that have already been drawn, "
    L"resized and blended by the window manager.\r\n\r\n"
    L"So the model here is working from worse inputs than it was built for, on an image that has already lost the information it wants. What it does to the desktop is not what it does in a "
    L"game, and neither is what it costs: the capture, the matching and the extra copies are all work a game would not be doing, and none of it is part of DLSS.\r\n\r\n"
    L"Judge DLSS 5 by a game that implements it. This is a way to see the model run on something it was never given, not a preview of what it does when it is used properly.";
constexpr int kNoticeLines = 9;
constexpr int kExpanderWidth = 28;

constexpr int kMinRowsPerColumn = 9;
constexpr int kMaxRowsPerColumn = 16;

// Each slider counts in steps of a unit: 1 counts whole numbers, 100 counts hundredths. A range says how
// far a slider reaches, not what the model accepts; the command line still takes any finite value.
struct FieldSpec
{
    const wchar_t* label;
    const wchar_t* hint;
    int minimum;
    int maximum; // where the slider ends, which for an open field is only where it ends to begin with
    int steps;
    bool open; // whether the model lets the number go on past the slider's end
};

// As far as a number the model puts no top on may be typed or stepped. The slider stretches to follow.
constexpr int kOpenUnits = 1000;

[[nodiscard]] constexpr int CeilingOf(const FieldSpec& spec) noexcept
{
    return spec.open ? kOpenUnits * spec.steps : spec.maximum;
}

constexpr std::array<FieldSpec, kFieldCount> kFields{ {
    { L"Intensity", L"How much of the model's work to keep. Past 1 the model makes no further difference, so 1 is the whole of it.", 0, 100, 100, false },
    { L"Local structure", L"Detail the model adds within a region. The slider's end is not the model's: type or step past it and the slider follows. Does nothing while auto mask is off.", 0, 1000,
      100, true },
    { L"Local tone", L"How far the model moves local brightness. The slider's end is not the model's: type or step past it and the slider follows.", 0, 1000, 100, true },
    { L"Skin structure", L"Detail on skin. The slider's end is not the model's. Does nothing while auto mask is off, or while skin follows local structure.", 0, 1000, 100, true },
    { L"Motion vector scale X", L"What the model multiplies the horizontal motion by. 1 passes the synthesised vectors through unchanged.", -400, 400, 100, false },
    { L"Motion vector scale Y", L"What the model multiplies the vertical motion by. 1 passes the synthesised vectors through unchanged.", -400, 400, 100, false },
    { L"Split position", L"Where the divider sits in the split view. Ctrl+Alt+Shift and the mouse drags it on screen.", 0, 100, 100, false },
    { L"Depth plane", L"The desktop has no depth, so one flat value stands in for all of it. Changing it re-clears the plane.", 0, 100, 100, false },
    { L"Reset threshold", L"How much of the picture has to go unmatched before the model's history is thrown away.", 0, 100, 100, false },
    { L"Motion detail level", L"Finest level the matcher works at: 0 full resolution, 1 half, 2 quarter. Lower costs more.", 0, 7, 1, false },
    { L"Super resolution preset", L"Render preset asked of DLSS Super Resolution; 0 leaves the choice to the driver.", 0, 15, 1, false },
} };

struct ToggleSpec
{
    const wchar_t* label;
    const wchar_t* hint;
    bool resettable; // a switch whose default is not obvious from the switch itself
};

constexpr std::array<ToggleSpec, kToggleCount> kToggles{ {
    { L"Run the model", L"Whether the model runs at all. Off costs nothing and shows the captured picture as it was.", false },
    { L"Auto mask", L"Let the model find skin itself. Skin structure and local structure do nothing while this is off.", true },
    { L"Skin follows local structure",
      L"Give skin whatever local structure is given, which is what the model reads -1 as. It is the only value between -1 and 0 that means anything, so it is a switch rather than part of the slider.",
      true },
    { L"UI correction", L"Ask the model to leave interface pixels alone. It reads a UI layer DlssScreen does not supply, so this is inert as wired.", true },
    { L"Depth is inverted", L"Tell the model the depth plane counts the other way. With one flat plane it changes little.", true },
    { L"Wait for the display", L"Present in step with the monitor. Off presents as fast as the pipeline allows.", true },
    { L"Capture border", L"Let Windows draw its yellow border around what is being captured.", true },
    { L"Always on top", L"Keep the output window above every other window.", true },
    { L"Redirection surface", L"Give the output window a GDI surface. Diagnostic; fixed when the window is made.", true },
    { L"Direct3D debug layer", L"Turn on the Direct3D 12 validation layer. Slow, and only useful when chasing a fault.", true },
    { L"Model indicator", L"Let the model draw its own overlay naming its version, the preset it resolved and its working size.", true },
    { L"Model kernel cache", L"Let the model cache its compiled kernels. Off makes it rebuild them every run.", true },
} };

struct GroupSpec
{
    const wchar_t* label;
    const wchar_t* hint;
    std::size_t count;
    std::array<const wchar_t*, kMaxChoices> choices;
};

constexpr std::array<GroupSpec, kGroupCount> kGroups{ {
    { L"Compare", L"What the window shows: the model's work, the picture as captured, or both either side of a divider.", 3, { L"Processed", L"Original", L"Split" } },
    { L"Style", L"Which of the model's three looks to ask for. The model clamps anything else.", 3, { L"Standard", L"Natural", L"Cinematic" } },
    { L"Cursor", L"Whether the captured picture includes the mouse pointer. Auto keeps the session's own choice.", 3, { L"Auto", L"On", L"Off" } },
    { L"Motion vectors", L"Where the model's motion comes from: matching blocks between frames, the hardware flow engine, or nothing at all.", 3, { L"Block matching", L"Optical flow", L"None" } },
    { L"Optical flow grid", L"How coarse the hardware flow engine's output is.", 3, { L"1", L"2", L"4" } },
    { L"Optical flow effort", L"How hard the hardware flow engine works.", 3, { L"Slow", L"Medium", L"Fast" } },
    { L"Super resolution", L"Whether DLSS Super Resolution runs before the model, and whether it runs at all when the sizes match.", 3, { L"Auto", L"DLAA", L"Off" } },
    { L"Colour format", L"How much precision the model's picture carries.", 2, { L"8 bit", L"16 bit float", nullptr } },
    { L"Log level", L"How much the log says.", 4, { L"Debug", L"Info", L"Warn", L"Error" } },
    { L"Console", L"Whether to use the console the program was launched from, make one, or go without.", 3, { L"Auto", L"On", L"Off" } },
} };

struct TextSpec
{
    const wchar_t* label;
    const wchar_t* hint;
};

struct ListSpec
{
    const wchar_t* label;
    const wchar_t* hint;
};

constexpr std::array<ListSpec, kListCount> kLists{ {
    { L"Preset", L"Which of the model's sets of weights to ask for. Only the ones the model says it carries are offered." },
    { L"Source", L"What to capture: the monitor Windows calls primary, every monitor as one picture, or one named monitor." },
    { L"Target", L"Which monitor to present on. Presenting on a different one asks super resolution to bridge the two sizes." },
    { L"Adapter", L"Which graphics adapter to run the model on." },
} };

constexpr std::array<TextSpec, kTextCount> kTexts{ {
    { L"Window title",
      L"Part of the title of the one window to work on, ignoring case. Empty captures monitors instead. The capture follows the window as it moves; resizing it needs a new session." },
    { L"Log file", L"Where to mirror the log. Empty writes to the console only. Read when the session starts." },
} };

// --- what sits on which page, and in what order -------------------------------------------------------

enum class Kind : std::uint8_t { Field, Toggle, Group, Text, List };

struct RowSpec
{
    Kind kind;
    std::size_t index;
};

[[nodiscard]] constexpr RowSpec Of(Field f) noexcept
{
    return RowSpec{ Kind::Field, static_cast<std::size_t>(f) };
}
[[nodiscard]] constexpr RowSpec Of(Toggle t) noexcept
{
    return RowSpec{ Kind::Toggle, static_cast<std::size_t>(t) };
}
[[nodiscard]] constexpr RowSpec Of(Group g) noexcept
{
    return RowSpec{ Kind::Group, static_cast<std::size_t>(g) };
}
[[nodiscard]] constexpr RowSpec Of(Text t) noexcept
{
    return RowSpec{ Kind::Text, static_cast<std::size_t>(t) };
}
[[nodiscard]] constexpr RowSpec Of(List l) noexcept
{
    return RowSpec{ Kind::List, static_cast<std::size_t>(l) };
}

constexpr std::size_t kMaxRows = kColumns * kMaxRowsPerColumn;

struct PageSpec
{
    const wchar_t* title;
    std::size_t count;
    std::array<RowSpec, kMaxRows> rows;
};

constexpr std::array<PageSpec, static_cast<std::size_t>(Page::Count)> kPages{ {
    { L"Model",
      15,
      { Of(Toggle::NeuralRendering), Of(Group::Style), Of(List::Preset), Of(Field::Intensity), Of(Field::LocalStructure), Of(Field::LocalTone), Of(Toggle::SkinFollowsStructure), Of(Field::Skin),
        Of(Toggle::AutoMask), Of(Toggle::UiCorrection), Of(Toggle::DepthInverted), Of(Field::DepthValue), Of(Field::ResetThreshold), Of(Field::MvScaleX), Of(Field::MvScaleY) } },
    { L"View", 7, { Of(Group::Compare), Of(Field::Split), Of(Toggle::Vsync), Of(Group::Cursor), Of(Toggle::CaptureBorder), Of(Toggle::Topmost), Of(Group::LogLevel) } },
    { L"Start-up",
      17,
      { Of(List::Source), Of(List::Target), Of(Group::Format), Of(Group::Sr), Of(Field::SrPreset), Of(Group::Motion), Of(Field::MvLevel), Of(Group::NvofGrid), Of(Group::NvofPerf), Of(List::Adapter),
        Of(Toggle::RedirectionBitmap), Of(Toggle::DebugLayer), Of(Toggle::Indicator), Of(Toggle::CubinCache), Of(Group::Console), Of(Text::Window), Of(Text::LogFile) } },
} };

// Every control belongs to exactly one page. One left off would be placed nowhere and stop the program as
// it starts, so the tables are counted here instead of trusted.
[[nodiscard]] constexpr std::size_t RowsOfKind(Kind kind) noexcept
{
    std::size_t total = 0; // WAIVER(R2): a count folded while compiling, not state the program keeps.
    for (const PageSpec& page : kPages)
        total += static_cast<std::size_t>(std::ranges::count_if(std::span<const RowSpec>(page.rows.data(), page.count), [kind](const RowSpec& r) { return r.kind == kind; }));
    return total;
}

static_assert(RowsOfKind(Kind::Field) == kFieldCount);
static_assert(RowsOfKind(Kind::Toggle) == kToggleCount);
static_assert(RowsOfKind(Kind::Group) == kGroupCount);
static_assert(RowsOfKind(Kind::Text) == kTextCount);
static_assert(RowsOfKind(Kind::List) == kListCount);

[[nodiscard]] std::span<const RowSpec> RowsOf(Page page) noexcept
{
    const PageSpec& spec = kPages[static_cast<std::size_t>(page)];
    return std::span<const RowSpec>(spec.rows.data(), spec.count);
}

// --- measurement ---------------------------------------------------------------------------------------

// Everything is measured in the display's own dots and in the height of a line of its own text, so a row
// is always tall enough for what it holds however the display is scaled.
struct Metrics
{
    int dpi;
    int line;
    int rows;                // slots to a column, taken from whichever page needs the most
    const PanelLists* lists; // borrowed for as long as the panel is being built, which is the only time it is read
    [[nodiscard]] int Of(int reference) const noexcept { return ::MulDiv(reference, dpi, kReferenceDpi); }
    [[nodiscard]] int LabelHeight() const noexcept { return line + Of(5); }
    [[nodiscard]] int ControlHeight() const noexcept { return std::max(Of(22), line + Of(9)); }
    [[nodiscard]] int RowHeight() const noexcept { return LabelHeight() + ControlHeight() + Of(10); }
    [[nodiscard]] int PageTop() const noexcept { return Of(kMargin + kTabHeight); }
    // The rows, then the button that starts a new session below them, then the notice, then the margin.
    [[nodiscard]] int PageHeight() const noexcept { return rows * RowHeight() + 2 * ControlHeight() + Of(3 * kMargin); }
    [[nodiscard]] int ButtonTop() const noexcept { return PageTop() + rows * RowHeight(); }
    [[nodiscard]] int NoticeTop() const noexcept { return ButtonTop() + ControlHeight() + Of(kMargin); }
    [[nodiscard]] int BodyHeight() const noexcept { return kNoticeLines * line + Of(kMargin); }
};

struct Placement
{
    int left; // in reference pixels
    int top;  // in the display's dots
    int control;
};

// A row holding a runtime list is as tall as the list needs; every other row is one slot high. A list with
// nothing in it takes no room at all, which is how a page drops a choice the machine could not offer.
[[nodiscard]] std::size_t LinesOfList(std::size_t count) noexcept
{
    return (count + kListPerLine - 1) / kListPerLine;
}

[[nodiscard]] std::size_t SlotsOf(const RowSpec& row, const PanelLists& lists) noexcept
{
    if (row.kind != Kind::List)
        return 1;
    return LinesOfList(lists[row.index].choices.Size());
}

// Where the next row starts. A row never straddles a column, so one that will not fit in what is left of
// this column begins the next one.
struct Cell
{
    std::size_t column;
    std::size_t slot;
};

[[nodiscard]] Cell Fitted(const Cell& at, std::size_t slots, std::size_t rows) noexcept
{
    if (at.slot + slots <= rows)
        return at;
    return Cell{ at.column + 1, 0 };
}

[[nodiscard]] Placement PlaceOfCell(const Cell& at, const Metrics& m) noexcept
{
    const int top = m.PageTop() + static_cast<int>(at.slot) * m.RowHeight();
    return Placement{ kMargin + static_cast<int>(at.column) * (kColumnWidth + kMargin), top, top + m.LabelHeight() };
}

// --- the window and its furniture ------------------------------------------------------------------------

// WAIVER(R17): the window procedure is called by the OS, which discards nothing and ignores attributes.
LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
{
    // Closing hides the panel rather than destroying it; the session reads that as the operator leaving.
    if (message == WM_CLOSE)
    {
        ::ShowWindow(window, SW_HIDE);
        return 0;
    }
    return ::DefWindowProcW(window, message, w, l);
}

[[nodiscard]] WNDCLASSEXW ClassDescription() noexcept
{
    return WNDCLASSEXW{ sizeof(WNDCLASSEXW), 0,      PanelProc, 0, 0, ::GetModuleHandleW(nullptr), nullptr, ::LoadCursorW(nullptr, IDC_ARROW), reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1), nullptr,
                        kPanelClass,         nullptr };
}

// Advisory: the older common controls register their classes as they load and refuse this call, while
// version 6 needs asking. Either way the controls are checked once built, which is the answer that counts.
void InitialiseCommonControls() noexcept
{
    INITCOMMONCONTROLSEX controls{ sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_TAB_CLASSES };
    (void)::InitCommonControlsEx(&controls);
}

// Segoe MDL2 Assets has shipped with Windows since 10, and its refresh glyph fits a button too short for a word.
constexpr wchar_t kRefreshGlyph[] = L"\uE72C";
constexpr wchar_t kResetHint[] = L"Put this setting back to the value it starts at.";

[[nodiscard]] UniqueFont IconFont(int dpi) noexcept
{
    LOGFONTW description{}; // WAIVER(R2): a request record filled once, before it is asked.
    description.lfHeight = -::MulDiv(11, dpi, kReferenceDpi);
    description.lfCharSet = DEFAULT_CHARSET;
    ENSURE(::wcscpy_s(description.lfFaceName, L"Segoe MDL2 Assets") == 0);
    return UniqueFont(::CreateFontIndirectW(&description));
}

// The font the rest of Windows writes its dialogs in, asked for at this display's scale. The plain query
// answers for the primary display, and scaling that answer again is what made the text outgrow its labels.
[[nodiscard]] LOGFONTW MessageDescription(int dpi) noexcept
{
    NONCLIENTMETRICSW metrics{}; // WAIVER(R2): a request record filled once, before it is asked.
    metrics.cbSize = sizeof(NONCLIENTMETRICSW);
    ENSURE(::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &metrics, 0, static_cast<UINT>(dpi)) != FALSE);
    return metrics.lfMessageFont;
}

[[nodiscard]] UniqueFont MessageFont(int dpi) noexcept
{
    const LOGFONTW description = MessageDescription(dpi);
    return UniqueFont(::CreateFontIndirectW(&description));
}

[[nodiscard]] UniqueFont BoldFont(int dpi) noexcept
{
    LOGFONTW description = MessageDescription(dpi); // WAIVER(R2): a request record, weighted once before it is asked.
    description.lfWeight = FW_BOLD;
    return UniqueFont(::CreateFontIndirectW(&description));
}

[[nodiscard]] int MeasuredOn(HDC dc, HFONT font) noexcept
{
    const HGDIOBJ previous = ::SelectObject(dc, font);
    TEXTMETRICW text{}; // WAIVER(R2): an answer record filled once by the measurement below.
    ENSURE(::GetTextMetricsW(dc, &text) != FALSE);
    (void)::SelectObject(dc, previous);
    return static_cast<int>(text.tmHeight);
}

[[nodiscard]] int LineHeight(HWND window, HFONT font) noexcept
{
    const HDC dc = ::GetDC(window);
    ENSURE(dc != nullptr);
    const int height = MeasuredOn(dc, font);
    ENSURE(::ReleaseDC(window, dc) == 1);
    return height;
}

[[nodiscard]] HWND CreateTooltip(HWND parent) noexcept
{
    return ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
}

[[nodiscard]] HWND CreateChild(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, DWORD extended, RECT bounds) noexcept
{
    return ::CreateWindowExW(extended, className, text, WS_CHILD | WS_VISIBLE | style, bounds.left, bounds.top, bounds.right, bounds.bottom, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

// Horizontal places are given in reference pixels and scaled; vertical ones are already in the display's
// dots, because they follow the text.
[[nodiscard]] RECT Bounds(const Metrics& m, int x, int top, int width, int height) noexcept
{
    return RECT{ m.Of(x), top, m.Of(width), height };
}

[[nodiscard]] HWND CreateLabel(HWND parent, const Metrics& m, const wchar_t* text, int x, int top, int width) noexcept
{
    return CreateChild(parent, WC_STATICW, text, SS_LEFT, 0, Bounds(m, x, top, width, m.LabelHeight()));
}

[[nodiscard]] HWND CreateButton(HWND parent, const Metrics& m, const wchar_t* text, DWORD style, int x, int top, int width) noexcept
{
    return CreateChild(parent, WC_BUTTONW, text, style, 0, Bounds(m, x, top, width, m.ControlHeight()));
}

[[nodiscard]] TTTOOLINFOW HintFor(HWND parent, HWND control, const wchar_t* text) noexcept
{
    TTTOOLINFOW info{ sizeof(TTTOOLINFOW), TTF_IDISHWND | TTF_SUBCLASS, parent, reinterpret_cast<UINT_PTR>(control), RECT{}, nullptr, const_cast<wchar_t*>(text), 0, nullptr };
    return info;
}

void AddHint(HWND tooltip, HWND parent, HWND control, const wchar_t* text) noexcept
{
    TTTOOLINFOW info = HintFor(parent, control, text);
    (void)::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
}

// --- reading and writing a number ------------------------------------------------------------------------

[[nodiscard]] int SliderPosition(HWND slider) noexcept
{
    return static_cast<int>(::SendMessageW(slider, TBM_GETPOS, 0, 0));
}

[[nodiscard]] int SpinPosition(HWND spin) noexcept
{
    return static_cast<int>(::SendMessageW(spin, UDM_GETPOS32, 0, 0));
}

[[nodiscard]] std::array<wchar_t, kTextCapacity> TextOf(HWND control) noexcept
{
    std::array<wchar_t, kTextCapacity> text{}; // WAIVER(R2): a local buffer filled once, before use.
    (void)::GetWindowTextW(control, text.data(), kTextCapacity);
    return text;
}

[[nodiscard]] std::optional<int> TypedSteps(HWND box, const FieldSpec& spec) noexcept
{
    const std::array<wchar_t, kTextCapacity> text = TextOf(box);
    wchar_t* end = nullptr;
    const float value = std::wcstof(text.data(), &end);
    if (end == text.data())
        return std::nullopt;
    return std::clamp(static_cast<int>(std::lround(value * static_cast<float>(spec.steps))), spec.minimum, CeilingOf(spec));
}

// The slider, the box and the arrows say the same number, so of the two that disagree the one the operator
// moved is the one that no longer matches the box, which still shows what all three last agreed on.
[[nodiscard]] int AwayFromBox(int slider, int spin, int typed) noexcept
{
    return slider != typed ? slider : spin;
}

// Answering with the slider whichever of the two had moved is what made the arrows look dead: they moved
// the spin, the slider answered, and the old value went straight back over them.
[[nodiscard]] int Moved(int slider, int spin, const std::optional<int>& typed) noexcept
{
    return typed.has_value() ? AwayFromBox(slider, spin, *typed) : slider;
}

[[nodiscard]] int Settled(const ControlPanel& panel, std::size_t field) noexcept
{
    const int slider = SliderPosition(panel.sliders[field]);
    const int spin = SpinPosition(panel.spins[field]);
    const std::optional<int> typed = TypedSteps(panel.boxes[field], kFields[field]);
    if (slider != spin)
        return Moved(slider, spin, typed);
    return typed.value_or(spin);
}

[[nodiscard]] infra::BoundedString<char, 15> Printed(int steps, const FieldSpec& spec) noexcept
{
    if (spec.steps == 1)
        return infra::Formatted<15>("{}", steps);
    return infra::Formatted<15>("{:.2f}", static_cast<double>(steps) / spec.steps);
}

// The digits are ASCII, so widening them is a character-for-character copy.
[[nodiscard]] std::array<wchar_t, kTextCapacity> Widened(std::string_view text) noexcept
{
    std::array<wchar_t, kTextCapacity> wide{}; // WAIVER(R2): a local buffer filled once, before use.
    std::ranges::copy(text | std::views::take(wide.size() - 1) | std::views::transform([](char c) { return static_cast<wchar_t>(c); }), wide.begin());
    return wide;
}

// The box is left alone while it has the keyboard, or a half-typed number would be rewritten under it.
void ShowInBox(HWND box, int steps, const FieldSpec& spec) noexcept
{
    if (::GetFocus() == box)
        return;
    ENSURE(::SetWindowTextW(box, Widened(Printed(steps, spec).Get()).data()) != FALSE);
}

// A number carried past the slider's end takes the slider with it, so all three controls keep agreeing and
// the one that moved is still the one that stands out.
void StretchSlider(HWND slider, int steps) noexcept
{
    if (steps > static_cast<int>(::SendMessageW(slider, TBM_GETRANGEMAX, 0, 0)))
        (void)::SendMessageW(slider, TBM_SETRANGEMAX, TRUE, steps);
}

void Commit(const ControlPanel& panel, std::size_t field, int steps) noexcept
{
    StretchSlider(panel.sliders[field], steps);
    (void)::SendMessageW(panel.sliders[field], TBM_SETPOS, TRUE, steps);
    (void)::SendMessageW(panel.spins[field], UDM_SETPOS32, 0, steps);
    ShowInBox(panel.boxes[field], steps, kFields[field]);
}

[[nodiscard]] float SettledValue(const ControlPanel& panel, Field field) noexcept
{
    const std::size_t index = static_cast<std::size_t>(field);
    const int steps = Settled(panel, index);
    Commit(panel, index, steps);
    return static_cast<float>(steps) / static_cast<float>(kFields[index].steps);
}

// --- the switches ------------------------------------------------------------------------------------

[[nodiscard]] bool IsChecked(HWND check) noexcept
{
    return ::SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

void SetChecked(HWND check, bool checked) noexcept
{
    (void)::SendMessageW(check, BM_SETCHECK, checked ? BST_CHECKED : BST_UNCHECKED, 0);
}

[[nodiscard]] bool IsPushed(HWND button) noexcept
{
    return (::SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0;
}

[[nodiscard]] bool IsOn(const ControlPanel& panel, Toggle toggle) noexcept
{
    return IsChecked(panel.toggles[static_cast<std::size_t>(toggle)]);
}

[[nodiscard]] std::span<const HWND> ChoicesOf(const ControlPanel& panel, Group group) noexcept
{
    const std::size_t index = static_cast<std::size_t>(group);
    return std::span<const HWND>(panel.choices[index].data(), kGroups[index].count);
}

[[nodiscard]] std::span<const HWND> ChoicesOfList(const ControlPanel& panel, List list) noexcept
{
    const std::size_t index = static_cast<std::size_t>(list);
    return std::span<const HWND>(panel.listChoices[index].data(), panel.listCounts[index]);
}

// A list nobody could offer has no buttons, so what the session started with stands.
[[nodiscard]] std::size_t ChosenInList(const ControlPanel& panel, List list, std::size_t fallback) noexcept
{
    const std::span<const HWND> choices = ChoicesOfList(panel, list);
    const auto found = std::ranges::find_if(choices, IsChecked);
    if (found == choices.end())
        return fallback;
    return static_cast<std::size_t>(std::ranges::distance(choices.begin(), found));
}

[[nodiscard]] std::size_t ChosenIn(const ControlPanel& panel, Group group, std::size_t fallback) noexcept
{
    const std::span<const HWND> choices = ChoicesOf(panel, group);
    const auto found = std::ranges::find_if(choices, IsChecked);
    return found == choices.end() ? fallback : static_cast<std::size_t>(std::ranges::distance(choices.begin(), found));
}

void ChooseOnly(std::span<const HWND> group, std::size_t index) noexcept
{
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, group.size()), [group, index](std::size_t i) { SetChecked(group[i], i == index); });
}

// --- the values the controls start at -----------------------------------------------------------------

[[nodiscard]] int StepsOf(float value, const FieldSpec& spec) noexcept
{
    return std::clamp(static_cast<int>(std::lround(value * static_cast<float>(spec.steps))), spec.minimum, CeilingOf(spec));
}

[[nodiscard]] std::array<float, kFieldCount> StartingValues(const interior::Options& o, const interior::LiveSettings& live) noexcept
{
    return { live.tuning.intensity.Get(),
             live.tuning.localStructure.Get(),
             live.tuning.localTone.Get(),
             live.tuning.skinStructure.Get(),
             live.mvScaleX.Get(),
             live.mvScaleY.Get(),
             kDefaultSplit,
             live.depth.Get(),
             live.resetThreshold.Get(),
             static_cast<float>(o.motionFinestLevel.Get()),
             static_cast<float>(o.srPreset.Get()) };
}

[[nodiscard]] std::array<bool, kToggleCount> StartingToggles(const interior::Options& o, const interior::LiveSettings& live) noexcept
{
    return { live.neuralRendering,
             live.tuning.autoMask,
             live.tuning.skinStructure.Get() < 0.0f,
             live.tuning.uiCorrection,
             live.depthInverted,
             live.vsync,
             o.captureBorder,
             o.topmost,
             o.redirectionBitmap,
             o.debugLayer,
             o.indicator,
             o.cubinCache };
}

[[nodiscard]] std::size_t CodeOfGrid(interior::GridSize grid) noexcept
{
    constexpr std::array<interior::GridSize, 3> grids{ interior::GridSize::One, interior::GridSize::Two, interior::GridSize::Four };
    const auto found = std::ranges::find(grids, grid);
    return found == grids.end() ? 0 : static_cast<std::size_t>(std::ranges::distance(grids.begin(), found));
}

[[nodiscard]] std::array<std::size_t, kGroupCount> StartingChoices(const interior::Options& o, interior::DisplayMode display) noexcept
{
    return { static_cast<std::size_t>(display),    interior::StyleCode(o.tuning.style), static_cast<std::size_t>(o.cursor), static_cast<std::size_t>(o.motion),   CodeOfGrid(o.nvofGrid),
             static_cast<std::size_t>(o.nvofPerf), static_cast<std::size_t>(o.sr),      static_cast<std::size_t>(o.format), static_cast<std::size_t>(o.logLevel), static_cast<std::size_t>(o.console) };
}

// --- building the controls -----------------------------------------------------------------------------

[[nodiscard]] HWND CreateSlider(HWND parent, const Metrics& m, const FieldSpec& spec, const Placement& at, int steps) noexcept
{
    const HWND slider = CreateChild(parent, TRACKBAR_CLASSW, nullptr, TBS_HORZ | TBS_NOTICKS, 0, Bounds(m, at.left, at.control, kSliderWidth, m.ControlHeight()));
    if (slider == nullptr)
        return nullptr;
    (void)::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(spec.minimum, spec.maximum));
    (void)::SendMessageW(slider, TBM_SETPOS, TRUE, steps);
    return slider;
}

// One click is one step, which on a hundredths field is a hundredth. Held down, the arrow works up to a
// tenth of a unit and then to a whole one, so the far end of a range is reachable without a hundred clicks.
void AccelerateSpin(HWND spin, const FieldSpec& spec) noexcept
{
    std::array<UDACCEL, 3> curve{ { { 0, 1 }, { 1, static_cast<UINT>(std::max(spec.steps / 10, 1)) }, { 3, static_cast<UINT>(spec.steps) } } };
    (void)::SendMessageW(spin, UDM_SETACCEL, curve.size(), reinterpret_cast<LPARAM>(curve.data()));
}

[[nodiscard]] HWND ArrangedSpin(HWND spin, HWND box, const FieldSpec& spec, int steps) noexcept
{
    (void)::SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(box), 0);
    (void)::SendMessageW(spin, UDM_SETRANGE32, static_cast<WPARAM>(spec.minimum), static_cast<LPARAM>(CeilingOf(spec)));
    (void)::SendMessageW(spin, UDM_SETPOS32, 0, steps);
    AccelerateSpin(spin, spec);
    return spin;
}

[[nodiscard]] HWND CreateSpin(HWND parent, HWND box, const FieldSpec& spec, int steps) noexcept
{
    const HWND spin =
        ::CreateWindowExW(0, UPDOWN_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    return spin == nullptr ? nullptr : ArrangedSpin(spin, box, spec, steps);
}

struct Built
{
    std::array<HWND, kFieldCount> labels;
    std::array<HWND, kFieldCount> sliders;
    std::array<HWND, kFieldCount> boxes;
    std::array<HWND, kFieldCount> spins;
    std::array<HWND, kFieldCount> resets;
    std::array<HWND, kToggleCount> toggles;
    std::array<HWND, kToggleCount> toggleResets;
    std::array<HWND, kGroupCount> groupLabels;
    std::array<std::array<HWND, kMaxChoices>, kGroupCount> choices;
    std::array<HWND, kTextCount> textLabels;
    std::array<HWND, kTextCount> texts;
    std::array<HWND, kListCount> listLabels;
    std::array<std::array<HWND, kMaxListChoices>, kListCount> listChoices;
};

// Walking a page: each row is put where the cursor stands, and the cursor moves on by the row's height.
struct Walk
{
    Cell at;
    std::optional<Placement> found;
};

[[nodiscard]] bool IsWanted(const RowSpec& row, Kind kind, std::size_t index) noexcept
{
    return row.kind == kind && row.index == index;
}

[[nodiscard]] Walk Stepped(const Walk& so, const RowSpec& row, Kind kind, std::size_t index, const Metrics& m) noexcept
{
    const std::size_t slots = SlotsOf(row, *m.lists);
    const Cell at = Fitted(so.at, slots, static_cast<std::size_t>(m.rows));
    const std::optional<Placement> found = IsWanted(row, kind, index) ? std::optional<Placement>{ PlaceOfCell(at, m) } : so.found;
    return Walk{ Cell{ at.column, at.slot + slots }, found };
}

[[nodiscard]] std::optional<Placement> PlacementOn(Page page, Kind kind, std::size_t index, const Metrics& m) noexcept
{
    const auto step = [&](const Walk& so, const RowSpec& row) { return Stepped(so, row, kind, index, m); };
    return std::ranges::fold_left(RowsOf(page), Walk{ Cell{ 0, 0 }, std::nullopt }, step).found;
}

// Where a control goes: its page decides which rows exist, and what stands above it decides the row.
[[nodiscard]] std::optional<Placement> PlacementFor(Kind kind, std::size_t index, const Metrics& m) noexcept
{
    const auto pages = std::views::iota(std::size_t{ 0 }, static_cast<std::size_t>(Page::Count));
    const auto first = [&](const std::optional<Placement>& so, std::size_t page) { return so.has_value() ? so : PlacementOn(static_cast<Page>(page), kind, index, m); };
    return std::ranges::fold_left(pages, std::optional<Placement>{}, first);
}

[[nodiscard]] Placement PlaceOfRow(Kind kind, std::size_t index, const Metrics& m) noexcept
{
    const std::optional<Placement> at = PlacementFor(kind, index, m);
    ENSURE(at.has_value());
    return *at;
}

// How many slots the tallest page needs down one column, which is how tall the panel is made.
[[nodiscard]] std::size_t SlotsOnPage(Page page, const PanelLists& lists) noexcept
{
    const auto add = [&lists](std::size_t so, const RowSpec& row) { return so + SlotsOf(row, lists); };
    return std::ranges::fold_left(RowsOf(page), std::size_t{ 0 }, add);
}

[[nodiscard]] std::size_t DeepestPage(const PanelLists& lists) noexcept
{
    const auto pages = std::views::iota(std::size_t{ 0 }, static_cast<std::size_t>(Page::Count));
    const auto deeper = [&lists](std::size_t so, std::size_t page) { return std::max(so, SlotsOnPage(static_cast<Page>(page), lists)); };
    return std::ranges::fold_left(pages, std::size_t{ 0 }, deeper);
}

// A row cannot straddle a column, so a column has to be a little taller than an even share of the slots.
[[nodiscard]] int RowsPerColumn(const PanelLists& lists) noexcept
{
    const std::size_t share = (DeepestPage(lists) + kColumns) / static_cast<std::size_t>(kColumns);
    return std::clamp(static_cast<int>(share), kMinRowsPerColumn, kMaxRowsPerColumn);
}

[[nodiscard]] Built BuildFields(HWND parent, const Metrics& m, const std::array<float, kFieldCount>& values, Built built) noexcept
{
    const auto steps = [&values](std::size_t f) { return StepsOf(values[f], kFields[f]); };
    built.labels = infra::Generated<HWND, kFieldCount>(
        [&](std::size_t f) { return CreateLabel(parent, m, kFields[f].label, PlaceOfRow(Kind::Field, f, m).left, PlaceOfRow(Kind::Field, f, m).top, kColumnWidth); });
    built.sliders = infra::Generated<HWND, kFieldCount>([&](std::size_t f) { return CreateSlider(parent, m, kFields[f], PlaceOfRow(Kind::Field, f, m), steps(f)); });
    built.boxes = infra::Generated<HWND, kFieldCount>([&](std::size_t f) {
        const Placement at = PlaceOfRow(Kind::Field, f, m);
        return CreateChild(parent, WC_EDITW, L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, Bounds(m, at.left + kBoxOffset, at.control, kBoxWidth, m.ControlHeight()));
    });
    built.spins = infra::Generated<HWND, kFieldCount>([&](std::size_t f) { return CreateSpin(parent, built.boxes[f], kFields[f], steps(f)); });
    built.resets = infra::Generated<HWND, kFieldCount>([&](std::size_t f) {
        const Placement at = PlaceOfRow(Kind::Field, f, m);
        return CreateButton(parent, m, kRefreshGlyph, 0, at.left + kResetOffset, at.control, kResetWidth);
    });
    return built;
}

[[nodiscard]] HWND CreateToggle(HWND parent, const Metrics& m, std::size_t toggle, bool on) noexcept
{
    const Placement at = PlaceOfRow(Kind::Toggle, toggle, m);
    const HWND check = CreateButton(parent, m, kToggles[toggle].label, BS_AUTOCHECKBOX, at.left, at.control, kColumnWidth - kResetWidth - kMargin);
    if (check != nullptr)
        SetChecked(check, on);
    return check;
}

// A switch is its own answer, so only one whose default is not obvious from looking at it gets a reset.
[[nodiscard]] HWND CreateToggleReset(HWND parent, const Metrics& m, std::size_t toggle) noexcept
{
    if (!kToggles[toggle].resettable)
        return nullptr;
    const Placement at = PlaceOfRow(Kind::Toggle, toggle, m);
    return CreateButton(parent, m, kRefreshGlyph, 0, at.left + kResetOffset, at.control, kResetWidth);
}

[[nodiscard]] Built BuildToggles(HWND parent, const Metrics& m, const std::array<bool, kToggleCount>& on, Built built) noexcept
{
    built.toggles = infra::Generated<HWND, kToggleCount>([&](std::size_t t) { return CreateToggle(parent, m, t, on[t]); });
    built.toggleResets = infra::Generated<HWND, kToggleCount>([&](std::size_t t) { return CreateToggleReset(parent, m, t); });
    return built;
}

[[nodiscard]] HWND Chosen(HWND choice, bool chosen) noexcept
{
    if (choice != nullptr)
        SetChecked(choice, chosen);
    return choice;
}

[[nodiscard]] std::array<HWND, kMaxChoices> CreateChoices(HWND parent, const Metrics& m, std::size_t group, std::size_t chosen) noexcept
{
    const Placement at = PlaceOfRow(Kind::Group, group, m);
    return infra::Generated<HWND, kMaxChoices>([&](std::size_t i) -> HWND {
        if (i >= kGroups[group].count)
            return nullptr;
        const DWORD style = BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0u);
        const HWND choice = CreateButton(parent, m, kGroups[group].choices[i], style, at.left + static_cast<int>(i) * kChoiceWidth, at.control, kChoiceWidth);
        if (choice != nullptr)
            SetChecked(choice, i == chosen);
        return choice;
    });
}

[[nodiscard]] Built BuildGroups(HWND parent, const Metrics& m, const std::array<std::size_t, kGroupCount>& chosen, Built built) noexcept
{
    built.groupLabels = infra::Generated<HWND, kGroupCount>(
        [&](std::size_t g) { return CreateLabel(parent, m, kGroups[g].label, PlaceOfRow(Kind::Group, g, m).left, PlaceOfRow(Kind::Group, g, m).top, kColumnWidth); });
    built.choices = infra::Generated<std::array<HWND, kMaxChoices>, kGroupCount>([&](std::size_t g) { return CreateChoices(parent, m, g, chosen[g]); });
    return built;
}

[[nodiscard]] const wchar_t* StartingText(const interior::Options& o, std::size_t text) noexcept
{
    return text == static_cast<std::size_t>(Text::Window) ? o.window.CString() : o.logFile.CString();
}

[[nodiscard]] HWND CreateTextBox(HWND parent, const Metrics& m, std::size_t text, const wchar_t* value) noexcept
{
    const Placement at = PlaceOfRow(Kind::Text, text, m);
    const HWND box = CreateChild(parent, WC_EDITW, value, WS_TABSTOP | ES_AUTOHSCROLL, WS_EX_CLIENTEDGE, Bounds(m, at.left, at.control, kColumnWidth - kMargin, m.ControlHeight()));
    if (box != nullptr)
        (void)::SendMessageW(box, EM_SETLIMITTEXT, static_cast<WPARAM>(kPathCapacity - 1), 0);
    return box;
}

// The choices of a runtime list run two to a line, wrapping down the row as far as the list is long.
[[nodiscard]] Placement PlaceOfChoice(const Placement& row, std::size_t index, const Metrics& m) noexcept
{
    const int line = static_cast<int>(index / kListPerLine);
    const int across = static_cast<int>(index % kListPerLine);
    return Placement{ row.left + across * kListChoiceWidth, row.top, row.control + line * m.ControlHeight() };
}

[[nodiscard]] HWND NamedChoice(HWND parent, const Metrics& m, std::size_t list, std::size_t index) noexcept
{
    const PanelList& entries = (*m.lists)[list];
    const Placement at = PlaceOfChoice(PlaceOfRow(Kind::List, list, m), index, m);
    const HWND choice = CreateButton(parent, m, entries.choices.At(index).CString(), BS_AUTORADIOBUTTON | (index == 0 ? WS_GROUP : 0u), at.left, at.control, kListChoiceWidth);
    return Chosen(choice, index == entries.chosen);
}

[[nodiscard]] HWND CreateListChoice(HWND parent, const Metrics& m, std::size_t list, std::size_t index) noexcept
{
    if (index >= (*m.lists)[list].choices.Size())
        return nullptr;
    return NamedChoice(parent, m, list, index);
}

[[nodiscard]] HWND CreateListLabel(HWND parent, const Metrics& m, std::size_t list) noexcept
{
    if ((*m.lists)[list].choices.IsEmpty())
        return nullptr;
    const Placement at = PlaceOfRow(Kind::List, list, m);
    return CreateLabel(parent, m, kLists[list].label, at.left, at.top, kColumnWidth);
}

// WAIVER(R7): a label and a row of buttons is what both a fixed group and a runtime list look like; which
// table the names come from, and whether a row exists at all, is what differs.
[[nodiscard]] Built BuildLists(HWND parent, const Metrics& m, Built built) noexcept
{
    built.listLabels = infra::Generated<HWND, kListCount>([&](std::size_t l) { return CreateListLabel(parent, m, l); });
    built.listChoices = infra::Generated<std::array<HWND, kMaxListChoices>, kListCount>(
        [&](std::size_t l) { return infra::Generated<HWND, kMaxListChoices>([&](std::size_t i) { return CreateListChoice(parent, m, l, i); }); });
    return built;
}

// WAIVER(R7): the label and the control of a row are built the same way whatever the row holds; what each
// of these makes, and from which table, is what differs.
[[nodiscard]] Built BuildTexts(HWND parent, const Metrics& m, const interior::Options& o, Built built) noexcept
{
    built.textLabels =
        infra::Generated<HWND, kTextCount>([&](std::size_t t) { return CreateLabel(parent, m, kTexts[t].label, PlaceOfRow(Kind::Text, t, m).left, PlaceOfRow(Kind::Text, t, m).top, kColumnWidth); });
    built.texts = infra::Generated<HWND, kTextCount>([&](std::size_t t) { return CreateTextBox(parent, m, t, StartingText(o, t)); });
    return built;
}

[[nodiscard]] Built BuildAll(HWND parent, const Metrics& m, const interior::Options& o, const interior::LiveSettings& live, interior::DisplayMode display) noexcept
{
    const Built numbers = BuildToggles(parent, m, StartingToggles(o, live), BuildFields(parent, m, StartingValues(o, live), Built{}));
    return BuildLists(parent, m, BuildTexts(parent, m, o, BuildGroups(parent, m, StartingChoices(o, display), numbers)));
}

// --- the pages -------------------------------------------------------------------------------------------

[[nodiscard]] HWND CreateTabs(HWND parent, const Metrics& m) noexcept
{
    return CreateChild(parent, WC_TABCONTROLW, nullptr, TCS_TABS, 0, Bounds(m, kMargin, m.Of(kMargin), kPanelWidth - 2 * kMargin, m.Of(kTabHeight)));
}

void AddTab(HWND tabs, std::size_t index, const wchar_t* title) noexcept
{
    TCITEMW item{ TCIF_TEXT, 0, 0, const_cast<wchar_t*>(title), 0, 0, 0 };
    (void)::SendMessageW(tabs, TCM_INSERTITEMW, index, reinterpret_cast<LPARAM>(&item));
}

void AddTabs(HWND tabs) noexcept
{
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, static_cast<std::size_t>(Page::Count)), [tabs](std::size_t i) { AddTab(tabs, i, kPages[i].title); });
}

[[nodiscard]] Page ChosenPage(const ControlPanel& panel) noexcept
{
    const int selected = static_cast<int>(::SendMessageW(panel.tabs, TCM_GETCURSEL, 0, 0));
    return static_cast<Page>(std::clamp<std::size_t>(static_cast<std::size_t>(std::max(selected, 0)), 0, static_cast<std::size_t>(Page::Count) - 1));
}

void ShowGroup(const ControlPanel& panel, std::size_t group, int how) noexcept;

void ShowAll(std::span<const HWND> controls, int how) noexcept
{
    std::ranges::for_each(controls, [how](HWND control) { (void)::ShowWindow(control, how); });
}

[[nodiscard]] std::array<HWND, 5> ControlsOfField(const ControlPanel& panel, std::size_t f) noexcept
{
    return { panel.labels[f], panel.sliders[f], panel.boxes[f], panel.spins[f], panel.resets[f] };
}

[[nodiscard]] std::array<HWND, 2> ControlsOfToggle(const ControlPanel& panel, std::size_t t) noexcept
{
    return { panel.toggles[t], panel.toggleResets[t] };
}

[[nodiscard]] int HowOf(bool visible) noexcept
{
    return visible ? SW_SHOW : SW_HIDE;
}

[[nodiscard]] std::array<HWND, 2> ControlsOfText(const ControlPanel& panel, std::size_t t) noexcept
{
    return { panel.textLabels[t], panel.texts[t] };
}

void ShowNumberOrSwitch(const ControlPanel& panel, const RowSpec& row, int how) noexcept
{
    if (row.kind == Kind::Field)
        ShowAll(ControlsOfField(panel, row.index), how);
    else
        ShowAll(ControlsOfToggle(panel, row.index), how);
}

void ShowList(const ControlPanel& panel, std::size_t list, int how) noexcept
{
    (void)::ShowWindow(panel.listLabels[list], how);
    ShowAll(ChoicesOfList(panel, static_cast<List>(list)), how);
}

void ShowGroupOrText(const ControlPanel& panel, const RowSpec& row, int how) noexcept
{
    if (row.kind == Kind::Group)
        ShowGroup(panel, row.index, how);
    else
        ShowAll(ControlsOfText(panel, row.index), how);
}

// A number and a switch each stand on a row of their own; a group and a box are laid out differently.
[[nodiscard]] bool StandsAlone(Kind kind) noexcept
{
    return kind == Kind::Field || kind == Kind::Toggle;
}

void ShowChoicesOrText(const ControlPanel& panel, const RowSpec& row, int how) noexcept
{
    if (row.kind == Kind::List)
        ShowList(panel, row.index, how);
    else
        ShowGroupOrText(panel, row, how);
}

void ShowRow(const ControlPanel& panel, const RowSpec& row, bool visible) noexcept
{
    if (StandsAlone(row.kind))
        ShowNumberOrSwitch(panel, row, HowOf(visible));
    else
        ShowChoicesOrText(panel, row, HowOf(visible));
}

void ShowGroup(const ControlPanel& panel, std::size_t group, int how) noexcept
{
    (void)::ShowWindow(panel.groupLabels[group], how);
    ShowAll(ChoicesOf(panel, static_cast<Group>(group)), how);
}

void ShowPage(const ControlPanel& panel, Page page, bool visible) noexcept
{
    std::ranges::for_each(RowsOf(page), [&panel, visible](const RowSpec& row) { ShowRow(panel, row, visible); });
}

// The first control of a row, which stands for the page the row is on. Each kind is indexed only by a row
// of its own kind, so the index is always in range for the array it picks.
[[nodiscard]] HWND MarkerOf(const ControlPanel& panel, const RowSpec& row) noexcept
{
    const std::array<std::span<const HWND>, 5> byKind{ panel.labels, panel.toggles, panel.groupLabels, panel.textLabels, panel.listLabels };
    return byKind[static_cast<std::size_t>(row.kind)][row.index];
}

// A row whose list is empty has no controls, so the page is marked by the first row that does have one.
[[nodiscard]] bool CarriesControls(const ControlPanel& panel, const RowSpec& row) noexcept
{
    return MarkerOf(panel, row) != nullptr;
}

[[nodiscard]] HWND PageMarker(const ControlPanel& panel, Page page) noexcept
{
    const std::span<const RowSpec> rows = RowsOf(page);
    const auto found = std::ranges::find_if(rows, [&panel](const RowSpec& row) { return CarriesControls(panel, row); });
    ENSURE(found != rows.end());
    return MarkerOf(panel, *found);
}

// The chosen page is shown and the others hidden, but only when the choice has moved, so the panel is not
// asked to redraw itself on every frame. Whether it has moved is read from the page's own first control.
[[nodiscard]] bool PageAlreadyShown(const ControlPanel& panel, Page chosen) noexcept
{
    return ::IsWindowVisible(PageMarker(panel, chosen)) != FALSE;
}

void ShowOnly(const ControlPanel& panel, Page chosen) noexcept
{
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, static_cast<std::size_t>(Page::Count)),
                          [&panel, chosen](std::size_t p) { ShowPage(panel, static_cast<Page>(p), static_cast<Page>(p) == chosen); });
    (void)::ShowWindow(panel.restart, chosen == Page::Startup ? SW_SHOW : SW_HIDE);
}

void ShowChosenPage(const ControlPanel& panel) noexcept
{
    const Page chosen = ChosenPage(panel);
    if (PageAlreadyShown(panel, chosen))
        return;
    ShowOnly(panel, chosen);
}

// --- turning the controls back into settings -------------------------------------------------------------

[[nodiscard]] interior::NrStyle StyleFrom(std::size_t code) noexcept
{
    constexpr std::array<interior::NrStyle, 3> styles{ interior::NrStyle::Standard, interior::NrStyle::Natural, interior::NrStyle::Cinematic };
    return styles[std::min(code, styles.size() - 1)];
}

[[nodiscard]] interior::DisplayMode DisplayFrom(std::size_t index) noexcept
{
    constexpr std::array<interior::DisplayMode, 3> modes{ interior::DisplayMode::Processed, interior::DisplayMode::Original, interior::DisplayMode::Split };
    return modes[std::min(index, modes.size() - 1)];
}

[[nodiscard]] interior::CursorMode CursorFrom(std::size_t index) noexcept
{
    constexpr std::array<interior::CursorMode, 3> modes{ interior::CursorMode::Auto, interior::CursorMode::On, interior::CursorMode::Off };
    return modes[std::min(index, modes.size() - 1)];
}

[[nodiscard]] interior::LogLevel LogLevelFrom(std::size_t index) noexcept
{
    constexpr std::array<interior::LogLevel, 4> levels{ interior::LogLevel::Debug, interior::LogLevel::Info, interior::LogLevel::Warn, interior::LogLevel::Error };
    return levels[std::min(index, levels.size() - 1)];
}

// -1 is the model's own way of saying "whatever local structure got", and nothing between it and 0 means
// anything, so the switch carries that value and the number carries the rest.
[[nodiscard]] interior::SkinStrength SkinOf(const ControlPanel& panel, interior::SkinStrength held) noexcept
{
    if (IsOn(panel, Toggle::SkinFollowsStructure))
        return interior::SkinStrengthTag::Parse(-1.0f).value_or(held);
    return interior::SkinStrengthTag::Parse(SettledValue(panel, Field::Skin)).value_or(held);
}

// The model names its own presets or none at all; with none the row is absent and the session keeps what
// it started with.
[[nodiscard]] interior::NgxPreset PresetOf(const ControlPanel& panel, interior::NgxPreset held) noexcept
{
    const std::size_t chosen = ChosenInList(panel, List::Preset, held.Get());
    return interior::NgxPresetTag::Parse(static_cast<std::uint32_t>(chosen)).value_or(held);
}

[[nodiscard]] interior::NrTuning TuningOf(const ControlPanel& panel, const interior::NrTuning& current) noexcept
{
    const auto strength = [&panel](Field field, interior::Strength held) { return interior::StrengthTag::Parse(SettledValue(panel, field)).value_or(held); };
    const interior::NrIntensity intensity = interior::NrIntensityTag::Parse(SettledValue(panel, Field::Intensity)).value_or(current.intensity);
    return interior::NrTuning{ PresetOf(panel, current.preset),
                               intensity,
                               StyleFrom(ChosenIn(panel, Group::Style, interior::StyleCode(current.style))),
                               strength(Field::LocalStructure, current.localStructure),
                               strength(Field::LocalTone, current.localTone),
                               SkinOf(panel, current.skinStructure),
                               IsOn(panel, Toggle::AutoMask),
                               IsOn(panel, Toggle::UiCorrection) };
}

[[nodiscard]] interior::LiveSettings LiveOf(const ControlPanel& panel, const interior::LiveSettings& current) noexcept
{
    const auto scale = [&panel](Field field, interior::MotionScale held) { return interior::MotionScaleTag::Parse(SettledValue(panel, field)).value_or(held); };
    return interior::LiveSettings{ IsOn(panel, Toggle::NeuralRendering),
                                   TuningOf(panel, current.tuning),
                                   IsOn(panel, Toggle::DepthInverted),
                                   scale(Field::MvScaleX, current.mvScaleX),
                                   scale(Field::MvScaleY, current.mvScaleY),
                                   IsOn(panel, Toggle::Vsync),
                                   interior::FractionTag::Parse(SettledValue(panel, Field::ResetThreshold)).value_or(current.resetThreshold),
                                   interior::DepthValueTag::Parse(SettledValue(panel, Field::DepthValue)).value_or(current.depth) };
}

[[nodiscard]] interior::SurfaceSettings SurfaceOf(const ControlPanel& panel) noexcept
{
    return interior::SurfaceSettings{ CursorFrom(ChosenIn(panel, Group::Cursor, 0)),    IsOn(panel, Toggle::CaptureBorder), panel.displayAffinity, IsOn(panel, Toggle::Topmost), panel.clickThrough,
                                      LogLevelFrom(ChosenIn(panel, Group::LogLevel, 1)) };
}

// --- the resets --------------------------------------------------------------------------------------

void ResetField(const ControlPanel& panel, std::size_t f, float value) noexcept
{
    if (IsPushed(panel.resets[f]))
        Commit(panel, f, StepsOf(value, kFields[f]));
}

void ResetToggle(const ControlPanel& panel, std::size_t t, bool on) noexcept
{
    if (IsPushed(panel.toggleResets[t]))
        SetChecked(panel.toggles[t], on);
}

void EnableAll(std::span<const HWND> controls, bool enabled) noexcept
{
    std::ranges::for_each(controls, [enabled](HWND control) { (void)::EnableWindow(control, enabled ? TRUE : FALSE); });
}

// The expander is a check box that looks like a button, so it keeps its own state and the panel reads it.
// Opening it grows the window, and nothing moves because the notice sits under everything else.
[[nodiscard]] bool IsNoticeOpen(const ControlPanel& panel) noexcept
{
    return IsChecked(panel.expander);
}

void ResizeForNotice(const ControlPanel& panel, bool open) noexcept
{
    RECT frame{};
    ENSURE(::GetWindowRect(panel.window.get(), &frame) != FALSE);
    const int height = open ? panel.tallHeight : panel.shortHeight;
    ENSURE(::SetWindowPos(panel.window.get(), nullptr, 0, 0, frame.right - frame.left, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
}

void ApplyNotice(const ControlPanel& panel) noexcept
{
    const bool open = IsNoticeOpen(panel);
    if (open == (::IsWindowVisible(panel.noticeBody) != FALSE))
        return;
    (void)::ShowWindow(panel.noticeBody, HowOf(open));
    ResizeForNotice(panel, open);
}

void ApplyEnables(const ControlPanel& panel) noexcept
{
    EnableAll(ControlsOfField(panel, static_cast<std::size_t>(Field::Skin)), !IsOn(panel, Toggle::SkinFollowsStructure));
    EnableAll(ChoicesOf(panel, Group::Sr), panel.superResolution);
    EnableAll(ControlsOfField(panel, static_cast<std::size_t>(Field::SrPreset)), panel.superResolution);
}

// A held reset puts its own control back where the defaults start it; the same answer every frame.
void ApplyResets(const ControlPanel& panel) noexcept
{
    const interior::Options d = interior::DefaultOptions();
    const std::array<float, kFieldCount> values = StartingValues(d, interior::DefaultLive(d));
    const std::array<bool, kToggleCount> on = StartingToggles(d, interior::DefaultLive(d));
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&](std::size_t f) { ResetField(panel, f, values[f]); });
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kToggleCount), [&](std::size_t t) { ResetToggle(panel, t, on[t]); });
}

// --- the session the start-up page describes ---------------------------------------------------------

constexpr std::size_t kArgumentCapacity = 1200;
constexpr std::size_t kPieceCapacity = 64;
using Arguments = infra::BoundedString<char, kArgumentCapacity>;
using Piece = infra::BoundedString<char, kPieceCapacity>;

[[nodiscard]] Piece Trimmed(std::string_view text) noexcept
{
    return Piece::Parse(text).value_or(Piece{});
}

[[nodiscard]] Arguments Joined(const Arguments& so, const Piece& next) noexcept
{
    if (next.IsEmpty())
        return so;
    return Arguments::Parse(infra::Formatted<kArgumentCapacity>("{} {}", so.Get(), next.Get()).Get()).value_or(so);
}

// Each page's arguments are a list of pieces folded onto what came before, so a builder is one list.
[[nodiscard]] Arguments JoinedAll(const Arguments& so, std::span<const Piece> pieces) noexcept
{
    return std::ranges::fold_left(pieces, so, Joined);
}

[[nodiscard]] int WholeOf(const ControlPanel& panel, Field field) noexcept
{
    return static_cast<int>(std::lround(SettledValue(panel, field)));
}

[[nodiscard]] Piece Choice(const ControlPanel& panel, Group group, std::string_view name, std::span<const char* const> words) noexcept
{
    return Trimmed(infra::Formatted<kPieceCapacity>("--{}={}", name, words[std::min(ChosenIn(panel, group, 0), words.size() - 1)]).Get());
}

[[nodiscard]] Piece Whole(const ControlPanel& panel, Field field, std::string_view name) noexcept
{
    return Trimmed(infra::Formatted<kPieceCapacity>("--{}={}", name, WholeOf(panel, field)).Get());
}

[[nodiscard]] std::string_view Word(bool on) noexcept
{
    return on ? "on" : "off";
}

[[nodiscard]] Piece Switch(const ControlPanel& panel, Toggle toggle, std::string_view name) noexcept
{
    return Trimmed(infra::Formatted<kPieceCapacity>("--{}={}", name, Word(IsOn(panel, toggle))).Get());
}

[[nodiscard]] Piece Decimal(std::string_view name, float value) noexcept
{
    return Trimmed(infra::Formatted<kPieceCapacity>("--{}={:.3f}", name, static_cast<double>(value)).Get());
}

[[nodiscard]] Piece Counted(std::string_view name, std::uint32_t value) noexcept
{
    return Trimmed(infra::Formatted<kPieceCapacity>("--{}={}", name, value).Get());
}

// The first entries of the source list are the two answers that name no monitor; the rest are the monitors
// in the order the session found them.
[[nodiscard]] Piece SourcePiece(const ControlPanel& panel) noexcept
{
    constexpr std::array<const char*, 2> kinds{ "primary", "all" };
    const std::size_t chosen = ChosenInList(panel, List::Source, 0);
    if (chosen < kinds.size())
        return Trimmed(infra::Formatted<kPieceCapacity>("--monitor={}", kinds[chosen]).Get());
    return Counted("monitor", static_cast<std::uint32_t>(chosen - kinds.size()));
}

// Leaving the first entry chosen says nothing, which is what "no target of its own" and "whichever adapter
// the search finds" mean on the command line.
[[nodiscard]] Piece FromListPiece(const ControlPanel& panel, List list, std::string_view name) noexcept
{
    const std::size_t chosen = ChosenInList(panel, list, 0);
    if (chosen == 0)
        return Piece{};
    return Counted(name, static_cast<std::uint32_t>(chosen - 1));
}

[[nodiscard]] Arguments StartupArguments(const ControlPanel& panel, const Arguments& so) noexcept
{
    constexpr std::array<const char*, 2> formats{ "rgba8", "rgba16f" };
    constexpr std::array<const char*, 3> sr{ "auto", "dlaa", "off" };
    constexpr std::array<const char*, 3> motion{ "builtin", "nvof", "none" };
    const std::array<Piece, 8> pieces{ SourcePiece(panel),
                                       FromListPiece(panel, List::Target, "target"),
                                       Choice(panel, Group::Format, "format", formats),
                                       Choice(panel, Group::Sr, "sr", sr),
                                       Whole(panel, Field::SrPreset, "sr-preset"),
                                       Choice(panel, Group::Motion, "mv", motion),
                                       Whole(panel, Field::MvLevel, "mv-level"),
                                       FromListPiece(panel, List::Adapter, "adapter") };
    return JoinedAll(so, pieces);
}

[[nodiscard]] Arguments FlowArguments(const ControlPanel& panel, const Arguments& so) noexcept
{
    constexpr std::array<const char*, 3> grids{ "1", "2", "4" };
    constexpr std::array<const char*, 3> effort{ "slow", "medium", "fast" };
    constexpr std::array<const char*, 3> console{ "auto", "on", "off" };
    const std::array<Piece, 7> pieces{ Choice(panel, Group::NvofGrid, "nvof-grid", grids), Choice(panel, Group::NvofPerf, "nvof-perf", effort),
                                       Choice(panel, Group::Console, "console", console),  Switch(panel, Toggle::RedirectionBitmap, "redirection-bitmap"),
                                       Switch(panel, Toggle::DebugLayer, "debug-layer"),   Switch(panel, Toggle::Indicator, "indicator"),
                                       Switch(panel, Toggle::CubinCache, "cubin-cache") };
    return JoinedAll(so, pieces);
}

// The live settings travel with the new session too, so it starts where this one left off.
[[nodiscard]] Arguments ModelArguments(const ControlPanel& panel, const Arguments& so) noexcept
{
    const interior::LiveSettings live = LiveOf(panel, interior::DefaultLive(interior::DefaultOptions()));
    const std::array<Piece, 8> pieces{ Switch(panel, Toggle::NeuralRendering, "nr"),
                                       Counted("nr-preset", live.tuning.preset.Get()),
                                       Decimal("nr-intensity", live.tuning.intensity.Get()),
                                       Decimal("nr-local-structure", live.tuning.localStructure.Get()),
                                       Decimal("nr-local-tone", live.tuning.localTone.Get()),
                                       Decimal("nr-skin", live.tuning.skinStructure.Get()),
                                       Counted("nr-style", interior::StyleCode(live.tuning.style)),
                                       Switch(panel, Toggle::AutoMask, "nr-automask") };
    return JoinedAll(so, pieces);
}

[[nodiscard]] Arguments SurfaceArguments(const ControlPanel& panel, const Arguments& so) noexcept
{
    constexpr std::array<const char*, 3> cursor{ "auto", "on", "off" };
    constexpr std::array<const char*, 4> levels{ "0", "1", "2", "3" };
    constexpr std::array<const char*, 3> compare{ "off", "original", "split" };
    const std::array<Piece, 11> pieces{ Switch(panel, Toggle::UiCorrection, "nr-ui-correction"),
                                        Switch(panel, Toggle::Vsync, "vsync"),
                                        Decimal("depth-value", SettledValue(panel, Field::DepthValue)),
                                        Switch(panel, Toggle::DepthInverted, "depth-inverted"),
                                        Decimal("mv-scale-x", SettledValue(panel, Field::MvScaleX)),
                                        Decimal("mv-scale-y", SettledValue(panel, Field::MvScaleY)),
                                        Decimal("reset-threshold", SettledValue(panel, Field::ResetThreshold)),
                                        Choice(panel, Group::Compare, "compare", compare),
                                        Choice(panel, Group::Cursor, "cursor", cursor),
                                        Choice(panel, Group::LogLevel, "log-level", levels),
                                        Switch(panel, Toggle::CaptureBorder, "capture-border") };
    return JoinedAll(so, pieces);
}

[[nodiscard]] Arguments WindowArguments(const ControlPanel& panel, const Arguments& so) noexcept
{
    const std::array<Piece, 3> pieces{ Trimmed(infra::Formatted<kPieceCapacity>("--affinity={}", Word(panel.displayAffinity)).Get()), Switch(panel, Toggle::Topmost, "topmost"),
                                       Trimmed(infra::Formatted<kPieceCapacity>("--click-through={}", Word(panel.clickThrough)).Get()) };
    return JoinedAll(so, pieces);
}

[[nodiscard]] Piece AppIdPiece(const std::optional<interior::NgxAppId>& id) noexcept
{
    if (!id.has_value())
        return Piece{};
    return Trimmed(infra::Formatted<kPieceCapacity>("--ngx-app-id={:x}", id->Get()).Get());
}

// What the panel has no control for still travels to the new session, so one started with an application
// id or a project id of its own keeps them rather than falling back to the defaults.
[[nodiscard]] Arguments RuntimeArguments(const interior::Options& o, const Arguments& so) noexcept
{
    constexpr std::array<const char*, 3> ngxLog{ "0", "1", "2" };
    const std::array<Piece, 3> pieces{ Trimmed(infra::Formatted<kPieceCapacity>("--ngx-log={}", ngxLog[static_cast<std::size_t>(o.ngxLogLevel)]).Get()),
                                       Trimmed(infra::Formatted<kPieceCapacity>("--ngx-project-id={}", o.ngxProjectId.Get()).Get()), AppIdPiece(o.ngxAppId) };
    return JoinedAll(so, pieces);
}

// The command line is ASCII, so widening it is a character-for-character copy.
[[nodiscard]] std::array<wchar_t, kArgumentCapacity + 1> WidenedLine(std::string_view text) noexcept
{
    std::array<wchar_t, kArgumentCapacity + 1> wide{}; // WAIVER(R2): a local buffer filled once, before use.
    std::ranges::copy(text | std::views::take(wide.size() - 1) | std::views::transform([](char c) { return static_cast<wchar_t>(c); }), wide.begin());
    return wide;
}

// --- putting the panel together ------------------------------------------------------------------------

constexpr DWORD kPanelStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

[[nodiscard]] Result<UniqueWindow, Error> CreatePanelWindow() noexcept
{
    HWND window = ::CreateWindowExW(WS_EX_TOPMOST, kPanelClass, L"DlssScreen controls", kPanelStyle, CW_USEDEFAULT, CW_USEDEFAULT, kPanelWidth, kPanelWidth, nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr)
        return Fail(LastError(ApiCall::CreateWindowExW));
    return UniqueWindow(window);
}

[[nodiscard]] Metrics MetricsOf(HWND window, HFONT font, const PanelLists& lists) noexcept
{
    return Metrics{ static_cast<int>(::GetDpiForWindow(window)), LineHeight(window, font), RowsPerColumn(lists), &lists };
}

// What the window measures on the outside for a page of a given height on the inside.
[[nodiscard]] int OuterHeight(const Metrics& m, int inner) noexcept
{
    RECT frame{ 0, 0, m.Of(kPanelWidth), inner };
    ENSURE(::AdjustWindowRectExForDpi(&frame, kPanelStyle, FALSE, 0, static_cast<UINT>(m.dpi)) != FALSE);
    return frame.bottom - frame.top;
}

void ResizeToFit(HWND window, const Metrics& m) noexcept
{
    RECT frame{ 0, 0, m.Of(kPanelWidth), m.PageTop() + m.PageHeight() };
    ENSURE(::AdjustWindowRectExForDpi(&frame, kPanelStyle, FALSE, 0, static_cast<UINT>(m.dpi)) != FALSE);
    ENSURE(::SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
}

// The notice sits under everything, so opening it grows the window rather than moving the controls.
struct Notice
{
    HWND line;
    HWND expander;
    HWND body;
};

[[nodiscard]] HWND CreateNoticeBody(HWND parent, const Metrics& m) noexcept
{
    const int top = m.NoticeTop() + m.ControlHeight();
    const HWND body = CreateChild(parent, WC_STATICW, kNoticeBody, SS_LEFT, 0, Bounds(m, kMargin, top, kPanelWidth - 2 * kMargin, m.BodyHeight()));
    if (body != nullptr)
        (void)::ShowWindow(body, SW_HIDE);
    return body;
}

[[nodiscard]] Notice CreateNotice(HWND parent, const Metrics& m) noexcept
{
    const int width = kPanelWidth - 2 * kMargin - kExpanderWidth;
    const HWND line = CreateChild(parent, WC_STATICW, kNoticeLine, SS_LEFTNOWORDWRAP, 0, Bounds(m, kMargin, m.NoticeTop(), width, m.ControlHeight()));
    const HWND expander = CreateChild(parent, WC_BUTTONW, kChevronGlyph, BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, Bounds(m, kMargin + width, m.NoticeTop(), kExpanderWidth, m.ControlHeight()));
    return Notice{ line, expander, CreateNoticeBody(parent, m) };
}

[[nodiscard]] std::array<std::size_t, kListCount> CountsOf(const PanelLists& lists) noexcept
{
    return infra::Generated<std::size_t, kListCount>([&lists](std::size_t l) { return lists[l].choices.Size(); });
}

[[nodiscard]] ControlPanel Assembled(UniqueWindow window, UniqueFont font, const Metrics& m, const interior::Options& o, const interior::LiveSettings& live, interior::DisplayMode display,
                                     const PanelFindings& findings) noexcept
{
    HWND parent = window.get();
    const HWND tabs = CreateTabs(parent, m);
    const Built built = BuildAll(parent, m, o, live, display);
    const Notice notice = CreateNotice(parent, m);
    return ControlPanel{ std::move(window),
                         std::move(font),
                         IconFont(m.dpi),
                         BoldFont(m.dpi),
                         tabs,
                         tabs == nullptr ? nullptr : CreateTooltip(parent),
                         CreateButton(parent, m, L"Start a new session with these", 0, kMargin, m.ButtonTop(), kColumnWidth),
                         built.labels,
                         built.sliders,
                         built.boxes,
                         built.spins,
                         built.resets,
                         built.toggles,
                         built.toggleResets,
                         built.groupLabels,
                         built.choices,
                         built.textLabels,
                         built.texts,
                         built.listLabels,
                         built.listChoices,
                         CountsOf(*m.lists),
                         o.displayAffinity,
                         o.clickThrough,
                         findings.superResolution,
                         notice.line,
                         notice.expander,
                         notice.body,
                         OuterHeight(m, m.PageTop() + m.PageHeight()),
                         OuterHeight(m, m.PageTop() + m.PageHeight() + m.BodyHeight()) };
}

[[nodiscard]] bool IsPresent(HWND control) noexcept
{
    return control != nullptr;
}

[[nodiscard]] bool AllPresent(std::span<const HWND> controls) noexcept
{
    return std::ranges::all_of(controls, IsPresent);
}

[[nodiscard]] std::array<HWND, 4> Furniture(const ControlPanel& panel) noexcept
{
    return { panel.restart, panel.notice, panel.expander, panel.noticeBody };
}

[[nodiscard]] std::array<std::span<const HWND>, 10> GroupsOf(const ControlPanel& panel) noexcept
{
    return { panel.labels, panel.sliders, panel.boxes, panel.spins, panel.resets, panel.toggles, panel.groupLabels, panel.textLabels, panel.texts, Furniture(panel) };
}

// A switch's reset is there exactly when its spec asks for one, so both a missing and a spare one is a fault.
[[nodiscard]] bool ResetAsSpecified(const ControlPanel& panel, std::size_t toggle) noexcept
{
    return kToggles[toggle].resettable == IsPresent(panel.toggleResets[toggle]);
}

[[nodiscard]] bool ResetsAsSpecified(const ControlPanel& panel) noexcept
{
    return std::ranges::all_of(std::views::iota(std::size_t{ 0 }, kToggleCount), [&panel](std::size_t t) { return ResetAsSpecified(panel, t); });
}

[[nodiscard]] bool EveryGroupPresent(const ControlPanel& panel) noexcept
{
    return std::ranges::all_of(GroupsOf(panel), AllPresent) && IsPresent(panel.tabs);
}

[[nodiscard]] bool IsComplete(const ControlPanel& panel) noexcept
{
    return EveryGroupPresent(panel) && ResetsAsSpecified(panel);
}

// A number's hint sits on the slider and on the box, so either one under the pointer explains itself.
void HintNumbers(const ControlPanel& panel, HWND parent) noexcept
{
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel, parent](std::size_t f) { AddHint(panel.tooltip, parent, panel.sliders[f], kFields[f].hint); });
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel, parent](std::size_t f) { AddHint(panel.tooltip, parent, panel.boxes[f], kFields[f].hint); });
}

void HintResets(const ControlPanel& panel, HWND parent) noexcept
{
    std::ranges::for_each(panel.resets, [&panel, parent](HWND button) { AddHint(panel.tooltip, parent, button, kResetHint); });
    std::ranges::for_each(panel.toggleResets, [&panel, parent](HWND button) { AddHint(panel.tooltip, parent, button, kResetHint); });
}

constexpr wchar_t kNoSuperResolution[] = L"Not offered: the driver reports no DLSS Super Resolution, whose model NVIDIA ships separately. The rest of the session runs without it.";

void HintChoices(const ControlPanel& panel, HWND parent) noexcept
{
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kToggleCount), [&panel, parent](std::size_t t) { AddHint(panel.tooltip, parent, panel.toggles[t], kToggles[t].hint); });
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kGroupCount), [&panel, parent](std::size_t g) { AddHint(panel.tooltip, parent, panel.groupLabels[g], kGroups[g].hint); });
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kTextCount), [&panel, parent](std::size_t t) { AddHint(panel.tooltip, parent, panel.texts[t], kTexts[t].hint); });
}

// A greyed control that says nothing is just a control that does not work, so the reason replaces the hint.
void HintMissingSuperResolution(const ControlPanel& panel, HWND parent) noexcept
{
    if (panel.superResolution)
        return;
    AddHint(panel.tooltip, parent, panel.groupLabels[static_cast<std::size_t>(Group::Sr)], kNoSuperResolution);
    AddHint(panel.tooltip, parent, panel.labels[static_cast<std::size_t>(Field::SrPreset)], kNoSuperResolution);
}

void HintRows(const ControlPanel& panel) noexcept
{
    HWND parent = panel.window.get();
    HintNumbers(panel, parent);
    HintChoices(panel, parent);
    HintResets(panel, parent);
    HintMissingSuperResolution(panel, parent);
}

// The message font goes on every child, so the reset buttons take their glyph font afterwards; a hint says
// what the glyph means, since a picture of a circling arrow does not say which value it puts back.
void WearIcon(const ControlPanel& panel, HWND button) noexcept
{
    if (button != nullptr)
        (void)::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(panel.iconFont.get()), TRUE);
}

void IconiseResets(const ControlPanel& panel) noexcept
{
    std::ranges::for_each(panel.resets, [&panel](HWND button) { WearIcon(panel, button); });
    std::ranges::for_each(panel.toggleResets, [&panel](HWND button) { WearIcon(panel, button); });
    WearIcon(panel, panel.expander);
    (void)::SendMessageW(panel.notice, WM_SETFONT, reinterpret_cast<WPARAM>(panel.boldFont.get()), TRUE);
}

void DressPanel(const ControlPanel& panel) noexcept
{
    HFONT font = panel.font.get();
    // WAIVER(R2): walking the panel's own children to give them the system font.
    ::EnumChildWindows(
        panel.window.get(),
        [](HWND child, LPARAM f) -> BOOL {
            (void)::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(f), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font));
    AddTabs(panel.tabs);
    IconiseResets(panel);
    HintRows(panel);
}

// The panel is kept out of the capture, or it would photograph the picture it is controlling.
[[nodiscard]] Result<ControlPanel, Error> Shown(ControlPanel panel) noexcept
{
    if (!IsComplete(panel))
        return Fail(LastError(ApiCall::CreateWindowExW));
    DressPanel(panel);
    return CheckBool(::SetWindowDisplayAffinity(panel.window.get(), WDA_EXCLUDEFROMCAPTURE), ApiCall::SetWindowDisplayAffinity).transform([&panel] {
        ShowOnly(panel, ChosenPage(panel)); // every control is created visible, so the first page is arranged rather than checked

        ::ShowWindow(panel.window.get(), SW_SHOWNOACTIVATE);
        return std::move(panel);
    });
}

[[nodiscard]] Result<ControlPanel, Error> Populated(UniqueWindow window, const interior::Options& o, const interior::LiveSettings& live, interior::DisplayMode display,
                                                    const PanelFindings& findings) noexcept
{
    UniqueFont font = MessageFont(static_cast<int>(::GetDpiForWindow(window.get())));
    const Metrics m = MetricsOf(window.get(), font.get(), findings.lists);
    ResizeToFit(window.get(), m);
    return Shown(Assembled(std::move(window), std::move(font), m, o, live, display, findings));
}

} // namespace

Result<ControlPanel, Error> CreateControlPanel(const interior::Options& options, const interior::LiveSettings& live, interior::DisplayMode display, const PanelFindings& findings) noexcept
{
    InitialiseCommonControls();
    return RegisterWindowClass(ClassDescription()).and_then([&] {
        return CreatePanelWindow().and_then([&](UniqueWindow window) { return Populated(std::move(window), options, live, display, findings); });
    });
}

// What the panel does to itself before it is read: the chosen page, the notice, what is greyed, and any
// reset the operator is holding down.
void Arrange(const ControlPanel& panel) noexcept
{
    ShowChosenPage(panel);
    ApplyNotice(panel);
    ApplyEnables(panel);
    ApplyResets(panel);
}

PanelReading ReadControlPanel(const ControlPanel& panel, const interior::LiveSettings& current) noexcept
{
    Arrange(panel);
    const interior::Fraction split = interior::FractionTag::Parse(SettledValue(panel, Field::Split)).value_or(*kCentre);
    return PanelReading{ LiveOf(panel, current), SurfaceOf(panel), DisplayFrom(ChosenIn(panel, Group::Compare, 0)), split, IsPushed(panel.restart) };
}

// A line too long for the buffer leaves the old one standing, so no session starts from half a path.
[[nodiscard]] interior::CommandLine Extended(const interior::CommandLine& so, const std::array<wchar_t, interior::CommandLine::Capacity + 1>& line, int written) noexcept
{
    if (written <= 0)
        return so;
    return interior::CommandLine::Parse(line.data()).value_or(so);
}

// A path holds whatever the file system allows, so it travels as it was written rather than through the
// ASCII buffer the switches are built in. The quotes keep a path with spaces in one argument.
[[nodiscard]] interior::CommandLine WithPath(const interior::CommandLine& so, const wchar_t* name, std::wstring_view path) noexcept
{
    if (path.empty())
        return so;
    std::array<wchar_t, interior::CommandLine::Capacity + 1> line{}; // WAIVER(R2): a local buffer filled once, before use.
    const int written =
        ::_snwprintf_s(line.data(), line.size(), _TRUNCATE, L"%.*s --%s=\"%.*s\"", static_cast<int>(so.Get().size()), so.Get().data(), name, static_cast<int>(path.size()), path.data());
    return Extended(so, line, written);
}

[[nodiscard]] std::array<wchar_t, kPathCapacity> PathOf(const ControlPanel& panel, Text text) noexcept
{
    std::array<wchar_t, kPathCapacity> typed{}; // WAIVER(R2): a local buffer filled once, before use.
    (void)::GetWindowTextW(panel.texts[static_cast<std::size_t>(text)], typed.data(), kPathCapacity);
    return typed;
}

[[nodiscard]] interior::CommandLine WithPaths(const interior::CommandLine& so, const ControlPanel& panel, const interior::Options& o) noexcept
{
    const std::array<wchar_t, kPathCapacity> logFile = PathOf(panel, Text::LogFile);
    const std::array<wchar_t, kPathCapacity> window = PathOf(panel, Text::Window);
    const interior::CommandLine paths = WithPath(WithPath(so, L"ngx-path", o.ngxPath.Get()), L"app-data", o.appDataPath.Get());
    return WithPath(WithPath(paths, L"log-file", std::wstring_view(logFile.data())), L"window", std::wstring_view(window.data()));
}

interior::CommandLine RestartCommandLine(const ControlPanel& panel, const interior::Options& options) noexcept
{
    const Arguments arguments = RuntimeArguments(options, WindowArguments(panel, SurfaceArguments(panel, ModelArguments(panel, FlowArguments(panel, StartupArguments(panel, Arguments{}))))));
    const interior::CommandLine line = interior::CommandLine::Parse(WidenedLine(arguments.Get()).data()).value_or(interior::CommandLine{});
    return WithPaths(line, panel, options);
}

void ApplyDisplay(const ControlPanel& panel, interior::DisplayMode display) noexcept
{
    ChooseOnly(ChoicesOf(panel, Group::Compare), static_cast<std::size_t>(display));
}

void ApplySplit(const ControlPanel& panel, interior::Fraction split) noexcept
{
    Commit(panel, static_cast<std::size_t>(Field::Split), StepsOf(split.Get(), kFields[static_cast<std::size_t>(Field::Split)]));
}

bool IsPanelClosed(const ControlPanel& panel) noexcept
{
    return ::IsWindowVisible(panel.window.get()) == FALSE;
}

} // namespace real
