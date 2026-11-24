import QtQuick
import QtQuick.Controls.Basic
import QtQuick.Layouts
import MediaAnalyse

/*
 * PRISM - "precision instrument" UI port for Qt Quick.
 * Reference: media-analyzer-redesign/design-preview.html + qt-preview*.png
 * Reading order: APP BAR > SOURCE > SUMMARY (one collapsible group holding
 *                OVERVIEW + STREAMS; NOTICE stays always visible) >
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
    property bool cstoreMode: false
    // Single-source analysis vs. dual-source comparison. The toggle is in the
    // app bar; dual mode runs both inputs through ONE worker and keeps two
    // independent frame stores/tables.
    property bool compareMode: false
    property bool compareCstoreMode: false
    property bool audioAlignOn: false
    property int activeSource: 0          // 0 = A (primary), 1 = B (compare)
    property string fetchText: ""
    property int tabIndex: 0
    property var streamList: []
    property var summary: ({})
    property var streamListB: []
    property var summaryB: ({})
    property var errorDiagB: ({})
    property bool anyBusy: Analysis.busy || (root.compareMode && AnalysisB.busy)
    property var activeAnalysis: (root.compareMode && root.activeSource === 1)
                                 ? AnalysisB : Analysis
    property var activeSummary: (root.compareMode && root.activeSource === 1)
                                ? root.summaryB : root.summary
    property var activeStreamList: (root.compareMode && root.activeSource === 1)
                                   ? root.streamListB : root.streamList
    // HLS timeline / A-V alignment report, filled only by a finished result.
    // Live snapshots deliberately leave it empty, so the card stays hidden
    // until there is a complete timeline to judge.
    property var alignment: (root.summary && root.summary.alignment)
                            ? root.summary.alignment : ({})
    // Cross-file comparison, present only when a second file was analyzed.
    property var comparison: (root.summary && root.summary.comparison)
                             ? root.summary.comparison : ({})
    // Unified offset curve: prefer the time-varying audio track when present,
    // otherwise fall back to the content-signature window curve.
    property var comparisonWindows: (root.comparison.hasAudioTrack &&
                                     (root.comparison.audioWindows || []).length > 1)
                                    ? root.comparison.audioWindows
                                    : (root.comparison.windows || [])
    property var alignmentB: (root.summaryB && root.summaryB.alignment)
                             ? root.summaryB.alignment : ({})
    property var errorDiag: ({})
    property bool noticeDismissed: false
    property bool decodeOn: false
    property string filterText: ""
    // Once the user toggles the SUMMARY panel by hand, automatic
    // small-window folding stops overriding their choice.
    property bool summaryUserSet: false
    // Digest printed on the collapsed header so folding the detail area never
    // hides the essential state (source / stream count / packets).
    property string summaryDigest: {
        var s = root.summary || ({})
        var src = root.isUrlMode
            ? (root.proto.ok ? root.proto.name : "网络流")
            : (s.formatName ? root.shortFormat(s.formatName) : "本地文件")
        var d = src + " · " + root.streamList.length + " 条流 · " +
                root.group(s.totalPackets || 0) + " 包"
        if (root.compareMode)
            d += " · A/B " + root.streamList.length + "/" +
                 root.streamListB.length + " 流"
        return d
    }

    function toggleSummary() {
        summaryPanel.expanded = !summaryPanel.expanded
        root.summaryUserSet = true
    }

    function toggleCompareMode() {
        if (root.anyBusy) return
        root.compareMode = !root.compareMode
        root.activeSource = 0
        root.resetSelection()
        if (root.compareMode)
            root.showToast("多流对比：填写 A / B 两路输入后点击「对比解析」")
    }

    function rowsForSource(src, video) {
        var c = (src === 1) ? AnalysisB : Analysis
        return video ? c.videoRows : c.audioRows
    }
    property var proto: root.isUrlMode ? root.detectProto(urlField.text)
                                       : { name: "", rule: "", ok: false, empty: true }
    property var activeProto: (root.compareMode && root.activeSource === 1)
        ? (root.compareCstoreMode
           ? { name: "cstore", rule: "录制分片回捞", ok: true, empty: false }
           : root.detectProto(compareField.text))
        : root.proto

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
        root.activeAnalysis.copySelection(root.selOnVideo, root.selR0, root.selR1,
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
    function compareMethodLabel(m) {
        if (m === "audio-xcorr") return "audio-xcorr / 音频互相关"
        if (m === "pdt") return "pdt / PDT 墙上时钟"
        if (m === "manual-anchor") return "manual-anchor / 人工锚点"
        if (m === "content-signature") return "content-signature / 关键帧签名"
        if (m === "segment-pattern") return "segment-pattern / 分片模式"
        if (m === "relative") return "relative / 相对比较"
        return m || "-"
    }

    function group(n) {
        return String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ",")
    }
    function signedGroup(n) {
        var v = Math.round(Number(n || 0))
        return (v > 0 ? "+" : "") + root.group(v)
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
    property bool activeHasResult: root.activeSummary &&
                                   root.activeSummary.formatName !== undefined
    property string appbarTag: root.isUrlMode
        ? (root.proto.ok ? root.proto.name + " 实时流 · 帧解析中" : "地址协议未识别")
        : "本地文件 · 离线解析"

    function computeMetrics() {
        var s = root.activeSummary || ({})
        var p = root.activeProto || ({})
        var has = root.hasResult
        var br = Number(s.bitrate || 0)
        var elapsed = Number(s.elapsedSeconds || 0)
        var rate = (has && elapsed > 0) ? root.group(Math.round(Number(s.totalPackets || 0) / elapsed)) : "0"
        if (root.isUrlMode) {
            return [
                { label: "时长", value: "LIVE", unit: "", note: "持续流 · 无固定时长", accent: false, live: true, size: 20 },
                { label: "平均码率", value: br > 0 ? (br / 1000000).toFixed(2) : "—", unit: "Mb/s", note: "容器 / 逐包统计", accent: false, live: false, size: 20 },
                { label: "已解析包数", value: has ? root.group(s.totalPackets || 0) : "0", unit: "", note: root.anyBusy ? "持续增长" : (s.stopped ? "已停止" : "—"), accent: false, live: false, size: 20 },
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
        var activeDiag = (root.compareMode && root.activeSource === 1)
                         ? root.errorDiagB : root.errorDiag
        if (activeDiag && activeDiag.code)
            return (root.compareMode ? (root.activeSource === 1 ? "B " : "A ") : "") +
                   "解析失败 · " + activeDiag.code
        if (root.anyBusy) return root.fetchText !== ""
            ? root.fetchText : (root.compareMode ? "对比解析中 · 两路独立帧表" :
                                ("解析中 · " + Analysis.statusText))
        if (root.compareMode)
            return root.activeHasResult
                ? (root.activeSource === 1 ? "B " : "A ") + "解析完成 · " +
                  root.group(root.activeSummary.totalPackets || 0) + " packets in " +
                  Number(root.activeSummary.elapsedSeconds || 0).toFixed(2) + " s"
                : "等待 A / B 两路输入"
        if (root.isUrlMode)
            return root.proto.ok ? "实时解析 · " + Number(root.summary.frameRate || 0).toFixed(1) + " fps"
                                 : "等待有效的流地址"
        return root.hasResult
            ? "解析完成 · " + root.group(root.summary.totalPackets || 0) + " packets in " + Number(root.summary.elapsedSeconds || 0).toFixed(2) + " s"
            : "等待输入"
    }
    property color overviewColor: {
        var activeDiag = (root.compareMode && root.activeSource === 1)
                         ? root.errorDiagB : root.errorDiag
        if (activeDiag && activeDiag.code) return Theme.error
        if (root.anyBusy) return Theme.accent
        return (root.activeHasResult || !root.isUrlMode) ? Theme.success : Theme.text4
    }

    function computeNotice() {
        var s = root.summary || ({})
        var p = root.proto || ({})
        if (root.compareMode && root.errorDiagB && root.errorDiagB.code) {
            var b = root.errorDiagB
            return {
                kind: "error", title: "B 源解析失败",
                body: (b.message || "") + (b.hint ? " · " + b.hint : ""),
                detail: "阶段 " + (b.stage || "-") + " · 错误码 " +
                        (b.code || "-") + " (" + (b.nativeCode || 0) + ") · 耗时 " +
                        Number(b.elapsedSeconds || 0).toFixed(2) + " s · " + (b.url || "")
            }
        }
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
    property string rowCountText: (root.tabIndex === 0
                                   ? root.activeAnalysis.videoRows
                                   : root.activeAnalysis.audioRows)
                                  + " 行 · " + Analysis.columnCountFor(true) + " 列" +
                                  (root.filterText.length > 0 ? "（已过滤）" : "")
    property string stateText: {
        if (root.errorDiag && root.errorDiag.code) return "解析失败"
        if (root.compareMode) {
            if (root.anyBusy) return Analysis.stopping ? "正在停止两路解析" : "对比解析中"
            if (root.errorDiagB && root.errorDiagB.code) return "B 解析失败"
            if (root.errorDiag && root.errorDiag.code) return "A 解析失败"
            return root.activeHasResult ? "A/B 就绪 · " + (root.activeSource === 1 ? "B" : "A") + " 表" : "A/B 就绪"
        }
        if (root.anyBusy) return Analysis.stopping ? "正在停止" :
            (root.isUrlMode ? "LIVE · 帧解析中" : "解析中")
        if (root.summary.stopped) return "已停止 · 部分结果"
        return root.isUrlMode ? "LIVE · 待连接" : "就绪"
    }
    property color stateColor: (root.errorDiag && root.errorDiag.code ||
                                root.compareMode && root.errorDiagB && root.errorDiagB.code) ? Theme.error
        : (root.anyBusy || root.summary.stopped ? Theme.accent : Theme.success)

    // ---------------- actions ----------------
    function startAnalysis() {
        if (root.compareMode && compareField.text.trim().length === 0) {
            compareField.forceActiveFocus()
            root.showToast("请先填写第二路流或文件路径")
            return
        }
        root.errorDiag = ({})
        root.errorDiagB = ({})
        root.fetchText = ""
        root.noticeDismissed = false
        if (root.compareMode) {
            root.summaryB = ({})
            root.streamListB = []
        }
        var opts = ({
            connectTimeout: connectSpin.value,
            stallTimeout: stallSpin.value,
            probeTimeout: probeSpin.value,
            maxFrames: maxFramesSpin.value,
            decodeFrames: root.decodeOn,
            maxDecodeFrames: maxDecodeSpin.value,
            extraOptions: root.parseExtra(extraField.text),
            source: root.cstoreMode ? "cstore" : "",
            fetchDir: fetchDirField.text.trim(),
            fetchJobs: fetchJobsSpin.value,
            compareInput: compareField.text.trim(),
            compareSource: root.compareCstoreMode ? "cstore" : "",
            compareFullOutput: root.compareMode,
            hasAnchorMs: (root.compareMode ? compareAnchorField.text.trim()
                                           : anchorField.text.trim()).length > 0,
            anchorMs: Number((root.compareMode ? compareAnchorField.text
                                               : anchorField.text).trim()),
            audioAlign: root.compareMode && root.audioAlignOn,
            audioMaxSeconds: 7200,
            audioMaxLagSeconds: 30,
            audioWindowSeconds: 10,
            audioLocalSearchSeconds: 4
        })
        var input = root.cstoreMode ? urlField.text.trim()
                                    : (root.isUrlMode ? urlField.text.trim()
                                                      : pathField.text.trim())
        Analysis.start(input, opts)
    }

    function stopAll() {
        if (!root.anyBusy) return
        // The primary controller's cancel also flips the secondary controller
        // into the stopping state; one cooperative cancel stops both inputs.
        Analysis.cancel()
    }

    function openClicked() {
        if (root.anyBusy) return        // Stop is a separate button now
        if (root.isUrlMode || root.cstoreMode) {
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

    function browseCompareOnly() {
        var picked = Analysis.pickMediaFile()
        if (picked && picked.length > 0) compareField.text = picked
    }

    function applyFilter() {
        root.resetSelection()
        Analysis.setFilter(true, root.filterText)
        Analysis.setFilter(false, root.filterText)
        AnalysisB.setFilter(true, root.filterText)
        AnalysisB.setFilter(false, root.filterText)
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
                PrismButton {
                    Layout.leftMargin: 6
                    compact: true
                    variant: "toggle"
                    toggled: root.compareMode
                    text: root.compareMode ? "◉ 多流对比" : "◌ 单流分析"
                    onClicked: root.toggleCompareMode()
                    ToolTip.text: "切换单流分析与 A/B 双流（双文件）对比"
                    ToolTip.visible: hovered
                }
                Rectangle { width: 1; height: 18; color: Theme.line2; Layout.leftMargin: 4; Layout.rightMargin: 4 }
                Text {
                    text: root.compareMode ? "A/B 双源 · 独立帧表" : root.appbarTag
                    color: Theme.text3
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }
                Item { Layout.fillWidth: true }
                PrismButton {
                    text: "日志"
                    variant: "ghost"
                    compact: true
                    onClicked: {
                        var d = Analysis.logFolder()
                        Analysis.openLogFolder()
                        root.showToast(d.length > 0 ? "日志目录：" + d : "日志未启用")
                    }
                }
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
                Layout.preferredHeight: root.compareMode
                                        ? (root.isUrlMode ? 232 : 258)
                                        : (root.isUrlMode ? 146 : 172)
                color: Theme.surface1
                radius: 8
                border.color: Theme.line
                border.width: 1

                StackLayout {
                    id: sourceStack
                    anchors.fill: parent
                    anchors.margins: 13
                    anchors.bottomMargin: root.compareMode ? 82 : 13
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
                                        onAccepted: { if (!root.anyBusy) root.startAnalysis() }
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
                                enabled: !root.anyBusy
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
                                visible: !root.anyBusy
                                text: "离线文件 · 可随机访问任意时间点"
                                color: Theme.text4; font.family: Theme.uiFont; font.pixelSize: 11
                            }
                            PrismButton {
                                visible: root.anyBusy
                                text: Analysis.stopping ? "正在停止…" : "停止解析"
                                variant: "ghost"; compact: true
                                onClicked: root.stopAll()
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
                                        onAccepted: { if (!root.anyBusy) root.openClicked() }
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
                                enabled: !root.anyBusy
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
                                visible: root.anyBusy
                                text: Analysis.stopping ? "正在停止…" : "停止解析"
                                variant: "ghost"; compact: true
                                onClicked: root.stopAll()
                            }
                        }
                    }
                }

                // B 路输入：多流对比模式专用。A 继续保持原有的本地文件 /
                // 网络流切换，B 接受本地文件或任意流地址，也可切换为 cstore 键。
                ColumnLayout {
                    visible: root.compareMode
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.leftMargin: 13
                    anchors.rightMargin: 13
                    anchors.bottomMargin: 11
                    spacing: 6
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 8
                        Rectangle {
                            width: 20; height: 20; radius: 4
                            color: Theme.video; border.color: Theme.accentEdge
                            Text { anchors.centerIn: parent; text: "B"; color: Theme.surface1; font.pixelSize: 11; font.bold: true }
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: Theme.controlHeight
                            radius: 6
                            color: Theme.sunken
                            border.color: compareField.activeFocus ? Theme.accent : Theme.line2
                            border.width: 1
                            RowLayout {
                                anchors.fill: parent
                                anchors.leftMargin: 10
                                anchors.rightMargin: 8
                                spacing: 8
                                Text { text: "◎"; color: Theme.text3; font.pixelSize: 13 }
                                TextField {
                                    id: compareField
                                    Layout.fillWidth: true
                                    Layout.preferredHeight: 32
                                    placeholderText: root.compareCstoreMode
                                        ? "第二路 cstore 对象键（如 seewo-edulyse/xxx.m3u8）"
                                        : "第二路：本地文件路径或 rtsp:// · rtmp:// · .m3u8 · .flv"
                                    placeholderTextColor: Theme.text3
                                    color: Theme.text1
                                    font.family: Theme.monoFont
                                    font.pixelSize: 12
                                    selectByMouse: true
                                    background: Item {}
                                    onAccepted: { if (!root.anyBusy) root.startAnalysis() }
                                }
                            }
                        }
                        PrismButton {
                            compact: true; variant: "toggle"
                            toggled: root.compareCstoreMode
                            text: root.compareCstoreMode ? "✓ cstore" : "cstore"
                            onClicked: root.compareCstoreMode = !root.compareCstoreMode
                            ToolTip.text: "第二路也走分片回捞；关闭时按普通文件/流地址处理"
                            ToolTip.visible: hovered
                        }
                        PrismButton {
                            compact: true; variant: "toggle"
                            toggled: root.audioAlignOn
                            text: root.audioAlignOn ? "✓ 音频对齐" : "音频对齐"
                            onClicked: root.audioAlignOn = !root.audioAlignOn
                            ToolTip.text: "两路共用同一声场时（如同一教室前后机位），按音频互相关对齐；优先于人工锚点"
                            ToolTip.visible: hovered
                        }
                        TextField {
                            id: compareAnchorField
                            Layout.preferredWidth: 96
                            Layout.preferredHeight: Theme.controlHeight
                            placeholderText: "锚点 ms"
                            placeholderTextColor: Theme.text4
                            color: Theme.text1
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                            selectByMouse: true
                            validator: DoubleValidator { }
                            ToolTip.text: "无 PDT 且帧签名也无法匹配时，填两路共同事件的毫秒偏移（B 晚于 A 为正）"
                            ToolTip.visible: hovered
                            background: Rectangle {
                                radius: 6; color: Theme.sunken
                                border.color: compareAnchorField.activeFocus ? Theme.accent : Theme.line2
                            }
                        }
                        PrismButton {
                            text: "浏览…"; variant: "secondary"; compact: true
                            onClicked: root.browseCompareOnly()
                        }
                        PrismButton {
                            text: "对比解析"; variant: "primary"
                            enabled: !root.anyBusy
                            onClicked: root.startAnalysis()
                        }
                    }
                    Text {
                        Layout.fillWidth: true
                        text: "A / B 独立解析并分别输出帧表；「A / B」切源、「视频 / 音频」切帧类型。对齐优先级：PDT → 音频互相关 → 人工锚点。"
                        color: Theme.text3; font.family: Theme.uiFont; font.pixelSize: 11
                        elide: Text.ElideRight
                    }
                }
            }

            // 03+05 SUMMARY / 概览与流信息 —— 同一个可折叠分组。
            // 收起后 DATAVIEW (Layout.fillHeight) 自动占满腾出的高度，
            // 小窗口下帧表不再被指标条 / 流卡片挤到只剩几行。
            ColumnLayout {
                id: summaryPanel
                Layout.fillWidth: true
                spacing: 12
                property bool expanded: true

                Component.onCompleted: {
                    if (root.height < 780) summaryPanel.expanded = false
                }
                // Until the user toggles by hand, keep auto-folding on small
                // windows so the frame table always has room.
                Connections {
                    target: root
                    function onHeightChanged() {
                        if (!root.summaryUserSet)
                            summaryPanel.expanded = root.height >= 780
                    }
                }

                // 折叠头：整行可点击；收起时仍显示来源 / 流数 / 包数摘要
                Rectangle {
                    id: summaryHeader
                    Layout.fillWidth: true
                    Layout.preferredHeight: 34
                    radius: 6
                    color: headerMouse.containsMouse ? Theme.surface2 : Theme.sunken
                    border.color: headerMouse.containsMouse ? Theme.line2 : Theme.line
                    border.width: 1
                    RowLayout {
                        anchors.fill: parent
                        anchors.leftMargin: 12
                        anchors.rightMargin: 12
                        spacing: 9
                        Text {
                            text: summaryPanel.expanded ? "▾" : "▸"
                            color: Theme.accent
                            font.pixelSize: 12
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Text {
                            text: "SUMMARY / 概览与流信息"
                            color: Theme.text2
                            font.family: Theme.uiFont
                            font.pixelSize: 10
                            font.bold: true
                            font.letterSpacing: 1.4
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.preferredHeight: 1
                            Layout.alignment: Qt.AlignVCenter
                            color: Theme.line
                        }
                        Text {
                            visible: !summaryPanel.expanded
                            text: root.summaryDigest
                            color: Theme.text3
                            font.family: Theme.monoFont
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            Layout.maximumWidth: 420
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Text {
                            visible: summaryPanel.expanded
                            text: root.streamList.length + " 条流"
                            color: Theme.text4
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            Layout.alignment: Qt.AlignVCenter
                        }
                        Text {
                            text: summaryPanel.expanded ? "收起" : "展开"
                            color: Theme.accent
                            font.family: Theme.uiFont
                            font.pixelSize: 11
                            Layout.alignment: Qt.AlignVCenter
                        }
                    }
                    MouseArea {
                        id: headerMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.toggleSummary()
                            console.log("[SUMMARY] expanded=" + summaryPanel.expanded)
                        }
                    }
                }

                // OVERVIEW 内容（随分组一起折叠）
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: summaryPanel.expanded
                    spacing: 12
                    SectionHead {
                        label: "OVERVIEW / 概览"
                        tail: root.overviewTail
                        tailColor: root.overviewColor
                    }
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
                }

                // DUAL SOURCE：A/B 两路各自的状态摘要，点击卡片切换帧表数据源。
                ColumnLayout {
                    visible: root.compareMode && summaryPanel.expanded
                    Layout.fillWidth: true
                    spacing: 8
                    SectionHead {
                        label: "DUAL SOURCE / 双路状态"
                        tail: "点击 A / B 卡片切换帧表"
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 10
                        Repeater {
                            model: 2
                            Rectangle {
                                id: sourceCard
                                Layout.fillWidth: true
                                Layout.preferredHeight: 66
                                radius: 7
                                color: (root.activeSource === index) ? Theme.surface2 : Theme.sunken
                                border.color: (root.activeSource === index) ? Theme.accentEdge : Theme.line
                                border.width: 1
                                property bool isB: index === 1
                                property var s: isB ? root.summaryB : root.summary
                                property var sl: isB ? root.streamListB : root.streamList
                                property var diag: isB ? root.errorDiagB : root.errorDiag
                                ColumnLayout {
                                    anchors.fill: parent
                                    anchors.margins: 9
                                    spacing: 3
                                    RowLayout {
                                        Layout.fillWidth: true
                                        Rectangle {
                                            width: 16; height: 16; radius: 3
                                            color: sourceCard.isB ? Theme.video : Theme.accent
                                            Text { anchors.centerIn: parent; text: sourceCard.isB ? "B" : "A"; color: Theme.surface1; font.pixelSize: 10; font.bold: true }
                                        }
                                        Text {
                                            text: sourceCard.isB
                                                  ? (root.compareCstoreMode ? "第二路 · cstore" : "第二路 · 文件/流")
                                                  : (root.cstoreMode ? "第一路 · cstore" : (root.isUrlMode ? "第一路 · 网络流" : "第一路 · 本地文件"))
                                            color: Theme.text2; font.family: Theme.uiFont; font.pixelSize: 11; font.bold: true
                                        }
                                        Item { Layout.fillWidth: true }
                                        Text {
                                            text: (sourceCard.diag && sourceCard.diag.code)
                                                  ? "失败 " + sourceCard.diag.code
                                                  : ((sourceCard.s && sourceCard.s.formatName !== undefined)
                                                     ? "完成" : (root.anyBusy ? "解析中" : "等待"))
                                            color: (sourceCard.diag && sourceCard.diag.code)
                                                   ? Theme.error : Theme.text3
                                            font.family: Theme.monoFont; font.pixelSize: 10
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.group(sourceCard.s ? (sourceCard.s.totalPackets || 0) : 0) +
                                              " 包 · " + sourceCard.sl.length + " 流 · " +
                                              root.group(sourceCard.s ? (sourceCard.s.videoFrames || 0) : 0) + " 视频帧 / " +
                                              root.group(sourceCard.s ? (sourceCard.s.audioFrames || 0) : 0) + " 音频帧"
                                        color: Theme.text3; font.family: Theme.monoFont; font.pixelSize: 10
                                        elide: Text.ElideRight
                                    }
                                }
                                MouseArea {
                                    anchors.fill: parent
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: { root.activeSource = index; root.resetSelection() }
                                }
                            }
                        }
                    }
                }

                // 04 NOTICE —— 常显，不随折叠隐藏：警告 / 错误 / 停止提示
                // 属于全局状态，收起概览时仍必须可见。
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

                // STREAMS 内容（随同一个分组折叠）
                ColumnLayout {
                    Layout.fillWidth: true
                    visible: summaryPanel.expanded
                    spacing: 12
                    SectionHead {
                        label: "STREAMS / 流信息" + (root.compareMode ? " · " + (root.activeSource === 1 ? "B" : "A") : "")
                        tail: root.activeStreamList.length + " 条流"
                    }
                    Item {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(102, Math.min(226, cardFlow.implicitHeight))
                        Flow {
                            id: cardFlow
                            width: parent.width
                            spacing: 12
                            Repeater {
                                model: root.activeStreamList
                                StreamCard {
                                    width: (cardFlow.width - 12) / 2
                                    stream: modelData
                                }
                            }
                            Text {
                                visible: root.activeStreamList.length === 0
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

                // ALIGNMENT / 对齐诊断 —— 与概览、流信息同属一个可折叠分组。
                // PDT 是可选锚点：有则用绝对墙上时钟，无则明确显示「相对」。
                ColumnLayout {
                    id: alignmentPanel
                    Layout.fillWidth: true
                    visible: summaryPanel.expanded &&
                             root.alignment.timeline !== undefined &&
                             root.alignment.timeline !== ""
                    spacing: 8
                    // Details start collapsed: a full diagnostic card would
                    // push the frame table out of view on a small window, which
                    // is exactly what the SUMMARY folding feature is for.
                    property bool alignExpanded: false

                    Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: 34
                        radius: 6
                        color: alignHeadMouse.containsMouse ? Theme.surface2 : Theme.sunken
                        border.color: alignHeadMouse.containsMouse ? Theme.line2 : Theme.line
                        border.width: 1
                        RowLayout {
                            anchors.fill: parent
                            anchors.leftMargin: 12
                            anchors.rightMargin: 12
                            spacing: 9
                            Text {
                                text: alignmentPanel.alignExpanded ? "▾" : "▸"
                                color: Theme.accent
                                font.pixelSize: 12
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                text: "ALIGNMENT / 对齐诊断"
                                color: Theme.text2
                                font.family: Theme.uiFont
                                font.pixelSize: 10
                                font.bold: true
                                font.letterSpacing: 1.4
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Text {
                                text: (root.alignment.timeline || "-") + " · " +
                                      (root.alignment.crossFile ? "PDT 绝对时钟" : "相对时间轴")
                                color: root.alignment.crossFile ? Theme.success : Theme.accent
                                font.family: Theme.monoFont
                                font.pixelSize: 11
                                Layout.alignment: Qt.AlignVCenter
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: 1
                                Layout.alignment: Qt.AlignVCenter
                                color: Theme.line
                            }
                            Text {
                                text: (root.alignment.errorCount || 0) + " 错误 · " +
                                      (root.alignment.warningCount || 0) + " 警告"
                                color: (root.alignment.errorCount || 0) > 0
                                       ? Theme.error : Theme.text3
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                                Layout.alignment: Qt.AlignVCenter
                            }
                        }
                        MouseArea {
                            id: alignHeadMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: alignmentPanel.alignExpanded =
                                       !alignmentPanel.alignExpanded
                        }
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        visible: alignmentPanel.alignExpanded
                        spacing: 8
                        Rectangle {
                        Layout.fillWidth: true
                        Layout.preferredHeight: alignBody.implicitHeight + 20
                        color: Theme.sunken
                        radius: 8
                        border.color: Theme.line
                        border.width: 1
                        ColumnLayout {
                            id: alignBody
                            anchors.left: parent.left
                            anchors.right: parent.right
                            anchors.top: parent.top
                            anchors.margins: 10
                            spacing: 6

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 14
                                Text {
                                    text: "时间基准  " + (root.alignment.timeline || "-")
                                    color: Theme.text1
                                    font.family: Theme.monoFont
                                    font.pixelSize: 11
                                }
                                Text {
                                    text: "跨文件对齐  " +
                                          (root.alignment.crossFile ? "可 · PDT 绝对时钟"
                                                                    : "不可 · 相对时间轴")
                                    color: root.alignment.crossFile ? Theme.success
                                                                    : Theme.accent
                                    font.family: Theme.uiFont
                                    font.pixelSize: 11
                                }
                                Text {
                                    text: "覆盖  " +
                                          Number(root.alignment.coveredSeconds || 0).toFixed(3) + " s"
                                    color: Theme.text3
                                    font.family: Theme.monoFont
                                    font.pixelSize: 11
                                }
                                Item { Layout.fillWidth: true }
                                Text {
                                    text: "分片 " + (root.alignment.segmentCount || 0) +
                                          " · PDT " + (root.alignment.pdtCount || 0)
                                    color: Theme.text3
                                    font.family: Theme.monoFont
                                    font.pixelSize: 11
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: (root.alignment.firstWallClock || "").length > 0
                                text: "墙上时钟  " + (root.alignment.firstWallClock || "") +
                                      " → " + (root.alignment.lastWallClock || "")
                                color: Theme.video
                                font.family: Theme.monoFont
                                font.pixelSize: 11
                            }
                            Text {
                                Layout.fillWidth: true
                                text: root.alignment.note || ""
                                color: Theme.text3
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                            }
                            Repeater {
                                model: root.alignment.streams || []
                                Text {
                                    Layout.fillWidth: true
                                    text: "流 #" + modelData.stream + " (" + modelData.kind +
                                          ")  起始偏移 " +
                                          Number(modelData.startOffsetMs || 0).toFixed(1) +
                                          " ms · 时钟漂移 " +
                                          Number(modelData.driftMsPerMin || 0).toFixed(1) +
                                          " ms/min · 最大空档 " +
                                          Number(modelData.maxGapSeconds || 0).toFixed(3) + " s"
                                    color: Theme.text2
                                    font.family: Theme.monoFont
                                    font.pixelSize: 11
                                }
                            }
                            Repeater {
                                model: Math.min(6, (root.alignment.issues || []).length)
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Text {
                                        text: "[" + root.alignment.issues[index].severity + "] " +
                                              root.alignment.issues[index].code
                                        color: root.alignment.issues[index].severity === "error"
                                               ? Theme.error
                                               : (root.alignment.issues[index].severity === "warning"
                                                  ? Theme.accent : Theme.text3)
                                        font.family: Theme.monoFont
                                        font.pixelSize: 11
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.alignment.issues[index].message
                                        color: Theme.text2
                                        font.family: Theme.uiFont
                                        font.pixelSize: 11
                                        elide: Text.ElideRight
                                    }
                                }
                            }
                            Text {
                                Layout.fillWidth: true
                                visible: (root.alignment.issues || []).length === 0
                                text: "未发现分片缺失、未标注的不连续点或明显音画偏移"
                                color: Theme.success
                                font.family: Theme.uiFont
                                font.pixelSize: 11
                            }

                            // Cross-file comparison block (only when a second
                            // file was analyzed). Shows method, offset and the
                            // confidence note so a low-confidence relative
                            // result is never mistaken for an exact one.
                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: cmpBody.implicitHeight + 16
                                visible: (root.comparison.method || "").length > 0
                                color: Theme.surface1
                                radius: 6
                                border.color: Theme.line2
                                border.width: 1
                                ColumnLayout {
                                    id: cmpBody
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: 8
                                    spacing: 4
                                    Text {
                                        Layout.fillWidth: true
                                        text: root.compareMode ? "COMPARISON / 双路对比"
                                                               : "COMPARISON / 两文件对齐"
                                        color: Theme.text2
                                        font.family: Theme.uiFont
                                        font.pixelSize: 10
                                        font.bold: true
                                        font.letterSpacing: 1.2
                                    }
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 12
                                        Text {
                                            text: "方法  " + root.compareMethodLabel(root.comparison.method)
                                            color: root.comparison.confidence >= 1.0
                                                   ? Theme.success
                                                   : (root.comparison.confidence >= 0.5
                                                      ? Theme.accent : Theme.error)
                                            font.family: Theme.monoFont
                                            font.pixelSize: 11
                                        }
                                        Text {
                                            text: "偏移  " +
                                                  Number(root.comparison.offsetMs || 0).toFixed(1) + " ms"
                                            color: Theme.text1
                                            font.family: Theme.monoFont
                                            font.pixelSize: 11
                                        }
                                        Text {
                                            text: "置信度  " +
                                                  Number(root.comparison.confidence || 0).toFixed(2)
                                            color: Theme.text3
                                            font.family: Theme.monoFont
                                            font.pixelSize: 11
                                        }
                                        Item { Layout.fillWidth: true }
                                        Text {
                                            text: "重叠  " +
                                                  Number(root.comparison.overlapSeconds || 0).toFixed(3) + " s"
                                            color: Theme.text3
                                            font.family: Theme.monoFont
                                            font.pixelSize: 11
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        text: (root.comparison.note || "") +
                                              (Number(root.comparison.audioPeakRatio || 0) > 0
                                               ? " · 互相关峰值 " +
                                                 Number(root.comparison.audioPeakRatio).toFixed(1) + "×"
                                               : "")
                                        color: Theme.text3
                                        font.family: Theme.uiFont
                                        font.pixelSize: 11
                                        wrapMode: Text.WordWrap
                                    }

                                    // 逐时段画面领先列表：把“谁快了几秒”直接写成文字。
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 4
                                        visible: (root.comparison.audioLeads || []).length > 0
                                        Text {
                                            Layout.fillWidth: true
                                            text: "画面领先时段（同一声场 · 音频互相关逐窗）"
                                            color: Theme.accent
                                            font.family: Theme.uiFont
                                            font.pixelSize: 11
                                            font.bold: true
                                        }
                                        Repeater {
                                            model: root.comparison.audioLeads || []
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 8
                                                Text {
                                                    text: root.clock(modelData.start) +
                                                          " → " + root.clock(modelData.end)
                                                    color: Theme.text2
                                                    font.family: Theme.monoFont
                                                    font.pixelSize: 10
                                                }
                                                Text {
                                                    Layout.fillWidth: true
                                                    text: modelData.label +
                                                          "（偏移 " +
                                                          root.signedGroup(modelData.offsetMs) +
                                                          " ms）"
                                                    color: Theme.success
                                                    font.family: Theme.uiFont
                                                    font.pixelSize: 10
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }
                                    }

                                    // 每路自己的音画同步摘要（与跨路对齐分开看）。
                                    RowLayout {
                                        Layout.fillWidth: true
                                        spacing: 16
                                        visible: root.compareMode && root.comparison.hasAudioTrack
                                        Text {
                                            text: "A 音画 " +
                                                  Number((root.comparison.avSyncA || {}).audioVideoOffsetMs || 0).toFixed(0) +
                                                  " ms · 分片跳变 " +
                                                  ((root.comparison.avSyncA || {}).segmentShiftIssues || 0) + " 处"
                                            color: Theme.text2
                                            font.family: Theme.monoFont
                                            font.pixelSize: 10
                                        }
                                        Text {
                                            text: "B 音画 " +
                                                  Number((root.comparison.avSyncB || {}).audioVideoOffsetMs || 0).toFixed(0) +
                                                  " ms · 分片跳变 " +
                                                  ((root.comparison.avSyncB || {}).segmentShiftIssues || 0) + " 处"
                                            color: Theme.text2
                                            font.family: Theme.monoFont
                                            font.pixelSize: 10
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: root.comparison.hasAudioTrack
                                        text: "音频可靠窗 " +
                                              Math.round((root.comparison.audioReliableRatio || 0) * 100) +
                                              "% · 整体漂移 " +
                                              Number(root.comparison.audioDriftMsPerMin || 0).toFixed(1) +
                                              " ms/min（同一声场但两路音频可能不完全重叠，未匹配的时段不采信）"
                                        color: Theme.text3
                                        font.family: Theme.uiFont
                                        font.pixelSize: 10
                                        wrapMode: Text.WordWrap
                                    }
                                    // 偏移随时间的变化：全局 offsetMs 看不出
                                    // “某一段时间不同步”，这里按时间窗画曲线。
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 96
                                        visible: root.comparisonWindows.length > 1
                                        color: Theme.sunken
                                        radius: 5
                                        border.color: Theme.line2
                                        Canvas {
                                            id: offsetCanvas
                                            anchors.fill: parent
                                            onPaint: {
                                                var ctx = getContext("2d")
                                                if (!ctx) return
                                                var W = width, H = height
                                                ctx.clearRect(0, 0, W, H)
                                                var wins = root.comparisonWindows
                                                if (wins.length < 2) return
                                                var base = Number(root.comparison.offsetMs || 0)
                                                var vals = []
                                                var lo = base, hi = base
                                                for (var i = 0; i < wins.length; ++i) {
                                                    var v = Number(wins[i].offsetMs || 0)
                                                    vals.push(v)
                                                    lo = Math.min(lo, v); hi = Math.max(hi, v)
                                                }
                                                if (hi - lo < 20) { lo -= 10; hi += 10 }
                                                var pad = (hi - lo) * 0.12
                                                lo -= pad; hi += pad
                                                var L = 8, R = 8, T = 18, B = 14
                                                function px(i) {
                                                    return L + (wins.length === 1 ? 0
                                                        : i * (W - L - R) / (wins.length - 1))
                                                }
                                                function py(v) {
                                                    return T + (hi - v) * (H - T - B) / (hi - lo)
                                                }
                                                // global reference
                                                ctx.strokeStyle = Theme.text4
                                                ctx.lineWidth = 1
                                                ctx.setLineDash([4, 3])
                                                ctx.beginPath()
                                                ctx.moveTo(L, py(base)); ctx.lineTo(W - R, py(base))
                                                ctx.stroke()
                                                ctx.setLineDash([])
                                                // offset curve
                                                ctx.strokeStyle = Theme.accent
                                                ctx.lineWidth = 2
                                                ctx.beginPath()
                                                for (var k = 0; k < vals.length; ++k) {
                                                    if (k === 0) ctx.moveTo(px(k), py(vals[k]))
                                                    else ctx.lineTo(px(k), py(vals[k]))
                                                }
                                                ctx.stroke()
                                                // anomalous windows in red
                                                ctx.strokeStyle = Theme.error
                                                ctx.lineWidth = 3
                                                var anomalies = root.comparison.anomalies || []
                                                for (var a = 0; a < anomalies.length; ++a) {
                                                    var st = Number(anomalies[a].start || 0)
                                                    var en = Number(anomalies[a].end || 0)
                                                    ctx.beginPath()
                                                    var started = false
                                                    for (var j = 0; j < wins.length; ++j) {
                                                        var mid = (Number(wins[j].start || 0) +
                                                                   Number(wins[j].end || 0)) / 2
                                                        if (mid < st - 0.1 || mid > en + 0.1) continue
                                                        if (started) ctx.lineTo(px(j), py(vals[j]))
                                                        else { ctx.moveTo(px(j), py(vals[j])); started = true }
                                                    }
                                                    ctx.stroke()
                                                }
                                            }
                                        }
                                        Text {
                                            anchors.left: parent.left
                                            anchors.leftMargin: 8
                                            anchors.top: parent.top
                                            anchors.topMargin: 2
                                            text: (root.comparison.hasAudioTrack
                                                   ? "A 相对 B 画面领先（音频逐窗，"
                                                   : "OFFSET / 时间轴偏移（") +
                                                  Number(root.comparison.windowSeconds || 10).toFixed(0) +
                                                  "s 窗口，虚线=整体偏移）"
                                            color: Theme.text3
                                            font.family: Theme.uiFont
                                            font.pixelSize: 10
                                        }
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: root.comparisonWindows.length > 1
                                        text: "漂移 " +
                                              Number(root.comparison.driftMsPerMin || 0).toFixed(1) +
                                              " ms/min · 最大偏差 " +
                                              Number(root.comparison.maxDeviationMs || 0).toFixed(1) +
                                              " ms"
                                        color: Theme.text2
                                        font.family: Theme.monoFont
                                        font.pixelSize: 10
                                    }
                                    Repeater {
                                        model: (root.comparison.anomalies || []).slice(0, 3)
                                        RowLayout {
                                            Layout.fillWidth: true
                                            spacing: 8
                                            Text {
                                                text: "⚠ " + root.clock(modelData.start) +
                                                      " → " + root.clock(modelData.end)
                                                color: Theme.error
                                                font.family: Theme.monoFont
                                                font.pixelSize: 10
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                text: (modelData.kind === "step" ? "突变 " : "局部偏移 ") +
                                                      root.signedGroup(modelData.deviationMs) + " ms"
                                                color: Theme.accent
                                                font.family: Theme.uiFont
                                                font.pixelSize: 10
                                                elide: Text.ElideRight
                                            }
                                        }
                                    }
                                    // A/B 结构差异始终给出：即使没有 PDT 或帧签名，
                                    // 两路的包数 / 帧数 / 流数仍可直接对照。
                                    Text {
                                        Layout.fillWidth: true
                                        visible: root.compareMode &&
                                                 root.summaryB.totalPackets !== undefined
                                        text: "A  " + root.group(root.summary.totalPackets || 0) +
                                              " 包 · " + (root.summary.streamCount || 0) + " 流 · " +
                                              root.group(root.summary.videoFrames || 0) + "v/" +
                                              root.group(root.summary.audioFrames || 0) + "a    |    B  " +
                                              root.group(root.summaryB.totalPackets || 0) +
                                              " 包 · " + (root.summaryB.streamCount || 0) + " 流 · " +
                                              root.group(root.summaryB.videoFrames || 0) + "v/" +
                                              root.group(root.summaryB.audioFrames || 0) + "a"
                                        color: Theme.text2
                                        font.family: Theme.monoFont
                                        font.pixelSize: 10
                                        wrapMode: Text.WordWrap
                                    }
                                    Text {
                                        Layout.fillWidth: true
                                        visible: root.compareMode &&
                                                 root.summaryB.totalPackets !== undefined
                                        text: "Δ 包 " + root.signedGroup(
                                                  (root.summary.totalPackets || 0) -
                                                  (root.summaryB.totalPackets || 0)) +
                                              " · Δ 视频帧 " + root.signedGroup(
                                                  (root.summary.videoFrames || 0) -
                                                  (root.summaryB.videoFrames || 0)) +
                                              " · Δ 音频帧 " + root.signedGroup(
                                                  (root.summary.audioFrames || 0) -
                                                  (root.summaryB.audioFrames || 0))
                                        color: Theme.accent
                                        font.family: Theme.monoFont
                                        font.pixelSize: 10
                                    }
                                }
                            }
                        }
                        } // details card
                    } // details ColumnLayout
                } // alignmentPanel
            } // summaryPanel
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
                                    text: root.group(index === 0
                                                     ? root.activeAnalysis.videoRows
                                                     : root.activeAnalysis.audioRows)
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

                // Source selector: only in dual mode. Selecting A/B switches
                // which independent frame store the 视频/音频 tables display.
                Rectangle {
                    visible: root.compareMode
                    width: 1; height: 18; color: Theme.line2
                    Layout.leftMargin: 10; Layout.rightMargin: 8
                }
                Repeater {
                    model: root.compareMode ? ["A", "B"] : []
                    Rectangle {
                        property bool active: root.activeSource === index
                        width: srcContent.implicitWidth + 28
                        height: 44
                        color: active ? Theme.accentWash : "transparent"
                        Rectangle {
                            anchors.left: parent.left; anchors.right: parent.right
                            anchors.bottom: parent.bottom
                            height: 2
                            color: active ? Theme.accent : "transparent"
                        }
                        RowLayout {
                            id: srcContent
                            anchors.centerIn: parent
                            spacing: 7
                            Text {
                                text: "源 " + modelData
                                color: active ? Theme.accent : Theme.text3
                                font.family: Theme.uiFont; font.pixelSize: 12; font.bold: active
                            }
                            Rectangle {
                                implicitWidth: srcCount.implicitWidth + 12
                                implicitHeight: 18; radius: 9
                                color: active ? Theme.surface3 : Theme.sunken
                                Text {
                                    id: srcCount
                                    anchors.centerIn: parent
                                    text: root.group(root.rowsForSource(index, root.tabIndex === 0))
                                    color: active ? Theme.accent : Theme.text3
                                    font.family: Theme.monoFont; font.pixelSize: 10; font.bold: true
                                }
                            }
                        }
                        MouseArea {
                            anchors.fill: parent
                            cursorShape: Qt.PointingHandCursor
                            onClicked: { root.activeSource = index; root.resetSelection() }
                        }
                    }
                }

                Loader {
                    visible: !root.compareMode && root.isUrlMode
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
                model: (root.compareMode && root.activeSource === 1)
                       ? AnalysisB.videoModel : Analysis.videoModel
            }
            FrameGrid {
                id: audioGrid
                ctl: root
                isVideo: false
                model: (root.compareMode && root.activeSource === 1)
                       ? AnalysisB.audioModel : Analysis.audioModel
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

    // Keyboard equivalent of clicking the SUMMARY header. Handy when the
    // window is small and the frame table needs the vertical space back.
    Shortcut {
        sequence: "Ctrl+Shift+S"
        onActivated: {
            root.toggleSummary()
            console.log("[SUMMARY] expanded=" + summaryPanel.expanded)
        }
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
                text: "录制回捞（cstore）"
                color: Theme.text2
                font.family: Theme.uiFont
                font.pixelSize: 11
                font.bold: true
            }
            RowLayout {
                Layout.fillWidth: true
                spacing: 8
                Rectangle {
                    width: 16; height: 16; radius: 3
                    color: root.cstoreMode ? Theme.accent : Theme.sunken
                    border.color: root.cstoreMode ? Theme.accent : Theme.line2
                    border.width: 1
                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            root.cstoreMode = !root.cstoreMode
                            if (root.cstoreMode) root.isUrlMode = true
                        }
                    }
                    Text {
                        anchors.centerIn: parent
                        text: root.cstoreMode ? "✓" : ""
                        color: Theme.surface1
                        font.pixelSize: 12
                    }
                }
                Text {
                    text: "从上方的「网络流」输入框读取 cstore 对象键（如 seewo-edulyse/xxx.m3u8），先拉全分片再分析"
                    color: Theme.text3
                    font.family: Theme.uiFont
                    font.pixelSize: 11
                    wrapMode: Text.Wrap
                    Layout.fillWidth: true
                }
            }
            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 8
                Label { text: "本地保存目录 (空=自动)"; color: Theme.text2; font.pixelSize: 12 }
                TextField {
                    id: fetchDirField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    placeholderText: "例: D:/cstore-fetched"
                    placeholderTextColor: Theme.text4
                    color: Theme.text1
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                    selectByMouse: true
                    background: Rectangle {
                        radius: 6; color: Theme.sunken
                        border.color: fetchDirField.activeFocus ? Theme.accent : Theme.line2
                    }
                }
                Label { text: "并发下载数"; color: Theme.text2; font.pixelSize: 12 }
                SpinBox {
                    id: fetchJobsSpin; from: 1; to: 16; value: 4; editable: true
                    palette.windowText: Theme.text1; palette.text: Theme.text1
                    palette.window: Theme.sunken; palette.base: Theme.sunken
                    palette.button: Theme.surface2; palette.buttonText: Theme.text1
                    background: Rectangle { radius: 6; color: Theme.sunken; border.color: Theme.line2 }
                }
            }
            Text {
                text: "对齐锚点（多流对比时可选）"
                color: Theme.text2
                font.family: Theme.uiFont
                font.pixelSize: 11
                font.bold: true
            }
            Text {
                text: "第二路输入在 SOURCE 面板的 B 行填写。两份清单都有 PDT 时给出精确墙上时钟偏移；没有 PDT 时可填共同事件的毫秒偏移，结果在 SUMMARY 的 COMPARISON 段显示。"
                color: Theme.text3
                font.family: Theme.uiFont
                font.pixelSize: 11
                wrapMode: Text.Wrap
                Layout.fillWidth: true
            }
            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 8
                Label { text: "人工锚点 ms（可选）"; color: Theme.text2; font.pixelSize: 12 }
                TextField {
                    id: anchorField
                    Layout.fillWidth: true
                    Layout.preferredHeight: 30
                    placeholderText: "无 PDT 时填共同事件的毫秒偏移，留空=相对对齐"
                    placeholderTextColor: Theme.text4
                    color: Theme.text1
                    font.family: Theme.monoFont
                    font.pixelSize: 11
                    selectByMouse: true
                    validator: DoubleValidator { }
                    background: Rectangle {
                        radius: 6; color: Theme.sunken
                        border.color: anchorField.activeFocus ? Theme.accent : Theme.line2
                    }
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
        // Live sources have no EOF: the worker publishes the stream list after
        // probing and keeps appending frame batches. Unlike a finished result,
        // this must NOT clear the selection, filter or scroll position - the
        // user is watching a growing table while it is being filled.
        function onLiveUpdated(summary, streams) {
            root.summary = summary
            if (summary.streamCount !== undefined)
                root.streamList = streams
        }
        function onAnalysisSucceeded(summary, streams) {
            root.resetSelection()
            root.filterText = ""
            filterInput.text = ""
            Analysis.setFilter(true, "")
            Analysis.setFilter(false, "")
            AnalysisB.setFilter(true, "")
            AnalysisB.setFilter(false, "")
            videoGrid.table.contentY = 0
            audioGrid.table.contentX = 0
            audioGrid.table.contentY = 0
            videoGrid.table.contentX = 0
            root.summary = summary
            root.streamList = streams
            root.errorDiag = ({})
            root.noticeDismissed = false
            if (offsetCanvas && offsetCanvas.requestPaint) offsetCanvas.requestPaint()
        }
        // A real failure is an inline notice + status state, never a modal
        // dialog. Stopping / cancelling never reaches this handler.
        function onAnalysisFailed(diag) {
            root.errorDiag = diag
            root.noticeDismissed = false
            root.showToast("解析失败 · " + (diag.message || diag.code))
        }
        function onFetchProgress(done, total, phase) {
            if (phase === "start") {
                root.fetchText = "回捞中 · 0 个分片"
            } else if (phase === "download" && total > 0) {
                root.fetchText = "回捞中 · " + done + " / " + total + " 个分片"
            }
        }
    }

    // B controller has no worker of its own: its signals are driven by the
    // source:"B" JSON lines dispatched from the primary worker.
    Connections {
        target: AnalysisB
        function onLiveUpdated(summary, streams) {
            root.summaryB = summary
            if (summary.streamCount !== undefined)
                root.streamListB = streams
        }
        function onAnalysisSucceeded(summary, streams) {
            root.filterText = ""
            filterInput.text = ""
            AnalysisB.setFilter(true, "")
            AnalysisB.setFilter(false, "")
            root.summaryB = summary
            root.streamListB = streams
            root.errorDiagB = ({})
            root.noticeDismissed = false
        }
        function onAnalysisFailed(diag) {
            root.errorDiagB = diag
            root.noticeDismissed = false
            root.showToast("B 源解析失败 · " + (diag.message || diag.code))
        }
        function onFetchProgress(done, total, phase) {
            if (phase === "start") {
                root.fetchText = "B 回捞中 · 0 个分片"
            } else if (phase === "download" && total > 0) {
                root.fetchText = "B 回捞中 · " + done + " / " + total + " 个分片"
            }
        }
    }
}
