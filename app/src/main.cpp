#include "ratcast/version.h"

#include <QtQml/qqmlextensionplugin.h>
#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>

// Our QML modules, linked in statically.
Q_IMPORT_QML_PLUGIN(Ratcast_VideoPlugin)
Q_IMPORT_QML_PLUGIN(Ratcast_CallPlugin)

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    QGuiApplication::setApplicationName(QStringLiteral("ratcast"));
    QGuiApplication::setOrganizationName(QStringLiteral("librats"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(ratcast::version()));

    QCommandLineParser options;
    options.setApplicationDescription(QStringLiteral("Share a screen, or watch one."));
    options.addHelpOption();
    options.addVersionOption();
    options.addPositionalArgument(QStringLiteral("link"),
                                  QStringLiteral("A rats:// link to watch at once."),
                                  QStringLiteral("[link]"));
    options.process(app);

    QQmlApplicationEngine engine;
    engine.setInitialProperties({
        {QStringLiteral("ratcastVersion"), QString::fromLatin1(ratcast::version())},
        {QStringLiteral("libratsVersion"), QString::fromLatin1(ratcast::librats_version())},
        {QStringLiteral("link"), options.positionalArguments().value(0)},
    });

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Ratcast", "Main");

    return QGuiApplication::exec();
}
