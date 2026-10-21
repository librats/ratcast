#include "ratcast/version.h"

#include <QtQml/qqmlextensionplugin.h>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>

// Ratcast.Video, linked in statically.
Q_IMPORT_QML_PLUGIN(Ratcast_VideoPlugin)

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("ratcast"));
    QGuiApplication::setOrganizationName(QStringLiteral("librats"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(ratcast::version()));

    QQmlApplicationEngine engine;
    engine.setInitialProperties({
        {QStringLiteral("ratcastVersion"), QString::fromLatin1(ratcast::version())},
        {QStringLiteral("libratsVersion"), QString::fromLatin1(ratcast::librats_version())},
    });

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Ratcast", "Main");

    return QGuiApplication::exec();
}
