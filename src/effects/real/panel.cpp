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
constexpr float kDefaultSplit = 0.5f;
constexpr auto kCentre = interior::FractionTag::Parse(kDefaultSplit);
static_assert(kCentre.has_value());
constexpr std::size_t kRadioCount = 3;
static_assert(kDisplayCount == kRadioCount && kStyleCount == kRadioCount);

// The layout in reference pixels; every one of them is scaled to the panel's own dots per inch.
constexpr int kMargin = 12;
constexpr int kPanelWidth = 404;
constexpr int kLabelHeight = 17;
constexpr int kControlHeight = 24;
constexpr int kFieldRow = 46;
constexpr int kCheckRow = 26;
constexpr int kGroupRow = 46;
constexpr int kSliderWidth = 214;
constexpr int kBoxLeft = 236;
constexpr int kBoxWidth = 84;
constexpr int kResetLeft = 330;
constexpr int kResetWidth = 62;
constexpr int kRadioWidth = 124;

struct FieldSpec
{
    const wchar_t* label;
    const wchar_t* hint;
    int minimum; // in steps
    int maximum;
    int steps; // steps per unit: 1 counts whole numbers, 100 counts hundredths
};

constexpr std::array<FieldSpec, kFieldCount> kFields{ {
    { L"Split position", L"Where the divider sits in the split view. Ctrl+Alt+Shift and the mouse drags it on screen.", 0, 100, 100 },
    { L"Preset", L"Which weights to ask the model for. This build of the model carries preset 1 only; anything else falls back to it.", 0, 7, 1 },
    { L"Intensity", L"How much of the model's work to keep. The model applies no limit of its own; it was authored around 0 to 1.", 0, 1000, 100 },
    { L"Local structure", L"Detail the model adds within a region. Does nothing while auto mask is off.", 0, 1000, 100 },
    { L"Local tone", L"How far the model moves local brightness.", 0, 1000, 100 },
    { L"Skin structure", L"Detail on skin; -1 follows local structure. Does nothing while auto mask is off.", -100, 1000, 100 },
} };

constexpr std::array<const wchar_t*, kCheckCount> kCheckLabels{ L"Neural rendering", L"Auto mask", L"UI correction" };
constexpr std::array<const wchar_t*, kCheckCount> kCheckHints{
    L"Run the model at all. Off shows the captured picture untouched.",
    L"Let the model find skin itself. Skin structure and local structure do nothing while this is off.",
    L"Ask the model to leave interface pixels alone. It reads a UI layer DlssScreen does not supply, so this is inert as wired.",
};

constexpr std::array<const wchar_t*, kDisplayCount> kDisplayLabels{ L"Processed", L"Original", L"Split" };
constexpr std::array<const wchar_t*, kStyleCount> kStyleLabels{ L"Standard", L"Natural", L"Cinematic" };

// Rows down the panel, each with its own height, so the numbers and the switches sit at their own pace.
enum class Row : std::size_t { View, Split, Model, Preset, Style, Intensity, LocalStructure, LocalTone, Skin, AutoMask, UiCorrection, ResetAll, Count };

constexpr std::array<int, static_cast<std::size_t>(Row::Count)> kRowHeights{ kGroupRow, kFieldRow, kCheckRow, kFieldRow, kGroupRow, kFieldRow,
                                                                             kFieldRow, kFieldRow, kFieldRow, kCheckRow, kCheckRow, kFieldRow };

[[nodiscard]] int RowTop(Row row) noexcept
{
    return std::ranges::fold_left(kRowHeights | std::views::take(static_cast<std::size_t>(row)), kMargin, std::plus<int>{});
}

[[nodiscard]] int PanelHeight() noexcept
{
    return RowTop(Row::Count) + kMargin;
}

[[nodiscard]] Row RowOfField(std::size_t field) noexcept
{
    constexpr std::array<Row, kFieldCount> rows{ Row::Split, Row::Preset, Row::Intensity, Row::LocalStructure, Row::LocalTone, Row::Skin };
    return rows[field];
}

[[nodiscard]] Row RowOfCheck(std::size_t check) noexcept
{
    constexpr std::array<Row, kCheckCount> rows{ Row::Model, Row::AutoMask, Row::UiCorrection };
    return rows[check];
}

// Everything the panel draws is measured in the display's own dots, so it is the same size everywhere.
struct Metrics
{
    int dpi;
    [[nodiscard]] int Of(int reference) const noexcept { return ::MulDiv(reference, dpi, kReferenceDpi); }
};

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
    INITCOMMONCONTROLSEX controls{ sizeof(INITCOMMONCONTROLSEX), ICC_BAR_CLASSES | ICC_STANDARD_CLASSES | ICC_UPDOWN_CLASS };
    (void)::InitCommonControlsEx(&controls);
}

// The font the rest of Windows writes its dialogs in, at the panel's own scale.
[[nodiscard]] UniqueFont MessageFont(const Metrics& m) noexcept
{
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(NONCLIENTMETRICSW);
    ENSURE(::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(NONCLIENTMETRICSW), &metrics, 0) != FALSE);
    metrics.lfMessageFont.lfHeight = -m.Of(-static_cast<int>(metrics.lfMessageFont.lfHeight));
    return UniqueFont(::CreateFontIndirectW(&metrics.lfMessageFont));
}

[[nodiscard]] HWND CreateChild(HWND parent, const wchar_t* className, const wchar_t* text, DWORD style, DWORD extended, RECT bounds) noexcept
{
    return ::CreateWindowExW(extended, className, text, WS_CHILD | WS_VISIBLE | style, bounds.left, bounds.top, bounds.right, bounds.bottom, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
}

[[nodiscard]] RECT Bounds(const Metrics& m, int x, int y, int width, int height) noexcept
{
    return RECT{ m.Of(x), m.Of(y), m.Of(width), m.Of(height) };
}

[[nodiscard]] HWND CreateLabel(HWND parent, const Metrics& m, const wchar_t* text, int x, int y, int width) noexcept
{
    return CreateChild(parent, WC_STATICW, text, SS_LEFT, 0, Bounds(m, x, y, width, kLabelHeight));
}

[[nodiscard]] HWND CreateButton(HWND parent, const Metrics& m, const wchar_t* text, DWORD style, int x, int y, int width) noexcept
{
    return CreateChild(parent, WC_BUTTONW, text, style, 0, Bounds(m, x, y, width, kControlHeight));
}

// --- the tooltip -------------------------------------------------------------------------------------

[[nodiscard]] HWND CreateTooltip(HWND parent) noexcept
{
    return ::CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, parent, nullptr,
                             ::GetModuleHandleW(nullptr), nullptr);
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

// --- reading and writing a number --------------------------------------------------------------------

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
    return std::clamp(static_cast<int>(std::lround(value * static_cast<float>(spec.steps))), spec.minimum, spec.maximum);
}

// The slider, the box and the arrows are three ways to say the same number, so whichever moved decides.
[[nodiscard]] int Settled(const ControlPanel& panel, std::size_t field) noexcept
{
    const int slider = SliderPosition(panel.sliders[field]);
    const int spin = SpinPosition(panel.spins[field]);
    if (slider != spin)
        return slider;
    return TypedSteps(panel.boxes[field], kFields[field]).value_or(spin);
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

// The box is left alone while it has the keyboard, or the operator's half-typed number would be rewritten.
void ShowInBox(HWND box, int steps, const FieldSpec& spec) noexcept
{
    if (::GetFocus() == box)
        return;
    ENSURE(::SetWindowTextW(box, Widened(Printed(steps, spec).Get()).data()) != FALSE);
}

void Commit(const ControlPanel& panel, std::size_t field, int steps) noexcept
{
    (void)::SendMessageW(panel.sliders[field], TBM_SETPOS, TRUE, steps);
    (void)::SendMessageW(panel.spins[field], UDM_SETPOS32, 0, steps);
    ShowInBox(panel.boxes[field], steps, kFields[field]);
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

[[nodiscard]] std::size_t CheckedIn(std::span<const HWND> group, std::size_t fallback) noexcept
{
    const auto found = std::ranges::find_if(group, IsChecked);
    return found == group.end() ? fallback : static_cast<std::size_t>(std::ranges::distance(group.begin(), found));
}

void CheckOnly(std::span<const HWND> group, std::size_t index) noexcept
{
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, group.size()), [group, index](std::size_t i) { SetChecked(group[i], i == index); });
}

// --- defaults ----------------------------------------------------------------------------------------

[[nodiscard]] int StepsOf(float value, const FieldSpec& spec) noexcept
{
    return std::clamp(static_cast<int>(std::lround(value * static_cast<float>(spec.steps))), spec.minimum, spec.maximum);
}

[[nodiscard]] std::array<float, kFieldCount> FieldValues(const interior::NrTuning& t, float split) noexcept
{
    return { split, static_cast<float>(t.preset.Get()), t.intensity.Get(), t.localStructure.Get(), t.localTone.Get(), t.skinStructure.Get() };
}

[[nodiscard]] int DefaultSteps(std::size_t field) noexcept
{
    return StepsOf(FieldValues(interior::DefaultOptions().tuning, kDefaultSplit)[field], kFields[field]);
}

[[nodiscard]] std::array<bool, kCheckCount> CheckValues(const interior::ModelControls& controls) noexcept
{
    return { controls.neuralRendering, controls.tuning.autoMask, controls.tuning.uiCorrection };
}

// --- building the panel ------------------------------------------------------------------------------

struct Row2
{
    int top;
    int control;
};

[[nodiscard]] Row2 RowOf(Row row) noexcept
{
    return Row2{ RowTop(row), RowTop(row) + kLabelHeight };
}

[[nodiscard]] HWND CreateSlider(HWND parent, const Metrics& m, const FieldSpec& spec, Row row, int steps) noexcept
{
    const HWND slider = CreateChild(parent, TRACKBAR_CLASSW, nullptr, TBS_HORZ | TBS_NOTICKS, 0, Bounds(m, kMargin, RowOf(row).control, kSliderWidth, kControlHeight));
    if (slider == nullptr)
        return nullptr;
    (void)::SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(spec.minimum, spec.maximum));
    (void)::SendMessageW(slider, TBM_SETPOS, TRUE, steps);
    return slider;
}

[[nodiscard]] HWND CreateBox(HWND parent, const Metrics& m, Row row) noexcept
{
    return CreateChild(parent, WC_EDITW, L"", ES_LEFT | ES_AUTOHSCROLL | WS_TABSTOP, WS_EX_CLIENTEDGE, Bounds(m, kBoxLeft, RowOf(row).control, kBoxWidth, kControlHeight));
}

[[nodiscard]] HWND ArrangedSpin(HWND spin, HWND box, const FieldSpec& spec, int steps) noexcept
{
    (void)::SendMessageW(spin, UDM_SETBUDDY, reinterpret_cast<WPARAM>(box), 0);
    (void)::SendMessageW(spin, UDM_SETRANGE32, static_cast<WPARAM>(spec.minimum), static_cast<LPARAM>(spec.maximum));
    (void)::SendMessageW(spin, UDM_SETPOS32, 0, steps);
    return spin;
}

[[nodiscard]] HWND CreateSpin(HWND parent, HWND box, const FieldSpec& spec, int steps) noexcept
{
    const HWND spin =
        ::CreateWindowExW(0, UPDOWN_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | UDS_ALIGNRIGHT | UDS_ARROWKEYS | UDS_NOTHOUSANDS, 0, 0, 0, 0, parent, nullptr, ::GetModuleHandleW(nullptr), nullptr);
    return spin == nullptr ? nullptr : ArrangedSpin(spin, box, spec, steps);
}

[[nodiscard]] std::array<HWND, kFieldCount> CreateFieldLabels(HWND parent, const Metrics& m) noexcept
{
    return infra::Generated<HWND, kFieldCount>([parent, &m](std::size_t f) { return CreateLabel(parent, m, kFields[f].label, kMargin, RowOf(RowOfField(f)).top, kPanelWidth - 2 * kMargin); });
}

[[nodiscard]] std::array<HWND, kFieldCount> CreateSliders(HWND parent, const Metrics& m, const std::array<int, kFieldCount>& steps) noexcept
{
    return infra::Generated<HWND, kFieldCount>([parent, &m, &steps](std::size_t f) { return CreateSlider(parent, m, kFields[f], RowOfField(f), steps[f]); });
}

[[nodiscard]] std::array<HWND, kFieldCount> CreateBoxes(HWND parent, const Metrics& m) noexcept
{
    return infra::Generated<HWND, kFieldCount>([parent, &m](std::size_t f) { return CreateBox(parent, m, RowOfField(f)); });
}

[[nodiscard]] std::array<HWND, kFieldCount> CreateSpins(HWND parent, const std::array<HWND, kFieldCount>& boxes, const std::array<int, kFieldCount>& steps) noexcept
{
    return infra::Generated<HWND, kFieldCount>([parent, &boxes, &steps](std::size_t f) { return CreateSpin(parent, boxes[f], kFields[f], steps[f]); });
}

[[nodiscard]] std::array<HWND, kFieldCount> CreateFieldResets(HWND parent, const Metrics& m) noexcept
{
    return infra::Generated<HWND, kFieldCount>([parent, &m](std::size_t f) { return CreateButton(parent, m, L"Reset", 0, kResetLeft, RowOf(RowOfField(f)).control, kResetWidth); });
}

[[nodiscard]] std::array<HWND, kCheckCount> CreateChecks(HWND parent, const Metrics& m, const interior::ModelControls& initial) noexcept
{
    const std::array<bool, kCheckCount> values = CheckValues(initial);
    return infra::Generated<HWND, kCheckCount>([parent, &m, &values](std::size_t c) {
        const HWND check = CreateButton(parent, m, kCheckLabels[c], BS_AUTOCHECKBOX, kMargin, RowOf(RowOfCheck(c)).top, kBoxLeft - kMargin);
        if (check != nullptr)
            SetChecked(check, values[c]);
        return check;
    });
}

[[nodiscard]] std::array<HWND, kCheckCount> CreateCheckResets(HWND parent, const Metrics& m) noexcept
{
    return infra::Generated<HWND, kCheckCount>([parent, &m](std::size_t c) { return CreateButton(parent, m, L"Reset", 0, kResetLeft, RowOf(RowOfCheck(c)).top, kResetWidth); });
}

[[nodiscard]] std::array<HWND, kRadioCount> CreateRadios(HWND parent, const Metrics& m, std::span<const wchar_t* const> labels, Row row, std::size_t checked) noexcept
{
    const std::array<HWND, kRadioCount> group = infra::Generated<HWND, kRadioCount>([parent, &m, labels, row](std::size_t i) {
        const DWORD style = BS_AUTORADIOBUTTON | (i == 0 ? WS_GROUP : 0u);
        return CreateButton(parent, m, labels[i], style, kMargin + static_cast<int>(i) * kRadioWidth, RowOf(row).control, kRadioWidth);
    });
    if (std::ranges::all_of(group, [](HWND h) { return h != nullptr; }))
        CheckOnly(group, checked);
    return group;
}

void ApplyFont(HWND control, HFONT font) noexcept
{
    if (control != nullptr)
        (void)::SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

// --- assembling and reading --------------------------------------------------------------------------

void CreateHeadings(HWND parent, const Metrics& m) noexcept
{
    (void)CreateFieldLabels(parent, m);
    (void)CreateLabel(parent, m, L"View", kMargin, RowOf(Row::View).top, kPanelWidth - 2 * kMargin);
    (void)CreateLabel(parent, m, L"Style", kMargin, RowOf(Row::Style).top, kPanelWidth - 2 * kMargin);
}

[[nodiscard]] std::array<int, kFieldCount> StartingSteps(const interior::ModelControls& initial, float split) noexcept
{
    return infra::Generated<int, kFieldCount>([&initial, split](std::size_t f) { return StepsOf(FieldValues(initial.tuning, split)[f], kFields[f]); });
}

[[nodiscard]] ControlPanel Assembled(UniqueWindow window, UniqueFont font, const Metrics& m, const interior::ModelControls& initial, interior::DisplayMode display) noexcept
{
    HWND parent = window.get();
    const std::array<int, kFieldCount> steps = StartingSteps(initial, kDefaultSplit);
    const std::array<HWND, kFieldCount> boxes = CreateBoxes(parent, m);
    CreateHeadings(parent, m);
    return ControlPanel{ std::move(window),
                         std::move(font),
                         CreateTooltip(parent),
                         CreateSliders(parent, m, steps),
                         boxes,
                         CreateSpins(parent, boxes, steps),
                         CreateFieldResets(parent, m),
                         CreateChecks(parent, m, initial),
                         CreateCheckResets(parent, m),
                         CreateRadios(parent, m, kDisplayLabels, Row::View, static_cast<std::size_t>(display)),
                         CreateRadios(parent, m, kStyleLabels, Row::Style, interior::StyleCode(initial.tuning.style)),
                         CreateButton(parent, m, L"Reset everything", 0, kMargin, RowOf(Row::ResetAll).control, 2 * kRadioWidth) };
}

[[nodiscard]] bool IsPresent(HWND control) noexcept
{
    return control != nullptr;
}

[[nodiscard]] bool AllPresent(std::span<const HWND> controls) noexcept
{
    return std::ranges::all_of(controls, IsPresent);
}

[[nodiscard]] std::array<std::span<const HWND>, 9> GroupsOf(const ControlPanel& panel) noexcept
{
    return { panel.sliders, panel.boxes, panel.spins, panel.resets, panel.checks, panel.checkResets, panel.displays, panel.styles, std::span<const HWND>(&panel.resetAll, 1) };
}

[[nodiscard]] bool IsComplete(const ControlPanel& panel) noexcept
{
    return std::ranges::all_of(GroupsOf(panel), AllPresent);
}

void DressPanel(const ControlPanel& panel) noexcept
{
    HFONT font = panel.font.get();
    // WAIVER(R2): walking the panel's own children to give them the system font and their hints.
    ::EnumChildWindows(
        panel.window.get(),
        [](HWND child, LPARAM f) -> BOOL {
            ApplyFont(child, reinterpret_cast<HFONT>(f));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font));
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel](std::size_t f) { AddHint(panel.tooltip, panel.window.get(), panel.sliders[f], kFields[f].hint); });
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel](std::size_t f) { AddHint(panel.tooltip, panel.window.get(), panel.boxes[f], kFields[f].hint); });
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kCheckCount), [&panel](std::size_t c) { AddHint(panel.tooltip, panel.window.get(), panel.checks[c], kCheckHints[c]); });
}

[[nodiscard]] float SettledValue(const ControlPanel& panel, std::size_t field) noexcept
{
    const int steps = Settled(panel, field);
    Commit(panel, field, steps);
    return static_cast<float>(steps) / static_cast<float>(kFields[field].steps);
}

[[nodiscard]] interior::NrStyle StyleFromCode(std::size_t code) noexcept
{
    constexpr std::array<interior::NrStyle, kStyleCount> styles{ interior::NrStyle::Standard, interior::NrStyle::Natural, interior::NrStyle::Cinematic };
    return styles[std::min(code, styles.size() - 1)];
}

[[nodiscard]] interior::DisplayMode DisplayFromIndex(std::size_t index) noexcept
{
    constexpr std::array<interior::DisplayMode, kDisplayCount> modes{ interior::DisplayMode::Processed, interior::DisplayMode::Original, interior::DisplayMode::Split };
    return modes[std::min(index, modes.size() - 1)];
}

void ResetHeldFields(const ControlPanel& panel, bool all) noexcept
{
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kFieldCount), [&panel, all](std::size_t f) {
        if (all || IsPushed(panel.resets[f]))
            Commit(panel, f, DefaultSteps(f));
    });
}

void ResetHeldChecks(const ControlPanel& panel, bool all) noexcept
{
    const std::array<bool, kCheckCount> values = CheckValues(interior::ModelControls{ interior::DefaultOptions().neuralRendering, interior::DefaultOptions().tuning });
    std::ranges::for_each(std::views::iota(std::size_t{ 0 }, kCheckCount), [&panel, &values, all](std::size_t c) {
        if (all || IsPushed(panel.checkResets[c]))
            SetChecked(panel.checks[c], values[c]);
    });
}

// A held reset button puts its control back where it started; resetting is the same answer every frame.
void ApplyResets(const ControlPanel& panel) noexcept
{
    const bool all = IsPushed(panel.resetAll);
    ResetHeldFields(panel, all);
    ResetHeldChecks(panel, all);
    if (all)
        CheckOnly(panel.styles, interior::StyleCode(interior::DefaultOptions().tuning.style));
}

[[nodiscard]] interior::NrTuning TuningOf(const ControlPanel& panel, const interior::NrTuning& current) noexcept
{
    const interior::NgxPreset preset = interior::NgxPresetTag::Parse(static_cast<std::uint32_t>(SettledValue(panel, 1))).value_or(current.preset);
    const auto strength = [&panel](std::size_t field, interior::Strength held) { return interior::StrengthTag::Parse(SettledValue(panel, field)).value_or(held); };
    const interior::SkinStrength skin = interior::SkinStrengthTag::Parse(SettledValue(panel, 5)).value_or(current.skinStructure);
    return interior::NrTuning{ preset,
                               strength(2, current.intensity),
                               StyleFromCode(CheckedIn(panel.styles, interior::StyleCode(current.style))),
                               strength(3, current.localStructure),
                               strength(4, current.localTone),
                               skin,
                               IsChecked(panel.checks[1]),
                               IsChecked(panel.checks[2]) };
}

constexpr DWORD kPanelStyle = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;

[[nodiscard]] Result<UniqueWindow, Error> CreatePanelWindow() noexcept
{
    HWND window = ::CreateWindowExW(WS_EX_TOPMOST, kPanelClass, L"DlssScreen controls", kPanelStyle, CW_USEDEFAULT, CW_USEDEFAULT, kPanelWidth, PanelHeight(), nullptr, nullptr,
                                    ::GetModuleHandleW(nullptr), nullptr);
    if (window == nullptr)
        return Fail(LastError(ApiCall::CreateWindowExW));
    return UniqueWindow(window);
}

// The window is made at a nominal size, then measured and resized in the dots its own display uses.
[[nodiscard]] Metrics MetricsOf(HWND window) noexcept
{
    return Metrics{ static_cast<int>(::GetDpiForWindow(window)) };
}

void ResizeToFit(HWND window, const Metrics& m) noexcept
{
    RECT frame{ 0, 0, m.Of(kPanelWidth), m.Of(PanelHeight()) };
    ENSURE(::AdjustWindowRectExForDpi(&frame, kPanelStyle, FALSE, 0, static_cast<UINT>(m.dpi)) != FALSE);
    ENSURE(::SetWindowPos(window, nullptr, 0, 0, frame.right - frame.left, frame.bottom - frame.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE) != FALSE);
}

// The panel is kept out of the capture, or it would photograph the picture it is controlling.
[[nodiscard]] Result<ControlPanel, Error> Shown(ControlPanel panel) noexcept
{
    if (!IsComplete(panel))
        return Fail(LastError(ApiCall::CreateWindowExW));
    DressPanel(panel);
    return CheckBool(::SetWindowDisplayAffinity(panel.window.get(), WDA_EXCLUDEFROMCAPTURE), ApiCall::SetWindowDisplayAffinity).transform([&panel] {
        ::ShowWindow(panel.window.get(), SW_SHOWNOACTIVATE);
        return std::move(panel);
    });
}

[[nodiscard]] Result<ControlPanel, Error> Populated(UniqueWindow window, const interior::ModelControls& initial, interior::DisplayMode display) noexcept
{
    const Metrics m = MetricsOf(window.get());
    ResizeToFit(window.get(), m);
    return Shown(Assembled(std::move(window), MessageFont(m), m, initial, display));
}

} // namespace

Result<ControlPanel, Error> CreateControlPanel(const interior::ModelControls& initial, interior::DisplayMode display) noexcept
{
    InitialiseCommonControls();
    return RegisterWindowClass(ClassDescription()).and_then([&initial, display] {
        return CreatePanelWindow().and_then([&initial, display](UniqueWindow window) { return Populated(std::move(window), initial, display); });
    });
}

PanelReading ReadControlPanel(const ControlPanel& panel, const interior::ModelControls& current) noexcept
{
    ApplyResets(panel);
    const interior::Fraction split = interior::FractionTag::Parse(SettledValue(panel, 0)).value_or(*kCentre);
    return PanelReading{ interior::ModelControls{ IsChecked(panel.checks[0]), TuningOf(panel, current.tuning) }, DisplayFromIndex(CheckedIn(panel.displays, 0)), split };
}

void ApplyDisplay(const ControlPanel& panel, interior::DisplayMode display) noexcept
{
    CheckOnly(panel.displays, static_cast<std::size_t>(display));
}

void ApplySplit(const ControlPanel& panel, interior::Fraction split) noexcept
{
    Commit(panel, 0, StepsOf(split.Get(), kFields[0]));
}

bool IsPanelClosed(const ControlPanel& panel) noexcept
{
    return ::IsWindowVisible(panel.window.get()) == FALSE;
}

} // namespace real
