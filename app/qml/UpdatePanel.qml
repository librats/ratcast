import QtQuick
import QtQuick.Controls.Material
import QtQuick.Layouts
import Ratcast.Update

// What the updater does, said, and what to do about it: a new version on its
// way, or in place and to restart into; a newer one waiting for the call to
// end; a check asked for. The start shows it, and the About dialog.
ColumnLayout {
    id: panel

    required property Updater updater
    /// A check offered even while the updater checks by itself.
    property bool alwaysOfferCheck: false

    spacing: 4

    Label {
        Layout.fillWidth: true
        horizontalAlignment: Text.AlignHCenter
        wrapMode: Text.Wrap
        opacity: 0.6
        text: {
            switch (panel.updater.state) {
            case Updater.Checking:
                return qsTr("Looking for a new version…")
            case Updater.Downloading:
                return qsTr("Downloading version %1… %2%").arg(panel.updater.version)
                                                         .arg(Math.round(panel.updater.progress * 100))
            case Updater.Installing:
                return qsTr("Installing version %1…").arg(panel.updater.version)
            case Updater.Installed:
                return qsTr("Version %1 is installed: it starts next time.").arg(panel.updater.version)
            default:
                if (panel.updater.notice.length > 0)
                    return panel.updater.notice
                // Found, and nothing downloaded while a call is on.
                if (panel.updater.version.length > 0 && panel.updater.quiet)
                    return qsTr("Version %1 is out: it downloads once the call is over.")
                               .arg(panel.updater.version)
                return ""
            }
        }
        visible: text.length > 0
    }

    // Narrow — a phone — one under another.
    GridLayout {
        Layout.alignment: Qt.AlignHCenter
        columns: panel.width < 400 ? 1 : 4
        columnSpacing: 8
        rowSpacing: 0

        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Restart now")
            icon.source: "icons/rotate-ccw.svg"
            highlighted: true
            visible: panel.updater.state === Updater.Installed
            onClicked: panel.updater.restart()
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Download page")
            icon.source: "icons/external-link.svg"
            flat: true
            visible: panel.updater.state === Updater.Idle && panel.updater.notice.length > 0
                     && panel.updater.page.toString().length > 0
            onClicked: Qt.openUrlExternally(panel.updater.page)
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Check for updates")
            icon.source: "icons/refresh-cw.svg"
            flat: true
            visible: panel.updater.state === Updater.Idle
                     && (panel.alwaysOfferCheck || !panel.updater.automatic)
            onClicked: panel.updater.check()
        }
        CheckBox {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Update automatically")
            checked: panel.updater.automatic
            onToggled: panel.updater.automatic = checked
        }
    }
}
