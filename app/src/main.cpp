#include "call_controller.h"
#include "install.h"
#include "language.h"
#include "librats/util/logger.h"
#include "links.h"
#include "ratcast/client/log_file.h"
#include "ratcast/version.h"

#include <QtQml/qqmlextensionplugin.h>
#include <QColor>
#include <QCommandLineParser>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QIcon>
#include <QPalette>
#include <QQmlApplicationEngine>
#include <QString>
#include <QSvgRenderer>
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

// Our QML modules, linked in statically.
Q_IMPORT_QML_PLUGIN(Ratcast_VideoPlugin)
Q_IMPORT_QML_PLUGIN(Ratcast_CallPlugin)
Q_IMPORT_QML_PLUGIN(Ratcast_UpdatePlugin)
Q_IMPORT_QML_PLUGIN(Ratcast_LanguagePlugin)

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

/// What the window asks of Android's activity; elsewhere nothing. Its
/// toBackground(): the app sent behind as Home sends it (a window cannot be
/// minimized there), the room going on. Its share full screen: turned on its
/// side, either way up, as a film is watched; back as it was after.
class Activity final : public QObject {
    Q_OBJECT

public:
    explicit Activity(QObject* window) : window_(window) {}

public Q_SLOTS:
    void to_background() {
#ifdef Q_OS_ANDROID
        QJniObject activity = QNativeInterface::QAndroidApplication::context();
        if (activity.isValid()) activity.callMethod<jboolean>("moveTaskToBack", jboolean(true));
#endif
    }

    void full_screen_changed() {
#ifdef Q_OS_ANDROID
        // ActivityInfo's SCREEN_ORIENTATION_SENSOR_LANDSCAPE, _UNSPECIFIED.
        const jint orientation = window_->property("fullScreen").toBool() ? 6 : -1;
        QJniObject activity = QNativeInterface::QAndroidApplication::context();
        if (activity.isValid()) activity.callMethod<void>("setRequestedOrientation", orientation);
#endif
    }

private:
    QObject* window_;
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
    ratcast::app::LinkOpener links;
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
    // The buttons' icons are SVG, drawn by Qt SVG's image plugin. Used here,
    // the library is linked for real (a linker drops one nothing uses), and so
    // each deploy tool — windeployqt, macdeployqt, androiddeployqt, linuxdeploy
    // — brings it and its plugin along.
    static_cast<void>(QSvgRenderer::staticMetaObject.className());
    // An update an earlier run downloaded, put in place before anything else
    // is loaded: the new version then starts in this one's place.
    if (ratcast::app::update::start_updated()) return EXIT_SUCCESS;

    const QString data = ratcast::app::data_folder();
    ratcast::app::forget_rooms_nodes(data);
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
                                  QStringLiteral("A link to a room, to join at once."),
                                  QStringLiteral("[link]"));
    options.process(app);
    const QString link = options.positionalArguments().value(0);
    // A link the system opened a second app with joined by the first, unless
    // it is in a room already.
    ratcast::app::SingleInstance instance(data);
    if (!link.isEmpty() && instance.handed_over(link)) {
        LOG_INFO("app", "the link handed to the app running");
        return EXIT_SUCCESS;
    }
    ratcast::app::register_link_scheme();
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
    // The language kept, or the system's: in place before the window is made.
    ratcast::app::Language language(&engine);
    LOG_INFO("app", "language " << language.current().toStdString()
                                << (language.chosen().isEmpty() ? ", the system's" : ", chosen"));
    engine.setInitialProperties({
        {QStringLiteral("ratcastVersion"), QString::fromLatin1(ratcast::version())},
        {QStringLiteral("libratsVersion"), QString::fromLatin1(ratcast::librats_version())},
        {QStringLiteral("link"), link},
        {QStringLiteral("statsEvery"), every},
        {QStringLiteral("dataFolder"), QUrl::fromLocalFile(data)},
    });

    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreationFailed, &app,
        [] { QCoreApplication::exit(EXIT_FAILURE); }, Qt::QueuedConnection);
    engine.loadFromModule("Ratcast", "Main");
    if (engine.rootObjects().isEmpty()) return QGuiApplication::exec();
    QObject* window = engine.rootObjects().constFirst();
    links.show_to(window);
    instance.listen(&links);
    Activity activity(window);
    QObject::connect(window, SIGNAL(toBackground()), &activity, SLOT(to_background()));
    QObject::connect(window, SIGNAL(fullScreenChanged()), &activity, SLOT(full_screen_changed()));

    return QGuiApplication::exec();
}

#include "main.moc"
