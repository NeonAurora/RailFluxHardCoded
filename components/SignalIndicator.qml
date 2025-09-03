import QtQuick

Item {
    id: signalIndicator

    property string signalId: ""
    property string signalType: "STARTER"  // STARTER, HOME, OUTER, ADVANCED_STARTER, SHUNT
    property string currentAspect: "RED"
    property string signalName: ""
    property bool isActive: true
    property int cellSize: 20

    // Size matching your train_detection_icon.py proportions
    width: cellSize * 5.0   // Your width scaling
    height: cellSize * 1.6  // Your height scaling

    // **CLICKABLE FUNCTIONALITY**
    signal signalClicked(string signalId, string signalType, string currentAspect)
    signal signalDoubleClicked(string signalId)

    // Main signal container (your rounded rectangle)
    Rectangle {
        id: signalContainer
        width: parent.width
        height: parent.height
        color: "transparent"
        border.color: "#ffffff"  // White outline like your design
        border.width: 1
        radius: height / 2  // Fully rounded ends

        // **HOVER EFFECT**
        Rectangle {
            anchors.fill: parent
            color: "white"
            opacity: mouseArea.containsMouse ? 0.1 : 0
            radius: parent.radius

            Behavior on opacity {
                NumberAnimation { duration: 150 }
            }
        }

        // **4 CIRCLES** (your train_detection_icon.py design)
        Row {
            anchors.centerIn: parent
            spacing: (parent.width - (4 * circleRadius * 2)) / 5  // Even spacing

            property real circleRadius: cellSize * 0.5

            // **SIGNAL STATE CIRCLE** (left-most, changes color)
            Rectangle {
                width: parent.circleRadius * 2
                height: parent.circleRadius * 2
                radius: parent.circleRadius
                color: getAspectColor()
                border.color: "#ffffff"
                border.width: 1

                // **SIGNAL GLOW EFFECT** for active signals
                Rectangle {
                    anchors.centerIn: parent
                    width: parent.width * 0.6
                    height: parent.height * 0.6
                    color: "white"
                    opacity: isActive ? 0.4 : 0
                    radius: width / 2

                    Behavior on opacity {
                        NumberAnimation { duration: 300 }
                    }
                }
            }

            // **3 INDICATOR CIRCLES** (white outlines)
            Repeater {
                model: 3
                Rectangle {
                    width: parent.circleRadius * 2
                    height: parent.circleRadius * 2
                    radius: parent.circleRadius
                    color: "transparent"
                    border.color: "#ffffff"
                    border.width: 1
                }
            }
        }

        // **SIGNAL TYPE INDICATOR**
        Rectangle {
            id: typeIndicator
            width: cellSize * 0.9  // Your stem_width
            height: cellSize * 0.9
            color: "#ffffff"
            anchors.right: parent.left
            anchors.verticalCenter: parent.verticalCenter

            // Signal type letter
            Text {
                anchors.centerIn: parent
                text: getSignalTypeLetter()
                color: "#000000"
                font.pixelSize: 8
                font.bold: true
            }
        }
    }

    // **SIGNAL NAME LABEL**
    Text {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: -15
        text: signalName || signalId
        color: "#ffffff"
        font.pixelSize: 10
        font.family: "Arial"  // Professional font
    }

    // **CLICKABLE MOUSE AREA**
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: Qt.PointingHandCursor

        onClicked: {
            console.log("Signal clicked:", signalId, signalType, currentAspect)
            signalIndicator.signalClicked(signalId, signalType, currentAspect)
        }

        onDoubleClicked: {
            console.log("Signal double-clicked:", signalId)
            signalIndicator.signalDoubleClicked(signalId)
        }
    }

    // **HELPER FUNCTIONS** (your color logic)
    function getAspectColor() {
        switch(currentAspect) {
            case "RED": return "#e53e3e"      // Your danger red
            case "YELLOW": return "#d69e2e"   // Your warning yellow
            case "GREEN": return "#38a169"    // Your success green
            case "WHITE": return "#ffffff"
            case "BLUE": return "#3182ce"     // Your accent blue
            default: return "#e53e3e"         // Safe default RED
        }
    }

    function getSignalTypeLetter() {
        switch(signalType) {
            case "STARTER": return "S"
            case "HOME": return "H"
            case "OUTER": return "O"
            case "ADVANCED_STARTER": return "A"
            case "SHUNT": return "SH"
            default: return "?"
        }
    }
}
