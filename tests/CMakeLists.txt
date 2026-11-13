add_executable(test_analysis test_analysis.cpp)
target_link_libraries(test_analysis PRIVATE wledproxy_core)
add_test(NAME analysis COMMAND test_analysis)

set(TEST_TMP "${CMAKE_CURRENT_BINARY_DIR}/tmp")
add_executable(test_settings test_settings.cpp)
target_link_libraries(test_settings PRIVATE wledproxy_core)
target_compile_definitions(test_settings PRIVATE
  WLEDPROXY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  WLEDPROXY_TEST_TMP="${TEST_TMP}")
add_test(NAME settings COMMAND test_settings)

# Mock target libraries and the loader harness.
add_library(mock_target MODULE mock_target.cpp)
target_include_directories(mock_target SYSTEM PRIVATE ${KODI_DEVKIT_INCLUDE_DIR})
set_target_properties(mock_target PROPERTIES PREFIX "")

add_library(mock_target_old MODULE mock_target.cpp)
target_include_directories(mock_target_old SYSTEM PRIVATE ${KODI_DEVKIT_INCLUDE_DIR})
target_compile_definitions(mock_target_old PRIVATE MOCK_VIS_VERSION="4.0.0")
set_target_properties(mock_target_old PROPERTIES PREFIX "")

add_executable(harness harness.cpp)
target_include_directories(harness SYSTEM PRIVATE ${KODI_DEVKIT_INCLUDE_DIR})
target_link_libraries(harness PRIVATE ${CMAKE_DL_LIBS})
target_compile_definitions(harness PRIVATE
  WLEDPROXY_SO="$<TARGET_FILE:wledproxy>"
  MOCK_TARGET_SO="$<TARGET_FILE:mock_target>"
  MOCK_TARGET_OLD_SO="$<TARGET_FILE:mock_target_old>"
  WLEDPROXY_FIXTURES="${CMAKE_CURRENT_SOURCE_DIR}/fixtures"
  WLEDPROXY_TEST_TMP="${TEST_TMP}")
add_dependencies(harness wledproxy mock_target mock_target_old)
add_test(NAME harness COMMAND harness)

if(Python3_FOUND)
  add_test(NAME package
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_package.py"
            "${CMAKE_BINARY_DIR}" "${ADDON_ID}" "${PROJECT_VERSION}")
endif()

if(Python3_FOUND)
  add_test(NAME make_repo
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_make_repo.py")
endif()
