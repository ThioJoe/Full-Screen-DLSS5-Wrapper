#include "effects/real/panel.h"

#include "effects/real/window.h"

#include "infrastructure/array_util.h"
#include "infrastructure/bounded_string.h"
#include "infrastructure/text.h"
#include "interior/ngx_params.h"

#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <ranges>
#include <string_view>

namespace real {
namespace {

using infra::Fail;
using infra::Result;

constexpr wchar_t kPanelClass[] = L"DlssScreenControlPanel";
constexpr int kScale = 100; // slider steps per unit: the sliders count hundredths
constexpr int kRowHeight = 46;
constexpr int kLabelX = 12;
constexpr int kSliderX = 12;
constexpr int kSliderWidth = 250;
constexpr int kReadoutX = 274;
constexpr int kReadoutWidth = 70;
constexpr int kPanelWidth = 372;
constexpr int kFirstRowY = 10;
constexpr int kCheckHeight = 24;
constexpr std::size_t kSkinSlider = 5;

// Each slider counts hundredths of a unit. Its range says how far the slider reaches, not what the model
// accepts: the command line still takes any finite value.
struct SliderSpec
{
    const wchar_t* label;
    int minimum;
    int maximum;
};

constexpr std::array<SliderSpec, kSliderCount> kSliders{ {
    { L"Preset (1 is the only one this model carries)", 0, 7 },
    { L"Style: 0 standard, 1 natural, 2 cinematic", 0, 2 },
    { L"Intensity", 0, 10 * kScale },
    { L"Local structure", 0, 10 * kScale },
    { L"Local tone", 0, 10 * kScale },
    { L"Skin structure (-1 follows local structure; needs auto mask)", -kScale, 10 * kScale },
} };

[[nodiscard]] int RowY(std::size_t row) noexcept
{
    return kFirstRowY + static_cast<int>(row) * kRowHeight;
}

[[nodiscard]] int PanelHeight() noexcept
{
    return RowY(kSliderCount) + 3 * kCheckHeight + 52;
}

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

[[nodiscard]] HWND CreateChild(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, int x, int y, int width, int height) noexcept
{
    return ::CreateWindowExW(0, className, text, WS_CHILD | WS_VISIBLE | style, x, y, width, height, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

[[nodiscard]] HWND CreateSlider(HWND parent, const SliderSpec& spec, std::size_t row, int position) noexcept
{
    const HWND slider = CreateChild(parent, TRACKBAR_CLASSW, nullptr, TBS_HORZ | TBS_NOTICKS, kSliderX, RowY(row) + 18, kSliderWidth, 26);
    if (slider == nullptr)
        return nullptr;
    ::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(spec.minimum, spec.maximum));
    ::SendMessageW(slider, TBM_SETPOS, TRUE, position);
    return slider;
}

[[nodiscard]] int PositionOf(const interior::NrTuning& t, std::size_t row) noexcept
{
    const std::array<float, kSliderCount> values{
        static_cast<float>(t.preset.Get()), static_cast<float>(interior::StyleCode(t.style)), t.intensity.Get(), t.localStructure.Get(), t.localTone.Get(), t.skinStructure.Get()
    };
    const float scale = row < 2 ? 1.0f : static_cast<float>(kScale);
    return static_cast<int>(std::lround(values[row] * scale));
}

[[nodiscard]] int ClampedToSpec(const SliderSpec& spec, int position) noexcept
{
    return std::clamp(position, spec.minimum, spec.maximum);
}

[[nodiscard]] float ValueOf(const ControlPanel& panel, std::size_t row) noexcept
{
    const auto raw = static_cast<float>(::SendMessageW(panel.sliders[row], TBM_GETPOS, 0, 0));
    return row < 2 ? raw : raw / static_cast<float>(kScale);
}

[[nodiscard]] bool IsChecked(HWND check) noexcept
{
    return ::SendMessageW(check, BM_GETCHECK, 0, 0) == BST_CHECKED;
}

[[nodiscard]] interior::NrStyle StyleFromCode(std::uint32_t code) noexcept
{
    constexpr std::array<interior::NrStyle, 3> styles{ interior::NrStyle::Standard, interior::NrStyle::Natural, interior::NrStyle::Cinematic };
    return styles[std::min<std::size_t>(code, styles.size() - 1)];
}

[[nodiscard]] interior::NrTuning TuningOf(const ControlPanel& panel, const interior::NrTuning& current) noexcept
{
    const interior::NgxPreset preset = interior::NgxPresetTag::Parse(static_cast<std::uint32_t>(ValueOf(panel, 0))).value_or(current.preset);
    const auto strength = [&panel](std::size_t row, interior::Strength held) { return interior::StrengthTag::Parse(ValueOf(panel, row)).value_or(held); };
    const interior::SkinStrength skin = interior::SkinStrengthTag::Parse(ValueOf(panel, kSkinSlider)).value_or(current.skinStructure);
    return interior::NrTuning{ preset,
                               strength(2, current.intensity),
                               StyleFromCode(static_cast<std::uint32_t>(ValueOf(panel, 1))),
                               strength(3, current.localStructure),
                               strength(4, current.localTone),
                               skin,
                               IsChecked(panel.autoMask),
                               IsChecked(panel.uiCorrection) };
}

// The readouts carry digits only, so widening them is a character-for-character copy.
[[nodiscard]] std::array<wchar_t, 16> Widened(std::string_view text) noexcept
{
    std::array<wchar_t, 16> wide{}; // WAIVER(R2): a local buffer filled once, before use.
    std::ranges::copy(text | std::views::take(wide.size() - 1) | std::views::transform([](char c) { return static_cast<wchar_t>(c); }), wide.begin());
    return wide;
}

[[nodiscard]] infra::BoundedString<char, 15> ReadoutText(const ControlPanel& panel, std::size_t row) noexcept
{
    if (row < 2)
        return infra::Formatted<15>("{}", static_cast<int>(ValueOf(panel, row)));
    return infra::Formatted<15>("{:.2f}", ValueOf(panel, row));
}

void RefreshReadout(const ControlPanel& panel, std::size_t row) noexcept
{
    const std::array<wchar_t, 16> text = Widened(ReadoutText(panel, row).Get());
    ENSURE(::SetWindowTextW(panel.readouts[row], text.data()) != FALSE);
}

void RefreshReadouts(const ControlPanel& panel) noexcept
{
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kSliderCount), [&panel](std::size_t row) { RefreshReadout(panel, row); });
}

// Advisory: the older common controls register their classes as they load and refuse this call, while
// version 6 needs asking. Either way the controls are checked once built, which is the answer that counts.
void InitialiseCommonControls() noexcept
{
    INITCOMMONCONTROLSEX controls{ sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES };
    (void)::InitCommonControlsEx(&controls);
}

constexpr DWORD kPanelStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

// The window big enough to hold the controls once its frame is added.
[[nodiscard]] RECT OuterFrame() noexcept
{
    RECT frame{ 0, 0, kPanelWidth, PanelHeight() };
    ENSURE(::AdjustWindowRectEx(&frame, kPanelStyle, FALSE, 0) != FALSE);
    return frame;
}

[[nodiscard]] Result<UniqueWindow, Error> CreatePanelWindow() noexcept
{
    const RECT frame = OuterFrame();
    HWND window = ::CreateWindowExW(WS_EX_TOPMOST, kPanelClass, L"DlssScreen controls", kPanelStyle, CW_USEDEFAULT, CW_USEDEFAULT, frame.right - frame.left, frame.bottom - frame.top, nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr)
        return Fail(LastError(ApiCall::CreateWindowExW));
    return UniqueWindow(window);
}

[[nodiscard]] HWND CreateLabel(HWND parent, const wchar_t* text, int x, int y, int width) noexcept
{
    return CreateChild(parent, WC_STATICW, text, SS_LEFT, x, y, width, 16);
}

[[nodiscard]] WPARAM CheckState(bool checked) noexcept
{
    return checked ? BST_CHECKED : BST_UNCHECKED;
}

[[nodiscard]] HWND Checked(HWND check, bool checked) noexcept
{
    if (check == nullptr)
        return nullptr;
    ::SendMessageW(check, BM_SETCHECK, CheckState(checked), 0);
    return check;
}

[[nodiscard]] HWND CreateCheck(HWND parent, const wchar_t* text, std::size_t index, bool checked) noexcept
{
    return Checked(CreateChild(parent, WC_BUTTONW, text, BS_AUTOCHECKBOX, kLabelX, RowY(kSliderCount) + static_cast<int>(index) * kCheckHeight, kPanelWidth - 2 * kLabelX, kCheckHeight), checked);
}

// Every control of the panel, in the order the operator reads them.
[[nodiscard]] ControlPanel Assembled(UniqueWindow window, const interior::ModelControls& initial) noexcept
{
    HWND parent = window.get();
    const auto slider = [parent, &initial](std::size_t row) {
        return CreateLabel(parent, kSliders[row].label, kLabelX, RowY(row), kSliderWidth) == nullptr
                   ? nullptr
                   : CreateSlider(parent, kSliders[row], row, ClampedToSpec(kSliders[row], PositionOf(initial.tuning, row)));
    };
    const auto readout = [parent](std::size_t row) { return CreateLabel(parent, L"", kReadoutX, RowY(row) + 22, kReadoutWidth); };
    return ControlPanel{ std::move(window),
                         infra::Generated<HWND, kSliderCount>(slider),
                         infra::Generated<HWND, kSliderCount>(readout),
                         CreateCheck(parent, L"Neural rendering", 0, initial.neuralRendering),
                         CreateCheck(parent, L"Auto mask", 1, initial.tuning.autoMask),
                         CreateCheck(parent, L"UI correction", 2, initial.tuning.uiCorrection) };
}

[[nodiscard]] bool IsPresent(HWND control) noexcept
{
    return control != nullptr;
}

[[nodiscard]] std::array<HWND, 3> ChecksOf(const ControlPanel& panel) noexcept
{
    return { panel.neuralRendering, panel.autoMask, panel.uiCorrection };
}

[[nodiscard]] bool HasRows(const ControlPanel& panel) noexcept
{
    return std::ranges::all_of(panel.sliders, IsPresent) && std::ranges::all_of(panel.readouts, IsPresent);
}

[[nodiscard]] bool IsComplete(const ControlPanel& panel) noexcept
{
    return HasRows(panel) && std::ranges::all_of(ChecksOf(panel), IsPresent);
}

// The panel is kept out of the capture, or it would photograph the picture it is controlling.
[[nodiscard]] Result<ControlPanel, Error> Populated(UniqueWindow window, const interior::ModelControls& initial) noexcept
{
    ControlPanel panel = Assembled(std::move(window), initial);
    if (!IsComplete(panel))
        return Fail(LastError(ApiCall::CreateWindowExW));
    return CheckBool(::SetWindowDisplayAffinity(panel.window.get(), WDA_EXCLUDEFROMCAPTURE), ApiCall::SetWindowDisplayAffinity).transform([&panel] {
        ::ShowWindow(panel.window.get(), SW_SHOWNOACTIVATE);
        return std::move(panel);
    });
}

} // namespace

Result<ControlPanel, Error> CreateControlPanel(const interior::ModelControls& initial) noexcept
{
    InitialiseCommonControls();
    return RegisterWindowClass(ClassDescription()).and_then([&initial] { return CreatePanelWindow().and_then([&initial](UniqueWindow window) { return Populated(std::move(window), initial); }); });
}

interior::ModelControls ReadControlPanel(const ControlPanel& panel, const interior::ModelControls& current) noexcept
{
    RefreshReadouts(panel);
    return interior::ModelControls{ IsChecked(panel.neuralRendering), TuningOf(panel, current.tuning) };
}

bool IsPanelClosed(const ControlPanel& panel) noexcept
{
    return ::IsWindowVisible(panel.window.get()) == FALSE;
}

} // namespace real
