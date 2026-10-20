ratcast_add_test(engine_tests
    SOURCES
        test_audio_codec.cpp
        test_file_source.cpp
        test_frame_pacer.cpp
        test_pattern.cpp
        test_video_codec.cpp
        test_video_convert.cpp
        test_video_frame.cpp
    LIBS
        ratcast::engine
)
# Private headers, for the parts tested on their own.
target_include_directories(engine_tests PRIVATE "${PROJECT_SOURCE_DIR}/engine/src")

# For RATCAST_OPUS_HAS_LBRR: whether the decoder can tell recovery from concealment.
if(RATCAST_OPUS_FOUND)
    target_link_libraries(engine_tests PRIVATE ratcast::opus)
endif()

if(RATCAST_PIPEWIRE_FOUND)
    target_sources(engine_tests PRIVATE
        linux/fake_portal.cpp
        linux/test_pipewire_capture.cpp
        linux/test_portal.cpp
        linux/test_video_node.cpp
    )
    target_link_libraries(engine_tests PRIVATE ratcast::pipewire ratcast::gio)
endif()
