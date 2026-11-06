import QtQuick
import QtQuick.Layouts
import MediaAnalyse

// Top rule + numbered id + aligned key/value grid.
Rectangle {
    id: root
    property var stream: ({})
    property bool isVideo: (root.stream.kind || "") === "video"
    property color signalColor: root.isVideo ? Theme.video : Theme.audio
    property var items: []

    height: 102
    color: Theme.surface1
    radius: 8
    border.color: Theme.line
    border.width: 1

    function group(n) {
        return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ",")
    }

    function rebuild() {
        var s = root.stream || {}
        var kind = s.kind || ""
        if (kind === "video") {
            root.items = [
                ["分辨率", (s.width || 0) + "×" + (s.height || 0)],
                ["像素格式", s.pixFmt || "-"],
                ["帧率", Number(s.frameRate || 0).toFixed(3) + " fps"],
                ["时基", s.timeBase || "-"]
            ]
        } else if (kind === "audio") {
            root.items = [
                ["声道", (s.channels || 0) + " ch"],
                ["采样率", root.group(s.sampleRate || 0) + " Hz"],
                ["采样格式", (s.sampleFmt || "-") + (s.profile ? " · " + s.profile : "")],
                ["时基", s.timeBase || "-"]
            ]
        } else {
            root.items = [
                ["类型", kind || "-"],
                ["编码", s.codec || "-"],
                ["时基", s.timeBase || "-"],
                ["时长", Number(s.duration || 0).toFixed(3) + " s"]
            ]
        }
    }

    onStreamChanged: root.rebuild()
    Component.onCompleted: root.rebuild()

    Rectangle {
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.leftMargin: 1
        anchors.rightMargin: 1
        height: 2
        color: root.signalColor
        radius: 2
    }

    ColumnLayout {
        anchors.fill: parent
        anchors.leftMargin: 15
        anchors.rightMargin: 15
        anchors.topMargin: 12
        anchors.bottomMargin: 10
        spacing: 10

        RowLayout {
            Layout.fillWidth: true
            spacing: 9
            Text {
                text: "#" + (root.stream.index !== undefined ? root.stream.index : "?")
                color: root.signalColor
                font.family: Theme.monoFont
                font.pixelSize: 11
                font.bold: true
            }
            Text {
                text: root.isVideo ? "视频流" : ((root.stream.kind || "") === "audio" ? "音频流" : "数据流")
                color: Theme.text1
                font.family: Theme.uiFont
                font.pixelSize: 13
                font.bold: true
            }
            Item { Layout.fillWidth: true }
            Rectangle {
                implicitWidth: codecText.implicitWidth + 12
                implicitHeight: 18
                radius: 3
                color: Theme.sunken
                border.color: Theme.line
                Text {
                    id: codecText
                    anchors.centerIn: parent
                    text: root.stream.codec || "-"
                    color: Theme.text3
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    font.bold: true
                }
            }
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 4
            columnSpacing: 12
            rowSpacing: 3

            Repeater {
                model: root.items
                Text {
                    text: modelData[0]
                    color: Theme.text3
                    font.family: Theme.uiFont
                    font.pixelSize: 10
                    font.bold: true
                    Layout.fillWidth: true
                    Layout.column: index
                    Layout.row: 0
                    elide: Text.ElideRight
                }
            }
            Repeater {
                model: root.items
                Text {
                    text: modelData[1]
                    color: index === 0 ? Theme.text1 : Theme.text2
                    font.family: Theme.monoFont
                    font.pixelSize: 12
                    font.bold: index === 0
                    Layout.fillWidth: true
                    Layout.column: index
                    Layout.row: 1
                    elide: Text.ElideRight
                }
            }
        }
    }
}
