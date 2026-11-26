# Recover the exact Classic core used by the approved Playground listening path.
include(FetchContent)
FetchContent_Declare(SkellyClassicPlayground
    GIT_REPOSITORY https://github.com/skellyruno/skelltune-playground.git
    GIT_TAG 9528d2d32d919ddd182321a3bcbd521874a8e81c
    SOURCE_SUBDIR _dependency_only)
FetchContent_Declare(SkellyClassicSnap
    GIT_REPOSITORY https://github.com/skellyruno/skell-snap.git
    GIT_TAG 8a4876259ebf30dd100204be1cf9beae2c3b5f03
    SOURCE_SUBDIR _dependency_only)
FetchContent_MakeAvailable(SkellyClassicPlayground SkellyClassicSnap)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SKELLY_CLASSIC_RECOVERED "${CMAKE_CURRENT_BINARY_DIR}/recovered-classic")
execute_process(COMMAND "${Python3_EXECUTABLE}"
    "${skellyclassicplayground_SOURCE_DIR}/tools/recover_snap_core.py"
    "${skellyclassicsnap_SOURCE_DIR}" "${SKELLY_CLASSIC_RECOVERED}/SnapCore.h"
    RESULT_VARIABLE recover_result)
if(NOT recover_result EQUAL 0)
    message(FATAL_ERROR "Cannot recover the pinned Classic core: ${recover_result}")
endif()
file(SHA256 "${SKELLY_CLASSIC_RECOVERED}/SnapCore.h" classic_sha)
if(NOT classic_sha STREQUAL "ca46212c500efe397968a2ef9869050f26b0ea015b5f462fd8ac99ffb6b3672b")
    message(FATAL_ERROR "Classic DSP checksum differs from the approved Playground source")
endif()
add_library(skellytune_classic INTERFACE)
target_include_directories(skellytune_classic INTERFACE
    "${SKELLY_CLASSIC_RECOVERED}" "${skellyclassicplayground_SOURCE_DIR}/dsp")
target_compile_features(skellytune_classic INTERFACE cxx_std_17)
