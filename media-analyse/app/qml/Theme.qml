pragma Singleton
import QtQuick

// PRISM design tokens (see media-analyzer-redesign/media-analyzer.qss).
// Single source of truth for the QML port: colours, type scale, spacing and
// the frame-table column contract.
QtObject {
    readonly property color bg:            "#14171B"
    readonly property color sunken:        "#101317"
    readonly property color surface1:      "#1B1F25"
    readonly property color surface2:      "#232830"
    readonly property color surface3:      "#2C333D"
    readonly property color line:          "#2A3038"
    readonly property color line2:         "#363E49"
    readonly property color line3:         "#4C5663"
    readonly property color rowLine:       "#1D2229"

    readonly property color text1:         "#E8EAED"
    readonly property color text2:         "#A9B2BE"
    readonly property color text3:         "#7E8998"
    readonly property color text4:         "#5B6572"

    readonly property color accent:        "#E0A44A"
    readonly property color accentHover:   "#EFC06C"
    readonly property color accentWash:    "#33291B"
    readonly property color accentInk:     "#1A1509"
    readonly property color accentEdge:    "#6B5227"

    readonly property color video:         "#82A9CC"
    readonly property color audio:         "#C99A72"
    readonly property color success:       "#86B49A"
    readonly property color error:         "#D97B72"
    readonly property color altRow:        "#161A20"

    // The user explicitly asked for QTableWidget-like blue-on-white cell
    // selection. Keep that semantic even though the reference HTML uses an
    // amber wash: selection behaviour outranks the colour suggestion.
    readonly property color selection:     "#1A73E8"
    readonly property color selectionText:"#FFFFFF"

    readonly property string uiFont:       "Segoe UI"
    readonly property string monoFont:     "Consolas"

    readonly property int rowHeight:       30
    readonly property int headerHeight:    34
    readonly property int controlHeight:   34

    readonly property var columns: [
        "#", "pkt#", "stream", "dts", "dts-time", "dts-delta", "pts",
        "pts-time", "duration", "size", "type", "samples",
        "dims", "format", "ch"
    ]
}
