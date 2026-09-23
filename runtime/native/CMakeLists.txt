if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  cmake_minimum_required(VERSION 3.24)
  project(GekkoAOTNativeHost LANGUAGES CXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GEKKOAOT_NATIVE_NOD "Use encounter/nod for direct disc-image access" ON)
option(GEKKOAOT_NATIVE_DEBUGGABLE "Keep native host frame pointers and symbols useful to GDB/perf" OFF)
option(GEKKOAOT_NATIVE_PORTABLE "Disable host-specific -march/-mtune flags for distributable binaries" OFF)
if(NOT DEFINED GEKKOAOT_NOD_VERSION OR GEKKOAOT_NOD_VERSION STREQUAL "")
  set(GEKKOAOT_NOD_VERSION "v2.0.0-alpha.12" CACHE STRING "encounter/nod release used by the standalone host")
endif()
set(GEKKOAOT_NOD_PACKAGE_URL "" CACHE STRING "Override prebuilt encounter/nod package URL")

if(GEKKOAOT_NATIVE_NOD)
    # Prefer an already-installed package. Otherwise fetch the same prebuilt
    # CMake package style used by Aurora, avoiding a Rust/Corrosion build on the
    # common desktop targets.
    find_package(nod QUIET CONFIG)
    if(NOT TARGET nod::nod)
        set(_gekkoaot_nod_platform "")
        string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" _gekkoaot_nod_arch)
        if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            if(_gekkoaot_nod_arch MATCHES "^(x86_64|amd64)$")
                set(_gekkoaot_nod_platform "linux-x86_64")
            elseif(_gekkoaot_nod_arch MATCHES "^(aarch64|arm64)$")
                set(_gekkoaot_nod_platform "linux-aarch64")
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            if(_gekkoaot_nod_arch MATCHES "^(x86_64|amd64)$")
                set(_gekkoaot_nod_platform "macos-x86_64")
            elseif(_gekkoaot_nod_arch MATCHES "^(aarch64|arm64)$")
                set(_gekkoaot_nod_platform "macos-arm64")
            endif()
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            if(_gekkoaot_nod_arch MATCHES "^(x86_64|amd64)$")
                set(_gekkoaot_nod_platform "windows-x86_64")
            elseif(_gekkoaot_nod_arch MATCHES "^(aarch64|arm64)$")
                set(_gekkoaot_nod_platform "windows-arm64")
            endif()
        endif()

        if(NOT GEKKOAOT_NOD_PACKAGE_URL)
            if(NOT _gekkoaot_nod_platform)
                message(FATAL_ERROR
                    "No prebuilt encounter/nod package is known for ${CMAKE_SYSTEM_NAME}/${CMAKE_SYSTEM_PROCESSOR}. "
                    "Install nod and set nod_DIR, or provide GEKKOAOT_NOD_PACKAGE_URL.")
            endif()
            set(GEKKOAOT_NOD_PACKAGE_URL
                "https://github.com/encounter/nod/releases/download/${GEKKOAOT_NOD_VERSION}/libnod-${_gekkoaot_nod_platform}.tar.gz")
        endif()

        message(STATUS "gekkoaot: Fetching encounter/nod ${GEKKOAOT_NOD_VERSION} (${_gekkoaot_nod_platform})")
        include(FetchContent)
        FetchContent_Declare(gekkoaot_nod_prebuilt
            URL "${GEKKOAOT_NOD_PACKAGE_URL}"
            DOWNLOAD_EXTRACT_TIMESTAMP FALSE
            EXCLUDE_FROM_ALL
        )
        FetchContent_MakeAvailable(gekkoaot_nod_prebuilt)

        # The release package exports nod::nod_static/nod::nod_shared and picks
        # nod::nod from BUILD_SHARED_LIBS. Standalone GekkoAOT wants a single
        # self-contained host binary, so select the static target.
        set(_gekkoaot_saved_build_shared_libs "${BUILD_SHARED_LIBS}")
        set(BUILD_SHARED_LIBS OFF)
        find_package(nod REQUIRED CONFIG
            PATHS "${gekkoaot_nod_prebuilt_SOURCE_DIR}"
            NO_DEFAULT_PATH
            NO_CMAKE_FIND_ROOT_PATH
        )
        set(BUILD_SHARED_LIBS "${_gekkoaot_saved_build_shared_libs}")
        unset(_gekkoaot_saved_build_shared_libs)
    endif()
    if(NOT TARGET nod::nod)
        message(FATAL_ERROR "encounter/nod was found but did not provide nod::nod")
    endif()
endif()

add_library(gekkoaot_native_runtime STATIC
    module_loader.cpp
    runtime.cpp
    sdk_autoresolver.cpp
    ../gx/host/host_bridge.cpp
    ../hw/cp/native_cp.cpp
    ../hw/pe/native_pe.cpp
    ../hw/pi/native_pi.cpp
    ../hw/mi/native_mi.cpp
    ../hw/ai/native_ai.cpp
    ../hw/di/native_di.cpp
    ../hw/exi/native_exi.cpp
    ../hw/lc/native_lc.cpp
    ../hw/si/native_si.cpp
    ../hw/vi/native_vi.cpp
    ../dsp/native/native_dsp.cpp
    ../dsp/native/native_ax_hle.cpp
    ../dsp/native/native_jaudio_hle.cpp
    ../dsp/native/native_dsp_core.cpp
    ../dsp/native/native_audio_sink.cpp
    ../video/native_vp6.cpp
)
target_include_directories(gekkoaot_native_runtime PUBLIC ${CMAKE_CURRENT_LIST_DIR}/..)
if(NOT DEFINED GEKKOAOT_VERSION OR GEKKOAOT_VERSION STREQUAL "")
    set(GEKKOAOT_VERSION "dev")
endif()
target_compile_definitions(gekkoaot_native_runtime PUBLIC GEKKOAOT_VERSION="${GEKKOAOT_VERSION}")

# v70 optional native VP6/VP6F codec backend for EA movie frontends.  The codec
# service is intentionally independent from game ABI interception: a proven
# movie-library frontend supplies complete compressed packets, while Aurora's
# video-texture sampling compatibility remains a separate graphics concern.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(GEKKOAOT_FFMPEG_VP6 QUIET IMPORTED_TARGET libavcodec libavutil)
endif()
if(TARGET PkgConfig::GEKKOAOT_FFMPEG_VP6)
    target_compile_definitions(gekkoaot_native_runtime PUBLIC GEKKOAOT_HAVE_FFMPEG_VP6=1)
    target_link_libraries(gekkoaot_native_runtime PUBLIC PkgConfig::GEKKOAOT_FFMPEG_VP6)
    message(STATUS "gekkoaot: native VP6/VP6F codec backend enabled (FFmpeg/libavcodec)")
else()
    message(STATUS "gekkoaot: libavcodec development files not found; native VP6 codec backend disabled")
endif()

message(STATUS "gekkoaot: native GameCube AX DSP HLE enabled (ROM-free; standalone LLE fallback for unknown ucodes)")
message(STATUS "gekkoaot: native early-JAudio HLE enabled (VPB/AFC/PCM/SRC/mixer; standalone LLE fallback for unknown families)")

# v45: standalone GameCube DSP LLE.  We compile only Dolphin's GPL DSP
# instruction semantic translation units against GekkoAOT-owned state/IFX
# shims; the resulting native host does not link Dolphin Core/System.
set(_gekkoaot_dolphin_core
    "${CMAKE_CURRENT_LIST_DIR}/../../.gekkoaot/src/ModernGekko/vendor/dolphin/Source/Core")
set(_gekkoaot_dsp_interp "${_gekkoaot_dolphin_core}/Core/DSP/Interpreter")
if(EXISTS "${_gekkoaot_dsp_interp}/DSPIntArithmetic.cpp")
    target_sources(gekkoaot_native_runtime PRIVATE
        "${_gekkoaot_dsp_interp}/DSPIntArithmetic.cpp"
        "${_gekkoaot_dsp_interp}/DSPIntBranch.cpp"
        "${_gekkoaot_dsp_interp}/DSPIntExtOps.cpp"
        "${_gekkoaot_dsp_interp}/DSPIntLoadStore.cpp"
        "${_gekkoaot_dsp_interp}/DSPIntMisc.cpp"
        "${_gekkoaot_dsp_interp}/DSPIntMultiplier.cpp"
    )
    target_include_directories(gekkoaot_native_runtime PRIVATE
        "${CMAKE_CURRENT_LIST_DIR}/../dsp/native/lle_shim"
        "${_gekkoaot_dolphin_core}")
    target_compile_definitions(gekkoaot_native_runtime PRIVATE GEKKOAOT_NATIVE_DSP_LLE=1)
    message(STATUS "gekkoaot: standalone native DSP LLE enabled (GPL instruction semantics, no Dolphin Core link)")
else()
    message(STATUS "gekkoaot: pinned Dolphin DSP source not materialized yet; native DSP LLE core disabled")
endif()
if(CMAKE_DL_LIBS)
    target_link_libraries(gekkoaot_native_runtime PUBLIC ${CMAKE_DL_LIBS})
endif()
if(GEKKOAOT_NATIVE_NOD)
    target_compile_definitions(gekkoaot_native_runtime PUBLIC GEKKOAOT_HAVE_NOD=1)
    target_link_libraries(gekkoaot_native_runtime PUBLIC nod::nod)
endif()

# Standalone input belongs to the native host. Prefer the system SDL3 package when available.
find_package(SDL3 QUIET CONFIG)
if(TARGET SDL3::SDL3)
    target_compile_definitions(gekkoaot_native_runtime PUBLIC GEKKOAOT_NATIVE_SDL_INPUT=1 GEKKOAOT_NATIVE_SDL3=1)
    target_link_libraries(gekkoaot_native_runtime PUBLIC SDL3::SDL3)
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_compile_definitions(gekkoaot_native_runtime PUBLIC HAVE_X11=1)
        target_link_libraries(gekkoaot_native_runtime PUBLIC X11::X11)
    endif()
    message(STATUS "gekkoaot: standalone SDL3 keyboard/gamepad input enabled")
else()
    message(STATUS "gekkoaot: SDL3 not found; standalone input bridge disabled")
endif()

add_executable(gekkoaot-native-run runner.cpp)
target_link_libraries(gekkoaot-native-run PRIVATE gekkoaot_native_runtime)

include(CheckIPOSupported)
check_ipo_supported(RESULT GEKKOAOT_NATIVE_IPO_OK OUTPUT GEKKOAOT_NATIVE_IPO_ERROR LANGUAGES CXX)
if(GEKKOAOT_NATIVE_IPO_OK)
    set_property(TARGET gekkoaot_native_runtime PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    set_property(TARGET gekkoaot-native-run PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    message(STATUS "gekkoaot: native runtime IPO/LTO enabled")
endif()

if(GEKKOAOT_NATIVE_NOD)
    add_executable(gekkoaot-native-disc disc_reader.cpp)
    target_link_libraries(gekkoaot-native-disc PRIVATE nod::nod)
endif()

if(MSVC)
    target_compile_options(gekkoaot_native_runtime PRIVATE /W4)
    target_compile_options(gekkoaot-native-run PRIVATE /W4)
    if(TARGET gekkoaot-native-disc)
        target_compile_options(gekkoaot-native-disc PRIVATE /W4)
    endif()
else()
    # Let CMAKE_BUILD_TYPE own the optimization level. Forcing -O3 and
    # -fomit-frame-pointer here made Debug/RelWithDebInfo misleading and made
    # compatibility crashes unnecessarily hard to inspect with GDB/perf.
    target_compile_options(gekkoaot_native_runtime PRIVATE -Wall -Wextra -Wpedantic)
    if(GEKKOAOT_NATIVE_DEBUGGABLE)
        target_compile_options(gekkoaot_native_runtime PRIVATE -fno-omit-frame-pointer)
        target_compile_options(gekkoaot-native-run PRIVATE -fno-omit-frame-pointer)
        message(STATUS "gekkoaot: native compatibility debug symbols/frame pointers enabled")
    endif()
    if(NOT APPLE)
        target_compile_options(gekkoaot_native_runtime PRIVATE -fno-plt)
        target_compile_options(gekkoaot-native-run PRIVATE -fno-plt)
    endif()
    if(NOT GEKKOAOT_NATIVE_PORTABLE AND NOT APPLE AND (CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64|aarch64|arm64"))
        target_compile_options(gekkoaot_native_runtime PRIVATE -march=native -mtune=native)
        target_compile_options(gekkoaot-native-run PRIVATE -march=native -mtune=native)
    endif()
    target_compile_options(gekkoaot-native-run PRIVATE -Wall -Wextra -Wpedantic)
    if(TARGET gekkoaot-native-disc)
        target_compile_options(gekkoaot-native-disc PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endif()

install(TARGETS gekkoaot-native-run RUNTIME DESTINATION bin)
if(TARGET gekkoaot-native-disc)
    install(TARGETS gekkoaot-native-disc RUNTIME DESTINATION bin)
endif()

if(BUILD_TESTING)
    add_executable(gekkoaot-test-cpu-timers ../../tests/hardware/test_cpu_timers.cpp)
    target_link_libraries(gekkoaot-test-cpu-timers PRIVATE gekkoaot_native_runtime)
    add_test(NAME hardware.cpu-timers COMMAND gekkoaot-test-cpu-timers)
    add_executable(gekkoaot-test-cp-fifo ../../tests/hardware/test_cp_fifo.cpp)
    target_link_libraries(gekkoaot-test-cp-fifo PRIVATE gekkoaot_native_runtime)
    add_test(NAME hardware.cp-fifo COMMAND gekkoaot-test-cp-fifo)
    add_executable(gekkoaot-test-device-events ../../tests/hardware/test_device_events.cpp)
    target_link_libraries(gekkoaot-test-device-events PRIVATE gekkoaot_native_runtime)
    add_test(NAME hardware.device-events COMMAND gekkoaot-test-device-events)
    add_library(gekkoaot-test-gx SHARED ../../tests/hardware/mock_gx.cpp)
    add_executable(gekkoaot-test-wgpipe ../../tests/hardware/test_wgpipe.cpp)
    target_link_libraries(gekkoaot-test-wgpipe PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.wgpipe COMMAND gekkoaot-test-wgpipe $<TARGET_FILE:gekkoaot-test-gx>)
endif()

if(BUILD_TESTING)
  add_executable(gekkoaot-test-vm ../../tests/hardware/test_vm.cpp)
  target_link_libraries(gekkoaot-test-vm PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.vm COMMAND gekkoaot-test-vm)
endif()

if(BUILD_TESTING)
  add_executable(gekkoaot-test-vi-timing ../../tests/hardware/test_vi_timing.cpp)
  target_link_libraries(gekkoaot-test-vi-timing PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.vi-timing COMMAND gekkoaot-test-vi-timing)

  add_executable(gekkoaot-test-ai-timing ../../tests/hardware/test_ai_timing.cpp)
  target_link_libraries(gekkoaot-test-ai-timing PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.ai-timing COMMAND gekkoaot-test-ai-timing)

  add_executable(gekkoaot-test-di-async ../../tests/hardware/test_di_async.cpp)
  target_link_libraries(gekkoaot-test-di-async PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.di-async COMMAND gekkoaot-test-di-async)

  add_executable(gekkoaot-test-exi-status ../../tests/hardware/test_exi_status.cpp)
  target_link_libraries(gekkoaot-test-exi-status PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.exi-status COMMAND gekkoaot-test-exi-status)

  add_executable(gekkoaot-test-os-message-priority ../../tests/hardware/test_os_message_priority.cpp)
  target_link_libraries(gekkoaot-test-os-message-priority PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.os-message-priority COMMAND gekkoaot-test-os-message-priority)

  add_executable(gekkoaot-test-si-poll ../../tests/hardware/test_si_poll.cpp)
  target_link_libraries(gekkoaot-test-si-poll PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.si-poll COMMAND gekkoaot-test-si-poll)

  add_executable(gekkoaot-test-aot-indirect-branch ../../tests/hardware/test_aot_indirect_branch.cpp)
  target_link_libraries(gekkoaot-test-aot-indirect-branch PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.aot-indirect-branch COMMAND gekkoaot-test-aot-indirect-branch)
endif()
