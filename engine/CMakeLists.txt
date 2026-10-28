# engine — media I/O: pictures in and out of sources, encoders and decoders.
# Knows nothing of the network: what it produces is what MediaSession sends.

include(RatcastFFmpeg)
include(RatcastOpus)
include(RatcastPipeWire)
include(RatcastWebRtcApm)

add_library(ratcast_engine STATIC
    src/audio/audio_codec.cpp
    src/audio/audio_device.cpp
    src/capture/camera_capture.cpp
    src/audio/audio_frame.cpp
    src/audio/audio_mixer.cpp
    src/audio/switchable_output.cpp
    src/pattern/pattern.cpp
    src/pattern/pattern_source.cpp
    src/pattern/tone_source.cpp
    src/pattern/virtual_output.cpp
    src/video/video_codec.cpp
    src/video/video_convert.cpp
    src/video/video_frame.cpp
)
add_library(ratcast::engine ALIAS ratcast_engine)

# Pixel loops written for the vectoriser, which GCC runs in full only at -O3:
# twice as fast as -O2 for BGRA into NV12.
if(NOT MSVC)
    set_source_files_properties(src/video/video_convert.cpp
        PROPERTIES COMPILE_OPTIONS "$<$<NOT:$<CONFIG:Debug>>:-O3>")
endif()

if(RATCAST_FFMPEG_FOUND)
    target_sources(ratcast_engine PRIVATE
        src/ffmpeg/av.cpp
        src/ffmpeg/file_source.cpp
        src/ffmpeg/video_decoder.cpp
        src/ffmpeg/video_encoder.cpp
    )
    # Pictures in GPU memory into encoders: Direct3D 11's on Windows.
    if(WIN32)
        target_sources(ratcast_engine PRIVATE src/ffmpeg/hw_frames_d3d11.cpp)
    else()
        target_sources(ratcast_engine PRIVATE src/ffmpeg/hw_frames_none.cpp)
    endif()
    # VideoToolbox's encoders, in the encoder table, on the system's own API.
    if(APPLE)
        target_sources(ratcast_engine PRIVATE src/macos/videotoolbox_encoder.cpp)
        target_link_libraries(ratcast_engine PRIVATE
            "-framework CoreFoundation" "-framework CoreMedia" "-framework CoreVideo"
            "-framework VideoToolbox")
    endif()
    target_link_libraries(ratcast_engine PRIVATE ratcast::ffmpeg)
else()
    target_sources(ratcast_engine PRIVATE src/ffmpeg_unavailable.cpp)
endif()

if(RATCAST_OPUS_FOUND)
    target_sources(ratcast_engine PRIVATE
        src/opus/audio_decoder.cpp
        src/opus/audio_encoder.cpp
    )
    target_link_libraries(ratcast_engine PRIVATE ratcast::opus)
else()
    target_sources(ratcast_engine PRIVATE src/opus_unavailable.cpp)
endif()

if(RATCAST_WEBRTC_APM_FOUND)
    target_sources(ratcast_engine PRIVATE src/webrtc/audio_processor.cpp)
    target_link_libraries(ratcast_engine PRIVATE ratcast::webrtc_apm)
else()
    target_sources(ratcast_engine PRIVATE src/webrtc_apm_unavailable.cpp)
endif()

if(RATCAST_PIPEWIRE_FOUND)
    target_sources(ratcast_engine PRIVATE
        src/linux/audio_device.cpp
        src/linux/camera_capture.cpp
        src/linux/pipewire_applications.cpp
        src/linux/pipewire_audio.cpp
        src/linux/pipewire_capture.cpp
        src/linux/pipewire_registry.cpp
        src/linux/pipewire_stream.cpp
        src/linux/portal.cpp
        src/linux/screen_awake.cpp
        src/linux/screen_capture.cpp
    )
    target_link_libraries(ratcast_engine PRIVATE ratcast::pipewire ratcast::gio)
elseif(WIN32)
    target_sources(ratcast_engine PRIVATE
        src/windows/audio_device.cpp
        src/windows/camera_capture.cpp
        src/windows/d3d11.cpp
        src/windows/graphics_capture.cpp
        src/windows/nv12_converter.cpp
        src/windows/screen_awake.cpp
        src/windows/screen_capture.cpp
        src/windows/wasapi_audio.cpp
    )
    target_link_libraries(ratcast_engine PRIVATE
        avrt d3d11 dwmapi dxgi mf mfplat mfreadwrite mfuuid mmdevapi ole32 windowsapp)
elseif(APPLE)
    target_sources(ratcast_engine PRIVATE
        src/macos/audio_device.cpp
        src/macos/camera_capture.mm
        src/macos/core_audio.mm
        src/macos/pixel_buffer.cpp
        src/macos/screen_awake.cpp
        src/macos/screen_capture.mm
    )
    # Objective-C's objects counted by the compiler (ARC), as Apple's own code is.
    set_source_files_properties(
        src/macos/camera_capture.mm src/macos/core_audio.mm src/macos/screen_capture.mm
        PROPERTIES COMPILE_OPTIONS -fobjc-arc)
    target_link_libraries(ratcast_engine PRIVATE
        "-framework AppKit" "-framework AudioToolbox" "-framework AVFoundation"
        "-framework CoreAudio"
        "-framework CoreFoundation" "-framework CoreGraphics" "-framework CoreMedia"
        "-framework CoreVideo" "-framework Foundation" "-framework IOKit"
        "-framework ScreenCaptureKit")
else()
    target_sources(ratcast_engine PRIVATE
        src/audio/audio_device_unavailable.cpp
        src/capture/camera_capture_unavailable.cpp
        src/capture/screen_capture_unavailable.cpp
        src/screen_awake_unavailable.cpp
    )
endif()

# Real-time waits: Windows' own timer, as Sleep() steps in 15.6 ms.
if(WIN32)
    target_sources(ratcast_engine PRIVATE src/windows/sleeper.cpp)
else()
    target_sources(ratcast_engine PRIVATE src/sleeper.cpp)
endif()

# Threads that keep time: macOS lets other threads' timers end late.
if(APPLE)
    target_sources(ratcast_engine PRIVATE src/macos/real_time.cpp)
else()
    target_sources(ratcast_engine PRIVATE src/real_time.cpp)
endif()

if(UNIX)
    target_sources(ratcast_engine PRIVATE src/posix/ts_io.cpp)
elseif(WIN32)
    target_sources(ratcast_engine PRIVATE src/windows/ts_io.cpp)
    target_link_libraries(ratcast_engine PRIVATE ws2_32)
else()
    target_sources(ratcast_engine PRIVATE src/ts/ts_io_unavailable.cpp)
endif()

target_include_directories(ratcast_engine
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ratcast_engine PUBLIC ratcast::ratcast)
set_target_properties(ratcast_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
ratcast_target_defaults(ratcast_engine)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
