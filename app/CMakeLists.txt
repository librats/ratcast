# The Qt 6 / QML desktop application. UI only: everything that is not about
# drawing and input belongs in a library below it, so the future Android UI and
# headless tools reuse it.
#
# Pictures are drawn by a shader (video/): Qt Shader Tools compiles it at build
# time, and the textures it reads are made through QRhi, whose header Debian
# and Ubuntu ship in qt6-base-private-dev.

# QRhi's API is semi-public: it may change between Qt's minor versions, which
# is understood (Qt Multimedia draws video the same way).
set(QT_NO_PRIVATE_MODULE_WARNING ON)
find_package(Qt6 6.6 QUIET COMPONENTS Quick ShaderTools GuiPrivate Network)

# Debian ships GuiPrivate's CMake package with qt6-base-dev and its headers with
# qt6-base-private-dev: found, it may still have no QRhi header.
set(qt_headers "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_HEADERS}/QtGui")
if(NOT EXISTS "${qt_headers}/rhi/qrhi.h" AND
   NOT EXISTS "${qt_headers}/${Qt6Gui_VERSION}/QtGui/rhi/qrhi.h")
    set(Qt6GuiPrivate_FOUND FALSE)
endif()

if(NOT Qt6Quick_FOUND OR NOT Qt6ShaderTools_FOUND OR NOT Qt6GuiPrivate_FOUND OR
   NOT Qt6Network_FOUND)
    message(WARNING
        "Qt 6.6+ with Qt Quick, Qt Shader Tools and QtGui's private headers not found: "
        "skipping the desktop app.\n"
        "Install them (Debian/Ubuntu: qt6-declarative-dev qt6-shadertools-dev "
        "qt6-base-private-dev; the Qt installer: the Qt Shader Tools module) or configure "
        "with -DRATCAST_BUILD_APP=OFF to silence this.")
    return()
endif()

qt_standard_project_setup(REQUIRES 6.6)
if(QT_KNOWN_POLICY_QTP0004)
    qt_policy(SET QTP0004 NEW)  # qmldir for QML files in subdirectories (qml/)
endif()

# Qt's signals / slots / emit macros would rewrite librats' headers (its
# EventBus has an emit()): Q_SIGNALS and Q_EMIT in ours.
add_compile_definitions(QT_NO_KEYWORDS)

add_subdirectory(video)
add_subdirectory(call)
add_subdirectory(update)

qt_add_executable(ratcast_app
    src/main.cpp
)

# The executable is ratcast, but its import library (MSVC) must not take the
# name of libratcast's ratcast.lib, which it links.
set_target_properties(ratcast_app PROPERTIES
    OUTPUT_NAME         ratcast
    ARCHIVE_OUTPUT_NAME ratcast_app
    WIN32_EXECUTABLE    ON
    MACOSX_BUNDLE       ON
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

# The QML import scan exists to link QML plugins into a static Qt. A shared Qt
# loads them at run time, and distribution packages do not ship the plugin
# targets the scan looks for, so it would only warn about each of them. On
# Windows it stays: the install deploys the QML modules it finds.
if(QT6_IS_SHARED_LIBS_BUILD AND NOT WIN32)
    set_target_properties(ratcast_app PROPERTIES QT_QML_MODULE_NO_IMPORT_SCAN TRUE)
endif()

target_link_libraries(ratcast_app PRIVATE
    Qt6::Quick
    ratcast_videoplugin
    ratcast_callplugin
    ratcast_call
    ratcast_updateplugin
    ratcast_update
    ratcast::ratcast
)
ratcast_target_defaults(ratcast_app)

ratcast_install_programs(ratcast_app)

# Qt's libraries, plugins and the QML modules Main.qml imports, beside the app
# (windeployqt). The compiler's runtime is ratcast_install_runtime()'s.
if(WIN32)
    qt_generate_deploy_qml_app_script(
        TARGET ratcast_app
        OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS
        NO_COMPILER_RUNTIME
        EXCLUDE_PLUGIN_TYPES qmltooling  # the QML debugger's
    )
    install(SCRIPT "${deploy_script}")
elseif(UNIX AND NOT APPLE)
    # The desktop's menu entry and icon; Qt and the libraries are bundled into
    # the AppImage by linuxdeploy (.github/workflows/release.yml).
    install(FILES resources/ratcast.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES resources/ratcast.svg
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
endif()
