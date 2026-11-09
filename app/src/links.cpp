#include "links.h"

#include "librats/util/logger.h"
#include "ratcast/client/people.h"
#include "ratcast/client/rats_url.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDesktopServices>
#include <QDir>
#include <QEvent>
#include <QFile>
#include <QFileInfo>
#include <QFileOpenEvent>
#include <QLocalSocket>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>
#include <QVariant>
#include <QWindow>

#include <utility>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ratcast::app {
namespace {

const QString kRats = QStringLiteral("rats");
const QString kHttps = QStringLiteral("https");

/// Whether links are handed from one process to another here: where the
/// system starts a process for each link it opens. macOS hands them to the
/// app running, Android to its activity.
constexpr bool kHandsOver =
#if defined(Q_OS_WIN) || (defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID))
    true;
#else
    false;
#endif

/// The longest line a process hands over: a link, with room to spare.
constexpr qint64 kMostLink = 8192;

}  // namespace

// ── LinkOpener ──────────────────────────────────────────────────────────────

LinkOpener::LinkOpener() {
    QDesktopServices::setUrlHandler(kRats, this, "open");
#ifdef Q_OS_ANDROID
    // The website's links, which Android's App Links open the app with.
    QDesktopServices::setUrlHandler(kHttps, this, "open");
#endif
#ifdef Q_OS_MACOS
    QCoreApplication::instance()->installEventFilter(this);
#endif
}

LinkOpener::~LinkOpener() {
    QDesktopServices::unsetUrlHandler(kRats);
#ifdef Q_OS_ANDROID
    QDesktopServices::unsetUrlHandler(kHttps);
#endif
}

void LinkOpener::show_to(QObject* window) {
    window_ = window;
    if (!pending_.isEmpty()) hand_on(std::exchange(pending_, {}));
}

bool LinkOpener::hand_on(const QString& link) {
    if (!window_) {
        pending_ = link;
        return false;
    }
    QVariant taken;
    QMetaObject::invokeMethod(window_, "openLink", Q_RETURN_ARG(QVariant, taken),
                              Q_ARG(QVariant, QVariant(link)));
    if (!taken.toBool()) return false;
    if (auto* window = qobject_cast<QWindow*>(window_.data())) {
        window->setWindowStates(window->windowStates() & ~Qt::WindowMinimized);
        window->show();  // out of the tray
        window->raise();
        window->requestActivate();
    }
    return true;
}

void LinkOpener::open(const QUrl& url) {
    const QString link = url.toString();
    if (url.scheme() != kRats && !parse_rats_link(link.toStdString()) &&
        !parse_person_link(link.toStdString()) && !parse_group_link(link.toStdString())) {
        // The handler takes every https:// link the app opens too — the
        // chat's: those go on to the browser.
        const QString scheme = url.scheme();
        QDesktopServices::unsetUrlHandler(scheme);
        QDesktopServices::openUrl(url);
        QDesktopServices::setUrlHandler(scheme, this, "open");
        return;
    }
    hand_on(link);
}

bool LinkOpener::eventFilter(QObject* watched, QEvent* event) {
    if (event->type() == QEvent::FileOpen) {
        const QUrl url = static_cast<QFileOpenEvent*>(event)->url();
        if (url.scheme() == kRats) {
            hand_on(url.toString());
            return true;
        }
    }
    return QObject::eventFilter(watched, event);
}

// ── SingleInstance ──────────────────────────────────────────────────────────

SingleInstance::SingleInstance(const QString& data_folder)
    : name_(QStringLiteral("ratcast-") +
            QString::fromLatin1(QCryptographicHash::hash(QDir(data_folder).absolutePath().toUtf8(),
                                                         QCryptographicHash::Sha256)
                                    .toHex()
                                    .left(16))) {
    server_.setSocketOptions(QLocalServer::UserAccessOption);
    connect(&server_, &QLocalServer::newConnection, this, [this] {
        while (QLocalSocket* socket = server_.nextPendingConnection()) {
            connect(socket, &QLocalSocket::disconnected, socket, &QObject::deleteLater);
            connect(socket, &QLocalSocket::readyRead, socket, [this, socket] {
                if (!socket->canReadLine()) {
                    if (socket->bytesAvailable() > kMostLink) socket->abort();
                    return;
                }
                const QString link = QString::fromUtf8(socket->readLine(kMostLink)).trimmed();
                const bool taken = opener_ && opener_->hand_on(link);
                LOG_INFO("app", "another start handed over, " << (taken ? "taken" : "not taken"));
                socket->write("1");
                socket->disconnectFromServer();
            });
        }
    });
}

bool SingleInstance::handed_over(const QString& link) {
    if (!kHandsOver) return false;
    QLocalSocket socket;
    socket.connectToServer(name_);
    if (!socket.waitForConnected(500)) return false;
#ifdef Q_OS_WIN
    // The window brought to the front by the process running: this one,
    // started by the browser, may give it the right.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    socket.write(link.toUtf8() + '\n');
    if (!socket.waitForBytesWritten(1000)) return false;
    // One that does not answer is stuck: no reason not to start.
    return socket.waitForReadyRead(3000);
}

void SingleInstance::listen(LinkOpener* opener) {
    if (!kHandsOver) return;
    opener_ = opener;
#ifdef Q_OS_WIN
    // The installer's and uninstaller's sign that the app is running
    // (AppMutex in app/installer/windows.iss), whatever its data folder: held
    // until the process ends.
    CreateMutexW(nullptr, FALSE, L"ratcast-app");
#endif
    // A process that answers keeps the name (one started at the same moment);
    // one that does not left it behind (a socket's file on Linux).
    QLocalSocket probe;
    probe.connectToServer(name_);
    if (probe.waitForConnected(200)) return;
    QLocalServer::removeServer(name_);
    if (!server_.listen(name_))
        LOG_WARN("app", "other starts not handed over: " << server_.errorString().toStdString());
}

void SingleInstance::stop() { server_.close(); }

// ── The scheme ──────────────────────────────────────────────────────────────

void register_link_scheme() {
#ifdef Q_OS_WIN
    const QString program = QDir::toNativeSeparators(QCoreApplication::applicationFilePath());
    QSettings scheme(QStringLiteral("HKEY_CURRENT_USER\\Software\\Classes\\rats"),
                     QSettings::NativeFormat);
    const QString key = QStringLiteral("shell/open/command/Default");
    const QString command = scheme.value(key).toString();
    const QString wanted = QStringLiteral("\"%1\" \"%2\"").arg(program, QStringLiteral("%1"));
    if (command == wanted) return;
#ifndef RATCAST_UPDATES
    // Another build's — a release's — kept while its program is there.
    if (command.startsWith(u'"')) {
        const qsizetype end = command.indexOf(u'"', 1);
        if (end > 1 && QFileInfo::exists(command.mid(1, end - 1))) return;
    }
#endif
    scheme.setValue(QStringLiteral("Default"), QStringLiteral("URL:rats"));
    scheme.setValue(QStringLiteral("URL Protocol"), QString());
    scheme.setValue(QStringLiteral("DefaultIcon/Default"), QStringLiteral("\"%1\",0").arg(program));
    scheme.setValue(key, wanted);
    scheme.sync();
    LOG_INFO("app", "rats:// links open " << program.toStdString());
#elif defined(Q_OS_LINUX) && !defined(Q_OS_ANDROID)
    // An AppImage is not installed: its desktop entry is nowhere the desktop
    // looks. One of its own, for links alone, made the scheme's default.
    const QString appimage = qEnvironmentVariable("APPIMAGE");
    if (appimage.isEmpty()) return;
    // A path the entry's Exec would have to escape is passed over.
    for (const QChar c : {u'"', u'`', u'$', u'\\', u'\n'}) {
        if (appimage.contains(c)) return;
    }
    const QString folder = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    const QString name = QStringLiteral("ratcast-links.desktop");
    const QByteArray entry =
        "[Desktop Entry]\n"
        "Type=Application\n"
        "Name=ratcast\n"
        "Exec=\"" +
        appimage.toUtf8() +
        "\" %u\n"
        "Icon=ratcast\n"
        "NoDisplay=true\n"
        "Terminal=false\n"
        "MimeType=x-scheme-handler/rats;\n";
    QFile file(QDir(folder).filePath(name));
    if (file.open(QIODevice::ReadOnly) && file.readAll() == entry) return;
    file.close();
    if (!QDir().mkpath(folder) || !file.open(QIODevice::WriteOnly | QIODevice::Truncate)) return;
    file.write(entry);
    file.close();
    QProcess::startDetached(QStringLiteral("xdg-mime"), {QStringLiteral("default"), name,
                                                         QStringLiteral("x-scheme-handler/rats")});
    LOG_INFO("app", "rats:// links open " << appimage.toStdString());
#endif
}

}  // namespace ratcast::app
