# librats, built from the submodule in external/librats and developed together
# with ratcast.
#
# Only the library itself is built by default. Each default below is applied
# only when the variable is not already set, so any of them can be overridden on
# the configure line, e.g. -DRATS_BUILD_TESTS=ON to run librats' own suite.

set(RATCAST_LIBRATS_DIR "${PROJECT_SOURCE_DIR}/external/librats")

if(NOT EXISTS "${RATCAST_LIBRATS_DIR}/CMakeLists.txt")
    message(FATAL_ERROR
        "librats submodule is missing. Run:\n"
        "  git submodule update --init --recursive")
endif()

macro(ratcast_librats_default name value)
    if(NOT DEFINED ${name})
        set(${name} ${value})
    endif()
endmacro()

ratcast_librats_default(RATS_BUILD_CLIENT   OFF)
ratcast_librats_default(RATS_BUILD_EXAMPLES OFF)
ratcast_librats_default(RATS_BUILD_TESTS    OFF)
ratcast_librats_default(RATS_BINDINGS       OFF)
ratcast_librats_default(RATS_INSTALL        OFF)

add_subdirectory("${RATCAST_LIBRATS_DIR}" "${PROJECT_BINARY_DIR}/external/librats")

if(NOT TARGET librats::rats)
    add_library(librats::rats ALIAS rats)
endif()
