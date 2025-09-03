import QtQuick
import QtQuick.Shapes

Item {
    id: routeOverlay
    anchors.fill: parent
    
    // === ROUTE PROPERTIES ===
    property string routeId
    property string sourceSignalId
    property string destSignalId
    property var assignedCircuits: []
    property var overlapCircuits: []
    property string routeState: "RESERVED"
    property int cellSize: 20
    property bool showRouteName: true
    property real lineWidth: 3.0
    
    // === VISUAL PROPERTIES ===
    property color routeColor: getRouteColor()
    property real opacity: getRouteOpacity()
    
    function getRouteColor() {
        switch(routeState) {
            case "RESERVED": return "#32CD32"    // LimeGreen
            case "ACTIVE": return "#FF4500"      // OrangeRed
            case "ASSIGNED": return "#FFD700"    // Gold
            case "FAILED": return "#DC143C"      // Crimson
            case "PARTIALLY_RELEASED": return "#87CEEB" // SkyBlue
            default: return "#FFD700"            // Gold default
        }
    }
    
    function getRouteOpacity() {
        switch(routeState) {
            case "ACTIVE": return 0.9
            case "RESERVED": return 0.8
            case "ASSIGNED": return 0.7
            case "FAILED": return 0.6
            default: return 0.7
        }
    }
    
    function getSignalPosition(signalId) {
        if (!globalDatabaseManager) return { x: 0, y: 0 }
        
        var signalData = globalDatabaseManager.getSignalById(signalId)
        if (!signalData) return { x: 0, y: 0 }
        
        return {
            x: (signalData.col || 0) * cellSize + cellSize / 2,
            y: (signalData.row || 0) * cellSize + cellSize / 2
        }
    }
    
    function getCircuitCenterPosition(circuitId) {
        if (!globalDatabaseManager) return { x: 0, y: 0 }
        
        var circuitData = globalDatabaseManager.getTrackCircuitById(circuitId)
        if (!circuitData) return { x: 0, y: 0 }
        
        return {
            x: (circuitData.col || 0) * cellSize + cellSize / 2,
            y: (circuitData.row || 0) * cellSize + cellSize / 2
        }
    }
    
    // === ROUTE PATH VISUALIZATION ===
    Shape {
        id: routePath
        anchors.fill: parent
        
        ShapePath {
            strokeWidth: lineWidth
            strokeColor: routeColor
            fillColor: "transparent"
            strokeStyle: routeState === "FAILED" ? ShapePath.DashLine : ShapePath.SolidLine
            
            PathMove {
                property var sourcePos: getSignalPosition(sourceSignalId)
                x: sourcePos.x
                y: sourcePos.y
            }
            
            // Draw path through assigned circuits
            Repeater {
                model: assignedCircuits
                
                PathLine {
                    property var circuitPos: getCircuitCenterPosition(modelData)
                    x: circuitPos.x
                    y: circuitPos.y
                }
            }
            
            PathLine {
                property var destPos: getSignalPosition(destSignalId)
                x: destPos.x
                y: destPos.y
            }
        }
        
        // === OVERLAP VISUALIZATION ===
        ShapePath {
            visible: overlapCircuits.length > 0
            strokeWidth: lineWidth * 0.7
            strokeColor: "#87CEEB"  // SkyBlue for overlap
            fillColor: "transparent"
            strokeStyle: ShapePath.DashLine
            dashPattern: [4, 4]
            
            PathMove {
                property var destPos: getSignalPosition(destSignalId)
                x: destPos.x
                y: destPos.y
            }
            
            // Draw overlap path
            Repeater {
                model: overlapCircuits
                
                PathLine {
                    property var circuitPos: getCircuitCenterPosition(modelData)
                    x: circuitPos.x
                    y: circuitPos.y
                }
            }
        }
    }
    
    // === ROUTE SIGNAL DOTS ===
    RouteSignalDot {
        id: sourceSignalDot
        property var pos: getSignalPosition(sourceSignalId)
        x: pos.x - width / 2
        y: pos.y - height / 2
        signalId: sourceSignalId
        signalRole: "SOURCE"
        routeColor: routeOverlay.routeColor
        isActive: routeState === "ACTIVE"
    }
    
    RouteSignalDot {
        id: destSignalDot
        property var pos: getSignalPosition(destSignalId)
        x: pos.x - width / 2
        y: pos.y - height / 2
        signalId: destSignalId
        signalRole: "DESTINATION"
        routeColor: routeOverlay.routeColor
        isActive: routeState === "ACTIVE"
    }
    
    // === ROUTE NAME DISPLAY ===
    Rectangle {
        visible: showRouteName && routeId
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.margins: 8
        width: routeNameText.width + 16
        height: routeNameText.height + 8
        color: "#2d3748"
        border.color: routeColor
        border.width: 1
        radius: 4
        opacity: 0.9
        
        Text {
            id: routeNameText
            anchors.centerIn: parent
            text: routeId ? routeId.substring(0, 8) + "..." : ""
            color: "#ffffff"
            font.pixelSize: 10
            font.weight: Font.Bold
        }
        
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            
            ToolTip.visible: containsMouse
            ToolTip.text: "Route: " + routeId + "\n" +
                         "State: " + routeState + "\n" +
                         "Source: " + sourceSignalId + "\n" +
                         "Destination: " + destSignalId + "\n" +
                         "Circuits: " + assignedCircuits.length
        }
    }
    
    // === ROUTE STATE INDICATOR ===
    Rectangle {
        visible: routeState === "ACTIVE"
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 8
        width: 80
        height: 20
        color: routeColor
        radius: 10
        
        Text {
            anchors.centerIn: parent
            text: "ACTIVE"
            color: "#ffffff"
            font.pixelSize: 9
            font.weight: Font.Bold
        }
        
        // Pulsing animation for active routes
        SequentialAnimation on opacity {
            running: routeState === "ACTIVE"
            loops: Animation.Infinite
            NumberAnimation { to: 0.5; duration: 1000 }
            NumberAnimation { to: 1.0; duration: 1000 }
        }
    }
    
    // === COMPONENT LIFECYCLE ===
    Component.onCompleted: {
        console.log(" RouteOverlay created for route:", routeId, "state:", routeState)
    }
    
    // === PROPERTY CHANGES ===
    onRouteStateChanged: {
        console.log(" Route", routeId, "state changed to:", routeState)
        routeColor = getRouteColor()
        opacity = getRouteOpacity()
    }
}
