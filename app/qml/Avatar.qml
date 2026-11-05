import QtQuick
import QtQuick.Controls.Material

// A person's first letter on a colour of their tag, the same everywhere — or
// a group's, square-ish — with a dot for whether they are there.
Rectangle {
    id: avatar

    property string name
    property string tag
    /// A group's: its corners round, not the whole.
    property bool square: false
    /// A dot: green when online, the accent's when in a room.
    property bool shows: false
    property bool online: false
    property bool inRoom: false

    readonly property var colors: ["#5865f2", "#3ba55c", "#faa61a", "#ed4245", "#eb459e",
                                   "#00a8fc", "#9b59b6", "#1abc9c"]

    implicitWidth: 32
    implicitHeight: 32
    radius: square ? 8 : width / 2
    color: {
        let sum = 0
        for (let i = 0; i < tag.length; ++i)
            sum += tag.charCodeAt(i)
        return colors[sum % colors.length]
    }

    Label {
        anchors.centerIn: parent
        text: avatar.name.length > 0 ? avatar.name.charAt(0).toUpperCase() : "?"
        font.bold: true
        textFormat: Text.PlainText
    }
    Rectangle {
        visible: avatar.shows && avatar.online
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: -1
        width: 11
        height: 11
        radius: width / 2
        color: avatar.inRoom ? Material.accent : "#3ba55c"
        border.width: 2
        border.color: Material.background
    }
}
