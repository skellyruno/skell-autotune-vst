cmake_minimum_required(VERSION 3.22)

project(HumHouseVocalTune VERSION 1.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Static MSVC runtime so users don't need VC++ redist
if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>"
        CACHE STRING "" FORCE)
endif()

# Warnings
if(MSVC)
    add_compile_options(/W4)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)
endif()

# Offline regressions use the real DSP headers with a minimal JUCE shim. Keep
# the original DSP-only option as an alias; its former test sources are absent.
option(SKELLYTUNE_LOW_LATENCY_TESTS_ONLY "Build focused offline tuning regressions" OFF)
option(SKELLYTUNE_DSP_TESTS_ONLY "Build offline tuning regressions (compatibility alias)" OFF)
option(SKELLYTUNE_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan for offline tests" OFF)

if(SKELLYTUNE_LOW_LATENCY_TESTS_ONLY OR SKELLYTUNE_DSP_TESTS_ONLY)
    enable_testing()
    add_executable(low_latency_regression tests/low_latency_regression.cpp)
    target_include_directories(low_latency_regression PRIVATE tests/support Source)
    if(SKELLYTUNE_ENABLE_SANITIZERS)
        if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "SKELLYTUNE_ENABLE_SANITIZERS requires GCC or Clang")
        endif()
        target_compile_options(low_latency_regression PRIVATE
            -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
        target_link_options(low_latency_regression PRIVATE -fsanitize=address,undefined)
    endif()
    add_test(NAME low_latency_regression COMMAND low_latency_regression)
    message(STATUS "Offline tuning regressions enabled; JUCE and plugin builds are skipped")
    return()
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

set(JUCE_VERSION 8.0.4 CACHE STRING "JUCE version to fetch")

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${JUCE_VERSION}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(JUCE)

# ---------------------------------------------------------------------------
# Plugin target
# ---------------------------------------------------------------------------
set(HUMTUNE_FORMATS VST3 Standalone)
if(APPLE)
    list(APPEND HUMTUNE_FORMATS AU)
endif()

juce_add_plugin(HumHouseVocalTune
    COMPANY_NAME              "HumHouse"
    BUNDLE_ID                 "com.humhouse.skellytune"
    PLUGIN_MANUFACTURER_CODE  Humh
    PLUGIN_CODE               Sktn
    FORMATS                   ${HUMTUNE_FORMATS}
    PRODUCT_NAME              "SkellyTune"
    IS_SYNTH                  FALSE
    NEEDS_MIDI_INPUT          FALSE
    NEEDS_MIDI_OUTPUT         FALSE
    IS_MIDI_EFFECT            FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD   FALSE
    VST3_CATEGORIES           "Fx" "Pitch Shift"
)

juce_generate_juce_header(HumHouseVocalTune)

target_sources(HumHouseVocalTune
    PRIVATE
        Source/PluginProcessor.cpp
        Source/PluginEditor.cpp
)

target_compile_definitions(HumHouseVocalTune
    PUBLIC
        JUCE_WEB_BROWSER=0
        JUCE_USE_CURL=0
        JUCE_VST3_CAN_REPLACE_VST2=0
        JUCE_DISPLAY_SPLASH_SCREEN=0
)

target_link_libraries(HumHouseVocalTune
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)
