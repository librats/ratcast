# WebRTC's AudioProcessing for the engine's voice processing — echo
# cancellation (AEC3), noise suppression, gain control — found through
# pkg-config as the freedesktop.org build ships it (BSD; Debian/Ubuntu:
# libwebrtc-audio-processing-dev).
#
# RATCAST_WITH_WEBRTC_APM: AUTO (use it if found), ON (require it), OFF (never).
# Defines the imported target ratcast::webrtc_apm when it is used.

set(RATCAST_WITH_WEBRTC_APM AUTO CACHE STRING
    "Build the engine's voice processing on WebRTC AudioProcessing: AUTO, ON or OFF")
set_property(CACHE RATCAST_WITH_WEBRTC_APM PROPERTY STRINGS AUTO ON OFF)

set(RATCAST_WEBRTC_APM_FOUND FALSE)
if(NOT RATCAST_WITH_WEBRTC_APM STREQUAL "OFF")
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(RATCAST_LIBWEBRTC_APM QUIET IMPORTED_TARGET GLOBAL
            webrtc-audio-processing-1>=1.0)
    endif()
    if(RATCAST_LIBWEBRTC_APM_FOUND)
        set(RATCAST_WEBRTC_APM_FOUND TRUE)
        add_library(ratcast::webrtc_apm ALIAS PkgConfig::RATCAST_LIBWEBRTC_APM)
        message(STATUS "WebRTC AudioProcessing: ${RATCAST_LIBWEBRTC_APM_VERSION}")
    elseif(RATCAST_WITH_WEBRTC_APM STREQUAL "ON")
        message(FATAL_ERROR "RATCAST_WITH_WEBRTC_APM=ON but webrtc-audio-processing-1 was not found")
    else()
        message(WARNING "webrtc-audio-processing-1 not found: the engine is built without "
                        "echo cancellation and noise suppression")
    endif()
endif()
