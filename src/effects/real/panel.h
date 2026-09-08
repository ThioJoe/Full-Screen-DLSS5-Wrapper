#pragma once
#include "effects/real/com.h"
#include "interior/options.h"

#include <array>

namespace real {

// The control panel: an ordinary window whose controls hold the operator's choices. It keeps no state
// of its own, so each frame the values are read back out of the controls themselves.
constexpr std::size_t kSliderCount = 6;

// Child windows die with their parent, so only the panel itself owns a handle.
struct ControlPanel
{
    UniqueWindow window;
    std::array<HWND, kSliderCount> sliders;
    std::array<HWND, kSliderCount> readouts;
    HWND neuralRendering;
    HWND autoMask;
    HWND uiCorrection;
};

[[nodiscard]] infra::Result<ControlPanel, Error> CreateControlPanel(const interior::ModelControls& initial) noexcept;

// What the controls say now, with the number beside each slider refreshed to match. A reading a unit
// will not accept keeps the value in `current`, so the session never loses its footing over a slider.
[[nodiscard]] interior::ModelControls ReadControlPanel(const ControlPanel& panel, const interior::ModelControls& current) noexcept;

// True once the operator has closed the panel, which ends the session.
[[nodiscard]] bool IsPanelClosed(const ControlPanel& panel) noexcept;

} // namespace real
