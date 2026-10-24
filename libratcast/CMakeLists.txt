# libratcast — media transport over librats: encoded frames in, encoded frames
# out. Knows nothing of capture, codecs or UI, so it stands on its own as a
# library (the librist / SRT counterpart) and the SFU links it without the rest.
# Its C API (ratcast/ratcast.h) stands on librats' C ABI.

add_library(ratcast
    src/capi/ratcast.cpp
    src/core/buffer.cpp
    src/core/result.cpp
    src/core/time.cpp
    src/core/version.cpp
    src/session/common_delay.cpp
    src/session/media_service.cpp
    src/session/media_session.cpp
    src/session/peer_clock.cpp
    src/session/presence.cpp
    src/session/rate_controller.cpp
    src/session/sframe.cpp
    src/session/shared_uplink.cpp
    src/stream/delivery_buffer.cpp
    src/stream/delivery_thread.cpp
    src/stream/fastest_transit.cpp
    src/stream/frame_tracker.cpp
    src/stream/jitter_buffer.cpp
    src/stream/stream_description.cpp
    src/wire/control_codec.cpp
    src/wire/frame_codec.cpp
)
add_library(ratcast::ratcast ALIAS ratcast)

include(GenerateExportHeader)
generate_export_header(ratcast
    BASE_NAME        RATCAST
    EXPORT_FILE_NAME "${CMAKE_CURRENT_BINARY_DIR}/include/ratcast/export.h"
)

configure_file(
    include/ratcast/version.h.in
    "${CMAKE_CURRENT_BINARY_DIR}/include/ratcast/version.h"
    @ONLY
)

# One include root per side: public headers are spelled "ratcast/<name>.h"
# everywhere, generated ones included.
target_include_directories(ratcast
    PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/include"
        "${CMAKE_CURRENT_BINARY_DIR}/include"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(ratcast PUBLIC RATCAST_STATIC_DEFINE)
endif()

target_link_libraries(ratcast PUBLIC librats::rats)
set_target_properties(ratcast PROPERTIES POSITION_INDEPENDENT_CODE ON)
ratcast_target_defaults(ratcast)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
