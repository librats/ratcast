ratcast_add_test(call_controller_tests
    OWN_MAIN
    SOURCES
        test_call_controller.cpp
        test_chat_model.cpp
        test_members_model.cpp
    LIBS
        ratcast_call
)
# A path that can go dead (client's tests have it too).
target_include_directories(call_controller_tests PRIVATE "${PROJECT_SOURCE_DIR}/client/tests")
