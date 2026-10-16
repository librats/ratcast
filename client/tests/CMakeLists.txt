ratcast_add_test(client_tests
    SOURCES
        test_over_librats.cpp
        test_video_pipeline.cpp
    LIBS
        ratcast::client
)
