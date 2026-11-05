import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Call

// The person's groups, on the start above the friends (group.h, people.h):
// each a room that lasts, who is in it now seen from here, entered with a
// click; one made here and named is entered at once, its link there to send.
// Renamed by its maker, left by anyone. A group's link opened (the start's
// field, or the system's) joins it.
ColumnLayout {
    id: panel

    required property PeopleController people
    /// Where a group's room is entered.
    required property CallController call

    spacing: 8

    /// A group's name, or, until a member met says it, its tag.
    function nameOf(group) {
        return group.name.length > 0 ? group.name : qsTr("A group %1").arg(group.tag)
    }

    function make() {
        const id = panel.people.makeGroup(nameField.text)
        if (id.length === 0)
            return
        nameField.text = ""
        panel.call.enter(id)
    }

    Label {
        text: qsTr("Groups")
        font.pixelSize: 20
        font.bold: true
    }

    RowLayout {
        Layout.fillWidth: true

        TextField {
            id: nameField
            Layout.fillWidth: true
            placeholderText: qsTr("a new group's name")
            maximumLength: 64
            onAccepted: if (text.trim().length > 0) panel.make()
        }
        Button {
            text: qsTr("Make")
            icon.source: "icons/plus.svg"
            enabled: nameField.text.trim().length > 0 && panel.people.ready
            onClicked: panel.make()
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("None yet. Make one for those you meet often, or open a group's link.")
        wrapMode: Text.Wrap
        font.pixelSize: 12
        opacity: 0.6
        visible: panel.people.groups.length === 0
    }

    Repeater {
        model: panel.people.groups

        RowLayout {
            id: row
            required property var modelData
            readonly property bool someone: modelData.present > 0
            Layout.fillWidth: true
            spacing: 8

            Avatar {
                name: panel.nameOf(row.modelData)
                tag: row.modelData.tag
                square: true
                shows: true
                online: row.someone
                inRoom: row.someone
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Label {
                    Layout.fillWidth: true
                    text: panel.nameOf(row.modelData)
                    textFormat: Text.PlainText
                    elide: Text.ElideRight
                }
                Label {
                    // As the servers see it: whose calls are in its room.
                    text: row.someone ? qsTr("in the room: %1").arg(row.modelData.present)
                                      : qsTr("nobody in the room")
                    font.pixelSize: 11
                    opacity: row.someone ? 0.8 : 0.5
                    color: row.someone ? Material.accent : Material.foreground
                }
            }
            ToolButton {
                display: AbstractButton.IconOnly
                icon.source: "icons/log-in.svg"
                highlighted: row.someone
                onClicked: panel.call.enter(row.modelData.id)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Join")
            }
            ToolButton {
                display: AbstractButton.IconOnly
                icon.source: "icons/ellipsis.svg"
                onClicked: groupMenu.open()

                Menu {
                    id: groupMenu

                    MenuItem {
                        text: qsTr("Copy the link")
                        icon.source: "icons/copy.svg"
                        onTriggered: panel.people.copy(row.modelData.link)
                    }
                    MenuItem {
                        text: qsTr("Show it as a QR code")
                        icon.source: "icons/qr-code.svg"
                        onTriggered: {
                            qr.link = row.modelData.link
                            qr.open()
                        }
                    }
                    MenuItem {
                        text: qsTr("Rename")
                        icon.source: "icons/pencil.svg"
                        visible: row.modelData.made
                        height: visible ? implicitHeight : 0
                        onTriggered: {
                            rename.group = row.modelData.id
                            rename.name = row.modelData.name
                            rename.open()
                        }
                    }
                    MenuItem {
                        text: qsTr("Leave the group")
                        icon.source: "icons/x.svg"
                        onTriggered: panel.people.leaveGroup(row.modelData.id)
                    }
                }
            }
        }
    }

    Popup {
        id: qr

        property string link

        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        padding: 16

        ColumnLayout {
            spacing: 8

            QrCode {
                Layout.alignment: Qt.AlignHCenter
                Layout.preferredWidth: 240
                Layout.preferredHeight: 240
                text: qr.link
            }
            Label {
                Layout.maximumWidth: 240
                text: qsTr("Point a phone's camera at it to join the group there.")
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                opacity: 0.7
            }
        }
    }

    Dialog {
        id: rename

        property string group
        property alias name: renameField.text

        parent: Overlay.overlay
        anchors.centerIn: parent
        modal: true
        title: qsTr("Rename the group")
        standardButtons: Dialog.Ok | Dialog.Cancel
        onOpened: renameField.forceActiveFocus()
        onAccepted: panel.people.renameGroup(group, renameField.text)

        TextField {
            id: renameField
            width: 260
            maximumLength: 64
            onAccepted: rename.accept()
        }
    }
}
