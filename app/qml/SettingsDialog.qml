import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Call

// The settings, a section at a time: its name in the list on the left, its
// page on the right. A section more is a name in `sections` and a page in
// the StackLayout, in the same place; narrow, the names are tabs along the
// top. What is set takes at once — a device
// chosen in a room too — and is kept: the devices by the controller, the
// rest by Main.qml's Settings.
Dialog {
    id: dialog

    required property CallController call
    /// Whether the desktop shows icons in a tray, for the window to go to,
    /// and tells a window it was minimized.
    required property bool trayAvailable
    required property bool minimizedKnown
    /// The window goes to the tray as it is minimized, or closed.
    property bool minimizeToTray: false
    property bool closeToTray: false

    // The window's last: Android has no tray, and no section for it.
    readonly property var sections: Qt.platform.os === "android"
                                    ? [qsTr("Devices"), qsTr("Network")]
                                    : [qsTr("Devices"), qsTr("Network"), qsTr("Window")]
    /// The section shown.
    property int section: 0
    readonly property bool narrow: width < 520

    title: qsTr("Settings")
    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent.width - 48, 640)
    height: Math.min(parent.height - 48, 520)
    standardButtons: Dialog.Close

    // What is plugged in now: looked at as the dialog opens.
    onAboutToShow: call.listDevices()

    /// A device to choose: the system's default, or one of `devices` (maps of
    /// id, name and, for the one chosen when it is gone, missing).
    component DeviceChoice: ColumnLayout {
        id: choice

        required property string label
        required property var devices
        required property string chosen
        property string defaultName: qsTr("System default")

        signal picked(string id)

        Layout.fillWidth: true
        spacing: 4

        Label {
            text: choice.label
            font.bold: true
        }
        ComboBox {
            readonly property var entries: [{ id: "", name: choice.defaultName }].concat(
                choice.devices.map(device => ({
                    id: device.id,
                    name: device.missing ? qsTr("%1 (not connected)").arg(device.name)
                                         : device.name
                })))

            Layout.fillWidth: true
            model: entries
            textRole: "name"
            valueRole: "id"
            currentIndex: Math.max(0, entries.findIndex(entry => entry.id === choice.chosen))
            onActivated: choice.picked(currentValue)
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 8

        TabBar {
            Layout.fillWidth: true
            visible: dialog.narrow
            currentIndex: dialog.section

            Repeater {
                model: dialog.sections

                TabButton {
                    required property string modelData
                    required property int index
                    text: modelData
                    onClicked: dialog.section = index
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            ListView {
                Layout.preferredWidth: 140
                Layout.fillHeight: true
                visible: !dialog.narrow
                model: dialog.sections
                currentIndex: dialog.section
                clip: true

                delegate: ItemDelegate {
                    required property string modelData
                    required property int index
                    width: ListView.view.width
                    text: modelData
                    highlighted: ListView.isCurrentItem
                    onClicked: dialog.section = index
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: dialog.section

                // ── Devices ─────────────────────────────────────────────────────
                ColumnLayout {
                    spacing: 16

                    DeviceChoice {
                        label: qsTr("Microphone")
                        devices: dialog.call.microphoneDevices
                        chosen: dialog.call.microphoneDevice
                        onPicked: id => dialog.call.microphoneDevice = id
                    }
                    DeviceChoice {
                        label: qsTr("Speaker")
                        devices: dialog.call.speakerDevices
                        chosen: dialog.call.speakerDevice
                        onPicked: id => dialog.call.speakerDevice = id
                    }
                    // None on Android yet (docs/PLAN.md, 13.4).
                    DeviceChoice {
                        visible: Qt.platform.os !== "android"
                        label: qsTr("Camera")
                        devices: dialog.call.cameraDevices
                        chosen: dialog.call.cameraDevice
                        defaultName: qsTr("The first there is")
                        onPicked: id => dialog.call.cameraDevice = id
                    }
                    RowLayout {
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.6
                            font.pixelSize: 12
                            text: qsTr("The system's default follows the device the system is set to.")
                        }
                        Button {
                            text: qsTr("Look again")
                            flat: true
                            onClicked: dialog.call.listDevices()
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("For a device plugged in just now")
                        }
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                }

                // ── Network ─────────────────────────────────────────────────────
                ColumnLayout {
                    spacing: 4

                    Label {
                        text: qsTr("Server")
                        font.bold: true
                    }
                    TextField {
                        Layout.fillWidth: true
                        placeholderText: qsTr("None: HOST or HOST:PORT")
                        text: dialog.call.server
                        onTextEdited: dialog.call.server = text
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.6
                        font.pixelSize: 12
                        text: qsTr("A ratcast-server that rooms meet at: they get through any NAT, " +
                                   "carried by it where no direct path does. Taken as a room is " +
                                   "made or joined.")
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                }

                // ── Window ──────────────────────────────────────────────────────
                ColumnLayout {
                    spacing: 4

                    CheckBox {
                        text: qsTr("Minimize to the tray")
                        enabled: dialog.trayAvailable && dialog.minimizedKnown
                        checked: dialog.minimizeToTray
                        onToggled: dialog.minimizeToTray = checked
                    }
                    CheckBox {
                        text: qsTr("Close to the tray")
                        enabled: dialog.trayAvailable
                        checked: dialog.closeToTray
                        onToggled: dialog.closeToTray = checked
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.6
                        font.pixelSize: 12
                        text: !dialog.trayAvailable
                              ? qsTr("This desktop shows no tray icons.")
                              : qsTr("The window goes, the room stays: its icon in the tray brings " +
                                     "it back, or quits.") + (dialog.minimizedKnown ? "" : " " +
                                qsTr("Minimized, it stays where it is: Wayland does not tell " +
                                     "applications."))
                    }
                    Item {
                        Layout.fillHeight: true
                    }
                }
            }
        }
    }
}
