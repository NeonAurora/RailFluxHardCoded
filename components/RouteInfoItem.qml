import QtQuick
import QtQuick.Controls

Rectangle {
    id: routeInfoItem
    height: 80
    color: getItemColor()
    border.color: getBorderColor()
    border.width: 1
    radius: 6
    
    // === PROPERTIES ===
    property var route
    property var routeService
    
    // === SIGNALS ===
    signal cancelRequested(string routeId)
    signal emergencyReleaseRequested(string routeId)
    
    function getItemColor() {
        if (!route) return "#1a1a1a"
        
        switch(route.state) {
            case "ACTIVE": return "#2d4a22"        // Dark green
            case "RESERVED": return "#2d3a22"      // Dark lime
            case "ASSIGNED": return "#3d3a1f"      // Dark gold
            case "FAILED": return "#3a1f1f"        // Dark red
            case "PARTIALLY_RELEASED": return "#1f2f3a" // Dark blue
            default: return "#1a1a1a"              // Dark gray
        }
    }
    
    function getBorderColor() {
        if (!route) return "#4a5568"
        
        switch(route.state) {
            case "ACTIVE": return "#38a169"        // Green
            case "RESERVED": return "#32cd32"      // Lime
            case "ASSIGNED": return "#ffd700"      // Gold  
            case "FAILED": return "#dc143c"        // Red
            case "PARTIALLY_RELEASED": return "#87ceeb" // Sky blue
            default: return "#4a5568"              // Gray
        }
    }
    
    function formatTime(timeStr) {
        if (!timeStr) return "N/A"
        
        try {
            var date = new Date(timeStr)
            return Qt.formatDateTime(date, "hh:mm:ss")
        } catch(e) {
            return "Invalid"
        }
    }
    
    function getElapsedTime() {
        if (!route || !route.createdAt) return "N/A"
        
        try {
            var created = new Date(route.createdAt)
            var now = new Date()
            var elapsed = Math.floor((now - created) / 1000) // seconds
            
            if (elapsed < 60) return elapsed + "s"
            if (elapsed < 3600) return Math.floor(elapsed / 60) + "m"
            return Math.floor(elapsed / 3600) + "h"
        } catch(e) {
            return "N/A"
        }
    }

    function getCircuitCount(circuits) {
        if (!circuits) return 0
        if (typeof circuits === 'string') {
            return circuits.split(',').filter(c => c.trim() !== '').length
        }
        if (Array.isArray(circuits)) {
            return circuits.length
        }
        return 0
    }
    
    // === MAIN CONTENT ===
    Row {
        anchors.fill: parent
        anchors.margins: 8
        spacing: 12
        
        // === LEFT COLUMN - ROUTE INFO ===
        Column {
            width: parent.width * 0.6
            anchors.verticalCenter: parent.verticalCenter
            spacing: 4
            
            // Route ID and State
            Row {
                width: parent.width
                spacing: 8
                
                Text {
                    text: route ? (route.id ? route.id.substring(0, 12) + "..." : "Unknown") : "No Route"
                    color: "#ffffff"
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    width: parent.width * 0.7
                    elide: Text.ElideRight
                }
                
                Rectangle {
                    width: stateText.width + 8
                    height: 16
                    color: getBorderColor()
                    radius: 8
                    
                    Text {
                        id: stateText
                        anchors.centerIn: parent
                        text: route ? route.state : "UNKNOWN"
                        color: "#ffffff"
                        font.pixelSize: 9
                        font.weight: Font.Bold
                    }
                }
            }
            
            // Signal route
            Row {
                width: parent.width
                spacing: 4
                
                Text {
                    text: ""
                    color: "#38a169"
                    font.pixelSize: 10
                }
                
                Text {
                    text: route ? (route.sourceSignalId + " → " + route.destSignalId) : "Unknown Route"
                    color: "#a0aec0"
                    font.pixelSize: 10
                    width: parent.width - 20
                    elide: Text.ElideRight
                }
            }
            
            // Timing information
            Row {
                width: parent.width
                spacing: 12
                
                Text {
                    text: " " + getElapsedTime()
                    color: "#a0aec0"
                    font.pixelSize: 9
                }
                
                Text {
                    text: " " + (route ? getCircuitCount(route.assignedCircuits) + " circuits" : "0 circuits")
                    color: "#a0aec0"
                    font.pixelSize: 9
                }
                
                Text {
                    text: " " + (route ? route.operatorId : "Unknown")
                    color: "#a0aec0"
                    font.pixelSize: 9
                }
            }
        }
        
        // === RIGHT COLUMN - ACTIONS ===
        Column {
            width: parent.width * 0.35
            anchors.verticalCenter: parent.verticalCenter
            spacing: 6
            
            // Main action button
            Button {
                width: parent.width
                height: 24
                enabled: route && route.state !== "FAILED" && route.state !== "RELEASED"
                
                text: {
                    if (!route) return "N/A"
                    switch(route.state) {
                        case "RESERVED": return "Activate"
                        case "ACTIVE": return "Release"
                        case "ASSIGNED": return "Cancel"
                        default: return "Cancel"
                    }
                }
                
                onClicked: {
                    if (!route) return
                    
                    switch(route.state) {
                        case "RESERVED":
                            activateRoute()
                            break
                        case "ACTIVE":
                            releaseRoute()
                            break
                        case "ASSIGNED":
                        default:
                            cancelRequested(route.id)
                            break
                    }
                }
                
                background: Rectangle {
                    color: {
                        if (!parent.enabled) return "#2d3748"
                        if (parent.pressed) return "#2c5aa0"
                        if (parent.hovered) return "#3182ce"
                        return "#2b6cb0"
                    }
                    border.color: "#3182ce"
                    border.width: 1
                    radius: 4
                }
                
                contentItem: Text {
                    text: parent.text
                    color: parent.enabled ? "#ffffff" : "#a0aec0"
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
            
            // Emergency release button
            Button {
                width: parent.width
                height: 20
                enabled: route && (route.state === "ACTIVE" || route.state === "RESERVED")
                text: "Emergency"
                
                onClicked: {
                    if (route) {
                        emergencyReleaseRequested(route.id)
                    }
                }
                
                background: Rectangle {
                    color: {
                        if (!parent.enabled) return "#2d3748"
                        if (parent.pressed) return "#c53030"
                        if (parent.hovered) return "#e53e3e"
                        return "#dc143c"
                    }
                    border.color: "#e53e3e"
                    border.width: 1
                    radius: 4
                }
                
                contentItem: Text {
                    text: parent.text
                    color: parent.enabled ? "#ffffff" : "#a0aec0"
                    font.pixelSize: 9
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
    
    // === ACTION FUNCTIONS ===
    function activateRoute() {
        if (!routeService || !route) return
        
        console.log(" Activating route:", route.id)
        var success = routeService.activateRoute(route.id)
        
        if (!success) {
            console.error(" Failed to activate route:", route.id)
        }
    }
    
    function releaseRoute() {
        if (!routeService || !route) return
        
        console.log(" Releasing route:", route.id)
        var success = routeService.releaseRoute(route.id, "Operator release")
        
        if (!success) {
            console.error(" Failed to release route:", route.id)
        }
    }
    
    // === MOUSE INTERACTION ===
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        
        onEntered: {
            parent.color = Qt.lighter(getItemColor(), 1.1)
        }
        
        onExited: {
            parent.color = getItemColor()
        }
        
        ToolTip.visible: containsMouse
        ToolTip.text: route ? (
            "Route: " + route.id + "\n" +
            "State: " + route.state + "\n" +
            "Source: " + route.sourceSignalId + "\n" +
            "Destination: " + route.destSignalId + "\n" +
            "Created: " + formatTime(route.createdAt) + "\n" +
            "Operator: " + route.operatorId + "\n" +
            "Priority: " + route.priority
        ) : "No route data"
    }
    
    // === COMPONENT LIFECYCLE ===
    Component.onCompleted: {
        if (route) {
            console.log(" RouteInfoItem created for route:", route.id, "state:", route.state)
        }
    }
}
