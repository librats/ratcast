import QtQuick
import QtQuick.Controls.Material

// A menu of the button it is in, opened beside it — below, or above where the
// window ends first; from its left edge, or its right where the window ends
// first — never over it: a second click, or a double one, would land on its
// first item.
Menu {
    id: menu

    onAboutToShow: {
        const button = menu.parent
        const window = button ? button.Window.window : null
        if (!window) return
        // As wide as its widest item: Material's is a fixed width, eliding.
        let widest = 0
        for (let i = 0; i < menu.count; ++i) {
            const item = menu.itemAt(i)
            if (item && item.visible) widest = Math.max(widest, item.implicitWidth)
        }
        menu.width = Math.min(Math.max(menu.implicitWidth, widest + menu.leftPadding
                                                           + menu.rightPadding),
                              window.width - 16)
        const corner = button.mapToItem(null, 0, button.height)
        menu.y = corner.y + menu.implicitHeight <= window.height - Math.max(0, menu.bottomMargin)
                 ? button.height : -menu.implicitHeight
        menu.x = corner.x + menu.width <= window.width - Math.max(0, menu.rightMargin)
                 ? 0 : button.width - menu.width
    }
}
