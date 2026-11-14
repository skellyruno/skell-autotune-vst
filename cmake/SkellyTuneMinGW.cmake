# JUCE 8.0.4 no longer supports MinGW upstream. This opt-in build path keeps
# compatibility changes local to the fetched dependency, leaving DSP untouched.
if(MINGW)
    enable_language(RC)
endif()
function(skellytune_configure_mingw_target target)
    if(NOT MINGW OR NOT TARGET ${target})
        return()
    endif()
    target_link_libraries(${target} PRIVATE
        dbghelp ws2_32 wininet winmm uuid advapi32 imm32 dwmapi d2d1 dxgi
        d3d11 dcomp comctl32 sapi shlwapi version ksguid ksuser)
    target_link_options(${target} PRIVATE -static)
    get_target_property(kind ${target} TYPE)
    if(kind STREQUAL "EXECUTABLE" OR kind STREQUAL "MODULE_LIBRARY" OR kind STREQUAL "SHARED_LIBRARY")
        set(resource_id 1)
        if(NOT kind STREQUAL "EXECUTABLE")
            set(resource_id 2)
        endif()
        set(resource "${CMAKE_CURRENT_BINARY_DIR}/${target}_common_controls.rc")
        file(WRITE "${resource}" "${resource_id} 24 \"${CMAKE_CURRENT_FUNCTION_LIST_DIR}/common-controls.manifest\"\n")
        target_sources(${target} PRIVATE "${resource}")
    endif()
endfunction()
