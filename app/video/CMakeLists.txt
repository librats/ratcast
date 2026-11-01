# Ratcast.Video: VideoView, the Qt Quick item that draws a player's pictures —
# their planes as textures, turned into colour by a shader (shaders/), or on
# Android a picture in GPU memory where it is (external.frag). A static QML
# module: the app links its plugin.

qt_add_library(ratcast_video STATIC
    video_node.cpp
    video_node.h
    video_view.cpp
    video_view.h
)
qt_add_qml_module(ratcast_video
    URI     Ratcast.Video
    VERSION 1.0
    OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/qml/Ratcast/Video"
)
qt_add_shaders(ratcast_video "ratcast_video_shaders"
    BATCHABLE
    PREFIX "/ratcast/video"
    FILES
        shaders/video.vert
        shaders/video.frag
        # OpenGL ES's own, with the external sampler only it has.
        "shaders/external.frag@glsl,100es,shaders/external_gles.frag"
)

target_include_directories(ratcast_video PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ratcast_video
    PUBLIC  Qt6::Quick ratcast::engine
    PRIVATE Qt6::GuiPrivate
)
if(ANDROID)  # pictures in GPU memory drawn where they are
    target_link_libraries(ratcast_video PRIVATE EGL GLESv2)
endif()
ratcast_target_defaults(ratcast_video)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
