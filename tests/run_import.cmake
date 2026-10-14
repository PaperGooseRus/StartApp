# Older CMake versions read File API queries before CMakeLists.txt is evaluated.
# Reconfigure only this repository's own build to refresh its metadata.
execute_process(COMMAND "${CMAKE_COMMAND}" -S "${SOURCE_DIR}" -B "${BUILD_DIR}"
    RESULT_VARIABLE configure_result)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "Cannot refresh self-test build metadata")
endif()
execute_process(COMMAND "${TEST_EXE}" "${SOURCE_DIR}" "${BUILD_DIR}" RESULT_VARIABLE test_result)
if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "Real CMake metadata import test failed: ${test_result}")
endif()
