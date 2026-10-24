# Ratcast.Call: CallController, a room as QML sees it — make one or join one,
# talk, share and watch — over client's Call, and the members it lists. A
# static QML module: the app links its plugin.

qt_add_library(ratcast_call STATIC
    call_controller.cpp
    call_controller.h
    members_model.cpp
    members_model.h
)
qt_add_qml_module(ratcast_call
    URI     Ratcast.Call
    VERSION 1.0
    OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/qml/Ratcast/Call"
    DEPENDENCIES Ratcast.Video
)

target_include_directories(ratcast_call PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ratcast_call PUBLIC Qt6::Quick ratcast::client ratcast_video)
target_compile_definitions(ratcast_call PRIVATE
    RATCAST_DEFAULT_SERVER="${RATCAST_DEFAULT_SERVER}")
ratcast_target_defaults(ratcast_call)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
