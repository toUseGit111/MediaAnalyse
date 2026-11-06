import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MediaAnalyse

/*
 * PRISM - "precision instrument" UI port for Qt Quick.
 * Reference: media-analyzer-redesign/design-preview.html + qt-preview*.png
 * Reading order: APP BAR > SOURCE > OVERVIEW > NOTICE > STREAMS >
 *                TOOLBAR > DATAVIEW > STATUS.
 *
 * Functional invariants carried over from the previous UI:
 *   - file mode: "打开" opens the native file dialog and auto-analyses;
 *   - 15 columns, one row per packet, decode extras are additive;
 *   - QTableWidget-style blue rectangular cell selection, Ctrl+C -> TSV;
 *   - fixed (non-flickable) table with manual wheel scrolling;
 *   - Stop is NEUTRAL: partial rows are published, no error dialog.
 */
ApplicationWindow {
    id: root
    width: 1240
    height: 900
    minimumWidth: 980
    minimumHeight: 720
    visible: true
    title: qsTr("媒体分析器 · MEDIA ANALYSE")
    color: Theme.bg

    // ---------------- state ----------------
    property bool isUrlMode: false
    property int tabIndex: 0
    property var streamList: []
    property var summary: ({})
    property var errorDiag: ({})
    property bool noticeDismissed: false
    property bool decodeOn: false
    property string filterText: ""
    property var proto: root.isUrlMode ? root.detectProto(urlField.text)
                                       : { name: "", rule: "", ok: false, empty: true }

    // ---- QTableWidget-style rectangular CELL selection ----
    property int selR0: -1
    property int selR1: -1
    property int selC0: -1
    property int selC1: -1
    property bool selOnVideo: true
    property int anchorRow: -1
    property int anchorCol: -1
    property bool dragging: false
    property int hoverRow: -1
    property int hoverCol: -1
    property bool hoverVideo: true

    function resetSelection() {
        root.selR0 = root.selR1 = root.selC0 = root.selC1 = -1
        root.anchorRow = root.anchorCol = -1
        root.dragging = false
    }

    function cellSelected(row, col, isVideo) {
        return root.selR0 >= 0 && root.selOnVideo === isVideo &&
               row >= root.selR0 && row <= root.selR1 &&
               col >= root.selC0 && col <= root.selC1
    }

    // Visible column index -> pixel width. The width is keyed on the INTERNAL
    // column (via the controller's mapping) so video and audio tabs size their
    // differing tail columns correctly.
    function columnWidth(isVideo, col) {
        var ic = Analysis.toInternalColumn(isVideo, col)
        if (ic === 0) return 56
        if (ic <= 2) return 68
        if (ic === 12) return 108   // dims (video only)
        if (ic === 13) return 104   // format
        if (ic === 14) return 48    // ch (audio only)
        return 88
    }

    // Wrappers so FrameGrid (which holds `ctl: root`) can reach the controller's
    // column contract through the ApplicationWindow.
    function columnHeaders(isVideo) { return Analysis.columnHeaders(isVideo) }
    function columnCountFor(isVideo) { return Analysis.columnCountFor(isVideo) }
    function typeColumn(isVideo) { return Analysis.typeColumn(isVideo) }

    function cellAt(view, x, y, isVideo) {
        var maxRow = Math.max(0, (view.rows || 1) - 1)
        var maxCol = Analysis.columnCountFor(isVideo) - 1
        var row = Math.max(0, Math.min(maxRow,
                          Math.floor((y + view.contentY) / Theme.rowHeight)))
        var cx = x + view.contentX
        var acc = 0
        var col = maxCol
        for (var c = 0; c <= maxCol; ++c) {
            var w = root.columnWidth(isVideo, c)
            if (cx < acc + w || c === maxCol) { col = c; break }
            acc += w
        }
        return Qt.point(col, row)
    }

    function beginSelection(view, row, col, isVideo, withShift) {
        if (!view || !(view.rows > 0)) return
        if (withShift && root.anchorRow >= 0 && root.selOnVideo === isVideo) {
            root.dragging = true
            root.extendSelection(view, row, col)
            return
        }
        root.selOnVideo = isVideo
        root.anchorRow = row
        root.anchorCol = col
        root.dragging = true
        root.extendSelection(view, row, col)
    }

    function extendSelection(view, row, col) {
        if (root.anchorRow < 0) return
        root.selR0 = Math.min(root.anchorRow, row)
        root.selR1 = Math.max(root.anchorRow, row)
        // Pressing the '#' column selects whole rows, like a row header.
        if (root.anchorCol === 0) {
            root.selC0 = 0
            root.selC1 = Analysis.columnCountFor(root.selOnVideo) - 1
        } else {
            root.selC0 = Math.min(root.anchorCol, col)
            root.selC1 = Math.max(root.anchorCol, col)
        }
    }

    function copySelection() {
        if (root.selR0 < 0) return
        Analysis.copySelection(root.selOnVideo, root.selR0, root.selR1,
                               root.selC0, root.selC1)
        root.showToast("已复制 " + (root.selR1 - root.selR0 + 1) + " 行 TSV 到剪贴板")
    }

    // interactive:false keeps the table fixed but disables Flickable's own
    // wheel handling - one notch (~120) moves three rows.
    function scrollByWheel(view, wheel) {
        if (!view) return
        var pdy = wheel.pixelDelta ? wheel.pixelDelta.y : 0
        var pdx = wheel.pixelDelta ? wheel.pixelDelta.x : 0
        var ady = wheel.angleDelta ? wheel.angleDelta.y : 0
        var adx = wheel.angleDelta ? wheel.angleDelta.x : 0
        var dy = pdy !== 0 ? pdy : ady * 0.75
        var dx = pdx !== 0 ? pdx : adx * 0.75
        if (wheel.modifiers & Qt.ShiftModifier) { dx += dy; dy = 0 }
        if (dy !== 0) {
            var maxY = Math.max(0, view.contentHeight - view.height)
            view.contentY = Math.max(0, Math.min(maxY, view.contentY - dy))
        }
        if (dx !== 0) {
            var maxX = Math.max(0, view.contentWidth - view.width)
            view.contentX = Math.max(0, Math.min(maxX, view.contentX - dx))
        }
        wheel.accepted = true
    }

    function releaseTextFocus() {
        pathField.focus = false
        urlField.focus = false
        filterInput.focus = false
    }

    // ---------------- helpers ----------------
    function group(n) {
        return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ",")
    }
    function clock(seconds) {
        var s = Math.max(0, Number(seconds || 0))
        var m = Math.floor(s / 60)
        var r = s - m * 60
        return (m < 10 ? "0" : "") + m + ":" + (r < 10 ? "0" : "") + r.toFixed(3)
    }
    function shortFormat(name) {
        if (!name) return "—"
        var a = String(name).split(",")
        var s = (a[0] || "").toUpperCase()
        if (a.length > 1) s += " / " + (a[1] || "").toUpperCase()
        return s
    }
    function fileExt(path) {
        var m = /\.([A-Za-z0-9]{1,5})$/.exec(path || "")
        return m ? m[1].toUpperCase() : "—"
    }
    function detectProto(raw) {
        var u = (raw || "").trim()
        if (!u) return { name: "待输入", rule: "粘贴流地址", ok: false, empty: true }
        if (/^rtsp(s)?:\/\//i.test(u)) return { name: "RTSP", rule: "rtsp:// 前缀", ok: true, empty: false }
        if (/^rtmp(s|t|e|te|fp)?:\/\//i.test(u)) return { name: "RTMP", rule: "rtmp:// 前缀", ok: true, empty: false }
        if (/\.m3u8(\?|#|$)/i.test(u)) return { name: "HLS", rule: ".m3u8 播放列表", ok: true, empty: false }
        if (/\.flv(\?|#|$)/i.test(u)) return { name: "HTTP-FLV", rule: ".flv 扩展名", ok: true, empty: false }
        return { name: "未识别", rule: "地址协议未识别", ok: false, empty: false }
    }
    function parseExtra(text) {
        var map = ({})
        var parts = String(text || "").split(/[,;\n]+/)
        for (var i = 0; i < parts.length; i++) {
            var p = parts[i].trim()
            if (!p) continue
            var eq = p.indexOf("=")
            if (eq > 0) map[p.slice(0, eq).trim()] = p.slice(eq + 1).trim()
        }
        return map
    }
    function showToast(msg) {
        toastText.text = msg
        toast.opacity = 1
        toastTimer.restart()
    }
    // "open 0.003s · probe 0.004s · demux 0.037s" for the diagnostics panel.
    // Stages that never ran arrive as -1 (worker sends null) and are omitted;
    // returns "" when nothing is available, so callers can append conditionally.
    function stageBreakdown(o) {
        if (!o) return ""
        var parts = []
        if (o.openSeconds >= 0) parts.push("open " + Number(o.openSeconds).toFixed(3) + "s")
        if (o.probeSeconds >= 0) parts.push("probe " + Number(o.probeSeconds).toFixed(3) + "s")
        if (o.demuxSeconds >= 0) parts.push("demux " + Number(o.demuxSeconds).toFixed(3) + "s")
        return parts.join(" · ")
    }

    // ---------------- derived view model ----------------
    property bool hasResult: root.summary && root.summary.formatName !== undefined
    property string appbarTag: root.isUrlMode
        ? (root.proto.ok ? root.proto.name + " 实时流 · 帧解析中" : "地址协议未识别")
        : "本地文件 · 离线解析"

    function computeMetrics() {
        var s = root.summary || ({})
        var p = root.proto || ({})
        var has = root.hasResult
        var br = Number(s.bitrate || 0)
        var elapsed = Number(s.elapsedSeconds || 0)
        var rate = (has && elapsed > 0) ? root.group(Math.round(Number(s.totalPackets || 0) / elapsed)) : "0"
        if (root.isUrlMode) {
            return [
                { label: "时长", value: "LIVE", unit: "", note: "持续流 · 无固定时长", accent: false, live: true, size: 20 },
                { label: "平均码率", value: br > 0 ? (br / 1000000).toFixed(2) : "—", unit: "Mb/s", note: "容器 / 逐包统计", accent: false, live: false, size: 20 },
                { label: "已解析包数", value: has ? root.group(s.totalPackets || 0) : "0", unit: "", note: Analysis.busy ? "持续增长" : (s.stopped ? "已停止" : "—"), accent: false, live: false, size: 20 },
                { label: "已解析帧数", value: has ? root.group(s.videoFrames || 0) : "0", unit: "帧", note: "含 " + root.group(s.keyFrames || 0) + " 个关键帧", accent: true, live: false, size: 20 },
                { label: "接入协议", value: p.ok ? p.name : (p.empty ? "待输入" : "未识别"), unit: "", note: p.ok ? "auto · " + p.rule : "地址无法识别", accent: false, live: false, size: 14 }
            ]
        }
        return [
            { label: "时长", value: has ? Number(s.duration || 0).toFixed(3) : "—", unit: has ? "s" : "", note: has ? root.clock(s.duration) : "等待解析", accent: false, live: false, size: 20 },
            { label: "平均码率", value: (has && br > 0) ? (br / 1000000).toFixed(2) : "—", unit: "Mb/s", note: has ? root.group(br) + " bit/s" : "—", accent: false, live: false, size: 20 },
            { label: "包总数", value: has ? root.group(s.totalPackets || 0) : "—", unit: "", note: has ? (root.group(s.videoFrames || 0) + " video · " + root.group(s.audioFrames || 0) + " audio") : "—", accent: false, live: false, size: 20 },
            { label: "解析耗时", value: has ? elapsed.toFixed(2) : "—", unit: "s", note: rate + " pkt/s", accent: true, live: false, size: 20 },
            { label: "容器格式", value: has ? root.shortFormat(s.formatName) : "—", unit: "", note: has ? (s.formatLongName || "—") : "—", accent: false, live: false, size: 14 }
        ]
    }
    property var metrics: root.computeMetrics()

    property string overviewTail: {
        if (root.errorDiag && root.errorDiag.code) return "解析失败 · " + root.errorDiag.code
        if (Analysis.busy) return "解析中 · " + Analysis.statusText
        if (root.isUrlMode)
            return root.proto.ok ? "实时解析 · " + Number(root.summary.frameRate || 0).toFixed(1) + " fps"
                                 : "等待有效的流地址"
        return root.hasResult
            ? "解析完成 · " + root.group(root.summary.totalPackets || 0) + " packets in " + Number(root.summary.elapsedSeconds || 0).toFixed(2) + " s"
            : "等待输入"
    }
    property color overviewColor: (root.errorDiag && root.errorDiag.code) ? Theme.error
        : (Analysis.busy ? Theme.accent : (root.hasResult || !root.isUrlMode ? Theme.success : Theme.text4))

    function computeNotice() {
        var s = root.summary || ({})
        var p = root.proto || ({})
        if (root.errorDiag && root.errorDiag.code) {
            return {
                kind: "error", title: "解析失败",
                body: (root.errorDiag.message || "") +
                      (root.errorDiag.hint ? " · " + root.errorDiag.hint : ""),
                detail: "阶段 " + (root.errorDiag.stage || "-") + " · 错误码 " +
                        (root.errorDiag.code || "-") + " (" + (root.errorDiag.nativeCode || 0) + ") · 耗时 " +
                        Number(root.errorDiag.elapsedSeconds || 0).toFixed(2) + " s" +
                        (root.stageBreakdown(root.errorDiag).length
                            ? " · " + root.stageBreakdown(root.errorDiag) : "") +
                        " · " + (root.errorDiag.url || "")
            }
        }
        if (s.stopped)
            return { kind: "warning", title: "已停止解析",
                     body: "仅显示已解析的前 " + root.group(s.totalPackets || 0) +
                           " 个包，后续帧不再解析" + (s.notes ? " · " + s.notes : ""),
                     detail: s.notes || "停止在当前帧数，不弹错误框" }
        if (root.isUrlMode) {
            if (!p.ok && !p.empty)
                return { kind: "warning", title: "地址协议未识别",
                         body: "仅支持 rtsp:// · rtmp:// · .m3u8 · .flv；仍会交给 FFmpeg 直接尝试（内部排障）",
                         detail: "识别规则：rtsp(s):// → RTSP · rtmp*:// → RTMP · .m3u8 → HLS · .flv → HTTP-FLV" }
            if (p.ok)
                return { kind: "info", title: "协议自动识别 · " + p.name,
                         body: p.rule + "；无需选择协议或配置连接参数，点击「解析」开始逐帧解析",
                         detail: "识别规则：rtsp(s):// → RTSP · rtmp*:// → RTMP · .m3u8 → HLS · .flv → HTTP-FLV" }
            return { kind: "info", title: "等待输入",
                     body: "粘贴 rtsp:// · rtmp:// · .m3u8 · .flv 地址，按回车或点击「解析」开始",
                     detail: "协议由地址自动判定" }
        }
        if (s.truncated)
            return { kind: "warning", title: "达到帧数上限",
                     body: "结果已按 maxFrames 截断 · " + root.group(s.totalPackets || 0) + " 个包",
                     detail: "在「配置」中调整最大帧数" }
        if (s.notes && s.notes.length > 0)
            return { kind: "warning", title: "解码信息", body: s.notes,
                     detail: "maxDecodeFrames = " + root.group(maxDecodeSpin.value) + "（每条流独立计数）" }
        if (root.hasResult)
            return { kind: "info", title: "解析完成",
                     body: root.group(s.totalPackets || 0) + " 个包 · " +
                           (s.streamCount || 0) + " 条流 · 耗时 " +
                           Number(s.elapsedSeconds || 0).toFixed(2) + " s" +
                           (root.decodeOn ? " · 解码已开启" : " · 仅包级信息"),
                     detail: (root.stageBreakdown(s).length
                                  ? root.stageBreakdown(s) + " · " : "") +
                             (root.decodeOn
                                 ? "解码上限 " + root.group(maxDecodeSpin.value) + " 帧/流，仅前若干帧补充 dims / format / ch"
                                 : "勾选「解码并解析」可补充帧级 dims / format / ch") }
        return { kind: "info", title: "就绪",
                 body: "选择本地文件并点击「打开」，或直接粘贴路径后按回车开始逐帧解析",
                 detail: root.decodeOn
                     ? "已开启解码：前 " + root.group(maxDecodeSpin.value) + " 帧/流补充 dims / format / ch"
                     : "未开启解码：仅解析包级信息（更快）" }
    }
    property var notice: root.computeNotice()
    property bool noticeVisible: !root.noticeDismissed
    property color noticeBg: root.notice.kind === "error" ? "#2B1D1C"
        : (root.notice.kind === "info" ? "#16232B" : "#241E14")
    property color noticeEdge: root.notice.kind === "error" ? "#A05B50"
        : (root.notice.kind === "info" ? "#2F4B5B" : "#4A3A21")
    property color noticeLeft: root.notice.kind === "error" ? Theme.error
        : (root.notice.kind === "info" ? Theme.video : Theme.accent)
    property color noticeInk: root.notice.kind === "error" ? "#E8BDB7"
        : (root.notice.kind === "info" ? "#C3D6E4" : "#E6D3B0")
    property color noticeBold: root.notice.kind === "error" ? Theme.error
        : (root.notice.kind === "info" ? Theme.video : Theme.accent)

    property string selectionText: root.selR0 >= 0
        ? (root.selR1 - root.selR0 + 1) + " 行 × " + (root.selC1 - root.selC0 + 1) + " 列 已选"
        : "未选择"
    property string rowCountText: (root.tabIndex === 0 ? Analysis.videoRows : Analysis.audioRows)
                                  + " 行 · " + Analysis.columnCountFor(true) + " 列" +
                                  (root.filterText.length > 0 ? "（已过滤）" : "")
    property string stateText: {
        if (root.errorDiag && root.errorDiag.code) return "解析失败"
        if (Analysis.busy) return Analysis.stopping ? "正在停止" :
            (root.isUrlMode ? "LIVE · 帧解析中" : "解析中")
        if (root.summary.stopped) return "已停止 · 部分结果"
        return root.isUrlMode ? "LIVE · 待连接" : "就绪"
    }
    property color stateColor: (root.errorDiag && root.errorDiag.code) ? Theme.error
        : (Analysis.busy || root.summary.stopped ? Theme.accent : Theme.success)

    // ---------------- actions ----------------
    function startAnalysis() {
        root.errorDiag = ({})
        root.noticeDismissed = false
        var opts = ({
            connectTimeout: connectSpin.value,
            stallTimeout: stallSpin.value,
            probeTimeout: probeSpin.value,
            maxFrames: maxFramesSpin.value,
            decodeFrames: root.decodeOn,
            maxDecodeFrames: maxDecodeSpin.value,
            extraOptions: root.parseExtra(extraField.text)
        })
        Analysis.start(root.isUrlMode ? urlField.text.trim() : pathField.text.trim(), opts)
    }

    function openClicked() {
        if (Analysis.busy) return       // Stop is a separate button now
        if (root.isUrlMode) {
            if (urlField.text.trim().length === 0) { urlField.forceActiveFocus(); return }
        } else {
            var picked = Analysis.pickMediaFile()
            if (!picked || picked.length === 0) return   // user cancelled
            pathField.text = picked
        }
        root.startAnalysis()
    }

    function browseOnly() {
        var picked = Analysis.pickMediaFile()
        if (picked && picked.length > 0) pathField.text = picked
    }

    function applyFilter() {
        root.resetSelection()
        Analysis.setFilter(true, root.filterText)
        Analysis.setFilter(false, root.filterText)
        videoGrid.table.contentY = 0
        audioGrid.table.contentY = 0
        videoGrid.table.contentX = 0
        audioGrid.table.contentX = 0
    }

    Component.onCompleted: {
        var init = (initialInput && initialInput.length > 0) ? initialInput : ""
        if (init.length > 0) {
            if (/^[a-z]+:\/\//i.test(init) || /^\./.test(init)) {
                root.isUrlMode = true
                urlField.text = init
            } else {
                pathField.text = init
            }
            root.startAnalysis()
        }
    }

    // ---------------- reusable inline components ----------------
    Component {
        id: segmentedComponent
        Rectangle {
            width: segRow.implicitWidth + 4
            height: 30
            radius: 7
            color: Theme.sunken
            border.color: Theme.line2
            border.width: 1
            Row {
                id: segRow
                anchors.centerIn: parent
                spacing: 2
                Repeater {
                    model: ["本地文件", "网络流"]
                    Rectangle {
                        property bool active: root.isUrlMode === (index === 1)
                        width: segLabel.implicitWidth + 26
                        height: 26
                        radius: 5
                        color: active ? Theme.surface3 : "transparent"
                        Row {
                            anchors.centerIn: parent
                            spacing: 6
                            Rectangle {
                                width: 6; height: 6; radius: 3
                                color: active ? Theme.accent : Theme.text4
                                anchors.verticalCenter: parent.verticalCenter
                            }
                            Text {
                                id: segLabel
                                text: modelData
                                color: active ? Theme.text1 : Theme.text3
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                                font.bold: true
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: root.isUrlMode = (index === 1)
                        }
                    }
                }
            }
        }
    }

    Component {
        id: badgeComponent
        Rectangle {
            property string text: ""
            property bool live: false
            width: badgeText.implicitWidth + 14
            height: 20
            radius: 4
            color: live ? Theme.accentWash : Theme.surface2
            border.color: live ? Theme.accentEdge : Theme.line2
            border.width: 1
            Row {
                anchors.centerIn: parent
                spacing: 6
                Rectangle {
                    width: 6; height: 6; radius: 3
                    color: live ? Theme.accent : Theme.success
                    anchors.verticalCenter: parent.verticalCenter
                }
                Text {
                    id: badgeText
                    text: parent.parent.text
                    color: live ? Theme.accent : Theme.text2
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    font.bold: true
                    font.letterSpacing: 0.8
                }
            }
        }
    }

    // =====================================================================
    // WINDOW
    // =====================================================================
    ColumnLayout {
        anchors.fill: parent
        spacing: 0

        // ---------- 01 APP BAR ----------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 48
            color: Theme.sunken
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1; color: Theme.line
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 18
                anchors.rightMargin: 16
                spacing: 10
                Text {
                    text: "◆"
                    color: Theme.accent
                    font.pixelSize: 15
                    Layout.alignment: Qt.AlignVCenter
                }
                Text {
                    text: "媒体分析器"
                    color: Theme.text1
                    font.family: Theme.uiFont
                    font.pixelSize: 14
                    font.bold: true
                }
                Text {
                    text: "MEDIA ANALYSE"
                    color: Theme.text3
                    font.family: Theme.monoFont
                    font.pixelSize: 10
                    font.letterSpacing: 1.2
                }
                Rectangle { width: 1; height: 18; color: Theme.line2; Layout.leftMargin: 4; Layout.rightMargin: 4 }
                Text {
                    text: root.appbarTag
                    color: Theme.text3
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Item { Layout.fillWidth: true }
                PrismButton {
                    text: "配置"
                    variant: "ghost"
                    compact: true
                    onClicked: configPopup.open()
                }
            }
        }

        // ---------- BODY (padded) ----------
        ColumnLayout {
            Layout.fillWidth: true
            Layout.leftMargin: 20
            Layout.rightMargin: 20
            Layout.topMargin: 14
            spacing: 12

            // 02 SOURCE
            SectionHead { label: "SOURCE / 输入源"; rightComponent: segmentedComponent }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: root.isUrlMode ? 146 : 172
                color: Theme.surface1
                radius: 8
                border.color: Theme.line
                border.width: 1

                StackLayout {
                    id: sourceStack
                    anchors.fill: parent
                    anchors.margins: 13
                    currentIndex: root.isUrlMode ? 1 : 0

                    // ----- LOCAL FILE -----
                    ColumnLayout {
                        spacing: 10
                        Text {
                            text: "文件路径"
                            color: Theme.text2
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            font.bold: true
                        }
                        RowLayout {
                            spacing: 8
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Theme.controlHeight
                                radius: 6
                                color: Theme.sunken
                                border.color: pathField.activeFocus ? Theme.accent : Theme.line2
                                border.width: 1
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 8
                                    spacing: 8
                                    Text {
                                        text: "▤"
                                        color: Theme.text3
                                        font.pixelSize: 13
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                    TextField {
                                        id: pathField
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 32
                                        placeholderText: "点击「打开」选择文件，或直接粘贴路径后按回车"
                                        placeholderTextColor: Theme.text3
                                        color: Theme.text1
                                        font.family: Theme.monoFont
                                        font.pixelSize: 12
                                        selectByMouse: true
                                        background: Item {}
                                        onAccepted: { if (!Analysis.busy) root.startAnalysis() }
                                    }
                                    Rectangle {
                                        visible: root.fileExt(pathField.text) !== "—"
                                        implicitWidth: extText.implicitWidth + 12
                                        implicitHeight: 18
                                        radius: 3
                                        color: Theme.accentWash
                                        border.color: Theme.accentEdge
                                        Text {
                                            id: extText
                                            anchors.centerIn: parent
                                            text: root.fileExt(pathField.text)
                                            color: Theme.accent
                                            font.family: Theme.monoFont
                                            font.pixelSize: 10
                                            font.bold: true
                                        }
                                    }
                                }
                            }
                            PrismButton { text: "浏览…"; variant: "secondary"; onClicked: root.browseOnly() }
                            PrismButton {
                                text: "打开"
                                variant: "primary"
                                enabled: !Analysis.busy
                                onClicked: root.openClicked()
                            }
                        }
                        RowLayout {
                            spacing: 10
                            Text {
                                Layout.fillWidth: true
                                text: "点击「打开」选择文件（选完自动解析），或直接粘贴路径后按回车"
                                color: Theme.text3
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                                elide: Text.ElideRight
                            }
                            PrismButton {
                                variant: "toggle"
                                toggled: root.decodeOn
                                text: (root.decodeOn ? "✓  " : "     ") + "解码并解析"
                                onClicked: root.decodeOn = !root.decodeOn
                                ToolTip.text: "为前 N 帧/流补充帧级信息（视频补 dims / format，音频补 format / ch）；不改变行数和 I/P/B、samples、时间戳"
                                ToolTip.visible: hovered
                            }
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
                        RowLayout {
                            spacing: 14
                            Loader {
                                sourceComponent: badgeComponent
                                onLoaded: { item.text = "FILE"; item.live = false }
                            }
                            Text {
                                text: "● 已解析 · " + (root.hasResult ? root.streamList.length : 0) + " 条流"
                                color: Theme.text3; font.family: Theme.monoFont; font.pixelSize: 11
                            }
                            Text {
                                text: "容器 " + (root.hasResult ? root.shortFormat(root.summary.formatName) : "—")
                                color: Theme.text3; font.family: Theme.monoFont; font.pixelSize: 11
                            }
                            Text {
                                text: "索引 " + (root.summary.truncated ? "截断" : "完整")
                                color: Theme.text3; font.family: Theme.monoFont; font.pixelSize: 11
                            }
                            Item { Layout.fillWidth: true }
                            Text {
                                visible: !Analysis.busy
                                text: "离线文件 · 可随机访问任意时间点"
                                color: Theme.text4; font.family: Theme.uiFont; font.pixelSize: 11
                            }
                            PrismButton {
                                visible: Analysis.busy
                                text: Analysis.stopping ? "正在停止…" : "停止解析"
                                variant: "ghost"; compact: true
                                onClicked: Analysis.cancel()
                            }
                        }
                    }

                    // ----- NETWORK STREAM -----
                    ColumnLayout {
                        spacing: 9
                        Text {
                            text: "流地址 · 协议自动识别"
                            color: Theme.text2
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            font.bold: true
                        }
                        RowLayout {
                            spacing: 8
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Theme.controlHeight
                                radius: 6
                                color: Theme.sunken
                                border.color: (!root.proto.ok && !root.proto.empty) ? "#A05B50"
                                            : (urlField.activeFocus ? Theme.accent : Theme.line2)
                                border.width: 1
                                RowLayout {
                                    anchors.fill: parent
                                    anchors.leftMargin: 10
                                    anchors.rightMargin: 8
                                    spacing: 8
                                    Text {
                                        text: "◎"
                                        color: Theme.text3
                                        font.pixelSize: 13
                                        Layout.alignment: Qt.AlignVCenter
                                    }
                                    TextField {
                                        id: urlField
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 32
                                        placeholderText: "粘贴 rtsp:// · rtmp:// · .m3u8 · .flv 地址"
                                        placeholderTextColor: Theme.text3
                                        color: Theme.text1
                                        font.family: Theme.monoFont
                                        font.pixelSize: 12
                                        selectByMouse: true
                                        background: Item {}
                                        onAccepted: { if (!Analysis.busy) root.openClicked() }
                                    }
                                }
                            }
                            Rectangle {
                                implicitWidth: protoText.implicitWidth + 20
                                implicitHeight: Theme.controlHeight
                                radius: 6
                                color: root.proto.ok ? "#1C2731" : "#2B1D1C"
                                border.color: root.proto.ok ? "#35506A" : "#A05B50"
                                border.width: 1
                                Text {
                                    id: protoText
                                    anchors.centerIn: parent
                                    text: (root.proto.ok ? "AUTO · " : "") + root.proto.name
                                    color: root.proto.ok ? Theme.video : Theme.error
                                    font.family: Theme.monoFont
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                            }
                            PrismButton {
                                text: "解析"
                                variant: "primary"
                                enabled: !Analysis.busy
                                onClicked: root.openClicked()
                                ToolTip.text: "协议未识别时仍会交给 FFmpeg 尝试（内部排障）"
                                ToolTip.visible: hovered && !root.proto.ok
                            }
                        }
                        Text {
                            Layout.fillWidth: true
                            text: root.proto.ok
                                  ? "已识别 " + root.proto.name + " · " + root.proto.rule + "；按回车或点击「解析」开始帧解析，协议无需配置"
                                  : (root.proto.empty
                                     ? "支持 rtsp:// · rtmp:// · .m3u8 · .flv，粘贴地址后按回车即开始帧解析"
                                     : "地址协议未识别 · 仅支持 rtsp:// · rtmp:// · .m3u8 · .flv（仍可强制交给 FFmpeg 尝试）")
                            color: (!root.proto.ok && !root.proto.empty) ? Theme.error : Theme.text3
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            elide: Text.ElideRight
                        }
                        Rectangle { Layout.fillWidth: true; height: 1; color: Theme.line }
                        RowLayout {
                            spacing: 14
                            Loader {
                                sourceComponent: badgeComponent
                                onLoaded: { item.text = "LIVE"; item.live = true }
                            }
                            Text {
                                text: "自动识别 " + (root.proto.ok ? root.proto.name : "—")
                                color: Theme.text3; font.family: Theme.monoFont; font.pixelSize: 11
                            }
                            Text {
                                text: "已解析帧 " + root.group(root.summary.videoFrames || 0)
                                color: Theme.text3; font.family: Theme.monoFont; font.pixelSize: 11
                            }
                            Text {
                                text: "关键帧 " + root.group(root.summary.keyFrames || 0)
                                color: Theme.text3; font.family: Theme.monoFont; font.pixelSize: 11
                            }
                            Text {
                                text: "帧率 " + Number(root.summary.frameRate || 0).toFixed(1) + " fps"
                                color: Theme.text3; font.family: Theme.monoFont; font.pixelSize: 11
                            }
                            Item { Layout.fillWidth: true }
                            PrismButton {
                                visible: Analysis.busy
                                text: Analysis.stopping ? "正在停止…" : "停止解析"
                                variant: "ghost"; compact: true
                                onClicked: Analysis.cancel()
                            }
                        }
                    }
                }
            }

            // 03 OVERVIEW
            SectionHead { label: "OVERVIEW / 概览"; tail: root.overviewTail; tailColor: root.overviewColor }

            Rectangle {
                Layout.fillWidth: true
                Layout.preferredHeight: 82
                color: Theme.sunken
                radius: 8
                border.color: Theme.line
                border.width: 1
                RowLayout {
                    anchors.fill: parent
                    spacing: 0
                    MetricCell {
                        label: root.metrics[0].label; value: root.metrics[0].value
                        unit: root.metrics[0].unit; note: root.metrics[0].note
                        accent: root.metrics[0].accent; live: root.metrics[0].live
                        fontSize: root.metrics[0].size
                    }
                    MetricCell {
                        label: root.metrics[1].label; value: root.metrics[1].value
                        unit: root.metrics[1].unit; note: root.metrics[1].note
                        accent: root.metrics[1].accent; live: root.metrics[1].live
                        fontSize: root.metrics[1].size
                    }
                    MetricCell {
                        label: root.metrics[2].label; value: root.metrics[2].value
                        unit: root.metrics[2].unit; note: root.metrics[2].note
                        accent: root.metrics[2].accent; live: root.metrics[2].live
                        fontSize: root.metrics[2].size
                    }
                    MetricCell {
                        label: root.metrics[3].label; value: root.metrics[3].value
                        unit: root.metrics[3].unit; note: root.metrics[3].note
                        accent: root.metrics[3].accent; live: root.metrics[3].live
                        fontSize: root.metrics[3].size
                    }
                    MetricCell {
                        divider: false
                        label: root.metrics[4].label; value: root.metrics[4].value
                        unit: root.metrics[4].unit; note: root.metrics[4].note
                        accent: root.metrics[4].accent; live: root.metrics[4].live
                        fontSize: root.metrics[4].size
                    }
                }
            }

            // 04 NOTICE
            Rectangle {
                visible: root.noticeVisible
                Layout.fillWidth: true
                Layout.preferredHeight: 38
                radius: 6
                color: root.noticeBg
                border.color: root.noticeEdge
                border.width: 1
                Rectangle {
                    anchors.left: parent.left; anchors.top: parent.top
                    anchors.bottom: parent.bottom
                    width: 3
                    color: root.noticeLeft
                }
                RowLayout {
                    anchors.fill: parent
                    anchors.leftMargin: 12
                    anchors.rightMargin: 8
                    spacing: 9
                    Text {
                        text: "▲"
                        color: root.noticeBold
                        font.pixelSize: 12
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "<b>" + root.notice.title + "</b> · " + root.notice.body
                        textFormat: Text.RichText
                        color: root.noticeInk
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    PrismButton {
                        text: "详情"
                        variant: "ghost"; compact: true
                        onClicked: root.showToast(root.notice.detail)
                    }
                    PrismButton {
                        text: "×"
                        variant: "ghost"; compact: true
                        onClicked: root.noticeDismissed = true
                    }
                }
            }

            // 05 STREAMS
            SectionHead { label: "STREAMS / 流信息"; tail: root.streamList.length + " 条流" }

            Item {
                Layout.fillWidth: true
                Layout.preferredHeight: Math.max(102, Math.min(226, cardFlow.implicitHeight))
                Flow {
                    id: cardFlow
                    width: parent.width
                    spacing: 12
                    Repeater {
                        model: root.streamList
                        StreamCard {
                            width: (cardFlow.width - 12) / 2
                            stream: modelData
                        }
                    }
                    Text {
                        visible: root.streamList.length === 0
                        width: cardFlow.width
                        height: 102
                        text: "尚无流信息 · 解析后将在此显示每条流的编码、分辨率、采样率与时基"
                        color: Theme.text4
                        font.family: Theme.uiFont
                        font.pixelSize: 12
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }

        // ---------- 06 TOOLBAR (full bleed) ----------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            color: Theme.surface1
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.bottom: parent.bottom
                height: 1; color: Theme.line
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 0

                Repeater {
                    model: ["视频", "音频"]
                    Rectangle {
                        property bool active: root.tabIndex === index
                        width: tabContent.implicitWidth + 32
                        height: 44
                        color: active ? Theme.surface1 : "transparent"
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 2
                            color: active ? Theme.accent : "transparent"
                        }
                        RowLayout {
                            id: tabContent
                            anchors.centerIn: parent
                            spacing: 8
                            Text {
                                text: modelData
                                color: active ? Theme.text1 : Theme.text3
                                font.family: Theme.uiFont
                                font.pixelSize: 13
                                font.bold: active
                            }
                            Rectangle {
                                implicitWidth: tabCount.implicitWidth + 12
                                implicitHeight: 18
                                radius: 9
                                color: active ? Theme.accentWash : Theme.sunken
                                border.color: active ? Theme.accentEdge : Theme.line
                                Text {
                                    id: tabCount
                                    anchors.centerIn: parent
                                    text: root.group(index === 0 ? Analysis.videoRows : Analysis.audioRows)
                                    color: active ? Theme.accent : Theme.text3
                                    font.family: Theme.monoFont
                                    font.pixelSize: 10
                                    font.bold: true
                                }
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { root.tabIndex = index; root.resetSelection() }
                        }
                    }
                }

                Loader {
                    visible: root.isUrlMode
                    Layout.leftMargin: 6
                    sourceComponent: badgeComponent
                    onLoaded: { item.text = "LIVE"; item.live = true }
                }
                Item { Layout.fillWidth: true }

                TextField {
                    id: filterInput
                    Layout.preferredWidth: 176
                    Layout.preferredHeight: 30
                    leftPadding: 28
                    rightPadding: 8
                    placeholderText: "过滤 pkt# / type…"
                    placeholderTextColor: Theme.text3
                    color: Theme.text1
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    selectByMouse: true
                    onTextEdited: { root.filterText = filterInput.text; filterTimer.restart() }
                    background: Rectangle {
                        radius: 6
                        color: Theme.sunken
                        border.color: filterInput.activeFocus ? Theme.accent : Theme.line2
                        border.width: 1
                        Rectangle {
                            x: 10; y: 9; width: 9; height: 9; radius: 5
                            color: "transparent"; border.color: Theme.text3; border.width: 1
                        }
                        Rectangle {
                            x: 18; y: 17; width: 5; height: 1
                            rotation: 45; color: Theme.text3
                        }
                    }
                }
                Rectangle { width: 1; height: 18; color: Theme.line2; Layout.leftMargin: 10; Layout.rightMargin: 10 }
                PrismButton {
                    text: "复制选区"
                    variant: "secondary"
                    compact: true
                    enabled: root.selR0 >= 0
                    onClicked: root.copySelection()
                }
            }
        }

        // ---------- 07 DATAVIEW ----------
        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: root.tabIndex
            FrameGrid {
                id: videoGrid
                ctl: root
                isVideo: true
                model: Analysis.videoModel
            }
            FrameGrid {
                id: audioGrid
                ctl: root
                isVideo: false
                model: Analysis.audioModel
            }
        }

        // ---------- 08 STATUS ----------
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 34
            color: Theme.sunken
            Rectangle {
                anchors.left: parent.left; anchors.right: parent.right
                anchors.top: parent.top
                height: 1; color: Theme.line
            }
            RowLayout {
                anchors.fill: parent
                anchors.leftMargin: 20
                anchors.rightMargin: 20
                spacing: 8
                Text {
                    Layout.preferredWidth: 520
                    text: "拖选矩形选区 · 点 # 选整行 · Shift 连选 · Ctrl+C 导出 TSV · 滚轮滚动 / Shift 横向"
                    color: Theme.text3
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Item { Layout.fillWidth: true }
                Text {
                    text: root.selectionText
                    color: Theme.text2
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                }
                Text { text: "·"; color: Theme.text4 }
                Text {
                    text: root.rowCountText
                    color: Theme.text2
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                }
                Text { text: "·"; color: Theme.text4 }
                Rectangle {
                    width: 6; height: 6; radius: 3
                    color: root.stateColor
                }
                Text {
                    text: root.stateText
                    color: root.stateColor
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                }
            }
        }
    }

    // ---------------- overlay bits ----------------
    Shortcut {
        sequence: "Ctrl+C"
        enabled: root.selR0 >= 0 && !pathField.activeFocus &&
                 !urlField.activeFocus && !filterInput.activeFocus
        onActivated: root.copySelection()
    }

    Timer {
        id: filterTimer
        interval: 180
        onTriggered: root.applyFilter()
    }

    Rectangle {
        id: toast
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 48
        width: toastText.implicitWidth + 32
        height: 34
        radius: 7
        color: Theme.surface3
        border.color: Theme.line3
        border.width: 1
        opacity: 0
        z: 50
        Behavior on opacity { NumberAnimation { duration: 180 } }
        Text {
            id: toastText
            anchors.centerIn: parent
            color: Theme.text1
            font.family: Theme.uiFont
            font.pixelSize: 12
        }
    }
    Timer { id: toastTimer; interval: 2400; onTriggered: toast.opacity = 0 }

    // ---------------- config (progressive disclosure) ----------------
    Popup {
        id: configPopup
        anchors.centerIn: parent
        width: 470
        modal: true
        padding: 16
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        background: Rectangle {
            radius: 9
            color: Theme.surface3
            border.color: Theme.line3
            border.width: 1
        }
        contentItem: ColumnLayout {
            spacing: 12
            Text {
                text: "配置"
                color: Theme.text1
                font.family: Theme.uiFont
                font.pixelSize: 16
                font.bold: true
            }
            Text {
                Layout.fillWidth: true
                text: "仅影响下一次解析。网络流的协议由地址自动识别，这里只放排障时才需要改动的高级参数。"
                color: Theme.text3
                font.family: Theme.uiFont
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 8
                Label { text: "连接超时 (秒)"; color: Theme.text2; font.pixelSize: 12 }
                SpinBox {
                    id: connectSpin; from: 1; to: 600; value: 15; editable: true
                    palette.windowText: Theme.text1; palette.text: Theme.text1
                    palette.window: Theme.sunken; palette.base: Theme.sunken
                    palette.button: Theme.surface2; palette.buttonText: Theme.text1
                    background: Rectangle { radius: 6; color: Theme.sunken; border.color: Theme.line2 }
                }
                Label { text: "无数据超时 (秒)"; color: Theme.text2; font.pixelSize: 12 }
                SpinBox {
                    id: stallSpin; from: 1; to: 600; value: 15; editable: true
                    palette.windowText: Theme.text1; palette.text: Theme.text1
                    palette.window: Theme.sunken; palette.base: Theme.sunken
                    palette.button: Theme.surface2; palette.buttonText: Theme.text1
                    background: Rectangle { radius: 6; color: Theme.sunken; border.color: Theme.line2 }
                }
                Label { text: "探测超时 (秒, 0=自动)"; color: Theme.text2; font.pixelSize: 12 }
                SpinBox {
                    id: probeSpin; from: 0; to: 900; value: 0; editable: true
                    palette.windowText: Theme.text1; palette.text: Theme.text1
                    palette.window: Theme.sunken; palette.base: Theme.sunken
                    palette.button: Theme.surface2; palette.buttonText: Theme.text1
                    background: Rectangle { radius: 6; color: Theme.sunken; border.color: Theme.line2 }
                }
                Label { text: "最大帧数"; color: Theme.text2; font.pixelSize: 12 }
                SpinBox {
                    id: maxFramesSpin; from: 1000; to: 10000000; stepSize: 10000; value: 200000
                    editable: true
                    palette.windowText: Theme.text1; palette.text: Theme.text1
                    palette.window: Theme.sunken; palette.base: Theme.sunken
                    palette.button: Theme.surface2; palette.buttonText: Theme.text1
                    background: Rectangle { radius: 6; color: Theme.sunken; border.color: Theme.line2 }
                }
                Label { text: "最大解码帧数 / 流"; color: Theme.text2; font.pixelSize: 12 }
                SpinBox {
                    id: maxDecodeSpin; from: 100; to: 1000000; stepSize: 1000; value: 3000
                    editable: true
                    palette.windowText: Theme.text1; palette.text: Theme.text1
                    palette.window: Theme.sunken; palette.base: Theme.sunken
                    palette.button: Theme.surface2; palette.buttonText: Theme.text1
                    background: Rectangle { radius: 6; color: Theme.sunken; border.color: Theme.line2 }
                }
            }
            Text {
                text: "FFmpeg 高级选项  K=V，逗号 / 分号 / 换行分隔"
                color: Theme.text2
                font.family: Theme.uiFont
                font.pixelSize: 11
                font.bold: true
            }
            TextField {
                id: extraField
                Layout.fillWidth: true
                Layout.preferredHeight: 32
                placeholderText: "rtsp_transport=tcp, reconnect=1"
                placeholderTextColor: Theme.text4
                color: Theme.text1
                font.family: Theme.monoFont
                font.pixelSize: 11
                selectByMouse: true
                background: Rectangle {
                    radius: 6; color: Theme.sunken
                    border.color: extraField.activeFocus ? Theme.accent : Theme.line2
                }
            }
            RowLayout {
                spacing: 6
                Text { text: "快捷预设"; color: Theme.text3; font.pixelSize: 11 }
                PrismButton { text: "清空"; variant: "secondary"; compact: true; onClicked: extraField.text = "" }
                PrismButton {
                    text: "RTSP over TCP"; variant: "secondary"; compact: true
                    onClicked: extraField.text = "rtsp_transport=tcp"
                }
                PrismButton {
                    text: "自动重连"; variant: "secondary"; compact: true
                    onClicked: extraField.text = "reconnect=1, reconnect_streamed=1, rw_timeout=10000000"
                }
            }
            RowLayout {
                spacing: 8
                Item { Layout.fillWidth: true }
                PrismButton {
                    text: "确定"
                    variant: "primary"
                    onClicked: configPopup.close()
                }
            }
        }
    }

    Connections {
        target: Analysis
        function onAnalysisSucceeded(summary, streams) {
            root.resetSelection()
            root.filterText = ""
            filterInput.text = ""
            Analysis.setFilter(true, "")
            Analysis.setFilter(false, "")
            videoGrid.table.contentY = 0
            audioGrid.table.contentX = 0
            audioGrid.table.contentY = 0
            videoGrid.table.contentX = 0
            root.summary = summary
            root.streamList = streams
            root.errorDiag = ({})
            root.noticeDismissed = false
        }
        // A real failure is an inline notice + status state, never a modal
        // dialog. Stopping / cancelling never reaches this handler.
        function onAnalysisFailed(diag) {
            root.errorDiag = diag
            root.noticeDismissed = false
            root.showToast("解析失败 · " + (diag.message || diag.code))
        }
    }
}
