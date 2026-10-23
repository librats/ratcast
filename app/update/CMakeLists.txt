# Ratcast.Update: Updater, the app keeping itself at GitHub's latest release —
# the release's package for this system fetched, checked and put in place of
# the one running (install.h). A static QML module: the app links its plugin.
# Only a package made with RATCAST_UPDATES updates itself; any other build
# says it cannot.

qt_add_library(ratcast_update STATIC
    install.cpp
    install.h
    release.cpp
    release.h
    updater.cpp
    updater.h
)
qt_add_qml_module(ratcast_update
    URI     Ratcast.Update
    VERSION 1.0
    OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/qml/Ratcast/Update"
)

target_include_directories(ratcast_update PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ratcast_update PUBLIC Qt6::Network Qt6::Qml ratcast::ratcast)
target_compile_definitions(ratcast_update PRIVATE
    RATCAST_UPDATE_REPOSITORY="${RATCAST_UPDATE_REPOSITORY}"
    $<$<BOOL:${RATCAST_UPDATES}>:RATCAST_UPDATES=1>
)
ratcast_target_defaults(ratcast_update)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
