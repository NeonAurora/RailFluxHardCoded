import QtQuick

Item {
    id: canvas

    property int gridSize: 20
    property bool showGrid: true

    // Grid color scheme for railway engineering
    property color gridColorNormal: "#333333"      // Subtle base grid
    property color gridColorMajor: "#666666"       // Every 10th line (white-ish)
    property color gridColorPrimary: "#cc0000"     // Every 20th line (red)
    property real gridOpacity: 0.4

    // VERTICAL GRID LINES
    Repeater {
        model: showGrid ? Math.ceil(canvas.width / gridSize) + 1 : 0

        Rectangle {
            x: index * gridSize
            y: 0
            width: getLineWidth(index)
            height: canvas.height
            color: getGridColor(index)
            opacity: canvas.gridOpacity
        }
    }

    // HORIZONTAL GRID LINES
    Repeater {
        model: showGrid ? Math.ceil(canvas.height / gridSize) + 1 : 0

        Rectangle {
            x: 0
            y: index * gridSize
            width: canvas.width
            height: getLineWidth(index)
            color: getGridColor(index)
            opacity: canvas.gridOpacity
        }
    }

    // GRID LINE STYLING FUNCTIONS
    function getGridColor(lineIndex) {
        if (lineIndex % 20 === 0) {
            return gridColorPrimary    // Every 20th = RED
        } else if (lineIndex % 10 === 0) {
            return gridColorMajor      // Every 10th = WHITE-ISH
        } else {
            return gridColorNormal     // Regular = SUBTLE
        }
    }

    function getLineWidth(lineIndex) {
        if (lineIndex % 20 === 0) {
            return 2    // Primary grid lines = 2px wide
        } else if (lineIndex % 10 === 0) {
            return 1    // Major grid lines = 1px wide
        } else {
            return 1    // Normal grid lines = 1px wide
        }
    }

    // GRID LABELS (Optional - like CAD software)
    // Row numbers on left side
    Repeater {
        model: showGrid ? Math.floor(canvas.height / gridSize / 10) : 0

        Text {
            x: 2
            y: (index * 10 * gridSize) + 2
            text: (index * 10).toString()
            color: "#999999"
            font.pixelSize: 8
            font.family: "monospace"
        }
    }

    // Column numbers on top
    Repeater {
        model: showGrid ? Math.floor(canvas.width / gridSize / 10) : 0

        Text {
            x: (index * 10 * gridSize) + 2
            y: 2
            text: (index * 10).toString()
            color: "#999999"
            font.pixelSize: 8
            font.family: "monospace"
        }
    }
}
