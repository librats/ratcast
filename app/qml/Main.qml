import QtCore
import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Call
import Ratcast.Video

// One window, three faces: the start (a name, a room to make or a link to
// join), joining, and the room. CallController says which. In the room, the
// members down the side — who speaks, who is muted or deafened, whose camera
// and share are on — the share watched in the middle with the cameras beside
// it, and the controls along the bottom: microphone, sound, camera, sharing,
// how the call goes (a panel on a button or Ctrl+I, its quality a dot), leave.
ApplicationWindow {
    id: root

    required property string ratcastVersion
    required property string libratsVersion
    /// From the command line: join it at once.
    required property string link
    /// From the command line: print how a call goes every so many seconds.
    required property int statsEvery

    readonly property bool inRoom: call.state === CallController.InRoom
    readonly property bool watching: call.watching.length > 0
    /// Cameras shown: this side's, if on, and the others'.
    readonly property int cameraTiles: (call.camera ? 1 : 0) + call.cameras

    width: 1100
    height: 680
    visible: true
    title: "ratcast"
    color: "#1e1f22"
    Material.theme: Material.Dark
    Material.accent: Material.Teal

    CallController {
        id: call
        video: picture
        preview: ownCamera
        statsEvery: root.statsEvery
    }

    Component.onCompleted: if (root.link.length > 0) call.join(root.link)

    // What is kept from one run to the next.
    Settings {
        property alias name: call.name
    }

    /// What a member is called: its own name, or, until it says one, a
    /// stranger with its short id.
    function memberName(name, peer) {
        return name.length > 0 ? name : qsTr("Someone %1").arg(peer)
    }

    /// A quality as a colour: "good", "fair", "poor", or none yet.
    function qualityColor(quality) {
        switch (quality) {
        case "good": return Material.color(Material.Green)
        case "fair": return Material.color(Material.Amber)
        case "poor": return Material.color(Material.Red)
        default: return "#4e5058"
        }
    }

    component Dot: Rectangle {
        implicitWidth: 10
        implicitHeight: 10
        radius: 5
    }

    // A name others chose: always plain text, never markup.
    component PlainLabel: Label {
        textFormat: Text.PlainText
        elide: Text.ElideRight
    }

    component Badge: Label {
        font.pixelSize: 11
        opacity: 0.7
    }

    // ── The start ───────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 520)
        spacing: 16
        visible: call.state === CallController.Idle

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "ratcast"
            font.pixelSize: 32
            font.bold: true
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            text: "v" + root.ratcastVersion + "  ·  librats " + root.libratsVersion
            opacity: 0.6
        }

        TextField {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 280
            placeholderText: qsTr("Your name, as the others see it")
            text: call.name
            maximumLength: 32
            onTextEdited: call.name = text
        }

        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Make a room")
            highlighted: true
            onClicked: call.create()
        }

        RowLayout {
            Layout.fillWidth: true

            TextField {
                id: linkField
                Layout.fillWidth: true
                placeholderText: qsTr("or a rats:// link to a room")
                onAccepted: if (text.length > 0) call.join(text)
            }
            Button {
                text: qsTr("Join")
                enabled: linkField.text.length > 0
                onClicked: call.join(linkField.text)
            }
        }

        Label {
            Layout.fillWidth: true
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            text: call.notice
            color: Material.color(Material.Orange)
            visible: text.length > 0
        }
    }

    // ── Joining ─────────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.centerIn: parent
        spacing: 12
        visible: call.state === CallController.Joining

        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: parent.visible
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Joining the room…")
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Cancel")
            flat: true
            onClicked: call.leave()
        }
    }

    // ── The room ────────────────────────────────────────────────────────────

    // Who is in it, and how to bring others.
    Pane {
        id: side
        anchors.left: parent.left
        anchors.top: parent.top
        anchors.bottom: controls.top
        width: 280
        visible: root.inRoom
        Material.elevation: 2

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                text: qsTr("Room")
                font.pixelSize: 20
                font.bold: true
            }
            Label {
                text: qsTr("Send a link to whoever is to join:")
                opacity: 0.7
                font.pixelSize: 12
            }
            Repeater {
                model: call.invites

                RowLayout {
                    required property string modelData
                    Layout.fillWidth: true

                    TextField {
                        Layout.fillWidth: true
                        readOnly: true
                        text: modelData
                        selectByMouse: true
                        font.pixelSize: 11
                    }
                    Button {
                        text: qsTr("Copy")
                        flat: true
                        onClicked: call.copy(modelData)
                    }
                }
            }

            Label {
                Layout.topMargin: 8
                text: qsTr("In the room")
                font.bold: true
            }

            // This side first.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Dot {
                    color: call.speaking ? Material.color(Material.Green) : "#4e5058"
                }
                PlainLabel {
                    Layout.fillWidth: true
                    text: qsTr("%1 (you)").arg(root.memberName(call.name, ""))
                }
                Badge {
                    text: call.deafened ? qsTr("deafened")
                                        : call.muted ? qsTr("muted") : ""
                }
                Badge {
                    text: call.sharing ? qsTr("sharing") : ""
                }
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: call.members

                delegate: RowLayout {
                    id: member
                    required property string peerId
                    required property string peer
                    required property string name
                    required property bool muted
                    required property bool deafened
                    required property bool speaking
                    required property bool sharing
                    width: ListView.view.width
                    spacing: 8

                    Dot {
                        color: member.speaking ? Material.color(Material.Green) : "#4e5058"
                    }
                    PlainLabel {
                        Layout.fillWidth: true
                        text: root.memberName(member.name, member.peer)
                    }
                    Badge {
                        text: member.deafened ? qsTr("deafened")
                                              : member.muted ? qsTr("muted") : ""
                    }
                    Button {
                        visible: member.sharing
                        flat: true
                        font.pixelSize: 11
                        text: call.watching === member.peerId ? qsTr("Watching") : qsTr("Watch")
                        enabled: call.watching !== member.peerId
                        onClicked: call.watch(member.peerId)
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: call.notice
                color: Material.color(Material.Orange)
                visible: text.length > 0
            }
        }
    }

    // The share watched, and the cameras: beside it, or alone in the middle.
    Item {
        id: stage
        anchors.left: side.right
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: controls.top
        anchors.margins: 12
        visible: root.inRoom

        VideoView {
            id: picture
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.bottom: parent.bottom
            anchors.right: root.watching && root.cameraTiles > 0 ? cameras.left : parent.right
            anchors.rightMargin: root.watching && root.cameraTiles > 0 ? 12 : 0
            visible: root.watching
        }

        RowLayout {
            anchors.top: parent.top
            anchors.horizontalCenter: picture.horizontalCenter
            visible: root.watching

            Button {
                text: qsTr("Stop watching")
                onClicked: call.watch("")
            }
        }

        Label {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 420)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            opacity: 0.6
            text: qsTr("Everyone here hears everyone. Share a screen or turn a camera on, " +
                       "and the others see it.")
            visible: !root.watching && root.cameraTiles === 0
        }

        component CameraFrame: Rectangle {
            id: frame
            default property alias content: holder.data
            property string caption
            color: "#111214"
            radius: 6
            border.color: "#4e5058"
            clip: true

            Item {
                id: holder
                anchors.fill: parent
                anchors.margins: 2
            }
            Label {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 6
                text: frame.caption
                textFormat: Text.PlainText
                font.pixelSize: 11
                style: Text.Outline
                styleColor: "#000000"
            }
        }

        // Beside the share a column of small ones; without one, a grid.
        Flow {
            id: cameras
            readonly property real tileWidth: root.watching ? 240 : 360
            anchors.right: parent.right
            anchors.top: parent.top
            width: root.watching ? tileWidth : parent.width
            spacing: 8

            // A tile a member, shown while its camera is on: the view stays
            // as long as the member does.
            Repeater {
                model: call.members

                CameraFrame {
                    id: tile
                    required property string peerId
                    required property string peer
                    required property string name
                    required property bool camera
                    width: cameras.tileWidth
                    height: width * 3 / 4
                    visible: camera
                    caption: root.memberName(name, peer)

                    VideoView {
                        id: theirCamera
                        anchors.fill: parent
                    }
                    Component.onCompleted: call.setCameraView(tile.peerId, theirCamera)
                    Component.onDestruction: call.setCameraView(tile.peerId, null)
                }
            }

            CameraFrame {
                width: cameras.tileWidth
                height: width * 3 / 4
                visible: call.camera
                caption: qsTr("You")

                // As a mirror shows one: what is left of the user is on the left.
                VideoView {
                    id: ownCamera
                    anchors.fill: parent
                    transform: Scale {
                        origin.x: ownCamera.width / 2
                        xScale: -1
                    }
                }
            }
        }
    }

    // How the call goes: a section a part, each with its quality and what was
    // measured.
    Rectangle {
        id: stats

        property bool shown: false

        anchors.left: stage.left
        anchors.top: stage.top
        anchors.margins: 8
        width: Math.min(420, stage.width - 16)
        height: Math.min(statsColumn.implicitHeight + 24, stage.height - 16)
        visible: root.inRoom && shown
        color: "#e6111214"
        radius: 6
        border.color: "#4e5058"

        ScrollView {
            anchors.fill: parent
            anchors.margins: 12
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                id: statsColumn
                width: parent.width
                spacing: 10

                Label {
                    text: qsTr("Measuring…")
                    opacity: 0.7
                    visible: call.stats.length === 0
                }

                Repeater {
                    model: call.stats

                    ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            spacing: 6
                            Dot {
                                color: root.qualityColor(modelData.quality)
                            }
                            Label {
                                text: modelData.title
                                font.bold: true
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.reason
                            wrapMode: Text.Wrap
                            color: root.qualityColor(modelData.quality)
                            visible: text.length > 0
                        }
                        Repeater {
                            model: modelData.rows

                            RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 12

                                Label {
                                    Layout.preferredWidth: 140
                                    text: modelData.label
                                    opacity: 0.7
                                    font.pixelSize: 12
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.value
                                    font.family: "monospace"
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Shortcut {
        sequence: "Ctrl+I"
        enabled: root.inRoom
        onActivated: stats.shown = !stats.shown
    }

    // ── The controls ────────────────────────────────────────────────────────

    Pane {
        id: controls
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.inRoom
        Material.elevation: 4

        RowLayout {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 8

            Button {
                text: call.muted ? qsTr("Unmute") : qsTr("Mute")
                checkable: true
                checked: call.muted
                onToggled: call.muted = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("The others do not hear you")
            }
            Button {
                text: call.deafened ? qsTr("Undeafen") : qsTr("Deafen")
                checkable: true
                checked: call.deafened
                onToggled: call.deafened = checked
                ToolTip.visible: hovered
                ToolTip.text: qsTr("You hear nobody, and nobody hears you")
            }
            Button {
                text: call.camera ? qsTr("Camera off") : qsTr("Camera")
                checkable: true
                checked: call.camera
                onToggled: call.camera = checked
            }
            Button {
                id: shareButton
                text: call.sharing ? qsTr("Stop sharing")
                                   : call.choosing ? qsTr("Choosing…") : qsTr("Share")
                highlighted: call.sharing
                onClicked: call.sharing || call.choosing ? call.stopSharing() : shareMenu.open()

                Menu {
                    id: shareMenu
                    y: -height

                    MenuItem {
                        text: qsTr("A screen")
                        onTriggered: call.share(false, withSound.checked)
                    }
                    MenuItem {
                        text: qsTr("A window")
                        onTriggered: call.share(true, withSound.checked)
                    }
                    MenuSeparator {}
                    MenuItem {
                        id: withSound
                        text: qsTr("With its sound")
                        checkable: true
                        checked: true
                    }
                }
            }
            RowLayout {
                Layout.leftMargin: 8
                spacing: 6

                // The call's quality, panel shown or not; why, on hovering.
                Dot {
                    color: root.qualityColor(call.quality)

                    HoverHandler {
                        id: qualityHover
                    }
                    ToolTip.visible: qualityHover.hovered && call.quality.length > 0
                    ToolTip.text: call.qualityReason.length > 0
                                  ? call.quality + ": " + call.qualityReason : call.quality
                }
                Button {
                    text: qsTr("Stats")
                    checkable: true
                    checked: stats.shown
                    onToggled: stats.shown = checked
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("How the call goes (Ctrl+I)")
                }
            }
            Button {
                Layout.leftMargin: 8
                text: qsTr("Leave")
                Material.background: Material.color(Material.Red, Material.Shade700)
                onClicked: call.leave()
            }
        }
    }
}
