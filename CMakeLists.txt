cmake_minimum_required(VERSION 3.21)
project(DlssScreen VERSION 0.2.0 LANGUAGES CXX)

# Layout (see COMPLIANCE.md):
#   src/infrastructure  generic primitives, contracts, tracing (the only metaprogramming)
#   src/interior        pure domain logic: no OS headers, no exceptions, results as values
#   src/effects/sim     seeded simulator of the effect layer (portable)
#   src/effects/real    Windows: D3D12, DirectComposition, Windows Graphics Capture, NGX
#   src/app             composition root and the generic session loop
#   tests               property tests and the seed fuzzer (portable)
#   gate                rules lint and mutation testing

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
if(POLICY CMP0141)
  # Embedded debug information (/Z7) keeps object files cacheable by sccache; the linker still writes the PDB.
  cmake_policy(SET CMP0141 NEW)
  set(CMAKE_MSVC_DEBUG_INFORMATION_FORMAT "$<$<CONFIG:Debug,RelWithDebInfo>:Embedded>")
endif()

option(DSCREEN_ENABLE_NVOF "Build the NVIDIA Optical Flow motion-vector backend (needs NVOF_SDK_DIR)" OFF)
option(DSCREEN_TRACE "Build-applied call tracing (/Gh /GH or -finstrument-functions)" ON)
option(DSCREEN_SANITIZE "Address and undefined-behaviour sanitizers for the portable targets" OFF)
set(DLSS_SDK_DIR "$ENV{DLSS_SDK_DIR}" CACHE PATH "Root of the NVIDIA DLSS SDK (contains include/ and lib/)")
set(NVOF_SDK_DIR "$ENV{NVOF_SDK_DIR}" CACHE PATH "Root of the NVIDIA Optical Flow SDK")

# --- warnings as errors, pedantic ------------------------------------------------------------
add_library(dscreen_flags INTERFACE)
if(MSVC)
  target_compile_options(dscreen_flags INTERFACE /W4 /WX /permissive- /utf-8 /EHsc /Zc:__cplusplus /bigobj /external:anglebrackets /external:W0 /w14062 /w14254 /w14265 /w14296 /w14545 /w14546 /w14547 /w14549 /w14555 /w14619 /w14640 /w14826 /w14905 /w14906 /w14928 /w15038)
  target_compile_definitions(dscreen_flags INTERFACE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00 WINVER=0x0A00 NTDDI_VERSION=0x0A000008 _CRT_SECURE_NO_WARNINGS)
  if(CMAKE_GENERATOR MATCHES "Visual Studio")
    target_compile_options(dscreen_flags INTERFACE /MP) # MSBuild compiles a project's files one at a time otherwise
  endif()
else()
  target_compile_options(dscreen_flags INTERFACE -Wall -Wextra -Wpedantic -Werror -Wshadow -Wconversion -Wsign-conversion -Wswitch-enum -Wunused-result -Wcast-align -Wnull-dereference -Wdouble-promotion -Wimplicit-fallthrough)
  if(DSCREEN_SANITIZE)
    target_compile_options(dscreen_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(dscreen_flags INTERFACE -fsanitize=address,undefined)
  endif()
endif()
target_include_directories(dscreen_flags INTERFACE "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/src")

# --- tracing (R25): applied by the compiler to every function outside the trace runtime -----
add_library(dscreen_trace_flags INTERFACE)
if(DSCREEN_TRACE)
  if(MSVC)
    target_compile_options(dscreen_trace_flags INTERFACE /Gh /GH)
  else()
    target_compile_options(dscreen_trace_flags INTERFACE -finstrument-functions)
  endif()
endif()

# --- infrastructure: trace runtime is compiled without instrumentation ---------------------------
add_library(dscreen_trace STATIC src/infrastructure/trace.cpp)
target_link_libraries(dscreen_trace PUBLIC dscreen_flags)
if(DSCREEN_TRACE AND MSVC)
  # The hooks are assembled in their own target: ml64 rejects the C++ switches (/W4, /permissive-, ...).
  enable_language(ASM_MASM)
  add_library(dscreen_trace_hooks STATIC src/infrastructure/trace_msvc.asm)
  set_target_properties(dscreen_trace_hooks PROPERTIES LINKER_LANGUAGE CXX)
  target_link_libraries(dscreen_trace PUBLIC dscreen_trace_hooks)
endif()

add_library(dscreen_core STATIC
  src/infrastructure/checked.cpp
  src/interior/units.cpp
  src/interior/options.cpp
  src/interior/usage.cpp
  src/interior/monitors.cpp
  src/interior/pyramid.cpp
  src/interior/plan.cpp
  src/interior/frame.cpp
  src/interior/ngx_params.cpp
  src/effects/sim/sim.cpp)
target_link_libraries(dscreen_core PUBLIC dscreen_flags dscreen_trace PRIVATE dscreen_trace_flags)

# --- tests (portable) ----------------------------------------------------------------------------
enable_testing()
add_executable(dscreen_tests
  tests/test_main.cpp
  tests/units_test.cpp
  tests/options_test.cpp
  tests/monitors_test.cpp
  tests/plan_test.cpp
  tests/frame_test.cpp
  tests/simulation_test.cpp)
target_link_libraries(dscreen_tests PRIVATE dscreen_core dscreen_trace_flags)
add_test(NAME properties_seed_1000 COMMAND dscreen_tests 1000)
add_test(NAME properties_seed_2000 COMMAND dscreen_tests 2000)
add_test(NAME properties_seed_3000 COMMAND dscreen_tests 3000)

# --- gate tools ----------------------------------------------------------------------------------
add_executable(rules_lint gate/rules_lint.cpp)
target_link_libraries(rules_lint PRIVATE dscreen_flags)
add_custom_target(lint
  COMMAND rules_lint "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/gate"
  DEPENDS rules_lint
  COMMENT "rules_lint: checking the rules that a tool can check"
  VERBATIM)

# --- the Windows application ----------------------------------------------------------------------
if(WIN32)
  include(cmake/WindowsApp.cmake)
endif()
