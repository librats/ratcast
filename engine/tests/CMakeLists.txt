ratcast_add_test(engine_tests
    SOURCES
        test_audio_codec.cpp
        test_audio_mixer.cpp
        test_audio_processing.cpp
        test_file_source.cpp
        test_frame_pacer.cpp
        test_pattern.cpp
        test_audio_source.cpp
        test_switchable_output.cpp
        test_video_codec.cpp
        test_video_convert.cpp
        test_video_frame.cpp
    LIBS
        ratcast::engine
    # A GPU's encoder sessions, the sound devices, the desktop, the cameras.
    EXCLUSIVE "Encoders/*:Wasapi*:GraphicsCapture*:CameraTest.*:CoreAudio*:MacScreenCapture*:MacCamera*"
)
# Private headers, for the parts tested on their own, and the tests' own.
target_include_directories(engine_tests PRIVATE
    "${PROJECT_SOURCE_DIR}/engine/src"
    "${CMAKE_CURRENT_SOURCE_DIR}"
)

# For RATCAST_OPUS_HAS_LBRR: whether the decoder can tell recovery from concealment.
if(RATCAST_OPUS_FOUND)
    target_link_libraries(engine_tests PRIVATE ratcast::opus)
endif()

# TS feeds are read and written on POSIX and Windows; elsewhere they are Unavailable.
if(UNIX OR WIN32)
    target_sources(engine_tests PRIVATE test_ts_io.cpp)
endif()
if(WIN32)
    target_link_libraries(engine_tests PRIVATE ws2_32)
endif()

if(RATCAST_PIPEWIRE_FOUND)
    target_sources(engine_tests PRIVATE
        linux/fake_portal.cpp
        linux/test_audio_devices.cpp
        linux/test_audio_node.cpp
        linux/test_camera_capture.cpp
        linux/test_pipewire_audio.cpp
        linux/test_pipewire_capture.cpp
        linux/test_portal.cpp
        linux/test_video_node.cpp
    )
    target_link_libraries(engine_tests PRIVATE ratcast::pipewire ratcast::gio)
elseif(WIN32)
    target_sources(engine_tests PRIVATE
        windows/test_camera_capture.cpp
        windows/test_d3d11.cpp
        windows/test_graphics_capture.cpp
        windows/test_wasapi_audio.cpp
    )
    if(RATCAST_FFMPEG_FOUND)
        target_sources(engine_tests PRIVATE windows/test_gpu_encoding.cpp)
    endif()
    target_link_libraries(engine_tests PRIVATE d3d11 mfplat mfuuid)
elseif(APPLE)
    target_sources(engine_tests PRIVATE
        macos/test_camera_capture.cpp
        macos/test_core_audio.cpp
        macos/test_screen_capture.cpp
    )
endif()
