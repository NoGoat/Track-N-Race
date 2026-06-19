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
            // Faint grid + axis lines (Qt Graphs' default is bright white). Grid
            // lines are aligned to our overlay labels via QValueAxis::tickInterval.
            grid.mainColor: Qt.rgba(0.59, 0.59, 0.59, 0.16)
            grid.subColor: "transparent"
            grid.mainWidth: 1
            axisX.mainColor: Qt.rgba(0.59, 0.59, 0.59, 0.5)
            axisY.mainColor: Qt.rgba(0.59, 0.59, 0.59, 0.5)
            // Native axis labels (Qt 6.12+) match the overlay's colour/size; no-op
            // on 6.11 where all labels are overlay-drawn.
            labelTextColor: vm.textColor
            axisX.labelTextColor: vm.textColor
            axisY.labelTextColor: vm.textColor
            axisXLabelFont.pixelSize: 11
            axisYLabelFont.pixelSize: 11
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
            // depth stacks multiple same-side axes into separate columns.
            readonly property bool isX: modelData.isX
            readonly property int colW: 40   // must match kAxisColW in ChartViewModel.cpp
            x: isX ? (root.xPix(modelData.t) - width / 2)
                   : (modelData.alignment === 2
                        ? pa.x + pa.width + 4 + modelData.depth * colW
                        : pa.x - width - 4 - modelData.depth * colW)
            y: isX ? (pa.y + pa.height + 3)
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
