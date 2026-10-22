# FFmpeg for the engine's codecs (libavcodec, libavutil) and file source
# (libavformat, libswscale), found through pkg-config: system packages on
# Linux, Homebrew on macOS, vcpkg everywhere.
#
# RATCAST_WITH_FFMPEG: AUTO (use it if found), ON (require it), OFF (never).
# Defines the imported target ratcast::ffmpeg when FFmpeg is used.

set(RATCAST_WITH_FFMPEG AUTO CACHE STRING "Build the engine's FFmpeg codecs: AUTO, ON or OFF")
set_property(CACHE RATCAST_WITH_FFMPEG PROPERTY STRINGS AUTO ON OFF)

set(RATCAST_FFMPEG_FOUND FALSE)
if(NOT RATCAST_WITH_FFMPEG STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        # FFmpeg 6.0 or newer.
        pkg_check_modules(RATCAST_FFMPEG QUIET IMPORTED_TARGET GLOBAL
            libavcodec>=60 libavformat>=60 libavutil>=58 libswscale>=7)
    endif()
    if(RATCAST_FFMPEG_FOUND)
        add_library(ratcast::ffmpeg ALIAS PkgConfig::RATCAST_FFMPEG)
        ratcast_pkg_config_per_config(PkgConfig::RATCAST_FFMPEG)
        message(STATUS "FFmpeg: libavcodec ${RATCAST_FFMPEG_libavcodec_VERSION}")
    elseif(RATCAST_WITH_FFMPEG STREQUAL "ON")
        message(FATAL_ERROR "RATCAST_WITH_FFMPEG=ON but FFmpeg 6+ (libavcodec, libavformat, "
                            "libavutil, libswscale) was not found")
    else()
        message(WARNING "FFmpeg 6+ not found: the engine is built without codecs or file source")
    endif()
endif()
