# Fetches the kodi-dev-kit headers from upstream xbmc/xbmc at a pinned Piers
# release. CoreELEC 22 builds Kodi from these upstream tags (stable and
# nightly builds alike); addon.xml declares the headers' minimum API versions,
# so the add-on also runs on older Piers builds down to those minimums.
#
# Only xbmc/addons/kodi-dev-kit/include is checked out (sparse, blobless,
# depth 1), so the download is a few megabytes instead of the whole tree.
# Sets KODI_DEVKIT_INCLUDE_DIR.

set(KODI_XBMC_REPO "https://github.com/xbmc/xbmc.git" CACHE STRING
    "Repository the kodi-dev-kit headers are taken from")
set(KODI_XBMC_COMMIT "d686683a55d50e576dd282f53fb2bf9914931b38" CACHE STRING
    "Commit of the kodi-dev-kit headers (tag 22.0rc2-Piers)")
set(KODI_DEVKIT_SOURCE_DIR "" CACHE PATH
    "Existing checkout containing xbmc/addons/kodi-dev-kit/include; skips the download")

find_package(Git REQUIRED)

if(KODI_DEVKIT_SOURCE_DIR)
  set(_kodi_root "${KODI_DEVKIT_SOURCE_DIR}")
else()
  set(_kodi_root "${CMAKE_BINARY_DIR}/_deps/kodi-xbmc-${KODI_XBMC_COMMIT}")
  set(_kodi_stamp "${_kodi_root}/.fetched")
  if(NOT EXISTS "${_kodi_stamp}")
    message(STATUS "Fetching kodi-dev-kit headers at ${KODI_XBMC_COMMIT}")
    file(REMOVE_RECURSE "${_kodi_root}")
    file(MAKE_DIRECTORY "${_kodi_root}")
    foreach(_cmd
        "init;-q"
        "remote;add;origin;${KODI_XBMC_REPO}"
        "sparse-checkout;set;--no-cone;/xbmc/addons/kodi-dev-kit/include/"
        "fetch;-q;--depth;1;--filter=blob:none;origin;${KODI_XBMC_COMMIT}"
        "checkout;-q;FETCH_HEAD")
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" ${_cmd}
        WORKING_DIRECTORY "${_kodi_root}"
        RESULT_VARIABLE _rc
        OUTPUT_QUIET)
      if(NOT _rc EQUAL 0)
        message(FATAL_ERROR "git ${_cmd} failed while fetching kodi-dev-kit headers")
      endif()
    endforeach()
    file(TOUCH "${_kodi_stamp}")
  endif()
endif()

set(KODI_DEVKIT_INCLUDE_DIR "${_kodi_root}/xbmc/addons/kodi-dev-kit/include")
if(NOT EXISTS "${KODI_DEVKIT_INCLUDE_DIR}/kodi/versions.h")
  message(FATAL_ERROR "kodi-dev-kit headers not found in ${KODI_DEVKIT_INCLUDE_DIR}")
endif()
