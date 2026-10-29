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

# An Android build's tests run on a device through adb (cmake/android-run.cmake):
# ctest and GoogleTest's discovery start every test program through it, which
# pushes the shared libraries they need beside them — libc++ and vcpkg's
# (FFmpeg's, arm64-android-release).
if(ANDROID)
    find_program(RATCAST_ADB adb HINTS "$ENV{ANDROID_SDK_ROOT}/platform-tools"
        "$ENV{ANDROID_HOME}/platform-tools" REQUIRED)
    set(android_libs "${CMAKE_SYSROOT}/usr/lib/${CMAKE_LIBRARY_ARCHITECTURE}/libc++_shared.so")
    if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
        file(GLOB vcpkg_libs "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib/*.so")
        list(APPEND android_libs ${vcpkg_libs})
    endif()
    list(JOIN android_libs "|" android_libs)
    set(CMAKE_CROSSCOMPILING_EMULATOR
        "${CMAKE_COMMAND}" "-DADB=${RATCAST_ADB}" "-DSTRIP=${CMAKE_STRIP}"
        "-DLIBS=${android_libs}" -P "${PROJECT_SOURCE_DIR}/cmake/android-run.cmake" --)
endif()

# ratcast_add_test(<name> SOURCES <files...> LIBS <targets...> [OWN_MAIN]
#                  [EXCLUSIVE <gtest filter>])
# OWN_MAIN: the sources have main() (a Qt test makes its application first).
# EXCLUSIVE: the tests that use what the machine has one of — its sound
# devices, its desktop, its GPU's encoder sessions — which run one at a time,
# never beside each other, whichever executable they are in.
function(ratcast_add_test name)
    cmake_parse_arguments(ARG "OWN_MAIN" "EXCLUSIVE" "SOURCES;LIBS" ${ARGN})
    add_executable(${name} ${ARG_SOURCES})
    if(ARG_OWN_MAIN)
        target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest)
    else()
        target_link_libraries(${name} PRIVATE ${ARG_LIBS} GTest::gtest_main)
    endif()
    ratcast_target_defaults(${name})
    if(ARG_EXCLUSIVE)
        gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST TEST_FILTER "-${ARG_EXCLUSIVE}")
        gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST TEST_FILTER "${ARG_EXCLUSIVE}"
            PROPERTIES RESOURCE_LOCK ratcast_machine)
    else()
        gtest_discover_tests(${name} DISCOVERY_MODE PRE_TEST)
    endif()
endfunction()
