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

    # libc++ before 19 (the NDK r27 that Qt for Android is built with) hides
    # std::jthread and std::stop_token behind its experimental switch; they
    # are header-only, so the switch is all they need.
    if(ANDROID AND CMAKE_CXX_COMPILER_ID STREQUAL "Clang"
       AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 19)
        target_compile_definitions(${target} PRIVATE _LIBCPP_ENABLE_EXPERIMENTAL)
    endif()

    # Every program of ours runs with UTF-8 for its code page, as our strings
    # are: MSVC's linker embeds the manifest that says so.
    get_target_property(type ${target} TYPE)
    if(MSVC AND type STREQUAL "EXECUTABLE")
        target_sources(${target} PRIVATE "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/utf8.manifest")
    endif()

    # On macOS a program outside a bundle carries its Info.plist in itself:
    # the usage descriptions the system asks the user's leave with.
    if(APPLE AND type STREQUAL "EXECUTABLE")
        get_target_property(bundle ${target} MACOSX_BUNDLE)
        if(NOT bundle)
            set(RATCAST_PROGRAM ${target})
            set(plist "${CMAKE_CURRENT_BINARY_DIR}/${target}.plist")
            configure_file("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/program.plist.in" "${plist}" @ONLY)
            target_link_options(${target} PRIVATE "LINKER:-sectcreate,__TEXT,__info_plist,${plist}")
            set_property(TARGET ${target} APPEND PROPERTY LINK_DEPENDS "${plist}")
        endif()
    endif()
endfunction()
