import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Call

// A message of the chat (a row of ChatModel): who said it — their avatar, and
// their name in its colour, with the tag of whose they are — when, what it
// says, and the files it carries. One that continues its author's last goes
// under it with neither, its time where the avatar would be while hovered.
Item {
    id: message

    required property CallController call
    /// The name to show for its author.
    required property string shownName
    required property string peer
    required property string tag
    required property string html
    required property string text
    required property string time
    required property bool mine
    required property bool continued
    required property var attachments

    /// An image of it asked to be seen whole.
    signal opened(var file)

    readonly property int avatarSide: 36
    readonly property int gutter: 12 + avatarSide + 10

    implicitHeight: body.implicitHeight + (continued ? 2 : 12)

    HoverHandler {
        id: hover
    }
    Rectangle {
        anchors.fill: parent
        color: Qt.rgba(1, 1, 1, 0.03)
        visible: hover.hovered
    }

    Avatar {
        id: avatar
        x: 12
        y: 12
        width: message.avatarSide
        height: message.avatarSide
        name: message.shownName
        // One with no card yet still a colour of its own, as among the members.
        tag: message.tag.length > 0 ? message.tag : message.peer
        visible: !message.continued
    }
    Label {
        anchors.right: body.left
        anchors.rightMargin: 6
        y: body.y + 3
        width: message.gutter - 16
        horizontalAlignment: Text.AlignRight
        text: message.time
        font.pixelSize: 10
        opacity: 0.45
        elide: Text.ElideLeft
        visible: message.continued && hover.hovered
    }

    ColumnLayout {
        id: body
        x: message.gutter
        y: message.continued ? 1 : 12
        width: message.width - message.gutter - 12
        spacing: 2

        RowLayout {
            Layout.fillWidth: true
            visible: !message.continued
            spacing: 6

            Label {
                Layout.maximumWidth: body.width - tagLabel.implicitWidth - timeLabel.implicitWidth - 12
                text: message.shownName
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.bold: true
                // The avatar's colour, a little lighter to read on the dark.
                color: Qt.lighter(avatar.color, 1.35)
            }
            Label {
                id: tagLabel
                text: message.tag
                font.pixelSize: 11
                opacity: 0.45
                visible: text.length > 0
            }
            Label {
                id: timeLabel
                Layout.fillWidth: true
                text: message.time
                font.pixelSize: 11
                opacity: 0.45
            }
        }

        // Escaped by the model: no markup but its links.
        TextEdit {
            id: words
            Layout.fillWidth: true
            visible: message.text.length > 0
            readOnly: true
            selectByMouse: true
            textFormat: TextEdit.RichText
            wrapMode: TextEdit.Wrap
            text: message.html
            color: Material.foreground
            selectionColor: Material.accent
            font.pixelSize: 14
            onLinkActivated: link => Qt.openUrlExternally(link)

            HoverHandler {
                cursorShape: words.hoveredLink.length > 0 ? Qt.PointingHandCursor : Qt.IBeamCursor
            }
        }

        Repeater {
            model: message.attachments

            FileView {
                required property var modelData
                Layout.topMargin: 4
                call: message.call
                file: modelData
                maxWidth: Math.min(body.width, 320)
                onOpened: file => message.opened(file)
            }
        }
    }
}
