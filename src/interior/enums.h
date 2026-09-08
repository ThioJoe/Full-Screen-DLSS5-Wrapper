#pragma once
#include <cstdint>

namespace interior {

enum class MonitorSelectionKind : std::uint8_t { Primary, All, Index };
enum class CursorMode : std::uint8_t { Auto, On, Off };
enum class SrMode : std::uint8_t { Auto, Dlaa, Off };
enum class MotionBackend : std::uint8_t { BuiltIn, NvOpticalFlow, None };
enum class CompareMode : std::uint8_t { Off, Split, Original };
enum class ColorFormat : std::uint8_t { Rgba8, Rgba16f };
enum class NrStyle : std::uint8_t { Standard, Natural, Cinematic };
enum class GridSize : std::uint8_t { One, Two, Four };
enum class PerfLevel : std::uint8_t { Slow, Medium, Fast };
enum class NgxLogLevel : std::uint8_t { Off, On, Verbose };
enum class LogLevel : std::uint8_t { Debug, Info, Warn, Error };
enum class SrQuality : std::uint8_t { Dlaa, UltraQuality, Quality, Balanced, Performance, UltraPerformance };
enum class DisplayMode : std::uint8_t { Processed, Original, Split };

} // namespace interior
