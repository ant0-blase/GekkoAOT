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

if(GEKKOAOT_BUILD_NATIVE_HOST)
    add_subdirectory(runtime/native)
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
install(DIRECTORY runtime DESTINATION share/gekkoaot)
install(DIRECTORY patches DESTINATION share/gekkoaot)
install(DIRECTORY game-packs crossgame-db docs DESTINATION share/gekkoaot)
install(PROGRAMS
    tools/adaptive_pgo.py
    tools/crossgame_db.py
    DESTINATION share/gekkoaot/tools)
install(PROGRAMS
    tools/compat/gekkoaot-gdb.sh
    tools/compat/gekkoaot-perf.sh
    DESTINATION share/gekkoaot/tools/compat)
install(FILES README.md CHANGELOG.md CONTRIBUTING.md LICENSE COPYING THIRD_PARTY_NOTICES.md version.txt DESTINATION share/gekkoaot)
