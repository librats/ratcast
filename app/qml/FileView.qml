import QtQuick
import QtQuick.Controls.Material
import QtQuick.Controls.impl
import QtQuick.Layouts
import Ratcast.Call

// A file of a message (ChatModel's attachments): an image as itself — its
// small picture, blurred, until it is here, which it comes by itself while
// small — clicked to be seen whole; any other file a card of its name and
// size, fetched as asked, then opened or shown in its folder. Either one
// coming shows how far it is, and can be stopped; one nobody in the room has
// says so.
Item {
    id: view

    required property CallController call
    /// A map of ChatModel's attachments.
    required property var file
    /// The most it may take across.
    property real maxWidth: 320

    /// The image asked to be seen whole.
    signal opened(var file)

    readonly property bool here: file.status === "here"
    readonly property bool fetching: file.status === "fetching"
    readonly property bool unavailable: file.status === "unavailable"

    implicitWidth: file.image ? picture.width : card.width
    implicitHeight: file.image ? picture.height : card.implicitHeight

    // ── An image ────────────────────────────────────────────────────────────

    Item {
        id: picture

        readonly property real fit: Math.min(1, view.maxWidth / Math.max(1, view.file.width),
                                             240 / Math.max(1, view.file.height))

        visible: view.file.image
        width: Math.max(48, Math.round(view.file.width * fit))
        height: Math.max(48, Math.round(view.file.height * fit))

        Rectangle {
            anchors.fill: parent
            color: Qt.rgba(1, 1, 1, 0.05)
            radius: 6
        }
        // Small, made larger: soft, as it should be until the image comes.
        Image {
            anchors.fill: parent
            source: view.file.preview
            fillMode: Image.Stretch
            smooth: true
            visible: whole.status !== Image.Ready
            opacity: 0.85
        }
        Image {
            id: whole
            anchors.fill: parent
            source: view.file.image && view.here ? view.file.url : ""
            asynchronous: true
            fillMode: Image.PreserveAspectFit
            // No more than it is shown at, the screen's density counted.
            sourceSize.width: picture.width * Screen.devicePixelRatio
            sourceSize.height: picture.height * Screen.devicePixelRatio
        }

        MouseArea {
            anchors.fill: parent
            enabled: whole.status === Image.Ready
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: view.opened(view.file)
        }

        // Not here: fetched on a click, the size said beside.
        RowLayout {
            anchors.centerIn: parent
            spacing: 6
            visible: !view.here && !view.fetching

            RoundButton {
                display: AbstractButton.IconOnly
                icon.source: "icons/download.svg"
                enabled: !view.unavailable
                Material.background: Qt.rgba(0, 0, 0, 0.55)
                onClicked: view.call.fetchFile(view.file.id)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Download")
            }
            Label {
                text: view.unavailable ? qsTr("Nobody in the room has it now") : view.file.size
                color: "white"
                style: Text.Outline
                styleColor: Qt.rgba(0, 0, 0, 0.6)
                wrapMode: Text.Wrap
                Layout.maximumWidth: picture.width - 64
            }
        }

        // Coming: how far, and stopped on a click.
        Item {
            anchors.fill: parent
            visible: view.fetching

            BusyIndicator {
                anchors.centerIn: parent
                running: view.fetching
            }
            ProgressBar {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 6
                value: view.file.progress
                visible: view.file.progress > 0
            }
            CardButton {
                text: qsTr("Stop")
                anchors.top: parent.top
                anchors.right: parent.right
                icon.source: "icons/x.svg"
                onClicked: view.call.stopFetching(view.file.id)
            }
        }
    }

    // ── Any other file ──────────────────────────────────────────────────────

    /// A button of the card: small, its icon alone, its name on hovering.
    component CardButton: ToolButton {
        Layout.preferredWidth: 34
        Layout.preferredHeight: 34
        padding: 6
        display: AbstractButton.IconOnly
        icon.width: 18
        icon.height: 18
        ToolTip.visible: hovered
        ToolTip.text: text
    }

    Pane {
        id: card

        visible: !view.file.image
        // One width for every file: their names and buttons line up.
        width: view.maxWidth
        padding: 8
        Material.elevation: 0
        Material.background: Qt.rgba(1, 1, 1, 0.06)
        Material.roundedScale: Material.SmallScale

        RowLayout {
            anchors.fill: parent
            spacing: 8

            IconImage {
                source: "icons/file.svg"
                sourceSize: Qt.size(28, 28)
                color: Material.accent
            }
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.fillWidth: true
                    text: view.file.name
                    textFormat: Text.PlainText
                    elide: Text.ElideMiddle
                    font.bold: true
                }
                Label {
                    Layout.fillWidth: true
                    text: view.unavailable ? qsTr("%1 · nobody in the room has it now")
                                             .arg(view.file.size)
                        : view.file.status === "failed" ? qsTr("%1 · it did not come")
                                                          .arg(view.file.size)
                        : view.file.size
                    font.pixelSize: 12
                    opacity: 0.6
                    elide: Text.ElideRight
                }
                ProgressBar {
                    Layout.fillWidth: true
                    visible: view.fetching
                    value: view.file.progress
                    indeterminate: view.file.progress === 0
                }
            }
            CardButton {
                text: qsTr("Download")
                visible: !view.here && !view.fetching
                enabled: !view.unavailable
                icon.source: "icons/download.svg"
                onClicked: view.call.fetchFile(view.file.id)
            }
            CardButton {
                text: qsTr("Stop")
                visible: view.fetching
                icon.source: "icons/x.svg"
                onClicked: view.call.stopFetching(view.file.id)
            }
            CardButton {
                text: qsTr("Open")
                visible: view.here
                icon.source: "icons/external-link.svg"
                onClicked: view.call.openFile(view.file.id)
            }
            CardButton {
                text: qsTr("Show in its folder")
                visible: view.here
                icon.source: "icons/folder-open.svg"
                onClicked: view.call.showFile(view.file.id)
            }
        }
    }
}
