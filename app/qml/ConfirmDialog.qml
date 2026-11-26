import QtQuick
import QtQuick.Controls.Material

// What is not undone at a click, asked first: its title the question, its
// text what follows, its button saying what it does.
Dialog {
    id: dialog

    property alias text: said.text
    /// What its button says it does ("Remove").
    property string action

    parent: Overlay.overlay
    anchors.centerIn: parent
    width: Math.min(parent ? parent.width - 48 : 420, 420)
    modal: true

    footer: DialogButtonBox {
        Button {
            text: qsTr("Cancel")
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.RejectRole
        }
        Button {
            text: dialog.action
            flat: true
            DialogButtonBox.buttonRole: DialogButtonBox.AcceptRole
        }
    }

    Label {
        id: said
        // Wrapped to the dialog's width only while it shows: made before the
        // window has its size, it is less than nothing wide, and its lines
        // loop through the dialog's height.
        width: dialog.visible ? parent.width : 0
        wrapMode: Text.Wrap
        textFormat: Text.PlainText
    }
}
