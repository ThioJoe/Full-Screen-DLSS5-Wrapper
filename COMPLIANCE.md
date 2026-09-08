# Compliance with the Rules for AI-Written Code

This document maps every rule to the way the code meets it and to the check that enforces it.
"Lint" means `gate/rules_lint.cpp`, which the gate runs on every commit; "compiler" means the
warnings-as-errors configuration in `CMakeLists.txt`; "gate" means `gate/gate.sh` / `gate/gate.ps1`.

## Architecture

| Layer | Directory | Allowed to |
| --- | --- | --- |
| infrastructure | `src/infrastructure` | define generic types and the only metaprogramming: results, strong types, bounded containers, checked arithmetic, contracts, tracing |
| interior | `src/interior` | compute: options, monitor geometry, the session plan, the per-frame step plan, NGX parameter lists. No OS or vendor header, no exception, no effect |
| simulated effects | `src/effects/sim` | interpret step plans against a seeded model of the display, capture, GPU and NGX, injecting failures |
| real effects | `src/effects/real` | talk to Direct3D 12, DirectComposition, Windows Graphics Capture, NGX, the optical flow engine, the clock and the console |
| app | `src/app` | the generic session loop (`session.h`) and the composition root (`main.cpp`) |
| tests | `tests` | property tests and the seed fuzzer, portable |
| gate | `gate` | the lint, mutation testing, the dependency lock and the gate scripts |

The interior never sees an effect: it turns a `FrameState` and a `FrameInput` into a `FramePlan`, a
bounded list of steps (`Transition`, `Dispatch`, `CopyBuffer`, `ClearTarget`, `EvaluateSr`,
`EvaluateNr`, `Draw`, `Submit`, `Present`) and the next state. `app::RunSession` is generic over the
environment; `real::RealEnvironment` records the steps into Direct3D 12 command lists and
`sim::SimEnvironment` checks them against a resource-state model. The same loop runs both.

## Rule by rule

| Rule | How the code meets it | Enforced by |
| --- | --- | --- |
| R1 One operation per function | Functions have at most five statements and one branch; every behaviour-changing condition is a named predicate; compositions are `and_then`/`transform` chains of named calls. A `switch` or `std::visit` over a closed set counts as one match. | Lint R1 (statement count, cyclomatic complexity, inline compound conditions) |
| R2 Single assignment, no mutation | Bindings are `const`; state is rebuilt with `WithX` constructors (`infra::WithElement`, `BoundedVector::Push`); iteration is `std::ranges::fold_left`, `infra::FoldResult`, `infra::ForEach`, `std::views::iota` and `infra::Generated`. The waivered exceptions are listed below. | Lint R2 (loop statements, compound assignment, increment) |
| R3 The call graph is static | No recursion, no virtual dispatch in domain code, no reflection or code loading. OS callbacks (`WindowProc`, `EnumDisplayMonitors`) and the NVIDIA function tables are external and stay in `src/effects/real`. `nvofapi64.dll` is loaded by name from the driver because NVIDIA ships no import library; the entry point is bound once at start-up. | Lint R3 (cycles in the call graph over uniquely named functions); COM and vendor interfaces are the effect boundary |
| R4 Names are specifications | A changed behaviour is a new function with a new name and every caller repointed; there is no `V2`. | Review; the commit history of this branch |
| R5 No pass-through functions | A function forwarding its arguments unchanged to one call does not exist; constructors that bind a constant or narrow a type do. | Lint R5 |
| R6 No unreferenced code | Every function is reachable from `main`, the OS callbacks or the test suites. | Lint R6 |
| R7 No duplicated logic | Bodies are compared token by token with called names normalised; one rule, one function. | Lint R7 (one waiver: the two interpreters of the step variant) |
| R8 No primitive types in interior signatures | Untrusted input becomes a domain type once, at the edge, through `Parse` functions that are the only constructors of `infra::Strong` types (`PixelCount`, `Coordinate`, `Fraction`, `Scale`, `LevelIndex`, ...). Interior functions never re-check. | Private constructors and `friend Tag` in `strong.h`; lint R8 (no type punning outside the effect layer) |
| R9 Distinct meanings get distinct types | `FrameNumber`, `FenceValue`, `Instant`, `Microseconds`, `ByteCount`, `PixelCount`, `Coordinate`, `MonitorIndex`, `RequestedMonitor`, `SetIndex`, `LevelIndex`, `FrameSlot`, `BackBufferIndex` are separate tags; a transposition does not compile. | Compiler |
| R10 Effects are declared in the type | C++ has no effect system, so the module boundary stands in: interior and infrastructure code cannot include an OS, vendor or effect header; every effectful function lives in `src/effects/real`, returns a `Result`, and receives its device, window and console explicitly. | Lint R10 (import lint) |
| R11 Dependencies arrive through a declared channel | The composition root builds the `Options`, `Geometry`, `SessionPlan`, `GpuDevice`, `NgxRuntime`, `Gpu` and `Console` once and passes them by value or const reference; no globals, singletons or ambient state. | Lint R11 (module-level mutable state, static locals); one waiver: the trace ring |
| R12 Exhaustive matching | Every `switch` over an internal enum lists every enumerator without `default`; `std::visit` over the step variant handles every alternative. | Compiler (`-Wswitch-enum`, MSVC C4062 as errors); lint R12 (wildcard arms); one waiver: the OS message switch |
| R13 Checked arithmetic | Unbounded values use `infra::CheckedAdd/Sub/Mul/Div` (compiler builtins or `intsafe.h`); everything else is a bounded type whose range is stated by its parser (`kMaxPixelCount`, `kMaxLevels`, `kMaxMonitors`, `kFrameLimit`). | Compiler (`-Wconversion`, `-Wsign-conversion`); property tests of the checked operations |
| R14 Errors are values | Every fallible function returns `std::expected` (`infra::Result`, `infra::Status`) with an enumerated error; there is no `throw`, `try`, `catch`, `new` or throwing accessor in the code. | Lint R14; `noexcept` on every function |
| R15 Two outcomes, no fallbacks | A failure stops the program with its `ApiCall` and code. Nothing substitutes for a failed operation: a missing optical-flow build, an unavailable NGX feature or an unsupported adapter is an error, not a passthrough. Running without a model is only possible through explicit options (`--nr off`, `--sr off`, `--mv none`), which the plan records and the log states. | Review; the simulator's failure injection checks that every failure variant propagates |
| R16 Invariants are contracts, and contracts run in production | `REQUIRE`/`ENSURE` are compiled in every configuration; a violation prints the predicate and location, dumps the trace ring to `dlssscreen-trace.txt` and aborts. | `infrastructure/contracts.h`, `trace.cpp` |
| R17 Every result is handled | Every non-void function is `[[nodiscard]]`; `std::expected` results are consumed by `and_then`, `transform`, `value_or` or `error_or`. | Lint R17; compiler (`-Wunused-result`) |
| R18 Writers verify their output | `CreateTexture` re-reads the resource description; `WriteZeros` reads the mapped bytes back; every NGX parameter write is read back (`NgxParameterRoundTrip`); `Log` checks the byte count of every write and the flush. | Code; lint R17 makes the verification result impossible to drop |
| R19 Nothing unfinished compiles | No TODO, FIXME, placeholder or stub on a shipped path. | Lint R19 |
| R20 Bound every wait | Fence waits time out after 4 s, the frame-latency wait after 2 s; the capture drain reads at most 8 frames, the message pump 64 messages, adapter enumeration 16 adapters, monitor enumeration 16 monitors, the session 2^40 frames. | Constants next to each wait |
| R21 Bound every resource | `infra::BoundedVector` and `infra::BoundedString` reject on overflow (`CapacityExceeded`, `StringTooLong`); the step list holds 192 steps, the parameter list 48, the argument list 64, the trace ring 65536 entries (oldest evicted). | Lint growth-site inventory (`growth_sites.txt`) |
| R22 No shared mutable state across threads | The process is single-threaded: the free-threaded capture pool is polled, never delivered by callback; the Direct3D 11 context is protected only because D3D11On12 requires it. There is no lock in the code. | Review; the thread sanitizer has nothing to observe because no thread is created |
| R23 All non-determinism is an input | The clock enters as `FrameInput.now`, capture arrival as `freshCapture`, hotkeys as toggles; the simulator draws all of them from a seeded SplitMix64 generator. Production code draws no randomness. | `interior/frame.h`, `effects/sim` |
| R24 No direct contact with reality | Only `src/effects/real` reads the clock, the GPU, the capture pool or the console. The simulator runs the same session loop from a numeric seed and injects device loss, capture loss, fence timeouts, NGX failures, scene cuts and clock jumps between frames; every failure reproduces from its seed. The tool is long-running and writes no persistent state except the optional append-only log mirror, so kill-at-random-point leaves nothing to recover. | `tests/simulation_test.cpp`; ctest seeds 1000, 2000, 3000; gate seeds 4000, 5000, 6000 |
| R25 Every call is traced | The compiler instruments every function outside `trace.cpp` (`-finstrument-functions`, or `/Gh /GH` with `trace_msvc.asm`); the ring records entry and exit addresses and is dumped on a contract violation. C++ has no reflection, so argument values are not serialised; the replay of a run is the seed under the simulator. | `CMakeLists.txt` (`DSCREEN_TRACE`), `trace.cpp` |
| R26 Tests are properties, and tests are tested | Fifty-one properties over generated inputs, including every failure variant of the parsers and planners; the option parser is fuzzed with random argument vectors; the simulator seed fuzzer runs whole sessions. `gate/mutate.py` introduces defects (`<`/`<=`, `==`/`!=`, `+`/`-`, `true`/`false`, `&&`/`||`) and fails the gate when the tests keep passing. Last sampled run: 60 of 135 mutants, 52 killed, 8 survived, score 0.87 (test seed 1000, sample seed 1). | ctest; gate (mutation threshold 0.8) |
| R27 No speculative generality | The environment abstraction has two implementations (real and simulated); every option is consumed by the plan; the single feature flag is inventoried. | Lint feature-flag inventory; review |
| R28 Comments are the last resort | Comments are waivers, citations or invariants, two lines at most; no doc comments, banners or commented-out code. | Lint R28 |
| R29 Standard library first | `std::expected`, ranges, `std::format`, `std::to_chars`; the only external code is NVIDIA's (DLSS SDK, optical flow SDK) and the platform. | `gate/dependencies.lock`, `gate/check_lock.py` |
| R30 Consult the index before writing | The lint writes `function_index.txt` (every function's name and signature) on each run; the clone check backs it. | Gate output |
| R31 Metaprogramming is confined to infrastructure | Templates and macros live in `src/infrastructure` (results, strong types, bounded containers, folds, formatting, contracts, tracing). The waivered exceptions are the effect interfaces (`com.h`, `session.h`), the composition root and the generic option-table lookups. Shader byte code is a build artefact produced by DXC from the checked-in HLSL, like an object file. `DSCREEN_HAVE_NVOF` is the only feature flag; the gate builds both values. | Lint R31; feature-flag inventory |
| R32 A change does what it says | Commits on this branch each state one change. | Review |

## Interpretations

- A `switch` or `std::visit` over a closed set is one match, not one decision per arm.
- A chain of `and_then`/`transform` calls is one composition statement.
- Templates are the generic types the rules require (strong types, results, bounded containers) and are
  confined to infrastructure; the effect interfaces that must be generic over the two environments carry waivers.
- COM and NVIDIA vtable calls are external effects; they are the boundary that R10's import lint draws.
- A function returning `void` is an effect and appears only in the effect layer or as an infrastructure sink.

## Waivers

Every waiver carries its rule number and reason next to the line; the gate lists them in `waivers.txt`.
The current inventory falls into these groups:

- R2 (state replaced whole): the simulator world, the real environment's frame context and counters,
  the trace ring, the monitor-enumeration callback's output slot, the session loop's frame state.
- R2 (loops): the session loop, the trace dump during a panic, the GPU search window in `Match.hlsl`,
  the test drivers and generators.
- R1: the session loop, whose body is the bounded iteration itself.
- R7: the real and the simulated interpreter of the step variant share the dispatch shape.
- R11: the trace ring written by compiler-inserted hooks that carry no context.
- R12: the window-message switch, an open set defined by the OS.
- R31: `com.h` and `session.h` (effect interfaces), `main.cpp` (composition root), the option-table lookups.

## The gate

`gate/gate.sh` (portable targets) and `gate/gate.ps1` (everything, including `DlssScreen.exe`) run:

1. the build with warnings as errors and tracing on (VIII.1, VIII.3);
2. the formatter check (`.clang-format`);
3. the lint with the function index, the waiver, growth-site and feature-flag inventories (VIII.2, 5, 9, 10);
4. the property tests under the ctest seeds and three further seeds (VIII.4, 6);
5. the address and undefined-behaviour sanitizers (portable targets);
6. mutation testing with the threshold (VIII.4); the surviving mutants of the last run are the open test gaps:
   `monitors.cpp` ordering and union arithmetic, `options.cpp` value validation, `plan.cpp` cursor and quality
   choice, `frame.cpp` finest-level flags and history staleness;
7. the dependency-lock check against the installed SDK headers (VIII.8).

## Differences from the first implementation

- No fallbacks: the first version silently ran as a passthrough when NGX was unavailable and fell back
  from optical flow to block matching; both are errors now (R15).
- NGX log lines go to NGX's own sinks; the log callback needed a global to reach the console (R11).
- The output is a `Result` at every step, so a failing call names the API and its code instead of an
  exception message.
- A contract violation aborts with the trace ring written next to the working directory (R16, R25).
