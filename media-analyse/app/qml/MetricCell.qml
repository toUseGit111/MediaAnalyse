import QtQuick
import QtQuick.Layouts
import MediaAnalyse

// Small label over a large tabular value. The whole strip changes meaning
// when the input source switches from a file to a live stream.
Item {
    id: root
    property string label: ""
    property string value: "—"
    property string unit: ""
    property string note: ""
    property bool accent: false
    property bool live: false
    property bool divider: true
    property int fontSize: 20

    Layout.fillWidth: true
    Layout.preferredHeight: 66

    Rectangle { // right hairline between metrics
        visible: root.divider
        anchors.right: parent.right
        anchors.top: parent.top
        anchors.topMargin: 6
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 6
        width: 1
        color: Theme.line
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 16
        anchors.rightMargin: 10
        spacing: 0

        Text {
            text: root.label
            color: Theme.text3
            font.family: Theme.uiFont
            font.pixelSize: 10
            font.bold: true
            font.letterSpacing: 1.0
            Layout.bottomMargin: 6
        }
        RowLayout {
            spacing: 4
            Text {
                text: root.value
                color: root.accent ? Theme.accent : Theme.text1
                font.family: Theme.monoFont
                font.pixelSize: root.live ? 16 : root.fontSize
                font.letterSpacing: root.live ? 0.8 : -0.3
            }
            Text {
                visible: root.unit.length > 0
                text: root.unit
                color: Theme.text3
                font.family: Theme.monoFont
                font.pixelSize: 11
                Layout.alignment: Qt.AlignBottom
                Layout.bottomMargin: 3
            }
            Item { Layout.fillWidth: true }
        }
        Text {
            text: root.note
            color: Theme.text4
            font.family: Theme.monoFont
            font.pixelSize: 10
            Layout.topMargin: 4
            elide: Text.ElideRight
        }
        Item { Layout.fillHeight: true }
    }
}
