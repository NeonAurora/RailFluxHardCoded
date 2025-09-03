// components/TrackSegment.qml
import QtQuick

Item {
    id: trackSegment

    // 
    // COMPONENT PROPERTIES
    // 
    property string segmentId: ""
    property string segmentName: ""           //  NEW: Track segment name from database
    property real startRow: 0
    property real startCol: 0
    property real endRow: 0
    property real endCol: 0
    property bool isOccupied: false
    property bool isAssigned: false
    property bool isOverlap: false
    property string occupiedBy: ""            //  NEW: What/who occupies this trackSegment
    property bool isActive: true              //  NEW: Whether trackSegment is active/in-service
    property int cellSize: 20
    property string trackSegmentType: "STRAIGHT"     //  ENHANCED: Track Segment type from database

    // 
    // VISUAL CONFIGURATION CONSTANTS
    // 

    // **TRACK SEGMENT VISUAL PROPERTIES**
    readonly property real trackSegmentThickness: 8
    readonly property real trackSegmentRadius: 2
    readonly property real containerPadding: 8
    readonly property real minimumContainerSize: 20

    // **RAIL LINE PROPERTIES**
    readonly property real railLineThickness: 1
    readonly property real railLineMargin: 1

    // ** ENHANCED: TRACK SEGMENT STATE COLORS WITH TRACK SEGMENT SEGMENT TYPE SUPPORT**
    readonly property color trackSegmentColorNormal: getTrackSegmentTypeColor()
    readonly property color trackSegmentColorOccupied: "#ff3232"       // Red for occupied
    readonly property color trackSegmentColorAssigned: "#00ffff"      // Yellow for assigned
    readonly property color trackSegmentColorOverlap: "#ffff00"
    readonly property color trackSegmentColorInactive: "#606060"      // Dark gray for inactive
    readonly property color railLineColor: "#a6a6a6"

    // ** NEW: TRACK SEGMENT SEGMENT TYPE SPECIFIC COLORS**
    readonly property color straightTrackSegmentColor: "#a6a6a6"      // Standard gray
    readonly property color curvedTrackSegmentColor: "#9999aa"        // Slightly blue-gray
    readonly property color sidingTrackSegmentColor: "#aa9966"        // Brown-ish for sidings
    readonly property color platformTrackSegmentColor: "#66aa99"      // Teal for platform trackSegments
    readonly property color yardTrackSegmentColor: "#996699"          // Purple-ish for yard trackSegments

    // **INTERACTION VISUAL PROPERTIES**
    readonly property real hoverOpacity: 0.2
    readonly property color hoverColor: "white"
    readonly property int hoverAnimationDuration: 150

    // ** NEW: OCCUPIED BY LABEL PROPERTIES**
    readonly property real occupiedLabelFontSize: 7
    readonly property color occupiedLabelColor: "#ffffff"
    readonly property color occupiedLabelBackground: "#000000"
    readonly property real occupiedLabelOpacity: 0.8

    // **DEBUG VISUAL PROPERTIES**
    readonly property real debugTextPadding: 6
    readonly property real debugBackgroundPadding: 4
    readonly property color debugBackgroundColor: "#000000"
    readonly property color debugTextColor: "yellow"
    readonly property real debugTextOpacity: 0.8
    readonly property real debugTextSize: 7
    readonly property real debugBorderRadius: 2

    // **CONTAINER DEBUG PROPERTIES**
    readonly property real debugBorderWidth: 1
    readonly property real debugBorderOpacity: 0.4
    readonly property color debugBorderColorDiagonal: "red"
    readonly property color debugBorderColorStraight: "cyan"

    // **CURSOR AND INTERACTION**
    readonly property int clickCursor: Qt.PointingHandCursor

    // 
    // COMPUTED PROPERTIES
    // 

    // **DIRECTION DETECTION**
    readonly property bool isHorizontal: Math.abs(endCol - startCol) > Math.abs(endRow - startRow)
    readonly property bool isVertical: Math.abs(endRow - startRow) > Math.abs(endCol - startCol)
    readonly property bool isDiagonal: !isHorizontal && !isVertical

    // **DIAGONAL DIRECTION DETECTION**
    readonly property bool isTopLeftToBottomRight: isDiagonal && (endRow > startRow)
    readonly property bool isBottomLeftToTopRight: isDiagonal && (endRow < startRow)

    // **PIXEL POSITION CALCULATIONS**
    readonly property real startX: startCol * cellSize
    readonly property real startY: startRow * cellSize
    readonly property real endX: endCol * cellSize
    readonly property real endY: endRow * cellSize

    // **CONTAINER DIMENSIONS**
    readonly property real containerWidth: Math.max(Math.abs(endX - startX) + (containerPadding * 2), minimumContainerSize)
    readonly property real containerHeight: Math.max(Math.abs(endY - startY) + (containerPadding * 2), minimumContainerSize)

    // ** ENHANCED: TRACK SEGMENT STATE COLOR LOGIC WITH INACTIVE SUPPORT**
    readonly property color currentTrackSegmentColor: {
        if (!isActive) return trackSegmentColorInactive;           // Highest priority: Inactive trackSegments
        if (isAssigned) return trackSegmentColorAssigned;          // High priority: Assignment
        if (isOverlap) return trackSegmentColorOverlap;
        if (isOccupied) return trackSegmentColorOccupied;         // Medium priority: Occupation
        return trackSegmentColorNormal;                           // Default: Normal state (trackSegment type specific)
    }

    // 
    // POSITIONING AND SIZING
    // 

    // Position container to encompass the trackSegment with padding
    x: Math.min(startX, endX) - containerPadding
    y: Math.min(startY, endY) - containerPadding
    width: containerWidth
    height: containerHeight

    // 
    // SIGNALS
    // 
    signal trackSegmentClicked(string segmentId, bool currentState)
    signal trackSegmentHovered(string segmentId)

    // 
    //  NEW: HELPER FUNCTIONS FOR TRACK SEGMENT SEGMENT TYPE COLORS
    // 
    function getTrackSegmentTypeColor() {
        switch(trackSegmentType.toUpperCase()) {
            case "STRAIGHT": return straightTrackSegmentColor
            case "CURVED": return curvedTrackSegmentColor
            case "SIDING": return sidingTrackSegmentColor
            case "PLATFORM": return platformTrackSegmentColor
            case "YARD": return yardTrackSegmentColor
            default: return straightTrackSegmentColor  // Safe default
        }
    }

    function getTrackSegmentTypeDisplayName() {
        switch(trackSegmentType.toUpperCase()) {
            case "STRAIGHT": return "Main Line"
            case "CURVED": return "Curved Track Segment"
            case "SIDING": return "Siding"
            case "PLATFORM": return "Platform"
            case "YARD": return "Yard Track Segment"
            default: return trackSegmentType
        }
    }

    function getTrackSegmentStatusText() {
        if (isAssigned && isOverlap) return "ASSIGNED+OVERLAP"  // Edge case
        if (isAssigned) return "ASSIGNED"
        if (isOverlap) return "OVERLAP"
        if (isOccupied) return "OCCUPIED" + (occupiedBy ? " by " + occupiedBy : "")
        return "NORMAL"
    }

    // 
    // VISUAL COMPONENTS
    // 

    // **MAIN TRACK SEGMENT RECTANGLE**
    Rectangle {
        id: trackSegmentBed

        // **POSITION TRACK SEGMENT WITHIN CONTAINER**
        x: startX - parent.x
        y: startY - parent.y
        width: Math.sqrt(Math.pow(endX - startX, 2) + Math.pow(endY - startY, 2))
        height: trackSegmentThickness

        transformOrigin: Item.Left
        rotation: Math.atan2(endY - startY, endX - startX) * 180 / Math.PI

        color: currentTrackSegmentColor
        radius: trackSegmentRadius

        // ** ENHANCED: INACTIVE TRACK SEGMENT VISUAL EFFECT**
        opacity: isActive ? 1.0 : 0.6

        // ** NEW: DASHED PATTERN FOR INACTIVE TRACK SEGMENTS**
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: isActive ? "transparent" : "#ffffff"
            border.width: isActive ? 0 : 1
            radius: parent.radius

            // Dashed border effect for inactive trackSegments
            visible: !isActive
            opacity: 0.5
        }

        // **RAIL LINES** - Visual trackSegment details
        Rectangle {
            width: parent.width
            height: railLineThickness
            anchors.top: parent.top
            anchors.topMargin: railLineMargin
            color: railLineColor
            opacity: isActive ? 1.0 : 0.5  //  NEW: Dimmed when inactive
        }

        Rectangle {
            width: parent.width
            height: railLineThickness
            anchors.bottom: parent.bottom
            anchors.bottomMargin: railLineMargin
            color: railLineColor
            opacity: isActive ? 1.0 : 0.5  //  NEW: Dimmed when inactive
        }

        // **HOVER EFFECT**
        Rectangle {
            anchors.fill: parent
            color: hoverColor
            opacity: hoverArea.containsMouse ? hoverOpacity : 0
            radius: parent.radius

            Behavior on opacity {
                NumberAnimation { duration: hoverAnimationDuration }
            }
        }
    }

    // ** NEW: OCCUPIED BY LABEL** - Shows what occupies the trackSegment
    Rectangle {
        id: occupiedByLabel
        anchors.centerIn: trackSegmentBed
        width: occupiedByText.contentWidth + 8
        height: occupiedByText.contentHeight + 4
        color: occupiedLabelBackground
        opacity: occupiedLabelOpacity
        radius: 2
        visible: isOccupied && occupiedBy !== ""

        Text {
            id: occupiedByText
            anchors.centerIn: parent
            text: occupiedBy
            color: occupiedLabelColor
            font.pixelSize: occupiedLabelFontSize
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // ** NEW: TRACK SEGMENT NAME LABEL** - Shows segment name for important trackSegments
    Text {
        id: trackSegmentNameLabel
        anchors.top: trackSegmentBed.bottom
        anchors.horizontalCenter: trackSegmentBed.horizontalCenter
        anchors.topMargin: 2
        text: segmentName
        color: "#cccccc"
        font.pixelSize: 6
        font.family: "Arial"
        visible: segmentName !== "" && trackSegmentType === "PLATFORM"  // Only show for platform trackSegments
        horizontalAlignment: Text.AlignHCenter
    }

    // **MOUSE INTERACTION AREA**
    MouseArea {
        id: hoverArea
        anchors.fill: parent
        hoverEnabled: true
        cursorShape: isActive ? clickCursor : Qt.ForbiddenCursor

        onClicked: {
            if (!isActive) {
                console.log("Track segment inactive:", segmentId, "- Click ignored")
                return
            }

            console.log("Track segment clicked:", segmentId,
                       "Name:", segmentName || "Unnamed",
                       "Type:", getTrackSegmentTypeDisplayName(),
                       "Coordinates:", "(" + startRow + "," + startCol + ") to (" + endRow + "," + endCol + ")",
                       "State:", getTrackSegmentStatusText(),
                       "Direction:", isHorizontal ? "H" : (isVertical ? "V" : (isTopLeftToBottomRight ? "TL→BR" : "BL→TR")),
                       "Active:", isActive)
            trackSegment.trackSegmentClicked(segmentId, isOccupied)
        }

        onEntered: {
            trackSegment.trackSegmentHovered(segmentId)
            console.log(" Track Segment Hover:", segmentId,
                       "Type:", getTrackSegmentTypeDisplayName(),
                       "Status:", isActive ? "Active" : "Inactive",
                       "State:", getTrackSegmentStatusText())  //  NEW: Enhanced hover info
        }
    }

    // ** ENHANCED: DEBUG TEXT WITH NEW FIELDS**
    Rectangle {
        id: debugBackground
        anchors.centerIn: parent
        width: debugText.contentWidth + debugTextPadding
        height: debugText.contentHeight + debugBackgroundPadding
        color: debugBackgroundColor
        opacity: debugTextOpacity
        radius: debugBorderRadius
        visible: debugText.visible

        Text {
            id: debugText
            anchors.centerIn: parent
            text: segmentId + (segmentName ? " (" + segmentName + ")" : "") +
                  "\n" + getTrackSegmentTypeDisplayName() +
                  "\n(" + startRow + "," + startCol + ")→(" + endRow + "," + endCol + ")" +
                  "\n" + (isActive ? "ACTIVE" : "INACTIVE") +
                  "\n" + getTrackSegmentStatusText()  //  NEW: Enhanced status text
            color: debugTextColor
            font.pixelSize: debugTextSize
            visible: false
            horizontalAlignment: Text.AlignHCenter
        }
    }

    // **CONTAINER BOUNDS DEBUG** - Shows clickable area
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        border.color: isDiagonal ? debugBorderColorDiagonal : debugBorderColorStraight
        border.width: debugBorderWidth
        opacity: debugBorderOpacity
        visible: false  // Set to true to see clickable bounds
    }

    // ** NEW: INACTIVE TRACK SEGMENT OVERLAY** - Visual indication for out-of-service trackSegments
    Rectangle {
        anchors.fill: trackSegmentBed
        color: "transparent"
        border.color: "#ff6600"
        border.width: 2
        radius: trackSegmentBed.radius
        visible: !isActive
        opacity: 0.7

        // "X" pattern for inactive trackSegments
        Rectangle {
            width: parent.width * 1.414  // √2 for diagonal
            height: 1
            color: "#ff6600"
            anchors.centerIn: parent
            rotation: 45
            opacity: 0.6
        }
        Rectangle {
            width: parent.width * 1.414
            height: 1
            color: "#ff6600"
            anchors.centerIn: parent
            rotation: -45
            opacity: 0.6
        }
    }
}
