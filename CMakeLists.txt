cmake_minimum_required(VERSION 3.21)

project(ratcast
    VERSION 0.1.0
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

include(RatcastCompiler)
include(RatcastLibrats)

if(RATCAST_BUILD_TESTS)
    enable_testing()
    include(RatcastTesting)
endif()

# Dependency order, bottom to top. A module may use only those above it here.
add_subdirectory(libratcast)

if(RATCAST_BUILD_BENCH)
    add_subdirectory(bench)
endif()

if(RATCAST_BUILD_APP)
    add_subdirectory(app)
endif()
