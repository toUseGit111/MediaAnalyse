import QtQuick
import QtQuick.Controls.Basic
import MediaAnalyse

// One button implementation for every state in the PRISM spec:
// primary (the single amber action), secondary, ghost and toggle.
Button {
    id: root

    property string variant: "secondary"   // primary | secondary | ghost | toggle
    property bool toggled: false
    property bool compact: false
    property color tint: Theme.accent

    implicitHeight: root.compact ? 26 : Theme.controlHeight
    implicitWidth: Math.max(root.compact ? 54 : 66,
                            contentItem.implicitWidth + (root.compact ? 18 : 26))
    padding: 0
    leftPadding: root.compact ? 9 : 13
    rightPadding: root.compact ? 9 : 13
    font.family: Theme.uiFont
    font.pixelSize: root.compact ? 11 : 12
    font.weight: Font.DemiBold

    readonly property color bgColor: {
        if (!root.enabled) return Theme.surface1
        if (root.variant === "primary") return root.hovered ? Theme.accentHover : root.tint
        if (root.variant === "toggle")
            return root.toggled ? Theme.accentWash : (root.hovered ? Theme.surface2 : Theme.sunken)
        if (root.variant === "ghost") return root.hovered ? Theme.surface2 : "transparent"
        return root.hovered ? Theme.surface3 : Theme.surface2   // secondary
    }
    readonly property color borderColor: {
        if (!root.enabled) return Theme.line
        if (root.variant === "primary") return root.hovered ? Theme.accentHover : root.tint
        if (root.variant === "ghost") return "transparent"
        if (root.variant === "toggle") return root.toggled ? Theme.accentEdge : Theme.line2
        return Theme.line2
    }
    readonly property color ink: {
        if (!root.enabled) return Theme.text4
        if (root.variant === "primary") return Theme.accentInk
        if (root.variant === "toggle") return root.toggled ? Theme.accent : Theme.text2
        if (root.variant === "ghost") return root.hovered ? Theme.text1 : Theme.text2
        return Theme.text1
    }

    contentItem: Text {
        text: root.text
        font: root.font
        color: root.ink
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
    }

    background: Rectangle {
        radius: 6
        color: root.bgColor
        border.color: root.borderColor
        border.width: 1
        Behavior on color { ColorAnimation { duration: 120 } }
    }
}
