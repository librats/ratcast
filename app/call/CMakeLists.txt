# Ratcast.Call: CallController, a call as QML sees it — share a screen or join
# one, and leave — over client's Call. A static QML module: the app links its
# plugin.

qt_add_library(ratcast_call STATIC
    call_controller.cpp
    call_controller.h
)
qt_add_qml_module(ratcast_call
    URI     Ratcast.Call
    VERSION 1.0
    OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/qml/Ratcast/Call"
    DEPENDENCIES Ratcast.Video
)

target_include_directories(ratcast_call PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ratcast_call PUBLIC Qt6::Quick ratcast::client ratcast_video)
ratcast_target_defaults(ratcast_call)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
