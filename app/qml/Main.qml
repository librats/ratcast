import QtCore
import QtQuick
// Qt makes every ToolTip from this module, in the style main() names: imported
// so that it is deployed with the app (windeployqt brings what is imported).
import QtQuick.Controls
import QtQuick.Controls.Material
// IconImage: an icon of icons/ outside a button, in the colour of its text.
import QtQuick.Controls.impl
import QtQuick.Layouts
import Qt.labs.platform as Platform
import Ratcast.Call
import Ratcast.Update
import Ratcast.Video

// One window, three faces: the start (the rat, alive — RatLogo — a name, a
// room to make or a link to join), joining, and the room. CallController says which. In the room, the
// members down the side — who speaks, who is muted or deafened, whose camera
// and share are on — the share watched in the middle with the cameras beside
// it, what is said in a pane on the right (on a button that counts what is
// unread), and the controls along the bottom: microphone, sound, camera,
// sharing (and, while sharing, another screen or window shown in its place),
// chat, how the call goes (a panel on a button or Ctrl+I, its quality a dot),
// leave — round buttons of icons (icons/: Lucide's), named on hovering, red
// while they keep this side unheard or unhearing. What this side shares it sees small in a corner of the stage, which
// it drags to another or hides; paused while the window is not the one in use.
// A share watched may go full screen (F, a double click): only the share, on
// black, its controls and the cursor there while the mouse moves.
// The start also says what the updater does: a new version on its way, or in
// place and to restart into. The start and the panel open the folder the log
// is kept in. The settings (SettingsDialog), from the start and the room; the
// window goes to the tray, if they say so, as it is minimized or closed —
// the room going on — and comes back from its icon.
// Narrow — a phone held upright — the room is one column: the stage on top,
// the members under it, chat and stats over both, and the controls that do
// not fit along the bottom in a menu. On Android, Back steps out of what is
// open, and in a room leaves the app to the background, the room going on.
ApplicationWindow {
    id: root

    required property string ratcastVersion
    required property string libratsVersion
    /// From the command line: join it at once.
    required property string link
    /// From the command line: print how a call goes every so many seconds.
    required property int statsEvery
    /// Where the app keeps its identities and its log.
    required property url dataFolder

    readonly property bool inRoom: call.state === CallController.InRoom
    readonly property bool watching: call.watching.length > 0
    /// Cameras shown: this side's, if on, and the others'.
    readonly property int cameraTiles: (call.camera ? 1 : 0) + call.cameras
    /// One column: a phone held upright, or a window as narrow.
    readonly property bool compact: root.width < 720
    /// Android's: the whole screen to share, never a window of it; no tray,
    /// no folder to open.
    readonly property bool mobile: Qt.platform.os === "android"

    /// This side's own share seen in a corner while it shares: which one
    /// (left 0, right 1; top 0, bottom 2), and whether it is shown or hidden.
    property int ownShareCorner: 2
    property bool ownShareShown: true

    /// The share watched on the whole screen, nothing else drawn.
    property bool fullScreen: false
    /// Whose share went full screen — another's, or none, ends it — and
    /// their name, to say so.
    property string fullScreenOf: ""
    property string fullScreenName: ""
    /// How the window was before, to go back to.
    property int windowedVisibility: Window.Windowed
    /// Said over the stage a few seconds: a share that ended full screen.
    property string stageNotice: ""
    /// How the window was shown last, to come back so from the tray.
    property int shownVisibility: Window.Windowed
    /// The tray has said once this run that the window went to it.
    property bool trayTold: false
    /// Quitting from the tray: the window closes for good.
    property bool quitting: false

    width: 1100
    height: 680
    visible: true
    title: "ratcast"
    color: root.fullScreen ? "black" : "#1e1f22"
    Material.theme: Material.Dark
    Material.accent: Material.Teal

    CallController {
        id: call
        video: picture
        preview: ownCamera
        // Hidden, or the window not the one in use — a game played over it —
        // nothing is made for it, nor drawn: the last picture stays.
        sharePreview: ownShare.visible && root.ownShareShown && root.active ? ownShareView : null
        statsEvery: root.statsEvery
    }

    // Nothing downloaded while a call needs the line.
    Updater {
        id: updater
        quiet: call.state !== CallController.Idle
    }

    Component.onCompleted: if (root.link.length > 0) call.join(root.link)

    /// A link the system opened the app with (Android's): joined as one given
    /// at the start is, unless a room is already on.
    function openLink(link) {
        if (call.state === CallController.Idle)
            call.join(link)
    }

    /// The name of the member watched, as the members' list shows it.
    function watchedName() {
        return root.memberName(call.watched.name ?? "", call.watched.peer ?? "")
    }

    /// Into full screen with the share watched, or out of it, back to the
    /// window as it was.
    function setFullScreen(on) {
        if (on === root.fullScreen || (on && !root.watching))
            return
        if (on) {
            root.windowedVisibility = root.visibility
            root.fullScreenOf = call.watching
            root.fullScreenName = root.watchedName()
            root.fullScreen = true
            root.showFullScreen()
            chrome.wake()
        } else {
            root.fullScreen = false
            root.visibility = root.windowedVisibility
        }
    }

    // The share full screen ended, or another plays in its place: out, and why.
    Connections {
        target: call
        function onWatchedChanged() {
            if (root.fullScreen && call.watched.peerId === root.fullScreenOf)
                root.fullScreenName = root.watchedName()
        }
        function onWatchingChanged() {
            if (!root.fullScreen || call.watching === root.fullScreenOf)
                return
            root.setFullScreen(false)
            root.stageNotice = qsTr("%1 stopped sharing").arg(root.fullScreenName)
            stageNoticeTimer.restart()
        }
    }

    // The desktop may take the window out of full screen itself. Minimized,
    // it may go to the tray (a desktop that tells: Wayland's do not).
    onVisibilityChanged: {
        if (root.fullScreen && root.visibility !== Window.FullScreen
                && root.visibility !== Window.Minimized && root.visibility !== Window.Hidden)
            root.fullScreen = false
        if (root.visibility === Window.Windowed || root.visibility === Window.Maximized
                || root.visibility === Window.FullScreen)
            root.shownVisibility = root.visibility
        if (root.visibility === Window.Minimized && settingsDialog.minimizeToTray && tray.available)
            Qt.callLater(root.toTray)
    }

    onClosing: close => {
        if (root.mobile && root.stepBack()) {
            close.accepted = false
            return
        }
        if (settingsDialog.closeToTray && tray.available && !root.quitting) {
            close.accepted = false
            root.toTray()
        }
    }

    /// The app sent to the background (main.cpp: Android's activity behind).
    signal toBackground()

    /// Android's Back: out of full screen, the chat or the stats, if open;
    /// in a room, the app to the background, the room going on. False when
    /// there is nothing to step out of: the app goes.
    function stepBack() {
        if (root.fullScreen)
            root.setFullScreen(false)
        else if (chatPane.shown)
            chatPane.shown = false
        else if (stats.shown)
            stats.shown = false
        else if (call.state !== CallController.Idle)
            root.toBackground()
        else
            return false
        return true
    }

    Shortcut {
        sequence: "Back"
        enabled: root.mobile
        onActivated: if (!root.stepBack()) Qt.quit()
    }

    /// The window hidden, its icon in the tray: the room goes on.
    function toTray() {
        root.hide()
        if (root.trayTold)
            return
        root.trayTold = true
        tray.showMessage("ratcast", root.inRoom ? qsTr("Still in the room, here in the tray.")
                                                : qsTr("Still here, in the tray."))
    }

    /// Back from the tray, as it was shown.
    function fromTray() {
        root.visibility = root.shownVisibility
        root.raise()
        root.requestActivate()
    }

    Platform.SystemTrayIcon {
        id: tray
        visible: available && (settingsDialog.minimizeToTray || settingsDialog.closeToTray)
        icon.source: "qrc:/ratcast/ratcast.png"
        tooltip: root.inRoom ? qsTr("ratcast: in a room") : "ratcast"
        onActivated: reason => {
            if (reason !== Platform.SystemTrayIcon.Context)
                root.fromTray()
        }

        menu: Platform.Menu {
            Platform.MenuItem {
                text: qsTr("Show ratcast")
                onTriggered: root.fromTray()
            }
            Platform.MenuItem {
                text: call.muted ? qsTr("Unmute") : qsTr("Mute")
                visible: root.inRoom
                onTriggered: call.muted = !call.muted
            }
            Platform.MenuItem {
                text: qsTr("Leave the room")
                visible: root.inRoom
                onTriggered: call.leave()
            }
            Platform.MenuSeparator {}
            Platform.MenuItem {
                text: qsTr("Quit")
                onTriggered: {
                    root.quitting = true  // Qt closes the windows first
                    Qt.quit()
                }
            }
        }
    }

    SettingsDialog {
        id: settingsDialog
        call: call
        trayAvailable: tray.available
        // Wayland does not tell a window it was minimized.
        minimizedKnown: Qt.platform.pluginName !== "wayland"
    }

    Timer {
        id: stageNoticeTimer
        interval: 5000
        onTriggered: root.stageNotice = ""
    }

    // What is kept from one run to the next; the devices, by the controller.
    Settings {
        property alias name: call.name
        property alias server: call.server
        property alias shareVolume: call.shareVolume
        property alias ownShareCorner: root.ownShareCorner
        property alias ownShareShown: root.ownShareShown
    }
    Settings {
        category: "window"
        property alias minimizeToTray: settingsDialog.minimizeToTray
        property alias closeToTray: settingsDialog.closeToTray
    }

    /// What a member is called: its own name, or, until it says one, a
    /// stranger with its short id.
    function memberName(name, peer) {
        return name.length > 0 ? name : qsTr("Someone %1").arg(peer)
    }

    /// A quality as a colour: "good", "fair", "poor", or none yet.
    function qualityColor(quality) {
        switch (quality) {
        case "good": return Material.color(Material.Green)
        case "fair": return Material.color(Material.Amber)
        case "poor": return Material.color(Material.Red)
        default: return "#4e5058"
        }
    }

    component Dot: Rectangle {
        implicitWidth: 10
        implicitHeight: 10
        radius: 5
    }

    // A name others chose: always plain text, never markup.
    component PlainLabel: Label {
        textFormat: Text.PlainText
        elide: Text.ElideRight
    }

    /// How loud the share watched plays, beside the voices.
    component ShareVolume: RowLayout {
        spacing: 4

        Label {
            text: qsTr("Sound")
        }
        Slider {
            id: slider
            Layout.fillWidth: true
            Layout.preferredWidth: 140
            from: 0
            to: call.maxVolume
            stepSize: 0.05
            value: call.shareVolume
            onMoved: call.shareVolume = value
        }
        Label {
            Layout.preferredWidth: 40
            text: qsTr("%1%").arg(Math.round(slider.value * 100))
        }
    }

    component Badge: Label {
        font.pixelSize: 11
        opacity: 0.7
    }

    /// An icon of icons/ by its name, in the colour of the text beside it.
    component Glyph: IconImage {
        /// Its name in icons/ (IconImage's own name is a theme's).
        property string glyph
        source: glyph.length > 0 ? "icons/" + glyph + ".svg" : ""
        sourceSize: Qt.size(16, 16)
        color: Material.foreground
        opacity: 0.8
    }

    /// A button of the call's: round, its icon alone, its name (or `tip`) on
    /// hovering. `alarm` paints it red: this side unheard or unhearing.
    component CallButton: RoundButton {
        id: callButton
        property string iconName
        property string tip: text
        property bool alarm: false
        /// A number in its corner: messages unread.
        property int count: 0
        /// A dot in its corner, in this colour: the call's quality.
        property color dot: "transparent"

        Layout.preferredWidth: 52
        Layout.preferredHeight: 52
        topInset: 2
        bottomInset: 2
        leftInset: 2
        rightInset: 2
        display: AbstractButton.IconOnly
        icon.source: "icons/" + iconName + ".svg"
        icon.width: 22
        icon.height: 22
        Material.background: alarm ? Material.color(Material.Red, Material.Shade700) : undefined
        ToolTip.visible: hovered && tip.length > 0
        ToolTip.text: tip
        ToolTip.delay: 300

        Rectangle {
            anchors.right: parent.right
            anchors.top: parent.top
            visible: callButton.count > 0
            width: Math.max(height, unreadLabel.implicitWidth + 8)
            height: 18
            radius: height / 2
            color: Material.color(Material.Red)

            Label {
                id: unreadLabel
                anchors.centerIn: parent
                text: callButton.count > 99 ? "99+" : callButton.count
                font.pixelSize: 11
                font.bold: true
                color: "white"
            }
        }
        Dot {
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 3
            visible: callButton.dot.a > 0
            color: callButton.dot
            border.color: root.color
            border.width: 2
            implicitWidth: 14
            implicitHeight: 14
            radius: 7
        }
    }

    // ── The start ───────────────────────────────────────────────────────────

    ColumnLayout {
        anchors.centerIn: parent
        width: Math.min(parent.width - 48, 520)
        spacing: 16
        visible: call.state === CallController.Idle

        // As large as the window leaves room for under it.
        RatLogo {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.max(140, Math.min(parent.width, 440,
                                                          (root.height - 420) * implicitWidth / implicitHeight))
            Layout.preferredHeight: Layout.preferredWidth * implicitHeight / implicitWidth
            running: parent.visible && root.visibility !== Window.Minimized
                     && root.visibility !== Window.Hidden
        }

        Label {
            Layout.alignment: Qt.AlignHCenter
            Layout.topMargin: -8
            text: "ratcast"
            font.pixelSize: 32
            font.bold: true
        }

        // Narrow, one under another.
        GridLayout {
            Layout.alignment: Qt.AlignHCenter
            columns: root.compact ? 1 : 3
            columnSpacing: 8
            rowSpacing: 0

            Label {
                Layout.alignment: Qt.AlignHCenter
                text: "v" + root.ratcastVersion + "  ·  librats " + root.libratsVersion
                opacity: 0.6
            }
            Button {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Open log folder")
                icon.source: "icons/folder-open.svg"
                flat: true
                visible: !root.mobile
                onClicked: Qt.openUrlExternally(root.dataFolder)
            }
            Button {
                Layout.alignment: Qt.AlignHCenter
                text: qsTr("Settings")
                icon.source: "icons/settings.svg"
                flat: true
                onClicked: settingsDialog.open()
            }
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 4
            visible: updater.supported

            Label {
                Layout.fillWidth: true
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.Wrap
                opacity: 0.6
                text: {
                    switch (updater.state) {
                    case Updater.Checking:
                        return qsTr("Looking for a new version…")
                    case Updater.Downloading:
                        return qsTr("Downloading version %1… %2%").arg(updater.version)
                                                                 .arg(Math.round(updater.progress * 100))
                    case Updater.Installing:
                        return qsTr("Installing version %1…").arg(updater.version)
                    case Updater.Installed:
                        return qsTr("Version %1 is installed: it starts next time.").arg(updater.version)
                    default:
                        return updater.notice
                    }
                }
                visible: text.length > 0
            }

            RowLayout {
                Layout.alignment: Qt.AlignHCenter
                spacing: 8

                Button {
                    text: qsTr("Restart now")
                    icon.source: "icons/rotate-ccw.svg"
                    highlighted: true
                    visible: updater.state === Updater.Installed
                    onClicked: updater.restart()
                }
                Button {
                    text: qsTr("Download page")
                    icon.source: "icons/external-link.svg"
                    flat: true
                    visible: updater.state === Updater.Idle && updater.notice.length > 0
                             && updater.page.toString().length > 0
                    onClicked: Qt.openUrlExternally(updater.page)
                }
                Button {
                    text: qsTr("Check for updates")
                    icon.source: "icons/refresh-cw.svg"
                    flat: true
                    visible: updater.state === Updater.Idle && !updater.automatic
                    onClicked: updater.check()
                }
                CheckBox {
                    text: qsTr("Update automatically")
                    checked: updater.automatic
                    onToggled: updater.automatic = checked
                }
            }
        }

        TextField {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: 280
            placeholderText: qsTr("Your name, as the others see it")
            text: call.name
            maximumLength: 32
            onTextEdited: call.name = text
        }

        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Make a room")
            icon.source: "icons/plus.svg"
            highlighted: true
            onClicked: call.create()
        }

        RowLayout {
            Layout.fillWidth: true

            TextField {
                id: linkField
                Layout.fillWidth: true
                placeholderText: qsTr("or a rats:// link to a room")
                onAccepted: if (text.length > 0) call.join(text)
            }
            Button {
                text: qsTr("Join")
                icon.source: "icons/log-in.svg"
                enabled: linkField.text.length > 0
                onClicked: call.join(linkField.text)
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

    // ── Joining ─────────────────────────────────────────────────────────────

    // The rat sending its signal while the room is looked for.
    ColumnLayout {
        anchors.centerIn: parent
        spacing: 12
        visible: call.state === CallController.Joining

        RatLogo {
            Layout.alignment: Qt.AlignHCenter
            Layout.preferredWidth: Math.min(root.width - 48, 260)
            Layout.preferredHeight: Layout.preferredWidth * implicitHeight / implicitWidth
            running: parent.visible
        }
        Label {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Joining the room…")
        }
        Button {
            Layout.alignment: Qt.AlignHCenter
            text: qsTr("Cancel")
            icon.source: "icons/x.svg"
            flat: true
            onClicked: call.leave()
        }
    }

    // ── The room ────────────────────────────────────────────────────────────

    // Who is in it, and how to bring others; narrow, under the stage, the
    // first invite only until the others are asked for.
    Pane {
        id: side

        property bool allInvites: false

        // Sizes bound, not anchors let go: an anchor taken away leaves the
        // size it gave, which a binding that did not change does not undo.
        anchors.left: parent.left
        anchors.top: root.compact ? stage.bottom : parent.top
        anchors.bottom: controls.top
        width: root.compact ? parent.width : 280
        visible: root.inRoom && !root.fullScreen
        Material.elevation: 2

        ColumnLayout {
            anchors.fill: parent
            spacing: 8

            Label {
                text: qsTr("Room")
                font.pixelSize: 20
                font.bold: true
            }
            Label {
                text: qsTr("Send a link to whoever is to join:")
                opacity: 0.7
                font.pixelSize: 12
            }
            Repeater {
                model: root.compact && !side.allInvites ? call.invites.slice(0, 1) : call.invites

                RowLayout {
                    required property string modelData
                    Layout.fillWidth: true

                    TextField {
                        Layout.fillWidth: true
                        readOnly: true
                        text: modelData
                        selectByMouse: true
                        font.pixelSize: 11
                    }
                    ToolButton {
                        text: qsTr("Copy")
                        display: AbstractButton.IconOnly
                        icon.source: "icons/copy.svg"
                        onClicked: call.copy(modelData)
                        ToolTip.visible: hovered
                        ToolTip.text: qsTr("Copy the link")
                    }
                }
            }

            Button {
                text: call.invites.length === 2 ? qsTr("1 more link")
                                                : qsTr("%1 more links").arg(call.invites.length - 1)
                flat: true
                visible: root.compact && !side.allInvites && call.invites.length > 1
                onClicked: side.allInvites = true
            }

            Label {
                Layout.topMargin: 8
                text: qsTr("In the room")
                font.bold: true
            }

            // This side first.
            RowLayout {
                Layout.fillWidth: true
                spacing: 8

                Dot {
                    color: call.speaking ? Material.color(Material.Green) : "#4e5058"
                }
                PlainLabel {
                    Layout.fillWidth: true
                    text: qsTr("%1 (you)").arg(root.memberName(call.name, ""))
                }
                Glyph {
                    glyph: "screen-share"
                    visible: call.sharing
                }
                Glyph {
                    glyph: call.deafened ? "headphone-off" : "mic-off"
                    visible: call.deafened || call.muted
                }
            }

            ListView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                spacing: 4
                model: call.members

                delegate: RowLayout {
                    id: member
                    required property string peerId
                    required property string peer
                    required property string name
                    required property bool muted
                    required property bool deafened
                    required property bool speaking
                    required property bool sharing
                    required property bool reconnecting
                    required property real volume
                    required property bool mutedHere
                    width: ListView.view.width
                    spacing: 8

                    Dot {
                        color: member.reconnecting ? Material.color(Material.Orange)
                             : member.speaking ? Material.color(Material.Green) : "#4e5058"
                    }
                    // A click on the name: how loud they are heard here.
                    PlainLabel {
                        Layout.fillWidth: true
                        text: root.memberName(member.name, member.peer)

                        HoverHandler {
                            cursorShape: Qt.PointingHandCursor
                        }
                        TapHandler {
                            acceptedButtons: Qt.LeftButton | Qt.RightButton
                            onTapped: hearing.open()
                        }
                    }
                    Badge {
                        text: member.mutedHere ? qsTr("off for you")
                                               : member.volume !== 1
                                                 ? qsTr("%1%").arg(Math.round(member.volume * 100))
                                                 : ""
                    }
                    Badge {
                        text: member.reconnecting ? qsTr("reconnecting…") : ""
                        color: Material.color(Material.Orange)
                    }
                    Glyph {
                        glyph: member.deafened ? "headphone-off" : "mic-off"
                        visible: !member.reconnecting && (member.deafened || member.muted)
                    }
                    Button {
                        visible: member.sharing
                        flat: true
                        font.pixelSize: 11
                        icon.source: call.watching === member.peerId ? "icons/eye.svg"
                                                                     : "icons/tv-minimal-play.svg"
                        icon.width: 16
                        icon.height: 16
                        text: call.watching === member.peerId ? qsTr("Watching") : qsTr("Watch")
                        enabled: call.watching !== member.peerId
                        onClicked: call.watch(member.peerId)
                    }

                    Popup {
                        id: hearing
                        y: member.height
                        width: member.width
                        padding: 12
                        background: Rectangle {
                            color: "#111214"
                            radius: 6
                            border.color: "#4e5058"
                        }

                        ColumnLayout {
                            anchors.fill: parent
                            spacing: 4

                            PlainLabel {
                                Layout.fillWidth: true
                                text: qsTr("%1, for you").arg(root.memberName(member.name, member.peer))
                                font.bold: true
                            }
                            RowLayout {
                                Layout.fillWidth: true
                                enabled: !member.mutedHere

                                Slider {
                                    id: volume
                                    Layout.fillWidth: true
                                    from: 0
                                    to: call.maxVolume
                                    stepSize: 0.05
                                    value: member.volume
                                    onMoved: call.setVolume(member.peerId, value)
                                }
                                Label {
                                    Layout.preferredWidth: 40
                                    horizontalAlignment: Text.AlignRight
                                    text: qsTr("%1%").arg(Math.round(volume.value * 100))
                                }
                            }
                            RowLayout {
                                CheckBox {
                                    text: qsTr("Mute for me")
                                    checked: member.mutedHere
                                    onToggled: call.setMutedHere(member.peerId, checked)
                                }
                                Item {
                                    Layout.fillWidth: true
                                }
                                Button {
                                    text: qsTr("Reset")
                                    icon.source: "icons/rotate-ccw.svg"
                                    flat: true
                                    visible: member.volume !== 1
                                    onClicked: {
                                        call.setVolume(member.peerId, 1)
                                        volume.value = 1
                                    }
                                }
                            }
                        }
                    }
                }
            }

            Label {
                Layout.fillWidth: true
                wrapMode: Text.Wrap
                text: call.notice
                color: Material.color(Material.Orange)
                visible: text.length > 0
            }
        }
    }

    // The share watched, and the cameras: beside it, or alone in the middle;
    // full screen, the share alone on the whole of it. Narrow, across the top:
    // the share as wide as it is, its buttons and the cameras under it.
    Item {
        id: stage
        readonly property bool camerasBeside: root.watching && root.cameraTiles > 0
                                              && !root.fullScreen && !root.compact
        /// Narrow, the top of one column.
        readonly property bool column: root.compact && !root.fullScreen
        anchors.left: root.fullScreen || root.compact ? parent.left : side.right
        anchors.right: chatPane.visible && !root.compact ? chatPane.left : parent.right
        anchors.top: parent.top
        anchors.margins: root.fullScreen ? 0 : 12
        // Bound, not anchored at the bottom: see the members' pane.
        height: root.fullScreen ? parent.height
              : !stage.column ? controls.y - 24
              : root.watching ? cameras.y + (cameras.visible ? cameras.height : 0)
              : Math.max(120, (controls.y - 24) * 0.45)
        visible: root.inRoom

        VideoView {
            id: picture
            anchors.left: parent.left
            anchors.top: parent.top
            height: stage.column ? stage.width * 9 / 16 : stage.height
            anchors.right: stage.camerasBeside ? cameras.left : parent.right
            anchors.rightMargin: stage.camerasBeside ? 12 : 0
            visible: root.watching
        }

        // A double click on the share takes it full screen and back; full
        // screen, the mouse moving brings the controls and the cursor.
        MouseArea {
            anchors.fill: picture
            enabled: root.watching
            hoverEnabled: root.fullScreen
            cursorShape: root.fullScreen && !chrome.shown ? Qt.BlankCursor : Qt.ArrowCursor
            onDoubleClicked: root.setFullScreen(!root.fullScreen)
            // A touch brings them as the mouse does.
            onPressed: if (root.fullScreen) chrome.wake()
            onPositionChanged: if (root.fullScreen) chrome.wake()
        }

        // Over the share; narrow, under it.
        GridLayout {
            id: shareRow
            anchors.top: stage.column ? picture.bottom : parent.top
            anchors.topMargin: stage.column ? 8 : 0
            anchors.horizontalCenter: picture.horizontalCenter
            width: stage.column ? stage.width : implicitWidth
            columns: stage.column ? 2 : 3
            visible: root.watching && !root.fullScreen

            Button {
                Layout.fillWidth: stage.column
                text: qsTr("Stop watching")
                icon.source: "icons/eye-off.svg"
                onClicked: call.watch("")
            }
            Button {
                Layout.fillWidth: stage.column
                text: qsTr("Full screen")
                icon.source: "icons/maximize.svg"
                onClicked: root.setFullScreen(true)
                ToolTip.visible: hovered
                ToolTip.text: qsTr("F, or a double click on the share; Esc to leave")
            }
            ShareVolume {
                Layout.columnSpan: stage.column ? 2 : 1
                Layout.fillWidth: stage.column
                Layout.leftMargin: stage.column ? 0 : 8
            }
        }

        Label {
            anchors.top: parent.top
            anchors.topMargin: 56
            anchors.horizontalCenter: parent.horizontalCenter
            z: 1  // over the cameras
            padding: 8
            text: root.stageNotice
            textFormat: Text.PlainText
            visible: text.length > 0 && !root.watching
            background: Rectangle {
                color: "#e6111214"
                radius: 6
            }
        }

        Label {
            anchors.centerIn: parent
            width: Math.min(parent.width - 48, 420)
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.Wrap
            opacity: 0.6
            text: qsTr("Everyone here hears everyone. Share a screen or turn a camera on, " +
                       "and the others see it.")
            visible: !root.watching && memberTiles.count === 0 && !call.camera && !call.sharing
        }

        // A member's place on the stage: its camera, or its name until there
        // is one to show; ringed while it speaks.
        component Tile: Rectangle {
            id: tile
            default property alias content: holder.data
            property string caption
            /// Whose first letter stands for the pictures not shown.
            property string named: caption
            /// Its member speaking.
            property bool ringed
            /// Its pictures shown.
            property bool showing
            /// A word in its corner.
            property string note
            color: "#111214"
            radius: 6
            border.color: ringed ? Material.color(Material.Green) : "#4e5058"
            border.width: ringed ? 3 : 1
            clip: true

            Rectangle {
                anchors.centerIn: parent
                width: Math.min(parent.width, parent.height) * 0.4
                height: width
                radius: width / 2
                color: "#2b2d31"
                visible: !tile.showing

                Label {
                    anchors.centerIn: parent
                    text: tile.named.length > 0 ? tile.named[0].toUpperCase() : "?"
                    textFormat: Text.PlainText
                    font.pixelSize: parent.width * 0.45
                }
            }
            Item {
                id: holder
                anchors.fill: parent
                anchors.margins: 3
                visible: tile.showing
            }
            Label {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.bottom: parent.bottom
                anchors.margins: 6
                text: tile.caption
                textFormat: Text.PlainText
                elide: Text.ElideRight
                font.pixelSize: 11
                style: Text.Outline
                styleColor: "#000000"
            }
            Label {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 6
                text: tile.note
                visible: text.length > 0
                opacity: 0.7
                font.pixelSize: 11
            }
        }

        // Beside the share a column of the cameras shown; without one, a grid
        // of everyone, each as large as the stage lets them all be.
        Flow {
            id: cameras

            readonly property int tiles: memberTiles.count + 1
            /// The grid's columns: those that make the tiles largest.
            readonly property int columns: {
                let best = 1
                let widest = 0
                for (let c = 1; c <= tiles; ++c) {
                    const rows = Math.ceil(tiles / c)
                    const across = (stage.width - (c - 1) * spacing) / c
                    const down = (stage.height - (rows - 1) * spacing) / rows * 4 / 3
                    if (Math.min(across, down) > widest) {
                        widest = Math.min(across, down)
                        best = c
                    }
                }
                return best
            }
            readonly property real tileWidth: root.watching
                ? (stage.column ? 120 : 240)
                : Math.min((stage.width - (columns - 1) * spacing) / columns,
                           (stage.height - (Math.ceil(tiles / columns) - 1) * spacing)
                           / Math.ceil(tiles / columns) * 4 / 3)

            // Placed, not anchored: anchors swapped by bindings meet for a
            // moment (right and horizontalCenter), which stretches the column
            // over the whole stage and leaves the share no room.
            x: !root.watching ? (stage.width - width) / 2 : stage.column ? 0 : stage.width - width
            y: !root.watching ? (stage.height - height) / 2
             : stage.column ? shareRow.y + shareRow.height + 8 : 0
            width: !root.watching ? columns * tileWidth + (columns - 1) * spacing
                 : stage.column ? stage.width : tileWidth
            spacing: 8
            visible: !root.fullScreen && (root.watching ? root.cameraTiles > 0
                                                        : memberTiles.count > 0 || call.camera)

            // A tile a member: the view stays as long as the member does.
            Repeater {
                id: memberTiles
                model: call.members

                Tile {
                    id: tile
                    required property string peerId
                    required property string peer
                    required property string name
                    required property bool camera
                    required property bool cameraShown
                    required property bool speaking
                    width: cameras.tileWidth
                    height: width * 3 / 4
                    visible: !root.watching || cameraShown
                    caption: root.memberName(name, peer)
                    ringed: tile.speaking
                    showing: cameraShown
                    // On, but not among the last to speak.
                    note: camera && !cameraShown ? qsTr("camera hidden") : ""

                    VideoView {
                        id: theirCamera
                        anchors.fill: parent
                    }
                    Component.onCompleted: call.setCameraView(tile.peerId, theirCamera)
                    Component.onDestruction: call.setCameraView(tile.peerId, null)
                }
            }

            Tile {
                width: cameras.tileWidth
                height: width * 3 / 4
                visible: !root.watching || call.camera
                caption: qsTr("You")
                named: call.name
                ringed: call.speaking
                showing: call.camera

                // As a mirror shows one: what is left of the user is on the left.
                VideoView {
                    id: ownCamera
                    anchors.fill: parent
                    transform: Scale {
                        origin.x: ownCamera.width / 2
                        xScale: -1
                    }
                }
            }
        }

        // What this side shares, as the others see it: small, in a corner of
        // the stage, dragged to another; hidden on asking, a button in its
        // place. Not on a phone: it shares the screen it would be shown on.
        Rectangle {
            id: ownShare

            readonly property real margin: 8
            /// The most it takes of the stage either way; its shape the
            /// picture's, a window's upright one too.
            readonly property real across: Math.min(root.compact ? 200 : 320, stage.width * 0.4)
            readonly property real down: Math.min(across, stage.height * 0.5)
            readonly property size shownSize: ownShareView.pictureSize
            /// The picture's scale into that; 0 before the first.
            readonly property real fit: shownSize.width > 0
                ? Math.min(across / shownSize.width, down / shownSize.height) : 0

            /// Back to its corner, bound to it again: after a drag.
            function place() {
                ownShare.x = Qt.binding(() => (root.ownShareCorner & 1)
                                              ? stage.width - ownShare.width - ownShare.margin
                                              : ownShare.margin)
                ownShare.y = Qt.binding(() => (root.ownShareCorner & 2)
                                              ? stage.height - ownShare.height - ownShare.margin
                                              : ownShare.margin)
            }

            Component.onCompleted: place()
            z: 2  // over the share and the cameras
            visible: call.sharing && !root.fullScreen && !root.mobile
            // The picture's border round it.
            width: !root.ownShareShown ? showOwnShare.implicitWidth
                 : fit > 0 ? shownSize.width * fit + 2 : across
            height: !root.ownShareShown ? showOwnShare.implicitHeight
                  : fit > 0 ? shownSize.height * fit + 2 : across * 9 / 16
            color: root.ownShareShown ? "#111214" : "transparent"
            radius: 6
            border.color: root.ownShareShown ? "#4e5058" : "transparent"
            clip: true

            VideoView {
                id: ownShareView
                anchors.fill: parent
                anchors.margins: 1
                visible: root.ownShareShown
            }
            // A share ended: no picture of it left for the next.
            Connections {
                target: call
                function onSharingChanged() {
                    if (!call.sharing)
                        ownShareView.clear()
                }
            }
            Label {
                anchors.left: parent.left
                anchors.bottom: parent.bottom
                anchors.margins: 6
                text: root.active ? qsTr("Your share") : qsTr("Your share · paused")
                visible: root.ownShareShown
                font.pixelSize: 11
                style: Text.Outline
                styleColor: "#000000"
            }
            // Dark under it, to be seen over any picture.
            ToolButton {
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 4
                width: 26
                height: 26
                visible: root.ownShareShown
                display: AbstractButton.IconOnly
                icon.source: "icons/eye-off.svg"
                icon.width: 14
                icon.height: 14
                padding: 0
                background: Rectangle {
                    radius: width / 2
                    color: parent.hovered ? "#e6111214" : "#99111214"
                }
                onClicked: root.ownShareShown = false
                ToolTip.visible: hovered
                ToolTip.text: qsTr("Hide your share")
            }
            Button {
                id: showOwnShare
                visible: !root.ownShareShown
                text: qsTr("Your share")
                icon.source: "icons/eye.svg"
                onClicked: root.ownShareShown = true
                ToolTip.visible: hovered
                ToolTip.text: qsTr("See what the others see")
            }

            DragHandler {
                target: ownShare
                enabled: root.ownShareShown
                // Let go: into the corner nearest.
                onActiveChanged: {
                    if (active)
                        return
                    const right = ownShare.x + ownShare.width / 2 > stage.width / 2
                    const bottom = ownShare.y + ownShare.height / 2 > stage.height / 2
                    root.ownShareCorner = (right ? 1 : 0) | (bottom ? 2 : 0)
                    ownShare.place()
                }
            }
            HoverHandler {
                id: ownShareHover
                enabled: root.ownShareShown
            }
            ToolTip.visible: ownShareHover.hovered && root.ownShareShown
            ToolTip.delay: 800
            ToolTip.text: qsTr("What the others see, paused while you are elsewhere. " +
                               "Drag it to another corner.")
        }
    }

    // How the call goes: a section a part, each with its quality and what was
    // measured.
    Rectangle {
        id: stats

        property bool shown: false

        anchors.left: root.compact ? parent.left : stage.left
        anchors.top: root.compact ? parent.top : stage.top
        anchors.margins: 8
        width: root.compact ? parent.width - 16 : Math.min(420, stage.width - 16)
        height: Math.min(statsColumn.implicitHeight + 24,
                         (root.compact ? controls.y : stage.height) - 16)
        visible: root.inRoom && shown
        color: "#e6111214"
        radius: 6
        border.color: "#4e5058"

        ScrollView {
            anchors.fill: parent
            anchors.margins: 12
            contentWidth: availableWidth
            clip: true

            ColumnLayout {
                id: statsColumn
                width: parent.width
                spacing: 10

                Label {
                    text: qsTr("Measuring…")
                    opacity: 0.7
                    visible: call.stats.length === 0
                }

                Repeater {
                    model: call.stats

                    ColumnLayout {
                        required property var modelData
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            spacing: 6
                            Dot {
                                color: root.qualityColor(modelData.quality)
                            }
                            Label {
                                text: modelData.title
                                font.bold: true
                            }
                        }
                        Label {
                            Layout.fillWidth: true
                            text: modelData.reason
                            wrapMode: Text.Wrap
                            color: root.qualityColor(modelData.quality)
                            visible: text.length > 0
                        }
                        Repeater {
                            model: modelData.rows

                            RowLayout {
                                required property var modelData
                                Layout.fillWidth: true
                                spacing: 12

                                Label {
                                    Layout.preferredWidth: 140
                                    text: modelData.label
                                    opacity: 0.7
                                    font.pixelSize: 12
                                }
                                Label {
                                    Layout.fillWidth: true
                                    text: modelData.value
                                    font.family: "monospace"
                                    font.pixelSize: 12
                                    wrapMode: Text.Wrap
                                }
                            }
                        }
                    }
                }

                Button {
                    text: qsTr("Open log folder")
                    icon.source: "icons/folder-open.svg"
                    flat: true
                    visible: !root.mobile
                    onClicked: Qt.openUrlExternally(root.dataFolder)
                }
            }
        }
    }

    // What is said: the messages down a pane, the newest at the bottom, a box
    // to write in under them (Enter sends, Shift+Enter breaks the line). A
    // text shows as it was written, its web links opening in the browser.
    Pane {
        id: chatPane

        property bool shown: false

        // Narrow, over all of the room but its controls.
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.bottom: controls.top
        width: root.compact ? parent.width : 320
        padding: 0
        visible: root.inRoom && !root.fullScreen && shown
        Material.elevation: 2

        // A phone's keyboard would cover what was said: written in when touched.
        onShownChanged: if (shown) {
            if (!root.mobile)
                draft.forceActiveFocus()
            messages.positionViewAtEnd()
        }

        // Read while shown in a window in front.
        Binding {
            target: call.chat
            property: "reading"
            value: chatPane.visible && root.active
        }

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 12
                Layout.topMargin: 8

                Label {
                    Layout.fillWidth: true
                    text: qsTr("Chat")
                    font.pixelSize: 20
                    font.bold: true
                }
                ToolButton {
                    text: qsTr("Close")
                    display: AbstractButton.IconOnly
                    icon.source: "icons/x.svg"
                    onClicked: chatPane.shown = false
                }
            }

            Item {
                Layout.fillWidth: true
                Layout.fillHeight: true

                Label {
                    anchors.centerIn: parent
                    text: qsTr("Nothing said yet")
                    opacity: 0.5
                    visible: messages.count === 0
                }

                ListView {
                    id: messages

                    // At the newest: a message coming is followed; scrolled
                    // up to read, the view stays.
                    property bool following: true

                    anchors.fill: parent
                    clip: true
                    model: call.chat
                    bottomMargin: 8
                    onContentYChanged: following = atYEnd
                    onCountChanged: if (following) Qt.callLater(positionViewAtEnd)
                    ScrollBar.vertical: ScrollBar {}

                    delegate: ColumnLayout {
                        id: message
                        required property string peer
                        required property string name
                        required property string html
                        required property string time
                        required property bool mine
                        required property bool continued
                        width: ListView.view.width
                        spacing: 0

                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.topMargin: 10
                            visible: !message.continued
                            spacing: 6

                            PlainLabel {
                                Layout.maximumWidth: message.width - 100
                                text: message.mine ? qsTr("You")
                                                   : root.memberName(message.name, message.peer)
                                font.bold: true
                                color: message.mine ? Material.accent : Material.foreground
                            }
                            Label {
                                Layout.fillWidth: true
                                text: message.time
                                opacity: 0.5
                                font.pixelSize: 11
                            }
                        }
                        // Escaped by the model: no markup but its links.
                        TextEdit {
                            id: words
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.topMargin: 2
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
                                cursorShape: words.hoveredLink.length > 0 ? Qt.PointingHandCursor
                                                                          : Qt.IBeamCursor
                            }
                        }
                    }
                }
            }

            // The box, and a button that sends it as Enter does.
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 8
                spacing: 4

                ScrollView {
                    Layout.fillWidth: true
                    Layout.maximumHeight: 120

                    TextArea {
                        id: draft
                        placeholderText: qsTr("Say something")
                        wrapMode: TextArea.Wrap

                        function send() {
                            if (call.say(text)) clear()
                        }
                        function sendOn(event) {
                            if (event.modifiers & Qt.ShiftModifier) {
                                event.accepted = false
                                return
                            }
                            send()
                        }
                        Keys.onReturnPressed: event => sendOn(event)
                        Keys.onEnterPressed: event => sendOn(event)
                    }
                }
                ToolButton {
                    Layout.alignment: Qt.AlignBottom
                    text: qsTr("Send")
                    display: AbstractButton.IconOnly
                    icon.source: "icons/send.svg"
                    icon.color: draft.text.trim().length > 0 ? Material.accent : Material.hintTextColor
                    enabled: draft.text.trim().length > 0
                    onClicked: draft.send()
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Send (Enter)")
                }
            }
        }
    }

    Shortcut {
        sequence: "Ctrl+I"
        enabled: root.inRoom
        onActivated: stats.shown = !stats.shown
    }
    Shortcut {
        sequence: "F"
        enabled: root.inRoom && root.watching
        onActivated: root.setFullScreen(!root.fullScreen)
    }
    Shortcut {
        sequence: "Escape"
        enabled: root.fullScreen
        onActivated: root.setFullScreen(false)
    }

    // ── The controls ────────────────────────────────────────────────────────

    Pane {
        id: controls
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        visible: root.inRoom && !root.fullScreen
        Material.elevation: 4

        RowLayout {
            anchors.horizontalCenter: parent.horizontalCenter
            spacing: 6

            CallButton {
                text: call.muted ? qsTr("Unmute") : qsTr("Mute")
                tip: call.muted ? qsTr("Unmute: the others hear you again")
                                : qsTr("Mute: the others do not hear you")
                iconName: call.muted ? "mic-off" : "mic"
                alarm: call.muted
                onClicked: call.muted = !call.muted
            }
            CallButton {
                text: call.deafened ? qsTr("Undeafen") : qsTr("Deafen")
                tip: call.deafened ? qsTr("Undeafen: hear the others, and be heard")
                                   : qsTr("Deafen: you hear nobody, and nobody hears you")
                iconName: call.deafened ? "headphone-off" : "headphones"
                visible: !root.compact
                alarm: call.deafened
                onClicked: call.deafened = !call.deafened
            }
            CallButton {
                text: call.camera ? qsTr("Camera off") : qsTr("Camera")
                tip: call.camera ? qsTr("Turn the camera off") : qsTr("Turn the camera on")
                iconName: call.camera ? "video" : "video-off"
                visible: !root.compact
                highlighted: call.camera
                onClicked: call.camera = !call.camera
            }
            CallButton {
                id: shareButton
                visible: !root.compact
                text: call.choosing ? qsTr("Choosing…")
                                    : call.sharing ? qsTr("Sharing") : qsTr("Share")
                tip: call.choosing ? qsTr("Choosing what to share: click to call it off")
                                   : call.sharing ? qsTr("Sharing: show another, or stop")
                                                  : qsTr("Share a screen or a window")
                iconName: call.choosing ? "loader-circle" : "screen-share"
                highlighted: call.sharing || call.choosing
                // The dialog open: called off, a share going on as it was.
                onClicked: call.choosing ? call.stopSharing()
                                         : call.sharing ? sharingMenu.open() : shareMenu.open()

                Menu {
                    id: shareMenu
                    y: -height

                    MenuItem {
                        text: qsTr("A screen")
                        icon.source: "icons/monitor.svg"
                        onTriggered: call.share(false, withSound.checked)
                    }
                    MenuItem {
                        text: qsTr("A window")
                        icon.source: "icons/app-window.svg"
                        visible: !root.mobile
                        height: visible ? implicitHeight : 0
                        onTriggered: call.share(true, withSound.checked)
                    }
                    MenuSeparator {}
                    MenuItem {
                        id: withSound
                        text: qsTr("With its sound")
                        checkable: true
                        checked: true
                    }
                }
                // Sharing: another in place of what is, the others watching
                // on, the sound as it is.
                Menu {
                    id: sharingMenu
                    y: -height

                    MenuItem {
                        text: qsTr("Show another screen")
                        icon.source: "icons/monitor.svg"
                        onTriggered: call.share(false, false)
                    }
                    MenuItem {
                        text: qsTr("Show another window")
                        icon.source: "icons/app-window.svg"
                        visible: !root.mobile
                        height: visible ? implicitHeight : 0
                        onTriggered: call.share(true, false)
                    }
                    MenuSeparator {}
                    MenuItem {
                        text: qsTr("Stop sharing")
                        icon.source: "icons/screen-share-off.svg"
                        onTriggered: call.stopSharing()
                    }
                }
            }
            // How the call goes, its quality the dot in the corner; why, on
            // hovering.
            CallButton {
                Layout.leftMargin: 8
                text: qsTr("Stats")
                tip: qsTr("How the call goes (Ctrl+I)")
                     + (call.quality.length === 0 ? ""
                        : call.qualityReason.length > 0 ? "\n" + call.quality + ": " + call.qualityReason
                                                        : "\n" + call.quality)
                iconName: "activity"
                visible: !root.compact
                highlighted: stats.shown
                dot: root.qualityColor(call.quality)
                onClicked: stats.shown = !stats.shown
            }
            CallButton {
                text: qsTr("Chat")
                iconName: "message-square"
                highlighted: chatPane.shown
                count: call.chat.unread
                onClicked: chatPane.shown = !chatPane.shown
            }
            CallButton {
                text: qsTr("Settings")
                iconName: "settings"
                visible: !root.compact
                onClicked: settingsDialog.open()
            }
            // Narrow, what does not fit; the call's quality in its corner.
            CallButton {
                text: qsTr("More")
                iconName: "ellipsis"
                visible: root.compact
                dot: root.qualityColor(call.quality)
                onClicked: moreMenu.open()

                Menu {
                    id: moreMenu
                    x: parent.width - width
                    y: -height
                    // The cameras there are, for the one to switch to.
                    onAboutToShow: if (call.camera) call.listDevices()

                    // An item hidden takes no room.
                    component MoreItem: MenuItem {
                        height: visible ? implicitHeight : 0
                    }

                    MoreItem {
                        text: call.deafened ? qsTr("Undeafen") : qsTr("Deafen")
                        icon.source: call.deafened ? "icons/headphones.svg" : "icons/headphone-off.svg"
                        onTriggered: call.deafened = !call.deafened
                    }
                    MoreItem {
                        text: call.camera ? qsTr("Camera off") : qsTr("Camera")
                        icon.source: call.camera ? "icons/video-off.svg" : "icons/video.svg"
                        onTriggered: call.camera = !call.camera
                    }
                    // The next of those listed: a phone's front and back.
                    MoreItem {
                        text: qsTr("Switch camera")
                        icon.source: "icons/switch-camera.svg"
                        visible: call.camera && call.cameraDevices.length > 1
                        onTriggered: {
                            const listed = call.cameraDevices
                            const at = listed.findIndex(d => d.id === call.cameraDevice)
                            call.cameraDevice = listed[(Math.max(at, 0) + 1) % listed.length].id
                        }
                    }
                    MoreItem {
                        text: root.mobile ? qsTr("Share the screen") : qsTr("Share a screen")
                        icon.source: root.mobile ? "icons/screen-share.svg" : "icons/monitor.svg"
                        visible: !call.sharing && !call.choosing
                        onTriggered: call.share(false, true)
                    }
                    MoreItem {
                        text: qsTr("Share a window")
                        icon.source: "icons/app-window.svg"
                        visible: !root.mobile && !call.sharing && !call.choosing
                        onTriggered: call.share(true, true)
                    }
                    MoreItem {
                        text: qsTr("Show another screen")
                        icon.source: "icons/monitor.svg"
                        visible: !root.mobile && call.sharing && !call.choosing
                        onTriggered: call.share(false, false)
                    }
                    MoreItem {
                        text: qsTr("Show another window")
                        icon.source: "icons/app-window.svg"
                        visible: !root.mobile && call.sharing && !call.choosing
                        onTriggered: call.share(true, false)
                    }
                    MoreItem {
                        text: call.choosing ? qsTr("Call off choosing") : qsTr("Stop sharing")
                        icon.source: "icons/screen-share-off.svg"
                        visible: call.sharing || call.choosing
                        onTriggered: call.stopSharing()
                    }
                    MoreItem {
                        text: qsTr("Stats")
                        icon.source: "icons/activity.svg"
                        checkable: true
                        checked: stats.shown
                        onTriggered: stats.shown = checked
                    }
                    MoreItem {
                        text: qsTr("Settings")
                        icon.source: "icons/settings.svg"
                        onTriggered: settingsDialog.open()
                    }
                }
            }
            Button {
                Layout.leftMargin: root.compact ? 0 : 8
                Layout.preferredHeight: 52
                topInset: 2
                bottomInset: 2
                text: qsTr("Leave")
                icon.source: "icons/phone-off.svg"
                display: root.compact ? AbstractButton.IconOnly : AbstractButton.TextBesideIcon
                Material.background: Material.color(Material.Red, Material.Shade700)
                Material.roundedScale: Material.FullScale
                onClicked: call.leave()
                ToolTip.visible: hovered && root.compact
                ToolTip.text: text
            }
        }
    }

    // ── Full screen's controls ──────────────────────────────────────────────

    // Whose share along the top, the controls along the bottom: there while
    // the mouse moves or is over the controls, gone with the cursor after a
    // moment still.
    Item {
        id: chrome

        property bool awake: true
        readonly property bool shown: awake || controlsHover.hovered

        function wake() {
            awake = true
            stillTimer.restart()
        }

        anchors.fill: parent
        visible: root.fullScreen && opacity > 0
        opacity: shown ? 1 : 0
        Behavior on opacity {
            NumberAnimation {
                duration: 250
            }
        }

        Timer {
            id: stillTimer
            interval: 2500
            onTriggered: chrome.awake = false
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            height: 64
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: "#cc000000"
                }
                GradientStop {
                    position: 1
                    color: "transparent"
                }
            }

            PlainLabel {
                anchors.left: parent.left
                anchors.right: parent.right
                anchors.top: parent.top
                anchors.margins: 16
                font.pixelSize: 16
                text: qsTr("%1's share").arg(root.fullScreenName)
            }
        }

        Rectangle {
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            height: 96
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: "transparent"
                }
                GradientStop {
                    position: 1
                    color: "#cc000000"
                }
            }

            // Narrow, in two columns across, alike.
            GridLayout {
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.bottom: parent.bottom
                anchors.bottomMargin: 16
                width: root.compact ? parent.width - 32 : implicitWidth
                columns: root.compact ? 2 : 6
                columnSpacing: 8

                HoverHandler {
                    id: controlsHover
                }

                Button {
                    Layout.fillWidth: root.compact
                    Layout.preferredWidth: root.compact ? 1 : -1  // columns alike
                    text: call.muted ? qsTr("Unmute") : qsTr("Mute")
                    icon.source: call.muted ? "icons/mic-off.svg" : "icons/mic.svg"
                    Material.background: call.muted ? Material.color(Material.Red, Material.Shade700)
                                                    : undefined
                    onClicked: call.muted = !call.muted
                }
                Button {
                    Layout.fillWidth: root.compact
                    Layout.preferredWidth: root.compact ? 1 : -1  // columns alike
                    text: call.deafened ? qsTr("Undeafen") : qsTr("Deafen")
                    icon.source: call.deafened ? "icons/headphone-off.svg" : "icons/headphones.svg"
                    Material.background: call.deafened ? Material.color(Material.Red, Material.Shade700)
                                                       : undefined
                    onClicked: call.deafened = !call.deafened
                }
                Dot {
                    Layout.leftMargin: 8
                    Layout.alignment: root.compact ? Qt.AlignHCenter : Qt.AlignVCenter
                    color: root.qualityColor(call.quality)

                    HoverHandler {
                        id: fullQualityHover
                    }
                    ToolTip.visible: fullQualityHover.hovered && call.quality.length > 0
                    ToolTip.text: call.qualityReason.length > 0
                                  ? call.quality + ": " + call.qualityReason : call.quality
                }
                ShareVolume {
                    Layout.fillWidth: root.compact
                    Layout.preferredWidth: root.compact ? 1 : -1  // columns alike
                    Layout.leftMargin: 8
                }
                Button {
                    Layout.fillWidth: root.compact
                    Layout.preferredWidth: root.compact ? 1 : -1  // columns alike
                    Layout.leftMargin: root.compact ? 0 : 8
                    text: qsTr("Stop watching")
                    icon.source: "icons/eye-off.svg"
                    onClicked: {
                        root.setFullScreen(false)
                        call.watch("")
                    }
                }
                Button {
                    Layout.fillWidth: root.compact
                    Layout.preferredWidth: root.compact ? 1 : -1  // columns alike
                    text: qsTr("Leave full screen")
                    icon.source: "icons/minimize.svg"
                    highlighted: true
                    onClicked: root.setFullScreen(false)
                    ToolTip.visible: hovered
                    ToolTip.text: qsTr("Esc, F, or a double click")
                }
            }
        }
    }
}
