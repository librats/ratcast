# client — what an application does with media, whatever its UI: a call
# (its node, invites, dialing), publishing sources as streams and playing the
# streams of others (later: rooms, chat). Glues engine to libratcast; the app
# and the tools sit on top.

add_library(ratcast_client STATIC
    src/audio_player.cpp
    src/audio_publisher.cpp
    src/call.cpp
    src/call_monitor.cpp
    src/camera.cpp
    src/cpu_meter.cpp
    src/data_player.cpp
    src/data_publisher.cpp
    src/log_file.cpp
    src/playback_meter.cpp
    src/playout_clock.cpp
    src/playout_schedule.cpp
    src/rats_url.cpp
    src/screen_share.cpp
    src/ts_probe.cpp
    src/video_player.cpp
    src/video_publisher.cpp
    src/voice.cpp
    src/voice_meter.cpp
)
add_library(ratcast::client ALIAS ratcast_client)

target_include_directories(ratcast_client
    PUBLIC  "${CMAKE_CURRENT_SOURCE_DIR}/include"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)
target_link_libraries(ratcast_client PUBLIC ratcast::engine ratcast::ratcast)
set_target_properties(ratcast_client PROPERTIES POSITION_INDEPENDENT_CODE ON)
ratcast_target_defaults(ratcast_client)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
