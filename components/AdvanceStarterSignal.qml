// components/AdvanceStarterSignal.qml
import QtQuick

Item {
    id: advanceStarterSignal

    property string signalId: ""
    property string signalName: ""
    property string currentAspect: "RED"
    property int aspectCount: 2                     //  NEW: Always 2-aspect (RED/GREEN)
    property var possibleAspects: []               //  NEW: Valid aspects from database
    property string direction: "UP"
    property bool isActive: true                   //  NEW: Signal active status from database
    property string locationDescription: ""        //  NEW: Location info from database (not rendered)
    property int cellSize: 20

    property real scalingConstant: 15
    property real scalingFactor: 0.46875
    property real parentWidth: cellSize * scalingConstant
    property real parentHeight: parentWidth * scalingFactor

    width: parentWidth
    height: parentHeight
    property real mastWidth: parentWidth * 0.03125
    property real mastHeight: parentHeight * 0.4

    property real armWidth: parentWidth * 0.1125
    property real armHeight: parentHeight * 0.053

    property real circleWidth: parentWidth * 0.15625
    property real circleHeight: parentHeight * 0.333
    property real circleSpacing: 0

    // 
    // BORDER GROUP
    // 
    property real borderWidth: 0.5
    property color borderColor: "#b3b3b3"

    // 
    //  ENHANCED: BASE COLOR GROUP WITH INACTIVE SUPPORT
    // 
    property color mastColor: "#ffffff"
    property color armColor: "#ffffff"
    property color lampOffColor: "#404040"
    property color inactiveColor: "#606060"
    property color inactiveMastColor: "#888888"

    // 
    // SIGNAL COLOR GROUP (Only RED and GREEN for Advanced Starter)
    // 
    property color redAspectColor: "#ff0000"
    property color greenAspectColor: "#00ff00"

    // 
    //  NEW: INACTIVE SIGNAL PROPERTIES
    // 
    readonly property real inactiveOpacity: 0.5
    readonly property color inactiveBorderColor: "#ff6600"
    readonly property real inactiveBorderWidth: 2

    signal signalClicked(string signalId, string currentAspect)
    signal contextMenuRequested(string signalId, string signalName, string currentAspect,
                              var possibleAspects, real x, real y)

    // 
    //  NEW: DATABASE VALIDATION FUNCTIONS
    // 
    function isValidAspect(aspect) {
        if (possibleAspects.length === 0) return true;
        return possibleAspects.indexOf(aspect) !== -1;
    }

    function isOperational() {
        return isActive && isValidAspect(currentAspect);
    }

    // 
    //  NEW: ENHANCED COLOR FUNCTIONS WITH VALIDATION
    // 
    function getMastColor() {
        return isActive ? mastColor : inactiveMastColor;
    }

    function getArmColor() {
        return isActive ? armColor : inactiveMastColor;
    }

    function getLampColor(aspectToCheck) {
        if (!isActive) return inactiveColor;

        switch(aspectToCheck) {
            case "RED":
                return currentAspect === "RED" ? redAspectColor : lampOffColor;
            case "GREEN":
                return currentAspect === "GREEN" ? greenAspectColor : lampOffColor;
            default:
                return lampOffColor;
        }
    }

    // 
    // UP SIGNAL LAYOUT: mast → arm → circles (RED, GREEN)
    // 
    Row {
        id: upSignalLayout
        visible: direction === "UP"
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        spacing: 0
        opacity: isActive ? 1.0 : inactiveOpacity

        Rectangle {
            id: upMast
            width: mastWidth
            height: mastHeight
            color: getMastColor()
            anchors.verticalCenter: parent.verticalCenter
        }

        Rectangle {
            id: upArm
            width: armWidth
            height: armHeight
            color: getArmColor()
            anchors.verticalCenter: parent.verticalCenter
        }

        // ** UP SEQUENCE: RED (Left), GREEN (Right)**
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: circleSpacing

            // **UP LAMP 1: RED (Left)**
            Rectangle {
                width: circleWidth
                height: circleHeight
                radius: width / 2
                color: getLampColor("RED")
                border.color: borderColor
                border.width: borderWidth
            }

            // **UP LAMP 2: GREEN (Right)**
            Rectangle {
                width: circleWidth
                height: circleHeight
                radius: width / 2
                color: getLampColor("GREEN")
                border.color: borderColor
                border.width: borderWidth
            }
        }
    }

    // 
    // DOWN SIGNAL LAYOUT: circles → arm → mast (GREEN, RED) - MIRRORED
    // 
    Row {
        id: downSignalLayout
        visible: direction === "DOWN"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        layoutDirection: Qt.RightToLeft
        spacing: 0
        opacity: isActive ? 1.0 : inactiveOpacity

        Rectangle {
            id: downMast
            width: mastWidth
            height: mastHeight
            color: getMastColor()
            anchors.verticalCenter: parent.verticalCenter
        }

        Rectangle {
            id: downArm
            width: armWidth
            height: armHeight
            color: getArmColor()
            anchors.verticalCenter: parent.verticalCenter
        }

        // ** DOWN SEQUENCE: GREEN (Left visually), RED (Right visually)**
        Row {
            anchors.verticalCenter: parent.verticalCenter
            spacing: circleSpacing

            // **DOWN LAMP 1: GREEN (Left visually)**
            Rectangle {
                width: circleWidth
                height: circleHeight
                radius: width / 2
                color: getLampColor("GREEN")
                border.color: borderColor
                border.width: borderWidth
            }

            // **DOWN LAMP 2: RED (Right visually)**
            Rectangle {
                width: circleWidth
                height: circleHeight
                radius: width / 2
                color: getLampColor("RED")
                border.color: borderColor
                border.width: borderWidth
            }
        }
    }

    //  NEW: INACTIVE SIGNAL OVERLAY
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: inactiveBorderColor
        border.width: inactiveBorderWidth
        radius: 4
        visible: !isActive
        opacity: 0.7

        Rectangle {
            width: parent.width * 1.414
            height: 1
            color: inactiveBorderColor
            anchors.centerIn: parent
            rotation: 45
            opacity: 0.6
        }
        Rectangle {
            width: parent.width * 1.414
            height: 1
            color: inactiveBorderColor
            anchors.centerIn: parent
            rotation: -45
            opacity: 0.6
        }
    }

    // 
    //  ENHANCED: INTERACTION WITH DATABASE VALIDATION
    // 
    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: isOperational() ? Qt.PointingHandCursor : Qt.ForbiddenCursor

        onClicked: function(mouse) {
            if (!isOperational()) {
                console.log("Advanced starter signal operation blocked:", signalId, "Active:", isActive)
                return
            }

            if (mouse.button === Qt.LeftButton) {
                //  NEW: Left-click shows context menu (moved from right-click)
                console.log("Advanced starter signal left-clicked:", signalId, "Showing context menu")
                advanceStarterSignal.contextMenuRequested(signalId, signalName, currentAspect, possibleAspects,
                                                         mouse.x + advanceStarterSignal.x, mouse.y + advanceStarterSignal.y)
            } else if (mouse.button === Qt.RightButton) {
                //  NEW: Right-click reserved for future route assignment
                console.log("Advanced starter signal right-clicked:", signalId, "Reserved for route assignment")
                // TODO: Future route assignment functionality
                if (routeAssignmentDialog) {
                    routeAssignmentDialog.openForSignal(signalId, signalName)
                }
            }
        }

        Rectangle {
            anchors.fill: parent
            color: isOperational() ? "white" : "red"
            opacity: parent.containsMouse ? 0.1 : 0
            radius: 4

            Behavior on opacity {
                NumberAnimation { duration: 150 }
            }
        }
    }
}
