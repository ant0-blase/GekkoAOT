cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/version.txt" GEKKOAOT_VERSION LIMIT_COUNT 1)
string(STRIP "${GEKKOAOT_VERSION}" GEKKOAOT_VERSION)
if(NOT GEKKOAOT_VERSION MATCHES "^[0-9]+[.][0-9]+[.][0-9]+([.-][0-9A-Za-z.-]+)?$")
    message(FATAL_ERROR "Invalid GekkoAOT version in version.txt: '${GEKKOAOT_VERSION}'")
endif()

project(GekkoAOT VERSION ${GEKKOAOT_VERSION} LANGUAGES C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(GEKKOAOT_BUILD_GUI "Build the Qt GekkoAOT frontend" ON)
option(GEKKOAOT_BUILD_NATIVE_HOST "Build the standalone native GameCube host" ON)

set(GEKKOAOT_DOLRECOMP_DEFAULT_REV "71ce7f97419b1bb1ba9a9596c41507f6629e0fb0")
set(GEKKOAOT_DOLRECOMP_REV "${GEKKOAOT_DOLRECOMP_DEFAULT_REV}" CACHE STRING "Pinned DolRecomp revision")

# CMake cache values survive changes to CACHE defaults. v1.4 moved the
# project pin from 40637c46 to 71ce7f97, so an existing build directory can
# otherwise keep compiling gekkoaotctl with the old revision forever. Migrate
# only the previous project default; explicit custom pins remain untouched.
if(GEKKOAOT_DOLRECOMP_REV STREQUAL "40637c4683bd2820ac5b23607ee344720beb26df")
    message(STATUS "gekkoaot: migrating DolRecomp pin 40637c46 -> 71ce7f97")
    set(GEKKOAOT_DOLRECOMP_REV "${GEKKOAOT_DOLRECOMP_DEFAULT_REV}" CACHE STRING "Pinned DolRecomp revision" FORCE)
endif()
set(GEKKOAOT_AURORA_DEFAULT_REV "9c0bf66f1ed3276b60ad1cd746e2fb48818a6298")
set(GEKKOAOT_AURORA_REV "${GEKKOAOT_AURORA_DEFAULT_REV}" CACHE STRING "Pinned Aurora revision with retail FIFO command processor")

# v131 regression bisect: stay on the modern Aurora renderer, but stop one
# commit before 3840bf9 ("Fix GX EFB alpha handling").  Migrate only known
# project defaults; explicit custom pins remain untouched.
if(GEKKOAOT_AURORA_REV STREQUAL "7f2801cd0133c9333eadb4e2e6b24100c328d328" OR
   GEKKOAOT_AURORA_REV STREQUAL "3840bf9ae735191026e4d4edb0ce6f24d91f7eea")
    message(STATUS "gekkoaot: v131 near-head Aurora pin -> 9c0bf66f (pre-EFB-alpha-fix)")
    set(GEKKOAOT_AURORA_REV "${GEKKOAOT_AURORA_DEFAULT_REV}" CACHE STRING "Pinned Aurora revision with retail FIFO command processor" FORCE)
endif()
set(GEKKOAOT_NOD_VERSION "v2.0.0-alpha.12" CACHE STRING "Pinned encounter/nod release")

add_executable(gekkoaotctl tools/gekkoaotctl.cpp)
target_compile_definitions(gekkoaotctl PRIVATE
    GEKKOAOT_VERSION="${PROJECT_VERSION}"
    GEKKOAOT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
    GEKKOAOT_DOLRECOMP_REV="${GEKKOAOT_DOLRECOMP_REV}"
    GEKKOAOT_AURORA_REV="${GEKKOAOT_AURORA_REV}"
)

add_executable(gekkoaot-module-meta tools/module_meta.cpp)
add_executable(gekkoaot-secondary-build tools/secondary_build.c)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS tools/secondary_build.c)
file(SHA256 "${CMAKE_CURRENT_SOURCE_DIR}/tools/secondary_build.c" GEKKOAOT_SECONDARY_SOURCE_HASH)
target_compile_definitions(gekkoaot-secondary-build PRIVATE
    GEKKOAOT_SECONDARY_SOURCE_HASH="${GEKKOAOT_SECONDARY_SOURCE_HASH}")
add_dependencies(gekkoaotctl gekkoaot-secondary-build)
target_compile_definitions(gekkoaot-module-meta PRIVATE GEKKOAOT_VERSION="${PROJECT_VERSION}")

if(MSVC)
    target_compile_options(gekkoaotctl PRIVATE /W4)
    target_compile_options(gekkoaot-module-meta PRIVATE /W4)
    target_compile_options(gekkoaot-secondary-build PRIVATE /W4)
else()
    target_compile_options(gekkoaotctl PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gekkoaot-module-meta PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(gekkoaot-secondary-build PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    add_executable(gekkoaot-test-controller-process tests/hardware/test_controller_process.cpp)
    target_compile_definitions(gekkoaot-test-controller-process PRIVATE
        GEKKOAOT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
    add_dependencies(gekkoaot-test-controller-process gekkoaot-secondary-build)
    add_test(NAME build.controller-process COMMAND gekkoaot-test-controller-process
        $<TARGET_FILE:gekkoaot-secondary-build>
        "${CMAKE_CURRENT_BINARY_DIR}/controller-process-regression")
    add_executable(gekkoaot-test-source-timestamps tests/hardware/test_source_timestamps.cpp)
    add_test(NAME build.source-timestamps COMMAND gekkoaot-test-source-timestamps
        "${CMAKE_CURRENT_BINARY_DIR}/source-timestamps-regression")
    add_executable(gekkoaot-test-secondary-build tests/hardware/test_secondary_build.c)
    target_compile_definitions(gekkoaot-test-secondary-build PRIVATE
        GEKKOAOT_TEST_RUNTIME_DIR="${CMAKE_CURRENT_SOURCE_DIR}/runtime")
    add_test(NAME build.secondary-cache COMMAND gekkoaot-test-secondary-build
        "${CMAKE_CURRENT_BINARY_DIR}/secondary-regression")
    add_executable(gekkoaot-test-secondary-dispatch-cache tests/hardware/test_secondary_dispatch_cache.cpp)
    target_include_directories(gekkoaot-test-secondary-dispatch-cache PRIVATE runtime)
    add_test(NAME build.secondary-dispatch-cache COMMAND gekkoaot-test-secondary-dispatch-cache)
    # This controller integration test resolves the native runner at startup;
    # it is not applicable to a tools-only build without that helper.
    if(GEKKOAOT_BUILD_NATIVE_HOST)
        add_executable(gekkoaot-test-fp-region-policy tests/hardware/test_fp_region_policy.cpp)
        target_compile_definitions(gekkoaot-test-fp-region-policy PRIVATE
            GEKKOAOT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
        add_test(NAME build.fp-region-policy COMMAND gekkoaot-test-fp-region-policy)
    endif()
    add_executable(gekkoaot-test-native-bind-group-cache tests/hardware/test_native_bind_group_cache.cpp)
    add_test(NAME hardware.native-bind-group-cache COMMAND gekkoaot-test-native-bind-group-cache)
    add_executable(gekkoaot-test-native-snapshot-atlas tests/hardware/test_native_snapshot_atlas.cpp)
    add_test(NAME hardware.native-snapshot-atlas COMMAND gekkoaot-test-native-snapshot-atlas)
    add_executable(gekkoaot-test-native-resource-table tests/hardware/test_native_resource_table.cpp)
    add_test(NAME hardware.native-resource-table COMMAND gekkoaot-test-native-resource-table)
    add_executable(gekkoaot-test-native-vertex-program tests/hardware/test_native_vertex_program.cpp)
    add_test(NAME hardware.native-vertex-program COMMAND gekkoaot-test-native-vertex-program)
    add_executable(gekkoaot-test-native-vertex-gather tests/hardware/test_native_vertex_gather.cpp)
    add_test(NAME hardware.native-vertex-gather COMMAND gekkoaot-test-native-vertex-gather)
    add_executable(gekkoaot-test-native-read-poll tests/hardware/test_native_read_poll.cpp)
    add_test(NAME hardware.native-read-poll COMMAND gekkoaot-test-native-read-poll)
    add_executable(gekkoaot-test-native-counter-poll tests/hardware/test_native_counter_poll.cpp)
    add_test(NAME hardware.native-counter-poll COMMAND gekkoaot-test-native-counter-poll)
    add_executable(gekkoaot-test-native-spin-probe tests/hardware/test_native_spin_probe.cpp)
    add_test(NAME hardware.native-spin-probe COMMAND gekkoaot-test-native-spin-probe)
    add_executable(gekkoaot-test-native-command-stream tests/hardware/test_native_command_stream.cpp)
    add_test(NAME hardware.native-command-stream COMMAND gekkoaot-test-native-command-stream)
    add_test(NAME hardware.native-command-stream-copy COMMAND gekkoaot-test-native-command-stream)
    set_tests_properties(hardware.native-command-stream-copy PROPERTIES ENVIRONMENT "GEKKOAOT_NATIVE_GX_DIRECT_COMMAND_SPANS=0")
    add_executable(gekkoaot-test-native-index-batch tests/hardware/test_native_index_batch.cpp)
    add_test(NAME hardware.native-index-batch COMMAND gekkoaot-test-native-index-batch)
    if(UNIX)
        add_executable(gekkoaot-test-pipeline-lock tests/hardware/test_pipeline_lock.cpp)
        add_test(NAME build.pipeline-lock COMMAND gekkoaot-test-pipeline-lock
            "${CMAKE_CURRENT_BINARY_DIR}/pipeline-lock-regression")
        set_tests_properties(build.pipeline-lock PROPERTIES TIMEOUT 10)
    endif()
endif()

if(GEKKOAOT_BUILD_NATIVE_HOST)
    add_subdirectory(runtime/native)
    if(TARGET gekkoaot-test-fp-region-policy)
        add_dependencies(gekkoaot-test-fp-region-policy gekkoaot-native-run)
    endif()
endif()

if(GEKKOAOT_BUILD_GUI)
    set(CMAKE_AUTOMOC ON)
    find_package(Qt6 6.5 REQUIRED COMPONENTS Widgets)

    if(TARGET WrapVulkanHeaders::WrapVulkanHeaders)
        get_target_property(_gekkoaot_vulkan_includes
            WrapVulkanHeaders::WrapVulkanHeaders INTERFACE_INCLUDE_DIRECTORIES)
        if(_gekkoaot_vulkan_includes)
            set(_gekkoaot_vulkan_includes_fixed "${_gekkoaot_vulkan_includes}")
            list(REMOVE_ITEM _gekkoaot_vulkan_includes_fixed "/usr/bin" "/usr/bin/")
            if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS "/usr/include/vulkan/vulkan.h")
                list(APPEND _gekkoaot_vulkan_includes_fixed "/usr/include")
            endif()
            list(REMOVE_DUPLICATES _gekkoaot_vulkan_includes_fixed)
            set_property(TARGET WrapVulkanHeaders::WrapVulkanHeaders PROPERTY
                INTERFACE_INCLUDE_DIRECTORIES "${_gekkoaot_vulkan_includes_fixed}")
        endif()
    endif()

    qt_add_executable(gekkoaot
        src/main.cpp
        src/MainWindow.cpp
        src/MainWindow.hpp
        src/InputProfile.cpp
        src/InputProfile.hpp
        src/BuildController.cpp
        src/BuildController.hpp
    )
    target_link_libraries(gekkoaot PRIVATE Qt6::Widgets)
    target_compile_definitions(gekkoaot PRIVATE
        GEKKOAOT_VERSION="${PROJECT_VERSION}"
        GEKKOAOT_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        GEKKOAOT_DOLRECOMP_REV="${GEKKOAOT_DOLRECOMP_REV}"
        GEKKOAOT_AURORA_REV="${GEKKOAOT_AURORA_REV}"
    )
    add_dependencies(gekkoaot gekkoaotctl gekkoaot-module-meta gekkoaot-secondary-build)
    if(GEKKOAOT_BUILD_NATIVE_HOST)
        add_dependencies(gekkoaot gekkoaot-native-run gekkoaot-native-disc)
    endif()

    if(MSVC)
        target_compile_options(gekkoaot PRIVATE /W4)
    else()
        target_compile_options(gekkoaot PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    install(TARGETS gekkoaot RUNTIME DESTINATION bin BUNDLE DESTINATION .)

    if(WIN32)
        # Keep Qt plugins beside the Windows executable in their standard
        # type-specific subdirectories, e.g. bin/platforms/qwindows.dll.
        set(QT_DEPLOY_PLUGINS_DIR "bin")

        # Make the installed Windows tree runnable on machines without a global Qt install.
        # qt_generate_deploy_app_script uses Qt's deployment machinery / windeployqt to
        # copy Qt DLLs, the qwindows platform plugin and the MSVC runtime dependencies.
        qt_generate_deploy_app_script(
            TARGET gekkoaot
            OUTPUT_SCRIPT GEKKOAOT_QT_DEPLOY_SCRIPT
            NO_TRANSLATIONS
            NO_UNSUPPORTED_PLATFORM_ERROR
        )
        install(SCRIPT ${GEKKOAOT_QT_DEPLOY_SCRIPT})
    endif()
endif()

install(TARGETS gekkoaotctl gekkoaot-module-meta gekkoaot-secondary-build RUNTIME DESTINATION bin)
# Install only source/resource formats consumed by the native build pipeline.
# Backups, profiles and local reports beside source files must never be staged.
install(DIRECTORY runtime DESTINATION share/gekkoaot FILES_MATCHING
    PATTERN "*.c" PATTERN "*.cpp" PATTERN "*.h" PATTERN "*.hpp"
    PATTERN "*.inc" PATTERN "*.py" PATTERN "*.cmake" PATTERN "CMakeLists.txt"
    PATTERN "*.exports" PATTERN "*.ld" PATTERN "README.md"
    PATTERN "__pycache__" EXCLUDE PATTERN "*.bak" EXCLUDE
    PATTERN "*backup*" EXCLUDE)
install(DIRECTORY patches DESTINATION share/gekkoaot FILES_MATCHING PATTERN "*.patch")
install(DIRECTORY game-packs crossgame-db DESTINATION share/gekkoaot
    FILES_MATCHING PATTERN "*.json" PATTERN "README.md")
install(DIRECTORY docs DESTINATION share/gekkoaot FILES_MATCHING PATTERN "*.md")
install(DIRECTORY assets DESTINATION share/gekkoaot FILES_MATCHING PATTERN "*.png")
install(PROGRAMS
    tools/adaptive_pgo.py
    tools/crossgame_db.py
    DESTINATION share/gekkoaot/tools)
install(PROGRAMS
    tools/compat/gekkoaot-gdb.sh
    tools/compat/gekkoaot-perf.sh
    DESTINATION share/gekkoaot/tools/compat)
install(FILES README.md CHANGELOG.md CONTRIBUTING.md LICENSE COPYING THIRD_PARTY_NOTICES.md version.txt DESTINATION share/gekkoaot)

if(BUILD_TESTING)
  add_executable(gekkoaot-test-native-hot-profile tests/hardware/test_native_hot_profile.cpp)
  target_include_directories(gekkoaot-test-native-hot-profile PRIVATE tools)
  add_test(NAME build.native-hot-profile COMMAND gekkoaot-test-native-hot-profile)
endif()
