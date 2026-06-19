import QtQuick
import QtGraphs

// Host scene for one ChartView. The C++ ChartViewModel is exposed as the context
// property `vm`; it owns the series/axes added to `graphsView` and feeds the
// overlays below (axis labels, crosshair, tooltip, legend, bands) that Qt Graphs
// can't draw itself. Kept deliberately generic — every chart type reuses it.
Item {
    id: root
    anchors.fill: parent

    // Plot rectangle (in this item's coordinates) that the axes/series occupy.
    readonly property rect pa: graphsView.plotArea

    // Stroke width (logical px) for the cartesian grid and the axis tick marks.
    readonly property real lineW: 2
    // Gap between a tick label and the plot edge, and the length of the tick mark
    // that bridges it. Must stay in sync with updateMargins() in ChartViewModel.cpp.
    readonly property real labelGap: 8
    readonly property real tickLen: 6

    function xPix(t) { return pa.x + t * pa.width; }
    function yPix(t) { return pa.y + (1.0 - t) * pa.height; }   // value grows upward

    // ── Background bands (behind the transparent GraphsView) ─────────────────
    Repeater {
        model: vm.bandRects
        Rectangle {
            x: pa.x
            width: pa.width
            y: root.yPix(modelData.t1)
            height: root.yPix(modelData.t0) - root.yPix(modelData.t1)
            color: modelData.color
        }
    }



    GraphsView {
        id: graphsView
        objectName: "graphsView"
        anchors.fill: parent

        theme: GraphsTheme {
            colorScheme: GraphsTheme.ColorScheme.Dark
            // Transparent so the band rectangles underneath remain visible and the
            // chart blends into the widget background like the old QCustomPlot did.
            backgroundVisible: false
            plotAreaBackgroundVisible: false
            grid.mainColor: vm.gridColor
            grid.subColor: "transparent"
            axisX.mainColor: vm.axisLineColor
            axisY.mainColor: vm.axisLineColor
            // Native axis labels (Qt 6.12+) match the overlay's colour/size; no-op
            // on 6.11 where all labels are overlay-drawn.
            labelTextColor: vm.textColor
            axisX.labelTextColor: vm.textColor
            axisY.labelTextColor: vm.textColor
            axisXLabelFont.pixelSize: 11
            axisYLabelFont.pixelSize: 11
        }
    }

    // ── Axis tick marks (short lines tying each number to the plot edge) ──────
    Repeater {
        model: vm.axisLabels
        Rectangle {
            readonly property bool isX: modelData.isX
            readonly property bool onRight: modelData.alignment === 2   // AlignRight
            color: vm.axisLineColor
            antialiasing: false
            x: isX ? root.xPix(modelData.t)
                   : (onRight ? pa.x + pa.width : pa.x - root.tickLen)
            y: isX ? (pa.y + pa.height) : root.yPix(modelData.t)
            width:  isX ? root.lineW : root.tickLen
            height: isX ? root.tickLen : root.lineW
        }
    }

    // ── Custom axis tick labels ──────────────────────────────────────────────
    Repeater {
        model: vm.axisLabels
        Text {
            text: modelData.text
            color: modelData.color
            font.pixelSize: 11
            // Qt::AlignBottom == 0x20 (x axis), AlignLeft == 1, AlignRight == 2.
            // depth stacks multiple same-side axes into separate columns. labelGap
            // sits the number past the tick mark; the X label also clears the ticks.
            readonly property bool isX: modelData.isX
            readonly property int colW: 40   // must match kAxisColW in ChartViewModel.cpp
            x: isX ? (root.xPix(modelData.t) - width / 2)
                   : (modelData.alignment === 2
                        ? pa.x + pa.width + root.labelGap + modelData.depth * colW
                        : pa.x - width - root.labelGap - modelData.depth * colW)
            y: isX ? (pa.y + pa.height + root.labelGap)
                   : (root.yPix(modelData.t) - height / 2)
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
