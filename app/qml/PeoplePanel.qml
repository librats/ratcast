import QtQuick
import QtQuick.Layouts
import Ratcast.Call

// Beside the start, or in its drawer when narrow: the groups, then the
// friends.
ColumnLayout {
    id: panel

    required property PeopleController people
    required property CallController call

    spacing: 24

    GroupsPanel {
        Layout.fillWidth: true
        people: panel.people
        call: panel.call
    }
    FriendsPanel {
        Layout.fillWidth: true
        people: panel.people
        call: panel.call
    }
}
