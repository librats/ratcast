import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Call
import Ratcast.Video

// One window, three faces: the start (share, or join from a link), the
// sharer's (who is watching, the links to send) and the viewer's (the
// picture). CallController says which.
ApplicationWindow {
    id: root

    required property string ratcastVersion
    required property string libratsVersion
    /// From the command line: watch it at once.
    required property string link

    readonly property bool watching: call.state === CallController.Joining
                                     || call.state === CallController.Watching

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

        Button {
            Layout.topMargin: 12
            text: qsTr("Stop sharing")
            onClicked: call.leave()
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

    Button {
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 16
        visible: root.watching
        text: qsTr("Leave")
        onClicked: call.leave()
    }
}
