# server — a public node calls behind NATs meet through and are carried by
# (ratcast-server). Stands on libratcast and librats alone: no codecs, no
# capture, no Qt, so it builds and runs on a bare host.

add_library(ratcast_server STATIC
    src/server.cpp
)
add_library(ratcast::server ALIAS ratcast_server)

target_include_directories(ratcast_server PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(ratcast_server PUBLIC ratcast::ratcast librats::rats)
ratcast_target_defaults(ratcast_server)

add_executable(ratcast_server_tool src/main.cpp)
set_target_properties(ratcast_server_tool PROPERTIES OUTPUT_NAME ratcast-server)
target_link_libraries(ratcast_server_tool PRIVATE ratcast::server)
ratcast_target_defaults(ratcast_server_tool)
ratcast_install_programs(ratcast_server_tool)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
