import QtQuick
import QtQuick.Layouts
import MediaAnalyse

// Eyebrow + hairline rule. Hierarchy comes from spacing, not from cards.
Item {
    id: root
    property string label: ""
    property string tail: ""
    property color tailColor: Theme.text4
    property var rightComponent: null

    implicitHeight: 20
    Layout.fillWidth: true

    RowLayout {
        anchors.fill: parent
        spacing: 9

        Text {
            text: root.label
            color: Theme.text3
            font.family: Theme.uiFont
            font.pixelSize: 10
            font.bold: true
            font.letterSpacing: 1.4
            Layout.alignment: Qt.AlignVCenter
        }
        Rectangle {
            Layout.fillWidth: true
            Layout.alignment: Qt.AlignVCenter
            height: 1
            color: Theme.line
        }
        Loader {
            sourceComponent: root.rightComponent
            Layout.alignment: Qt.AlignVCenter
        }
        Text {
            visible: root.tail.length > 0
            text: root.tail
            color: root.tailColor
            font.family: Theme.uiFont
            font.pixelSize: 11
            Layout.alignment: Qt.AlignVCenter
        }
    }
}
