# Command-line tools: ratcast-send streams a screen, window, file or test
# pattern; ratcast-recv plays it in a window (SDL3, when found) or headless;
# ratcast-room is a member of a room, headless, with a stamped tone for a
# voice; ratcast-tsprobe sends and measures a stamped MPEG-TS feed; ratcast-netsim
# emulates a path between them in user space.
# Thin over client/: what they do beyond parsing options and printing stats
# belongs in the libraries — but for netsim, which is no part of ratcast and
# has its own here.

add_library(ratcast_tool_common STATIC
    common/arguments.cpp
    common/tool.cpp
)
target_include_directories(ratcast_tool_common PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ratcast_tool_common PUBLIC ratcast::client librats::rats)
ratcast_target_defaults(ratcast_tool_common)

add_executable(ratcast_send send/main.cpp)
set_target_properties(ratcast_send PROPERTIES OUTPUT_NAME ratcast-send)
target_link_libraries(ratcast_send PRIVATE ratcast_tool_common)
ratcast_target_defaults(ratcast_send)

add_executable(ratcast_recv recv/main.cpp)
set_target_properties(ratcast_recv PROPERTIES OUTPUT_NAME ratcast-recv)
target_link_libraries(ratcast_recv PRIVATE ratcast_tool_common)
ratcast_target_defaults(ratcast_recv)

add_executable(ratcast_room room/main.cpp)
set_target_properties(ratcast_room PROPERTIES OUTPUT_NAME ratcast-room)
target_link_libraries(ratcast_room PRIVATE ratcast_tool_common)
ratcast_target_defaults(ratcast_room)

add_executable(ratcast_tsprobe probe/main.cpp)
set_target_properties(ratcast_tsprobe PROPERTIES OUTPUT_NAME ratcast-tsprobe)
target_link_libraries(ratcast_tsprobe PRIVATE ratcast_tool_common)
ratcast_target_defaults(ratcast_tsprobe)

# netsim: an emulated path (netem's rules) and the UDP proxy that runs one.
add_library(ratcast_netsim STATIC
    netsim/link.cpp
    netsim/proxy.cpp
)
target_include_directories(ratcast_netsim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ratcast_netsim PUBLIC ratcast::ratcast librats::rats)
if(WIN32)
    target_link_libraries(ratcast_netsim PRIVATE winmm)  # timeBeginPeriod: 1 ms waits
endif()
ratcast_target_defaults(ratcast_netsim)

add_executable(ratcast_netsim_tool netsim/main.cpp)
set_target_properties(ratcast_netsim_tool PROPERTIES OUTPUT_NAME ratcast-netsim)
target_link_libraries(ratcast_netsim_tool PRIVATE ratcast_netsim ratcast_tool_common)
ratcast_target_defaults(ratcast_netsim_tool)

find_package(SDL3 QUIET CONFIG)
if(SDL3_FOUND)
    target_sources(ratcast_recv PRIVATE recv/video_window.cpp)
    target_link_libraries(ratcast_recv PRIVATE SDL3::SDL3)
    target_compile_definitions(ratcast_recv PRIVATE RATCAST_TOOLS_WINDOW)
    message(STATUS "SDL3 ${SDL3_VERSION}: ratcast-recv has a window")
else()
    message(STATUS "SDL3 not found: ratcast-recv is headless only")
endif()

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
