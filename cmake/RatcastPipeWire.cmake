# Screen capture and sound on Linux: PipeWire carries the pictures and the
# sound, and GIO talks to the xdg-desktop-portal that hands screen streams out
# (the only way to capture under Wayland, and the sanctioned one under X11).
# Found through pkg-config; 0.3.50 for pw_stream_get_time_n().
#
# RATCAST_WITH_PIPEWIRE: AUTO (use it if found), ON (require it), OFF (never).
# Defines the imported targets ratcast::pipewire and ratcast::gio when used.

set(RATCAST_WITH_PIPEWIRE AUTO CACHE STRING "Build Linux screen capture and sound (PipeWire + portal): AUTO, ON or OFF")
set_property(CACHE RATCAST_WITH_PIPEWIRE PROPERTY STRINGS AUTO ON OFF)

set(RATCAST_PIPEWIRE_FOUND FALSE)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND NOT RATCAST_WITH_PIPEWIRE STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RATCAST_LIBPIPEWIRE QUIET IMPORTED_TARGET GLOBAL libpipewire-0.3>=0.3.50)
        pkg_check_modules(RATCAST_GIO QUIET IMPORTED_TARGET GLOBAL gio-2.0>=2.68 gio-unix-2.0>=2.68)
    endif()
    if(RATCAST_LIBPIPEWIRE_FOUND AND RATCAST_GIO_FOUND)
        set(RATCAST_PIPEWIRE_FOUND TRUE)
        add_library(ratcast::pipewire ALIAS PkgConfig::RATCAST_LIBPIPEWIRE)
        add_library(ratcast::gio ALIAS PkgConfig::RATCAST_GIO)
        message(STATUS "PipeWire: ${RATCAST_LIBPIPEWIRE_VERSION}, GIO: ${RATCAST_GIO_gio-2.0_VERSION}")
    elseif(RATCAST_WITH_PIPEWIRE STREQUAL "ON")
        message(FATAL_ERROR "RATCAST_WITH_PIPEWIRE=ON but libpipewire-0.3 or gio-2.0 was not found")
    else()
        message(WARNING "PipeWire or GIO not found: the engine is built without screen capture or sound devices")
    endif()
endif()
