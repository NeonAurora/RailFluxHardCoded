import QtQuick

Item {
    id: levelCrossing

    property string gateId: ""
    property string gateName: ""
    property string gateState: "OPEN"  // OPEN, CLOSED
    property int cellSize: 20

    width: cellSize * 2
    height: cellSize * 2

    signal gateClicked(string gateId)

    // Basic gate representation
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: "#ffffff"
        border.width: 2

        // Gate barrier
        Rectangle {
            width: parent.width * 0.8
            height: 4
            anchors.centerIn: parent
            color: gateState === "CLOSED" ? "#e53e3e" : "#38a169"
            rotation: gateState === "CLOSED" ? 0 : -45

            Behavior on rotation {
                NumberAnimation { duration: 300 }
            }
        }

        // Status indicator
        Rectangle {
            width: 8
            height: 8
            anchors.top: parent.top
            anchors.right: parent.right
            color: gateState === "CLOSED" ? "#e53e3e" : "#38a169"
            radius: 4
        }
    }

    // Gate ID
    Text {
        anchors.bottom: parent.bottom
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottomMargin: -15
        text: gateId
        color: "#ffffff"
        font.pixelSize: 8
    }

    MouseArea {
        anchors.fill: parent
        cursorShape: Qt.PointingHandCursor
        onClicked: levelCrossing.gateClicked(gateId)
    }
}
