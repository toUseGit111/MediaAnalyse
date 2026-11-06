import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MediaAnalyse

// Fixed, non-flickable packet table with sticky header, zebra rows, frame-type
// chips and a QTableWidget-style rectangular cell selection. All pointer and
// wheel handling lives in one overlay so a selection can span any cells.
Item {
    id: root
    property var ctl: null
    property bool isVideo: true
    property var model: null
    property alias table: table

    Layout.fillWidth: true
    Layout.fillHeight: true

    Rectangle { anchors.fill: parent; color: Theme.sunken }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // Manual sticky header: it follows TableView.contentX so no
        // HorizontalHeaderView styling can drift away from the cell widths.
        Item {
            Layout.fillWidth: true
            Layout.preferredHeight: Theme.headerHeight
            clip: true
            Rectangle { anchors.fill: parent; color: Theme.surface1 }
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1; color: Theme.line2
            }
            Row {
                id: headerRow
                x: -table.contentX
                y: 0
                height: Theme.headerHeight
                Repeater {
                    model: root.ctl ? root.ctl.columnHeaders(root.isVideo) : []
                    Rectangle {
                        width: root.ctl ? root.ctl.columnWidth(root.isVideo, index) : 90
                        height: Theme.headerHeight
                        color: "transparent"
                        Rectangle {
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.bottom: parent.bottom
                            width: 1
                            color: Theme.line
                            visible: index < root.ctl.columnCountFor(root.isVideo) - 1
                        }
                        Text {
                            anchors.fill: parent
                            anchors.rightMargin: 8
                            anchors.leftMargin: 4
                            text: modelData
                            color: index === 0 ? Theme.text3 : Theme.text2
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            font.bold: true
                            horizontalAlignment: Text.AlignRight
                            verticalAlignment: Text.AlignVCenter
                            elide: Text.ElideRight
                        }
                    }
                }
            }
        }

        Item {
            Layout.fillWidth: true
            Layout.fillHeight: true

            TableView {
                id: table
                anchors.fill: parent
                model: root.model
                clip: true
                interactive: false   // fixed table: no mouse flicking
                reuseItems: true
                boundsBehavior: Flickable.StopAtBounds
                columnWidthProvider: function(col) {
                    return root.ctl ? root.ctl.columnWidth(root.isVideo, col) : 90
                }
                rowHeightProvider: function() { return Theme.rowHeight }

                ScrollBar.vertical: ScrollBar {
                    policy: ScrollBar.AlwaysOn
                    background: Rectangle { color: Theme.sunken }
                    contentItem: Rectangle {
                        implicitWidth: 10
                        color: parent.pressed ? Theme.line3 : Theme.surface3
                        radius: 4
                    }
                }
                ScrollBar.horizontal: ScrollBar {
                    policy: ScrollBar.AsNeeded
                    background: Rectangle { color: Theme.sunken }
                    contentItem: Rectangle {
                        implicitHeight: 10
                        color: parent.pressed ? Theme.line3 : Theme.surface3
                        radius: 4
                    }
                }

                delegate: Rectangle {
                    id: cell
                    implicitWidth: root.ctl ? root.ctl.columnWidth(root.isVideo, column) : 90
                    implicitHeight: Theme.rowHeight
                    readonly property bool picked: root.ctl
                        ? root.ctl.cellSelected(row, column, root.isVideo) : false
                    readonly property bool hovered: root.ctl
                        ? (root.ctl.hoverRow === row && root.ctl.hoverVideo === root.isVideo)
                        : false
                    readonly property string value: model.display !== undefined
                        ? model.display : ""
                    readonly property bool chip: column === root.ctl.typeColumn(root.isVideo) &&
                        (value === "I" || value === "P" || value === "B")
                    readonly property color chipInk: value === "I" ? Theme.accent
                        : (value === "P" ? Theme.video : Theme.text2)
                    readonly property color chipBg: value === "I" ? Theme.accentWash
                        : (value === "P" ? "#1C2731" : Theme.surface2)
                    readonly property color chipEdge: value === "I" ? Theme.accentEdge
                        : (value === "P" ? "#35506A" : Theme.line2)

                    color: cell.picked ? Theme.selection
                         : (cell.hovered ? Theme.surface2
                            : (row % 2 === 0 ? Theme.sunken : Theme.altRow))

                    Rectangle {
                        anchors.right: parent.right
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 1
                        color: Theme.rowLine
                        visible: column < root.ctl.columnCountFor(root.isVideo) - 1
                    }
                    Rectangle {
                        anchors.left: parent.left
                        anchors.right: parent.right
                        anchors.bottom: parent.bottom
                        height: 1
                        color: Theme.rowLine
                    }
                    // Left accent on the first selected column of each row.
                    Rectangle {
                        visible: cell.picked && column === root.ctl.selC0
                        anchors.left: parent.left
                        anchors.top: parent.top
                        anchors.bottom: parent.bottom
                        width: 2
                        color: "#8AB4F8"
                    }

                    Text {
                        anchors.fill: parent
                        anchors.leftMargin: 4
                        anchors.rightMargin: 8
                        text: cell.value
                        color: cell.picked ? Theme.selectionText
                             : (column === 0 || cell.value === "-" ? Theme.text4
                                : (valueIsNumeric(cell.value) ? Theme.text1 : Theme.text2))
                        font.family: Theme.monoFont
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        elide: Text.ElideRight
                        visible: !cell.chip

                        function valueIsNumeric(v) {
                            return /^[-+]?[\d.]+$/.test(v)
                        }
                    }

                    Rectangle { // frame-type chip: letter + border + fill
                        visible: cell.chip
                        anchors.centerIn: parent
                        width: 20
                        height: 18
                        radius: 3
                        color: cell.picked ? Theme.selection : cell.chipBg
                        border.color: cell.picked ? "#BBD5FF" : cell.chipEdge
                        border.width: 1
                        Text {
                            anchors.centerIn: parent
                            text: cell.value
                            color: cell.picked ? Theme.selectionText : cell.chipInk
                            font.family: Theme.monoFont
                            font.pixelSize: 10
                            font.bold: true
                        }
                    }
                }
            }

            Text {
                anchors.centerIn: parent
                visible: table.rows === 0
                text: root.ctl && root.ctl.filterText.length > 0
                      ? "没有匹配的包 · 清除过滤条件后重试"
                      : "尚无帧数据 · 选择本地文件或粘贴流地址开始解析"
                color: Theme.text3
                font.family: Theme.uiFont
                font.pixelSize: 12
            }

            MouseArea {
                anchors.fill: parent
                anchors.rightMargin: 14
                anchors.bottomMargin: 14
                acceptedButtons: Qt.LeftButton
                hoverEnabled: true
                onPressed: (mouse) => {
                    if (root.ctl) root.ctl.releaseTextFocus()
                    if (!(table.rows > 0)) return
                    var c = root.ctl.cellAt(table, mouse.x, mouse.y, root.isVideo)
                    root.ctl.beginSelection(table, c.y, c.x, root.isVideo,
                                            !!(mouse.modifiers & Qt.ShiftModifier))
                }
                onPositionChanged: (mouse) => {
                    if (!root.ctl) return
                    var c = root.ctl.cellAt(table, mouse.x, mouse.y, root.isVideo)
                    root.ctl.hoverRow = c.y
                    root.ctl.hoverCol = c.x
                    root.ctl.hoverVideo = root.isVideo
                    if (root.ctl.dragging)
                        root.ctl.extendSelection(table, c.y, c.x)
                }
                onExited: { if (root.ctl) root.ctl.hoverRow = -1 }
                onReleased: { if (root.ctl) root.ctl.dragging = false }
                onCanceled: { if (root.ctl) root.ctl.dragging = false }
                onWheel: (wheel) => { if (root.ctl) root.ctl.scrollByWheel(table, wheel) }
            }
        }
    }
}
