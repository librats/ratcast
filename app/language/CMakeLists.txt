# Ratcast.Language: Language, the language the window speaks — the system's,
# or one chosen and kept — through the app's translations (../i18n), which
# this library carries in its resources when Qt's Linguist tools are there
# (app/CMakeLists.txt). A static QML module: the app links its plugin.

qt_add_library(ratcast_language STATIC
    language.cpp
    language.h
)
qt_add_qml_module(ratcast_language
    URI     Ratcast.Language
    VERSION 1.0
    OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/qml/Ratcast/Language"
)

target_include_directories(ratcast_language PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(ratcast_language PUBLIC Qt6::Gui Qt6::Qml)
ratcast_target_defaults(ratcast_language)

if(RATCAST_BUILD_TESTS)
    add_subdirectory(tests)
endif()
