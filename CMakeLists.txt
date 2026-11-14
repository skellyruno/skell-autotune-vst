cmake_minimum_required(VERSION 3.22)

project(HumHouseVocalTune VERSION 1.1.2 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# The residual source-cycle candidate is still under listening evaluation.
# Keep the accepted r7 synthesis by default; this changes no saved parameter.
option(SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT
       "Enable experimental residual evidence for source-cycle timing" OFF)
add_library(skellytune_dsp_configuration INTERFACE)
if(SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT)
    target_compile_definitions(skellytune_dsp_configuration INTERFACE
        SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT=1)
endif()

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
        fast_hop_toggle_regression
        fuzz_robustness_regression
        scale_quantizer_regression
        key_glide_regression
        key_estimator_regression
        latency_sweep_regression
        engine_safety_regression
        voicing_formant_regression
        note_transition_regression
        low_latency_coverage_regression
        onset_trace_regression
        predictive_grain_regression
        phase_locked_cycle_regression
        standard_causal_consistency_regression
        phase_window_artifact_regression
        phase_history_bounds_regression
        neutral_bypass_gate_regression
        overlap_phase_coherence_regression
        dry_delay_automation_regression
        handoff_energy_regression
        cancellation_diagnostic_regression
        range_protection_regression
        stereo_cancellation_continuity_regression
        steady_note_forecast_regression
        v39_steady_phase_precision_regression
        stereo_phase_mark_link_regression
        overlap_gain_regression
        live_unity_phase_regression
        live_cycle_clock_regression
        live_spectral_stereo_regression
        harmonic_evidence_regression
        cycle_formant_energy_regression
        source_epoch_regression
        mode_rate_matrix_regression
        spectral_evaluator_regression
        glide_wave_cache_regression
        moderate_harmonic_pop_regression
        handoff_crest_spectrum_regression
        natural_alias_regression
        epoch_observation_regression
        harmonic_phase_modulation_regression
        natural_vocal_cycle_regression
        heavy_source_rebuild_regression
        fractional_period_regression
        weak_fundamental_regression
        epoch_reference_regression
        standard_period_evidence_regression
        standard_register_tail_regression
        model_reuse_regression
        onset_forecast_braking_regression
        onset_reentry_regression
        periodic_basis_equivalence_regression
        gain_ramp_regression
        enable_history_regression
        frequency_slew_regression
        detector_difference_regression
    )

    if(SKELLYTUNE_EXPERIMENTAL_EPOCH_REFINEMENT)
        list(APPEND SKELLYTUNE_DSP_TESTS vocal_tract_epoch_regression)
    endif()

    # The real-vocal fixtures are supplied separately from the public checkout.
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/harmonic_alias_g_500ms.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS harmonic_evidence_regression)
        message(STATUS "Skipping harmonic_evidence_regression: local vocal fixture required")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/near_dual_mono_4s.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS live_spectral_stereo_regression)
        message(STATUS "Skipping live_spectral_stereo_regression: local vocal fixture required")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/moderate_harmonic_pop_6s.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS moderate_harmonic_pop_regression)
        message(STATUS "Skipping moderate_harmonic_pop_regression: local vocal fixture required")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/natural_a_alias.f32" OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/natural_a_register_guard.f32" OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/natural_g_register_guard.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS natural_alias_regression)
        message(STATUS "Skipping natural_alias_regression: local vocal fixture required")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/near_dual_mono_4s.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS natural_vocal_cycle_regression)
        message(STATUS "Skipping natural_vocal_cycle_regression: local vocal fixture required")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/moderate_harmonic_pop_6s.f32" OR NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/strong_cycle_reentry_A_3s.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS onset_reentry_regression)
        message(STATUS "Skipping onset_reentry_regression: local vocal fixture required")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/moderate_harmonic_pop_6s.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS standard_period_evidence_regression)
        message(STATUS "Skipping standard_period_evidence_regression: local vocal fixture required")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/standard_register_tail_16s.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS standard_register_tail_regression)
        message(STATUS "Skipping standard_register_tail_regression: local vocal fixture required")
    endif()
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/near_dual_mono_4s.f32")
        list(REMOVE_ITEM SKELLYTUNE_DSP_TESTS stereo_phase_mark_link_regression)
        message(STATUS "Skipping stereo_phase_mark_link_regression: local vocal fixture required")
    endif()

    foreach(test_name IN LISTS SKELLYTUNE_DSP_TESTS)
        add_executable(${test_name} "tests/${test_name}.cpp")
        target_include_directories(${test_name} PRIVATE tests Source)
        target_compile_features(${test_name} PRIVATE cxx_std_20)
        target_link_libraries(${test_name} PRIVATE Threads::Threads skellytune_dsp_configuration)
        # Regression tests use assert(): CMake Release normally adds -DNDEBUG,
        # which would silently disable most of their actual quality gates.
        # Explicitly undo that definition for test executables only.
        if(MSVC)
            target_compile_options(${test_name} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_name} PRIVATE -UNDEBUG)
        endif()
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
    target_link_libraries(vocal_render_harness PRIVATE skellytune_dsp_configuration)

    # Reproducible full-vocal renderer with preallocated per-hop diagnostics.
    # Passing '-' for both CSV paths disconnects offline instrumentation for timing.
    add_executable(vocal_trace_renderer tests/render_trace.cpp)
    target_include_directories(vocal_trace_renderer PRIVATE tests Source)
    target_compile_features(vocal_trace_renderer PRIVATE cxx_std_20)
    target_link_libraries(vocal_trace_renderer PRIVATE skellytune_dsp_configuration)

    add_executable(realtime_stress_benchmark tests/realtime_stress_benchmark.cpp)
    target_include_directories(realtime_stress_benchmark PRIVATE tests Source)
    target_compile_features(realtime_stress_benchmark PRIVATE cxx_std_20)
    target_link_libraries(realtime_stress_benchmark PRIVATE skellytune_dsp_configuration)

    message(STATUS "SkellyTune DSP-only mode enabled: JUCE fetch and plugin targets are skipped")
    message(STATUS "These targets validate the DSP core; they do not build a VST3/Standalone plugin")
    return()
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
include(FetchContent)

set(JUCE_VERSION 8.0.4 CACHE STRING "JUCE version to fetch")

option(SKELLYTUNE_MINGW_COMPAT "Apply local JUCE 8.0.4 compatibility fixes for MinGW" ON)
set(SKELLYTUNE_JUCE_PATCH_COMMAND "")
if(MINGW AND SKELLYTUNE_MINGW_COMPAT)
    if(NOT JUCE_VERSION STREQUAL "8.0.4")
        message(FATAL_ERROR "The MinGW compatibility patch requires JUCE 8.0.4")
    endif()
    set(SKELLYTUNE_JUCE_PATCH_COMMAND
        "${CMAKE_COMMAND}" "-DJUCE_SOURCE_DIR=<SOURCE_DIR>"
        "-DSKELLYTUNE_CMAKE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/cmake"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PatchJuceMinGW.cmake")
    # FetchContent skips PATCH_COMMAND when an explicit source directory is used.
    if(FETCHCONTENT_SOURCE_DIR_JUCE)
        set(JUCE_SOURCE_DIR "${FETCHCONTENT_SOURCE_DIR_JUCE}")
        set(SKELLYTUNE_CMAKE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
        include(cmake/PatchJuceMinGW.cmake)
    endif()
endif()

FetchContent_Declare(
    JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG        ${JUCE_VERSION}
    GIT_SHALLOW    TRUE
    PATCH_COMMAND ${SKELLYTUNE_JUCE_PATCH_COMMAND}
)
FetchContent_MakeAvailable(JUCE)

# SkellCore provides the hub-free, same-session Key/Scale/A4 receiver.
# DSP-only builds above remain completely independent of this dependency.
FetchContent_Declare(SkellCoreShared
    GIT_REPOSITORY https://github.com/skellyruno/skellcore-vst-template.git
    GIT_TAG 4c499c58cb3b2791eed2d5dcef6d8d543fcaea8d
    SOURCE_SUBDIR SkellCore)
FetchContent_MakeAvailable(SkellCoreShared)


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
        SkellCore::UI
    PUBLIC
        skellytune_dsp_configuration
        juce::juce_recommended_config_flags
        juce::juce_recommended_lto_flags
        juce::juce_recommended_warning_flags
)

if(MINGW AND SKELLYTUNE_MINGW_COMPAT)
    include(cmake/SkellyTuneMinGW.cmake)
    skellytune_configure_mingw_target(HumHouseVocalTune)
    skellytune_configure_mingw_target(HumHouseVocalTune_Standalone)
    skellytune_configure_mingw_target(HumHouseVocalTune_VST3)
    skellytune_configure_mingw_target(juce_vst3_helper)
endif()

if(WIN32)
    add_executable(skellytune_vst3_smoke EXCLUDE_FROM_ALL tests/vst3_binary_smoke.cpp)
    target_include_directories(skellytune_vst3_smoke PRIVATE
        "${juce_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK")
    if(MSVC)
        target_compile_options(skellytune_vst3_smoke PRIVATE /UNDEBUG)
    else()
        target_compile_options(skellytune_vst3_smoke PRIVATE -UNDEBUG)
    endif()
    if(MINGW AND SKELLYTUNE_MINGW_COMPAT)
        skellytune_configure_mingw_target(skellytune_vst3_smoke)
    endif()
    add_executable(skellytune_vst3_contract EXCLUDE_FROM_ALL tests/vst3_contract_host.cpp)
    target_include_directories(skellytune_vst3_contract PRIVATE
        "${juce_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK")
    if(MSVC)
        target_compile_options(skellytune_vst3_contract PRIVATE /UNDEBUG)
    else()
        target_compile_options(skellytune_vst3_contract PRIVATE -UNDEBUG)
    endif()
    if(MINGW AND SKELLYTUNE_MINGW_COMPAT)
        skellytune_configure_mingw_target(skellytune_vst3_contract)
    endif()
    add_executable(skellytune_vst3_stress EXCLUDE_FROM_ALL tests/vst3_stress_host.cpp)
    target_include_directories(skellytune_vst3_stress PRIVATE
        "${juce_SOURCE_DIR}/modules/juce_audio_processors/format_types/VST3_SDK")
    if(MSVC)
        target_compile_options(skellytune_vst3_stress PRIVATE /UNDEBUG)
    else()
        target_compile_options(skellytune_vst3_stress PRIVATE -UNDEBUG)
    endif()
    if(MINGW AND SKELLYTUNE_MINGW_COMPAT)
        skellytune_configure_mingw_target(skellytune_vst3_stress)
    endif()
endif()
