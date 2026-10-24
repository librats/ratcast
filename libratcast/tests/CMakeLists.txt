ratcast_add_test(libratcast_tests
    SOURCES
        test_buffer.cpp
        test_common_delay.cpp
        test_capi.cpp
        test_control_codec.cpp
        test_frame_codec.cpp
        test_frame_tracker.cpp
        test_delivery_buffer.cpp
        test_delivery_thread.cpp
        test_fastest_transit.cpp
        test_jitter_buffer.cpp
        test_media_service.cpp
        test_media_session.cpp
        test_peer_clock.cpp
        test_presence.cpp
        test_rate_controller.cpp
        test_sframe.cpp
        test_shared_uplink.cpp
        test_result.cpp
        test_stream_description.cpp
        test_time.cpp
        test_version.cpp
        test_wire_bytes.cpp
    LIBS
        ratcast::ratcast
)

# White-box tests reach the private headers (wire/…) as the library does.
target_include_directories(libratcast_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../src")

# The C API from C: the header compiles as C11, and a feed crosses two nodes.
add_executable(libratcast_c_test test_capi_c.c)
target_link_libraries(libratcast_c_test PRIVATE ratcast::ratcast)
set_target_properties(libratcast_c_test PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
    # <stdatomic.h> is behind a switch of its own there.
    target_compile_options(libratcast_c_test PRIVATE /experimental:c11atomics)
endif()
ratcast_target_defaults(libratcast_c_test)
add_test(NAME CApiFromC COMMAND libratcast_c_test)
