import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Call
import Ratcast.Update

// What this ratcast is: the rat, its version and what it stands on — to copy
// into a report —, whether a newer one is on its way (UpdatePanel), and where
// the project is.
Dialog {
    id: dialog

    required property Updater updater
    /// For its clipboard.
    required property CallController call
    required property string ratcastVersion
    required property string libratsVersion
    /// Qt's version, the system and its processor.
    required property string systemInfo

    /// Everything a report wants to know of this one.
    readonly property string versions: "ratcast " + dialog.ratcastVersion + " · librats "
                                       + dialog.libratsVersion + " · " + dialog.systemInfo

    modal: true
    anchors.centerIn: Overlay.overlay
    width: Math.min(parent.width - 32, 460)
    height: Math.min(implicitHeight, parent.height - 32)
    padding: 20
    // Its own button, not Dialog.Close: Qt's word for that is in Qt's
    // translations, which the app does not carry.
    footer: DialogButtonBox {
        Button {
            text: qsTr("Close")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
    }

    // A window too low for it all — a phone on its side — scrolls it.
    ScrollView {
        anchors.fill: parent
        contentWidth: availableWidth

        ColumnLayout {
            width: parent.width
            spacing: 12

            // Left out where the window is low.
            RatLogo {
                Layout.alignment: Qt.AlignHCenter
                visible: dialog.parent.height >= 560
                Layout.preferredWidth: 140
                Layout.preferredHeight: Layout.preferredWidth * implicitHeight / implicitWidth
                running: dialog.visible && visible
            }

            ColumnLayout {
                Layout.fillWidth: true
                spacing: 2

                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: "ratcast"
                    font.pixelSize: 26
                    font.bold: true
                }
                Label {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Version %1").arg(dialog.ratcastVersion)
                    font.pixelSize: 15
                }
                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    text: "librats " + dialog.libratsVersion + " · " + dialog.systemInfo
                    font.pixelSize: 12
                    opacity: 0.6
                }
            }

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                text: qsTr("Peer-to-peer calls and screen sharing for watching things together. Free and open source, under the MIT license.")
            }

            // Narrow — a phone — one under another.
            GridLayout {
                Layout.alignment: Qt.AlignHCenter
                columns: dialog.availableWidth < 400 ? 1 : 3
                columnSpacing: 4
                rowSpacing: 0

                Button {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Copy version")
                    icon.source: "icons/copy.svg"
                    flat: true
                    onClicked: {
                        dialog.call.copy(dialog.versions)
                        text = qsTr("Copied")
                        copiedTimer.restart()
                    }

                    Timer {
                        id: copiedTimer
                        interval: 1500
                        onTriggered: parent.text = Qt.binding(() => qsTr("Copy version"))
                    }
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Website")
                    icon.source: "icons/globe.svg"
                    flat: true
                    onClicked: Qt.openUrlExternally("https://ratcast.app")
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Source code")
                    icon.source: "icons/external-link.svg"
                    flat: true
                    onClicked: Qt.openUrlExternally(dialog.updater.repository)
                }
            }

            MenuSeparator {
                Layout.fillWidth: true
            }

            UpdatePanel {
                Layout.fillWidth: true
                updater: dialog.updater
                alwaysOfferCheck: true
                visible: dialog.updater.supported
            }

            // A build that does not update itself: one's own, or Android's,
            // which its store or a newer APK updates.
            ColumnLayout {
                Layout.fillWidth: true
                spacing: 4
                visible: !dialog.updater.supported

                Label {
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.Wrap
                    opacity: 0.6
                    text: qsTr("This build does not update itself. The new versions are on the releases page.")
                }
                Button {
                    Layout.alignment: Qt.AlignHCenter
                    text: qsTr("Releases")
                    icon.source: "icons/external-link.svg"
                    flat: true
                    onClicked: Qt.openUrlExternally(dialog.updater.repository + "/releases")
                }
            }
        }
    }
}
