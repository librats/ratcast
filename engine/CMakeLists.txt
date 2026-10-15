# engine — media I/O: pictures in and out of sources, encoders and decoders.
# Knows nothing of the network: what it produces is what MediaSession sends.

include(RatcastFFmpeg)

add_library(ratcast_engine STATIC
    src/pattern/pattern.cpp
    src/pattern/pattern_source.cpp
    src/video/video_codec.cpp
    src/video/video_frame.cpp
)
add_library(ratcast::engine ALIAS ratcast_engine)

if(RATCAST_FFMPEG_FOUND)
    target_sources(ratcast_engine PRIVATE
        src/ffmpeg/av.cpp
        src/ffmpeg/video_decoder.cpp
        src/ffmpeg/video_encoder.cpp
    )
    target_link_libraries(ratcast_engine PRIVATE ratcast::ffmpeg)
else()
    target_sources(ratcast_engine PRIVATE src/codecs_unavailable.cpp)
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
