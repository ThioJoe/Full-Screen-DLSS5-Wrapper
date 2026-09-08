# The Windows executable: real effect layer + composition root. Needs the DLSS SDK and DXC.

find_path(NGX_INCLUDE_DIR nvsdk_ngx.h HINTS "${DLSS_SDK_DIR}/include")
set(_ngx_lib_hints "${DLSS_SDK_DIR}/lib/Windows_x86_64/x86_64" "${DLSS_SDK_DIR}/lib/Windows_x86_64/x64" "${DLSS_SDK_DIR}/lib/Windows_x86_64" "${DLSS_SDK_DIR}/lib")
# The SDK ships nvsdk_ngx_s.lib for the static CRT (/MT, which this project uses) and nvsdk_ngx_d.lib for the dynamic one.
find_library(NGX_LIBRARY_RELEASE NAMES nvsdk_ngx_s nvsdk_ngx_d3d12 HINTS ${_ngx_lib_hints})
find_library(NGX_LIBRARY_DEBUG NAMES nvsdk_ngx_s_dbg nvsdk_ngx_d3d12_dbg HINTS ${_ngx_lib_hints})
if(NOT NGX_INCLUDE_DIR OR NOT NGX_LIBRARY_RELEASE)
  message(FATAL_ERROR "NVIDIA DLSS SDK not found: configure with -DDLSS_SDK_DIR=<checkout of https://github.com/NVIDIA/DLSS>")
endif()
if(NOT NGX_LIBRARY_DEBUG)
  set(NGX_LIBRARY_DEBUG "${NGX_LIBRARY_RELEASE}")
endif()

set(_dxc_hints)
if(DEFINED ENV{WindowsSdkVerBinPath})
  list(APPEND _dxc_hints "$ENV{WindowsSdkVerBinPath}x64" "$ENV{WindowsSdkVerBinPath}/x64")
endif()
foreach(_root "$ENV{WindowsSdkBinPath}" "${CMAKE_WINDOWS_KITS_10_DIR}/bin" "C:/Program Files (x86)/Windows Kits/10/bin")
  file(GLOB _sdk_bin_dirs LIST_DIRECTORIES true "${_root}/10.*")
  foreach(_d IN LISTS _sdk_bin_dirs)
    list(APPEND _dxc_hints "${_d}/x64")
  endforeach()
endforeach()
list(REVERSE _dxc_hints) # newest Windows SDK first
find_program(DXC_EXECUTABLE NAMES dxc dxc.exe HINTS ${_dxc_hints})
if(NOT DXC_EXECUTABLE)
  message(FATAL_ERROR "dxc.exe was not found; it ships with the Windows SDK under bin/<version>/x64 (set -DDXC_EXECUTABLE=<path>)")
endif()

set(DSCREEN_SHADER_OUT "${CMAKE_BINARY_DIR}/generated/shaders")
file(MAKE_DIRECTORY "${DSCREEN_SHADER_OUT}")
set(DSCREEN_SHADER_HEADERS)
function(dscreen_compile_shader hlsl entry profile variable)
  set(_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/${hlsl}")
  set(_out "${DSCREEN_SHADER_OUT}/${variable}.h")
  add_custom_command(OUTPUT "${_out}"
    COMMAND "${DXC_EXECUTABLE}" -nologo -T ${profile} -E ${entry} -O3 -WX -I "${CMAKE_CURRENT_SOURCE_DIR}/shaders" -Fh "${_out}" -Vn k${variable} "${_src}"
    DEPENDS "${_src}" "${CMAKE_CURRENT_SOURCE_DIR}/shaders/Common.hlsli"
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/shaders"
    COMMENT "DXC ${hlsl}:${entry}" VERBATIM)
  set(DSCREEN_SHADER_HEADERS ${DSCREEN_SHADER_HEADERS} "${_out}" PARENT_SCOPE)
endfunction()
dscreen_compile_shader(Convert.hlsl    CS_Convert    cs_6_0 ConvertCS)
dscreen_compile_shader(Downsample.hlsl CS_Downsample cs_6_0 DownsampleCS)
dscreen_compile_shader(Match.hlsl      CS_Match      cs_6_0 MatchCS)
dscreen_compile_shader(Match.hlsl      CS_Finalize   cs_6_0 FinalizeCS)
dscreen_compile_shader(FlowToMv.hlsl   CS_FlowToMv   cs_6_0 FlowToMvCS)
dscreen_compile_shader(Blit.hlsl       VS_Blit       vs_6_0 BlitVS)
dscreen_compile_shader(Blit.hlsl       PS_Blit       ps_6_0 BlitPS)
add_custom_target(DlssScreenShaders DEPENDS ${DSCREEN_SHADER_HEADERS})

set(DSCREEN_REAL_SOURCES
  src/effects/real/com.cpp
  src/effects/real/window.cpp
  src/effects/real/device.cpp
  src/effects/real/resources.cpp
  src/effects/real/presenter.cpp
  src/effects/real/pipelines.cpp
  src/effects/real/capture.cpp
  src/effects/real/ngx.cpp
  src/effects/real/executor.cpp
  src/effects/real/clock.cpp
  src/effects/real/console.cpp
  src/effects/real/environment.cpp
  src/app/main.cpp)
if(DSCREEN_ENABLE_NVOF)
  find_path(NVOF_INCLUDE_DIR nvOpticalFlowD3D12.h HINTS "${NVOF_SDK_DIR}/NvOFInterface" "${NVOF_SDK_DIR}/include" "${NVOF_SDK_DIR}")
  if(NOT NVOF_INCLUDE_DIR)
    message(FATAL_ERROR "DSCREEN_ENABLE_NVOF is ON but nvOpticalFlowD3D12.h was not found under NVOF_SDK_DIR")
  endif()
  list(APPEND DSCREEN_REAL_SOURCES src/effects/real/nvof.cpp)
endif()

add_executable(DlssScreen ${DSCREEN_REAL_SOURCES})
add_dependencies(DlssScreen DlssScreenShaders)
target_include_directories(DlssScreen PRIVATE "${CMAKE_BINARY_DIR}/generated" "${NGX_INCLUDE_DIR}")
target_compile_definitions(DlssScreen PRIVATE DSCREEN_VERSION_STRING="${PROJECT_VERSION}")
if(DSCREEN_ENABLE_NVOF)
  target_include_directories(DlssScreen PRIVATE "${NVOF_INCLUDE_DIR}")
  target_compile_definitions(DlssScreen PRIVATE DSCREEN_HAVE_NVOF=1)
else()
  target_compile_definitions(DlssScreen PRIVATE DSCREEN_HAVE_NVOF=0)
endif()
target_link_libraries(DlssScreen PRIVATE dscreen_core dscreen_trace_flags
  "$<IF:$<CONFIG:Debug>,${NGX_LIBRARY_DEBUG},${NGX_LIBRARY_RELEASE}>"
  d3d12 dxgi d3d11 dcomp dxguid user32 gdi32 shcore shell32 ole32 runtimeobject)
target_link_options(DlssScreen PRIVATE /SUBSYSTEM:CONSOLE /MAP)

file(GLOB _ngx_dlls "${DLSS_SDK_DIR}/lib/Windows_x86_64/rel/nvngx_dlss.dll" "${DLSS_SDK_DIR}/lib/Windows_x86_64/nvngx_dlss.dll")
foreach(_dll IN LISTS _ngx_dlls)
  add_custom_command(TARGET DlssScreen POST_BUILD COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${_dll}" "$<TARGET_FILE_DIR:DlssScreen>")
endforeach()
install(TARGETS DlssScreen RUNTIME DESTINATION .)
