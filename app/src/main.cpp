#include "call_controller.h"
#include "install.h"
#include "librats/util/logger.h"
#include "ratcast/client/log_file.h"
#include "ratcast/version.h"

#include <QtQml/qqmlextensionplugin.h>
#include <QCommandLineParser>
#include <QDir>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QString>
#include <QSysInfo>
#include <QUrl>

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>

// Our QML modules, linked in statically.
Q_IMPORT_QML_PLUGIN(Ratcast_VideoPlugin)
Q_IMPORT_QML_PLUGIN(Ratcast_CallPlugin)
Q_IMPORT_QML_PLUGIN(Ratcast_UpdatePlugin)

namespace {

/// Qt's messages, QML's warnings among them, logged with the rest: in the log
/// file too.
void log_qt_message(QtMsgType type, const QMessageLogContext& context, const QString& text) {
    using librats::LogLevel;
    LogLevel level = LogLevel::ERROR;  // critical and fatal
    switch (type) {
        case QtDebugMsg: level = LogLevel::DEBUG; break;
        case QtInfoMsg: level = LogLevel::INFO; break;
        case QtWarningMsg: level = LogLevel::WARN; break;
        default: break;
    }
    const bool named = context.category && std::strcmp(context.category, "default") != 0;
    librats::Logger::getInstance().log(level, named ? context.category : "qt", text.toStdString());
}

}  // namespace

int main(int argc, char* argv[]) {
    QGuiApplication app(argc, argv);
    // Qt takes the user's locale on Unix; numbers written and read below Qt —
    // the call's stats, codecs' options — keep the C one, as Qt advises.
    std::setlocale(LC_NUMERIC, "C");
    QGuiApplication::setApplicationName(QStringLiteral("ratcast"));
    QGuiApplication::setOrganizationName(QStringLiteral("ratcast"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(ratcast::version()));
    // An update an earlier run downloaded, put in place before anything else
    // is loaded: the new version then starts in this one's place.
    if (ratcast::app::update::start_updated()) return EXIT_SUCCESS;

    const QString data = ratcast::app::data_folder();
    const ratcast::LogFile log(
        {.path = QDir(data).filePath(QStringLiteral("ratcast.log")).toStdString()});
    qInstallMessageHandler(log_qt_message);
    LOG_INFO("app", "ratcast " << ratcast::version() << ", librats " << ratcast::librats_version()
                               << ", Qt " << qVersion() << ", "
                               << QSysInfo::prettyProductName().toStdString() << " "
                               << QSysInfo::currentCpuArchitecture().toStdString());

    QCommandLineParser options;
    options.setApplicationDescription(
        QStringLiteral("Rooms to talk, share a screen and watch one in."));
    options.addHelpOption();
    options.addVersionOption();
    const QCommandLineOption stats(
        QStringLiteral("stats"),
        QStringLiteral("In a call, print how it goes on standard output every two seconds."));
    const QCommandLineOption stats_every(
        QStringLiteral("stats-every"),
        QStringLiteral("In a call, print how it goes every <seconds> instead."),
        QStringLiteral("seconds"));
    options.addOptions({stats, stats_every});
    options.addPositionalArgument(QStringLiteral("link"),
                                  QStringLiteral("A rats:// link to a room, to join at once."),
                                  QStringLiteral("[link]"));
    options.process(app);
    int every = options.isSet(stats) ? 2 : 0;
    if (options.isSet(stats_every)) {
        bool number = false;
        every = options.value(stats_every).toInt(&number);
        if (!number || every <= 0) {
            std::fputs("ratcast: --stats-every wants a number of seconds\n", stderr);
            return EXIT_FAILURE;
        }
    }

    QQmlApplicationEngine engine;
    engine.setInitialProperties({
        {QStringLiteral("ratcastVersion"), QString::fromLatin1(ratcast::version())},
        {QStringLiteral("libratsVersion"), QString::fromLatin1(ratcast::librats_version())},
        {QStringLiteral("link"), options.positionalArguments().value(0)},
        {QStringLiteral("statsEvery"), every},
        {QStringLiteral("dataFolder"), QUrl::fromLocalFile(data)},
    });

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Ratcast", "Main");

    return QGuiApplication::exec();
}
