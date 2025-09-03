// components/HomeSignal.qml
import QtQuick

Item {
    id: homeSignal

    // 
    // COMPONENT PROPERTIES (unchanged)
    // 
    property string signalId: ""
    property string signalName: ""
    property string currentAspect: "RED"
    property int aspectCount: 3
    property var possibleAspects: []
    property string callingOnAspect: "OFF"         // "WHITE", "DARK", "OFF"
    property string loopAspect: "OFF"              // "YELLOW", "DARK", "OFF"
    property string loopSignalConfiguration: "UR"
    property string direction: "UP"
    property bool isActive: true
    property string locationDescription: ""
    property int cellSize: 20

    // 
    //   CORRECT VISIBILITY AND ACTIVITY LOGIC
    // 

    //  VISIBILITY: OFF = don't render, DARK/ACTIVE = render
    property bool isLoopSignalVisible: loopAspect !== "OFF"
    property bool isCallingOnVisible: callingOnAspect !== "OFF"

    //  ACTIVITY: Only signature color = active, DARK = inactive but visible
    property bool isLoopSignalActive: loopAspect === "YELLOW"  // Signature color for loop
    property bool isCallingOnActive: callingOnAspect === "WHITE"  // Signature color for calling-on

    // 
    // PARSED CONFIGURATION PROPERTIES (unchanged)
    // 
    property string loopMastDirection: loopSignalConfiguration.charAt(0)
    property string loopArmDirection: loopSignalConfiguration.charAt(1)

    // 
    // DIMENSIONS (unchanged)
    // 
    property real scalingConstant: 15
    property real scalingFactor: 0.46875
    property real parentWidth: cellSize * scalingConstant
    property real parentHeight: parentWidth * scalingFactor

    width: parentWidth
    height: parentHeight

    // ... (dimension variables unchanged) ...
    property real mast1Width: parentWidth * 0.03125
    property real mast1Height: parentHeight * 0.4
    property real mast2Width: parentWidth * 0.025
    property real mast2Height: parentHeight * 0.43
    property real armSegment1Width: parentWidth * 0.1125
    property real armSegment2Width: parentWidth * 0.1125
    property real armSegment3Width: parentWidth * 0.08
    property real armHeight: parentHeight * 0.053
    property real callingOnWidth: parentWidth * 0.15625 * 0.75
    property real callingOnHeight: parentHeight * 0.333 * 0.75
    property real loopWidth: parentWidth * 0.15625 * 0.7
    property real loopHeight: parentHeight * 0.333 * 0.7
    property real circleWidth: parentWidth * 0.15625
    property real circleHeight: parentHeight * 0.333
    property real circleSpacing: 0
    property real borderWidth: 0.5
    property color borderColor: "#b3b3b3"

    // 
    //  ENHANCED: COLOR PROPERTIES WITH DARK STATE SUPPORT
    // 
    property color mastColor: "#ffffff"
    property color armColorActive: "#ffffff"
    property color armColorInactive: "#b3b3b3"
    property color lampOffColor: "#404040"
    property color darkStateColor: "#333333"        //  NEW: Dark but visible
    property color inactiveColor: "#606060"
    property color inactiveMastColor: "#888888"

    // Signal colors
    property color redAspectColor: "#ff0000"
    property color yellowAspectColor: "#ffff00"
    property color greenAspectColor: "#00ff00"
    property color callingOnColor: "#f0f8ff"        // WHITE
    property color loopColor: "#ffff00"             // YELLOW

    // Inactive signal properties
    readonly property real inactiveOpacity: 0.5
    readonly property color inactiveBorderColor: "#ff6600"
    readonly property real inactiveBorderWidth: 2

    // 
    //  ENHANCED: DYNAMIC COLOR LOGIC WITH OFF/DARK/ACTIVE SUPPORT
    // 
    property color currentArmColor: {
        if (!isActive) return inactiveMastColor;
        return (isCallingOnVisible) ? armColorActive : armColorInactive;
    }

    function getMastColor() {
        return isActive ? mastColor : inactiveMastColor;
    }

    function getLoopSignalColor() {
        if (!isActive) return inactiveColor;

        switch(loopAspect) {
            case "YELLOW": return loopColor          //  Active: Show signature color
            case "DARK": return darkStateColor       //  Dark: Visible but dark
            case "OFF": return lampOffColor          //  Off: Should not be visible anyway
            default: return lampOffColor             // Safe default
        }
    }

    function getCallingOnColor() {
        if (!isActive) return inactiveColor;

        switch(callingOnAspect) {
            case "WHITE": return callingOnColor      //  Active: Show signature color
            case "DARK": return darkStateColor       //  Dark: Visible but dark
            case "OFF": return lampOffColor          //  Off: Should not be visible anyway
            default: return lampOffColor             // Safe default
        }
    }

    function getMainLampColor(aspectToCheck) {
        if (!isActive) return inactiveColor;
        return (currentAspect === aspectToCheck) ? getAspectColor(aspectToCheck) : lampOffColor;
    }

    function getAspectColor(aspect) {
        switch(aspect) {
            case "RED": return redAspectColor;
            case "YELLOW": return yellowAspectColor;
            case "GREEN": return greenAspectColor;
            default: return lampOffColor;
        }
    }

    // 
    // DATABASE VALIDATION FUNCTIONS (unchanged)
    // 
    function isValidAspect(aspect) {
        if (possibleAspects.length === 0) return true;
        return possibleAspects.indexOf(aspect) !== -1;
    }

    function getAspectDisplayName(aspect) {
        switch(aspect) {
            case "RED": return "Danger"
            case "YELLOW": return "Caution"
            case "GREEN": return "Clear"
            default: return aspect
        }
    }

    function getSignalTypeDescription() {
        return "Home Signal (" + aspectCount + "-aspect)"
    }

    function isOperational() {
        return isActive && isValidAspect(currentAspect);
    }

    function getNextValidAspect() {
        if (possibleAspects.length === 0) return currentAspect;
        var currentIndex = possibleAspects.indexOf(currentAspect);
        var nextIndex = (currentIndex + 1) % possibleAspects.length;
        return possibleAspects[nextIndex];
    }

    signal signalClicked(string signalId, string currentAspect)
    signal contextMenuRequested(string signalId, string signalName, string currentAspect,
                              var possibleAspects, string callingOnAspect, string loopAspect,
                              real x, real y)

    // 
    //   UP SIGNAL LAYOUT WITH CORRECT VISIBILITY LOGIC
    // 
    Item {
        id: upSignalLayout
        visible: direction === "UP"
        anchors.left: parent.left
        anchors.verticalCenter: parent.verticalCenter
        opacity: isActive ? 1.0 : inactiveOpacity

        Row {
            id: mainHorizontalLine
            anchors.verticalCenter: parent.verticalCenter
            spacing: 0

            // **STEP 1: MAST 1**
            Rectangle {
                id: upMast1
                width: mast1Width
                height: mast1Height
                color: getMastColor()
                anchors.verticalCenter: parent.verticalCenter
            }

            // **STEP 2: ARM SEGMENT 1**
            Rectangle {
                id: upArmSegment1
                width: armSegment1Width
                height: armHeight
                color: currentArmColor
                anchors.verticalCenter: parent.verticalCenter
            }

            // ** STEP 3: CALLING-ON SIGNAL - CONDITIONAL VISIBILITY**
            Rectangle {
                id: upCallingOnSignal
                width: callingOnWidth
                height: callingOnHeight
                radius: width / 2
                color: getCallingOnColor()
                border.color: borderColor
                border.width: borderWidth
                anchors.verticalCenter: parent.verticalCenter
                visible: isCallingOnVisible  //   Only render if not OFF
            }

            // ** STEP 4: ARM SEGMENT 2 - CONDITIONAL VISIBILITY FOR LOOP**
            Item {
                id: upArmSegment2Container
                width: armSegment2Width
                height: armHeight
                anchors.verticalCenter: parent.verticalCenter
                visible: isLoopSignalVisible  //   Show if not OFF (even if DARK)

                // Arm segment 2 background
                Rectangle {
                    id: upArmSegment2
                    anchors.fill: parent
                    color: currentArmColor
                }

                // **DYNAMIC MAST 2**
                Rectangle {
                    id: upMast2
                    width: mast2Width
                    height: mast2Height
                    color: getMastColor()
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: isLoopSignalVisible  //   Based on visibility, not activity

                    anchors.bottom: loopMastDirection === "U" ? parent.top : undefined
                    anchors.top: loopMastDirection === "D" ? parent.bottom : undefined
                }

                // **DYNAMIC ARM SEGMENT 3**
                Rectangle {
                    id: upArmSegment3
                    width: armSegment3Width
                    height: armHeight
                    color: currentArmColor
                    visible: isLoopSignalVisible  //   Based on visibility

                    anchors.left: loopArmDirection === "R" ? upMast2.right : undefined
                    anchors.right: loopArmDirection === "L" ? upMast2.left : undefined
                    anchors.verticalCenter: loopMastDirection === "U" ? upMast2.top : upMast2.bottom
                }

                // ** DYNAMIC LOOP SIGNAL - ALWAYS VISIBLE IF CONTAINER IS VISIBLE**
                Rectangle {
                    id: upLoopSignal
                    width: loopWidth
                    height: loopHeight
                    radius: width / 2
                    color: getLoopSignalColor()  //  Color shows state (YELLOW/DARK)
                    border.color: borderColor
                    border.width: borderWidth
                    visible: isLoopSignalVisible  //   Always visible if not OFF

                    anchors.left: loopArmDirection === "R" ? upArmSegment3.right : undefined
                    anchors.right: loopArmDirection === "L" ? upArmSegment3.left : undefined
                    anchors.verticalCenter: upArmSegment3.verticalCenter
                }
            }

            // **STEP 5: MAIN HOME SIGNAL CIRCLES** (unchanged)
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: circleSpacing

                Rectangle {
                    width: circleWidth
                    height: circleHeight
                    radius: width / 2
                    color: getMainLampColor("RED")
                    border.color: borderColor
                    border.width: borderWidth
                }

                Rectangle {
                    width: circleWidth
                    height: circleHeight
                    radius: width / 2
                    color: getMainLampColor("YELLOW")
                    border.color: borderColor
                    border.width: borderWidth
                    visible: aspectCount >= 2
                }

                Rectangle {
                    width: circleWidth
                    height: circleHeight
                    radius: width / 2
                    color: getMainLampColor("GREEN")
                    border.color: borderColor
                    border.width: borderWidth
                    visible: aspectCount >= 3
                }
            }
        }
    }

    // 
    //   DOWN SIGNAL LAYOUT (Same visibility logic as UP)
    // 
    Item {
        id: downSignalLayout
        visible: direction === "DOWN"
        anchors.right: parent.right
        anchors.verticalCenter: parent.verticalCenter
        opacity: isActive ? 1.0 : inactiveOpacity

        Row {
            id: downMainHorizontalLine
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            layoutDirection: Qt.RightToLeft
            spacing: 0

            Rectangle {
                id: downMast1
                width: mast1Width
                height: mast1Height
                color: getMastColor()
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                id: downArmSegment1
                width: armSegment1Width
                height: armHeight
                color: currentArmColor
                anchors.verticalCenter: parent.verticalCenter
            }

            // ** CALLING-ON SIGNAL - CONDITIONAL VISIBILITY**
            Rectangle {
                id: downCallingOnSignal
                width: callingOnWidth
                height: callingOnHeight
                radius: width / 2
                color: getCallingOnColor()
                border.color: borderColor
                border.width: borderWidth
                anchors.verticalCenter: parent.verticalCenter
                visible: isCallingOnVisible  //   Only render if not OFF
            }

            // ** ARM SEGMENT 2 - CONDITIONAL VISIBILITY FOR LOOP**
            Item {
                id: downArmSegment2Container
                width: armSegment2Width
                height: armHeight
                anchors.verticalCenter: parent.verticalCenter
                visible: isLoopSignalVisible  //   Show if not OFF

                Rectangle {
                    id: downArmSegment2
                    anchors.fill: parent
                    color: currentArmColor
                }

                Rectangle {
                    id: downMast2
                    width: mast2Width
                    height: mast2Height
                    color: getMastColor()
                    anchors.horizontalCenter: parent.horizontalCenter
                    visible: isLoopSignalVisible

                    anchors.bottom: loopMastDirection === "U" ? parent.top : undefined
                    anchors.top: loopMastDirection === "D" ? parent.bottom : undefined
                }

                Rectangle {
                    id: downArmSegment3
                    width: armSegment3Width
                    height: armHeight
                    color: currentArmColor
                    visible: isLoopSignalVisible

                    anchors.left: loopArmDirection === "L" ? downMast2.right : undefined
                    anchors.right: loopArmDirection === "R" ? downMast2.left : undefined
                    anchors.verticalCenter: loopMastDirection === "U" ? downMast2.top : downMast2.bottom
                }

                // ** LOOP SIGNAL**
                Rectangle {
                    id: downLoopSignal
                    width: loopWidth
                    height: loopHeight
                    radius: width / 2
                    color: getLoopSignalColor()  //  Color shows state
                    border.color: borderColor
                    border.width: borderWidth
                    visible: isLoopSignalVisible  //   Always visible if not OFF

                    anchors.left: loopArmDirection === "L" ? downArmSegment3.right : undefined
                    anchors.right: loopArmDirection === "R" ? downArmSegment3.left : undefined
                    anchors.verticalCenter: downArmSegment3.verticalCenter
                }
            }

            // **MAIN SIGNAL CIRCLES** (unchanged)
            Row {
                anchors.verticalCenter: parent.verticalCenter
                spacing: circleSpacing

                Rectangle {
                    width: circleWidth
                    height: circleHeight
                    radius: width / 2
                    color: getMainLampColor("GREEN")
                    border.color: borderColor
                    border.width: borderWidth
                    visible: aspectCount >= 3
                }

                Rectangle {
                    width: circleWidth
                    height: circleHeight
                    radius: width / 2
                    color: getMainLampColor("YELLOW")
                    border.color: borderColor
                    border.width: borderWidth
                    visible: aspectCount >= 2
                }

                Rectangle {
                    width: circleWidth
                    height: circleHeight
                    radius: width / 2
                    color: getMainLampColor("RED")
                    border.color: borderColor
                    border.width: borderWidth
                }
            }
        }
    }

    // 
    // INACTIVE SIGNAL OVERLAY AND INTERACTION (unchanged)
    // 
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

    MouseArea {
        id: mouseArea
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        cursorShape: isOperational() ? Qt.PointingHandCursor : Qt.ForbiddenCursor

        onClicked: function(mouse) {
            if (!isOperational()) {
                console.log("Home signal operation blocked:", signalId,
                           "Active:", isActive,
                           "Valid aspect:", isValidAspect(currentAspect))
                return
            }

            if (mouse.button === Qt.LeftButton) {
                //  NEW: Left-click shows context menu (moved from right-click)
                console.log("Home signal left-clicked:", signalId, "Showing context menu")
                homeSignal.contextMenuRequested(signalId, signalName, currentAspect, possibleAspects,
                                                callingOnAspect, loopAspect,
                                                mouse.x + homeSignal.x, mouse.y + homeSignal.y)
            } else if (mouse.button === Qt.RightButton) {
                //  NEW: Right-click reserved for future route assignment
                console.log("Home signal right-clicked:", signalId, "Reserved for route assignment")
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
