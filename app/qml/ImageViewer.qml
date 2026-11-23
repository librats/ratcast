import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Call

// An image of the chat seen whole, over the window: as large as it fits, its
// name and what to do with it along the top — saved among the downloads,
// opened as the system does — closed by a click beside it or Escape.
Popup {
    id: viewer

    required property CallController call
    /// The image shown: a map of ChatModel's attachments.
    property var file: ({})

    function show(image) {
        file = image
        saved.text = ""
        open()
    }

    parent: Overlay.overlay
    x: 0
    y: 0
    width: parent ? parent.width : 0
    height: parent ? parent.height : 0
    padding: 0
    modal: true
    focus: true
    closePolicy: Popup.CloseOnEscape

    background: Rectangle {
        color: Qt.rgba(0, 0, 0, 0.88)
    }

    // A click beside the image closes it.
    MouseArea {
        anchors.fill: parent
        onClicked: viewer.close()
    }

    // Where it may be: under the bar, a margin round.
    Item {
        id: area
        anchors.fill: parent
        anchors.topMargin: bar.height + 16
        anchors.margins: 24

        Image {
            id: image
            anchors.centerIn: parent
            // As large as it fits, never larger than it is: a pixel of it a
            // pixel of the screen.
            width: Math.min(area.width, (viewer.file.width || 0) / Screen.devicePixelRatio)
            height: Math.min(area.height, (viewer.file.height || 0) / Screen.devicePixelRatio)
            source: viewer.visible && viewer.file.url ? viewer.file.url : ""
            asynchronous: true
            fillMode: Image.PreserveAspectFit
            sourceSize.width: width * Screen.devicePixelRatio
            sourceSize.height: height * Screen.devicePixelRatio

            // A click on it is no click beside it.
            MouseArea {
                x: (image.width - image.paintedWidth) / 2
                y: (image.height - image.paintedHeight) / 2
                width: image.paintedWidth
                height: image.paintedHeight
            }
        }
    }
    BusyIndicator {
        anchors.centerIn: parent
        running: image.status === Image.Loading
    }

    RowLayout {
        id: bar
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.margins: 8
        spacing: 4

        Label {
            Layout.fillWidth: true
            Layout.leftMargin: 8
            text: viewer.file.name || ""
            textFormat: Text.PlainText
            elide: Text.ElideMiddle
            color: "white"
            font.bold: true
        }
        Label {
            id: saved
            color: "white"
            opacity: 0.7
            elide: Text.ElideLeft
            Layout.maximumWidth: viewer.width / 3
        }
        ToolButton {
            text: qsTr("Save")
            icon.source: "icons/download.svg"
            onClicked: {
                const path = viewer.call.saveFile(viewer.file.id)
                saved.text = path.length > 0 ? qsTr("Saved among the downloads") : ""
            }
        }
        ToolButton {
            text: qsTr("Open")
            display: AbstractButton.IconOnly
            icon.source: "icons/external-link.svg"
            onClicked: viewer.call.openFile(viewer.file.id)
            ToolTip.visible: hovered
            ToolTip.text: text
        }
        ToolButton {
            text: qsTr("Close")
            display: AbstractButton.IconOnly
            icon.source: "icons/x.svg"
            onClicked: viewer.close()
            ToolTip.visible: hovered
            ToolTip.text: text
        }
    }
}
