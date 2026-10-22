import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Call
import Ratcast.Video

// One window, three faces: the start (share, or join from a link), the
// sharer's (who is watching, the links to send) and the viewer's (the
// picture). CallController says which. In a call, the cameras over it: the
// other side's, and this side's own; and, on a button or Ctrl+I, how the
// call goes, with its quality as a dot beside the button.
ApplicationWindow {
    id: root

    required property string ratcastVersion
    required property string libratsVersion
    /// From the command line: watch it at once.
    required property string link
    /// From the command line: print how a call goes every so many seconds.
    required property int statsEvery

    readonly property bool watching: call.state === CallController.Joining
                                     || call.state === CallController.Watching
    readonly property bool inCall: watching || call.state === CallController.Sharing

    width: 960
    height: 600
    visible: true
    title: "ratcast"
    color: "#1e1f22"
    Material.theme: Material.Dark
    Material.accent: Material.Teal

    CallController {
        id: call
        video: picture
        cameraView: theirCamera
        preview: ownCamera
        statsEvery: root.statsEvery
    }

    Component.onCompleted: if (root.link.length > 0) call.join(root.link)

    VideoView {
        id: picture
        anchors.fill: parent
        visible: root.watching
    }

    // ── The start ───────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 520)
        spacing: 16
        visible: call.state === CallController.Idle || call.state === CallController.Choosing

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

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            enabled: call.state === CallController.Idle

            Button {
                text: qsTr("Share a screen")
                highlighted: true
                onClicked: call.share(false, withSound.checked)
            }
            Button {
                text: qsTr("Share a window")
                onClicked: call.share(true, withSound.checked)
            }
            CheckBox {
                id: withSound
                text: qsTr("With its sound")
                checked: true
            }
        }

        RowLayout {
            Layout.fillWidth: true
            enabled: call.state === CallController.Idle

            TextField {
                id: linkField
                Layout.fillWidth: true
                placeholderText: qsTr("rats:// link to watch")
                onAccepted: if (text.length > 0) call.join(text)
            }
            Button {
                text: qsTr("Join")
                enabled: linkField.text.length > 0
                onClicked: call.join(linkField.text)
            }
        }

        RowLayout {
            Layout.alignment: Qt.AlignHCenter
            visible: call.state === CallController.Choosing

            BusyIndicator {
                running: parent.visible
            }
            Label {
                text: qsTr("Choose what to share in the desktop's dialog…")
            }
            Button {
                text: qsTr("Cancel")
                flat: true
                onClicked: call.leave()
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

    // ── Sharing ─────────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 640)
        spacing: 12
        visible: call.state === CallController.Sharing

        Label {
            text: qsTr("Sharing")
            font.pixelSize: 24
            font.bold: true
        }

        Label {
            text: call.viewers === 0 ? qsTr("Nobody is watching yet.")
                                     : qsTr("%n watching.", "", call.viewers)
            opacity: 0.8
        }

        Label {
            Layout.topMargin: 12
            text: qsTr("Send one of these links to whoever is to watch:")
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
                }
                Button {
                    text: qsTr("Copy")
                    onClicked: call.copy(modelData)
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

    // ── Watching ────────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.centerIn: parent
        visible: call.state === CallController.Joining

        BusyIndicator {
            Layout.alignment: Qt.AlignHCenter
            running: parent.visible
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: call.notice.length > 0 ? call.notice : qsTr("Connecting…")
        }
    }

    // ── In a call, either side ──────────────────────────────────────────────

    component CameraFrame: Rectangle {
        default property alias content: holder.data
        color: "#111214"
        radius: 6
        border.color: "#4e5058"
        clip: true

        Item {
            id: holder
            anchors.fill: parent
            anchors.margins: 2
        }
    }

    ColumnLayout {
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 16
        spacing: 8
        visible: root.inCall

        CameraFrame {
            Layout.preferredWidth: 240
            Layout.preferredHeight: 180
            Layout.alignment: Qt.AlignRight
            visible: call.cameraShown

            VideoView {
                id: theirCamera
                anchors.fill: parent
            }
        }
        CameraFrame {
            Layout.preferredWidth: 160
            Layout.preferredHeight: 120
            Layout.alignment: Qt.AlignRight
            visible: call.camera

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

    // How the call goes: a section a part, each with its quality and what was
    // measured.
    Rectangle {
        id: stats

        property bool shown: false

        anchors.left: parent.left
        anchors.top: parent.top
        anchors.margins: 16
        width: Math.min(420, parent.width - 32)
        height: Math.min(statsColumn.implicitHeight + 24, controls.y - 32)
        visible: root.inCall && shown
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
        enabled: root.inCall
        onActivated: stats.shown = !stats.shown
    }

    component Speaker: RowLayout {
        property alias text: name.text
        property bool active
        spacing: 6

        Dot {
            color: parent.active ? Material.color(Material.Green) : "#4e5058"
        }
        Label {
            id: name
        }
    }

    Pane {
        id: controls
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        visible: root.inCall
        Material.elevation: 4

        RowLayout {
            spacing: 16

            Speaker {
                text: call.muted ? qsTr("You (muted)") : qsTr("You")
                active: call.speaking
            }
            Speaker {
                text: call.state === CallController.Sharing ? qsTr("Viewers") : qsTr("Sharer")
                active: call.hearing
            }
            Button {
                text: call.muted ? qsTr("Unmute") : qsTr("Mute")
                checkable: true
                checked: call.muted
                onToggled: call.muted = checked
            }
            Button {
                text: call.camera ? qsTr("Camera off") : qsTr("Camera")
                checkable: true
                checked: call.camera
                onToggled: call.camera = checked
            }
            RowLayout {
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
                text: call.state === CallController.Sharing ? qsTr("Stop sharing")
                                                            : qsTr("Leave")
                onClicked: call.leave()
            }
        }
    }
}
