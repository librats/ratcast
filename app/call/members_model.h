#pragma once

/**
 * @file members_model.h
 * @brief The others in a room, as a list QML shows: one row a member.
 *
 * Rows change in place as the members do — who speaks changes a few times a
 * second — so that what QML made for a member (its row, its camera's view)
 * stays as long as the member does. set() takes the members as they are now,
 * by id, and tells the views only what changed. GUI thread only.
 */

#include <QAbstractListModel>
#include <QString>
#include <QVariantMap>

#include <vector>

namespace ratcast::app {

struct Member {
    QString id;    ///< the peer id, in full: what calls name it by (role `peerId`)
    QString peer;  ///< a short one, to tell members of one name apart
    QString name;  ///< what it calls itself; "" until it says
    bool muted = false;
    bool deafened = false;
    bool speaking = false;
    bool camera = false;  ///< its camera on
    /// Its camera's pictures received here: on, and among the last to speak
    /// where only so many are (role `cameraShown`; CallPlayback::most_cameras).
    bool camera_shown = false;
    bool sharing = false;  ///< a screen or a window shared
    /// Its link cut — lost and dialed again, or gone quiet — and maybe back in
    /// a moment (Call::interrupted()).
    bool reconnecting = false;
    double volume = 1.0;      ///< how loud it is heard here: 1 as it comes
    bool muted_here = false;  ///< not heard here, whatever its volume (role `mutedHere`)

    friend bool operator==(const Member&, const Member&) = default;
};

class MembersModel : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        IdRole = Qt::UserRole + 1,
        PeerRole,
        NameRole,
        MutedRole,
        DeafenedRole,
        SpeakingRole,
        CameraRole,
        CameraShownRole,
        SharingRole,
        ReconnectingRole,
        VolumeRole,
        MutedHereRole,
    };

    using QAbstractListModel::QAbstractListModel;

    /// The members now, ordered by id.
    void set(std::vector<Member> members);
    const std::vector<Member>& members() const { return members_; }

    /// Row `row` as a map of its roles; empty past the end.
    Q_INVOKABLE QVariantMap get(int row) const;

    int rowCount(const QModelIndex& parent = {}) const override;
    QVariant data(const QModelIndex& index, int role) const override;
    QHash<int, QByteArray> roleNames() const override;

private:
    std::vector<Member> members_;
};

}  // namespace ratcast::app
