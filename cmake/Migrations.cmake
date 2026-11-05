# SPDX-License-Identifier: GPL-3.0-or-later
# Cache migrations for build directories configured with an older project.
#
# CMake keeps cached values when a CACHE default changes, so a build directory
# configured with a previous default pin would keep building against it. Each
# block below replaces only a former project default with the current one;
# explicit custom pins are left untouched. Included by CMakeLists.txt after the
# pins are declared.

# The DolRecomp pin moved from 40637c46 to 71ce7f97.
if(GEKKOAOT_DOLRECOMP_REV STREQUAL "40637c4683bd2820ac5b23607ee344720beb26df")
    message(STATUS "gekkoaot: migrating DolRecomp pin 40637c46 -> 71ce7f97")
    set(GEKKOAOT_DOLRECOMP_REV "${GEKKOAOT_DOLRECOMP_DEFAULT_REV}" CACHE STRING "Pinned DolRecomp revision" FORCE)
endif()

# The Aurora pin stays one commit before 3840bf9 ("Fix GX EFB alpha
# handling"), which a regression bisect identified; 7f2801cd and 3840bf9a
# were earlier defaults.
if(GEKKOAOT_AURORA_REV STREQUAL "7f2801cd0133c9333eadb4e2e6b24100c328d328" OR
   GEKKOAOT_AURORA_REV STREQUAL "3840bf9ae735191026e4d4edb0ce6f24d91f7eea")
    message(STATUS "gekkoaot: migrating Aurora pin to 9c0bf66f (before the EFB alpha change)")
    set(GEKKOAOT_AURORA_REV "${GEKKOAOT_AURORA_DEFAULT_REV}" CACHE STRING "Pinned Aurora revision with retail FIFO command processor" FORCE)
endif()
