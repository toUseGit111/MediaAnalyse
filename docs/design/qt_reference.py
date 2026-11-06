#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
MEDIA ANALYSE / PRISM — Qt Widgets reference implementation
==========================================================
A runnable, self-contained skeleton showing how to apply media-analyzer.qss
and how to draw the parts QSS cannot express (frame-type chips, tabular
numerals, selected-row accent rule).

Run it:
    python qt_reference.py                          # interactive window
    python qt_reference.py --shot preview.png       # render offscreen, save PNG
    python qt_reference.py --mode stream --shot live.png   # network-stream view
    python qt_reference.py --url "https://x/live.m3u8" --shot hls.png
    python qt_reference.py --check                 # QSS + protocol detection table

PROTOCOL DETECTION
    A network stream needs exactly one input: its URL. detect_proto() derives
    RTSP / RTMP / HLS / HTTP-FLV from the scheme or playlist extension. There
    is no protocol picker, transport/buffer combo, auth, timeout, reconnect
    option or window-size setting anywhere in this UI.

Requires:  pip install PySide6      (Qt 6.x / Python 3.9+)
Porting:   PyQt6  -> change imports; enums become Qt.AlignmentFlag.AlignRight
           PyQt5  -> change imports; exec_() instead of exec()
           C++ Qt -> same objectName / setProperty contract, same QSS file,
                     port FrameTypeDelegate::paint() to QPainter directly.

WHY THIS FILE EXISTS
---------------------
QSS has no variables, cannot target a single table column, cannot draw chips,
and cannot right-align only numeric cells. The delegate below is the intended
home for those rules. Keep TOKENS in sync with the token table at the top of
media-analyzer.qss.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

from PySide6.QtCore import Qt, QFile, QTextStream, QRect, QTimer
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QApplication, QMainWindow, QWidget, QFrame, QLabel, QPushButton, QLineEdit,
    QTabBar, QTableWidget, QTableWidgetItem, QAbstractItemView, QHeaderView,
    QStyledItemDelegate, QStyle, QVBoxLayout, QHBoxLayout, QGridLayout,
    QSizePolicy, QCheckBox, QButtonGroup, QStackedWidget,
)

HERE = Path(__file__).resolve().parent
QSS_PATH = HERE / "media-analyzer.qss"

# ---------------------------------------------------------------------------
# Design tokens — mirror of the table in media-analyzer.qss
# ---------------------------------------------------------------------------
TOKENS = {
    "bg":        "#14171B",
    "sunken":    "#101317",
    "surface1":  "#1B1F25",
    "surface2":  "#232830",
    "surface3":  "#2C333D",
    "line":      "#2A3038",
    "line2":     "#363E49",
    "text":      "#E8EAED",
    "text2":     "#A9B2BE",
    "text3":     "#7E8998",
    "text4":     "#5B6572",
    "accent":    "#E0A44A",
    "accent_wash": "#33291B",
    "accent_ink":  "#1A1509",
    "video":     "#82A9CC",
    "audio":     "#C99A72",
    "ok":        "#86B49A",
    "alt_row":   "#161A20",
    "frame_I":   "#E0A44A",
    "frame_P":   "#82A9CC",
    "frame_B":   "#8A93A0",
}
MONO = QFont("Cascadia Mono", 10)
MONO.setStyleHint(QFont.Monospace)
UI_FONT = QFont("Segoe UI", 10)

COLUMNS = ["#", "pkt#", "stream", "dts", "dts-time", "dts-delta",
           "pts", "pts-time", "duration", "size", "type", "samples"]

# Real first 15 rows from the original screenshot.
SEED = [
    (1, 2, 0, 0, "0.000", "-", 0, "0.000", 512, 4124, "I", "-"),
    (2, 5, 0, 512, "0.033", "0.033", 512, "0.033", 512, 340, "P", "-"),
    (3, 7, 0, 1024, "0.067", "0.033", 1024, "0.067", 512, 291, "P", "-"),
    (4, 10, 0, 1536, "0.100", "0.033", 1536, "0.100", 512, 263, "P", "-"),
    (5, 12, 0, 2048, "0.133", "0.033", 2048, "0.133", 512, 267, "P", "-"),
    (6, 15, 0, 2560, "0.167", "0.033", 2560, "0.167", 512, 260, "P", "-"),
    (7, 17, 0, 3072, "0.200", "0.033", 3072, "0.200", 512, 236, "P", "-"),
    (8, 20, 0, 3584, "0.233", "0.033", 3584, "0.233", 512, 229, "P", "-"),
    (9, 22, 0, 4096, "0.267", "0.033", 4096, "0.267", 512, 290, "P", "-"),
    (10, 24, 0, 4608, "0.300", "0.033", 4608, "0.300", 512, 271, "P", "-"),
    (11, 27, 0, 5120, "0.333", "0.033", 5120, "0.333", 512, 385, "P", "-"),
    (12, 29, 0, 5632, "0.367", "0.033", 5632, "0.367", 512, 187, "P", "-"),
    (13, 32, 0, 6144, "0.400", "0.033", 6144, "0.400", 512, 296, "P", "-"),
    (14, 34, 0, 6656, "0.433", "0.033", 6656, "0.433", 512, 282, "P", "-"),
    (15, 37, 0, 7168, "0.467", "0.033", 7168, "0.467", 512, 263, "P", "-"),
]


# ---------------------------------------------------------------------------
# Protocol auto-detection — the URL is the only input
# ---------------------------------------------------------------------------
PROTO_RULES = (
    (re.compile(r"^rtsp(s)?://", re.I), "RTSP", "rtsp:// 前缀"),
    (re.compile(r"^rtmp(s|t|e|te|fp)?://", re.I), "RTMP", "rtmp:// 前缀"),
    (re.compile(r"\.m3u8(\?|#|$)", re.I), "HLS", ".m3u8 播放列表"),
    (re.compile(r"\.flv(\?|#|$)", re.I), "HTTP-FLV", ".flv 扩展名"),
)
UNSUPPORTED = "仅支持 rtsp:// · rtmp:// · .m3u8 · .flv"


def detect_proto(raw: str) -> tuple[str, str, bool]:
    """Return (protocol, human-readable rule, recognised).

    Explicit schemes win first; HTTP-based streams are classified by their
    playlist / file extension. Mirrored verbatim in design-preview.html.
    """
    url = (raw or "").strip()
    if not url:
        return "待输入", "粘贴流地址", False
    for rx, name, rule in PROTO_RULES:
        if rx.search(url):
            return name, rule, True
    return "未识别", UNSUPPORTED, False


def load_qss(app: QApplication) -> str:
    """Load the stylesheet. Missing file must fail loudly, not silently."""
    if not QSS_PATH.exists():
        raise FileNotFoundError(f"Stylesheet not found: {QSS_PATH}")
    f = QFile(str(QSS_PATH))
    if not f.open(QFile.ReadOnly | QFile.Text):
        raise IOError(f"Cannot open stylesheet: {QSS_PATH}")
    qss = QTextStream(f).readAll()
    f.close()
    app.setStyleSheet(qss)
    return qss


def label(text: str, role: str = "", *, mono: bool = False) -> QLabel:
    w = QLabel(text)
    if role:
        w.setProperty("role", role)          # QSS attribute selector hook
    if mono:
        w.setFont(MONO)
    return w


def section_header(text: str, tail: str = "") -> QWidget:
    """Eyebrow + hairline rule. Hierarchy comes from spacing, not cards."""
    bar = QWidget()
    h = QHBoxLayout(bar)
    h.setContentsMargins(0, 0, 0, 0)
    h.setSpacing(10)
    h.addWidget(label(text, "eyebrow"))
    rule = QFrame()
    rule.setFrameShape(QFrame.HLine)
    rule.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
    h.addWidget(rule, 1)
    if tail:
        h.addWidget(label(tail, "muted"))
    return bar


class Metric(QWidget):
    """Small label over a large tabular value — replaces scattered status text.

    set_data() lets the whole strip change meaning when the input source
    switches from a local file to a live stream (duration -> LIVE, etc.).
    """

    def __init__(self, name: str, value: str, unit: str = "", note: str = "",
                 accent: bool = False):
        super().__init__()
        v = QVBoxLayout(self)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(0)
        self._name = label(name, "metric-label")
        v.addWidget(self._name)
        row = QHBoxLayout()
        row.setSpacing(4)
        self._value = label(value, "metric-value")
        row.addWidget(self._value)
        self._unit = label(unit, "metric-unit")
        row.addWidget(self._unit)
        row.addStretch(1)
        v.addLayout(row)
        self._note = label(note, "muted")
        self._note.setFont(MONO)
        v.addWidget(self._note)
        self.set_data(name, value, unit, note, accent)

    def set_data(self, name, value, unit="", note="", accent=False):
        self._name.setText(name)
        self._value.setText(value)
        self._value.setStyleSheet(
            f"color: {TOKENS['accent']};" if accent else "")
        self._unit.setText(unit)
        self._note.setText(note)


class StreamCard(QFrame):
    """Top rule + numbered id + aligned key/value grid."""

    def __init__(self, index: str, title: str, codec: str, variant: str, rows: list):
        super().__init__()
        self.setObjectName("StreamCard")
        self.setProperty("variant", variant)
        v = QVBoxLayout(self)
        v.setContentsMargins(15, 10, 15, 10)
        v.setSpacing(8)

        head = QHBoxLayout()
        head.setSpacing(9)
        head.addWidget(label(index, "stream-id"))
        head.addWidget(label(title, "stream-title"))
        badge = label(codec, "muted")
        badge.setFont(MONO)
        head.addWidget(badge, 1, Qt.AlignRight)
        v.addLayout(head)

        # Instrument-style readout: small label above a tabular value, four
        # across. Keeps the header compact so the data table stays primary.
        grid = QGridLayout()
        grid.setHorizontalSpacing(12)
        grid.setVerticalSpacing(2)
        for c, (k, val) in enumerate(rows):
            grid.setColumnStretch(c, 1)
            grid.addWidget(label(k, "metric-label"), 0, c)
            value = label(val, "meta")
            grid.addWidget(value, 1, c)
        v.addLayout(grid)


class FrameTypeDelegate(QStyledItemDelegate):
    """
    The one piece QSS cannot do:
      1. I / P / B chips (letter + border + colour, never colour alone)
      2. 2px accent rule on the first cell of selected rows
      3. consistent tabular numerals / right alignment
    """

    CHIP_BG = {"I": "#33291B", "P": "#1C2731", "B": "#232830"}
    CHIP_FG = {"I": "#E0A44A", "P": "#82A9CC", "B": "#A9B2BE"}
    CHIP_BD = {"I": "#6B5227", "P": "#35506A", "B": "#363E49"}

    def paint(self, painter: QPainter, option, index):
        selected = bool(option.state & QStyle.State_Selected)
        hovered = bool(option.state & QStyle.State_MouseOver)
        text = str(index.data(Qt.DisplayRole) or "")

        # Deterministic background — mirrors the QSS ::item rules so the chip
        # column never looks different from its neighbours.
        if selected:
            bg = QColor(TOKENS["accent_wash"])
        elif hovered:
            bg = QColor(TOKENS["surface2"])
        elif index.row() % 2:
            bg = QColor(TOKENS["alt_row"])
        else:
            bg = QColor(TOKENS["sunken"])
        painter.fillRect(option.rect, bg)

        if index.column() == 10 and text in self.CHIP_BG:
            chip = QRect(0, 0, 22, 17)
            chip.moveCenter(option.rect.center())
            chip.moveRight(option.rect.right() - 10)
            painter.setRenderHint(QPainter.Antialiasing)
            painter.setBrush(QColor(self.CHIP_BG[text]))
            painter.setPen(QPen(QColor(self.CHIP_BD[text]), 1))
            painter.drawRoundedRect(chip, 3, 3)
            painter.setPen(QColor(self.CHIP_FG[text]))
            f = QFont("Cascadia Mono", 9)
            f.setBold(True)
            painter.setFont(f)
            painter.drawText(chip, Qt.AlignCenter, text)
        else:
            # Fully manual text rendering keeps colour and alignment identical
            # across every column, independent of QStyle/QSS ::item quirks.
            if selected:
                fg = QColor("#F2E3C6")
            elif index.column() == 0:
                fg = QColor(TOKENS["text3"])      # quiet row number, still AA
            elif text == "-":
                fg = QColor(TOKENS["text4"])
            else:
                fg = QColor(TOKENS["text"])
            painter.setFont(MONO)
            painter.setPen(fg)
            if index.column() == 0:
                # Row numbers are narrow ("1" is a single thin glyph). Centre
                # them so they never clip against the right cell inset.
                painter.drawText(option.rect.adjusted(2, 0, -2, 0),
                                 Qt.AlignCenter | Qt.AlignVCenter, text)
            else:
                painter.drawText(option.rect.adjusted(6, 0, -11, 0),
                                 Qt.AlignRight | Qt.AlignVCenter, text)

        # Bottom hairline + selected-row marker on the row-number column.
        painter.setPen(QPen(QColor(TOKENS["line"]), 1))
        painter.drawLine(option.rect.bottomLeft(), option.rect.bottomRight())
        if selected and index.column() == 0:
            painter.fillRect(QRect(option.rect.left(), option.rect.top(),
                                   2, option.rect.height()), QColor(TOKENS["accent"]))

    def sizeHint(self, option, index):
        s = super().sizeHint(option, index)
        s.setHeight(30)
        return s


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setObjectName("AppRoot")
        self.setWindowTitle("媒体分析器  ·  MEDIA ANALYSE")
        self.resize(1120, 900)

        central = QWidget()
        central.setObjectName("AppRoot")
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        root.setContentsMargins(0, 0, 0, 0)
        root.setSpacing(0)

        root.addWidget(self._app_bar())

        body = QWidget()
        b = QVBoxLayout(body)
        b.setContentsMargins(20, 14, 20, 12)
        b.setSpacing(0)
        # Header region keeps its natural height; the data table below is the
        # primary surface and receives every remaining pixel of the window.
        root.addWidget(body)

        # "Network stream" is a first-class input: the mode switch lives in
        # the section header, always visible — never hidden in a menu.
        src_head = QWidget()
        sh = QHBoxLayout(src_head)
        sh.setContentsMargins(0, 0, 0, 0)
        sh.setSpacing(10)
        sh.addWidget(label("Source / 输入源", "eyebrow"))
        rule = QFrame()
        rule.setFrameShape(QFrame.HLine)
        rule.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        sh.addWidget(rule, 1)
        mode_box = QFrame()
        mode_box.setObjectName("SourceMode")
        mb = QHBoxLayout(mode_box)
        mb.setContentsMargins(2, 2, 2, 2)
        mb.setSpacing(2)
        self.mode_group = QButtonGroup(self)
        self.mode_group.setExclusive(True)
        self.btn_file = QPushButton("本地文件")
        self.btn_stream = QPushButton("网络流")
        for i, btn in enumerate((self.btn_file, self.btn_stream)):
            btn.setProperty("variant", "segment")
            btn.setCheckable(True)
            btn.setFixedHeight(26)
            self.mode_group.addButton(btn, i)
            mb.addWidget(btn)
        self.btn_file.setChecked(True)
        sh.addWidget(mode_box)
        b.addWidget(src_head)
        b.addSpacing(9)
        b.addWidget(self._source_bar())
        b.addSpacing(12)

        ov_head = QWidget()
        oh = QHBoxLayout(ov_head)
        oh.setContentsMargins(0, 0, 0, 0)
        oh.setSpacing(10)
        oh.addWidget(label("Overview / 概览", "eyebrow"))
        rule2 = QFrame()
        rule2.setFrameShape(QFrame.HLine)
        rule2.setSizePolicy(QSizePolicy.Expanding, QSizePolicy.Fixed)
        oh.addWidget(rule2, 1)
        self.overview_status = label(
            "解析完成 · 43,841 packets in 6.95 s", "status-ok")
        oh.addWidget(self.overview_status)
        b.addWidget(ov_head)
        b.addSpacing(9)
        b.addWidget(self._metric_strip())
        b.addSpacing(12)

        b.addWidget(self._notice_bar())
        b.addSpacing(12)

        b.addWidget(section_header("Streams / 流信息", "2 条流"))
        b.addSpacing(9)
        streams = QHBoxLayout()
        streams.setSpacing(12)
        streams.addWidget(StreamCard("#0", "视频流", "H.264", "video", [
            ("分辨率", "1920×1080"), ("像素格式", "yuv420p"),
            ("帧率", "30.000 fps"), ("时基", "1/15360")]))
        streams.addWidget(StreamCard("#1", "音频流", "AAC", "audio", [
            ("声道", "2 ch"), ("采样率", "44,100 Hz"),
            ("采样格式", "fltp · LC"), ("时基", "1/44100")]))
        b.addLayout(streams)

        root.addWidget(self._toolbar())
        root.addWidget(self._table(), 1)
        root.addWidget(self._statusbar())

        self.btn_file.clicked.connect(lambda: self._apply_mode(0))
        self.btn_stream.clicked.connect(lambda: self._apply_mode(1))
        self._apply_mode(0)

    # -- regions -----------------------------------------------------------
    def _app_bar(self) -> QFrame:
        bar = QFrame()
        bar.setObjectName("AppBar")
        bar.setFixedHeight(48)
        h = QHBoxLayout(bar)
        h.setContentsMargins(18, 0, 16, 0)
        h.setSpacing(10)
        mark = label("◆", "brandmark")
        h.addWidget(mark)
        h.addWidget(label("媒体分析器", "appname"))
        en = label("MEDIA ANALYSE", "muted")
        en.setFont(MONO)
        h.addWidget(en)
        sep = QFrame()
        sep.setFrameShape(QFrame.VLine)
        sep.setFixedWidth(1)
        h.addWidget(sep)
        self.appbar_status = label("本地文件 · 离线解析", "muted")
        h.addWidget(self.appbar_status)
        h.addStretch(1)
        cfg = QPushButton("配置")
        cfg.setProperty("variant", "ghost")
        cfg.setFixedHeight(28)
        h.addWidget(cfg)
        return bar

    def _source_bar(self) -> QFrame:
        """Two stacked pages. QStackedWidget keeps a constant height, so the
        data table never jumps when the input mode changes."""
        box = QFrame()
        box.setObjectName("SourceBar")
        v = QVBoxLayout(box)
        v.setContentsMargins(16, 13, 16, 13)
        v.setSpacing(0)
        self.source_stack = QStackedWidget()
        self.source_stack.addWidget(self._file_page())
        self.source_stack.addWidget(self._stream_page())
        v.addWidget(self.source_stack)
        return box

    def _file_page(self) -> QWidget:
        page = QWidget()
        v = QVBoxLayout(page)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(0)
        v.addWidget(label("文件路径", "field-label"))
        v.addSpacing(7)
        row = QHBoxLayout()
        row.setSpacing(8)
        self.path_edit = QLineEdit("D:/BaiduNetdiskDownload/e1_1080p_timecode.mp4")
        self.path_edit.setPlaceholderText("粘贴路径后按回车，或点击「浏览…」")
        self.path_edit.setFixedHeight(34)
        row.addWidget(self.path_edit, 1)
        browse = QPushButton("浏览…")
        browse.setProperty("variant", "secondary")
        browse.setFixedHeight(34)
        row.addWidget(browse)
        open_btn = QPushButton("打开")
        open_btn.setProperty("variant", "primary")   # the ONLY primary button
        open_btn.setFixedHeight(34)
        row.addWidget(open_btn)
        v.addLayout(row)
        v.addSpacing(10)
        foot = QHBoxLayout()
        foot.addWidget(label("点击「打开」选择文件，或直接粘贴路径后按回车", "muted"))
        foot.addStretch(1)
        decode = QCheckBox("解码并解析")
        decode.setChecked(True)
        decode.setProperty("variant", "toggle")
        foot.addWidget(decode)
        v.addLayout(foot)
        v.addSpacing(8)
        v.addWidget(self._source_status(offline=True))
        return page

    def _stream_page(self) -> QWidget:
        """One URL in, one protocol out. No settings of any kind."""
        page = QWidget()
        v = QVBoxLayout(page)
        v.setContentsMargins(0, 0, 0, 0)
        v.setSpacing(0)
        v.addWidget(label("流地址 · 协议自动识别", "field-label"))
        v.addSpacing(7)
        row = QHBoxLayout()
        row.setSpacing(8)
        self.stream_edit = QLineEdit(
            "rtsp://192.168.1.64:554/Streaming/Channels/101")
        self.stream_edit.setPlaceholderText(
            "粘贴 rtsp:// · rtmp:// · .m3u8 · .flv 地址")
        self.stream_edit.setFixedHeight(34)
        row.addWidget(self.stream_edit, 1)
        # Read-only read-out. It is a QLabel, deliberately not a button: the
        # user never chooses a protocol, the address already contains it.
        self.proto_label = label("AUTO · RTSP", "proto")
        self.proto_label.setProperty("state", "detected")
        row.addWidget(self.proto_label)
        parse = QPushButton("解析")
        parse.setProperty("variant", "primary")   # same single accent button
        parse.setFixedHeight(34)
        row.addWidget(parse)
        self.parse_btn = parse
        v.addLayout(row)

        v.addSpacing(10)
        self.url_hint = label(
            "已识别 RTSP · rtsp:// 前缀 ；按回车或点击「解析」开始帧解析", "muted")
        v.addWidget(self.url_hint)
        v.addSpacing(8)
        v.addWidget(self._source_status(offline=False))

        self.stream_edit.textChanged.connect(self._on_url_changed)
        self.stream_edit.returnPressed.connect(self._start_parse)
        parse.clicked.connect(self._start_parse)
        return page

    def _source_status(self, offline: bool) -> QFrame:
        bar = QFrame()
        bar.setObjectName("StreamStatus")
        h = QHBoxLayout(bar)
        h.setContentsMargins(0, 8, 0, 0)
        h.setSpacing(10)
        badge = label("● FILE" if offline else "● LIVE", "live")
        if offline:
            badge.setProperty("state", "offline")
        h.addWidget(badge)
        detail = label(
            "已解析 · 2 条流  ·  MOV/MP4  ·  索引完整" if offline else
            "自动识别 RTSP  ·  已解析 18,432 帧  ·  612 关键帧  ·  30.0 fps",
            "live-stat")
        if not offline:
            self.live_stat = detail
        h.addWidget(detail)
        h.addStretch(1)
        return bar

    @staticmethod
    def _repolish(*widgets):
        """QSS attribute selectors need an explicit re-polish after a
        setProperty() change, otherwise the old state keeps its colours."""
        for w in widgets:
            w.style().unpolish(w)
            w.style().polish(w)

    def _on_url_changed(self, text: str):
        """Re-derive the protocol on every keystroke (pure, no IO)."""
        name, rule, ok = detect_proto(text)
        self.proto_label.setText(("AUTO · " + name) if ok else name)
        self.proto_label.setProperty("state", "detected" if ok else "unknown")
        self.stream_edit.setProperty("state", "detected" if ok else "unknown")
        self._repolish(self.proto_label, self.stream_edit)
        self.parse_btn.setEnabled(ok)
        self.url_hint.setText(
            f"已识别 {name} · {rule}；按回车或点击「解析」开始帧解析"
            if ok else rule)
        self.live_stat.setText(
            f"自动识别 {name}  ·  已解析 18,432 帧  ·  612 关键帧  ·  30.0 fps"
            if ok else "地址协议未识别  ·  等待有效的流地址")
        if self.source_stack.currentIndex() == 1 and hasattr(self, "metrics"):
            self._sync_detected()

    def _sync_detected(self):
        """Propagate the detected protocol to app bar, metrics and notice."""
        name, rule, ok = detect_proto(self.stream_edit.text())
        self.appbar_status.setText(
            f"{name} 实时流 · 帧解析中" if ok else "地址协议未识别")
        self.metrics[4].set_data(
            "接入协议", name, "",
            f"auto · {rule}" if ok else "地址无法识别", False)
        self.notice_text.setText(
            f"协议自动识别 · {name} · {rule}；无需选择协议或配置连接参数，直接开始逐帧解析"
            if ok else f"地址协议未识别 · {UNSUPPORTED}")
        self.notice_bar.setProperty("variant", "info" if ok else "warning")
        self._repolish(self.notice_bar, self.notice_text)
        self.overview_status.setText(
            "实时解析 · 30.0 fps · 12.4 Mb/s" if ok else "等待有效的流地址")
        self.status_state.setText(
            f"● LIVE · {name} 帧解析中" if ok else "● 地址协议未识别")

    def _start_parse(self):
        name, _rule, ok = detect_proto(self.stream_edit.text())
        self.status_state.setText(
            f"● LIVE · {name} 帧解析中" if ok else "● 地址协议未识别")

    def _metric_strip(self) -> QFrame:
        strip = QFrame()
        strip.setObjectName("MetricStrip")
        g = QHBoxLayout(strip)
        g.setContentsMargins(4, 12, 4, 12)
        g.setSpacing(0)
        self.metrics = [
            Metric("时长", "600.003", "s", "10:00.003"),
            Metric("平均码率", "9.75", "Mb/s", "9,748,792 bit/s"),
            Metric("包总数", "43,841", "", "video · audio"),
            Metric("解析耗时", "6.95", "s", "6,307 pkt/s", accent=True),
            Metric("容器格式", "MOV/MP4", "", "m4a · 3gp · 3g2 · mj2"),
        ]
        for i, m in enumerate(self.metrics):
            if i:
                line = QFrame()
                line.setFrameShape(QFrame.VLine)
                line.setFixedWidth(1)
                g.addWidget(line)
            g.addWidget(m, 1)
        return strip

    def _apply_mode(self, index: int):
        """A live source changes the semantics of the whole header region."""
        live = (index == 1)
        self.source_stack.setCurrentIndex(index)
        self.appbar_status.setText(
            "RTSP 实时流 · 帧解析中" if live else "本地文件 · 离线解析")
        self.overview_status.setText(
            "实时解析 · 30.0 fps · 12.4 Mb/s" if live else
            "解析完成 · 43,841 packets in 6.95 s")
        file_data = [
            ("时长", "600.003", "s", "10:00.003", False),
            ("平均码率", "9.75", "Mb/s", "9,748,792 bit/s", False),
            ("包总数", "43,841", "", "video · audio", False),
            ("解析耗时", "6.95", "s", "6,307 pkt/s", True),
            ("容器格式", "MOV/MP4", "", "m4a · 3gp · 3g2 · mj2", False),
        ]
        live_data = [
            ("时长", "LIVE", "", "持续流 · 无固定时长", True),
            ("实时码率", "12.4", "Mb/s", "逐包统计", False),
            ("已解析包数", "1,284,306", "", "持续增长", False),
            ("已解析帧数", "18,432", "帧", "含 612 个关键帧", True),
            ("接入协议", "RTSP", "", "auto · rtsp:// 前缀", False),
        ]
        for m, d in zip(self.metrics, live_data if live else file_data):
            m.set_data(*d)
        self.notice_text.setText(
            "协议自动识别 · 按地址前缀与扩展名判定，无需选择协议或配置连接参数"
            if live else
            "解码上限 · 仅前 3000 帧为帧级数据，之后的行按包级显示（maxDecodeFrames）")
        self.notice_bar.setProperty("variant", "info" if live else "warning")
        self._repolish(self.notice_bar, self.notice_text)
        self.status_state.setText("● LIVE · 帧解析中" if live else "● 就绪")
        if live:
            self._sync_detected()

    def _notice_bar(self) -> QFrame:
        bar = QFrame()
        bar.setObjectName("NoticeBar")
        bar.setProperty("variant", "warning")
        self.notice_bar = bar
        h = QHBoxLayout(bar)
        h.setContentsMargins(12, 9, 12, 9)
        h.setSpacing(10)
        icon = label("▲", "warning")
        h.addWidget(icon)
        txt = label("解码上限 · 仅前 3000 帧为帧级数据，之后的行按包级显示（maxDecodeFrames）",
                    "warning")
        self.notice_text = txt
        txt.setWordWrap(True)
        h.addWidget(txt, 1)
        close = QPushButton("×")
        close.setProperty("variant", "ghost")
        close.setFixedSize(24, 24)
        close.clicked.connect(bar.hide)
        h.addWidget(close)
        return bar

    def _toolbar(self) -> QFrame:
        bar = QFrame()
        bar.setObjectName("TableToolbar")
        bar.setFixedHeight(44)
        h = QHBoxLayout(bar)
        h.setContentsMargins(20, 0, 20, 0)
        h.setSpacing(10)
        tabs = QTabBar()
        tabs.setDrawBase(False)
        tabs.addTab("视频   43,841")
        tabs.addTab("音频   25,806")
        tabs.setCurrentIndex(0)
        tabs.setSizePolicy(QSizePolicy.Fixed, QSizePolicy.Expanding)
        h.addWidget(tabs)
        h.addStretch(1)
        search = QLineEdit()
        search.setPlaceholderText("过滤 pkt# / type…")
        search.setFixedWidth(180)
        search.setFixedHeight(30)
        h.addWidget(search)
        copy = QPushButton("复制选区")
        copy.setProperty("variant", "secondary")
        copy.setObjectName("CopyButton")
        copy.setFixedHeight(30)
        copy.setEnabled(False)
        h.addWidget(copy)
        self.copy_btn = copy
        self.tabs = tabs
        return bar

    def _table(self) -> QTableWidget:
        t = QTableWidget()
        t.setObjectName("DataView")
        t.setMinimumHeight(230)          # the table is the primary surface
        t.setColumnCount(len(COLUMNS))
        t.setHorizontalHeaderLabels(COLUMNS)
        t.verticalHeader().setVisible(False)
        t.setAlternatingRowColors(True)
        t.setShowGrid(False)
        t.setSelectionMode(QAbstractItemView.ExtendedSelection)
        t.setSelectionBehavior(QAbstractItemView.SelectItems)  # rectangular selection
        t.setEditTriggers(QAbstractItemView.NoEditTriggers)
        t.setItemDelegate(FrameTypeDelegate(t))
        t.verticalHeader().setDefaultSectionSize(30)
        hdr = t.horizontalHeader()
        hdr.setFixedHeight(34)
        hdr.setSectionResizeMode(QHeaderView.Stretch)
        hdr.setSectionResizeMode(0, QHeaderView.Fixed)   # row number
        t.setColumnWidth(0, 46)
        hdr.setSectionResizeMode(10, QHeaderView.Fixed)  # frame type chip
        t.setColumnWidth(10, 62)
        t.setFont(MONO)
        t.setRowCount(48)
        for r in range(48):
            if r < len(SEED):
                vals = SEED[r]
            else:
                i = r + 1
                dts = r * 512
                dt = f"{dts / 15360:.3f}"
                kind = "I" if r % 30 == 0 else ("B" if r % 7 == 0 else "P")
                vals = (i, 2 + i * 2 + i // 7, 0, dts, dt, "0.033", dts, dt,
                        512, 180 + (r * 137) % 260, kind, "-")
            for c, val in enumerate(vals):
                item = QTableWidgetItem(str(val))
                item.setTextAlignment(Qt.AlignRight | Qt.AlignVCenter)
                if c == 0:
                    item.setForeground(QColor(TOKENS["text4"]))
                elif str(val) == "-":
                    item.setForeground(QColor(TOKENS["text4"]))
                else:
                    item.setForeground(QColor(TOKENS["text"]))
                t.setItem(r, c, item)
        t.selectionModel().selectionChanged.connect(
            lambda *_: self.copy_btn.setEnabled(bool(t.selectedIndexes())))
        return t

    def _statusbar(self) -> QFrame:
        bar = QFrame()
        bar.setObjectName("StatusBar")
        bar.setFixedHeight(30)
        h = QHBoxLayout(bar)
        h.setContentsMargins(20, 0, 20, 0)
        h.setSpacing(8)
        for text in ["拖选矩形选区", "·", "点 # 列选整行", "·", "Shift 连选",
                     "·", "Ctrl+C 导出 TSV"]:
            h.addWidget(label(text, "muted"))
        h.addStretch(1)
        h.addWidget(label("43,841 行 · 12 列", "muted"))
        self.status_state = label("● 就绪", "status-ok")
        h.addWidget(self.status_state)
        return bar


def main() -> int:
    ap = argparse.ArgumentParser(description="PRISM Qt reference UI")
    ap.add_argument("--shot", metavar="PNG", help="render offscreen and save a screenshot")
    ap.add_argument("--check", action="store_true", help="verify the QSS was applied")
    ap.add_argument("--mode", choices=["file", "stream"], default="file",
                    help="initial input source mode (default: file)")
    ap.add_argument("--url", metavar="URL",
                    help="prefill one stream URL (implies --mode stream)")
    args = ap.parse_args()

    if args.shot:
        # Offscreen avoids stealing focus; Qt's grab() still renders the tree.
        os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
        if sys.platform == "win32":
            # Without a font dir, Windows offscreen Qt loads no font database
            # and every CJK glyph renders as a tofu box.
            os.environ.setdefault("QT_QPA_FONTDIR", "C:/Windows/Fonts")

    app = QApplication(sys.argv)
    app.setFont(UI_FONT)
    qss = load_qss(app)
    win = MainWindow()
    if args.url or args.mode == "stream":
        win.btn_stream.setChecked(True)
        if args.url:
            win.stream_edit.setText(args.url)
        win._apply_mode(1)

    if args.check:
        print(f"QSS loaded      : {len(qss)} chars from {QSS_PATH}")
        print(f"input mode      : {'stream' if (args.url or args.mode == 'stream') else 'file'}")
        print(f"app stylesheet  : {'OK' if app.styleSheet() else 'EMPTY'}")
        print(f"root objectName : {win.objectName()}")
        print(f"accent token    : {TOKENS['accent']}")
        print("protocol detection:")
        for u in (
            "rtsp://192.168.1.64:554/Streaming/Channels/101",
            "rtsps://cam.example.com/live",
            "rtmp://live.example.com/live/stream",
            "https://live.example.com/hls/index.m3u8?token=abc",
            "https://live.example.com/live/stream.flv",
            "https://live.example.com/watch?v=123",
        ):
            name, _rule, ok = detect_proto(u)
            print(f"   {'OK ' if ok else '-- '} {u:<62} -> {name}")
        print(f"active URL      : {win.stream_edit.text()}")
        print(f"detected label  : {win.proto_label.text()}")
        # A real computed check: sample the rendered background pixel.
        win.show()
        app.processEvents()
        img = win.grab().toImage()
        print(f"rendered size   : {img.width()} x {img.height()}")
        print(f"sample pixel    : {img.pixelColor(8, 300).name()}")
        return 0

    if args.shot:
        win.show()
        # Warm the glyph cache: under the offscreen platform the very first
        # painted cell can drop a thin glyph ("1" in the row-number column).
        for _ in range(3):
            app.processEvents()
        QTimer.singleShot(150, lambda: _save_and_quit(win, args.shot, app))
        return app.exec()

    win.show()
    return app.exec()


def _save_and_quit(win: QMainWindow, path: str, app: QApplication):
    out = Path(path)
    if not out.is_absolute():
        out = HERE / out
    ok = win.grab().save(str(out))
    print(f"screenshot       : {out}  ({'OK' if ok else 'FAILED'})")
    app.quit()


if __name__ == "__main__":
    raise SystemExit(main())
