#pragma once
#include "effects/real/com.h"
#include "interior/options.h"

#include <array>

namespace real {

struct FontDeleter
{
    void operator()(HFONT font) const noexcept { ENSURE(::DeleteObject(font) != FALSE); }
};
using UniqueFont = std::unique_ptr<std::remove_pointer_t<HFONT>, FontDeleter>;

// One row per number the model reads: a slider to sweep it, a box to type it, arrows to step it.
enum class Field : std::size_t { Split, Preset, Intensity, LocalStructure, LocalTone, Skin, Count };

constexpr std::size_t kFieldCount = static_cast<std::size_t>(Field::Count);
constexpr std::size_t kCheckCount = 3;   // the model on or off, auto mask, UI correction
constexpr std::size_t kDisplayCount = 3; // processed, original, split
constexpr std::size_t kStyleCount = 3;   // standard, natural, cinematic

// The panel keeps no state of its own: the controls hold the operator's choices and are read each frame.
// Child windows die with their parent, so only the panel itself and its font own a handle.
struct ControlPanel
{
    UniqueWindow window;
    UniqueFont font;
    HWND tooltip;
    std::array<HWND, kFieldCount> sliders;
    std::array<HWND, kFieldCount> boxes;
    std::array<HWND, kFieldCount> spins;
    std::array<HWND, kFieldCount> resets;
    std::array<HWND, kCheckCount> checks;
    std::array<HWND, kCheckCount> checkResets;
    std::array<HWND, kDisplayCount> displays;
    std::array<HWND, kStyleCount> styles;
    HWND resetAll;
};

// What the panel says this frame: the model's settings and the view the operator wants.
struct PanelReading
{
    interior::ModelControls controls;
    interior::DisplayMode display;
    interior::Fraction split;
};

[[nodiscard]] infra::Result<ControlPanel, Error> CreateControlPanel(const interior::ModelControls& initial, interior::DisplayMode display) noexcept;

// Reads every control and settles any disagreement between a slider, its box and its arrows, writing the
// answer back to all three. A value a unit refuses keeps what `current` holds.
[[nodiscard]] PanelReading ReadControlPanel(const ControlPanel& panel, const interior::ModelControls& current) noexcept;

// Moves the panel's own controls, so the hotkeys and the divider drag stay in step with what it shows.
void ApplyDisplay(const ControlPanel& panel, interior::DisplayMode display) noexcept;
void ApplySplit(const ControlPanel& panel, interior::Fraction split) noexcept;

// True once the operator has closed the panel, which ends the session.
[[nodiscard]] bool IsPanelClosed(const ControlPanel& panel) noexcept;

} // namespace real
