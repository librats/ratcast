# What `cmake --install` lays out: the app and the command-line tools, and on
# Windows everything they run on, so the folder works on a machine with none of
# it — the release zip is this folder.
#
# On Windows the layout is flat (CMAKE_INSTALL_BINDIR "."): the programs, the
# DLLs of the libraries they link (FFmpeg and its codecs, Opus, WebRTC's
# AudioProcessing, SDL3) and the compiler's runtime side by side, Qt's plugins
# and QML modules in plugins/ and qml/ (windeployqt, through Qt's deploy script
# in app/). What a GPU's driver brings — NVENC, AMF, Quick Sync's runtime — is
# the machine's own: FFmpeg loads it when it opens such an encoder.

if(WIN32 AND NOT DEFINED CMAKE_INSTALL_BINDIR)
    set(CMAKE_INSTALL_BINDIR "." CACHE PATH "Programs and their DLLs (Windows: the top)")
endif()
include(GNUInstallDirs)

# ratcast_install_programs(<target>...)
# Installs the executables; on Windows their runtime dependencies go with them
# (ratcast_install_runtime()).
function(ratcast_install_programs)
    if(WIN32)
        install(TARGETS ${ARGN}
            RUNTIME_DEPENDENCY_SET ratcast_runtime
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
        set_property(GLOBAL PROPERTY RATCAST_RUNTIME_DEPENDENCIES ON)
    else()
        install(TARGETS ${ARGN}
            RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
            BUNDLE DESTINATION .)
    endif()
endfunction()

# ratcast_install_runtime()
# After every ratcast_install_programs(): the DLLs those programs import, but
# Windows' own and Qt's (windeployqt brings those, with their plugins), and the
# compiler's runtime.
function(ratcast_install_runtime)
    get_property(used GLOBAL PROPERTY RATCAST_RUNTIME_DEPENDENCIES)
    if(NOT used)
        return()
    endif()

    # A DLL is looked for beside the program in the build tree first (vcpkg
    # copies the ones it links there, of the configuration built), then in the
    # bin/ of each prefix searched: vcpkg's installed tree, or one of one's own.
    set(directories)
    foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
        if(IS_DIRECTORY "${prefix}/bin")
            list(APPEND directories "${prefix}/bin")
        endif()
    endforeach()

    # DLL names are matched in lowercase; paths as they are.
    install(RUNTIME_DEPENDENCY_SET ratcast_runtime
        DESTINATION "${CMAKE_INSTALL_BINDIR}"
        PRE_EXCLUDE_REGEXES "^api-ms-" "^ext-ms-" "^qt6"
        POST_EXCLUDE_REGEXES "^[A-Za-z]:[/\\\\][Ww][Ii][Nn][Dd][Oo][Ww][Ss][/\\\\]"
        DIRECTORIES ${directories})

    # MSVC's C++ runtime beside the programs; the universal CRT is Windows'
    # own since Windows 10.
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_DESTINATION "${CMAKE_INSTALL_BINDIR}")
    set(CMAKE_INSTALL_UCRT_LIBRARIES OFF)
    include(InstallRequiredSystemLibraries)
endfunction()
