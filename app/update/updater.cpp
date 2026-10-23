#include "updater.h"

#include <QCoreApplication>
#include <QDir>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QSettings>

#include <system_error>
#include <utility>

#include "ratcast/version.h"

namespace ratcast::app {

namespace {

const QString kAutomaticKey = QStringLiteral("updates/automatic");
/// Of a request that has stopped moving.
constexpr int kCheckTimeoutMs = 30'000;
constexpr int kDownloadTimeoutMs = 60'000;
/// Unpacking the Windows zip (its own tar, Windows 10 1803 and later).
constexpr int kUnpackTimeoutMs = 5 * 60'000;

QNetworkRequest request_to(const QUrl& url, int timeout_ms) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("ratcast/%1").arg(QString::fromLatin1(ratcast::version())));
    request.setTransferTimeout(timeout_ms);
    return request;
}

}  // namespace

Updater::Environment Updater::system() {
    Environment environment;
    environment.target = update::this_target();
    environment.latest = QUrl(QStringLiteral(
        "https://api.github.com/repos/" RATCAST_UPDATE_REPOSITORY "/releases/latest"));
    environment.current = QVersionNumber::fromString(QString::fromLatin1(ratcast::version()));
    return environment;
}

Updater::Updater(QObject* parent) : Updater(system(), parent) {}

Updater::Updater(Environment environment, QObject* parent)
    : QObject(parent),
      environment_(std::move(environment)),
      network_(new QNetworkAccessManager(this)),
      automatic_(QSettings().value(kAutomaticKey, true).toBool()) {
    timer_.setSingleShot(true);
    connect(&timer_, &QTimer::timeout, this, [this] {
        if (automatic_) begin(false);
    });
    if (supported() && automatic_) timer_.start(environment_.first_check);
}

Updater::~Updater() {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
    }
    if (unpack_) {
        unpack_->disconnect(this);
        unpack_->kill();
        unpack_->waitForFinished();
    }
}

void Updater::check() { begin(true); }

void Updater::begin(bool asked) {
    if (!supported() || state_ != State::Idle) return;
    asked_ = asked;
    if (pending_) {  // found already, waiting for quiet to end
        fetch();
        return;
    }
    setNotice({});
    setState(State::Checking);
    QNetworkRequest request = request_to(environment_.latest, kCheckTimeoutMs);
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("X-GitHub-Api-Version", "2022-11-28");
    QNetworkReply* const reply = network_->get(request);
    reply_ = reply;
    connect(reply, &QNetworkReply::finished, this, &Updater::checked);
}

void Updater::checked() {
    QNetworkReply* const reply = std::exchange(reply_, nullptr).data();
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        give_up(tr("Could not look for a new version: %1").arg(reply->errorString()));
        return;
    }
    std::optional<update::Release> release =
        update::parse_release(reply->readAll(), environment_.target->suffix);
    if (!release || release->version <= environment_.current) {
        qCInfo(update_log) << "no newer version than" << environment_.current;
        release_.reset();
        Q_EMIT versionChanged();
        if (asked_) setNotice(tr("This is the latest version."));
        idle();
        return;
    }
    qCInfo(update_log) << "version" << release->version << "found:" << release->url;
    release_ = std::move(release);
    Q_EMIT versionChanged();
    fetch();
}

void Updater::fetch() {
    pending_ = false;
    if (quiet_) {
        pending_ = true;
        setState(State::Idle);
        return;
    }
    const update::Target& target = *environment_.target;
    const update::fs::path path = update::download_path(target, release_->asset);
    std::error_code error;
    if (target.kind == update::Target::Kind::Folder) {
        update::fs::remove_all(update::staging(target.path), error);
        update::fs::create_directories(update::staging(target.path), error);
    }
    file_.setFileName(update::to_qstring(path));
    if (error || !file_.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        refuse(
            tr("ratcast cannot update itself: %1 cannot be written to. Download "
               "version %2 yourself.")
                .arg(QDir::toNativeSeparators(update::to_qstring(path.parent_path())), version()));
        return;
    }
    hash_.reset();
    stopped_ = false;
    write_failed_ = false;
    setProgress(0);
    setNotice({});
    setState(State::Downloading);
    QNetworkReply* const reply = network_->get(request_to(release_->url, kDownloadTimeoutMs));
    reply_ = reply;
    connect(reply, &QNetworkReply::readyRead, this, &Updater::received);
    connect(reply, &QNetworkReply::downloadProgress, this, [this](qint64 got, qint64 total) {
        if (total > 0) setProgress(double(got) / double(total));
    });
    connect(reply, &QNetworkReply::finished, this, &Updater::downloaded);
}

void Updater::received() {
    const QByteArray bytes = reply_->readAll();
    hash_.addData(bytes);
    if (file_.write(bytes) != bytes.size() && !write_failed_) {
        write_failed_ = true;
        reply_->abort();
    }
}

void Updater::downloaded() {
    if (reply_->error() == QNetworkReply::NoError) received();
    QNetworkReply* const reply = std::exchange(reply_, nullptr).data();
    reply->deleteLater();
    file_.close();
    if (stopped_) {
        file_.remove();
        pending_ = true;
        setState(State::Idle);
        return;
    }
    if (write_failed_) {
        file_.remove();
        refuse(tr("ratcast cannot update itself: %1 cannot be written to. Download "
                  "version %2 yourself.")
                   .arg(QDir::toNativeSeparators(file_.fileName()), version()));
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        file_.remove();
        give_up(tr("Could not download version %1: %2").arg(version(), reply->errorString()));
        return;
    }
    if ((release_->size > 0 && file_.size() != release_->size) ||
        (!release_->sha256.isEmpty() && hash_.result() != release_->sha256)) {
        file_.remove();
        give_up(tr("Version %1 came damaged: it is downloaded again later.").arg(version()));
        return;
    }
    install();
}

void Updater::install() {
    setState(State::Installing);
    const update::Target& target = *environment_.target;
    if (target.kind == update::Target::Kind::File) {
        if (!update::replace_file(update::to_path(file_.fileName()), target.path)) {
            file_.remove();
            refuse(tr("ratcast cannot update itself: %1 cannot be replaced. Download "
                      "version %2 yourself.")
                       .arg(QDir::toNativeSeparators(update::to_qstring(target.path)), version()));
            return;
        }
        installed();
        return;
    }
    // The zip unpacked beside it, by the tar Windows has.
    const QString files = update::to_qstring(update::staging(target.path) / "files");
    QDir().mkpath(files);
    QProcess* const unpack = new QProcess(this);
    unpack_ = unpack;
    unpack_->setProgram(QDir(qEnvironmentVariable("SystemRoot")).filePath("System32/tar.exe"));
    unpack_->setArguments({QStringLiteral("-xf"), QDir::toNativeSeparators(file_.fileName()),
                           QStringLiteral("-C"), QDir::toNativeSeparators(files)});
    unpack_->setProcessChannelMode(QProcess::MergedChannels);
    connect(unpack, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        if (code != 0 || status != QProcess::NormalExit) {
            qCWarning(update_log) << "tar:" << unpack_->readAll();
        }
        unpacked(code == 0 && status == QProcess::NormalExit);
    });
    connect(unpack, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart) unpacked(false);
    });
    unpack_->start();
    QTimer::singleShot(kUnpackTimeoutMs, unpack, [unpack] { unpack->kill(); });
}

void Updater::unpacked(bool ok) {
    unpack_->deleteLater();
    unpack_ = nullptr;
    file_.remove();
    const update::Target& target = *environment_.target;
    const update::fs::path files = update::staging(target.path) / "files";
    const std::optional<update::fs::path> root =
        ok ? update::find_root(files, target.executable.filename()) : std::nullopt;
    if (!root || !update::stage(target.path, *root)) {
        std::error_code ignored;
        update::fs::remove_all(update::staging(target.path), ignored);
        give_up(tr("Could not unpack version %1.").arg(version()));
        return;
    }
    installed();
}

void Updater::installed() {
    qCInfo(update_log) << "version" << release_->version << "installed";
    setProgress(1);
    setState(State::Installed);
}

void Updater::restart() {
    if (state_ != State::Installed) return;
    // On Windows the program there is still this one: it puts the new one in
    // place as it starts (start_updated()), and starts that.
    if (!QProcess::startDetached(update::to_qstring(environment_.target->executable), {})) {
        setNotice(tr("Could not start ratcast again: start it yourself."));
        return;
    }
    QCoreApplication::quit();
}

void Updater::give_up(const QString& message) {
    qCWarning(update_log) << message;
    if (asked_) setNotice(message);
    idle();
}

void Updater::refuse(const QString& message) {
    qCWarning(update_log) << message;
    setNotice(message);
    idle();
}

void Updater::idle() {
    setState(State::Idle);
    if (automatic_) timer_.start(environment_.every);
}

void Updater::setAutomatic(bool automatic) {
    if (automatic_ == automatic) return;
    automatic_ = automatic;
    QSettings().setValue(kAutomaticKey, automatic);
    if (!automatic) {
        timer_.stop();
    } else if (supported() && state_ == State::Idle) {
        timer_.start(environment_.first_check);
    }
    Q_EMIT automaticChanged();
}

void Updater::setQuiet(bool quiet) {
    if (quiet_ == quiet) return;
    quiet_ = quiet;
    Q_EMIT quietChanged();
    if (quiet_ && state_ == State::Downloading) {
        stopped_ = true;
        reply_->abort();  // downloaded(), at once
    } else if (!quiet_ && pending_) {
        fetch();
    }
}

QString Updater::version() const { return release_ ? release_->version.toString() : QString(); }

void Updater::setState(State state) {
    if (state_ == state) return;
    state_ = state;
    Q_EMIT stateChanged();
}

void Updater::setNotice(const QString& notice) {
    if (notice_ == notice) return;
    notice_ = notice;
    Q_EMIT noticeChanged();
}

void Updater::setProgress(double progress) {
    if (progress_ == progress) return;
    progress_ = progress;
    Q_EMIT progressChanged();
}

}  // namespace ratcast::app
