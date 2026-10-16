# client — what an application does with media, whatever its UI: publishing
# sources as streams and playing the streams of others (later: rooms, calls,
# chat). Glues engine to libratcast; the app and the tools sit on top.

add_library(ratcast_client STATIC
    src/video_player.cpp
    src/video_publisher.cpp
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
