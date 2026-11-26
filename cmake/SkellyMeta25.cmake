# Exact MetaTune 2.5 Playground source pin; research only, AGPL-3.0 obligations.
include(FetchContent)
FetchContent_Declare(SkellyMeta25Playground
    GIT_REPOSITORY https://github.com/skellyruno/skelltune-playground.git
    GIT_TAG 9cfbaa6c2e5cd8025788b1035d4096355ae02eb2
    SOURCE_SUBDIR _dependency_only)
FetchContent_Declare(SkellyMeta25Native
    GIT_REPOSITORY https://github.com/skellyruno/MetaTuneInspiredvst.git
    GIT_TAG 73d759307ce9536e522b2cf78dc1da943afc8f3d
    SOURCE_SUBDIR _dependency_only)
FetchContent_MakeAvailable(SkellyMeta25Playground SkellyMeta25Native)
add_library(skellytune_meta25 STATIC)
foreach(src IN ITEMS TunerChain.cpp Pitch/YinDetector.cpp Pitch/TargetSelector.cpp
        Pitch/CorrectionEngine.cpp Pitch/PitchShifter.cpp Doubler.cpp FormantAccent.cpp)
    target_sources(skellytune_meta25 PRIVATE "${skellymeta25native_SOURCE_DIR}/Source/LyraDSP/${src}")
endforeach()
target_include_directories(skellytune_meta25 PUBLIC
    "${skellymeta25native_SOURCE_DIR}/Source" "${skellymeta25playground_SOURCE_DIR}/dsp")
target_compile_features(skellytune_meta25 PUBLIC cxx_std_20)
target_compile_definitions(skellytune_meta25 PUBLIC SKELLTUNE_AUTO_2P5=1
    SKELLTUNE_LIVE_FIXED_LOW_LATENCY=1 SKELLTUNE_LOW_VOICE_LAB=1 SKELLTUNE_LAB_FAST_2P5=1)
