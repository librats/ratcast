# Benchmarks, on librats' harness (external/librats/bench/framework): a header-only
# timing loop plus global allocation counters. Build with -DRATCAST_BUILD_BENCH=ON
# and run from a Release build:
#
#   cmake --preset release -DRATCAST_BUILD_BENCH=ON
#   cmake --build --preset release && ./build/release/bin/bench_core

set(RATCAST_BENCH_FRAMEWORK "${RATCAST_LIBRATS_DIR}/bench")

function(ratcast_add_bench name)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${name}
        ${ARG_SOURCES}
        "${RATCAST_BENCH_FRAMEWORK}/framework/alloc_track.cpp"
    )
    target_include_directories(${name} PRIVATE "${RATCAST_BENCH_FRAMEWORK}")
    target_link_libraries(${name} PRIVATE ${ARG_LIBS})
    ratcast_target_defaults(${name})
endfunction()

# Media bytes and frame headers: libratcast's per-frame hot path.
ratcast_add_bench(bench_core
    SOURCES bench_core.cpp
    LIBS    ratcast::ratcast
)
target_include_directories(bench_core PRIVATE "${PROJECT_SOURCE_DIR}/libratcast/src")

# MediaSession's own per-frame costs, over a network that does nothing.
ratcast_add_bench(bench_session
    SOURCES bench_session.cpp
    LIBS    ratcast::ratcast
)
target_include_directories(bench_session PRIVATE "${PROJECT_SOURCE_DIR}/libratcast/src")

# Engine codecs: milliseconds per picture at 720p and 1080p, per encoder.
ratcast_add_bench(bench_codec
    SOURCES bench_codec.cpp
    LIBS    ratcast::engine
)
