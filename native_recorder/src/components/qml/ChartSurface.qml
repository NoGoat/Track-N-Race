import QtQuick
import QtGraphs

// Host scene for one ChartView. The C++ ChartViewModel is exposed as the context
// property `vm`; it owns the series/axes added to `graphsView`.
//
// Axes, grid, tick marks and axis labels are drawn NATIVELY by Qt Graphs (there is
// no more hand-drawn tick overlay, and no background bands — Qt Graphs can't do
// those). The only custom overlays kept are the hover crosshair + value tooltip and
// the legend, which Qt Graphs has no native equivalent for. Kept generic — every
// chart type reuses it.
Item {
    id: root
    anchors.fill: parent

    // Plot rectangle (in this item's coordinates) that the series occupy.
    readonly property rect pa: graphsView.plotArea

    function xPix(t) { return pa.x + t * pa.width; }

    GraphsView {
        id: graphsView
        objectName: "graphsView"
        anchors.fill: parent

        theme: GraphsTheme {
            colorScheme: GraphsTheme.ColorScheme.Dark
            // Transparent so the chart blends into the widget background.
            backgroundVisible: false
            plotAreaBackgroundVisible: false
            grid.mainColor: vm.gridColor
            grid.subColor: "transparent"
            axisX.mainColor: vm.axisLineColor
            axisY.mainColor: vm.axisLineColor
            // Native axis label colour (one slot per axis; Qt Graphs has no per-tick
            // or secondary-axis label colour).
            labelTextColor: vm.textColor
            axisX.labelTextColor: vm.textColor
            axisY.labelTextColor: vm.textColor
            axisXLabelFont.pixelSize: 11
            axisYLabelFont.pixelSize: 11
        }
    }

    // ── Hover crosshair ──────────────────────────────────────────────────────
    Rectangle {
        visible: vm.crosshairVisible
        width: 1
        color: Qt.rgba(0.59, 0.59, 0.59, 0.63)
        x: root.xPix(vm.crosshairT)
        y: pa.y
        height: pa.height
    }

    MouseArea {
        id: hover
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.NoButton
        property real curX: 0
        property real curY: 0
        onPositionChanged: (m) => {
            curX = m.x; curY = m.y;
            if (pa.width <= 0 || m.x < pa.x || m.x > pa.x + pa.width) { vm.hoverLeave(); return; }
            vm.hoverAt((m.x - pa.x) / pa.width);
        }
        onExited: vm.hoverLeave()
    }

    // ── Tooltip (rich text, follows the cursor, flips at edges) ──────────────
    Rectangle {
        id: tip
        visible: vm.tooltipVisible
        color: Qt.rgba(0.08, 0.08, 0.08, 0.82)
        border.color: Qt.rgba(0.59, 0.59, 0.59, 0.35)
        border.width: 1
        radius: 4
        width: tipText.width + 16
        height: tipText.height + 10
        readonly property int pad: 14
        x: (hover.curX + pad + width > root.width) ? hover.curX - pad - width : hover.curX + pad
        y: (hover.curY + pad + height > root.height) ? hover.curY - pad - height : hover.curY + pad
        Text {
            id: tipText
            x: 8; y: 5
            textFormat: Text.RichText
            text: vm.tooltipHtml
        }
    }

    // ── Overlay legend (top-centered) ────────────────────────────────────────
    Row {
        visible: vm.legendVisible
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 4
        spacing: 12
        Repeater {
            model: vm.legendEntries
            Row {
                spacing: 4
                Rectangle { width: 10; height: 10; radius: 2; color: modelData.color
                            anchors.verticalCenter: parent.verticalCenter }
                Text { text: modelData.name; color: vm.textColor; font.pixelSize: 11 }
            }
        }
    }
}
