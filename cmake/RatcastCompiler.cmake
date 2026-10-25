# Per-target build settings for ratcast's own code.
#
# Applied target by target rather than with add_compile_options(), so they never
# leak into librats or any other third-party code built in the same tree.

function(ratcast_target_defaults target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8 /Zc:__cplusplus)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(WIN32)
        # librats' headers pull in windows.h; keep its min/max macros and the
        # rarely used parts of the API out of our translation units.
        target_compile_definitions(${target} PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()

    # Every program of ours runs with UTF-8 for its code page, as our strings
    # are: MSVC's linker embeds the manifest that says so.
    get_target_property(type ${target} TYPE)
    if(MSVC AND type STREQUAL "EXECUTABLE")
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.manifest")
    endif()
endfunction()
