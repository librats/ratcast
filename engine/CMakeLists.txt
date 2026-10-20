# engine — media I/O: pictures in and out of sources, encoders and decoders.
# Knows nothing of the network: what it produces is what MediaSession sends.

include(RatcastFFmpeg)
include(RatcastOpus)
include(RatcastPipeWire)

add_library(ratcast_engine STATIC
    src/audio/audio_codec.cpp
    src/audio/audio_frame.cpp
    src/pattern/pattern.cpp
    src/pattern/pattern_source.cpp
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

if(RATCAST_PIPEWIRE_FOUND)
    target_sources(ratcast_engine PRIVATE
        src/linux/pipewire_capture.cpp
        src/linux/portal.cpp
        src/linux/screen_capture.cpp
    )
    target_link_libraries(ratcast_engine PRIVATE ratcast::pipewire ratcast::gio)
else()
    target_sources(ratcast_engine PRIVATE src/capture/screen_capture_unavailable.cpp)
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
