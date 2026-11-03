import QtQuick
import QtQuick.Controls.Material
import QtQuick.Controls.impl
import QtQuick.Layouts
import Ratcast.Call

// The people one knows, on the start (people.h): this person — their name and
// tag, their link to send, copied or as a QR code, and a new one in its place
// — a friend's link to ask by, the requests both ways, and the friends.
// Beside the start when wide; narrow, in a drawer of its own.
ColumnLayout {
    id: panel

    required property PeopleController people

    spacing: 8

    /// A person's first letter on a colour of their tag: the same everywhere.
    component Avatar: Rectangle {
        property string name
        property string tag

        readonly property var colors: ["#5865f2", "#3ba55c", "#faa61a", "#ed4245", "#eb459e",
                                       "#00a8fc", "#9b59b6", "#1abc9c"]

        implicitWidth: 32
        implicitHeight: 32
        radius: width / 2
        color: {
            let sum = 0
            for (let i = 0; i < tag.length; ++i)
                sum += tag.charCodeAt(i)
            return colors[sum % colors.length]
        }

        Label {
            anchors.centerIn: parent
            text: name.length > 0 ? name.charAt(0).toUpperCase() : "?"
            font.bold: true
            textFormat: Text.PlainText
        }
    }

    component Plain: Label {
        textFormat: Text.PlainText
        elide: Text.ElideRight
    }

    component Heading: Label {
        Layout.topMargin: 8
        font.bold: true
        opacity: 0.8
    }

    /// A name with its tag after it, smaller.
    component Named: RowLayout {
        property string name
        property string tag
        spacing: 6

        Plain {
            Layout.fillWidth: true
            Layout.maximumWidth: implicitWidth
            text: name.length > 0 ? name : qsTr("Someone")
        }
        Plain {
            Layout.fillWidth: true
            text: tag
            font.pixelSize: 11
            opacity: 0.5
        }
    }

    Label {
        text: qsTr("Friends")
        font.pixelSize: 20
        font.bold: true
    }

    // ── You ─────────────────────────────────────────────────────────────────

    RowLayout {
        Layout.fillWidth: true
        spacing: 8

        Avatar {
            name: panel.people.name
            tag: panel.people.tag
        }
        Named {
            Layout.fillWidth: true
            name: panel.people.name.length > 0 ? panel.people.name : qsTr("You")
            tag: panel.people.tag
        }
    }

    Label {
        Layout.fillWidth: true
        text: qsTr("Send your link to whoever is to be your friend:")
        wrapMode: Text.Wrap
        font.pixelSize: 12
        opacity: 0.7
    }
    RowLayout {
        Layout.fillWidth: true
        spacing: 0

        TextField {
            Layout.fillWidth: true
            readOnly: true
            text: panel.people.link
            selectByMouse: true
            font.pixelSize: 11
            // Its start shown: the site's name, not the secret's end.
            onTextChanged: cursorPosition = 0
            Component.onCompleted: cursorPosition = 0
        }
        ToolButton {
            display: AbstractButton.IconOnly
            icon.source: "icons/copy.svg"
            onClicked: {
                panel.people.copy(panel.people.link)
                copied.restart()
            }
            ToolTip.visible: hovered || copied.running
            ToolTip.text: copied.running ? qsTr("Copied") : qsTr("Copy your link")

            Timer {
                id: copied
                interval: 1500
            }
        }
        ToolButton {
            display: AbstractButton.IconOnly
            icon.source: "icons/qr-code.svg"
            onClicked: qr.open()
            ToolTip.visible: hovered
            ToolTip.text: qsTr("Show it as a QR code")
        }
        ToolButton {
            display: AbstractButton.IconOnly
            icon.source: "icons/ellipsis.svg"
            onClicked: linkMenu.open()

            Menu {
                id: linkMenu

                MenuItem {
                    text: qsTr("A new link (the old one stops working)")
                    icon.source: "icons/refresh-cw.svg"
                    onTriggered: panel.people.resetLink()
                }
            }
        }
    }

    Popup {
        id: qr

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
                text: panel.people.link
            }
            Label {
                Layout.maximumWidth: 240
                text: qsTr("Point a phone's camera at it to open your link there.")
                wrapMode: Text.Wrap
                horizontalAlignment: Text.AlignHCenter
                opacity: 0.7
            }
        }
    }

    // ── Asking ──────────────────────────────────────────────────────────────

    RowLayout {
        Layout.fillWidth: true
        Layout.topMargin: 8

        TextField {
            id: addField
            Layout.fillWidth: true
            placeholderText: qsTr("a friend's link")
            onAccepted: if (text.length > 0 && panel.people.add(text)) text = ""
        }
        Button {
            text: qsTr("Add")
            icon.source: "icons/user-plus.svg"
            enabled: addField.text.length > 0 && panel.people.ready
            onClicked: if (panel.people.add(addField.text)) addField.text = ""
        }
    }
    Label {
        Layout.fillWidth: true
        text: panel.people.notice
        wrapMode: Text.Wrap
        color: Material.color(Material.Teal)
        visible: text.length > 0
    }

    Heading {
        text: qsTr("Asking you")
        visible: panel.people.asking > 0
    }
    Repeater {
        model: panel.people.requests.filter(request => request.incoming)

        RowLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: 8

            Avatar {
                name: modelData.name
                tag: modelData.tag
            }
            Named {
                Layout.fillWidth: true
                name: modelData.name
                tag: modelData.tag
            }
            ToolButton {
                display: AbstractButton.IconOnly
                icon.source: "icons/check.svg"
                icon.color: Material.color(Material.Green)
                onClicked: panel.people.accept(modelData.person)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Accept")
            }
            ToolButton {
                display: AbstractButton.IconOnly
                icon.source: "icons/x.svg"
                onClicked: panel.people.decline(modelData.person)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Decline")
            }
        }
    }

    Heading {
        text: qsTr("Asked by you")
        visible: panel.people.requests.some(request => !request.incoming)
    }
    Repeater {
        model: panel.people.requests.filter(request => !request.incoming)

        RowLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: 8

            Avatar {
                name: modelData.name
                tag: modelData.tag
                opacity: 0.6
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 0

                Named {
                    Layout.fillWidth: true
                    name: modelData.name
                    tag: modelData.tag
                }
                Label {
                    text: modelData.declined ? qsTr("declined")
                                             : qsTr("waiting until they are online and accept")
                    font.pixelSize: 11
                    opacity: 0.6
                    color: modelData.declined ? Material.color(Material.Orange) : Material.foreground
                }
            }
            ToolButton {
                display: AbstractButton.IconOnly
                icon.source: "icons/x.svg"
                onClicked: panel.people.decline(modelData.person)
                ToolTip.visible: hovered
                ToolTip.text: modelData.declined ? qsTr("Remove") : qsTr("Take back")
            }
        }
    }

    // ── Friends ─────────────────────────────────────────────────────────────

    Heading {
        text: panel.people.friends.length > 0 ? qsTr("Your friends (%1)").arg(panel.people.friends.length)
                                              : qsTr("Your friends")
    }
    Label {
        Layout.fillWidth: true
        text: qsTr("None yet. Send your link, or add someone you meet in a room: click their name there.")
        wrapMode: Text.Wrap
        font.pixelSize: 12
        opacity: 0.6
        visible: panel.people.friends.length === 0
    }
    Repeater {
        model: panel.people.friends

        RowLayout {
            required property var modelData
            Layout.fillWidth: true
            spacing: 8

            Avatar {
                name: modelData.name
                tag: modelData.tag
            }
            Named {
                Layout.fillWidth: true
                name: modelData.name
                tag: modelData.tag
            }
            ToolButton {
                display: AbstractButton.IconOnly
                icon.source: "icons/ellipsis.svg"
                onClicked: friendMenu.open()

                Menu {
                    id: friendMenu

                    MenuItem {
                        text: qsTr("Remove from friends")
                        icon.source: "icons/x.svg"
                        onTriggered: panel.people.remove(modelData.person)
                    }
                }
            }
        }
    }
}
