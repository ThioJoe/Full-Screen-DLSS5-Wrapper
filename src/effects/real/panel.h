#pragma once
#include "effects/real/com.h"
#include "infrastructure/bounded_vector.h"
#include "interior/options.h"

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
enum class Page : std::size_t { Model, View, Advanced, About, Inert, Count };

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
// controls, since style changes nothing while local tone is zero; the skin controls; and the comparison.
enum class Frame : std::size_t { Tone, Skin, Compare, Count };

// A line or a few of text on the About page, one of them with a link to the repository in it.
enum class Note : std::size_t { Title, Purpose, Repository, Count };

constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);
constexpr std::size_t kToggleCount = static_cast<std::size_t>(Toggle::Count);
constexpr std::size_t kGroupCount = static_cast<std::size_t>(Group::Count);
constexpr std::size_t kPickCount = static_cast<std::size_t>(Pick::Count);
constexpr std::size_t kFrameCount = static_cast<std::size_t>(Frame::Count);
constexpr std::size_t kNoteCount = static_cast<std::size_t>(Note::Count);
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

// What the panel says this frame: the settings that take effect at once, and the view they belong to.
struct PanelReading
{
    interior::LiveSettings live;
    interior::SurfaceSettings surface;
    interior::DisplayMode display;
    interior::Fraction split;
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

// The window the crosshair was last left on, which is what a session built from the panel would follow.
[[nodiscard]] std::optional<interior::MonitorHandle> PickedWindow(const ControlPanel& panel) noexcept;

// The command line the start-up page describes, for the session the operator has asked for.
[[nodiscard]] interior::CommandLine RestartCommandLine(const ControlPanel& panel, const interior::Options& options) noexcept;

// Moves the panel's own controls, so the hotkeys and the divider drag stay in step with what it shows.
void ApplyDisplay(const ControlPanel& panel, interior::DisplayMode display) noexcept;
void ApplySplit(const ControlPanel& panel, interior::Fraction split) noexcept;

// True once the operator has closed the panel, which ends the session.
[[nodiscard]] bool IsPanelClosed(const ControlPanel& panel) noexcept;

} // namespace real
