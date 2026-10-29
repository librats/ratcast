#pragma once

/**
 * @file updater.h
 * @brief The app keeping itself at the latest release on GitHub.
 *
 *   Idle ── check() ──▶ Checking ─(newer)─▶ Downloading ─▶ Installing ─▶ Installed
 *     ▲                    │                    │              │
 *     └────────────────────┴── nothing newer, or it failed ────┘
 *
 * Automatic (the default, kept from one run to the next), it checks a little
 * after the start and every few hours: GitHub's latest release, its version
 * against this one's, and the package of it for this system (release.h). A
 * newer one is downloaded, checked against the size and SHA-256 GitHub says,
 * and put in place (install.h): it is what starts next time, or at once with
 * restart(). Prereleases are never taken.
 *
 * Quiet — while a call is on, whose streams need the line — nothing is
 * downloaded: a download under way stops, and starts again once it is not.
 *
 * Only a release package updates itself (RATCAST_UPDATES, and on Linux only
 * from its AppImage, on macOS only from its bundle on Apple Silicon):
 * elsewhere `supported` is false and nothing happens. A
 * check that fails is only said when the user asked for it; an update that
 * cannot be put in place — a folder that cannot be written to, say — always
 * is, with the release's page to fetch it from by hand. GUI thread only.
 */

#include <QtQml/qqmlregistration.h>
#include <QCryptographicHash>
#include <QFile>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QTimer>
#include <QUrl>
#include <QVersionNumber>

#include <chrono>
#include <optional>

#include "install.h"
#include "release.h"

class QNetworkAccessManager;
class QNetworkReply;
class QProcess;

namespace ratcast::app {

class Updater : public QObject {
    Q_OBJECT
    QML_ELEMENT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
    /// This package can update itself.
    Q_PROPERTY(bool supported READ supported CONSTANT)
    /// Check, download and install without being asked; kept.
    Q_PROPERTY(bool automatic READ automatic WRITE setAutomatic NOTIFY automaticChanged)
    /// Download nothing now: a call is on.
    Q_PROPERTY(bool quiet READ quiet WRITE setQuiet NOTIFY quietChanged)
    /// The newer version found: being downloaded, installed, or in place; ""
    /// for none.
    Q_PROPERTY(QString version READ version NOTIFY versionChanged)
    /// Of the download, 0 to 1.
    Q_PROPERTY(double progress READ progress NOTIFY progressChanged)
    /// What went wrong, or, asked, that there is nothing newer; "" for none.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    /// The newer release's page, to download it by hand; empty for none.
    Q_PROPERTY(QUrl page READ page NOTIFY versionChanged)

public:
    enum class State { Idle, Checking, Downloading, Installing, Installed };
    Q_ENUM(State)

    /// What the updater takes from the system; tests stand in for it.
    struct Environment {
        /// What is updated; none: nothing is.
        std::optional<update::Target> target;
        /// GitHub's latest release of the repository (its API).
        QUrl latest;
        /// This version.
        QVersionNumber current;
        /// When the first automatic check comes, and how often the next.
        std::chrono::milliseconds first_check{std::chrono::seconds(10)};
        std::chrono::milliseconds every{std::chrono::hours(6)};
    };
    /// This process's: RATCAST_UPDATE_REPOSITORY's releases, this version.
    static Environment system();

    explicit Updater(QObject* parent = nullptr);
    explicit Updater(Environment environment, QObject* parent = nullptr);
    ~Updater() override;

    /// Look for a newer version now, and install it if there is one.
    Q_INVOKABLE void check();
    /// Once Installed: the new version started in this one's place, and this
    /// one quits.
    Q_INVOKABLE void restart();

    State state() const { return state_; }
    bool supported() const { return environment_.target.has_value(); }
    bool automatic() const { return automatic_; }
    void setAutomatic(bool automatic);
    bool quiet() const { return quiet_; }
    void setQuiet(bool quiet);
    QString version() const;
    double progress() const { return progress_; }
    QString notice() const { return notice_; }
    QUrl page() const { return release_ ? release_->page : QUrl(); }

Q_SIGNALS:
    void stateChanged();
    void automaticChanged();
    void quietChanged();
    void versionChanged();
    void progressChanged();
    void noticeChanged();

private:
    void begin(bool asked);
    void checked();
    void fetch();
    void received();
    void downloaded();
    void install();
    void unpacked(bool ok);
    void installed();
    /// Back to Idle, the next check scheduled; `message` said if it was asked.
    void give_up(const QString& message);
    /// Back to Idle with `message` said, asked or not.
    void refuse(const QString& message);
    void idle();
    void setState(State state);
    void setNotice(const QString& notice);
    void setProgress(double progress);

    const Environment environment_;
    QNetworkAccessManager* const network_;
    QTimer timer_;  ///< the next automatic check

    State state_ = State::Idle;
    bool automatic_ = true;
    bool quiet_ = false;
    bool asked_ = false;    ///< the check under way is the user's
    bool pending_ = false;  ///< release_ to download once not quiet
    bool stopped_ = false;  ///< the download stopped for quiet
    bool write_failed_ = false;
    double progress_ = 0;
    QString notice_;
    std::optional<update::Release> release_;  ///< the newer one

    QPointer<QNetworkReply> reply_;
    QPointer<QProcess> unpack_;
    QFile file_;  ///< what is downloaded into
    QCryptographicHash hash_{QCryptographicHash::Sha256};
};

}  // namespace ratcast::app
