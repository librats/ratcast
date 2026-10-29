#include "call_controller.h"
#include "install.h"
#include "librats/util/logger.h"
#include "ratcast/client/log_file.h"
#include "ratcast/version.h"

#include <QtQml/qqmlextensionplugin.h>
#include <QColor>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QPointer>
#include <QQmlApplicationEngine>
#include <QString>
#include <QSysInfo>
#include <QUrl>
#include <QVariant>

#ifdef Q_OS_ANDROID
#include <QJniEnvironment>
#include <QJniObject>

#include "ratcast/engine/android.h"
#endif

#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <utility>

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

/// rats:// links the system opens the app with — Android's intent filter
/// (app/android/AndroidManifest.xml), handed on by Qt — joined by the window
/// (its openLink()); one that comes before the window is kept for it.
class LinkOpener final : public QObject {
    Q_OBJECT

public:
    void show_to(QObject* window) {
        window_ = window;
        if (!pending_.isEmpty()) open(QUrl(std::exchange(pending_, {})));
    }

public Q_SLOTS:
    void open(const QUrl& url) {
        if (!window_) {
            pending_ = url.toString();
            return;
        }
        QMetaObject::invokeMethod(window_, "openLink", QVariant(url.toString()));
    }

private:
    QPointer<QObject> window_;
    QString pending_;
};

}  // namespace

int main(int argc, char* argv[]) {
    // What Qt itself makes of "import QtQuick.Controls" — every ToolTip —
    // drawn in the style of the rest of the window.
    if (!qEnvironmentVariableIsSet("QT_QUICK_CONTROLS_STYLE"))
        qputenv("QT_QUICK_CONTROLS_STYLE", "Material");
    QGuiApplication app(argc, argv);
#ifdef Q_OS_ANDROID
    // What the engine reaches only through Java (the screen kept on).
    ratcast::android::attach(QJniEnvironment::javaVM(),
                             QNativeInterface::QAndroidApplication::context().object());
#endif
    LinkOpener links;
    QDesktopServices::setUrlHandler(QStringLiteral("rats"), &links, "open");
    // Qt takes the user's locale on Unix; numbers written and read below Qt —
    // the call's stats, codecs' options — keep the C one, as Qt advises.
    std::setlocale(LC_NUMERIC, "C");
    QGuiApplication::setApplicationName(QStringLiteral("ratcast"));
    QGuiApplication::setOrganizationName(QStringLiteral("ratcast"));
    QGuiApplication::setApplicationVersion(QString::fromLatin1(ratcast::version()));
    // Wayland finds the window's icon by its desktop entry; Windows' is the
    // executable's own (ratcast.rc.in), macOS's Dock the bundle's (ratcast.icns).
    QGuiApplication::setDesktopFileName(QStringLiteral("ratcast"));
#ifndef Q_OS_WIN
    QGuiApplication::setWindowIcon(QIcon(QStringLiteral(":/ratcast/ratcast.png")));
#endif
    // Links in rich text (the chat's) are drawn in the palette's colour as
    // they are read: one the dark window shows.
    QPalette palette = QGuiApplication::palette();
    palette.setColor(QPalette::Link, QColor(0x81, 0xd4, 0xfa));  // Material Light Blue 200
    QGuiApplication::setPalette(palette);
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
    // The times logged are this computer's own: told against UTC, so that two
    // members' logs line up.
    const int offset = QDateTime::currentDateTime().offsetFromUtc() / 60;
    LOG_INFO("app",
             "times are UTC" << QString::asprintf("%c%02d:%02d", offset < 0 ? '-' : '+',
                                                  std::abs(offset) / 60, std::abs(offset) % 60)
                                    .toStdString());

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
    if (!engine.rootObjects().isEmpty()) links.show_to(engine.rootObjects().constFirst());

    return QGuiApplication::exec();
}

#include "main.moc"
