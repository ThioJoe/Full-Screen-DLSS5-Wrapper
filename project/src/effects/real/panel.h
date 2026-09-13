#pragma once
#include "effects/real/com.h"
#include "infrastructure/bounded_vector.h"
#include "interior/options.h"
#include "interior/sweep.h"

#include <array>
#include <optional>

namespace real {

struct FontDeleter
{
    void operator()(HFONT font) const noexcept { ENSURE(::DeleteObject(font) != FALSE); }
};
using UniqueFont = std::unique_ptr<std::remove_pointer_t<HFONT>, FontDeleter>;

// The panel's pages: the model's own tuning, what is captured and where it is shown, the choices worth
// having but rarely worth changing, and what the program is and where its source is. Inert holds what
// changes nothing on a desktop; --show-inert asks for it, so it comes last.
enum class Page : std::size_t { Model, View, Advanced, Capture, About, Inert, Count };

// A number the operator sets: a slider to sweep it, a box to type it, arrows to step it.
enum class Field : std::size_t { Intensity, LocalStructure, LocalTone, Skin, MvScaleX, MvScaleY, Split, DepthValue, ResetThreshold, MvLevel, SrPreset, Passes, Count };

// A switch the operator flips.
enum class Toggle : std::size_t {
    NeuralRendering,
    AutoMask,
    SkinFollowsStructure,
    UiCorrection,
    DepthInverted,
    Vsync,
    CaptureBorder,
    Topmost,
    RedirectionBitmap,
    DebugLayer,
    Indicator,
    CubinCache,
    AllParameters, // every parameter goes into a capture's name, the ones at their defaults included
    CaptureCursor, // the cursor is drawn into captures where it was, when the capture itself leaves it out
    Count
};

// A choice among a few named alternatives.
enum class Group : std::size_t { Compare, Style, Cursor, Motion, NvofGrid, NvofPerf, Sr, Format, LogLevel, Count };

// A set of named alternatives only known once the machine has been looked at: the monitors attached, the
// adapters the system has, the presets the model carries. An empty list has no row at all.
enum class List : std::size_t { Preset, Source, Target, Adapter, Count };

// A window, picked by dragging a crosshair onto it. Nothing about a window fits a slider or a list, and
// its title is not something anyone should have to type.
enum class Pick : std::size_t { Window, Count };

// A group box drawn around rows that belong together, captioned with what they have in common: the tone
// controls, since style changes nothing while local tone is zero; the skin controls; the comparison view;
// and the comparison capture, which runs settings through their values.
enum class Frame : std::size_t { Tone, Skin, Compare, Comparison, Count };

// A line or a few of text: the About page's, one with a link to the repository in it, and what a
// comparison capture does, at the top of its box.
enum class Note : std::size_t { Title, Purpose, Repository, Comparison, Count };

// A bar that fills as a comparison capture goes along.
enum class Progress : std::size_t { Comparison, Count };

// A word over a column of controls, saying what the column holds.
enum class Heading : std::size_t { Values, Count };

// A folder the operator names, typed into a box or browsed for.
enum class Folder : std::size_t { Captures, Count };

// A push button that asks for one thing to be done: a screenshot, from the Capture page and again from the
// Model page, so it is to hand while the model is being tuned; a recording; a comparison capture; and
// bringing back the window being worked on when it has minimised itself.
enum class Action : std::size_t { Screenshot, ModelScreenshot, Record, Compare, RestoreWindow, Count };

constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);
constexpr std::size_t kToggleCount = static_cast<std::size_t>(Toggle::Count);
constexpr std::size_t kGroupCount = static_cast<std::size_t>(Group::Count);
constexpr std::size_t kPickCount = static_cast<std::size_t>(Pick::Count);
constexpr std::size_t kFrameCount = static_cast<std::size_t>(Frame::Count);
constexpr std::size_t kNoteCount = static_cast<std::size_t>(Note::Count);
constexpr std::size_t kFolderCount = static_cast<std::size_t>(Folder::Count);
constexpr std::size_t kActionCount = static_cast<std::size_t>(Action::Count);
constexpr std::size_t kSweepCount = interior::kSweepParameterCount;
constexpr std::size_t kProgressCount = static_cast<std::size_t>(Progress::Count);
constexpr std::size_t kHeadingCount = static_cast<std::size_t>(Heading::Count);
constexpr std::size_t kListCount = static_cast<std::size_t>(List::Count);
constexpr std::size_t kMaxListChoices = 20;
constexpr std::size_t kMaxChoices = 4;

using ChoiceText = infra::BoundedString<wchar_t, 62>;

// One list as the session found it: what to call each alternative, and which of them the session is using.
using ChoiceTexts = infra::BoundedVector<ChoiceText, kMaxListChoices>;

struct PanelList
{
    ChoiceTexts choices;
    std::size_t chosen;
};

using PanelLists = std::array<PanelList, kListCount>;

// What the session found out that the panel needs and cannot ask for itself.
struct PanelFindings
{
    PanelLists lists;
    bool superResolution;                          // whether the driver offers it at all; without it the choice is shown but greyed
    bool opticalFlow;                              // whether this build carries the NVIDIA Optical Flow backend; without it that choice is shown but greyed
    bool modelAsNamed;                             // whether nvngx_dlssnr.dll calls its product what the model is called; without that it runs, under a warning
    interior::DirectoryPath captureFolder;         // where captures go until the operator names another folder
    std::optional<interior::MonitorHandle> window; // the window this session is working on, when it is working on one
};

// The panel keeps no state of its own: the controls hold the operator's choices and are read each frame.
// Child windows die with their parent, so only the panel itself and its font own a handle.
struct ControlPanel
{
    UniqueWindow window;
    UniqueFont font;
    UniqueFont iconFont; // the reset buttons wear a glyph rather than a word, so they get their own face
    UniqueFont boldFont; // the notice under the controls, which should not read as one more label
    HWND tabs;
    HWND tooltip;
    std::array<HWND, kFieldCount> labels;
    std::array<HWND, kFieldCount> sliders;
    std::array<HWND, kFieldCount> boxes;
    std::array<HWND, kFieldCount> spins;
    std::array<HWND, kFieldCount> resets;
    std::array<HWND, kFieldCount> warnings; // a glyph at the end of a label whose number wants a word of caution, or nothing
    std::array<HWND, kToggleCount> toggles;
    std::array<HWND, kToggleCount> toggleResets;
    std::array<HWND, kToggleCount> toggleWarnings; // a glyph before a switch when what it warns of is true, or nothing
    std::array<HWND, kGroupCount> groupLabels;
    std::array<HWND, kGroupCount> groupWarnings; // a glyph before a group's label when what it warns of is true, or nothing
    std::array<std::array<HWND, kMaxChoices>, kGroupCount> choices;
    std::array<HWND, kPickCount> pickLabels;
    std::array<HWND, kPickCount> crosshairs; // each holds the window it was last dragged onto
    std::array<HWND, kPickCount> pickNames;
    std::array<HWND, kPickCount> pickResets; // lets go of the window again, back to capturing a monitor
    std::array<HWND, kListCount> listLabels;
    std::array<std::array<HWND, kMaxListChoices>, kListCount> listChoices;
    std::array<std::size_t, kListCount> listCounts;
    std::array<HWND, kFrameCount> frames; // each lies under the rows it surrounds, drawn last so it clips none of them
    std::array<HWND, kNoteCount> notes;   // the About page's text; a link in it is opened by the panel's own window procedure
    std::array<HWND, kFolderCount> folderLabels;
    std::array<HWND, kFolderCount> folderBoxes;
    std::array<HWND, kFolderCount> folderBrowsers; // each holds the box beside it, which the folder it browses for goes into
    std::array<HWND, kActionCount> actions;        // each holds whether it was clicked since the panel was last read
    HWND recordingLabel;                           // how long the recording has run, beside the button that stops it; empty otherwise
    std::array<HWND, kSweepCount> sweepChecks;     // one per setting a comparison capture can run through
    std::array<HWND, kSweepCount> sweepBoxes;      // how many values, for a setting that takes a count; nothing for the rest
    std::array<HWND, kSweepCount> sweepSpins;
    HWND comparisonLabel; // how many pictures a comparison capture would take, or how far the one under way has got
    std::array<HWND, kProgressCount> progress;
    std::array<HWND, kHeadingCount> headings;
    // Two settings the panel carries but does not show: off, each spoils the picture rather than changing
    // it, so they are the command line's to set and the panel's to pass on unaltered.
    bool displayAffinity;
    bool clickThrough;
    bool superResolution;
    bool opticalFlow;
    bool showInert;  // whether the Inert page has a tab, which is what decides how tall the panel is
    HWND notice;     // the one line that is always there
    HWND expander;   // holds its own state, which is the panel's record of whether the notice is open
    HWND noticeBody; // the rest of it, shown only when the operator opens it
    int shortHeight; // what the window measures with the notice closed, and with it open
    int tallHeight;
};

// What a comparison capture is asked to run through, and whether its button was clicked, which starts one or
// stops the one under way.
struct ComparisonRequest
{
    bool start;
    interior::SweepSpec axes;
};

// What the operator asked of the capture this frame, and with what.
struct CaptureRequest
{
    bool screenshot; // a screenshot button was clicked since the panel was last read
    bool record;     // the record button was clicked since the panel was last read, which starts or stops a recording
    interior::DirectoryPath folder;
    bool everything; // every parameter goes into the name
    bool cursor;     // the cursor is drawn into the pictures
    ComparisonRequest comparison;
};

// What the panel says this frame: the settings that take effect at once, the view they belong to, and what
// the capture was asked for.
struct PanelReading
{
    interior::LiveSettings live;
    interior::SurfaceSettings surface;
    interior::DisplayMode display;
    interior::Fraction split;
    CaptureRequest capture;
    bool restoreWindow; // the button that brings the window back was clicked since the panel was last read
};

[[nodiscard]] infra::Result<ControlPanel, Error> CreateControlPanel(const interior::Options& options, const interior::LiveSettings& live, interior::DisplayMode display,
                                                                    const PanelFindings& findings) noexcept;

// Reads every control and settles any disagreement between a slider, its box and its arrows, writing the
// answer back to all three. A value a unit refuses keeps what `current` holds.
[[nodiscard]] PanelReading ReadControlPanel(const ControlPanel& panel, const interior::LiveSettings& current) noexcept;

// The settings that decide what a session is built from, as a line of their own. A session watches this
// for a change: the rest of the panel it can follow while it runs, this it can only be rebuilt for.
[[nodiscard]] interior::CommandLine SessionShape(const ControlPanel& panel) noexcept;

// Lets go of the window the crosshair is holding, for when that window is closed, hidden or minimised
// while a session is following it.
void ReleaseWindow(const ControlPanel& panel) noexcept;
// Offers the button that brings the window back only while there is a window being followed to bring back.
void ApplyFollowing(const ControlPanel& panel, bool following) noexcept;

// The window the crosshair was last left on, which is what a session built from the panel would follow.
[[nodiscard]] std::optional<interior::MonitorHandle> PickedWindow(const ControlPanel& panel) noexcept;

// The command line the start-up page describes, for the session the operator has asked for.
[[nodiscard]] interior::CommandLine RestartCommandLine(const ControlPanel& panel, const interior::Options& options) noexcept;

// Moves the panel's own controls, so the hotkeys and the divider drag stay in step with what it shows.
void ApplyDisplay(const ControlPanel& panel, interior::DisplayMode display) noexcept;
void ApplySplit(const ControlPanel& panel, interior::Fraction split) noexcept;

// Shows whether a recording is under way and for how long: the record button reads as the way to stop it,
// and the time runs beside it. Nothing means none is.
void ApplyRecording(const ControlPanel& panel, const std::optional<interior::Microseconds>& elapsed) noexcept;

// Shows how many pictures a comparison capture would take as the boxes stand, or how far the one under way
// has got: the button reads as the way to stop it, the count runs beside it, and the bar fills.
void ApplyComparison(const ControlPanel& panel, std::uint32_t planned, const std::optional<interior::SweepProgress>& running) noexcept;

// True once the operator has closed the panel, which ends the session.
[[nodiscard]] bool IsPanelClosed(const ControlPanel& panel) noexcept;

} // namespace real
