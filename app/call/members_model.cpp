#include "members_model.h"

#include <algorithm>
#include <utility>

namespace ratcast::app {

void MembersModel::set(std::vector<Member> members) {
    // Those gone first, from the end so that rows keep their numbers.
    for (int row = int(members_.size()); row-- > 0;) {
        const bool stays = std::ranges::any_of(
            members, [&](const Member& member) { return member.id == members_[row].id; });
        if (stays) continue;
        beginRemoveRows({}, row, row);
        members_.erase(members_.begin() + row);
        endRemoveRows();
    }
    // What stays is in the same order as what comes: walk both, adding the
    // new and changing the changed.
    for (int row = 0; row < int(members.size()); ++row) {
        Member& member = members[row];
        if (row < int(members_.size()) && members_[row].id == member.id) {
            if (members_[row] == member) continue;
            members_[row] = std::move(member);
            Q_EMIT dataChanged(index(row), index(row));
            continue;
        }
        beginInsertRows({}, row, row);
        members_.insert(members_.begin() + row, std::move(member));
        endInsertRows();
    }
}

QVariantMap MembersModel::get(int row) const {
    QVariantMap map;
    if (row < 0 || row >= rowCount()) return map;
    const QHash<int, QByteArray> roles = roleNames();
    for (auto it = roles.begin(); it != roles.end(); ++it) {
        map.insert(QString::fromUtf8(it.value()), data(index(row), it.key()));
    }
    return map;
}

int MembersModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : int(members_.size());
}

QVariant MembersModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() >= rowCount()) return {};
    const Member& member = members_[size_t(index.row())];
    switch (role) {
        case IdRole: return member.id;
        case PeerRole: return member.peer;
        case NameRole: return member.name;
        case MutedRole: return member.muted;
        case DeafenedRole: return member.deafened;
        case SpeakingRole: return member.speaking;
        case CameraRole: return member.camera;
        case CameraShownRole: return member.camera_shown;
        case SharingRole: return member.sharing;
        case ReconnectingRole: return member.reconnecting;
        case VolumeRole: return member.volume;
        case MutedHereRole: return member.muted_here;
        default: return {};
    }
}

QHash<int, QByteArray> MembersModel::roleNames() const {
    return {
        {IdRole, "peerId"},         {PeerRole, "peer"},
        {NameRole, "name"},         {MutedRole, "muted"},
        {DeafenedRole, "deafened"}, {SpeakingRole, "speaking"},
        {CameraRole, "camera"},     {CameraShownRole, "cameraShown"},
        {SharingRole, "sharing"},   {ReconnectingRole, "reconnecting"},
        {VolumeRole, "volume"},     {MutedHereRole, "mutedHere"},
    };
}

}  // namespace ratcast::app
