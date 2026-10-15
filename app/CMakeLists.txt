# The Qt 6 / QML desktop application. UI only: everything that is not about
# drawing and input belongs in a library below it, so the future Android UI and
# headless tools reuse it.

find_package(Qt6 6.5 QUIET COMPONENTS Quick)

if(NOT Qt6Quick_FOUND)
    message(WARNING
        "Qt 6.5+ with Qt Quick not found: skipping the desktop app.\n"
        "Install it (Debian/Ubuntu: qt6-declarative-dev) or configure with "
        "-DRATCAST_BUILD_APP=OFF to silence this.")
    return()
endif()

qt_standard_project_setup(REQUIRES 6.5)

qt_add_executable(ratcast_app
    src/main.cpp
)

set_target_properties(ratcast_app PROPERTIES
    OUTPUT_NAME      ratcast
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE    ON
    MACOSX_BUNDLE_GUI_IDENTIFIER com.librats.ratcast
    MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION}
    MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION}
)

qt_add_qml_module(ratcast_app
    URI     Ratcast
    VERSION 1.0
    QML_FILES
        qml/Main.qml
)

target_link_libraries(ratcast_app PRIVATE
    Qt6::Quick
    ratcast::ratcast
)
ratcast_target_defaults(ratcast_app)
