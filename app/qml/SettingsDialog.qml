import QtQuick
import QtQuick.Controls.Material
import QtQuick.Dialogs
import QtQuick.Layouts
import Ratcast.Call
import Ratcast.Language

// The settings, a section at a time: its name in the list on the left, its
// page on the right. A section more is a name in `sections` and a page in
// the StackLayout (a SettingsPage, scrolled where the dialog is too low), in
// the same place; narrow — a phone —, the names are tabs along the top and
// the dialog nearly the whole window. What is set takes at once — a device
// chosen in a room too — and is kept: the devices by the controller, the
// rest by Main.qml's Settings.
Dialog {
    id: dialog

    required property CallController call
    required property PeopleController people
    /// Whether the desktop shows icons in a tray, for the window to go to,
    /// and tells a window it was minimized.
    required property bool trayAvailable
    required property bool minimizedKnown
    /// The window goes to the tray as it is minimized, or closed.
    property bool minimizeToTray: false
    property bool closeToTray: false
    /// The small picture of one's share keeps playing with another window in use.
    property bool keepSharePreview: false

    // The window's last: Android has no tray, and no section for it.
    readonly property var sections: Qt.platform.os === "android"
                                    ? [qsTr("Devices"), qsTr("Network"), qsTr("Language"),
                                       qsTr("You")]
                                    : [qsTr("Devices"), qsTr("Network"), qsTr("Language"),
                                       qsTr("You"), qsTr("Window")]
    /// Each section's icon, of icons/.
    readonly property var sectionIcons: ["headphones", "globe", "languages", "user", "app-window"]
    /// The section shown.
    property int section: 0
    readonly property bool narrow: width < 520

    title: qsTr("Settings")
    modal: true
    anchors.centerIn: Overlay.overlay
    // A phone's: nearly all the screen.
    width: Math.min(parent.width - (parent.width < 560 ? 24 : 48), 640)
    height: narrow ? parent.height - 24 : Math.min(parent.height - 48, 520)
    // Its own button, not Dialog.Close: Qt's word for that is in Qt's
    // translations, which the app does not carry.
    footer: DialogButtonBox {
        Button {
            text: qsTr("Close")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    // What is plugged in now: looked at as the dialog opens.
    onAboutToShow: call.listDevices()

    /// A check box whose words wrap, as narrow as the page: Qt's keeps its
    /// text on one line, as wide as it is.
    component WrappingCheckBox: CheckBox {
        id: box

        Layout.fillWidth: true
        contentItem: Label {
            text: box.text
            wrapMode: Text.Wrap
            verticalAlignment: Text.AlignVCenter
            leftPadding: box.indicator && !box.mirrored ? box.indicator.width + box.spacing : 0
            rightPadding: box.indicator && box.mirrored ? box.indicator.width + box.spacing : 0
            opacity: box.enabled ? 1 : 0.5
        }
    }

    /// A section's page, the dialog's width: scrolled where it is too low.
    component SettingsPage: ScrollView {
        id: page

        default property alias content: column.data

        contentWidth: availableWidth
        clip: true

        // Its rows as far apart as the page's spacing says.
        ColumnLayout {
            id: column
            width: page.availableWidth
            spacing: page.spacing
        }
    }

    /// A section's heading: its words wrap, a page never wider for it.
    component Heading: Label {
        Layout.fillWidth: true
        wrapMode: Text.Wrap
        font.bold: true
    }

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

        Heading {
            text: choice.label
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
                    icon.source: "icons/" + dialog.sectionIcons[index] + ".svg"
                    onClicked: dialog.section = index
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 16

            ListView {
                // Wide enough for the longest name in any language
                // ("Устройства", "Périphériques").
                Layout.preferredWidth: 180
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
                    icon.source: "icons/" + dialog.sectionIcons[index] + ".svg"
                    highlighted: ListView.isCurrentItem
                    onClicked: dialog.section = index
                }
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                currentIndex: dialog.section

                // ── Devices ─────────────────────────────────────────────────────
                SettingsPage {
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
                    DeviceChoice {
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
                            icon.source: "icons/refresh-cw.svg"
                            flat: true
                            onClicked: dialog.call.listDevices()
                            ToolTip.visible: hovered
                            ToolTip.text: qsTr("For a device plugged in just now")
                        }
                    }
                }

                // ── Network ─────────────────────────────────────────────────────
                SettingsPage {
                    spacing: 4

                    Heading {
                        text: qsTr("Server")
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
                }

                // ── Language ────────────────────────────────────────────────────
                SettingsPage {
                    spacing: 4

                    Heading {
                        text: qsTr("Language")
                    }
                    ComboBox {
                        // The system's first, in the language it is now; the
                        // rest each in its own.
                        readonly property var entries: [{
                            code: "",
                            name: qsTr("The system's (%1)").arg(
                                Language.languages.find(entry => entry.code === Language.current)?.name
                                ?? "English")
                        }].concat(Language.languages)

                        Layout.fillWidth: true
                        model: entries
                        textRole: "name"
                        valueRole: "code"
                        currentIndex: Math.max(0, entries.findIndex(entry => entry.code === Language.chosen))
                        onActivated: Language.chosen = currentValue
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.6
                        font.pixelSize: 12
                        text: qsTr("Takes effect at once.")
                    }
                }

                // ── You: this person on their devices, and backed up ────────────
                ScrollView {
                    contentWidth: availableWidth
                    clip: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 4

                        Heading {
                            text: qsTr("Your name")
                        }
                        RowLayout {
                            Layout.fillWidth: true

                            TextField {
                                Layout.fillWidth: true
                                text: dialog.people.name
                                maximumLength: 32
                                onTextEdited: dialog.people.name = text
                            }
                            Label {
                                text: dialog.people.tag
                                opacity: 0.5
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.6
                            font.pixelSize: 12
                            text: qsTr("The four characters after it are yours alone: they tell two of a name apart.")
                        }

                        Heading {
                            Layout.topMargin: 12
                            text: qsTr("Your other devices")
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.6
                            font.pixelSize: 12
                            text: qsTr("A phone or another computer can be you too: the same friends, " +
                                       "the same name. Make a link here and open it there, " +
                                       "or point that phone's camera at its QR code.")
                        }
                        Button {
                            text: qsTr("Add a device")
                            icon.source: "icons/smartphone.svg"
                            visible: dialog.people.deviceLink.length === 0
                            enabled: dialog.people.ready
                            onClicked: dialog.people.makeDeviceLink()
                        }
                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 4
                            visible: dialog.people.deviceLink.length > 0

                            QrCode {
                                Layout.alignment: Qt.AlignHCenter
                                Layout.preferredWidth: 200
                                Layout.preferredHeight: 200
                                text: dialog.people.deviceLink
                            }
                            RowLayout {
                                Layout.fillWidth: true

                                TextField {
                                    Layout.fillWidth: true
                                    readOnly: true
                                    selectByMouse: true
                                    font.pixelSize: 11
                                    text: dialog.people.deviceLink
                                    onTextChanged: cursorPosition = 0
                                }
                                ToolButton {
                                    display: AbstractButton.IconOnly
                                    icon.source: "icons/copy.svg"
                                    onClicked: dialog.people.copy(dialog.people.deviceLink)
                                    ToolTip.visible: hovered
                                    ToolTip.text: qsTr("Copy the link")
                                }
                            }
                            Label {
                                Layout.fillWidth: true
                                wrapMode: Text.Wrap
                                opacity: 0.6
                                font.pixelSize: 12
                                text: qsTr("For the next ten minutes, and for one device: you are asked before it becomes you. Send it to nobody else.")
                            }
                            Button {
                                text: qsTr("Cancel")
                                flat: true
                                onClicked: dialog.people.cancelDeviceLink()
                            }
                        }

                        Heading {
                            Layout.topMargin: 12
                            text: qsTr("Make this device someone else's")
                        }
                        RowLayout {
                            Layout.fillWidth: true

                            TextField {
                                id: deviceField
                                Layout.fillWidth: true
                                placeholderText: qsTr("the link your other device made")
                            }
                            Button {
                                text: qsTr("Ask")
                                enabled: deviceField.text.length > 0 && !dialog.people.joining
                                onClicked: if (dialog.people.joinAsDevice(deviceField.text)) deviceField.text = ""
                            }
                        }

                        Heading {
                            Layout.topMargin: 12
                            text: qsTr("Backup")
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            opacity: 0.6
                            font.pixelSize: 12
                            text: qsTr("Your key and your friends in a file: with it, a new device is you again " +
                                       "even if every other is lost. Whoever has the file can be you.")
                        }
                        // One under the other when they do not fit.
                        Flow {
                            Layout.fillWidth: true

                            Button {
                                text: qsTr("Save a backup…")
                                icon.source: "icons/save.svg"
                                flat: true
                                enabled: dialog.people.ready
                                onClicked: saveBackup.open()
                            }
                            Button {
                                text: qsTr("Restore…")
                                icon.source: "icons/upload.svg"
                                flat: true
                                enabled: dialog.people.ready
                                onClicked: restoreBackup.open()
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            wrapMode: Text.Wrap
                            text: dialog.people.notice
                            color: Material.color(Material.Teal)
                            visible: text.length > 0
                        }

                        FileDialog {
                            id: saveBackup
                            fileMode: FileDialog.SaveFile
                            defaultSuffix: "txt"
                            nameFilters: [qsTr("Backups (*.txt)")]
                            onAccepted: dialog.people.saveBackup(selectedFile)
                        }
                        FileDialog {
                            id: restoreBackup
                            fileMode: FileDialog.OpenFile
                            nameFilters: [qsTr("Backups (*.txt)")]
                            onAccepted: dialog.people.restoreBackup(selectedFile)
                        }
                    }
                }

                // ── Window ──────────────────────────────────────────────────────
                SettingsPage {
                    spacing: 4

                    WrappingCheckBox {
                        text: qsTr("Minimize to the tray")
                        enabled: dialog.trayAvailable && dialog.minimizedKnown
                        checked: dialog.minimizeToTray
                        onToggled: dialog.minimizeToTray = checked
                    }
                    WrappingCheckBox {
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
                    WrappingCheckBox {
                        Layout.topMargin: 12
                        text: qsTr("Keep your share's preview playing in the background")
                        checked: dialog.keepSharePreview
                        onToggled: dialog.keepSharePreview = checked
                    }
                    Label {
                        Layout.fillWidth: true
                        wrapMode: Text.Wrap
                        opacity: 0.6
                        font.pixelSize: 12
                        text: qsTr("The small picture of your share goes on while another window is " +
                                   "in use. It takes a little of the processor and graphics card, " +
                                   "which a game may miss.")
                    }
                }
            }
        }
    }
}
