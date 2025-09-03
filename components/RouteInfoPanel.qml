import QtQuick
import QtQuick.Controls
import RailFlux.Route

Rectangle {
    id: routeInfoPanel
    color: "#2d3748"
    border.color: "#4a5568"
    border.width: 1
    radius: 8
    
    // === PROPERTIES ===
    property var routeAssignmentService: globalRouteAssignmentService
    property var activeRoutes: []
    property var pendingRequests: []
    property bool isOperational: false
    property real averageProcessingTime: 0.0
    property string lastUpdateTime: "Never"
    
    // === HEADER ===
    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40
        color: "#1a1a1a"
        radius: 8
        
        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 12
            spacing: 12
            
            Text {
                text: "Route Management"
                color: "#ffffff"
                font.pixelSize: 16
                font.weight: Font.Bold
                anchors.verticalCenter: parent.verticalCenter
            }
            
            Rectangle {
                width: 12
                height: 12
                radius: 6
                color: isOperational ? "#38a169" : "#e53e3e"
                anchors.verticalCenter: parent.verticalCenter
                
                SequentialAnimation on opacity {
                    running: isOperational
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.5; duration: 1000 }
                    NumberAnimation { to: 1.0; duration: 1000 }
                }
            }
            
            Text {
                text: isOperational ? "Operational" : "Not Operational"
                color: isOperational ? "#38a169" : "#e53e3e"
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        
        Row {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 12
            spacing: 8
            
            Button {
                width: 24
                height: 24
                text: ""
                ToolTip.text: "Refresh route data"
                ToolTip.visible: hovered
                
                onClicked: refreshRoutes()
                
                background: Rectangle {
                    color: parent.hovered ? "#3182ce" : "transparent"
                    border.color: "#4a5568"
                    border.width: 1
                    radius: 4
                }
                
                contentItem: Text {
                    text: parent.text
                    color: "#ffffff"
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                    font.pixelSize: 10
                }
            }
            
            Button {
                width: 80
                height: 24
                text: "Emergency Stop"
                enabled: isOperational && activeRoutes.length > 0
                
                onClicked: emergencyStopDialog.open()
                
                background: Rectangle {
                    color: parent.pressed ? "#c53030" : parent.hovered ? "#e53e3e" : "#dc143c"
                    border.color: "#e53e3e"
                    border.width: 1
                    radius: 4
                }
                
                contentItem: Text {
                    text: parent.text
                    color: "#ffffff"
                    font.pixelSize: 10
                    font.weight: Font.Bold
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }
    
    // === SYSTEM STATUS ===
    Rectangle {
        id: statusBar
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.topMargin: 1
        height: 30
        color: "#1a1a1a"
        border.color: "#4a5568"
        border.width: 1
        
        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 12
            spacing: 20
            
            Text {
                text: "Active Routes: " + activeRoutes.length
                color: "#ffffff"
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
            
            Text {
                text: "Pending: " + pendingRequests.length
                color: pendingRequests.length > 0 ? "#d69e2e" : "#a0aec0"
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
            
            Text {
                text: "Avg Time: " + averageProcessingTime.toFixed(0) + "ms"
                color: averageProcessingTime > 1000 ? "#e53e3e" : "#a0aec0"
                font.pixelSize: 12
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        
        Text {
            anchors.right: parent.right
            anchors.verticalCenter: parent.verticalCenter
            anchors.rightMargin: 12
            text: "Last Update: " + lastUpdateTime
            color: "#a0aec0"
            font.pixelSize: 10
        }
    }
    
    // === ACTIVE ROUTES LIST ===
    ScrollView {
        id: routeScrollView
        anchors.top: statusBar.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8
        
        ListView {
            id: routeListView
            model: activeRoutes
            spacing: 4
            
            delegate: RouteInfoItem {
                width: routeListView.width
                route: modelData
                routeService: routeAssignmentService
                
                onCancelRequested: function(routeId) {
                    confirmCancelRoute(routeId)
                }
                
                onEmergencyReleaseRequested: function(routeId) {
                    confirmEmergencyRelease(routeId)
                }
            }
            
            // Empty state
            Rectangle {
                visible: activeRoutes.length === 0
                anchors.centerIn: parent
                width: parent.width * 0.8
                height: 100
                color: "transparent"
                
                Column {
                    anchors.centerIn: parent
                    spacing: 8
                    
                    Text {
                        text: ""
                        font.pixelSize: 24
                        color: "#a0aec0"
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                    
                    Text {
                        text: "No Active Routes"
                        font.pixelSize: 16
                        color: "#a0aec0"
                        font.weight: Font.Bold
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                    
                    Text {
                        text: isOperational ? "Routes will appear here when assigned" : "System not operational"
                        font.pixelSize: 12
                        color: "#718096"
                        anchors.horizontalCenter: parent.horizontalCenter
                    }
                }
            }
        }
    }
    
    // === EMERGENCY STOP CONFIRMATION DIALOG ===
    Dialog {
        id: emergencyStopDialog
        title: " Emergency Stop All Routes"
        modal: true
        anchors.centerIn: parent
        width: Math.min(400, routeInfoPanel.width * 0.9)
        
        background: Rectangle {
            color: "#2d3748"
            border.color: "#e53e3e"
            border.width: 2
            radius: 8
        }
        
        Column {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 16
            
            Text {
                width: parent.width
                text: "This will immediately release ALL active routes and stop all route processing."
                font.pixelSize: 14
                color: "#ffffff"
                wrapMode: Text.Wrap
            }
            
            Text {
                width: parent.width
                text: "Current active routes: " + activeRoutes.length
                font.pixelSize: 12
                color: "#e53e3e"
                font.weight: Font.Bold
            }
            
            Text {
                width: parent.width
                text: " This action cannot be undone and should only be used in emergency situations."
                font.pixelSize: 12
                color: "#d69e2e"
                wrapMode: Text.Wrap
                font.italic: true
            }
            
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12
                
                Button {
                    text: "Cancel"
                    onClicked: emergencyStopDialog.close()
                    
                    background: Rectangle {
                        color: parent.pressed ? "#4a5568" : "#2d3748"
                        border.color: "#4a5568"
                        border.width: 1
                        radius: 4
                    }
                    
                    contentItem: Text {
                        text: parent.text
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                
                Button {
                    text: " EMERGENCY STOP"
                    onClicked: {
                        performEmergencyStop()
                        emergencyStopDialog.close()
                    }
                    
                    background: Rectangle {
                        color: parent.pressed ? "#c53030" : "#e53e3e"
                        border.color: "#e53e3e"
                        border.width: 1
                        radius: 4
                    }
                    
                    contentItem: Text {
                        text: parent.text
                        color: "#ffffff"
                        font.weight: Font.Bold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }
    
    // === ROUTE CANCEL CONFIRMATION DIALOG ===
    Dialog {
        id: routeCancelDialog
        property string routeIdToCancel: ""
        
        title: "Cancel Route"
        modal: true
        anchors.centerIn: parent
        width: Math.min(300, routeInfoPanel.width * 0.8)
        
        background: Rectangle {
            color: "#2d3748"
            border.color: "#d69e2e"
            border.width: 1
            radius: 8
        }
        
        Column {
            anchors.fill: parent
            anchors.margins: 16
            spacing: 12
            
            Text {
                width: parent.width
                text: "Cancel route: " + routeCancelDialog.routeIdToCancel.substring(0, 12) + "..."
                font.pixelSize: 14
                color: "#ffffff"
                font.weight: Font.Bold
                wrapMode: Text.Wrap
            }
            
            Text {
                width: parent.width
                text: "This will release all resources and set signals to safe state."
                font.pixelSize: 12
                color: "#a0aec0"
                wrapMode: Text.Wrap
            }
            
            Row {
                anchors.horizontalCenter: parent.horizontalCenter
                spacing: 12
                
                Button {
                    text: "Keep Route"
                    onClicked: routeCancelDialog.close()
                    
                    background: Rectangle {
                        color: parent.pressed ? "#4a5568" : "#2d3748"
                        border.color: "#4a5568"
                        border.width: 1
                        radius: 4
                    }
                    
                    contentItem: Text {
                        text: parent.text
                        color: "#ffffff"
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
                
                Button {
                    text: "Cancel Route"
                    onClicked: {
                        cancelRoute(routeCancelDialog.routeIdToCancel)
                        routeCancelDialog.close()
                    }
                    
                    background: Rectangle {
                        color: parent.pressed ? "#b7791f" : "#d69e2e"
                        border.color: "#d69e2e"
                        border.width: 1
                        radius: 4
                    }
                    
                    contentItem: Text {
                        text: parent.text
                        color: "#ffffff"
                        font.weight: Font.Bold
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }
        }
    }
    
    // === FUNCTIONS ===
    function refreshRoutes() {
        if (!routeAssignmentService) return
        
        console.log(" RouteInfoPanel: Refreshing route data")
        
        try {
            activeRoutes = routeAssignmentService.getActiveRoutes()
            pendingRequests = routeAssignmentService.getPendingRequests()
            isOperational = routeAssignmentService.isOperational
            averageProcessingTime = routeAssignmentService.averageProcessingTimeMs
            lastUpdateTime = Qt.formatDateTime(new Date(), "hh:mm:ss")
            
            console.log(" Route data refreshed - Active:", activeRoutes.length, "Pending:", pendingRequests.length)
        } catch (error) {
            console.error(" Failed to refresh route data:", error)
            lastUpdateTime = "Error"
        }
    }
    
    function confirmCancelRoute(routeId) {
        routeCancelDialog.routeIdToCancel = routeId
        routeCancelDialog.open()
    }
    
    function confirmEmergencyRelease(routeId) {
        if (!routeAssignmentService) return
        
        console.log(" Emergency release requested for route:", routeId)
        routeAssignmentService.emergencyReleaseRoute(routeId, "Operator emergency release")
        refreshRoutes()
    }
    
    function cancelRoute(routeId) {
        if (!routeAssignmentService) return
        
        console.log(" Cancelling route:", routeId)
        var success = routeAssignmentService.cancelRoute(routeId, "Operator cancelled")
        
        if (success) {
            console.log(" Route cancelled successfully")
        } else {
            console.error(" Failed to cancel route")
        }
        
        refreshRoutes()
    }
    
    function performEmergencyStop() {
        if (!routeAssignmentService) return
        
        console.log(" EMERGENCY STOP: Releasing all routes")
        var success = routeAssignmentService.emergencyReleaseAllRoutes("Operator emergency stop - all routes")
        
        if (success) {
            console.log(" Emergency stop completed")
        } else {
            console.error(" Emergency stop failed")
        }
        
        refreshRoutes()
    }
    
    // === COMPONENT LIFECYCLE ===
    Component.onCompleted: {
        console.log(" RouteInfoPanel initialized")
        
        if (routeAssignmentService) {
            // Connect to service signals
            routeAssignmentService.routeAssigned.connect(refreshRoutes)
            routeAssignmentService.routeActivated.connect(refreshRoutes)
            routeAssignmentService.routeReleased.connect(refreshRoutes)
            routeAssignmentService.routeFailed.connect(refreshRoutes)
            routeAssignmentService.operationalStateChanged.connect(refreshRoutes)
            
            // Initial refresh
            refreshRoutes()
        }
    }
    
    // === AUTO-REFRESH TIMER ===
    Timer {
        interval: 2000  // Refresh every 2 seconds
        running: true
        repeat: true
        onTriggered: refreshRoutes()
    }
}
