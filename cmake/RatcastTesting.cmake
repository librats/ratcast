# GoogleTest, shared by every module's tests/ directory.
#
# Same version and source as librats uses, so when librats' own tests are also
# enabled both sides resolve to the one fetched copy.

find_package(GTest QUIET)

if(NOT GTest_FOUND)
    include(FetchContent)
    FetchContent_Declare(
        googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)
endif()

include(GoogleTest)

# ratcast_add_test(<name> SOURCES <files...> LIBS <targets...>)
function(ratcast_add_test name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main)
    ratcast_target_defaults(${name})
    gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
endfunction()
