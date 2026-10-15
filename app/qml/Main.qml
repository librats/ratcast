import QtQuick

Window {
    id: root

    required property string ratcastVersion
    required property string libratsVersion

    width: 960
    height: 600
    visible: true
    title: "ratcast"
    color: "#1e1f22"

    Column {
        anchors.centerIn: parent
        spacing: 8

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "ratcast"
            color: "#f2f3f5"
            font.pixelSize: 32
            font.bold: true
        }

        Text {
            anchors.horizontalCenter: parent.horizontalCenter
            text: "v" + root.ratcastVersion + "  ·  librats " + root.libratsVersion
            color: "#949ba4"
            font.pixelSize: 14
        }
    }
}
