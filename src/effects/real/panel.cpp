#include "effects/real/panel.h"

#include "effects/real/window.h"
#include "infrastructure/array_util.h"
#include "infrastructure/text.h"
#include "interior/ngx_params.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <ranges>
#include <span>
#include <string_view>

namespace real {
namespace {

using infra::Fail;
using infra::Result;

constexpr wchar_t kPanelClass[] = L"FullScreenWrapperForDLSS5ControlPanel";
constexpr wchar_t kCrosshairClass[] = L"FullScreenWrapperForDLSS5WindowPicker";
constexpr wchar_t kOutlineClass[] = L"FullScreenWrapperForDLSS5PickOutline";
constexpr int kCrosshairWidth = 34;
constexpr int kReferenceDpi = 96;
// The longest text a control shows is a window's title. A write is skipped only when all of it reads back,
// so a shorter buffer had the picked window's name rewritten, and repainted, every frame.
constexpr int kTextCapacity = static_cast<int>(interior::WindowTitle::Capacity) + 1;
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
constexpr int kWarningWidth = 26; // a glyph in front of a slider, which gives up that much of its length to it
constexpr int kBoxOffset = 214;
constexpr int kBoxWidth = 78;
constexpr int kResetOffset = 312;
constexpr int kResetWidth = 28;
constexpr int kChoiceWidth = 122;
constexpr int kTabHeight = 30;
constexpr int kTipWidth = 360; // a hint wraps at this rather than running on in one line
// A runtime list carries names rather than words, so its buttons are wider and fewer to a line.
constexpr int kListChoiceWidth = 186;
constexpr std::size_t kListPerLine = 2;
constexpr wchar_t kChevronGlyph[] = L"\uE70D";
constexpr wchar_t kNoticeLine[] = L"NOTICE: A game with \"True\" native DLSS5 will likely look different (expand for details)";
constexpr wchar_t kNoticeBody[] =
    L"This app simply takes a flat video stream input and passes it to the model. A native game would provide additional data such as motion data, object depth data, etc.\r\n\r\n"
    L"A game hands the model its own motion vectors, its own depth buffer and the sub-pixel jitter it rendered with, frame by frame, before anything is composited. This app has none of "
    L"that. It captures the finished desktop and makes substitutes: one flat depth plane, and motion guessed by matching blocks between two pictures that have already been drawn, "
    L"resized and blended by the window manager.\r\n\r\n"
    L"So the model here is working from worse inputs than it was built for, on an image that has already lost the information it wants. What it does to the desktop is not what it does in a "
    L"game.\r\n\r\n"
    L"This should be considered an experimental demo, NOT a preview of what it does when it is used properly.";
constexpr int kExpanderWidth = 28;
constexpr int kClassNameCapacity = 16; // long enough to tell a link control's class name from any other

constexpr int kMinRowsPerColumn = 7;
constexpr int kMaxRowsPerColumn = 16;
// The room around the rows, in reference pixels. Each row ends with the gap; a frame's caption band and
// its bottom edge are on top of what the rows in it take.
constexpr int kRowGap = 6;      // under a row's control, before whatever stands next
constexpr int kCaptionGap = 8;  // between a frame's caption and the first row in it
constexpr int kFrameBottom = 4; // between the last row's gap and the frame's bottom edge
constexpr int kFrameGap = 10;   // between a frame's bottom edge and whatever stands under it
constexpr int kFrameInset = 10; // how far the rows inside a frame stand in from its edge, on either side

// How a number is written in its box: as a count, to two decimal places, or as a percentage of one.
enum class Notation : std::uint8_t { Count, Decimal, Percent };

// Each slider counts in steps of a unit: 1 counts whole numbers, 100 counts hundredths. A range says how
// far a slider reaches, not what the model accepts; the command line still takes any finite value.
struct FieldSpec
{
    const wchar_t* label;
    const wchar_t* hint;
    int minimum;
    int maximum; // where the slider ends, which for an open field is only where it ends to begin with
    int steps;
    Notation notation;
    int increment;          // what one click of an arrow moves, in the same steps as the rest
    int ceiling;            // as far as the number may be typed or stepped: the slider's end, or past it where the model puts no top on it
    const wchar_t* warning; // what a glyph beside the label warns of, or nothing
};

constexpr wchar_t kPassesWarning[] = L"EXPERIMENTAL - FOR FUN ONLY.\n"
                                     L"Run the model multiple times against each same frame.\n"
                                     L"Running above 1 is NOT how it was meant to be used, but can lead to humorous results.";

// As far as a number the model puts no top on may be typed or stepped. The slider stretches to follow.
constexpr int kOpenUnits = 1000;

[[nodiscard]] constexpr int Open(int steps) noexcept
{
    return kOpenUnits * steps;
}

constexpr std::array<FieldSpec, kFieldCount> kFields{ {
    { L"Intensity", L"0% is the original image, 100% is the full effect: a blending percentage.\r\nThe command line takes it as 0 to 1.", 0, 100, 100, Notation::Percent, 10, 100, nullptr },
    { L"Local structure", L"Detail the model adds within a region.\r\nDoes nothing while auto mask is off.\r\n(You can manually set this higher than the slider limit)", 0, 1000, 100,
      Notation::Decimal, 100, Open(100), nullptr },
    { L"Local tone", L"How far the model moves local brightness.\r\n(You can manually set this higher than the slider limit)", 0, 1000, 100, Notation::Decimal, 100, Open(100), nullptr },
    { L"Skin structure", L"Detail on skin.\r\nDoes nothing while auto mask is off, or while skin follows local structure.\r\n(You can manually set this higher than the slider limit)", 0, 1000, 100,
      Notation::Decimal, 100, Open(100), nullptr },
    { L"Motion vector scale X", L"What the model multiplies the horizontal motion by. 1 passes the synthesised vectors through unchanged.", -400, 400, 100, Notation::Decimal, 10, 400, nullptr },
    { L"Motion vector scale Y", L"What the model multiplies the vertical motion by. 1 passes the synthesised vectors through unchanged.", -400, 400, 100, Notation::Decimal, 10, 400, nullptr },
    { L"Split position", L"Where the divider sits in the split view. Ctrl+Alt+Shift and the mouse drags it on screen.", 0, 100, 100, Notation::Decimal, 10, 100, nullptr },
    { L"Depth plane", L"The desktop has no depth, so one flat value stands in for all of it. Every pixel carries the same number, so changing it does nothing you can see.", 0, 100, 100,
      Notation::Decimal, 10, 100, nullptr },
    { L"Reset threshold", L"How much of the picture has to go unmatched before the model's history is thrown away.", 0, 100, 100, Notation::Decimal, 10, 100, nullptr },
    { L"Motion detail level", L"Finest level the matcher works at: 0 full resolution, 1 half, 2 quarter. Lower costs more.", 0, 7, 1, Notation::Count, 1, 7, nullptr },
    { L"Super resolution preset", L"Render preset asked of DLSS Super Resolution; 0 leaves the choice to the driver.", 0, 15, 1, Notation::Count, 1, 15, nullptr },
    { L"Model passes",
      L"How many times a frame the model runs, each pass on the picture the one before it made and with a history of its own. 1 is the model as it is meant to run.\r\n(You can manually set this "
      L"higher than the slider limit)",
      1, 8, 1, Notation::Count, 1, std::numeric_limits<int>::max(), kPassesWarning },
} };

struct ToggleSpec
{
    const wchar_t* label;
    const wchar_t* hint;
    bool resettable;        // a switch whose default is not obvious from the switch itself
    const wchar_t* warning; // what a glyph before the switch warns of, when the session says it applies, or nothing
};

// The model file passed the signature check, or the session would have stopped; this is about what it says it is.
constexpr wchar_t kModelMisnamed[] = L"nvngx_dlssnr.dll is signed by NVIDIA, but does not call its product \"NVIDIA DLSSNR\".\n"
                                     L"It may be another of NVIDIA's files under the model's name, or a later model.\n"
                                     L"It is used anyway; the log says what it calls itself.";

constexpr std::array<ToggleSpec, kToggleCount> kToggles{ {
    { L"Enable the model", L"Whether the model runs at all. Off costs nothing and shows the captured picture as it was.", false, kModelMisnamed },
    { L"Auto mask", L"Let the model find skin itself. Skin structure and local structure do nothing while this is off.", true, nullptr },
    { L"Skin follows local structure",
      L"Give skin whatever local structure is given, which is what the model reads -1 as. It is the only value between -1 and 0 that means anything, so it is a switch rather than part of the slider.",
      true, nullptr },
    { L"UI correction", L"Ask the model to leave interface pixels alone. The model reads this from a UI layer this app never binds, so it does nothing either way.", true, nullptr },
    { L"Depth is inverted", L"Tell the model the depth plane counts the other way. The plane is one constant, and a constant read backwards is the same constant, so this does nothing.", true,
      nullptr },
    { L"Vsync", L"Present in step with the monitor. Off presents as fast as the pipeline allows, which tears.", true, nullptr },
    { L"Capture border", L"Let Windows draw its yellow border around what is being captured.", true, nullptr },
    { L"Always on top", L"Keep the output window above every other window.", true, nullptr },
    { L"Redirection surface", L"Give the output window a GDI surface. Diagnostic; fixed when the window is made.", true, nullptr },
    { L"Direct3D debug layer",
      L"Turn on the Direct3D 12 validation layer. Slow, and only useful when chasing a fault. Windows turns it on for the whole program and will not turn it off, so turning it off here starts the "
      L"program again.",
      true, nullptr },
    { L"Model indicator", L"Let the model draw its own overlay naming its version, the preset it resolved and its working size. Read as the model loads, so it may need the program restarted.", true,
      nullptr },
    { L"Model kernel cache", L"Let the model cache its compiled kernels. Off makes it rebuild them every run. Read as the model loads, so it may need the program restarted.", true, nullptr },
} };

// Whether a switch's warning applies. The only switch that carries one is the model's, and what it warns of
// is the model file calling itself something else, which the session settled before the panel was built.
[[nodiscard]] bool ToggleWarns(std::size_t toggle, const PanelFindings& findings) noexcept
{
    return kToggles[toggle].warning != nullptr && !findings.modelAsNamed;
}

struct GroupSpec
{
    const wchar_t* label;
    const wchar_t* hint;
    std::size_t count;
    std::array<const wchar_t*, kMaxChoices> choices;
    const wchar_t* warning; // what a glyph before the label warns of, when the session says it applies, or nothing
};

// What super resolution wants and this machine may not have. The glyph and the hint appear together.
constexpr wchar_t kNoSuperResolution[] = L"Not offered.\n"
                                         L"DLSS Super Resolution runs from nvngx_dlss.dll, which NVIDIA ships separately: games carry a copy, the DLSS SDK has one, and recent drivers keep one "
                                         L"of their own. Put it next to the executable or in the folder --ngx-path names.\n"
                                         L"The rest of the session runs without it.";

constexpr std::array<GroupSpec, kGroupCount> kGroups{ {
    { L"Show", L"What the window shows: the model's work, both either side of a divider, or the picture as captured.", 3, { L"Processed", L"Split", L"Original" }, nullptr },
    { L"Style", L"Which of the model's three looks to ask for. The model clamps anything else.", 3, { L"Standard", L"Natural", L"Cinematic" }, nullptr },
    { L"Cursor", L"Whether the captured picture includes the mouse pointer. Auto keeps the session's own choice.", 3, { L"Auto", L"On", L"Off" }, nullptr },
    { L"Motion vectors",
      L"Where the model's motion comes from: matching blocks between frames, the hardware flow engine, or nothing at all.",
      3,
      { L"Block matching", L"Optical flow", L"None" },
      nullptr },
    { L"Optical flow grid", L"How coarse the hardware flow engine's output is.", 3, { L"1", L"2", L"4" }, nullptr },
    { L"Optical flow effort", L"How hard the hardware flow engine works.", 3, { L"Slow", L"Medium", L"Fast" }, nullptr },
    { L"Super resolution", L"Whether DLSS Super Resolution runs before the model, and whether it runs at all when the sizes match.", 3, { L"Auto", L"DLAA", L"Off" }, kNoSuperResolution },
    { L"Colour format", L"How much precision the model's picture carries.", 2, { L"8 bit", L"16 bit float", nullptr }, nullptr },
    { L"Log level", L"How much the log says.", 4, { L"Debug", L"Info", L"Warn", L"Error" }, nullptr },
} };

// Whether a group's warning applies. The only group that carries one is super resolution, and what it
// warns of is the model being absent, which the session settled before the panel was built.
[[nodiscard]] bool GroupWarns(std::size_t group, const PanelFindings& findings) noexcept
{
    return kGroups[group].warning != nullptr && !findings.superResolution;
}

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

struct PickSpec
{
    const wchar_t* label;
    const wchar_t* hint;
    const wchar_t* nothing;
};

constexpr std::array<PickSpec, kPickCount> kPicks{ {
    { L"Window",
      L"Drag this onto a window to work on that one window instead of a monitor. The title under the pointer is shown beside it as you go, and letting go over the desktop goes back to a monitor.",
      L"none: capturing a monitor" },
} };

// Text on the About page, plain or with a web address in it as a link. How many lines it wraps into is
// written here: the column and the text scale together with the display, so the count holds.
struct NoteSpec
{
    const wchar_t* text;
    int lines;
};

constexpr std::array<NoteSpec, kNoteCount> kNotes{ {
    { L"Full-Screen Wrapper for DLSS5, version " DSCREEN_VERSION_STRING, 1 },
    { L"An experimental tool that runs NVIDIA's DLSS 5 Neural Rendering model on the desktop, or on one window. It is not an NVIDIA product, and the model file is not included with it.", 3 },
    { L"Source code, releases and issues: <a href=\"https://github.com/ThioJoe/DLSS5-Entire-Screen\">github.com/ThioJoe/DLSS5-Entire-Screen</a>", 2 },
} };

// --- what sits on which page, and in what order -------------------------------------------------------

// A frame is a group box around the rows it names. A break ends a column early, so a page can say which
// rows stand on the right rather than leaving that to how many happen to fit on the left. A note is text.
enum class Kind : std::uint8_t { Field, Toggle, Group, Pick, List, Frame, Break, Note };

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
[[nodiscard]] constexpr RowSpec Of(Pick t) noexcept
{
    return RowSpec{ Kind::Pick, static_cast<std::size_t>(t) };
}
[[nodiscard]] constexpr RowSpec Of(List l) noexcept
{
    return RowSpec{ Kind::List, static_cast<std::size_t>(l) };
}
[[nodiscard]] constexpr RowSpec Of(Frame f) noexcept
{
    return RowSpec{ Kind::Frame, static_cast<std::size_t>(f) };
}
[[nodiscard]] constexpr RowSpec Of(Note n) noexcept
{
    return RowSpec{ Kind::Note, static_cast<std::size_t>(n) };
}

constexpr RowSpec kNextColumn{ Kind::Break, 0 };

constexpr std::size_t kMaxFrameRows = 4;

struct FrameSpec
{
    const wchar_t* caption;
    std::size_t count;
    std::array<RowSpec, kMaxFrameRows> rows;
};

// Style changes nothing while local tone is zero, so the two share a frame; the skin controls share one
// because each of the three decides what the others mean; and the comparison is a view, not a setting.
constexpr std::array<FrameSpec, kFrameCount> kFrames{ {
    { L"Tone", 2, { Of(Field::LocalTone), Of(Group::Style) } },
    { L"Skin", 3, { Of(Toggle::AutoMask), Of(Toggle::SkinFollowsStructure), Of(Field::Skin) } },
    { L"Compare", 2, { Of(Group::Compare), Of(Field::Split) } },
} };

constexpr std::size_t kMaxRows = kColumns * kMaxRowsPerColumn;

struct PageSpec
{
    const wchar_t* title;
    std::size_t count;
    std::array<RowSpec, kMaxRows> rows;
};

// Each page's two columns are set by the table rather than by how many rows happen to fit the left one, so
// the Model page has the tuning down the left and on the right how many times the model runs, what its
// work is compared against and which window it is given; the View page has what is captured on the left
// and how it is shown on the right; and the Advanced page has what the model is fed on the left and the
// machine and its diagnostics on the right. The Inert page holds what a desktop gives the model no way to
// answer to: the depth plane is one constant, UI correction reads a layer nothing binds, and optical flow
// drives a backend this build leaves out.
constexpr std::array<PageSpec, static_cast<std::size_t>(Page::Count)> kPages{ {
    { L"Model",
      10,
      { Of(Toggle::NeuralRendering), Of(List::Preset), Of(Field::Intensity), Of(Field::LocalStructure), Of(Frame::Tone), Of(Frame::Skin), kNextColumn, Of(Field::Passes), Of(Frame::Compare),
        Of(Pick::Window) } },
    { L"View", 8, { Of(List::Source), Of(Group::Cursor), Of(Toggle::CaptureBorder), kNextColumn, Of(List::Target), Of(Toggle::Vsync), Of(Toggle::Topmost), Of(Group::LogLevel) } },
    { L"Advanced",
      14,
      { Of(Group::Format), Of(Group::Sr), Of(Field::SrPreset), Of(Group::Motion), Of(Field::MvLevel), Of(Field::MvScaleX), Of(Field::MvScaleY), Of(Field::ResetThreshold), kNextColumn,
        Of(List::Adapter), Of(Toggle::RedirectionBitmap), Of(Toggle::DebugLayer), Of(Toggle::Indicator), Of(Toggle::CubinCache) } },
    { L"About", 3, { Of(Note::Title), Of(Note::Purpose), Of(Note::Repository) } },
    { L"Inert", 6, { Of(Field::DepthValue), Of(Toggle::DepthInverted), Of(Toggle::UiCorrection), kNextColumn, Of(Group::NvofGrid), Of(Group::NvofPerf) } },
} };

[[nodiscard]] constexpr std::span<const RowSpec> RowsOf(const PageSpec& page) noexcept
{
    return std::span<const RowSpec>(page.rows.data(), page.count);
}

[[nodiscard]] constexpr std::span<const RowSpec> RowsOf(const FrameSpec& frame) noexcept
{
    return std::span<const RowSpec>(frame.rows.data(), frame.count);
}

[[nodiscard]] constexpr std::span<const RowSpec> RowsOf(Page page) noexcept
{
    return RowsOf(kPages[static_cast<std::size_t>(page)]);
}

// The rows a frame surrounds, which stand on its page inside it rather than in the page's own list.
[[nodiscard]] constexpr std::span<const RowSpec> InnerRows(std::size_t frame) noexcept
{
    return RowsOf(kFrames[frame]);
}

// Every control belongs to exactly one page, on it or in a frame on it. One left off would be placed
// nowhere and stop the program as it starts, so the tables are counted here instead of trusted.
[[nodiscard]] constexpr std::size_t CountOfKind(std::span<const RowSpec> rows, Kind kind) noexcept
{
    return static_cast<std::size_t>(std::ranges::count_if(rows, [kind](const RowSpec& r) { return r.kind == kind; }));
}

[[nodiscard]] constexpr std::size_t RowsOfKind(Kind kind) noexcept
{
    const auto onPage = [kind](std::size_t so, const PageSpec& page) { return so + CountOfKind(RowsOf(page), kind); };
    const auto inFrame = [kind](std::size_t so, const FrameSpec& frame) { return so + CountOfKind(RowsOf(frame), kind); };
    return std::ranges::fold_left(kFrames, std::ranges::fold_left(kPages, std::size_t{ 0 }, onPage), inFrame);
}

// A frame holds rows, not frames or breaks; and a page with two columns breaks at most once.
[[nodiscard]] constexpr bool FramesAreFlat() noexcept
{
    return std::ranges::all_of(kFrames, [](const FrameSpec& frame) { return CountOfKind(RowsOf(frame), Kind::Frame) == 0 && CountOfKind(RowsOf(frame), Kind::Break) == 0; });
}

[[nodiscard]] constexpr bool PagesBreakOnce() noexcept
{
    return std::ranges::all_of(kPages, [](const PageSpec& page) { return CountOfKind(RowsOf(page), Kind::Break) < static_cast<std::size_t>(kColumns); });
}

static_assert(RowsOfKind(Kind::Field) == kFieldCount);
static_assert(RowsOfKind(Kind::Toggle) == kToggleCount);
static_assert(RowsOfKind(Kind::Group) == kGroupCount);
static_assert(RowsOfKind(Kind::Pick) == kPickCount);
static_assert(RowsOfKind(Kind::List) == kListCount);
static_assert(RowsOfKind(Kind::Frame) == kFrameCount);
static_assert(RowsOfKind(Kind::Note) == kNoteCount);
static_assert(FramesAreFlat());
static_assert(PagesBreakOnce());

// --- measurement ---------------------------------------------------------------------------------------

// Everything is measured in the display's own dots and in the height of a line of its own text, so a row
// is always tall enough for what it holds however the display is scaled.
struct Metrics
{
    int dpi;
    int line;
    int column;              // how tall a column is, taken from whichever page needs the most
    int body;                // how tall the notice's own text is, measured in the font it is drawn in
    const PanelLists* lists; // borrowed for as long as the panel is being built, which is the only time it is read
    [[nodiscard]] int Of(int reference) const noexcept { return ::MulDiv(reference, dpi, kReferenceDpi); }
    [[nodiscard]] int LabelHeight() const noexcept { return line + Of(3); }
    [[nodiscard]] int ControlHeight() const noexcept { return std::max(Of(22), line + Of(6)); }
    [[nodiscard]] int RowGap() const noexcept { return Of(kRowGap); }
    // A labelled row, which is what most rows are and what a column is counted in.
    [[nodiscard]] int RowHeight() const noexcept { return LabelHeight() + ControlHeight() + RowGap(); }
    // From a frame's top edge, where its caption is written, to the first row in it.
    [[nodiscard]] int CaptionHeight() const noexcept { return line + Of(kCaptionGap); }
    // A note is as tall as the lines it wraps into, with a little under the last for the descenders.
    [[nodiscard]] int NoteHeight(int lines) const noexcept { return lines * line + Of(2); }
    [[nodiscard]] int PageTop() const noexcept { return Of(kMargin + kTabHeight); }
    // The rows, then the notice under them, then the margin.
    [[nodiscard]] int PageHeight() const noexcept { return column + ControlHeight() + Of(2 * kMargin); }
    [[nodiscard]] int NoticeTop() const noexcept { return PageTop() + column + Of(kMargin); }
    [[nodiscard]] int BodyHeight() const noexcept { return body + Of(kMargin); }
};

struct Placement
{
    int left; // in reference pixels
    int top;  // in the display's dots
    int control;
    int width; // in reference pixels: the column, or what is left of it inside a frame
};

// How much of a column a row takes, in the display's dots, the gap under it included.
[[nodiscard]] int HeightOf(const RowSpec& row, const Metrics& m) noexcept
{
    // A switch is its own label, so its row is the control and the gap. A row holding a runtime list is as
    // tall as the list needs, and a list with nothing in it takes no room at all, which is how a page drops
    // a choice the machine could not offer.
    static constexpr auto LinesOfList = [] [[nodiscard]] (std::size_t count) noexcept -> int { return static_cast<int>((count + kListPerLine - 1) / kListPerLine); };

    static constexpr auto HeightOfList = [] [[nodiscard]] (std::size_t list, const Metrics& m) noexcept -> int {
        const int lines = LinesOfList((*m.lists)[list].choices.Size());
        if (lines == 0)
            return 0;
        return m.LabelHeight() + lines * m.ControlHeight() + m.RowGap();
    };

    static constexpr auto HeightOfRow = [] [[nodiscard]] (const RowSpec& row, const Metrics& m) noexcept -> int {
        if (row.kind == Kind::Toggle)
            return m.ControlHeight() + m.RowGap();
        if (row.kind == Kind::List)
            return HeightOfList(row.index, m);
        if (row.kind == Kind::Note)
            return m.NoteHeight(kNotes[row.index].lines) + m.RowGap();
        return m.RowHeight();
    };

    // A frame is as tall as the rows in it, with its caption above them, its edge below them and a gap of
    // its own under that; a break has no height, only a column of its own.
    static constexpr auto HeightOfFrame = [] [[nodiscard]] (std::size_t frame, const Metrics& m) noexcept -> int {
        const auto counted = [&m](int so, const RowSpec& inner) { return so + HeightOfRow(inner, m); };
        return std::ranges::fold_left(InnerRows(frame), m.CaptionHeight() + m.Of(kFrameBottom + kFrameGap), counted);
    };
    if (row.kind == Kind::Break)
        return 0;
    if (row.kind == Kind::Frame)
        return HeightOfFrame(row.index, m);
    return HeightOfRow(row, m);
}

// Where the next row starts. A row never straddles a column, so one that will not fit in what is left of
// this column begins the next one.
struct Cell
{
    std::size_t column;
    int offset; // from the top of the page, in the display's dots
};

[[nodiscard]] Cell Fitted(const Cell& at, int height, int column) noexcept
{
    if (at.offset + height <= column)
        return at;
    return Cell{ at.column + 1, 0 };
}

// Where a row lands, and where the row after it starts. A break lands nowhere and starts the next column.
struct Landing
{
    Cell at;
    Cell next;
};

[[nodiscard]] Landing Landed(const Cell& so, const RowSpec& row, const Metrics& m) noexcept
{
    static constexpr auto Past = [] [[nodiscard]] (const Cell& at, int height) noexcept -> Cell { return Cell{ at.column, at.offset + height }; };
    if (row.kind == Kind::Break)
        return Landing{ Cell{ so.column + 1, 0 }, Cell{ so.column + 1, 0 } };
    const int height = HeightOf(row, m);
    const Cell at = Fitted(so, height, m.column);
    return Landing{ at, Past(at, height) };
}

// Walking a page: each row is put where the cursor stands, and the cursor moves on by the row's height.
struct Walk
{
    Cell at;
    std::optional<Placement> found;
};

// A switch has no label of its own above it, and a note is only text, so each stands where a label would.
[[nodiscard]] Placement PlaceOf(const RowSpec& row, const Cell& at, const Metrics& m) noexcept
{
    const int top = m.PageTop() + at.offset;
    const int control = row.kind == Kind::Toggle || row.kind == Kind::Note ? top : top + m.LabelHeight();
    return Placement{ kMargin + static_cast<int>(at.column) * (kColumnWidth + kMargin), top, control, kColumnWidth };
}

// The rows inside a frame stand in from its edges, so its lines run clear of them.
[[nodiscard]] Placement Inset(const Placement& at) noexcept
{
    return Placement{ at.left + kFrameInset, at.top, at.control, at.width - 2 * kFrameInset };
}

// Where the row asked for stands, if it is this row or is inside this frame.
[[nodiscard]] std::optional<Placement> PlacedAt(const RowSpec& row, const Cell& at, Kind kind, std::size_t index, const Metrics& m) noexcept
{
    static constexpr auto IsWanted = [] [[nodiscard]] (const RowSpec& row, Kind kind, std::size_t index) noexcept -> bool { return row.kind == kind && row.index == index; };

    // The rows inside a frame start under its caption, each right under the one before: the frame was fitted
    // whole, so none of them can run out of column.
    static constexpr auto FoundInFrame = [] [[nodiscard]] (std::size_t frame, const Cell& top, Kind kind, std::size_t index, const Metrics& m) noexcept -> std::optional<Placement> {
        const auto step = [kind, index, &m](const Walk& so, const RowSpec& inner) {
            const std::optional<Placement> found = IsWanted(inner, kind, index) ? std::optional<Placement>{ Inset(PlaceOf(inner, so.at, m)) } : so.found;
            return Walk{ Cell{ so.at.column, so.at.offset + HeightOf(inner, m) }, found };
        };
        return std::ranges::fold_left(InnerRows(frame), Walk{ Cell{ top.column, top.offset + m.CaptionHeight() }, std::nullopt }, step).found;
    };
    if (IsWanted(row, kind, index))
        return PlaceOf(row, at, m);
    if (row.kind == Kind::Frame)
        return FoundInFrame(row.index, at, kind, index, m);
    return std::nullopt;
}

[[nodiscard]] Walk Stepped(const Walk& so, const RowSpec& row, Kind kind, std::size_t index, const Metrics& m) noexcept
{
    const Landing landed = Landed(so.at, row, m);
    return Walk{ landed.next, so.found.has_value() ? so.found : PlacedAt(row, landed.at, kind, index, m) };
}

// --- the window and its furniture ------------------------------------------------------------------------

// Closing hides the panel rather than destroying it; the session reads that as the operator leaving.
[[nodiscard]] LRESULT Closed(HWND window) noexcept
{
    (void)::ShowWindow(window, SW_HIDE);
    return 0;
}

[[nodiscard]] std::optional<int> TypedSteps(HWND box, const FieldSpec& spec) noexcept;
void WriteBox(HWND box, int steps, const FieldSpec& spec) noexcept;

// WAIVER(R17): the window procedure is called by the OS, which discards nothing and ignores attributes.
LRESULT CALLBACK PanelProc(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
{
    // Dragging the panel by its title bar is done here rather than left to Windows, whose way of doing it owns
    // the thread until the button comes up. That thread draws the picture, which stopped dead for every drag.
    static constexpr auto CursorNow = [] [[nodiscard]] () noexcept -> POINT {
        POINT cursor{ 0, 0 }; // WAIVER(R2): the answer of one query, read once after it.
        (void)::GetCursorPos(&cursor);
        return cursor;
    };

    static constexpr auto GrabbedCaption = [] [[nodiscard]] (HWND window) noexcept -> LRESULT {
        // Where the window's corner sits relative to the pointer, packed into the window's own data: two halves of
        // one number, so the drag needs nowhere else to keep anything.
        static constexpr auto HoldGrip = [](HWND window, LONG x, LONG y) noexcept -> void {
            const std::uint64_t packed = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(x)) << 32) | static_cast<std::uint32_t>(y);
            (void)::SetWindowLongPtrW(window, GWLP_USERDATA, static_cast<LONG_PTR>(packed));
        };

        static constexpr auto Captured = [] [[nodiscard]] (HWND window) noexcept -> LRESULT {
            (void)::SetCapture(window);
            return 0;
        };
        RECT frame{ 0, 0, 0, 0 }; // WAIVER(R2): the answer of one query, read once after it.
        ENSURE(::GetWindowRect(window, &frame) != FALSE);
        const POINT cursor = CursorNow();
        HoldGrip(window, frame.left - cursor.x, frame.top - cursor.y);
        return Captured(window);
    };

    static constexpr auto GrabsCaption = [] [[nodiscard]] (UINT message, WPARAM w) noexcept -> bool { return message == WM_NCLBUTTONDOWN && w == HTCAPTION; };

    static constexpr auto ClosedOrMoved = [] [[nodiscard]] (HWND window, UINT message, WPARAM w, LPARAM l) noexcept -> LRESULT {
        static constexpr auto MovedOrDefault = [] [[nodiscard]] (HWND window, UINT message, WPARAM w, LPARAM l) noexcept -> LRESULT {
            static constexpr auto DraggedTo = [] [[nodiscard]] (HWND window) noexcept -> LRESULT {
                static constexpr auto Grip = [] [[nodiscard]] (HWND window) noexcept -> POINT {
                    const std::uint64_t packed = static_cast<std::uint64_t>(::GetWindowLongPtrW(window, GWLP_USERDATA));
                    return POINT{ static_cast<LONG>(static_cast<std::int32_t>(packed >> 32)), static_cast<LONG>(static_cast<std::int32_t>(packed & 0xFFFFFFFFu)) };
                };
                if (::GetCapture() != window)
                    return 0;
                const POINT cursor = CursorNow();
                const POINT grip = Grip(window);
                (void)::SetWindowPos(window, nullptr, cursor.x + grip.x, cursor.y + grip.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
                return 0;
            };

            static constexpr auto LetGoOrDefault = [] [[nodiscard]] (HWND window, UINT message, WPARAM w, LPARAM l) noexcept -> LRESULT {
                static constexpr auto Released = [] [[nodiscard]] () noexcept -> LRESULT {
                    (void)::ReleaseCapture();
                    return 0;
                };

                static constexpr auto NotifiedOrDefault = [] [[nodiscard]] (HWND window, UINT message, WPARAM w, LPARAM l) noexcept -> LRESULT {
                    static constexpr auto Notified = [] [[nodiscard]] (LPARAM l) noexcept -> LRESULT {
                        // An up-down can only write whole numbers into its buddy, so it is answered here instead: one turn of an
                        // arrow is one of the field's own increments, applied to the box, which is what the panel reads.
                        static constexpr auto Nudge = [](const NMUPDOWN* delta) noexcept -> void {
                            static constexpr auto NudgeBox = [](HWND box, const FieldSpec& spec, int steps) noexcept -> void {
                                const std::optional<int> now = TypedSteps(box, spec);
                                if (!now.has_value())
                                    return;
                                WriteBox(box, std::clamp(*now + steps * spec.increment, spec.minimum, spec.ceiling), spec);
                            };
                            const std::size_t field = static_cast<std::size_t>(::GetWindowLongPtrW(delta->hdr.hwndFrom, GWLP_USERDATA));
                            HWND box = reinterpret_cast<HWND>(::SendMessageW(delta->hdr.hwndFrom, UDM_GETBUDDY, 0, 0));
                            NudgeBox(box, kFields[field], delta->iDelta);
                        };

                        static constexpr auto Nudged = [] [[nodiscard]] (LPARAM l) noexcept -> LRESULT {
                            Nudge(reinterpret_cast<const NMUPDOWN*>(l));
                            return 1; // the control keeps the position it was given, which nothing reads
                        };

                        // A link on the About page is opened in the default browser. Only a link control is answered, since
                        // the tabs send the same click, and only a web address is opened, which is all the notes carry.
                        static constexpr auto Follow = [](const NMHDR* header, LPARAM l) noexcept -> void {
                            static constexpr auto IsLink = [] [[nodiscard]] (HWND from) noexcept -> bool {
                                std::array<wchar_t, kClassNameCapacity> name{}; // WAIVER(R2): a local buffer filled once, before use.
                                (void)::GetClassNameW(from, name.data(), kClassNameCapacity);
                                return std::wstring_view(name.data()) == WC_LINK;
                            };

                            static constexpr auto OpenWebAddress = [](const wchar_t* address) noexcept -> void {
                                if (!std::wstring_view(address).starts_with(L"https://"))
                                    return;
                                (void)::ShellExecuteW(nullptr, L"open", address, nullptr, nullptr, SW_SHOWNORMAL);
                            };
                            if (!IsLink(header->hwndFrom))
                                return;
                            OpenWebAddress(reinterpret_cast<const NMLINK*>(l)->item.szUrl);
                        };
                        const NMHDR* header = reinterpret_cast<const NMHDR*>(l);
                        if (header->code == UDN_DELTAPOS)
                            return Nudged(l);
                        if (header->code == NM_CLICK || header->code == NM_RETURN)
                            Follow(header, l);
                        return 0;
                    };
                    if (message == WM_NOTIFY)
                        return Notified(l);
                    return ::DefWindowProcW(window, message, w, l);
                };
                if (message == WM_LBUTTONUP)
                    return Released();
                return NotifiedOrDefault(window, message, w, l);
            };
            if (message == WM_MOUSEMOVE)
                return DraggedTo(window);
            return LetGoOrDefault(window, message, w, l);
        };
        if (message == WM_CLOSE)
            return Closed(window);
        return MovedOrDefault(window, message, w, l);
    };
    if (GrabsCaption(message, w))
        return GrabbedCaption(window);
    return ClosedOrMoved(window, message, w, l);
}

// Segoe MDL2 Assets has shipped with Windows since 10, and its refresh glyph fits a button too short for a word.
constexpr wchar_t kRefreshGlyph[] = L"\uE72C";
constexpr wchar_t kWarningGlyph[] = L"\uE7BA";
constexpr wchar_t kResetHint[] = L"Put this setting back to the value it starts at.";
constexpr wchar_t kReleaseHint[] = L"Let the window go and capture a monitor again.";

// --- reading and writing a number ------------------------------------------------------------------------

[[nodiscard]] int SliderPosition(HWND slider) noexcept
{
    return static_cast<int>(::SendMessageW(slider, TBM_GETPOS, 0, 0));
}

[[nodiscard]] std::array<wchar_t, kTextCapacity> TextOf(HWND control) noexcept
{
    std::array<wchar_t, kTextCapacity> text{}; // WAIVER(R2): a local buffer filled once, before use.
    (void)::GetWindowTextW(control, text.data(), kTextCapacity);
    return text;
}

[[nodiscard]] std::optional<int> TypedSteps(HWND box, const FieldSpec& spec) noexcept
{
    // A percentage is typed as one, with or without its sign, and stands for the fraction it names.
    static constexpr auto Fraction = [] [[nodiscard]] (float typed, const FieldSpec& spec) noexcept -> float { return spec.notation == Notation::Percent ? typed / 100.0f : typed; };
    const std::array<wchar_t, kTextCapacity> text = TextOf(box);
    wchar_t* end = nullptr;
    const float value = std::wcstof(text.data(), &end);
    if (end == text.data())
        return std::nullopt;
    return std::clamp(static_cast<int>(std::lround(Fraction(value, spec) * static_cast<float>(spec.steps))), spec.minimum, spec.ceiling);
}

[[nodiscard]] int Settled(const ControlPanel& panel, std::size_t field) noexcept
{
    // The value the slider and the box were last agreed on, kept beside the box. Whichever of the two now
    // differs from it is the one the operator moved, and the arrows move the box.
    static constexpr auto CommittedIn = [] [[nodiscard]] (HWND box) noexcept -> int { return static_cast<int>(::GetWindowLongPtrW(box, GWLP_USERDATA)); };
    const int committed = CommittedIn(panel.boxes[field]);
    const int slider = SliderPosition(panel.sliders[field]);
    if (slider != committed)
        return slider;
    return TypedSteps(panel.boxes[field], kFields[field]).value_or(committed);
}

// The panel is written to on every frame, and writing a control text it already holds still invalidates it
// and costs a repaint, on the thread that draws the picture. So nothing is written twice.
[[nodiscard]] bool Reads(HWND control, const wchar_t* wanted) noexcept
{
    const std::array<wchar_t, kTextCapacity> now = TextOf(control);
    return std::wstring_view(now.data()) == std::wstring_view(wanted);
}

void WriteText(HWND control, const wchar_t* wanted) noexcept
{
    if (Reads(control, wanted))
        return;
    ENSURE(::SetWindowTextW(control, wanted) != FALSE);
}

void WriteBox(HWND box, int steps, const FieldSpec& spec) noexcept
{
    static constexpr auto Printed = [] [[nodiscard]] (int steps, const FieldSpec& spec) noexcept -> infra::BoundedString<char, 15> {
        if (spec.notation == Notation::Count)
            return infra::Formatted<15>("{}", steps);
        if (spec.notation == Notation::Percent)
            return infra::Formatted<15>("{}%", std::lround(100.0 * steps / spec.steps));
        return infra::Formatted<15>("{:.2f}", static_cast<double>(steps) / spec.steps);
    };

    // The digits are ASCII, so widening them is a character-for-character copy.
    static constexpr auto Widened = [] [[nodiscard]] (std::string_view text) noexcept -> std::array<wchar_t, kTextCapacity> {
        std::array<wchar_t, kTextCapacity> wide{}; // WAIVER(R2): a local buffer filled once, before use.
        std::ranges::copy(text | std::views::take(wide.size() - 1) | std::views::transform([](char c) { return static_cast<wchar_t>(c); }), wide.begin());
        return wide;
    };
    const std::array<wchar_t, kTextCapacity> wanted = Widened(Printed(steps, spec).Get());
    WriteText(box, wanted.data());
}

void Commit(const ControlPanel& panel, std::size_t field, int steps) noexcept
{
    static constexpr auto KeepCommitted = [](HWND box, int steps) noexcept -> void { (void)::SetWindowLongPtrW(box, GWLP_USERDATA, static_cast<LONG_PTR>(steps)); };

    // A box being typed into is left alone; anything else moves the caret out from under the operator.
    static constexpr auto ShowInBox = [](HWND box, int steps, const FieldSpec& spec) noexcept -> void {
        if (::GetFocus() == box)
            return;
        WriteBox(box, steps, spec);
    };

    // A number carried past the slider's end takes the slider with it, so all three controls keep agreeing and
    // the one that moved is still the one that stands out.
    static constexpr auto StretchSlider = [](HWND slider, int steps) noexcept -> void {
        if (steps > static_cast<int>(::SendMessageW(slider, TBM_GETRANGEMAX, 0, 0)))
            (void)::SendMessageW(slider, TBM_SETRANGEMAX, TRUE, steps);
    };

    // A slider redraws its thumb inside TBM_SETPOS rather than at the next paint, so a slider already at the
    // position asked for is left where it is.
    static constexpr auto MoveSlider = [](HWND slider, int steps) noexcept -> void {
        if (SliderPosition(slider) == steps)
            return;
        (void)::SendMessageW(slider, TBM_SETPOS, TRUE, steps);
    };
    StretchSlider(panel.sliders[field], steps);
    MoveSlider(panel.sliders[field], steps);
    KeepCommitted(panel.boxes[field], steps);
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

// --- the comparison ------------------------------------------------------------------------------------

// The compare buttons run Processed, Split, Original, so the two whole pictures stand either side of the
// view that shows both. DisplayMode counts them in another order, so a choice is looked up rather than cast.
constexpr std::array<interior::DisplayMode, 3> kCompareChoices{ interior::DisplayMode::Processed, interior::DisplayMode::Split, interior::DisplayMode::Original };
static_assert(kCompareChoices.size() == kGroups[static_cast<std::size_t>(Group::Compare)].count);

[[nodiscard]] interior::DisplayMode DisplayFrom(std::size_t choice) noexcept
{
    return kCompareChoices[std::min(choice, kCompareChoices.size() - 1)];
}

[[nodiscard]] std::size_t ChoiceOfDisplay(interior::DisplayMode display) noexcept
{
    const auto found = std::ranges::find(kCompareChoices, display);
    return found == kCompareChoices.end() ? 0 : static_cast<std::size_t>(std::ranges::distance(kCompareChoices.begin(), found));
}

// --- the values the controls start at -----------------------------------------------------------------

[[nodiscard]] int StepsOf(float value, const FieldSpec& spec) noexcept
{
    return std::clamp(static_cast<int>(std::lround(value * static_cast<float>(spec.steps))), spec.minimum, spec.ceiling);
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
             static_cast<float>(o.srPreset.Get()),
             static_cast<float>(live.passes.Get()) };
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

// --- building the controls -----------------------------------------------------------------------------

// An arrow key on the slider moves what an arrow beside the box moves, and a page moves five of them, so
// the three ways of nudging a number all agree with one another.
[[nodiscard]] HWND Stepped(HWND slider, const FieldSpec& spec) noexcept
{
    (void)::SendMessageW(slider, TBM_SETLINESIZE, 0, spec.increment);
    (void)::SendMessageW(slider, TBM_SETPAGESIZE, 0, 5 * spec.increment);
    return slider;
}

struct Built
{
    std::array<HWND, kFieldCount> labels;
    std::array<HWND, kFieldCount> sliders;
    std::array<HWND, kFieldCount> boxes;
    std::array<HWND, kFieldCount> spins;
    std::array<HWND, kFieldCount> resets;
    std::array<HWND, kFieldCount> warnings;
    std::array<HWND, kToggleCount> toggles;
    std::array<HWND, kToggleCount> toggleResets;
    std::array<HWND, kToggleCount> toggleWarnings;
    std::array<HWND, kGroupCount> groupLabels;
    std::array<HWND, kGroupCount> groupWarnings;
    std::array<std::array<HWND, kMaxChoices>, kGroupCount> choices;
    std::array<HWND, kPickCount> pickLabels;
    std::array<HWND, kPickCount> crosshairs;
    std::array<HWND, kPickCount> pickNames;
    std::array<HWND, kPickCount> pickResets;
    std::array<HWND, kListCount> listLabels;
    std::array<std::array<HWND, kMaxListChoices>, kListCount> listChoices;
    std::array<HWND, kFrameCount> frames;
    std::array<HWND, kNoteCount> notes;
};

[[nodiscard]] LRESULT CALLBACK HighlightProc(HWND window, UINT message, WPARAM w, LPARAM l) noexcept;

// The crosshair keeps two windows in its own window data, so the panel can be moved about and copied
// without the picking leaving anything dangling behind it.
constexpr int kPointingAt = GWLP_USERDATA;                     // what the pointer is over, which the label shows as it goes
constexpr int kChosen = 0;                                     // what the operator chose by letting the button up
constexpr int kHighlight = static_cast<int>(sizeof(LONG_PTR)); // the outline drawn around what the pointer is over

[[nodiscard]] std::optional<interior::MonitorHandle> HandleAt(HWND crosshair, int slot) noexcept
{
    return infra::AsOptional(interior::MonitorHandleTag::Parse(static_cast<std::uintptr_t>(::GetWindowLongPtrW(crosshair, slot))));
}

void KeepAt(HWND crosshair, int slot, const std::optional<interior::MonitorHandle>& window) noexcept
{
    (void)::SetWindowLongPtrW(crosshair, slot, static_cast<LONG_PTR>(window.has_value() ? window->Get() : 0u));
}

// Letting go of a window, and picking one up, both mean the same thing to a drag that has not started.
void KeepBoth(HWND crosshair, const std::optional<interior::MonitorHandle>& window) noexcept
{
    KeepAt(crosshair, kPointingAt, window);
    KeepAt(crosshair, kChosen, window);
}

// WAIVER(R17): the window procedure is called by the OS, which discards nothing and ignores attributes.
LRESULT CALLBACK CrosshairProc(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
{
    static constexpr auto PaintedCrosshair = [] [[nodiscard]] (HWND window) noexcept -> LRESULT {
        static constexpr auto PaintCrosshair = [](HWND crosshair) noexcept -> void {
            static constexpr auto DrawCrosshair = [](HDC dc, const RECT& box) noexcept -> void {
                static constexpr auto ArmsOf = [](HDC dc, const RECT& box, int radius) noexcept -> void {
                    static constexpr auto VerticalArms = [](HDC dc, const RECT& box, int radius) noexcept -> void {
                        (void)::MoveToEx(dc, box.right / 2, box.top + 1, nullptr);
                        (void)::LineTo(dc, box.right / 2, box.bottom / 2 - radius);
                        (void)::MoveToEx(dc, box.right / 2, box.bottom / 2 + radius, nullptr);
                        (void)::LineTo(dc, box.right / 2, box.bottom - 1);
                    };
                    (void)::MoveToEx(dc, box.left + 1, box.bottom / 2, nullptr);
                    (void)::LineTo(dc, box.right / 2 - radius, box.bottom / 2);
                    (void)::MoveToEx(dc, box.right / 2 + radius, box.bottom / 2, nullptr);
                    (void)::LineTo(dc, box.right - 1, box.bottom / 2);
                    VerticalArms(dc, box, radius);
                };
                ::FillRect(dc, &box, ::GetSysColorBrush(COLOR_BTNFACE));
                (void)::SelectObject(dc, ::GetStockObject(NULL_BRUSH));
                const int radius = std::min(box.right - box.left, box.bottom - box.top) / 3;
                ::Ellipse(dc, box.right / 2 - radius, box.bottom / 2 - radius, box.right / 2 + radius, box.bottom / 2 + radius);
                ArmsOf(dc, box, radius);
            };
            PAINTSTRUCT paint{}; // WAIVER(R2): the record the OS fills to hand over the device context.
            RECT box{};
            (void)::GetClientRect(crosshair, &box);
            DrawCrosshair(::BeginPaint(crosshair, &paint), box);
            (void)::EndPaint(crosshair, &paint);
        };
        PaintCrosshair(window);
        return 0;
    };

    static constexpr auto DraggedCrosshair = [] [[nodiscard]] (HWND window, UINT message, WPARAM w, LPARAM l) noexcept -> LRESULT {
        // The drag is a capture rather than a modal loop, so the session keeps running and the panel keeps being
        // read while the operator is choosing.
        static constexpr auto MovedOrFinished = [] [[nodiscard]] (HWND window, UINT message, WPARAM w, LPARAM l) noexcept -> LRESULT {
            static constexpr auto HighlightOf = [] [[nodiscard]] (HWND crosshair) noexcept -> HWND { return reinterpret_cast<HWND>(::GetWindowLongPtrW(crosshair, kHighlight)); };

            static constexpr auto HideOutline = [](HWND outline) noexcept -> void { (void)::ShowWindow(outline, SW_HIDE); };

            static constexpr auto MovedDrag = [] [[nodiscard]] (HWND window) noexcept -> LRESULT {
                // Dragging over one of our own windows, or over the desktop, picks nothing, which is how a window is let
                // go of again.
                static constexpr auto PickUnderCursor = [](HWND crosshair) noexcept -> void {
                    POINT cursor{}; // WAIVER(R2): the answer of one query, read once after it.
                    if (::GetCursorPos(&cursor) == FALSE)
                        return;
                    KeepAt(crosshair, kPointingAt, WindowUnder(cursor.x, cursor.y));
                };

                // Around whatever the pointer is over, and nowhere at all when that is our own windows or the desktop.
                static constexpr auto OutlineAround = [](HWND outline, const std::optional<interior::MonitorHandle>& picked) noexcept -> void {
                    static constexpr auto ShowOutlineOn = [](HWND outline, const interior::ScreenRect& bounds) noexcept -> void {
                        const int width = bounds.Right().Get() - bounds.Left().Get();
                        const int height = bounds.Bottom().Get() - bounds.Top().Get();
                        (void)::SetWindowPos(outline, HWND_TOPMOST, bounds.Left().Get(), bounds.Top().Get(), width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);
                    };

                    static constexpr auto BoundsOfPicked = [] [[nodiscard]] (const std::optional<interior::MonitorHandle>& picked) noexcept -> std::optional<interior::ScreenRect> {
                        if (!picked.has_value())
                            return std::nullopt;
                        return BoundsOfWindow(*picked);
                    };
                    const std::optional<interior::ScreenRect> bounds = BoundsOfPicked(picked);
                    if (bounds.has_value())
                        ShowOutlineOn(outline, *bounds);
                    else
                        HideOutline(outline);
                };
                if (::GetCapture() != window)
                    return 0;
                (void)::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
                PickUnderCursor(window);
                OutlineAround(HighlightOf(window), HandleAt(window, kPointingAt));
                return 0;
            };

            // The pick is taken when the button comes up, and not before. Every window the pointer crosses on its
            // way, and the desktop it pauses over, would otherwise each be a session built and thrown away.
            static constexpr auto FinishedDrag = [] [[nodiscard]] (HWND window, UINT message, WPARAM w, LPARAM l) noexcept -> LRESULT {
                // A drag that ends with something else taking the capture chooses nothing, and the label says so again.
                static constexpr auto AbandonedDrag = [] [[nodiscard]] (HWND window, UINT message, WPARAM w, LPARAM l) noexcept -> LRESULT {
                    if (message != WM_CAPTURECHANGED)
                        return ::DefWindowProcW(window, message, w, l);
                    KeepAt(window, kPointingAt, HandleAt(window, kChosen));
                    HideOutline(HighlightOf(window));
                    return 0;
                };
                if (message != WM_LBUTTONUP)
                    return AbandonedDrag(window, message, w, l);
                KeepAt(window, kChosen, HandleAt(window, kPointingAt));
                HideOutline(HighlightOf(window));
                (void)::ReleaseCapture();
                return 0;
            };
            if (message == WM_MOUSEMOVE)
                return MovedDrag(window);
            return FinishedDrag(window, message, w, l);
        };

        static constexpr auto StartedDrag = [] [[nodiscard]] (HWND window) noexcept -> LRESULT {
            (void)::SetCapture(window);
            (void)::SetCursor(::LoadCursorW(nullptr, IDC_CROSS));
            return 0;
        };
        if (message == WM_LBUTTONDOWN)
            return StartedDrag(window);
        return MovedOrFinished(window, message, w, l);
    };
    if (message == WM_PAINT)
        return PaintedCrosshair(window);
    return DraggedCrosshair(window, message, w, l);
}

// The outline drawn around the window under the pointer. Only its border is painted: the middle is
// filled with a colour the window is told to treat as nothing, so what is inside stays visible.
constexpr COLORREF kOutlineHollow = RGB(0, 0, 1);
constexpr COLORREF kOutlineEdge = RGB(0, 160, 255);
constexpr int kOutlineEdgeWidth = 4;
constexpr DWORD kOutlineStyle = WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST;

// WAIVER(R17): the window procedure is called by the OS, which discards nothing and ignores attributes.
LRESULT CALLBACK HighlightProc(HWND window, UINT message, WPARAM w, LPARAM l) noexcept
{
    static constexpr auto PaintOutline = [](HWND window) noexcept -> void {
        static constexpr auto DrawOutline = [](HDC dc, const RECT& box) noexcept -> void {
            static constexpr auto FilledWith = [](HDC dc, const RECT& box, COLORREF colour) noexcept -> void {
                const HBRUSH brush = ::CreateSolidBrush(colour);
                ::FillRect(dc, &box, brush);
                ENSURE(::DeleteObject(brush) != FALSE);
            };
            const RECT inside{ box.left + kOutlineEdgeWidth, box.top + kOutlineEdgeWidth, box.right - kOutlineEdgeWidth, box.bottom - kOutlineEdgeWidth };
            FilledWith(dc, box, kOutlineEdge);
            FilledWith(dc, inside, kOutlineHollow);
        };
        PAINTSTRUCT paint{}; // WAIVER(R2): the record the OS fills to hand over the device context.
        RECT box{};
        (void)::GetClientRect(window, &box);
        DrawOutline(::BeginPaint(window, &paint), box);
        (void)::EndPaint(window, &paint);
    };
    if (message != WM_PAINT)
        return ::DefWindowProcW(window, message, w, l);
    PaintOutline(window);
    return 0;
}

// --- the pages -------------------------------------------------------------------------------------------

[[nodiscard]] Page ChosenPage(const ControlPanel& panel) noexcept
{
    const int selected = static_cast<int>(::SendMessageW(panel.tabs, TCM_GETCURSEL, 0, 0));
    return static_cast<Page>(std::clamp<std::size_t>(static_cast<std::size_t>(std::max(selected, 0)), 0, static_cast<std::size_t>(Page::Count) - 1));
}

[[nodiscard]] std::array<HWND, 6> ControlsOfField(const ControlPanel& panel, std::size_t f) noexcept
{
    return { panel.labels[f], panel.sliders[f], panel.boxes[f], panel.spins[f], panel.resets[f], panel.warnings[f] };
}

[[nodiscard]] int HowOf(bool visible) noexcept
{
    return visible ? SW_SHOW : SW_HIDE;
}

// WAIVER(R7): showing the pages and naming the tabs both walk the page table; what they do with it differs.
void ShowOnly(const ControlPanel& panel, Page chosen) noexcept
{
    static constexpr auto ShowPage = [](const ControlPanel& panel, Page page, bool visible) noexcept -> void {
        // A frame is shown with the rows inside it; a break has nothing to show.
        static constexpr auto ShowRowOrFrame = [](const ControlPanel& panel, const RowSpec& row, bool visible) noexcept -> void {
            static constexpr auto ShowRow = [](const ControlPanel& panel, const RowSpec& row, bool visible) noexcept -> void {
                static constexpr auto ShowAll = [](std::span<const HWND> controls, int how) noexcept -> void {
                    std::ranges::for_each(controls, [how](HWND control) { (void)::ShowWindow(control, how); });
                };

                static constexpr auto ShowNumberOrSwitch = [](const ControlPanel& panel, const RowSpec& row, int how) noexcept -> void {
                    static constexpr auto ControlsOfToggle = [] [[nodiscard]] (const ControlPanel& panel, std::size_t t) noexcept -> std::array<HWND, 3> {
                        return { panel.toggles[t], panel.toggleResets[t], panel.toggleWarnings[t] };
                    };
                    if (row.kind == Kind::Field)
                        ShowAll(ControlsOfField(panel, row.index), how);
                    else
                        ShowAll(ControlsOfToggle(panel, row.index), how);
                };

                // A number and a switch each stand on a row of their own; a group and a box are laid out differently.
                static constexpr auto StandsAlone = [] [[nodiscard]] (Kind kind) noexcept -> bool { return kind == Kind::Field || kind == Kind::Toggle; };

                static constexpr auto ShowChoicesOrText = [](const ControlPanel& panel, const RowSpec& row, int how) noexcept -> void {
                    static constexpr auto ShowList = [](const ControlPanel& panel, std::size_t list, int how) noexcept -> void {
                        (void)::ShowWindow(panel.listLabels[list], how);
                        ShowAll(ChoicesOfList(panel, static_cast<List>(list)), how);
                    };

                    static constexpr auto ShowGroupOrText = [](const ControlPanel& panel, const RowSpec& row, int how) noexcept -> void {
                        static constexpr auto ControlsOfPick = [] [[nodiscard]] (const ControlPanel& panel, std::size_t t) noexcept -> std::array<HWND, 4> {
                            return { panel.pickLabels[t], panel.crosshairs[t], panel.pickNames[t], panel.pickResets[t] };
                        };

                        // A group's glyph exists only when its warning applies, so showing nothing is showing it.
                        static constexpr auto ShowGroup = [](const ControlPanel& panel, std::size_t group, int how) noexcept -> void {
                            (void)::ShowWindow(panel.groupLabels[group], how);
                            (void)::ShowWindow(panel.groupWarnings[group], how);
                            ShowAll(ChoicesOf(panel, static_cast<Group>(group)), how);
                        };
                        if (row.kind == Kind::Group)
                            ShowGroup(panel, row.index, how);
                        else
                            ShowAll(ControlsOfPick(panel, row.index), how);
                    };
                    if (row.kind == Kind::List)
                        ShowList(panel, row.index, how);
                    else
                        ShowGroupOrText(panel, row, how);
                };
                if (StandsAlone(row.kind))
                    ShowNumberOrSwitch(panel, row, HowOf(visible));
                else
                    ShowChoicesOrText(panel, row, HowOf(visible));
            };

            static constexpr auto ShowFrame = [](const ControlPanel& panel, std::size_t frame, bool visible) noexcept -> void {
                (void)::ShowWindow(panel.frames[frame], HowOf(visible));
                std::ranges::for_each(InnerRows(frame), [&panel, visible](const RowSpec& inner) { ShowRow(panel, inner, visible); });
            };
            if (row.kind == Kind::Break)
                return;
            if (row.kind == Kind::Note)
                (void)::ShowWindow(panel.notes[row.index], HowOf(visible));
            else if (row.kind == Kind::Frame)
                ShowFrame(panel, row.index, visible);
            else
                ShowRow(panel, row, visible);
        };
        std::ranges::for_each(RowsOf(page), [&panel, visible](const RowSpec& row) { ShowRowOrFrame(panel, row, visible); });
    };
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, static_cast<std::size_t>(Page::Count)),
                          [&panel, chosen](std::size_t p) { ShowPage(panel, static_cast<Page>(p), static_cast<Page>(p) == chosen); });
}

// --- turning the controls back into settings -------------------------------------------------------------

[[nodiscard]] interior::LiveSettings LiveOf(const ControlPanel& panel, const interior::LiveSettings& current) noexcept
{
    static constexpr auto TuningOf = [] [[nodiscard]] (const ControlPanel& panel, const interior::NrTuning& current) noexcept -> interior::NrTuning {
        static constexpr auto StyleFrom = [] [[nodiscard]] (std::size_t code) noexcept -> interior::NrStyle {
            constexpr std::array<interior::NrStyle, 3> styles{ interior::NrStyle::Standard, interior::NrStyle::Natural, interior::NrStyle::Cinematic };
            return styles[std::min(code, styles.size() - 1)];
        };

        // -1 is the model's own way of saying "whatever local structure got", and nothing between it and 0 means
        // anything, so the switch carries that value and the number carries the rest.
        static constexpr auto SkinOf = [] [[nodiscard]] (const ControlPanel& panel, interior::SkinStrength held) noexcept -> interior::SkinStrength {
            if (IsOn(panel, Toggle::SkinFollowsStructure))
                return interior::SkinStrengthTag::Parse(-1.0f).value_or(held);
            return interior::SkinStrengthTag::Parse(SettledValue(panel, Field::Skin)).value_or(held);
        };

        // The model names its own presets or none at all; with none the row is absent and the session keeps what
        // it started with.
        static constexpr auto PresetOf = [] [[nodiscard]] (const ControlPanel& panel, interior::NgxPreset held) noexcept -> interior::NgxPreset {
            const std::size_t chosen = ChosenInList(panel, List::Preset, held.Get());
            return interior::NgxPresetTag::Parse(static_cast<std::uint32_t>(chosen)).value_or(held);
        };
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
    };
    static constexpr auto PassesOf = [] [[nodiscard]] (const ControlPanel& panel, interior::PassCount held) noexcept -> interior::PassCount {
        return interior::PassCountTag::Parse(static_cast<std::uint32_t>(std::lround(SettledValue(panel, Field::Passes)))).value_or(held);
    };
    const auto scale = [&panel](Field field, interior::MotionScale held) { return interior::MotionScaleTag::Parse(SettledValue(panel, field)).value_or(held); };
    return interior::LiveSettings{ IsOn(panel, Toggle::NeuralRendering),
                                   TuningOf(panel, current.tuning),
                                   PassesOf(panel, current.passes),
                                   IsOn(panel, Toggle::DepthInverted),
                                   scale(Field::MvScaleX, current.mvScaleX),
                                   scale(Field::MvScaleY, current.mvScaleY),
                                   IsOn(panel, Toggle::Vsync),
                                   interior::FractionTag::Parse(SettledValue(panel, Field::ResetThreshold)).value_or(current.resetThreshold),
                                   interior::DepthValueTag::Parse(SettledValue(panel, Field::DepthValue)).value_or(current.depth) };
}

// --- the resets --------------------------------------------------------------------------------------

void ShowPickedName(const ControlPanel& panel, std::size_t pick) noexcept
{
    // What the crosshair is currently pointing at, which during a drag is whatever is under the pointer.
    static constexpr auto TitlePicked = [] [[nodiscard]] (const std::optional<interior::MonitorHandle>& picked) noexcept -> interior::WindowTitle {
        return picked.has_value() ? TitleOfWindow(*picked) : interior::WindowTitle{};
    };
    const interior::WindowTitle title = TitlePicked(HandleAt(panel.crosshairs[pick], kPointingAt));
    WriteText(panel.pickNames[pick], title.IsEmpty() ? kPicks[pick].nothing : title.CString());
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

// Each page's arguments are a list of pieces folded onto what came before, so a builder is one list.
[[nodiscard]] Arguments JoinedAll(const Arguments& so, std::span<const Piece> pieces) noexcept
{
    static constexpr auto Joined = [] [[nodiscard]] (const Arguments& so, const Piece& next) noexcept -> Arguments {
        if (next.IsEmpty())
            return so;
        return Arguments::Parse(infra::Formatted<kArgumentCapacity>("{} {}", so.Get(), next.Get()).Get()).value_or(so);
    };
    return std::ranges::fold_left(pieces, so, Joined);
}

[[nodiscard]] Piece Choice(const ControlPanel& panel, Group group, std::string_view name, std::span<const char* const> words) noexcept
{
    return Trimmed(infra::Formatted<kPieceCapacity>("--{}={}", name, words[std::min(ChosenIn(panel, group, 0), words.size() - 1)]).Get());
}

[[nodiscard]] std::string_view Word(bool on) noexcept
{
    return on ? "on" : "off";
}

[[nodiscard]] Piece Switch(const ControlPanel& panel, Toggle toggle, std::string_view name) noexcept
{
    return Trimmed(infra::Formatted<kPieceCapacity>("--{}={}", name, Word(IsOn(panel, toggle))).Get());
}

[[nodiscard]] Piece Counted(std::string_view name, std::uint32_t value) noexcept
{
    return Trimmed(infra::Formatted<kPieceCapacity>("--{}={}", name, value).Get());
}

[[nodiscard]] Arguments StartupArguments(const ControlPanel& panel, const Arguments& so) noexcept
{
    static constexpr auto Whole = [] [[nodiscard]] (const ControlPanel& panel, Field field, std::string_view name) noexcept -> Piece {
        static constexpr auto WholeOf = [] [[nodiscard]] (const ControlPanel& panel, Field field) noexcept -> int { return static_cast<int>(std::lround(SettledValue(panel, field))); };
        return Trimmed(infra::Formatted<kPieceCapacity>("--{}={}", name, WholeOf(panel, field)).Get());
    };

    // The first entries of the source list are the two answers that name no monitor; the rest are the monitors
    // in the order the session found them.
    static constexpr auto SourcePiece = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> Piece {
        constexpr std::array<const char*, 2> kinds{ "primary", "all" };
        const std::size_t chosen = ChosenInList(panel, List::Source, 0);
        if (chosen < kinds.size())
            return Trimmed(infra::Formatted<kPieceCapacity>("--monitor={}", kinds[chosen]).Get());
        return Counted("monitor", static_cast<std::uint32_t>(chosen - kinds.size()));
    };

    // Leaving the first entry chosen says nothing, which is what "no target of its own" and "whichever adapter
    // the search finds" mean on the command line.
    static constexpr auto FromListPiece = [] [[nodiscard]] (const ControlPanel& panel, List list, std::string_view name) noexcept -> Piece {
        const std::size_t chosen = ChosenInList(panel, list, 0);
        if (chosen == 0)
            return Piece{};
        return Counted(name, static_cast<std::uint32_t>(chosen - 1));
    };

    // A handle rather than a title: the panel has the window itself, and a title is not a name for anything.
    static constexpr auto WindowPiece = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> Piece {
        const std::optional<interior::MonitorHandle> picked = PickedWindow(panel);
        if (!picked.has_value())
            return Piece{};
        return Trimmed(infra::Formatted<kPieceCapacity>("--window=0x{:x}", picked->Get()).Get());
    };
    constexpr std::array<const char*, 2> formats{ "rgba8", "rgba16f" };
    constexpr std::array<const char*, 3> sr{ "auto", "dlaa", "off" };
    constexpr std::array<const char*, 3> motion{ "builtin", "nvof", "none" };
    const std::array<Piece, 9> pieces{ WindowPiece(panel),
                                       SourcePiece(panel),
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
    const std::array<Piece, 6> pieces{
        Choice(panel, Group::NvofGrid, "nvof-grid", grids), Choice(panel, Group::NvofPerf, "nvof-perf", effort), Switch(panel, Toggle::RedirectionBitmap, "redirection-bitmap"),
        Switch(panel, Toggle::DebugLayer, "debug-layer"),   Switch(panel, Toggle::Indicator, "indicator"),       Switch(panel, Toggle::CubinCache, "cubin-cache")
    };
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

// The notice sits under everything, so opening it grows the window rather than moving the controls.
struct Notice
{
    HWND line;
    HWND expander;
    HWND body;
};

[[nodiscard]] bool IsPresent(HWND control) noexcept
{
    return control != nullptr;
}

[[nodiscard]] bool AllPresent(std::span<const HWND> controls) noexcept
{
    return std::ranges::all_of(controls, IsPresent);
}

[[nodiscard]] bool ResetsAsSpecified(const ControlPanel& panel) noexcept
{
    // A switch's reset is there exactly when its spec asks for one, so both a missing and a spare one is a fault.
    static constexpr auto ResetAsSpecified = [] [[nodiscard]] (const ControlPanel& panel, std::size_t toggle) noexcept -> bool {
        return kToggles[toggle].resettable == IsPresent(panel.toggleResets[toggle]);
    };
    return std::ranges::all_of(std::views::iota(std::size_t{ 0 }, kToggleCount), [&panel](std::size_t t) { return ResetAsSpecified(panel, t); });
}

constexpr wchar_t kNoOpticalFlow[] = L"Where the model's motion comes from: matching blocks between frames, or nothing at all. Optical flow is not offered: this build has no NVIDIA "
                                     L"Optical Flow backend (configure with -DDSCREEN_ENABLE_NVOF=ON).";
constexpr wchar_t kNoOpticalFlowEngine[] = L"Not offered: this build has no NVIDIA Optical Flow backend, so this changes nothing.";
// The Motion group lists its choices in the order of the MotionBackend enumeration, which is how the panel reads them back.
constexpr std::size_t kOpticalFlowChoice = static_cast<std::size_t>(interior::MotionBackend::NvOpticalFlow);

} // namespace

Result<ControlPanel, Error> CreateControlPanel(const interior::Options& options, const interior::LiveSettings& live, interior::DisplayMode display, const PanelFindings& findings) noexcept
{
    static constexpr auto ClassDescription = [] [[nodiscard]] () noexcept -> WNDCLASSEXW {
        return WNDCLASSEXW{ .cbSize = sizeof(WNDCLASSEXW),
                            .style = 0,
                            .lpfnWndProc = &PanelProc,
                            .cbClsExtra = 0,
                            .cbWndExtra = 0,
                            .hInstance = ::GetModuleHandleW(nullptr),
                            .hIcon = LargeAppIcon(),
                            .hCursor = ::LoadCursorW(nullptr, IDC_ARROW),
                            .hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE),
                            .lpszMenuName = nullptr,
                            .lpszClassName = kPanelClass,
                            .hIconSm = SmallAppIcon() };
    };

    // Advisory: the older common controls register their classes as they load and refuse this call, while
    // version 6 needs asking. Either way the controls are checked once built, which is the answer that counts.
    static constexpr auto InitialiseCommonControls = []() noexcept -> void {
        INITCOMMONCONTROLSEX controls{ sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS | ICC_TAB_CLASSES | ICC_LINK_CLASS };
        (void)::InitCommonControlsEx(&controls);
    };

    // WAIVER(R7): a window class is described the same way wherever one is made; every field differs in kind.
    static constexpr auto HighlightDescription = [] [[nodiscard]] () noexcept -> WNDCLASSEXW {
        return WNDCLASSEXW{ .cbSize = sizeof(WNDCLASSEXW),
                            .style = 0,
                            .lpfnWndProc = &HighlightProc,
                            .cbClsExtra = 0,
                            .cbWndExtra = 0,
                            .hInstance = ::GetModuleHandleW(nullptr),
                            .hIcon = nullptr,
                            .hCursor = ::LoadCursorW(nullptr, IDC_CROSS),
                            .hbrBackground = nullptr,
                            .lpszMenuName = nullptr,
                            .lpszClassName = kOutlineClass,
                            .hIconSm = nullptr };
    };

    // WAIVER(R7): two window classes are described the same way; the procedure, the cursor and the name differ.
    static constexpr auto CrosshairDescription = [] [[nodiscard]] () noexcept -> WNDCLASSEXW {
        return WNDCLASSEXW{ .cbSize = sizeof(WNDCLASSEXW),
                            .style = 0,
                            .lpfnWndProc = &CrosshairProc,
                            .cbClsExtra = 0,
                            .cbWndExtra = 2 * sizeof(LONG_PTR),
                            .hInstance = ::GetModuleHandleW(nullptr),
                            .hIcon = nullptr,
                            .hCursor = ::LoadCursorW(nullptr, IDC_CROSS),
                            .hbrBackground = ::GetSysColorBrush(COLOR_BTNFACE),
                            .lpszMenuName = nullptr,
                            .lpszClassName = kCrosshairClass,
                            .hIconSm = nullptr };
    };

    static constexpr auto CreatePanelWindow = [] [[nodiscard]] () noexcept -> Result<UniqueWindow, Error> {
        HWND window = ::CreateWindowExW(WS_EX_TOPMOST, kPanelClass, L"Full-Screen Wrapper for DLSS5 \u2014 controls", kPanelStyle, CW_USEDEFAULT, CW_USEDEFAULT, kPanelWidth, kPanelWidth, nullptr,
                                        nullptr, ::GetModuleHandleW(nullptr), nullptr);
        if (window == nullptr)
            return Fail(LastError(ApiCall::CreateWindowExW));
        return UniqueWindow(window);
    };

    static constexpr auto Populated = [] [[nodiscard]] (UniqueWindow window, const interior::Options& o, const interior::LiveSettings& live, interior::DisplayMode display,
                                                        const PanelFindings& findings) noexcept -> Result<ControlPanel, Error> {
        // The Inert page is there only when it was asked for, so it neither wears a tab nor makes the panel taller.
        // Its rows are still laid out and its controls still built, which is what keeps every control on a page.
        static constexpr auto PagesShown = [] [[nodiscard]] (bool showInert) noexcept -> std::size_t {
            return showInert ? static_cast<std::size_t>(Page::Count) : static_cast<std::size_t>(Page::Inert);
        };

        // The font the rest of Windows writes its dialogs in, asked for at this display's scale. The plain query
        // answers for the primary display, and scaling that answer again is what made the text outgrow its labels.
        static constexpr auto MessageDescription = [] [[nodiscard]] (int dpi) noexcept -> LOGFONTW {
            NONCLIENTMETRICSW metrics{}; // WAIVER(R2): a request record filled once, before it is asked.
            metrics.cbSize = sizeof(NONCLIENTMETRICSW);
            ENSURE(::SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &metrics, 0, static_cast<UINT>(dpi)) != FALSE);
            return metrics.lfMessageFont;
        };

        static constexpr auto MessageFont = [] [[nodiscard]] (int dpi) noexcept -> UniqueFont {
            const LOGFONTW description = MessageDescription(dpi);
            return UniqueFont(::CreateFontIndirectW(&description));
        };

        static constexpr auto MetricsOf = [] [[nodiscard]] (HWND window, HFONT font, const PanelLists& lists, bool showInert) noexcept -> Metrics {
            static constexpr auto LineHeight = [] [[nodiscard]] (HWND window, HFONT font) noexcept -> int {
                static constexpr auto MeasuredOn = [] [[nodiscard]] (HDC dc, HFONT font) noexcept -> int {
                    const HGDIOBJ previous = ::SelectObject(dc, font);
                    TEXTMETRICW text{}; // WAIVER(R2): an answer record filled once by the measurement below.
                    ENSURE(::GetTextMetricsW(dc, &text) != FALSE);
                    (void)::SelectObject(dc, previous);
                    return static_cast<int>(text.tmHeight);
                };
                const HDC dc = ::GetDC(window);
                ENSURE(dc != nullptr);
                const int height = MeasuredOn(dc, font);
                ENSURE(::ReleaseDC(window, dc) == 1);
                return height;
            };

            // The notice is as tall as its own text: how many lines the words wrap into at the panel's width is
            // the display's to answer, not a number kept here beside them.
            static constexpr auto BodyHeightOf = [] [[nodiscard]] (HWND window, HFONT font, int dpi) noexcept -> int {
                static constexpr auto MeasuredOn = [] [[nodiscard]] (HDC dc, HFONT font, int width) noexcept -> int {
                    const HGDIOBJ previous = ::SelectObject(dc, font);
                    RECT box{ 0, 0, width, 0 }; // WAIVER(R2): an answer record filled once by the measurement below.
                    const int height = ::DrawTextW(dc, kNoticeBody, -1, &box, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
                    (void)::SelectObject(dc, previous);
                    return height;
                };
                const HDC dc = ::GetDC(window);
                ENSURE(dc != nullptr);
                const int height = MeasuredOn(dc, font, ::MulDiv(kPanelWidth - 2 * kMargin, dpi, kReferenceDpi));
                ENSURE(::ReleaseDC(window, dc) == 1);
                return height;
            };

            // The shortest column every page shown fits two of. The panel is as tall as that and no taller, so pages
            // left out cost nothing and a page losing rows makes the window shorter.
            static constexpr auto ColumnHeight = [] [[nodiscard]] (const Metrics& m, bool showInert) noexcept -> int {
                // The rows are measured by the metrics alone; the column they are fitted to is the one being tried.
                static constexpr auto Tried = [] [[nodiscard]] (const Metrics& m, int column) noexcept -> Metrics { return Metrics{ m.dpi, m.line, column, m.body, m.lists }; };

                static constexpr auto FitsAt = [] [[nodiscard]] (const Metrics& m, bool showInert) noexcept -> bool {
                    // A row cannot straddle a column, and a break leaves the rest of one empty, so a page can need more room
                    // than its rows alone say. Rather than guess at that from a share of the rows, the layout itself is
                    // asked how many columns it takes.
                    static constexpr auto ColumnsNeeded = [] [[nodiscard]] (Page page, const Metrics& m) noexcept -> std::size_t {
                        const auto step = [&m](const Cell& at, const RowSpec& row) { return Landed(at, row, m).next; };
                        return std::ranges::fold_left(RowsOf(page), Cell{ 0, 0 }, step).column + 1;
                    };
                    const auto pages = std::views::iota(std::size_t{ 0 }, PagesShown(showInert));
                    return std::ranges::all_of(pages, [&m](std::size_t page) { return ColumnsNeeded(static_cast<Page>(page), m) <= static_cast<std::size_t>(kColumns); });
                };
                const int least = kMinRowsPerColumn * m.RowHeight();
                const int most = kMaxRowsPerColumn * m.RowHeight();
                const auto heights = std::views::iota(least, most + 1);
                const auto found = std::ranges::find_if(heights, [&m, showInert](int column) { return FitsAt(Tried(m, column), showInert); });
                return found == heights.end() ? most : *found;
            };
            const int dpi = static_cast<int>(::GetDpiForWindow(window));
            // Measured with no column at all, which is what the rows are measured by before the column is chosen.
            const Metrics unfitted{ dpi, LineHeight(window, font), 0, BodyHeightOf(window, font, dpi), &lists };
            return Metrics{ dpi, unfitted.line, ColumnHeight(unfitted, showInert), unfitted.body, &lists };
        };

        static constexpr auto ResizeToFit = [](HWND window, const Metrics& m) noexcept -> void {
            RECT frame{ 0, 0, m.Of(kPanelWidth), m.PageTop() + m.PageHeight() };
            ENSURE(::AdjustWindowRectExForDpi(&frame, kPanelStyle, FALSE, 0, static_cast<UINT>(m.dpi)) != FALSE);
            ENSURE(::SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
        };

        static constexpr auto Assembled = [] [[nodiscard]] (UniqueWindow window, UniqueFont font, const Metrics& m, const interior::Options& o, const interior::LiveSettings& live,
                                                            interior::DisplayMode display, const PanelFindings& findings) noexcept -> ControlPanel {
            static constexpr auto IconFont = [] [[nodiscard]] (int dpi) noexcept -> UniqueFont {
                static constexpr auto FaceOf = [] [[nodiscard]] (int height, LONG weight, const wchar_t* name) noexcept -> LOGFONTW {
                    LOGFONTW description{ .lfHeight = height,
                                          .lfWidth = 0,
                                          .lfEscapement = 0,
                                          .lfOrientation = 0,
                                          .lfWeight = weight,
                                          .lfItalic = FALSE,
                                          .lfUnderline = FALSE,
                                          .lfStrikeOut = FALSE,
                                          .lfCharSet = DEFAULT_CHARSET,
                                          .lfOutPrecision = OUT_DEFAULT_PRECIS,
                                          .lfClipPrecision = CLIP_DEFAULT_PRECIS,
                                          .lfQuality = DEFAULT_QUALITY,
                                          .lfPitchAndFamily = DEFAULT_PITCH | FF_DONTCARE,
                                          .lfFaceName = {} };
                    ENSURE(::wcscpy_s(description.lfFaceName, name) == 0);
                    return description;
                };
                const LOGFONTW description = FaceOf(-::MulDiv(11, dpi, kReferenceDpi), FW_NORMAL, L"Segoe MDL2 Assets");
                return UniqueFont(::CreateFontIndirectW(&description));
            };

            static constexpr auto BoldFont = [] [[nodiscard]] (int dpi) noexcept -> UniqueFont {
                LOGFONTW description = MessageDescription(dpi); // WAIVER(R2): a request record, weighted once before it is asked.
                description.lfWeight = FW_BOLD;
                return UniqueFont(::CreateFontIndirectW(&description));
            };

            // A tooltip only wraps its text, and only honours a line break in it, once told how wide it may be.
            static constexpr auto CreateTooltip = [] [[nodiscard]] (HWND parent, const Metrics& m) noexcept -> HWND {
                static constexpr auto Wrapped = [] [[nodiscard]] (HWND tooltip, int width) noexcept -> HWND {
                    if (tooltip != nullptr)
                        (void)::SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, width);
                    return tooltip;
                };
                return Wrapped(::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent,
                                                 nullptr, ::GetModuleHandleW(nullptr), nullptr),
                               m.Of(kTipWidth));
            };

            static constexpr auto CreateChild = [] [[nodiscard]] (HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, DWORD extended, RECT bounds) noexcept -> HWND {
                return ::CreateWindowExW(extended, className, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style, bounds.left, bounds.top, bounds.right, bounds.bottom, parent, nullptr,
                                         ::GetModuleHandleW(nullptr), nullptr);
            };

            // Horizontal places are given in reference pixels and scaled; vertical ones are already in the display's
            // dots, because they follow the text.
            static constexpr auto Bounds = [] [[nodiscard]] (const Metrics& m, int x, int top, int width, int height) noexcept -> RECT { return RECT{ m.Of(x), top, m.Of(width), height }; };

            static constexpr auto BuildAll = [] [[nodiscard]] (HWND parent, const Metrics& m, const interior::Options& o, const interior::LiveSettings& live, interior::DisplayMode display,
                                                               const PanelFindings& findings) noexcept -> Built {
                static constexpr auto CreateLabel = [] [[nodiscard]] (HWND parent, const Metrics& m, const wchar_t* text, int x, int top, int width) noexcept -> HWND {
                    return CreateChild(parent, WC_STATICW, text, SS_LEFT, 0, Bounds(m, x, top, width, m.LabelHeight()));
                };

                static constexpr auto CreateButton = [] [[nodiscard]] (HWND parent, const Metrics& m, const wchar_t* text, DWORD style, int x, int top, int width) noexcept -> HWND {
                    return CreateChild(parent, WC_BUTTONW, text, style, 0, Bounds(m, x, top, width, m.ControlHeight()));
                };

                static constexpr auto StartingChoices = [] [[nodiscard]] (const interior::Options& o, interior::DisplayMode display) noexcept -> std::array<std::size_t, kGroupCount> {
                    static constexpr auto CodeOfGrid = [] [[nodiscard]] (interior::GridSize grid) noexcept -> std::size_t {
                        constexpr std::array<interior::GridSize, 3> grids{ interior::GridSize::One, interior::GridSize::Two, interior::GridSize::Four };
                        const auto found = std::ranges::find(grids, grid);
                        return found == grids.end() ? 0 : static_cast<std::size_t>(std::ranges::distance(grids.begin(), found));
                    };
                    return { ChoiceOfDisplay(display),
                             interior::StyleCode(o.tuning.style),
                             static_cast<std::size_t>(o.cursor),
                             static_cast<std::size_t>(o.motion),
                             CodeOfGrid(o.nvofGrid),
                             static_cast<std::size_t>(o.nvofPerf),
                             static_cast<std::size_t>(o.sr),
                             static_cast<std::size_t>(o.format),
                             static_cast<std::size_t>(o.logLevel) };
                };

                static constexpr auto PlaceOfRow = [] [[nodiscard]] (Kind kind, std::size_t index, const Metrics& m) noexcept -> Placement {
                    // Where a control goes: its page decides which rows exist, and what stands above it decides the row.
                    static constexpr auto PlacementFor = [] [[nodiscard]] (Kind kind, std::size_t index, const Metrics& m) noexcept -> std::optional<Placement> {
                        static constexpr auto PlacementOn = [] [[nodiscard]] (Page page, Kind kind, std::size_t index, const Metrics& m) noexcept -> std::optional<Placement> {
                            const auto step = [&](const Walk& so, const RowSpec& row) { return Stepped(so, row, kind, index, m); };
                            return std::ranges::fold_left(RowsOf(page), Walk{ Cell{ 0, 0 }, std::nullopt }, step).found;
                        };
                        const auto pages = std::views::iota(std::size_t{ 0 }, static_cast<std::size_t>(Page::Count));
                        const auto first = [&](const std::optional<Placement>& so, std::size_t page) { return so.has_value() ? so : PlacementOn(static_cast<Page>(page), kind, index, m); };
                        return std::ranges::fold_left(pages, std::optional<Placement>{}, first);
                    };
                    const std::optional<Placement> at = PlacementFor(kind, index, m);
                    ENSURE(at.has_value());
                    return *at;
                };

                static constexpr auto BuildFields = [] [[nodiscard]] (HWND parent, const Metrics& m, const std::array<float, kFieldCount>& values, Built built) noexcept -> Built {
                    // A number with a warning wears the glyph in front of its slider, which starts after it and ends where it always did.
                    static constexpr auto SliderInset = [] [[nodiscard]] (const FieldSpec& spec) noexcept -> int { return spec.warning == nullptr ? 0 : kWarningWidth; };

                    static constexpr auto CreateSlider = [] [[nodiscard]] (HWND parent, const Metrics& m, const FieldSpec& spec, const Placement& at, int steps) noexcept -> HWND {
                        const HWND slider = CreateChild(parent, TRACKBAR_CLASSW, nullptr, TBS_HORZ | TBS_NOTICKS, 0,
                                                        Bounds(m, at.left + SliderInset(spec), at.control, kSliderWidth - SliderInset(spec), m.ControlHeight()));
                        if (slider == nullptr)
                            return nullptr;
                        (void)::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(spec.minimum, spec.maximum));
                        (void)::SendMessageW(slider, TBM_SETPOS, TRUE, steps);
                        return Stepped(slider, spec);
                    };

                    // A plain number box: the up-down takes the edit control as its buddy, which puts it at the box's right-hand
                    // end and sizes it. It is not asked to write the box, because it can only write whole numbers.
                    static constexpr auto CreateSpin = [] [[nodiscard]] (HWND parent, HWND box, std::size_t field, const FieldSpec& spec, int steps) noexcept -> HWND {
                        static constexpr auto ArrangedSpin = [] [[nodiscard]] (HWND spin, HWND box, const FieldSpec& spec, int steps) noexcept -> HWND {
                            // One click asks for one, and holding an arrow asks for five at a time; what one of them is worth is the
                            // field's own increment, applied where the arrows are answered.
                            static constexpr auto AccelerateSpin = [](HWND spin, const FieldSpec&) noexcept -> void {
                                std::array<UDACCEL, 2> curve{ { { 0, 1 }, { 2, 5 } } };
                                (void)::SendMessageW(spin, UDM_SETACCEL, curve.size(), reinterpret_cast<LPARAM>(curve.data()));
                            };
                            (void)::SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(box), 0);
                            (void)::SendMessageW(spin, UDM_SETRANGE32, static_cast<WPARAM>(spec.minimum), static_cast<LPARAM>(spec.ceiling));
                            (void)::SendMessageW(spin, UDM_SETPOS32, 0, steps);
                            AccelerateSpin(spin, spec);
                            return spin;
                        };
                        const HWND spin = ::CreateWindowExW(0, UPDOWN_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, parent,
                                                            nullptr, ::GetModuleHandleW(nullptr), nullptr);
                        if (spin == nullptr)
                            return nullptr;
                        (void)::SetWindowLongPtrW(spin, GWLP_USERDATA, static_cast<LONG_PTR>(field));
                        return ArrangedSpin(spin, box, spec, steps);
                    };
                    const auto steps = [&values](std::size_t f) { return StepsOf(values[f], kFields[f]); };
                    built.labels = infra::Generated<HWND, kFieldCount>([&](std::size_t f) {
                        const Placement at = PlaceOfRow(Kind::Field, f, m);
                        return CreateLabel(parent, m, kFields[f].label, at.left, at.top, at.width);
                    });
                    built.sliders = infra::Generated<HWND, kFieldCount>([&](std::size_t f) { return CreateSlider(parent, m, kFields[f], PlaceOfRow(Kind::Field, f, m), steps(f)); });
                    built.boxes = infra::Generated<HWND, kFieldCount>([&](std::size_t f) {
                        const Placement at = PlaceOfRow(Kind::Field, f, m);
                        return CreateChild(parent, WC_EDITW, L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, Bounds(m, at.left + kBoxOffset, at.control, kBoxWidth, m.ControlHeight()));
                    });
                    built.spins = infra::Generated<HWND, kFieldCount>([&](std::size_t f) { return CreateSpin(parent, built.boxes[f], f, kFields[f], steps(f)); });
                    // A number that wants a word of caution wears a glyph in front of its slider; the hint says what for.
                    static constexpr auto CreateWarning = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t f) noexcept -> HWND {
                        if (kFields[f].warning == nullptr)
                            return nullptr;
                        const Placement at = PlaceOfRow(Kind::Field, f, m);
                        return CreateChild(parent, WC_STATICW, kWarningGlyph, SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY, 0, Bounds(m, at.left, at.control, kWarningWidth - 4, m.ControlHeight()));
                    };
                    built.resets = infra::Generated<HWND, kFieldCount>([&](std::size_t f) {
                        const Placement at = PlaceOfRow(Kind::Field, f, m);
                        return CreateButton(parent, m, kRefreshGlyph, 0, at.left + kResetOffset, at.control, kResetWidth);
                    });
                    built.warnings = infra::Generated<HWND, kFieldCount>([&](std::size_t f) { return CreateWarning(parent, m, f); });
                    return built;
                };

                static constexpr auto BuildToggles = [] [[nodiscard]] (HWND parent, const Metrics& m, const std::array<bool, kToggleCount>& on, const PanelFindings& findings,
                                                                       Built built) noexcept -> Built {
                    // A switch whose warning applies wears the glyph in front of its box, and the box starts after it.
                    static constexpr auto ToggleInset = [] [[nodiscard]] (std::size_t t, const PanelFindings& findings) noexcept -> int { return ToggleWarns(t, findings) ? kWarningWidth : 0; };

                    static constexpr auto CreateToggle = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t toggle, bool on, const PanelFindings& findings) noexcept -> HWND {
                        const Placement at = PlaceOfRow(Kind::Toggle, toggle, m);
                        const HWND check = CreateButton(parent, m, kToggles[toggle].label, BS_AUTOCHECKBOX, at.left + ToggleInset(toggle, findings), at.control,
                                                        kResetOffset - kMargin - ToggleInset(toggle, findings));
                        if (check != nullptr)
                            SetChecked(check, on);
                        return check;
                    };

                    static constexpr auto CreateToggleWarning = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t t, const PanelFindings& findings) noexcept -> HWND {
                        if (!ToggleWarns(t, findings))
                            return nullptr;
                        const Placement at = PlaceOfRow(Kind::Toggle, t, m);
                        return CreateChild(parent, WC_STATICW, kWarningGlyph, SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY, 0, Bounds(m, at.left, at.control, kWarningWidth - 4, m.ControlHeight()));
                    };

                    // A switch is its own answer, so only one whose default is not obvious from looking at it gets a reset.
                    static constexpr auto CreateToggleReset = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t toggle) noexcept -> HWND {
                        if (!kToggles[toggle].resettable)
                            return nullptr;
                        const Placement at = PlaceOfRow(Kind::Toggle, toggle, m);
                        return CreateButton(parent, m, kRefreshGlyph, 0, at.left + kResetOffset, at.control, kResetWidth);
                    };
                    built.toggles = infra::Generated<HWND, kToggleCount>([&](std::size_t t) { return CreateToggle(parent, m, t, on[t], findings); });
                    built.toggleResets = infra::Generated<HWND, kToggleCount>([&](std::size_t t) { return CreateToggleReset(parent, m, t); });
                    built.toggleWarnings = infra::Generated<HWND, kToggleCount>([&](std::size_t t) { return CreateToggleWarning(parent, m, t, findings); });
                    return built;
                };

                static constexpr auto BuildGroups = [] [[nodiscard]] (HWND parent, const Metrics& m, const std::array<std::size_t, kGroupCount>& chosen, const PanelFindings& findings,
                                                                      Built built) noexcept -> Built {
                    static constexpr auto CreateChoices = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t group, std::size_t chosen) noexcept -> std::array<HWND, kMaxChoices> {
                        // Four choices at the usual width run past the column, and past the window from the right-hand one.
                        static constexpr auto WidthOfChoice = [] [[nodiscard]] (const Placement& at, std::size_t count) noexcept -> int {
                            return std::min(kChoiceWidth, at.width / static_cast<int>(count));
                        };
                        const Placement at = PlaceOfRow(Kind::Group, group, m);
                        const int width = WidthOfChoice(at, kGroups[group].count);
                        return infra::Generated<HWND, kMaxChoices>([&](std::size_t i) -> HWND {
                            if (i >= kGroups[group].count)
                                return nullptr;
                            const DWORD style = BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0u);
                            const HWND choice = CreateButton(parent, m, kGroups[group].choices[i], style, at.left + static_cast<int>(i) * width, at.control, width);
                            if (choice != nullptr)
                                SetChecked(choice, i == chosen);
                            return choice;
                        });
                    };
                    // A group whose warning applies wears the glyph in front of its label, and the label starts after it; one
                    // whose warning does not apply has no glyph, and its label starts where every other label does.
                    static constexpr auto GroupInset = [] [[nodiscard]] (std::size_t g, const PanelFindings& findings) noexcept -> int { return GroupWarns(g, findings) ? kWarningWidth : 0; };

                    static constexpr auto CreateGroupWarning = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t g, const PanelFindings& findings) noexcept -> HWND {
                        if (!GroupWarns(g, findings))
                            return nullptr;
                        const Placement at = PlaceOfRow(Kind::Group, g, m);
                        return CreateChild(parent, WC_STATICW, kWarningGlyph, SS_CENTER | SS_CENTERIMAGE | SS_NOTIFY, 0, Bounds(m, at.left, at.top, kWarningWidth - 4, m.LabelHeight()));
                    };
                    built.groupLabels = infra::Generated<HWND, kGroupCount>([&](std::size_t g) {
                        const Placement at = PlaceOfRow(Kind::Group, g, m);
                        return CreateLabel(parent, m, kGroups[g].label, at.left + GroupInset(g, findings), at.top, at.width - GroupInset(g, findings));
                    });
                    built.groupWarnings = infra::Generated<HWND, kGroupCount>([&](std::size_t g) { return CreateGroupWarning(parent, m, g, findings); });
                    built.choices = infra::Generated<std::array<HWND, kMaxChoices>, kGroupCount>([&](std::size_t g) { return CreateChoices(parent, m, g, chosen[g]); });
                    return built;
                };

                // WAIVER(R7): a label and a row of buttons is what both a fixed group and a runtime list look like; which
                // table the names come from, and whether a row exists at all, is what differs.
                static constexpr auto BuildLists = [] [[nodiscard]] (HWND parent, const Metrics& m, Built built) noexcept -> Built {
                    static constexpr auto CreateListChoice = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t list, std::size_t index) noexcept -> HWND {
                        static constexpr auto NamedChoice = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t list, std::size_t index) noexcept -> HWND {
                            static constexpr auto Chosen = [] [[nodiscard]] (HWND choice, bool chosen) noexcept -> HWND {
                                if (choice != nullptr)
                                    SetChecked(choice, chosen);
                                return choice;
                            };

                            // The choices of a runtime list run two to a line, wrapping down the row as far as the list is long.
                            static constexpr auto PlaceOfChoice = [] [[nodiscard]] (const Placement& row, std::size_t index, const Metrics& m) noexcept -> Placement {
                                const int line = static_cast<int>(index / kListPerLine);
                                const int across = static_cast<int>(index % kListPerLine);
                                return Placement{ row.left + across * kListChoiceWidth, row.top, row.control + line * m.ControlHeight(), row.width };
                            };
                            const PanelList& entries = (*m.lists)[list];
                            const Placement at = PlaceOfChoice(PlaceOfRow(Kind::List, list, m), index, m);
                            const HWND choice = CreateButton(parent, m, entries.choices.At(index).CString(), BS_AUTORADIOBUTTON | (index == 0 ? WS_GROUP : 0u), at.left, at.control, kListChoiceWidth);
                            return Chosen(choice, index == entries.chosen);
                        };
                        if (index >= (*m.lists)[list].choices.Size())
                            return nullptr;
                        return NamedChoice(parent, m, list, index);
                    };

                    static constexpr auto CreateListLabel = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t list) noexcept -> HWND {
                        if ((*m.lists)[list].choices.IsEmpty())
                            return nullptr;
                        const Placement at = PlaceOfRow(Kind::List, list, m);
                        return CreateLabel(parent, m, kLists[list].label, at.left, at.top, at.width);
                    };
                    built.listLabels = infra::Generated<HWND, kListCount>([&](std::size_t l) { return CreateListLabel(parent, m, l); });
                    built.listChoices = infra::Generated<std::array<HWND, kMaxListChoices>, kListCount>(
                        [&](std::size_t l) { return infra::Generated<HWND, kMaxListChoices>([&](std::size_t i) { return CreateListChoice(parent, m, l, i); }); });
                    return built;
                };

                // WAIVER(R7): the label and the control of a row are built the same way whatever the row holds; what each
                // of these makes, and from which table, is what differs.
                static constexpr auto BuildPicks = [] [[nodiscard]] (HWND parent, const Metrics& m, const PanelFindings& findings, Built built) noexcept -> Built {
                    static constexpr auto CreateCrosshair = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t pick, const std::optional<interior::MonitorHandle>& window) noexcept -> HWND {
                        static constexpr auto Furnished = [] [[nodiscard]] (HWND crosshair, HWND parent, const std::optional<interior::MonitorHandle>& window) noexcept -> HWND {
                            static constexpr auto KeepHighlight = [](HWND crosshair, HWND highlight) noexcept -> void {
                                (void)::SetWindowLongPtrW(crosshair, kHighlight, reinterpret_cast<LONG_PTR>(highlight));
                            };

                            // Owned by the panel, so it goes when the panel goes and nothing has to remember to take it down.
                            static constexpr auto CreateOutline = [] [[nodiscard]] (HWND owner) noexcept -> HWND {
                                // Hidden from every capture: it is a picking aid, and a recording of the work should not hold it.
                                static constexpr auto Prepared = [] [[nodiscard]] (HWND outline) noexcept -> HWND {
                                    if (outline == nullptr)
                                        return nullptr;
                                    (void)::SetLayeredWindowAttributes(outline, kOutlineHollow, 0, LWA_COLORKEY);
                                    (void)::SetWindowDisplayAffinity(outline, WDA_EXCLUDEFROMCAPTURE);
                                    return outline;
                                };
                                return Prepared(::CreateWindowExW(kOutlineStyle, kOutlineClass, nullptr, WS_POPUP, 0, 0, 0, 0, owner, nullptr, ::GetModuleHandleW(nullptr), nullptr));
                            };
                            if (crosshair == nullptr)
                                return nullptr;
                            KeepBoth(crosshair, window);
                            KeepHighlight(crosshair, CreateOutline(parent));
                            return crosshair;
                        };
                        const Placement at = PlaceOfRow(Kind::Pick, pick, m);
                        return Furnished(CreateChild(parent, kCrosshairClass, nullptr, 0, WS_EX_CLIENTEDGE, Bounds(m, at.left, at.control, kCrosshairWidth, m.ControlHeight())), parent, window);
                    };

                    static constexpr auto CreatePickReset = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t pick) noexcept -> HWND {
                        const Placement at = PlaceOfRow(Kind::Pick, pick, m);
                        return CreateButton(parent, m, kRefreshGlyph, 0, at.left + kResetOffset, at.control, kResetWidth);
                    };

                    static constexpr auto CreatePickName = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t pick) noexcept -> HWND {
                        const Placement at = PlaceOfRow(Kind::Pick, pick, m);
                        const int left = at.left + kCrosshairWidth + kMargin;
                        return CreateChild(parent, WC_STATICW, kPicks[pick].nothing, SS_LEFTNOWORDWRAP | SS_ENDELLIPSIS, 0,
                                           Bounds(m, left, at.control, kResetOffset - kCrosshairWidth - 2 * kMargin, m.ControlHeight()));
                    };
                    built.pickLabels = infra::Generated<HWND, kPickCount>([&](std::size_t t) {
                        const Placement at = PlaceOfRow(Kind::Pick, t, m);
                        return CreateLabel(parent, m, kPicks[t].label, at.left, at.top, at.width);
                    });
                    built.crosshairs = infra::Generated<HWND, kPickCount>([&](std::size_t t) { return CreateCrosshair(parent, m, t, findings.window); });
                    built.pickNames = infra::Generated<HWND, kPickCount>([&](std::size_t t) { return CreatePickName(parent, m, t); });
                    built.pickResets = infra::Generated<HWND, kPickCount>([&](std::size_t t) { return CreatePickReset(parent, m, t); });
                    return built;
                };

                // A frame is made after everything it surrounds. Siblings paint in the order they were made, each clipped
                // by those before it, so a frame made first would hide its own rows.
                static constexpr auto BuildFrames = [] [[nodiscard]] (HWND parent, const Metrics& m, Built built) noexcept -> Built {
                    static constexpr auto CreateFrame = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t f) noexcept -> HWND {
                        const Placement at = PlaceOfRow(Kind::Frame, f, m);
                        const int height = HeightOf(Of(static_cast<Frame>(f)), m) - m.Of(kFrameGap);
                        return CreateChild(parent, WC_BUTTONW, kFrames[f].caption, BS_GROUPBOX, 0, Bounds(m, at.left, at.top, at.width, height));
                    };
                    built.frames = infra::Generated<HWND, kFrameCount>([&](std::size_t f) { return CreateFrame(parent, m, f); });
                    return built;
                };

                // A note is a link control whether or not it has a link in it: the control shows plain text as plain text.
                static constexpr auto BuildNotes = [] [[nodiscard]] (HWND parent, const Metrics& m, Built built) noexcept -> Built {
                    static constexpr auto CreateNote = [] [[nodiscard]] (HWND parent, const Metrics& m, std::size_t n) noexcept -> HWND {
                        const Placement at = PlaceOfRow(Kind::Note, n, m);
                        return CreateChild(parent, WC_LINK, kNotes[n].text, WS_TABSTOP, 0, Bounds(m, at.left, at.top, at.width, m.NoteHeight(kNotes[n].lines)));
                    };
                    built.notes = infra::Generated<HWND, kNoteCount>([&](std::size_t n) { return CreateNote(parent, m, n); });
                    return built;
                };
                const Built numbers = BuildToggles(parent, m, StartingToggles(o, live), findings, BuildFields(parent, m, StartingValues(o, live), Built{}));
                const Built rows = BuildLists(parent, m, BuildPicks(parent, m, findings, BuildGroups(parent, m, StartingChoices(o, display), findings, numbers)));
                return BuildNotes(parent, m, BuildFrames(parent, m, rows));
            };

            static constexpr auto CreateTabs = [] [[nodiscard]] (HWND parent, const Metrics& m) noexcept -> HWND {
                return CreateChild(parent, WC_TABCONTROLW, nullptr, TCS_TABS, 0, Bounds(m, kMargin, m.Of(kMargin), kPanelWidth - 2 * kMargin, m.Of(kTabHeight)));
            };

            // What the window measures on the outside for a page of a given height on the inside.
            static constexpr auto OuterHeight = [] [[nodiscard]] (const Metrics& m, int inner) noexcept -> int {
                RECT frame{ 0, 0, m.Of(kPanelWidth), inner };
                ENSURE(::AdjustWindowRectExForDpi(&frame, kPanelStyle, FALSE, 0, static_cast<UINT>(m.dpi)) != FALSE);
                return frame.bottom - frame.top;
            };

            static constexpr auto CreateNotice = [] [[nodiscard]] (HWND parent, const Metrics& m) noexcept -> Notice {
                static constexpr auto CreateNoticeBody = [] [[nodiscard]] (HWND parent, const Metrics& m) noexcept -> HWND {
                    const int top = m.NoticeTop() + m.ControlHeight();
                    const HWND body = CreateChild(parent, WC_STATICW, kNoticeBody, SS_LEFT, 0, Bounds(m, kMargin, top, kPanelWidth - 2 * kMargin, m.BodyHeight()));
                    if (body != nullptr)
                        (void)::ShowWindow(body, SW_HIDE);
                    return body;
                };
                const int width = kPanelWidth - 2 * kMargin - kExpanderWidth;
                const HWND line = CreateChild(parent, WC_STATICW, kNoticeLine, SS_LEFTNOWORDWRAP, 0, Bounds(m, kMargin, m.NoticeTop(), width, m.ControlHeight()));
                const HWND expander = CreateChild(parent, WC_BUTTONW, kChevronGlyph, BS_AUTOCHECKBOX | BS_PUSHLIKE, 0, Bounds(m, kMargin + width, m.NoticeTop(), kExpanderWidth, m.ControlHeight()));
                return Notice{ line, expander, CreateNoticeBody(parent, m) };
            };

            static constexpr auto CountsOf = [] [[nodiscard]] (const PanelLists& lists) noexcept -> std::array<std::size_t, kListCount> {
                return infra::Generated<std::size_t, kListCount>([&lists](std::size_t l) { return lists[l].choices.Size(); });
            };
            HWND parent = window.get();
            const HWND tabs = CreateTabs(parent, m);
            const Built built = BuildAll(parent, m, o, live, display, findings);
            const Notice notice = CreateNotice(parent, m);
            return ControlPanel{ std::move(window),
                                 std::move(font),
                                 IconFont(m.dpi),
                                 BoldFont(m.dpi),
                                 tabs,
                                 tabs == nullptr ? nullptr : CreateTooltip(parent, m),
                                 built.labels,
                                 built.sliders,
                                 built.boxes,
                                 built.spins,
                                 built.resets,
                                 built.warnings,
                                 built.toggles,
                                 built.toggleResets,
                                 built.toggleWarnings,
                                 built.groupLabels,
                                 built.groupWarnings,
                                 built.choices,
                                 built.pickLabels,
                                 built.crosshairs,
                                 built.pickNames,
                                 built.pickResets,
                                 built.listLabels,
                                 built.listChoices,
                                 CountsOf(*m.lists),
                                 built.frames,
                                 built.notes,
                                 o.displayAffinity,
                                 o.clickThrough,
                                 findings.superResolution,
                                 findings.opticalFlow,
                                 o.showInert,
                                 notice.line,
                                 notice.expander,
                                 notice.body,
                                 OuterHeight(m, m.PageTop() + m.PageHeight()),
                                 OuterHeight(m, m.PageTop() + m.PageHeight() + m.BodyHeight()) };
        };

        // The panel is kept out of the capture, or it would photograph the picture it is controlling.
        static constexpr auto Shown = [] [[nodiscard]] (ControlPanel panel) noexcept -> Result<ControlPanel, Error> {
            static constexpr auto IsComplete = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> bool {
                static constexpr auto EveryGroupPresent = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> bool {
                    // The controls that belong to the panel rather than to any one page. Returned by value, so it is only ever
                    // looked at within the expression that asks for it: a span kept past that would outlive what it points at.
                    static constexpr auto Furniture = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> std::array<HWND, 3> { return { panel.notice, panel.expander, panel.noticeBody }; };

                    static constexpr auto EveryRowPresent = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> bool {
                        // Every span here points into the panel itself, which outlives the answer.
                        static constexpr auto GroupsOf = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> std::array<std::span<const HWND>, 13> {
                            return { panel.labels,     panel.sliders,    panel.boxes,     panel.spins,      panel.resets, panel.toggles, panel.groupLabels,
                                     panel.pickLabels, panel.crosshairs, panel.pickNames, panel.pickResets, panel.frames, panel.notes };
                        };
                        return std::ranges::all_of(GroupsOf(panel), AllPresent) && IsPresent(panel.tabs);
                    };
                    return EveryRowPresent(panel) && AllPresent(Furniture(panel));
                };
                return EveryGroupPresent(panel) && ResetsAsSpecified(panel);
            };

            static constexpr auto DressPanel = [](const ControlPanel& panel) noexcept -> void {
                static constexpr auto AddTabs = [](HWND tabs, bool showInert) noexcept -> void {
                    static constexpr auto AddTab = [](HWND tabs, std::size_t index, const wchar_t* title) noexcept -> void {
                        TCITEMW item{ TCIF_TEXT, 0, 0, const_cast<wchar_t*>(title), 0, 0, 0 };
                        (void)::SendMessageW(tabs, TCM_INSERTITEMW, index, reinterpret_cast<LPARAM>(&item));
                    };
                    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, PagesShown(showInert)), [tabs](std::size_t i) { AddTab(tabs, i, kPages[i].title); });
                };

                static constexpr auto HintRows = [](const ControlPanel& panel) noexcept -> void {
                    static constexpr auto AddHint = [](HWND tooltip, HWND parent, HWND control, const wchar_t* text) noexcept -> void {
                        static constexpr auto HintFor = [] [[nodiscard]] (HWND parent, HWND control, const wchar_t* text) noexcept -> TTTOOLINFOW {
                            TTTOOLINFOW info{ .cbSize = sizeof(TTTOOLINFOW),
                                              .uFlags = TTF_IDISHWND | TTF_SUBCLASS,
                                              .hwnd = parent,
                                              .uId = reinterpret_cast<UINT_PTR>(control),
                                              .rect = RECT{ 0, 0, 0, 0 },
                                              .hinst = nullptr,
                                              .lpszText = const_cast<wchar_t*>(text),
                                              .lParam = 0,
                                              .lpReserved = nullptr };
                            return info;
                        };
                        TTTOOLINFOW info = HintFor(parent, control, text);
                        (void)::SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
                    };

                    // WAIVER(R7): walking one table twice is what several of these do; each does something else with it.
                    // A number's hint sits on the slider and on the box, so either one under the pointer explains itself.
                    static constexpr auto HintNumbers = [](const ControlPanel& panel, HWND parent) noexcept -> void {
                        static constexpr auto HintWarning = [](const ControlPanel& panel, HWND parent, std::size_t f) noexcept -> void {
                            if (panel.warnings[f] != nullptr)
                                AddHint(panel.tooltip, parent, panel.warnings[f], kFields[f].warning);
                        };
                        std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel, parent](std::size_t f) { AddHint(panel.tooltip, parent, panel.sliders[f], kFields[f].hint); });
                        std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel, parent](std::size_t f) { AddHint(panel.tooltip, parent, panel.boxes[f], kFields[f].hint); });
                        std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel, parent](std::size_t f) { HintWarning(panel, parent, f); });
                    };

                    static constexpr auto HintResets = [](const ControlPanel& panel, HWND parent) noexcept -> void {
                        std::ranges::for_each(panel.resets, [&panel, parent](HWND button) { AddHint(panel.tooltip, parent, button, kResetHint); });
                        std::ranges::for_each(panel.toggleResets, [&panel, parent](HWND button) { AddHint(panel.tooltip, parent, button, kResetHint); });
                    };

                    static constexpr auto HintChoices = [](const ControlPanel& panel, HWND parent) noexcept -> void {
                        static constexpr auto HintToggleWarning = [](const ControlPanel& panel, HWND parent, std::size_t t) noexcept -> void {
                            if (panel.toggleWarnings[t] != nullptr)
                                AddHint(panel.tooltip, parent, panel.toggleWarnings[t], kToggles[t].warning);
                        };
                        std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kToggleCount),
                                              [&panel, parent](std::size_t t) { AddHint(panel.tooltip, parent, panel.toggles[t], kToggles[t].hint); });
                        std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kToggleCount), [&panel, parent](std::size_t t) { HintToggleWarning(panel, parent, t); });
                        std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kGroupCount),
                                              [&panel, parent](std::size_t g) { AddHint(panel.tooltip, parent, panel.groupLabels[g], kGroups[g].hint); });
                        std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kPickCount), [&panel, parent](std::size_t t) { AddHint(panel.tooltip, parent, panel.crosshairs[t], kPicks[t].hint); });
                        std::ranges::for_each(panel.pickResets, [&panel, parent](HWND button) { AddHint(panel.tooltip, parent, button, kReleaseHint); });
                    };

                    // A greyed control that says nothing is just a control that does not work, so the reason replaces the hint.
                    static constexpr auto HintMissingSuperResolution = [](const ControlPanel& panel, HWND parent) noexcept -> void {
                        if (panel.superResolution)
                            return;
                        AddHint(panel.tooltip, parent, panel.groupLabels[static_cast<std::size_t>(Group::Sr)], kNoSuperResolution);
                        AddHint(panel.tooltip, parent, panel.groupWarnings[static_cast<std::size_t>(Group::Sr)], kNoSuperResolution);
                        AddHint(panel.tooltip, parent, panel.labels[static_cast<std::size_t>(Field::SrPreset)], kNoSuperResolution);
                    };
                    static constexpr auto HintMissingOpticalFlow = [](const ControlPanel& panel, HWND parent) noexcept -> void {
                        if (panel.opticalFlow)
                            return;
                        AddHint(panel.tooltip, parent, panel.groupLabels[static_cast<std::size_t>(Group::Motion)], kNoOpticalFlow);
                        AddHint(panel.tooltip, parent, panel.groupLabels[static_cast<std::size_t>(Group::NvofGrid)], kNoOpticalFlowEngine);
                        AddHint(panel.tooltip, parent, panel.groupLabels[static_cast<std::size_t>(Group::NvofPerf)], kNoOpticalFlowEngine);
                    };
                    HWND parent = panel.window.get();
                    HintNumbers(panel, parent);
                    HintChoices(panel, parent);
                    HintResets(panel, parent);
                    HintMissingSuperResolution(panel, parent);
                    HintMissingOpticalFlow(panel, parent);
                };

                static constexpr auto IconiseResets = [](const ControlPanel& panel) noexcept -> void {
                    // The message font goes on every child, so the reset buttons take their glyph font afterwards; a hint says
                    // what the glyph means, since a picture of a circling arrow does not say which value it puts back.
                    static constexpr auto WearIcon = [](const ControlPanel& panel, HWND button) noexcept -> void {
                        if (button != nullptr)
                            (void)::SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(panel.iconFont.get()), TRUE);
                    };
                    std::ranges::for_each(panel.resets, [&panel](HWND button) { WearIcon(panel, button); });
                    std::ranges::for_each(panel.toggleResets, [&panel](HWND button) { WearIcon(panel, button); });
                    std::ranges::for_each(panel.pickResets, [&panel](HWND button) { WearIcon(panel, button); });
                    std::ranges::for_each(panel.warnings, [&panel](HWND glyph) { WearIcon(panel, glyph); });
                    std::ranges::for_each(panel.groupWarnings, [&panel](HWND glyph) { WearIcon(panel, glyph); });
                    std::ranges::for_each(panel.toggleWarnings, [&panel](HWND glyph) { WearIcon(panel, glyph); });
                    WearIcon(panel, panel.expander);
                    (void)::SendMessageW(panel.notice, WM_SETFONT, reinterpret_cast<WPARAM>(panel.boldFont.get()), TRUE);
                };
                HFONT font = panel.font.get();
                // WAIVER(R2): walking the panel's own children to give them the system font.
                ::EnumChildWindows(
                    panel.window.get(),
                    [](HWND child, LPARAM f) -> BOOL {
                        (void)::SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(f), TRUE);
                        return TRUE;
                    },
                    reinterpret_cast<LPARAM>(font));
                AddTabs(panel.tabs, panel.showInert);
                IconiseResets(panel);
                HintRows(panel);
            };
            if (!IsComplete(panel))
                return Fail(LastError(ApiCall::CreateWindowExW));
            DressPanel(panel);
            return CheckBool(::SetWindowDisplayAffinity(panel.window.get(), WDA_EXCLUDEFROMCAPTURE), ApiCall::SetWindowDisplayAffinity).transform([&panel] {
                ShowOnly(panel, ChosenPage(panel)); // every control is created visible, so the first page is arranged rather than checked

                ::ShowWindow(panel.window.get(), SW_SHOWNOACTIVATE);
                return std::move(panel);
            });
        };
        UniqueFont font = MessageFont(static_cast<int>(::GetDpiForWindow(window.get())));
        const Metrics m = MetricsOf(window.get(), font.get(), findings.lists, o.showInert);
        ResizeToFit(window.get(), m);
        return Shown(Assembled(std::move(window), std::move(font), m, o, live, display, findings));
    };
    InitialiseCommonControls();
    return RegisterWindowClass(ClassDescription())
        .and_then([] { return RegisterWindowClass(CrosshairDescription()); })
        .and_then([] { return RegisterWindowClass(HighlightDescription()); })
        .and_then([&] { return CreatePanelWindow().and_then([&](UniqueWindow window) { return Populated(std::move(window), options, live, display, findings); }); });
}

PanelReading ReadControlPanel(const ControlPanel& panel, const interior::LiveSettings& current) noexcept
{
    static constexpr auto SurfaceOf = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> interior::SurfaceSettings {
        static constexpr auto CursorFrom = [] [[nodiscard]] (std::size_t index) noexcept -> interior::CursorMode {
            constexpr std::array<interior::CursorMode, 3> modes{ interior::CursorMode::Auto, interior::CursorMode::On, interior::CursorMode::Off };
            return modes[std::min(index, modes.size() - 1)];
        };

        static constexpr auto LogLevelFrom = [] [[nodiscard]] (std::size_t index) noexcept -> interior::LogLevel {
            constexpr std::array<interior::LogLevel, 4> levels{ interior::LogLevel::Debug, interior::LogLevel::Info, interior::LogLevel::Warn, interior::LogLevel::Error };
            return levels[std::min(index, levels.size() - 1)];
        };
        return interior::SurfaceSettings{ CursorFrom(ChosenIn(panel, Group::Cursor, 0)),    IsOn(panel, Toggle::CaptureBorder), panel.displayAffinity, IsOn(panel, Toggle::Topmost), panel.clickThrough,
                                          LogLevelFrom(ChosenIn(panel, Group::LogLevel, 1)) };
    };

    static constexpr auto Arrange = [](const ControlPanel& panel) noexcept -> void {
        static constexpr auto ShowChosenPage = [](const ControlPanel& panel) noexcept -> void {
            // The chosen page is shown and the others hidden, but only when the choice has moved, so the panel is not
            // asked to redraw itself on every frame. Whether it has moved is read from the page's own first control.
            static constexpr auto PageAlreadyShown = [] [[nodiscard]] (const ControlPanel& panel, Page chosen) noexcept -> bool {
                static constexpr auto PageMarker = [] [[nodiscard]] (const ControlPanel& panel, Page page) noexcept -> HWND {
                    // The first control of a row, which stands for the page the row is on. A frame stands for itself, and a
                    // break for nothing.
                    static constexpr auto MarkerOf = [] [[nodiscard]] (const ControlPanel& panel, const RowSpec& row) noexcept -> HWND {
                        // Each kind is indexed only by a row of its own kind, so the index is always in range for the array it picks.
                        static constexpr auto MarkerOfRow = [] [[nodiscard]] (const ControlPanel& panel, const RowSpec& row) noexcept -> HWND {
                            const std::array<std::span<const HWND>, 5> byKind{ panel.labels, panel.toggles, panel.groupLabels, panel.pickLabels, panel.listLabels };
                            return byKind[static_cast<std::size_t>(row.kind)][row.index];
                        };
                        if (row.kind == Kind::Break)
                            return nullptr;
                        if (row.kind == Kind::Frame)
                            return panel.frames[row.index];
                        if (row.kind == Kind::Note)
                            return panel.notes[row.index];
                        return MarkerOfRow(panel, row);
                    };

                    // A row whose list is empty has no controls, so the page is marked by the first row that does have one.
                    static constexpr auto CarriesControls = [] [[nodiscard]] (const ControlPanel& panel, const RowSpec& row) noexcept -> bool { return MarkerOf(panel, row) != nullptr; };
                    const std::span<const RowSpec> rows = RowsOf(page);
                    const auto found = std::ranges::find_if(rows, [&panel](const RowSpec& row) { return CarriesControls(panel, row); });
                    ENSURE(found != rows.end());
                    return MarkerOf(panel, *found);
                };
                return ::IsWindowVisible(PageMarker(panel, chosen)) != FALSE;
            };
            const Page chosen = ChosenPage(panel);
            if (PageAlreadyShown(panel, chosen))
                return;
            ShowOnly(panel, chosen);
        };

        static constexpr auto ApplyNotice = [](const ControlPanel& panel) noexcept -> void {
            // The expander is a check box that looks like a button, so it keeps its own state and the panel reads it.
            // Opening it grows the window, and nothing moves because the notice sits under everything else.
            static constexpr auto IsNoticeOpen = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> bool { return IsChecked(panel.expander); };

            static constexpr auto ResizeForNotice = [](const ControlPanel& panel, bool open) noexcept -> void {
                RECT frame{};
                ENSURE(::GetWindowRect(panel.window.get(), &frame) != FALSE);
                const int height = open ? panel.tallHeight : panel.shortHeight;
                ENSURE(::SetWindowPos(panel.window.get(), nullptr, 0, 0, frame.right - frame.left, height, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
            };
            const bool open = IsNoticeOpen(panel);
            if (open == (::IsWindowVisible(panel.noticeBody) != FALSE))
                return;
            (void)::ShowWindow(panel.noticeBody, HowOf(open));
            ResizeForNotice(panel, open);
        };

        static constexpr auto ApplyEnables = [](const ControlPanel& panel) noexcept -> void {
            static constexpr auto EnableAll = [](std::span<const HWND> controls, bool enabled) noexcept -> void {
                std::ranges::for_each(controls, [enabled](HWND control) { (void)::EnableWindow(control, enabled ? TRUE : FALSE); });
            };

            static constexpr auto ShowsASplit = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> bool {
                return DisplayFrom(ChosenIn(panel, Group::Compare, 0)) == interior::DisplayMode::Split;
            };
            EnableAll(ControlsOfField(panel, static_cast<std::size_t>(Field::Split)), ShowsASplit(panel));
            EnableAll(ControlsOfField(panel, static_cast<std::size_t>(Field::Skin)), !IsOn(panel, Toggle::SkinFollowsStructure));
            static constexpr auto OpticalFlowChoice = [] [[nodiscard]] (const ControlPanel& panel) noexcept -> std::span<const HWND> {
                return ChoicesOf(panel, Group::Motion).subspan(kOpticalFlowChoice, 1);
            };
            EnableAll(ChoicesOf(panel, Group::Sr), panel.superResolution);
            EnableAll(ControlsOfField(panel, static_cast<std::size_t>(Field::SrPreset)), panel.superResolution);
            EnableAll(OpticalFlowChoice(panel), panel.opticalFlow);
            EnableAll(ChoicesOf(panel, Group::NvofGrid), panel.opticalFlow);
            EnableAll(ChoicesOf(panel, Group::NvofPerf), panel.opticalFlow);
        };

        // What the panel does to itself before it is read: the chosen page, the notice, what is greyed, and any
        // reset the operator is holding down.
        static constexpr auto Readback = [](const ControlPanel& panel) noexcept -> void {
            // A plain button reads as pushed only while it is actually held down, which suits a reset the operator
            // leans on and does not suit anything answered by a single click: between two reads the click is gone.
            static constexpr auto IsPushed = [] [[nodiscard]] (HWND button) noexcept -> bool { return (::SendMessageW(button, BM_GETSTATE, 0, 0) & BST_PUSHED) != 0; };

            // WAIVER(R7): walking one small table twice is what several of these do; each does something else with it.
            static constexpr auto ApplyPicks = [](const ControlPanel& panel) noexcept -> void {
                // Holding the reset lets the window go, and the session goes back to the monitor the source names.
                static constexpr auto ReleasePicked = [](const ControlPanel& panel, std::size_t pick) noexcept -> void {
                    if (IsPushed(panel.pickResets[pick]))
                        KeepBoth(panel.crosshairs[pick], std::nullopt);
                };
                std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kPickCount), [&panel](std::size_t p) { ReleasePicked(panel, p); });
                std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kPickCount), [&panel](std::size_t p) { ShowPickedName(panel, p); });
            };

            static constexpr auto SettleAll = [](const ControlPanel& panel) noexcept -> void {
                // Every frame each number is read, settled, and written back to all three controls. Without this the boxes
                // were never filled in at all, and the three were free to disagree for good once any of them had moved.
                static constexpr auto SettleField = [](const ControlPanel& panel, std::size_t field) noexcept -> void { Commit(panel, field, Settled(panel, field)); };
                std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel](std::size_t f) { SettleField(panel, f); });
            };

            // A held reset puts its own control back where the defaults start it; the same answer every frame.
            static constexpr auto ApplyResets = [](const ControlPanel& panel) noexcept -> void {
                static constexpr auto ResetField = [](const ControlPanel& panel, std::size_t f, float value) noexcept -> void {
                    // A slider stretched to follow a value past its end is put back where it started, so a reset takes back the
                    // reach as well as the value.
                    static constexpr auto Restored = [](const ControlPanel& panel, std::size_t f, float value) noexcept -> void {
                        (void)::SendMessageW(panel.sliders[f], TBM_SETRANGEMAX, TRUE, kFields[f].maximum);
                        Commit(panel, f, StepsOf(value, kFields[f]));
                    };
                    if (IsPushed(panel.resets[f]))
                        Restored(panel, f, value);
                };

                static constexpr auto ResetToggle = [](const ControlPanel& panel, std::size_t t, bool on) noexcept -> void {
                    if (IsPushed(panel.toggleResets[t]))
                        SetChecked(panel.toggles[t], on);
                };
                const interior::Options d = interior::DefaultOptions();
                const std::array<float, kFieldCount> values = StartingValues(d, interior::DefaultLive(d));
                const std::array<bool, kToggleCount> on = StartingToggles(d, interior::DefaultLive(d));
                std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&](std::size_t f) { ResetField(panel, f, values[f]); });
                std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kToggleCount), [&](std::size_t t) { ResetToggle(panel, t, on[t]); });
            };
            ApplyResets(panel);
            ApplyPicks(panel);
            SettleAll(panel);
        };
        ShowChosenPage(panel);
        ApplyNotice(panel);
        ApplyEnables(panel);
        Readback(panel);
    };
    Arrange(panel);
    const interior::Fraction split = interior::FractionTag::Parse(SettledValue(panel, Field::Split)).value_or(*kCentre);
    return PanelReading{ LiveOf(panel, current), SurfaceOf(panel), DisplayFrom(ChosenIn(panel, Group::Compare, 0)), split };
}

// The window a session was following has gone: the panel lets go of it too, so what it shows is the source
// the next session is built from, and the crosshair is ready to be dragged onto another window.
std::optional<interior::MonitorHandle> PickedWindow(const ControlPanel& panel) noexcept
{
    return HandleAt(panel.crosshairs[static_cast<std::size_t>(Pick::Window)], kChosen);
}

void ReleaseWindow(const ControlPanel& panel) noexcept
{
    KeepBoth(panel.crosshairs[static_cast<std::size_t>(Pick::Window)], std::nullopt);
    ShowPickedName(panel, static_cast<std::size_t>(Pick::Window));
}

interior::CommandLine SessionShape(const ControlPanel& panel) noexcept
{
    const Arguments shape = FlowArguments(panel, StartupArguments(panel, Arguments{}));
    return interior::CommandLine::Parse(WidenedLine(shape.Get()).data()).value_or(interior::CommandLine{});
}

interior::CommandLine RestartCommandLine(const ControlPanel& panel, const interior::Options& options) noexcept
{
    static constexpr auto Decimal = [] [[nodiscard]] (std::string_view name, float value) noexcept -> Piece {
        return Trimmed(infra::Formatted<kPieceCapacity>("--{}={:.3f}", name, static_cast<double>(value)).Get());
    };

    // The live settings travel with the new session too, so it starts where this one left off.
    static constexpr auto ModelArguments = [] [[nodiscard]] (const ControlPanel& panel, const Arguments& so) noexcept -> Arguments {
        const interior::LiveSettings live = LiveOf(panel, interior::DefaultLive(interior::DefaultOptions()));
        const std::array<Piece, 9> pieces{ Switch(panel, Toggle::NeuralRendering, "nr"),
                                           Counted("nr-passes", live.passes.Get()),
                                           Counted("nr-preset", live.tuning.preset.Get()),
                                           Decimal("nr-intensity", live.tuning.intensity.Get()),
                                           Decimal("nr-local-structure", live.tuning.localStructure.Get()),
                                           Decimal("nr-local-tone", live.tuning.localTone.Get()),
                                           Decimal("nr-skin", live.tuning.skinStructure.Get()),
                                           Counted("nr-style", interior::StyleCode(live.tuning.style)),
                                           Switch(panel, Toggle::AutoMask, "nr-automask") };
        return JoinedAll(so, pieces);
    };

    static constexpr auto SurfaceArguments = [] [[nodiscard]] (const ControlPanel& panel, const Arguments& so) noexcept -> Arguments {
        constexpr std::array<const char*, 3> cursor{ "auto", "on", "off" };
        constexpr std::array<const char*, 4> levels{ "0", "1", "2", "3" };
        constexpr std::array<const char*, 3> compare{ "off", "split", "original" }; // in the order of the buttons
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
    };

    static constexpr auto WindowArguments = [] [[nodiscard]] (const ControlPanel& panel, const Arguments& so) noexcept -> Arguments {
        const std::array<Piece, 3> pieces{ Trimmed(infra::Formatted<kPieceCapacity>("--affinity={}", Word(panel.displayAffinity)).Get()), Switch(panel, Toggle::Topmost, "topmost"),
                                           Trimmed(infra::Formatted<kPieceCapacity>("--click-through={}", Word(panel.clickThrough)).Get()) };
        return JoinedAll(so, pieces);
    };

    // What the panel has no control for still travels to the new session, so one started with an application
    // id or a project id of its own keeps them rather than falling back to the defaults.
    static constexpr auto RuntimeArguments = [] [[nodiscard]] (const interior::Options& o, const Arguments& so) noexcept -> Arguments {
        static constexpr auto AppIdPiece = [] [[nodiscard]] (const std::optional<interior::NgxAppId>& id) noexcept -> Piece {
            if (!id.has_value())
                return Piece{};
            return Trimmed(infra::Formatted<kPieceCapacity>("--ngx-app-id={:x}", id->Get()).Get());
        };
        constexpr std::array<const char*, 3> ngxLog{ "0", "1", "2" };
        const std::array<Piece, 6> pieces{ Trimmed(infra::Formatted<kPieceCapacity>("--ngx-log={}", ngxLog[static_cast<std::size_t>(o.ngxLogLevel)]).Get()),
                                           Trimmed(infra::Formatted<kPieceCapacity>("--ngx-project-id={}", o.ngxProjectId.Get()).Get()),
                                           AppIdPiece(o.ngxAppId),
                                           Trimmed(infra::Formatted<kPieceCapacity>("--show-inert={}", Word(o.showInert)).Get()),
                                           Trimmed(infra::Formatted<kPieceCapacity>("--exclude-own-windows={}", Word(o.excludeOwnWindows)).Get()),
                                           Trimmed(infra::Formatted<kPieceCapacity>("--exclusion-log={}", Word(o.exclusionLog)).Get()) };
        return JoinedAll(so, pieces);
    };

    static constexpr auto WithPaths = [] [[nodiscard]] (const interior::CommandLine& so, const interior::Options& o) noexcept -> interior::CommandLine {
        // A path holds whatever the file system allows, so it travels as it was written rather than through the
        // ASCII buffer the switches are built in. The quotes keep a path with spaces in one argument.
        static constexpr auto WithPath = [] [[nodiscard]] (const interior::CommandLine& so, const wchar_t* name, std::wstring_view path) noexcept -> interior::CommandLine {
            // A line too long for the buffer leaves the old one standing, so no session starts from half a path.
            static constexpr auto Extended = [] [[nodiscard]] (const interior::CommandLine& so, const std::array<wchar_t, interior::CommandLine::Capacity + 1>& line,
                                                               int written) noexcept -> interior::CommandLine {
                if (written <= 0)
                    return so;
                return interior::CommandLine::Parse(line.data()).value_or(so);
            };
            if (path.empty())
                return so;
            std::array<wchar_t, interior::CommandLine::Capacity + 1> line{}; // WAIVER(R2): a local buffer filled once, before use.
            const int written =
                ::_snwprintf_s(line.data(), line.size(), _TRUNCATE, L"%.*s --%s=\"%.*s\"", static_cast<int>(so.Get().size()), so.Get().data(), name, static_cast<int>(path.size()), path.data());
            return Extended(so, line, written);
        };
        const interior::CommandLine paths = WithPath(WithPath(so, L"ngx-path", o.ngxPath.Get()), L"app-data", o.appDataPath.Get());
        return WithPath(paths, L"log-file", o.logFile.Get());
    };
    const Arguments arguments = RuntimeArguments(options, WindowArguments(panel, SurfaceArguments(panel, ModelArguments(panel, FlowArguments(panel, StartupArguments(panel, Arguments{}))))));
    const interior::CommandLine line = interior::CommandLine::Parse(WidenedLine(arguments.Get()).data()).value_or(interior::CommandLine{});
    return WithPaths(line, options);
}

void ApplyDisplay(const ControlPanel& panel, interior::DisplayMode display) noexcept
{
    static constexpr auto ChooseOnly = [](std::span<const HWND> group, std::size_t index) noexcept -> void {
        std::ranges::for_each(std::views::iota(std::size_t{ 0 }, group.size()), [group, index](std::size_t i) { SetChecked(group[i], i == index); });
    };
    ChooseOnly(ChoicesOf(panel, Group::Compare), ChoiceOfDisplay(display));
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
