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
    guest_address_window.cpp
    guest_memory.cpp
    native_vfs.cpp
    runtime.cpp
    ../NativeHW/native_hw.cpp
    ../NativeHW/EXI/native_exi.cpp
    ../NativeHW/EXI/native_card.cpp
    ../NativeHW/EXI/ipl_font.cpp
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
    pkg_check_modules(GEKKOAOT_FFMPEG_SWSCALE QUIET IMPORTED_TARGET libswscale)
endif()
if(TARGET PkgConfig::GEKKOAOT_FFMPEG_VP6)
    target_compile_definitions(gekkoaot_native_runtime PUBLIC GEKKOAOT_HAVE_FFMPEG_VP6=1)
    target_link_libraries(gekkoaot_native_runtime PUBLIC PkgConfig::GEKKOAOT_FFMPEG_VP6)
    if(TARGET PkgConfig::GEKKOAOT_FFMPEG_SWSCALE)
        target_compile_definitions(gekkoaot_native_runtime PUBLIC GEKKOAOT_HAVE_FFMPEG_SWSCALE=1)
        target_link_libraries(gekkoaot_native_runtime PUBLIC PkgConfig::GEKKOAOT_FFMPEG_SWSCALE)
        message(STATUS "gekkoaot: VP6 YUV conversion accelerated with libswscale")
    endif()
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

# Standalone input belongs to the native host. Prefer a system SDL3 package
# for developer builds. Portable release builds must not silently ship without
# input, so fetch a pinned static SDL3 when the host has no package installed.
set(_gekkoaot_sdl3_target "")
find_package(SDL3 QUIET CONFIG)
if(TARGET SDL3::SDL3)
    set(_gekkoaot_sdl3_target SDL3::SDL3)
elseif(TARGET SDL3::SDL3-static)
    set(_gekkoaot_sdl3_target SDL3::SDL3-static)
endif()

if(NOT _gekkoaot_sdl3_target AND GEKKOAOT_NATIVE_PORTABLE)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(SDL_INSTALL OFF CACHE BOOL "" FORCE)

    # Portable GekkoAOT only needs SDL's input stack. Aurora owns the actual
    # presentation window, while keyboard state comes from Win32/X11 directly.
    # Disabling SDL video avoids requiring the full X11/Wayland development
    # stack on release builders and keeps the bundled SDL dependency minimal.
    set(SDL_VIDEO OFF CACHE BOOL "" FORCE)
    set(SDL_RENDER OFF CACHE BOOL "" FORCE)
    set(SDL_GPU OFF CACHE BOOL "" FORCE)
    set(SDL_CAMERA OFF CACHE BOOL "" FORCE)
    set(SDL_UNIX_CONSOLE_BUILD ON CACHE BOOL "" FORCE)
    FetchContent_Declare(gekkoaot_sdl3
        GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
        GIT_TAG release-3.2.22
        GIT_SHALLOW TRUE
        EXCLUDE_FROM_ALL
    )
    FetchContent_MakeAvailable(gekkoaot_sdl3)
    if(TARGET SDL3::SDL3-static)
        set(_gekkoaot_sdl3_target SDL3::SDL3-static)
    elseif(TARGET SDL3::SDL3)
        set(_gekkoaot_sdl3_target SDL3::SDL3)
    endif()
endif()

if(_gekkoaot_sdl3_target)
    target_compile_definitions(gekkoaot_native_runtime PUBLIC GEKKOAOT_NATIVE_SDL_INPUT=1 GEKKOAOT_NATIVE_SDL3=1)
    target_link_libraries(gekkoaot_native_runtime PUBLIC ${_gekkoaot_sdl3_target})
    find_package(X11 QUIET)
    if(X11_FOUND)
        target_compile_definitions(gekkoaot_native_runtime PUBLIC HAVE_X11=1)
        target_link_libraries(gekkoaot_native_runtime PUBLIC X11::X11)
    endif()
    message(STATUS "gekkoaot: standalone SDL3 keyboard/gamepad input enabled (${_gekkoaot_sdl3_target})")
else()
    message(STATUS "gekkoaot: SDL3 not found; standalone input bridge disabled")
endif()

add_executable(gekkoaot-native-run runner.cpp)
target_link_libraries(gekkoaot-native-run PRIVATE gekkoaot_native_runtime)

# The Windows host performs native renderer/runtime initialization on the main
# thread. Reserve explicit headroom so future large local state or deep native
# call chains fail less readily than the platform's small default stack.
if(WIN32)
    if(MSVC)
        target_link_options(gekkoaot-native-run PRIVATE /STACK:16777216)
    else()
        target_link_options(gekkoaot-native-run PRIVATE "LINKER:--stack,16777216")
    endif()
endif()

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
        # v136: the native host is a closed executable/static-runtime pair; no
        # runtime symbol interposition is part of its ABI. Let GCC/Clang fold
        # internal calls/state more aggressively before and during LTO.
        if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(gekkoaot_native_runtime PRIVATE -fno-semantic-interposition)
            target_compile_options(gekkoaot-native-run PRIVATE -fno-semantic-interposition)
        endif()
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            target_compile_options(gekkoaot_native_runtime PRIVATE -fipa-pta)
        endif()
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
    add_executable(gekkoaot-test-native-hardware ../../tests/hardware/test_native_hardware.cpp)
    target_link_libraries(gekkoaot-test-native-hardware PRIVATE gekkoaot_native_runtime)
    add_test(NAME hardware.native-hardware COMMAND gekkoaot-test-native-hardware)
    add_executable(gekkoaot-test-stable-mmio ../../tests/hardware/test_stable_mmio.cpp)
    target_link_libraries(gekkoaot-test-stable-mmio PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.stable-mmio COMMAND gekkoaot-test-stable-mmio $<TARGET_FILE:gekkoaot-test-gx>)
    add_executable(gekkoaot-test-wgpipe ../../tests/hardware/test_wgpipe.cpp)
    target_link_libraries(gekkoaot-test-wgpipe PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.wgpipe COMMAND gekkoaot-test-wgpipe $<TARGET_FILE:gekkoaot-test-gx>)
    add_executable(gekkoaot-test-pe-event-order ../../tests/hardware/test_pe_event_order.cpp)
    target_link_libraries(gekkoaot-test-pe-event-order PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.pe-event-order COMMAND gekkoaot-test-pe-event-order $<TARGET_FILE:gekkoaot-test-gx>)
    add_test(NAME hardware.pe-event-order-legacy COMMAND gekkoaot-test-pe-event-order $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.pe-event-order-legacy PROPERTIES ENVIRONMENT "GEKKOAOT_GX_ORDERED_PE_IRQ=0")
    add_executable(gekkoaot-test-gx-sdk-packet ../../tests/hardware/test_gx_sdk_packet.cpp)
    target_link_libraries(gekkoaot-test-gx-sdk-packet PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.gx-sdk-packet COMMAND gekkoaot-test-gx-sdk-packet $<TARGET_FILE:gekkoaot-test-gx>)
    add_test(NAME hardware.gx-native-matrix-service COMMAND gekkoaot-test-gx-sdk-packet $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-native-matrix-service PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_HW=1;GEKKOAOT_NATIVE_GX_DIRECT_MATRICES=1;GEKKOAOT_TEST_DIRECT_MATRICES=1")
    add_test(NAME hardware.gx-native-matrix-disabled COMMAND gekkoaot-test-gx-sdk-packet $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-native-matrix-disabled PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_HW=1;GEKKOAOT_NATIVE_GX_DIRECT_MATRICES=0;GEKKOAOT_TEST_DIRECT_MATRICES=1")
    add_executable(gekkoaot-test-gx-array-service ../../tests/hardware/test_gx_array_service.cpp)
    target_link_libraries(gekkoaot-test-gx-array-service PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.gx-array-service COMMAND gekkoaot-test-gx-array-service $<TARGET_FILE:gekkoaot-test-gx>)
    add_test(NAME hardware.gx-array-service-scalar COMMAND gekkoaot-test-gx-array-service $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-array-service-scalar PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_SDK_PACKETS=0")
    add_test(NAME hardware.gx-array-service-disabled COMMAND gekkoaot-test-gx-array-service $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-array-service-disabled PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_ARRAY_SERVICE=0")
    add_test(NAME hardware.gx-array-service-typed COMMAND gekkoaot-test-gx-array-service $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-array-service-typed PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_HW=1;GEKKOAOT_NATIVE_GX_TYPED_ARRAYS=1")
    add_test(NAME hardware.wgpipe-sdk-legacy COMMAND gekkoaot-test-wgpipe $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.wgpipe-sdk-legacy PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_SDK_PACKETS=0")
    add_executable(gekkoaot-test-gx-batch-slot ../../tests/hardware/test_gx_batch_slot.cpp)
    target_link_libraries(gekkoaot-test-gx-batch-slot PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.gx-batch-slot COMMAND gekkoaot-test-gx-batch-slot $<TARGET_FILE:gekkoaot-test-gx>)
    add_executable(gekkoaot-test-frame-signal ../../tests/hardware/test_frame_signal.cpp)
    target_link_libraries(gekkoaot-test-frame-signal PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.frame-signal COMMAND gekkoaot-test-frame-signal $<TARGET_FILE:gekkoaot-test-gx> async)
    add_test(NAME hardware.frame-signal-fenced COMMAND gekkoaot-test-frame-signal $<TARGET_FILE:gekkoaot-test-gx> fenced)
    set_tests_properties(hardware.frame-signal-fenced PROPERTIES ENVIRONMENT "GEKKOAOT_GX_ASYNC_COMMANDS=0")
    add_executable(gekkoaot-test-gx-async-commands ../../tests/hardware/test_gx_async_commands.cpp)
    target_link_libraries(gekkoaot-test-gx-async-commands PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.gx-async-commands COMMAND gekkoaot-test-gx-async-commands $<TARGET_FILE:gekkoaot-test-gx>)
    add_test(NAME hardware.gx-async-typed-commands COMMAND gekkoaot-test-gx-async-commands $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-async-typed-commands PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_TYPED_ARRAYS=1")
    add_executable(gekkoaot-test-gx-typed-batches ../../tests/hardware/test_gx_typed_batches.cpp)
    target_link_libraries(gekkoaot-test-gx-typed-batches PRIVATE gekkoaot_native_runtime gekkoaot-test-gx)
    add_test(NAME hardware.gx-typed-batches COMMAND gekkoaot-test-gx-typed-batches $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-typed-batches PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_TYPED_ARRAYS=1")
    add_test(NAME hardware.gx-typed-batches-copy COMMAND gekkoaot-test-gx-typed-batches $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-typed-batches-copy PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_TYPED_ARRAYS=1;GEKKOAOT_GX_DIRECT_BATCH_SLOT=0;GEKKOAOT_WGPIPE_BATCH_BYTES=32")
    add_test(NAME hardware.gx-typed-batches-overflow COMMAND gekkoaot-test-gx-typed-batches $<TARGET_FILE:gekkoaot-test-gx>)
    set_tests_properties(hardware.gx-typed-batches-overflow PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_TYPED_ARRAYS=1;GEKKOAOT_WGPIPE_BATCH_BYTES=4096")
    add_test(NAME hardware.gx-typed-batches-serial COMMAND gekkoaot-test-gx-typed-batches $<TARGET_FILE:gekkoaot-test-gx> serial)
    set_tests_properties(hardware.gx-typed-batches-serial PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_TYPED_ARRAYS=1")
    add_library(gekkoaot-test-gx-legacy SHARED ../../tests/hardware/mock_gx.cpp)
    target_compile_definitions(gekkoaot-test-gx-legacy PRIVATE GEKKOAOT_TEST_LEGACY_GX=1)
    add_executable(gekkoaot-test-frame-signal-legacy ../../tests/hardware/test_frame_signal.cpp)
    target_link_libraries(gekkoaot-test-frame-signal-legacy PRIVATE gekkoaot_native_runtime gekkoaot-test-gx-legacy)
    add_test(NAME hardware.frame-signal-legacy COMMAND gekkoaot-test-frame-signal-legacy $<TARGET_FILE:gekkoaot-test-gx-legacy> legacy)
    add_test(NAME hardware.typed-gx-legacy-abi COMMAND gekkoaot-test-frame-signal-legacy $<TARGET_FILE:gekkoaot-test-gx-legacy> legacy)
    set_tests_properties(hardware.typed-gx-legacy-abi PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_TYPED_ARRAYS=1")
endif()

if(BUILD_TESTING)
  add_executable(gekkoaot-test-guest-address-window ../../tests/hardware/test_guest_address_window.cpp)
  target_link_libraries(gekkoaot-test-guest-address-window PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.guest-address-window COMMAND gekkoaot-test-guest-address-window)
  add_executable(gekkoaot-test-vm ../../tests/hardware/test_vm.cpp)
  target_link_libraries(gekkoaot-test-vm PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.vm COMMAND gekkoaot-test-vm)
endif()

if(BUILD_TESTING)
  add_executable(gekkoaot-test-vi-timing ../../tests/hardware/test_vi_timing.cpp)
  add_executable(gekkoaot-test-sdk-idle-poll ../../tests/hardware/test_sdk_idle_poll.cpp)
  add_executable(gekkoaot-test-widescreen ../../tests/hardware/test_widescreen.cpp)
  add_executable(gekkoaot-test-frame-replay ../../tests/hardware/test_frame_replay.cpp)
  target_compile_features(gekkoaot-test-frame-replay PRIVATE cxx_std_20)
  add_test(NAME hardware.frame-replay COMMAND gekkoaot-test-frame-replay)
  add_executable(gekkoaot-test-array-upload-policy ../../tests/hardware/test_array_upload_policy.cpp)
  add_test(NAME hardware.array-upload-policy COMMAND gekkoaot-test-array-upload-policy)
  add_executable(gekkoaot-test-compact-array-window ../../tests/hardware/test_compact_array_window.cpp)
  target_compile_features(gekkoaot-test-compact-array-window PRIVATE cxx_std_20)
  add_test(NAME hardware.compact-array-window COMMAND gekkoaot-test-compact-array-window)
  add_executable(gekkoaot-test-cached-scanout ../../tests/hardware/test_cached_scanout.cpp)
  target_compile_features(gekkoaot-test-cached-scanout PRIVATE cxx_std_20)
  add_test(NAME hardware.cached-scanout COMMAND gekkoaot-test-cached-scanout)
  add_executable(gekkoaot-test-si-mapping ../../tests/hardware/test_si_mapping.cpp)
  target_include_directories(gekkoaot-test-si-mapping PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
  add_test(NAME hardware.si-mapping COMMAND gekkoaot-test-si-mapping)
  add_executable(gekkoaot-test-native-hw ../../tests/hardware/test_native_hw.cpp
    guest_address_window.cpp
    ../NativeHW/native_hw.cpp ../NativeHW/EXI/native_exi.cpp
    ../NativeHW/EXI/native_card.cpp ../NativeHW/EXI/ipl_font.cpp)
  target_include_directories(gekkoaot-test-native-hw PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
  add_test(NAME hardware.native-hw COMMAND gekkoaot-test-native-hw)
  add_executable(gekkoaot-test-ipl-font ../../tests/hardware/test_ipl_font.cpp ../NativeHW/EXI/ipl_font.cpp)
  target_include_directories(gekkoaot-test-ipl-font PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
  target_compile_features(gekkoaot-test-ipl-font PRIVATE cxx_std_20)
  add_test(NAME hardware.ipl-font COMMAND gekkoaot-test-ipl-font)
  add_test(NAME hardware.widescreen COMMAND gekkoaot-test-widescreen)
  add_test(NAME hardware.sdk-idle-poll COMMAND gekkoaot-test-sdk-idle-poll)
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

  add_executable(gekkoaot-test-os-context-fpu ../../tests/hardware/test_os_context_fpu.cpp)
  target_link_libraries(gekkoaot-test-os-context-fpu PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.os-context-fpu COMMAND gekkoaot-test-os-context-fpu)

  add_executable(gekkoaot-test-si-poll ../../tests/hardware/test_si_poll.cpp)
  target_link_libraries(gekkoaot-test-si-poll PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.si-poll COMMAND gekkoaot-test-si-poll)

  add_executable(gekkoaot-test-si-transfer-events ../../tests/hardware/test_si_transfer_events.cpp)
  target_link_libraries(gekkoaot-test-si-transfer-events PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.si-transfer-events COMMAND gekkoaot-test-si-transfer-events)

  add_executable(gekkoaot-test-aot-indirect-branch ../../tests/hardware/test_aot_indirect_branch.cpp)
  target_link_libraries(gekkoaot-test-aot-indirect-branch PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.aot-indirect-branch COMMAND gekkoaot-test-aot-indirect-branch)

  add_executable(gekkoaot-test-sdk-clock ../../tests/hardware/test_sdk_clock.cpp)
  target_link_libraries(gekkoaot-test-sdk-clock PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.sdk-clock COMMAND gekkoaot-test-sdk-clock)
  add_executable(gekkoaot-test-native-counter-clock ../../tests/hardware/test_native_counter_clock.cpp)
  target_link_libraries(gekkoaot-test-native-counter-clock PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.native-counter-clock COMMAND gekkoaot-test-native-counter-clock)
  add_executable(gekkoaot-test-native-sdk-admission ../../tests/hardware/test_native_sdk_admission.cpp)
  target_link_libraries(gekkoaot-test-native-sdk-admission PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.native-sdk-admission COMMAND gekkoaot-test-native-sdk-admission)
  add_test(NAME hardware.native-sdk-disabled COMMAND gekkoaot-test-native-sdk-admission)
  set_tests_properties(hardware.native-sdk-disabled PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_DVD_FST=0;GEKKOAOT_NATIVE_GX_NORMAL_MATRIX=0;GEKKOAOT_NATIVE_GX_ARRAY_SERVICE=0")
  add_executable(gekkoaot-test-dvd-fst-service ../../tests/hardware/test_dvd_fst_service.cpp)
  target_link_libraries(gekkoaot-test-dvd-fst-service PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.dvd-fst-service COMMAND gekkoaot-test-dvd-fst-service)
  add_test(NAME hardware.dvd-fst-disabled COMMAND gekkoaot-test-dvd-fst-service)
  set_tests_properties(hardware.dvd-fst-disabled PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_DVD_FST=0")
  add_executable(gekkoaot-test-ee-pending-barrier ../../tests/hardware/test_ee_pending_barrier.cpp)
  target_link_libraries(gekkoaot-test-ee-pending-barrier PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.ee-pending-barrier COMMAND gekkoaot-test-ee-pending-barrier)
  add_executable(gekkoaot-test-native-index-range-cache ../../tests/hardware/test_native_index_range_cache.cpp)
  target_include_directories(gekkoaot-test-native-index-range-cache PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
  add_test(NAME hardware.native-index-range-cache COMMAND gekkoaot-test-native-index-range-cache)
  add_executable(gekkoaot-test-native-attribute-plan ../../tests/hardware/test_native_attribute_plan.cpp)
  target_include_directories(gekkoaot-test-native-attribute-plan PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
  add_test(NAME hardware.native-attribute-plan COMMAND gekkoaot-test-native-attribute-plan)
  add_executable(gekkoaot-test-native-address-service ../../tests/hardware/test_native_address_service.cpp)
  target_link_libraries(gekkoaot-test-native-address-service PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.native-address-service COMMAND gekkoaot-test-native-address-service)
  add_executable(gekkoaot-test-native-locked-cache-service ../../tests/hardware/test_native_locked_cache_service.cpp)
  target_link_libraries(gekkoaot-test-native-locked-cache-service PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.native-locked-cache-service COMMAND gekkoaot-test-native-locked-cache-service)

  add_executable(gekkoaot-test-native-low-vector ../../tests/hardware/test_native_low_vector.cpp)
  target_link_libraries(gekkoaot-test-native-low-vector PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.native-low-vector COMMAND gekkoaot-test-native-low-vector)
endif()

if(BUILD_TESTING)
  add_executable(gekkoaot-test-tlut-memory ../../tests/hardware/test_tlut_memory.cpp)
  target_include_directories(gekkoaot-test-tlut-memory PRIVATE ${CMAKE_CURRENT_LIST_DIR}/..)
  add_test(NAME graphics.tlut-memory COMMAND gekkoaot-test-tlut-memory)
endif()

if(BUILD_TESTING)
  add_executable(gekkoaot-test-disc-runtime ../../tests/hardware/test_disc_runtime.cpp)
  target_link_libraries(gekkoaot-test-disc-runtime PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.disc-runtime COMMAND gekkoaot-test-disc-runtime)
  add_executable(gekkoaot-test-native-jaudio ../../tests/hardware/test_native_jaudio.cpp)
  target_link_libraries(gekkoaot-test-native-jaudio PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.native-jaudio COMMAND gekkoaot-test-native-jaudio)

  add_executable(gekkoaot-test-guest-write-tracker ../../tests/hardware/test_guest_write_tracker.cpp)
  target_link_libraries(gekkoaot-test-guest-write-tracker PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.guest-write-tracker COMMAND gekkoaot-test-guest-write-tracker)

  add_executable(gekkoaot-test-dsp-reset ../../tests/hardware/test_dsp_reset.cpp)
  target_link_libraries(gekkoaot-test-dsp-reset PRIVATE gekkoaot_native_runtime)
  add_test(NAME hardware.dsp-reset COMMAND gekkoaot-test-dsp-reset)
endif()
