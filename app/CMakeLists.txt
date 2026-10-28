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
# qt6-base-private-dev: found, it may still have no QRhi header. On macOS Qt's
# modules are frameworks, their headers inside them rather than in include/.
set(qt_headers "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_HEADERS}/QtGui")
set(qt_framework_headers "${QT6_INSTALL_PREFIX}/${QT6_INSTALL_LIBS}/QtGui.framework/Headers")
if(NOT EXISTS "${qt_headers}/rhi/qrhi.h" AND
   NOT EXISTS "${qt_headers}/${Qt6Gui_VERSION}/QtGui/rhi/qrhi.h" AND
   NOT EXISTS "${qt_framework_headers}/rhi/qrhi.h")
    set(Qt6GuiPrivate_FOUND FALSE)
endif()

set(qt_missing)
foreach(component IN ITEMS Quick ShaderTools GuiPrivate Network)
    if(NOT Qt6${component}_FOUND)
        list(APPEND qt_missing ${component})
    endif()
endforeach()
if(qt_missing)
    list(JOIN qt_missing ", " qt_missing)
    message(WARNING
        "Qt 6.6+ with Qt Quick, Qt Shader Tools and QtGui's private headers not found "
        "(${qt_missing}): skipping the desktop app.\n"
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
)

# Its icon: the tray's, and elsewhere than on Windows the windows' (the menu's
# comes from the desktop entry); on Windows the executable's, with its version
# (resources/ratcast.rc.in).
qt_add_resources(ratcast_app ratcast_icon
    PREFIX /ratcast
    BASE   resources
    FILES  resources/ratcast.png
)
if(WIN32)
    configure_file(resources/ratcast.rc.in ratcast.rc @ONLY)
    target_sources(ratcast_app PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/ratcast.rc")
endif()

# On macOS a bundle, ratcast.app: its Info.plist — what the system asks the
# user's leave with — and the Finder's and the Dock's icon.
if(APPLE)
    configure_file(resources/Info.plist.in Info.plist @ONLY)
    set_target_properties(ratcast_app PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_BINARY_DIR}/Info.plist")
    target_sources(ratcast_app PRIVATE resources/ratcast.icns)
    set_source_files_properties(resources/ratcast.icns
        PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
endif()

qt_add_qml_module(ratcast_app
    URI     Ratcast
    VERSION 1.0
    QML_FILES
        qml/Main.qml
        qml/SettingsDialog.qml
)

# The QML import scan exists to link QML plugins into a static Qt. A shared Qt
# loads them at run time, and distribution packages do not ship the plugin
# targets the scan looks for, so it would only warn about each of them. On
# Windows and macOS it stays: the install deploys the QML modules it finds.
if(QT6_IS_SHARED_LIBS_BUILD AND NOT WIN32 AND NOT APPLE)
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
# (windeployqt) or in its bundle (macdeployqt). The compiler's runtime is
# ratcast_install_runtime()'s.
if(WIN32)
    qt_generate_deploy_qml_app_script(
        TARGET ratcast_app
        OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS
        NO_COMPILER_RUNTIME
        EXCLUDE_PLUGIN_TYPES qmltooling  # the QML debugger's
    )
    install(SCRIPT "${deploy_script}")
elseif(APPLE)
    # Into the bundle (macdeployqt): Qt's frameworks, plugins and QML modules,
    # and the libraries the app links — FFmpeg's and the codecs', found where
    # vcpkg or the prefixes searched keep them, as they are linked by @rpath.
    set(library_paths)
    if(VCPKG_INSTALLED_DIR AND VCPKG_TARGET_TRIPLET)
        list(APPEND library_paths "-libpath=${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}/lib")
    endif()
    foreach(prefix IN LISTS CMAKE_PREFIX_PATH)
        if(IS_DIRECTORY "${prefix}/lib")
            list(APPEND library_paths "-libpath=${prefix}/lib")
        endif()
    endforeach()
    qt_generate_deploy_qml_app_script(
        TARGET ratcast_app
        OUTPUT_SCRIPT deploy_script
        NO_TRANSLATIONS
        DEPLOY_TOOL_OPTIONS ${library_paths}
    )
    install(SCRIPT "${deploy_script}")
elseif(UNIX)
    # The desktop's menu entry and icon; Qt and the libraries are bundled into
    # the AppImage by linuxdeploy (.github/workflows/release.yml).
    install(FILES resources/ratcast.desktop DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES resources/ratcast.png
            DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/512x512/apps")
endif()
