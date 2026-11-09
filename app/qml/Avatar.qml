import QtQuick
import QtQuick.Controls.Material

// A person's first letter on a colour of their tag, the same everywhere — or
// a group's, square-ish — with a dot for whether they are there, and in a room
// a ring around it: green and glowing while they are heard, orange while
// their connection comes back. The ring lies outside its size: 6 px to leave.
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
    property bool speaking: false
    property bool reconnecting: false

    readonly property var colors: ["#5865f2", "#3ba55c", "#faa61a", "#ed4245", "#eb459e",
                                   "#00a8fc", "#9b59b6", "#1abc9c"]

    readonly property bool ringed: speaking || reconnecting
    readonly property color ringColor: reconnecting ? "#faa61a" : "#2fd573"
    // Quick to light, as a voice starts; slower to go, over a word's gap.
    property real ringOpacity: ringed ? 1 : 0
    Behavior on ringOpacity {
        NumberAnimation {
            duration: avatar.ringed ? 90 : 280
            easing.type: Easing.OutQuad
        }
    }
    // How strongly the glow breathes around the ring.
    property real glowing: 0.55
    SequentialAnimation on glowing {
        running: avatar.speaking && !avatar.reconnecting && avatar.visible
        loops: Animation.Infinite
        NumberAnimation {
            from: 0.55
            to: 0.2
            duration: 650
            easing.type: Easing.InOutSine
        }
        NumberAnimation {
            from: 0.2
            to: 0.55
            duration: 650
            easing.type: Easing.InOutSine
        }
    }

    implicitWidth: 32
    implicitHeight: 32
    radius: square ? 8 : width / 2
    color: {
        let sum = 0
        for (let i = 0; i < tag.length; ++i)
            sum += tag.charCodeAt(i)
        return colors[sum % colors.length]
    }

    Rectangle {
        anchors.fill: parent
        anchors.margins: -6
        radius: avatar.radius + 6
        color: "transparent"
        border.width: 3
        border.color: avatar.ringColor
        opacity: avatar.ringOpacity * (avatar.reconnecting ? 0 : avatar.glowing)
        visible: opacity > 0
    }
    // A pixel off the picture, as Discord's.
    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        radius: avatar.radius + 3
        color: "transparent"
        border.width: 2
        border.color: avatar.ringColor
        opacity: avatar.ringOpacity
        visible: opacity > 0
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
