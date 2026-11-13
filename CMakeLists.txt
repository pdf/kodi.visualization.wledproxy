cmake_minimum_required(VERSION 3.20)

# The add-on version lives here only; addon.xml is generated from it.
project(visualization.wledproxy VERSION 0.1.0 LANGUAGES C CXX)

set(ADDON_ID "visualization.wledproxy")

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_CXX_FLAGS_RELEASE "-O2")
set(CMAKE_C_FLAGS_RELEASE "-O2")

option(WLEDPROXY_BUILD_TESTS "Build the test programs" ON)

include(cmake/KodiDevKit.cmake)

# ---------------------------------------------------------------------------
# Core library: everything except the exported entry points, so the tests can
# link the same code natively.
# ---------------------------------------------------------------------------
add_library(pugixml STATIC third_party/pugixml/pugixml.cpp)
target_include_directories(pugixml SYSTEM PUBLIC third_party/pugixml)
target_compile_options(pugixml PRIVATE -fvisibility=hidden -fvisibility-inlines-hidden)

add_library(wledproxy_core STATIC
  src/po.cpp
  src/resample.cpp
  src/sender.cpp
  src/target_settings.cpp
  src/version_check.cpp
  src/wled_analysis.cpp
)
target_include_directories(wledproxy_core PUBLIC src)
target_include_directories(wledproxy_core SYSTEM PUBLIC ${KODI_DEVKIT_INCLUDE_DIR})
target_link_libraries(wledproxy_core PUBLIC pugixml)
# Everything linked into the add-on must see the same kodi-dev-kit inline
# functions (versions.h reports only the types marked _USED).
target_compile_definitions(wledproxy_core PUBLIC
  BUILD_KODI_ADDON
  ADDON_GLOBAL_VERSION_GENERAL_USED
  ADDON_GLOBAL_VERSION_FILESYSTEM_USED
  ADDON_INSTANCE_VERSION_VISUALIZATION_USED)
target_compile_options(wledproxy_core PRIVATE
  -Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)

# ---------------------------------------------------------------------------
# The add-on library.
# ---------------------------------------------------------------------------
add_library(wledproxy MODULE
  src/entry.cpp
  src/redirect.cpp
  src/target.cpp
)
set_target_properties(wledproxy PROPERTIES
  OUTPUT_NAME "${ADDON_ID}"
  PREFIX ""
  SUFFIX ".so")
target_compile_definitions(wledproxy PRIVATE WLEDPROXY_ADDON_ID="${ADDON_ID}")
target_compile_options(wledproxy PRIVATE
  -Wall -Wextra -fvisibility=hidden -fvisibility-inlines-hidden)
target_link_libraries(wledproxy PRIVATE wledproxy_core ${CMAKE_DL_LIBS})
target_link_options(wledproxy PRIVATE
  "-Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map"
  -Wl,--no-undefined
  -Wl,-z,defs
  -Wl,--as-needed)
set_property(TARGET wledproxy APPEND PROPERTY LINK_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/src/exports.map")

# ---------------------------------------------------------------------------
# Staged add-on folder and packages.
# ---------------------------------------------------------------------------
set(STAGE_DIR "${CMAKE_BINARY_DIR}/stage/${ADDON_ID}")
set(ADDON_VERSION "${PROJECT_VERSION}")
# addon.xml imports: version = the headers' API version, minversion = their
# _MIN (the oldest Kodi ABI they work with), as Kodi's own add-on build writes
# them. Without minversion, Kodi requires at least `version`.
file(READ "${KODI_DEVKIT_INCLUDE_DIR}/kodi/versions.h" _kodi_versions_h)
foreach(_api GLOBAL_VERSION_MAIN GLOBAL_VERSION_GENERAL GLOBAL_VERSION_FILESYSTEM
             INSTANCE_VERSION_VISUALIZATION)
  foreach(_suffix "" "_MIN")
    if(NOT _kodi_versions_h MATCHES "#define ADDON_${_api}${_suffix}[ \t]+\"([0-9.]+)\"")
      message(FATAL_ERROR "ADDON_${_api}${_suffix} not found in versions.h")
    endif()
    set(KODI_${_api}${_suffix} "${CMAKE_MATCH_1}")
  endforeach()
endforeach()
configure_file(addon/addon.xml.in "${CMAKE_BINARY_DIR}/addon.xml" @ONLY)

file(GLOB_RECURSE ADDON_RESOURCE_FILES CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/addon/resources/*")

add_custom_target(stage
  COMMAND "${CMAKE_COMMAND}" -E rm -rf "${STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E make_directory "${STAGE_DIR}"
  COMMAND "${CMAKE_COMMAND}" -E copy_directory
          "${CMAKE_CURRENT_SOURCE_DIR}/addon/resources" "${STAGE_DIR}/resources"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_BINARY_DIR}/addon.xml" "${STAGE_DIR}/addon.xml"
  COMMAND "${CMAKE_COMMAND}" -E copy "$<TARGET_FILE:wledproxy>" "${STAGE_DIR}/"
  COMMAND "${CMAKE_COMMAND}" -E copy "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE" "${STAGE_DIR}/LICENSE.txt"
  DEPENDS wledproxy "${CMAKE_BINARY_DIR}/addon.xml" ${ADDON_RESOURCE_FILES}
  COMMENT "Staging ${ADDON_ID}"
  VERBATIM)

set(PACKAGE_BASENAME "${ADDON_ID}-${PROJECT_VERSION}")
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_custom_target(package_addon
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/make_package.py"
            --stage "${CMAKE_BINARY_DIR}/stage" --id "${ADDON_ID}"
            --out "${CMAKE_BINARY_DIR}/${PACKAGE_BASENAME}"
    DEPENDS stage
    COMMENT "Packaging ${PACKAGE_BASENAME}.tar.gz and .zip"
    VERBATIM)
  # "make package" in the build directory.
  add_custom_target(package DEPENDS package_addon)
endif()

add_custom_target(check_elf
  COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/tools/check_elf.sh" "$<TARGET_FILE:wledproxy>"
  DEPENDS wledproxy
  VERBATIM)

# ---------------------------------------------------------------------------
# Tests (native builds only).
# ---------------------------------------------------------------------------
if(WLEDPROXY_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING)
  enable_testing()
  add_subdirectory(tests)
endif()
