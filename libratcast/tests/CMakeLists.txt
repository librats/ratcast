ratcast_add_test(libratcast_tests
    SOURCES
        test_buffer.cpp
        test_control_codec.cpp
        test_frame_codec.cpp
        test_frame_tracker.cpp
        test_jitter_buffer.cpp
        test_media_service.cpp
        test_media_session.cpp
        test_rate_controller.cpp
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
