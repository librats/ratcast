# Opus for the engine's audio codec, found through pkg-config: libopus is BSD,
# and FFmpeg's own Opus encoder is experimental, so the engine uses libopus
# directly.
#
# RATCAST_WITH_OPUS: AUTO (use it if found), ON (require it), OFF (never).
# Defines the imported target ratcast::opus when Opus is used.

set(RATCAST_WITH_OPUS AUTO CACHE STRING "Build the engine's Opus audio codec: AUTO, ON or OFF")
set_property(CACHE RATCAST_WITH_OPUS PROPERTY STRINGS AUTO ON OFF)

set(RATCAST_OPUS_FOUND FALSE)
if(NOT RATCAST_WITH_OPUS STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RATCAST_LIBOPUS QUIET IMPORTED_TARGET GLOBAL opus>=1.3)
    endif()
    if(RATCAST_LIBOPUS_FOUND)
        set(RATCAST_OPUS_FOUND TRUE)
        add_library(ratcast::opus ALIAS PkgConfig::RATCAST_LIBOPUS)
        ratcast_pkg_config_per_config(PkgConfig::RATCAST_LIBOPUS)
        # 1.5 tells whether a packet carries in-band FEC (opus_packet_has_lbrr),
        # which is what the decoder's stats tell recovery from concealment by.
        if(RATCAST_LIBOPUS_VERSION VERSION_GREATER_EQUAL 1.5)
            target_compile_definitions(PkgConfig::RATCAST_LIBOPUS INTERFACE RATCAST_OPUS_HAS_LBRR)
        endif()
        message(STATUS "Opus: ${RATCAST_LIBOPUS_VERSION}")
    elseif(RATCAST_WITH_OPUS STREQUAL "ON")
        message(FATAL_ERROR "RATCAST_WITH_OPUS=ON but libopus 1.3+ was not found")
    else()
        message(WARNING "libopus 1.3+ not found: the engine is built without an audio codec")
    endif()
endif()
