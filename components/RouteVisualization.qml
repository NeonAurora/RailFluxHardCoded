import QtQuick
import QtQuick.Shapes
import RailFlux.Route

Rectangle {
    id: routeVisualization
    color: "transparent"
    
    // === PROPERTIES ===
    property var stationLayout: null
    property var routeAssignmentService: globalRouteAssignmentService
    property bool isEnabled: true
    property bool showRouteNames: true
    property real routeLineWidth: 3.0
    
    // === COLOR SCHEME ===
    readonly property color routeColorAssigned: "#FFD700"    // Gold
    readonly property color routeColorActive: "#FF4500"      // OrangeRed  
    readonly property color routeColorOverlap: "#87CEEB"     // SkyBlue
    readonly property color routeColorConflict: "#FF6347"    // Tomato
    readonly property color routeColorReserved: "#32CD32"    // LimeGreen
    readonly property color routeColorFailed: "#DC143C"      // Crimson
    
    // === INTERNAL STATE ===
    property var activeRoutes: []
    property var routeOverlays: ({})  // routeId -> overlay component
    property int cellSize: stationLayout ? stationLayout.cellSize : 20
    
    // === ROUTE DATA MANAGEMENT ===
    function refreshRoutes() {
        if (!routeAssignmentService || !isEnabled) return
        
        console.log(" RouteVisualization: Refreshing active routes")
        
        // Get active routes from service
        activeRoutes = routeAssignmentService.getActiveRoutes()
        
        // Update visualization
        updateRouteOverlays()
    }
    
    function updateRouteOverlays() {
        console.log(" RouteVisualization: Updating route overlays for", activeRoutes.length, "routes")
        
        // Clear existing overlays
        clearAllOverlays()
        
        // Create overlays for active routes
        for (var i = 0; i < activeRoutes.length; i++) {
            var route = activeRoutes[i]
            createRouteOverlay(route)
        }
    }
    
    function clearAllOverlays() {
        // Destroy existing route overlays
        for (var routeId in routeOverlays) {
            if (routeOverlays[routeId]) {
                routeOverlays[routeId].destroy()
            }
        }
        routeOverlays = {}
    }
    
    function createRouteOverlay(route) {
        if (!route || !route.id) return
        
        console.log(" Creating route overlay for:", route.id, "state:", route.state)
        
        // Parse assigned circuits from database format
        var assignedCircuits = parseCircuitArray(route.assignedCircuits)
        var overlapCircuits = parseCircuitArray(route.overlapCircuits)
        
        if (assignedCircuits.length === 0) {
            console.warn(" Route", route.id, "has no assigned circuits")
            return
        }
        
        // Create route overlay component
        var overlayComponent = Qt.createComponent("RouteOverlay.qml")
        if (overlayComponent.status === Component.Ready) {
            var overlay = overlayComponent.createObject(routeVisualization, {
                "routeId": route.id,
                "sourceSignalId": route.sourceSignalId,
                "destSignalId": route.destSignalId,
                "assignedCircuits": assignedCircuits,
                "overlapCircuits": overlapCircuits,
                "routeState": route.state,
                "cellSize": cellSize,
                "showRouteName": showRouteNames,
                "lineWidth": routeLineWidth
            })
            
            if (overlay) {
                routeOverlays[route.id] = overlay
                console.log(" Route overlay created for:", route.id)
            }
        } else {
            console.error(" Failed to create RouteOverlay component:", overlayComponent.errorString())
        }
    }
    
    function parseCircuitArray(circuitStr) {
        if (!circuitStr || circuitStr === "{}") return []
        
        // Remove braces and split by comma
        var cleaned = circuitStr.replace(/[{}]/g, "").trim()
        if (cleaned === "") return []
        
        return cleaned.split(",").map(function(circuit) {
            return circuit.trim()
        }).filter(function(circuit) {
            return circuit !== ""
        })
    }
    
    function getSignalPosition(signalId) {
        if (!globalDatabaseManager) return { x: 0, y: 0 }
        
        var signalData = globalDatabaseManager.getSignalById(signalId)
        if (!signalData) return { x: 0, y: 0 }
        
        return {
            x: (signalData.col || 0) * cellSize,
            y: (signalData.row || 0) * cellSize
        }
    }
    
    function getCircuitPosition(circuitId) {
        if (!globalDatabaseManager) return { x: 0, y: 0 }
        
        var circuitData = globalDatabaseManager.getTrackCircuitById(circuitId)
        if (!circuitData) return { x: 0, y: 0 }
        
        return {
            x: (circuitData.col || 0) * cellSize,
            y: (circuitData.row || 0) * cellSize
        }
    }
    
    function handleRouteRequested(requestId, sourceSignal, destSignal) {
        console.log(" Route requested:", requestId, sourceSignal, "→", destSignal)
        // Route requests don't get visualized until they become active
    }
    
    function handleRouteAssigned(routeId, sourceSignal, destSignal, path) {
        console.log(" Route assigned:", routeId, "path:", path)
        refreshRoutes()
    }
    
    function handleRouteActivated(routeId) {
        console.log(" Route activated:", routeId)
        
        // Update the specific route overlay state
        if (routeOverlays[routeId]) {
            routeOverlays[routeId].routeState = "ACTIVE"
        }
        refreshRoutes()
    }
    
    function handleRouteReleased(routeId, reason) {
        console.log(" Route released:", routeId, "reason:", reason)
        
        // Remove the route overlay
        if (routeOverlays[routeId]) {
            routeOverlays[routeId].destroy()
            delete routeOverlays[routeId]
        }
        refreshRoutes()
    }
    
    function handleRouteFailed(routeId, reason) {
        console.log(" Route failed:", routeId, "reason:", reason)
        
        // Update overlay to show failed state
        if (routeOverlays[routeId]) {
            routeOverlays[routeId].routeState = "FAILED"
        }
    }
    
    // === COMPONENT LIFECYCLE ===
    Component.onCompleted: {
        console.log(" RouteVisualization component initialized")
        
        // Connect to route assignment service signals
        if (routeAssignmentService) {
            routeAssignmentService.routeRequested.connect(handleRouteRequested)
            routeAssignmentService.routeAssigned.connect(handleRouteAssigned)
            routeAssignmentService.routeActivated.connect(handleRouteActivated)
            routeAssignmentService.routeReleased.connect(handleRouteReleased)
            routeAssignmentService.routeFailed.connect(handleRouteFailed)
            
            console.log(" Connected to RouteAssignmentService signals")
            
            // Initial route refresh
            refreshRoutes()
        } else {
            console.warn(" RouteAssignmentService not available")
        }
    }
    
    Component.onDestruction: {
        clearAllOverlays()
        console.log(" RouteVisualization component destroyed")
    }
    
    // === VISIBILITY CONTROL ===
    visible: isEnabled && routeAssignmentService
    
    // === REFRESH TIMER ===
    Timer {
        id: refreshTimer
        interval: 5000  // Refresh every 5 seconds
        running: isEnabled && visible
        repeat: true
        onTriggered: refreshRoutes()
    }
    
    // === STATUS INDICATOR ===
    Rectangle {
        visible: isEnabled && activeRoutes.length > 0
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 10
        width: 120
        height: 30
        color: "#2d3748"
        border.color: "#4a5568"
        border.width: 1
        radius: 4
        
        Text {
            anchors.centerIn: parent
            text: "Routes: " + activeRoutes.length
            color: "#ffffff"
            font.pixelSize: 12
            font.weight: Font.Bold
        }
    }
}
