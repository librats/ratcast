# Command-line tools: ratcast-send streams a screen, window, file or test
# pattern; ratcast-recv plays it in a window (SDL3, when found) or headless;
# ratcast-tsprobe sends and measures a stamped MPEG-TS feed.
# Thin over client/: what they do beyond parsing options and printing stats
# belongs in the libraries.

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

add_executable(ratcast_tsprobe probe/main.cpp)
set_target_properties(ratcast_tsprobe PROPERTIES OUTPUT_NAME ratcast-tsprobe)
target_link_libraries(ratcast_tsprobe PRIVATE ratcast_tool_common)
ratcast_target_defaults(ratcast_tsprobe)

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
