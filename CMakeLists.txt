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

# ---------------------------------------------------------------------------
# Offline DSP-only build (no JUCE download required)
# ---------------------------------------------------------------------------
# This option builds the real DSP engine and its regression executables using
# the minimal JUCE test shim in tests/JuceHeader.h. It is useful in CI and in
# restricted/headless environments where fetching JUCE or loading a GUI plugin
# is not possible. It does NOT build a VST3 or Standalone application.
option(SKELLYTUNE_DSP_TESTS_ONLY "Build offline DSP regression executables without JUCE" OFF)
option(SKELLYTUNE_ENABLE_SANITIZERS "Enable AddressSanitizer and UBSan for DSP-only tests" OFF)

if(SKELLYTUNE_DSP_TESTS_ONLY)
    if(SKELLYTUNE_ENABLE_SANITIZERS)
        if(MSVC OR NOT CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            message(FATAL_ERROR "SKELLYTUNE_ENABLE_SANITIZERS requires GCC or Clang")
        endif()
    endif()

    enable_testing()
    find_package(Threads REQUIRED)
    set(SKELLYTUNE_DSP_TESTS
        stereo_detector_smoke
        pitch_shifter_quality
        autotune_regression
        realtime_allocation_smoke
        scale_quantizer_regression
        key_glide_regression
        key_estimator_regression
        latency_sweep_regression
        engine_safety_regression
        voicing_formant_regression
    )

    foreach(test_name IN LISTS SKELLYTUNE_DSP_TESTS)
        add_executable(${test_name} "tests/${test_name}.cpp")
        target_include_directories(${test_name} PRIVATE tests Source)
        target_compile_features(${test_name} PRIVATE cxx_std_20)
        target_link_libraries(${test_name} PRIVATE Threads::Threads)
        if(SKELLYTUNE_ENABLE_SANITIZERS)
            target_compile_options(${test_name} PRIVATE
                -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer)
            target_link_options(${test_name} PRIVATE -fsanitize=address,undefined)
        endif()
        add_test(NAME ${test_name} COMMAND ${test_name})
    endforeach()

    # Offline audio renderer used for real-vocal A/B diagnostics. This is a tool,
    # not a test case: it accepts an input.raw file and writes a float WAV + CSV.
    add_executable(vocal_render_harness tests/vocal_render_harness.cpp)
    target_include_directories(vocal_render_harness PRIVATE tests Source)
    target_compile_features(vocal_render_harness PRIVATE cxx_std_20)

    message(STATUS "SkellyTune DSP-only mode enabled: JUCE fetch and plugin targets are skipped")
    message(STATUS "These targets validate the DSP core; they do not build a VST3/Standalone plugin")
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
