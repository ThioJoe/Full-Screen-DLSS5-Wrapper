#pragma once
#include "effects/real/com.h"
#include "infrastructure/bounded_vector.h"
#include "interior/options.h"

#include <array>

namespace real {

struct FontDeleter
{
    void operator()(HFONT font) const noexcept { ENSURE(::DeleteObject(font) != FALSE); }
};
using UniqueFont = std::unique_ptr<std::remove_pointer_t<HFONT>, FontDeleter>;

// The panel's pages. Everything that takes effect at once is on the first three; the fourth holds what
// only a fresh session can change, and the operator asks for that session with a button.
enum class Page : std::size_t { Model, View, Startup, Count };

// A number the operator sets: a slider to sweep it, a box to type it, arrows to step it.
enum class Field : std::size_t { Intensity, LocalStructure, LocalTone, Skin, MvScaleX, MvScaleY, Split, DepthValue, ResetThreshold, MvLevel, SrPreset, Count };

// A switch the operator flips.
enum class Toggle : std::size_t { NeuralRendering, AutoMask, UiCorrection, DepthInverted, Vsync, CaptureBorder, Topmost, RedirectionBitmap, DebugLayer, Indicator, CubinCache, Count };

// A choice among a few named alternatives.
enum class Group : std::size_t { Compare, Style, Cursor, Motion, NvofGrid, NvofPerf, Sr, Format, LogLevel, Console, Count };

// A set of named alternatives only known once the machine has been looked at: the monitors attached, the
// adapters the system has, the presets the model carries. An empty list has no row at all.
enum class List : std::size_t { Preset, Source, Target, Adapter, Count };

// A path the operator types. Too long and too free for a slider, so it gets a plain box of its own.
enum class Text : std::size_t { LogFile, Count };

constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);
constexpr std::size_t kToggleCount = static_cast<std::size_t>(Toggle::Count);
constexpr std::size_t kGroupCount = static_cast<std::size_t>(Group::Count);
constexpr std::size_t kTextCount = static_cast<std::size_t>(Text::Count);
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

// The panel keeps no state of its own: the controls hold the operator's choices and are read each frame.
// Child windows die with their parent, so only the panel itself and its font own a handle.
struct ControlPanel
{
    UniqueWindow window;
    UniqueFont font;
    UniqueFont iconFont; // the reset buttons wear a glyph rather than a word, so they get their own face
    HWND tabs;
    HWND tooltip;
    HWND restart;
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
    std::array<std::size_t, kListCount> listCounts;
    // Two settings the panel carries but does not show: off, each spoils the picture rather than changing
    // it, so they are the command line's to set and the panel's to pass on unaltered.
    bool displayAffinity;
    bool clickThrough;
};

// What the panel says this frame: the settings that take effect at once, and the view they belong to.
struct PanelReading
{
    interior::LiveSettings live;
    interior::SurfaceSettings surface;
    interior::DisplayMode display;
    interior::Fraction split;
    bool restartWanted;
};

[[nodiscard]] infra::Result<ControlPanel, Error> CreateControlPanel(const interior::Options& options, const interior::LiveSettings& live, interior::DisplayMode display,
                                                                    const PanelLists& lists) noexcept;

// Reads every control and settles any disagreement between a slider, its box and its arrows, writing the
// answer back to all three. A value a unit refuses keeps what `current` holds.
[[nodiscard]] PanelReading ReadControlPanel(const ControlPanel& panel, const interior::LiveSettings& current) noexcept;

// The command line the start-up page describes, for the session the operator has asked for.
[[nodiscard]] interior::CommandLine RestartCommandLine(const ControlPanel& panel, const interior::Options& options) noexcept;

// Moves the panel's own controls, so the hotkeys and the divider drag stay in step with what it shows.
void ApplyDisplay(const ControlPanel& panel, interior::DisplayMode display) noexcept;
void ApplySplit(const ControlPanel& panel, interior::Fraction split) noexcept;

// True once the operator has closed the panel, which ends the session.
[[nodiscard]] bool IsPanelClosed(const ControlPanel& panel) noexcept;

} // namespace real
