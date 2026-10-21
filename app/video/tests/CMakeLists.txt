# Drawn for real, on the display's GPU or Mesa's software rasteriser (Xvfb in
# CI); the drawing tests skip without a display.
ratcast_add_test(video_tests
    OWN_MAIN
    SOURCES
        test_video_view.cpp
    LIBS
        ratcast_video
)
