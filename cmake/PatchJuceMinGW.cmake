if(NOT DEFINED JUCE_SOURCE_DIR OR NOT DEFINED SKELLYTUNE_CMAKE_DIR)
    message(FATAL_ERROR "JUCE_SOURCE_DIR and SKELLYTUNE_CMAKE_DIR are required")
endif()
find_package(Git REQUIRED)
set(patch "${SKELLYTUNE_CMAKE_DIR}/juce-8.0.4-mingw.patch")
execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --check "${patch}"
    WORKING_DIRECTORY "${JUCE_SOURCE_DIR}" RESULT_VARIABLE can_apply OUTPUT_QUIET ERROR_QUIET)
if(can_apply EQUAL 0)
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace "${patch}"
        WORKING_DIRECTORY "${JUCE_SOURCE_DIR}" COMMAND_ERROR_IS_FATAL ANY)
else()
    execute_process(COMMAND "${GIT_EXECUTABLE}" apply --ignore-whitespace --reverse --check "${patch}"
        WORKING_DIRECTORY "${JUCE_SOURCE_DIR}" RESULT_VARIABLE already_applied OUTPUT_QUIET ERROR_QUIET)
    if(NOT already_applied EQUAL 0)
        message(FATAL_ERROR "JUCE MinGW patch does not match this source. Use clean JUCE 8.0.4 or disable SKELLYTUNE_MINGW_COMPAT.")
    endif()
endif()
file(COPY "${SKELLYTUNE_CMAKE_DIR}/SkellyTuneMinGW.cmake"
          "${SKELLYTUNE_CMAKE_DIR}/common-controls.manifest"
    DESTINATION "${JUCE_SOURCE_DIR}/extras/Build/juceaide")
