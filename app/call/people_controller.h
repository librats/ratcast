#pragma once

/**
 * @file people_controller.h
 * @brief The people one knows, as QML sees them: People (people.h) on the
 * GUI thread.
 *
 * It starts the home node as the window is made (componentComplete()) and
 * keeps it for as long as the app runs: friends asked by link or from a
 * room, the requests both ways, the groups (a CallController enters their
 * rooms), this device made another's person's or
 * another device made this person's, the person's key backed up. People's
 * events are brought onto the GUI thread. A CallController given it
 * (CallController::people) certifies its rooms as this person's.
 */

#include <QtQml/qqmlregistration.h>
#include <QObject>
#include <QQmlParserStatus>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <cstdint>
#include <memory>

#include "ratcast/client/people.h"

namespace ratcast::app {

class PeopleController : public QObject, public QQmlParserStatus {
    Q_OBJECT
    Q_INTERFACES(QQmlParserStatus)
    QML_ELEMENT
    /// The home node is up: what follows is there.
    Q_PROPERTY(bool ready READ ready NOTIFY changed)
    /// What friends call this person — the same on each of their devices.
    Q_PROPERTY(QString name READ name WRITE setName NOTIFY changed)
    /// Four characters of the person's key: what tells two of a name apart.
    Q_PROPERTY(QString tag READ tag NOTIFY changed)
    /// The person's key in hex, as members are told apart by (Member::person).
    Q_PROPERTY(QString person READ person NOTIFY changed)
    /// The person's link, the website's: to send to whoever is to ask.
    Q_PROPERTY(QString link READ link NOTIFY changed)
    /// Friends, those in a room first, then those online, each by name: a
    /// map of person (the key in hex), name, tag, online, inRoom.
    Q_PROPERTY(QVariantList friends READ friends NOTIFY changed)
    /// Groups, by name: a map of id (hex), name ("" until a member met
    /// says), tag, link, made (here: renamed here), present (how many are in
    /// its room now).
    Q_PROPERTY(QVariantList groups READ groups NOTIFY changed)
    /// Requests, the newest first: person, name, tag, incoming, declined.
    Q_PROPERTY(QVariantList requests READ requests NOTIFY changed)
    /// How many incoming requests wait for an answer.
    Q_PROPERTY(int asking READ asking NOTIFY changed)
    /// A device link made here, while it lasts; "" for none.
    Q_PROPERTY(QString deviceLink READ deviceLink NOTIFY deviceChanged)
    /// The device that asks to become this person, by name; "" for none.
    Q_PROPERTY(QString deviceAsking READ deviceAsking NOTIFY deviceChanged)
    /// Joining as another's device: waiting for their answer.
    Q_PROPERTY(bool joining READ joining NOTIFY deviceChanged)
    /// What went wrong, or what came of what was asked; "" for nothing.
    Q_PROPERTY(QString notice READ notice NOTIFY noticeChanged)
    /// Friends ringing, unanswered: each a map of id, person, name, tag, room,
    /// group (the group's name, to a group's room; else "").
    Q_PROPERTY(QVariantList rings READ rings NOTIFY ringsChanged)
    /// The friend this device rings, as one of those; empty for none.
    Q_PROPERTY(QVariantMap ringing READ ringing NOTIFY ringsChanged)

public:
    struct Environment {
        PeopleConfig people = {};
        /// What this person is called until the user says otherwise.
        QString name = {};

        /// This computer's: the user's data directory and name, the build's
        /// servers and those kept there, the host's name for the device's.
        static Environment system();
    };

    explicit PeopleController(QObject* parent = nullptr);
    ~PeopleController() override;

    /// Before componentComplete(), or in its place (tests): the home node
    /// started with it.
    void setEnvironment(Environment environment);
    void start();

    void classBegin() override {}
    void componentComplete() override;

    bool ready() const { return people_ != nullptr; }
    QString name() const;
    void setName(const QString& name);
    QString tag() const;
    QString person() const;
    QString link() const;
    QVariantList friends() const;
    QVariantList requests() const;
    QVariantList groups() const;
    int asking() const;
    QString deviceLink() const { return device_link_; }
    QString deviceAsking() const { return device_asking_; }
    bool joining() const { return joining_; }
    QString notice() const { return notice_; }
    QVariantList rings() const;
    QVariantMap ringing() const;

    /// Ring `person` (the key in hex), a friend, to the room at `room`;
    /// whether it rings (CallController::ring() makes the room).
    Q_INVOKABLE bool ring(const QString& person, const QString& room);
    Q_INVOKABLE void hangUp();
    /// Answer a friend's ring: accepted, the room's link to join (else "").
    Q_INVOKABLE QString answerRing(const QString& id, bool accept);

    /// A link the system or the user gave: whether it is a person's (a
    /// friend's or a device's) rather than a room's.
    Q_INVOKABLE static bool isPersonLink(const QString& link);
    Q_INVOKABLE static bool isDeviceLink(const QString& link);
    Q_INVOKABLE static bool isGroupLink(const QString& link);

    /// A new group of this person's named `name`: its id, to enter its room
    /// by (CallController::enter()); "" with a notice.
    Q_INVOKABLE QString makeGroup(const QString& name);
    Q_INVOKABLE void renameGroup(const QString& id, const QString& name);
    Q_INVOKABLE void leaveGroup(const QString& id);

    /// Ask to be a friend by `link`; whether it was taken (else a notice).
    Q_INVOKABLE bool add(const QString& link);
    Q_INVOKABLE void accept(const QString& person);
    Q_INVOKABLE void decline(const QString& person);
    Q_INVOKABLE void remove(const QString& person);
    Q_INVOKABLE void resetLink();
    Q_INVOKABLE void copy(const QString& text);

    /// A device link for another device to become this person by.
    Q_INVOKABLE void makeDeviceLink();
    Q_INVOKABLE void cancelDeviceLink();
    Q_INVOKABLE void allowDevice(bool allowed);
    /// Become the person whose device link this is; whether it was taken.
    Q_INVOKABLE bool joinAsDevice(const QString& link);

    /// The person's key and what they know, written to `file`; read back
    /// from one. Whether it went (else a notice).
    Q_INVOKABLE bool saveBackup(const QUrl& file);
    Q_INVOKABLE bool restoreBackup(const QUrl& file);

    /// The home node, for a CallController; null before ready.
    People* people() const { return people_.get(); }

Q_SIGNALS:
    void changed();
    void deviceChanged();
    void noticeChanged();
    /// This device became another's person's (true), or was refused.
    void joined(bool joined);
    void ringsChanged();
    /// What came of a ring, for the user to be told: declined, unanswered,
    /// missed here (CallController shows it).
    void ringNotice(const QString& notice);

private:
    void set_notice(const QString& notice);
    /// Run `task` on the GUI thread, unless the home node it came from is gone.
    void post(uint64_t generation, std::function<void()> task);

    Environment environment_;
    std::unique_ptr<People> people_;
    uint64_t generation_ = 0;
    QString device_link_;
    QString device_asking_;
    bool joining_ = false;
    QString notice_;
};

}  // namespace ratcast::app
