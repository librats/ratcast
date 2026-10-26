cmake_minimum_required(VERSION 3.22)

# The version is the release tag's: release.yml passes it (v1.2.3, a
# pre-release's -rc1 not part of it); any other build takes the last v* tag git
# sees, or 0.0.0 without one.
if(NOT RATCAST_VERSION)
    execute_process(
        COMMAND git describe --tags --abbrev=0 --match "v[0-9]*"
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        OUTPUT_VARIABLE RATCAST_VERSION
        OUTPUT_STRIP_TRAILING_WHITESPACE
        RESULT_VARIABLE git_result
        ERROR_QUIET)
    if(NOT git_result EQUAL 0)
        set(RATCAST_VERSION 0.0.0)
    endif()
endif()
if(NOT RATCAST_VERSION MATCHES "^v?([0-9]+\\.[0-9]+\\.[0-9]+)(-.*)?$")
    message(FATAL_ERROR "RATCAST_VERSION \"${RATCAST_VERSION}\" is not a version like v1.2.3")
endif()

project(ratcast
    VERSION ${CMAKE_MATCH_1}
    DESCRIPTION "P2P calls and screen streaming on top of librats"
    HOMEPAGE_URL "https://github.com/librats/ratcast"
    LANGUAGES C CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

list(APPEND CMAKE_MODULE_PATH "${PROJECT_SOURCE_DIR}/cmake")

# Everything we build lands in one place, so the app finds its libraries and
# tests run from bin/ on every platform without RPATH or PATH tricks.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")

option(RATCAST_BUILD_APP   "Build the Qt 6 desktop application (needs Qt 6 Quick)" ON)
option(RATCAST_BUILD_TESTS "Build unit tests" ${PROJECT_IS_TOP_LEVEL})
option(RATCAST_BUILD_BENCH "Build benchmarks (bench/)" OFF)
option(RATCAST_BUILD_TOOLS "Build ratcast-send and ratcast-recv (tools/)" ON)
# A server's host: libratcast and ratcast-server alone, no media library looked
# for (engine, client, tools, bench and app left out).
option(RATCAST_SERVER_ONLY "Build libratcast and ratcast-server alone, for a bare host" OFF)
# The release packages (.github/workflows/release.yml) keep themselves at the
# latest release of this repository on GitHub; any other build never updates.
option(RATCAST_UPDATES "The app updates itself from GitHub's releases (release packages)" OFF)
# The ratcast-server the app's rooms meet at until the user names another.
set(RATCAST_DEFAULT_SERVER "" CACHE STRING
    "The server the app's rooms meet at by default: HOST or HOST:PORT; empty: none")
# The ratcast-servers a client knows of from the start; more are learnt from them.
set(RATCAST_SERVERS "degitx.com" CACHE STRING
    "The servers a client knows of from the start: HOST or HOST:PORT, separated by ';'")
set(RATCAST_UPDATE_REPOSITORY "librats/ratcast" CACHE STRING
    "The GitHub repository (owner/name) whose releases the app updates itself to")

include(RatcastCompiler)
include(RatcastPkgConfig)
include(RatcastInstall)  # before librats, whose GNUInstallDirs would set bin/ first
include(RatcastLibrats)

if(RATCAST_BUILD_TESTS)
    enable_testing()
    include(RatcastTesting)
endif()

# Dependency order, bottom to top. A module may use only those above it here.
add_subdirectory(libratcast)
add_subdirectory(server)

if(NOT RATCAST_SERVER_ONLY)
    add_subdirectory(engine)
    add_subdirectory(client)

    if(RATCAST_BUILD_TOOLS)
        add_subdirectory(tools)
    endif()

    if(RATCAST_BUILD_BENCH)
        add_subdirectory(bench)
    endif()

    if(RATCAST_BUILD_APP)
        add_subdirectory(app)
    endif()
endif()

ratcast_install_runtime()
