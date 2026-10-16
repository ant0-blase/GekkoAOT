# Optional official SDK, with the same C++ backend on desktop and Android.
# No SDK binary is downloaded implicitly: Discord distributes it through the
# application's developer portal under its own SDK terms.
set(GEKKOAOT_DISCORD_SDK "AUTO" CACHE STRING "Discord Social SDK: AUTO, ON (required), or OFF")
set_property(CACHE GEKKOAOT_DISCORD_SDK PROPERTY STRINGS AUTO ON OFF)
set(_gekkoaot_discord_sdk_default "$ENV{GEKKOAOT_DISCORD_SDK_ROOT}")
if(NOT _gekkoaot_discord_sdk_default)
    foreach(candidate "${PROJECT_SOURCE_DIR}/.deps/discord_social_sdk"
            "${PROJECT_SOURCE_DIR}/.gekkoaot/deps/discord-social-sdk/discord_social_sdk")
        if(EXISTS "${candidate}/include/discordpp.h")
            set(_gekkoaot_discord_sdk_default "${candidate}")
            break()
        endif()
    endforeach()
endif()
set(GEKKOAOT_DISCORD_SDK_ROOT "${_gekkoaot_discord_sdk_default}" CACHE PATH "Extracted official Discord Social SDK (1.10+)")
set(GEKKOAOT_DISCORD_DEFAULT_APPLICATION_ID "1560710093606228101")
set(GEKKOAOT_DISCORD_APPLICATION_ID "${GEKKOAOT_DISCORD_DEFAULT_APPLICATION_ID}" CACHE STRING "Public Discord application ID for packaged builds")
# Adopt the project ID in checkouts configured before the application existed.
if(GEKKOAOT_DISCORD_APPLICATION_ID STREQUAL "")
    set(GEKKOAOT_DISCORD_APPLICATION_ID "${GEKKOAOT_DISCORD_DEFAULT_APPLICATION_ID}" CACHE STRING "Public Discord application ID for packaged builds" FORCE)
endif()
if(NOT GEKKOAOT_DISCORD_SDK MATCHES "^(AUTO|ON|OFF)$")
    message(FATAL_ERROR "GEKKOAOT_DISCORD_SDK must be AUTO, ON or OFF")
endif()
if(NOT GEKKOAOT_DISCORD_APPLICATION_ID STREQUAL "" AND NOT GEKKOAOT_DISCORD_APPLICATION_ID MATCHES "^[1-9][0-9]*$")
    message(FATAL_ERROR "GEKKOAOT_DISCORD_APPLICATION_ID must be a decimal public application ID")
endif()

add_library(gekkoaot-discord STATIC
    "${PROJECT_SOURCE_DIR}/src/DiscordPresence.cpp" "${PROJECT_SOURCE_DIR}/src/DiscordPresence.hpp"
    "${PROJECT_SOURCE_DIR}/src/DiscordSocialSdk.cpp")
target_link_libraries(gekkoaot-discord PUBLIC Qt6::Core)
target_compile_definitions(gekkoaot-discord PRIVATE
    GEKKOAOT_DISCORD_APPLICATION_ID="${GEKKOAOT_DISCORD_APPLICATION_ID}")
set(GEKKOAOT_HAS_DISCORD_SDK OFF)

if(NOT GEKKOAOT_DISCORD_SDK STREQUAL "OFF")
    if(ANDROID)
        # Prefab supplied by the official discord_partner_sdk.aar. Its Java
        # initialization and RPC transport are required alongside the native SO.
        find_package(discord_partner_sdk QUIET CONFIG)
        if(TARGET discord_partner_sdk::discord_partner_sdk)
            target_link_libraries(gekkoaot-discord PRIVATE discord_partner_sdk::discord_partner_sdk)
            set(GEKKOAOT_HAS_DISCORD_SDK ON)
        endif()
    elseif(GEKKOAOT_DISCORD_SDK_ROOT)
        find_path(GEKKOAOT_DISCORD_INCLUDE_DIR discordpp.h
            PATHS "${GEKKOAOT_DISCORD_SDK_ROOT}/include"
                "${GEKKOAOT_DISCORD_SDK_ROOT}/lib/release/discord_partner_sdk.framework/Headers"
            NO_DEFAULT_PATH REQUIRED)
        # Both paths can be supplied explicitly for an SDK layout/architecture
        # different from the standard standalone archive. Never select a host
        # SDK from /usr when cross-compiling another operating system.
        find_library(GEKKOAOT_DISCORD_LIBRARY NAMES discord_partner_sdk
            PATHS "${GEKKOAOT_DISCORD_SDK_ROOT}/lib/release"
            NO_DEFAULT_PATH REQUIRED)
        add_library(gekkoaot-discord-sdk UNKNOWN IMPORTED)
        set_target_properties(gekkoaot-discord-sdk PROPERTIES
            IMPORTED_LOCATION "${GEKKOAOT_DISCORD_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GEKKOAOT_DISCORD_INCLUDE_DIR}")
        if(WIN32)
            find_file(GEKKOAOT_DISCORD_RUNTIME NAMES discord_partner_sdk.dll
                PATHS "${GEKKOAOT_DISCORD_SDK_ROOT}/bin/release" NO_DEFAULT_PATH REQUIRED)
        endif()
        target_link_libraries(gekkoaot-discord PRIVATE gekkoaot-discord-sdk)
        set(GEKKOAOT_HAS_DISCORD_SDK ON)
    endif()
    if(GEKKOAOT_HAS_DISCORD_SDK)
        target_compile_definitions(gekkoaot-discord PRIVATE GEKKOAOT_DISCORD_SOCIAL_SDK=1)
        message(STATUS "gekkoaot: Discord Social SDK enabled (${CMAKE_SYSTEM_NAME})")
    elseif(GEKKOAOT_DISCORD_SDK STREQUAL "ON")
        message(FATAL_ERROR "Discord Social SDK required: set GEKKOAOT_DISCORD_SDK_ROOT on desktop, or provide the official 1.10+ Android AAR/Prefab package")
    else()
        message(STATUS "gekkoaot: Discord integration available after supplying the official Social SDK (see docs/discord-rich-presence.md)")
    endif()
endif()

function(gekkoaot_deploy_discord target)
    if(NOT GEKKOAOT_HAS_DISCORD_SDK OR ANDROID)
        return()
    endif()
    if(EXISTS "${GEKKOAOT_DISCORD_SDK_ROOT}/License-Notices.txt")
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${GEKKOAOT_DISCORD_SDK_ROOT}/License-Notices.txt"
            "$<TARGET_FILE_DIR:${target}>/Discord-SDK-License-Notices.txt")
        if(target STREQUAL "gekkoaot")
            install(FILES "${GEKKOAOT_DISCORD_SDK_ROOT}/License-Notices.txt"
                DESTINATION share/licenses/GekkoAOT/discord)
        endif()
    endif()
    if(WIN32)
        set(runtime "${GEKKOAOT_DISCORD_RUNTIME}")
    else()
        set(runtime "${GEKKOAOT_DISCORD_LIBRARY}")
    endif()
    if(APPLE AND IS_DIRECTORY "${runtime}" AND runtime MATCHES "[.]framework$")
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${runtime}" "$<TARGET_FILE_DIR:${target}>/Frameworks/discord_partner_sdk.framework")
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "@loader_path/Frameworks")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "@loader_path/Frameworks")
        if(target STREQUAL "gekkoaot")
            install(DIRECTORY "${runtime}" DESTINATION bin/Frameworks)
        endif()
    else()
        add_custom_command(TARGET ${target} POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${runtime}" "$<TARGET_FILE_DIR:${target}>")
        if(APPLE)
            set(origin "@loader_path")
        else()
            set(origin "$ORIGIN")
        endif()
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "${origin}")
        set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "${origin}")
        if(target STREQUAL "gekkoaot")
            install(FILES "${runtime}" DESTINATION bin)
        endif()
    endif()
endfunction()
