import QtQuick
import QtQuick.Controls
import RailFlux.Route

Rectangle {
    id: performanceDashboard
    color: "#1a1a1a"
    border.color: "#4a5568"
    border.width: 1
    radius: 8
    
    // === PROPERTIES ===
    property var routeAssignmentService: globalRouteAssignmentService
    property var telemetryService: globalTelemetryService
    property var safetyMonitorService: globalSafetyMonitorService
    
    property var performanceStats: ({})
    property var operationalStats: ({})
    property var systemMetrics: ({})
    property string lastUpdateTime: "Never"
    
    // === PERFORMANCE TARGETS ===
    readonly property real targetResponseTime: 1000.0      // 1 second
    readonly property real targetSuccessRate: 95.0         // 95%
    readonly property int targetActiveRoutes: 10           // Max concurrent
    readonly property real warningResponseTime: 2000.0     // 2 seconds
    
    // === HEADER ===
    Rectangle {
        id: header
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40
        color: "#2d3748"
        radius: 8
        
        Row {
            anchors.left: parent.left
            anchors.verticalCenter: parent.verticalCenter
            anchors.leftMargin: 12
            spacing: 12
            
            Text {
                text: "Performance Dashboard"
                color: "#ffffff"
                font.pixelSize: 16
                font.weight: Font.Bold
                anchors.verticalCenter: parent.verticalCenter
            }
            
            Rectangle {
                width: 12
                height: 12
                radius: 6
                color: getSystemHealthColor()
                anchors.verticalCenter: parent.verticalCenter
                
                SequentialAnimation on opacity {
                    running: getSystemHealthColor() === "#38a169"
                    loops: Animation.Infinite
                    NumberAnimation { to: 0.5; duration: 1500 }
                    NumberAnimation { to: 1.0; duration: 1500 }
                }
            }
            
            Text {
                text: getSystemHealthText()
                color: getSystemHealthColor()
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
                ToolTip.text: "Refresh metrics"
                ToolTip.visible: hovered
                
                onClicked: refreshMetrics()
                
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
            
            Text {
                text: "Updated: " + lastUpdateTime
                color: "#a0aec0"
                font.pixelSize: 10
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }
    
    // === CONTENT AREA ===
    ScrollView {
        anchors.top: header.bottom
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: 8
        
        Column {
            width: performanceDashboard.width - 16
            spacing: 12
            
            // === KEY METRICS CARDS ===
            Rectangle {
                width: parent.width
                height: 120
                color: "#2d3748"
                border.color: "#4a5568"
                border.width: 1
                radius: 6
                
                Column {
                    anchors.fill: parent
                    anchors.margins: 12
                    
                    Text {
                        text: "Key Performance Indicators"
                        color: "#ffffff"
                        font.pixelSize: 14
                        font.weight: Font.Bold
                    }
                    
                    Grid {
                        width: parent.width
                        columns: 4
                        rowSpacing: 8
                        columnSpacing: 12
                        
                        // Route Success Rate
                        MetricCard {
                            title: "Success Rate"
                            value: getSuccessRate().toFixed(1) + "%"
                            target: targetSuccessRate + "%"
                            status: getSuccessRate() >= targetSuccessRate ? "good" : "warning"
                            icon: ""
                        }
                        
                        // Average Setup Time
                        MetricCard {
                            title: "Avg Setup Time"
                            value: getAverageSetupTime().toFixed(0) + "ms"
                            target: targetResponseTime.toFixed(0) + "ms"
                            status: getAverageSetupTime() <= targetResponseTime ? "good" : "warning"
                            icon: ""
                        }
                        
                        // Active Routes
                        MetricCard {
                            title: "Active Routes"
                            value: getActiveRouteCount().toString()
                            target: "≤" + targetActiveRoutes
                            status: getActiveRouteCount() <= targetActiveRoutes ? "good" : "warning"
                            icon: ""
                        }
                        
                        // Emergency Releases (24h)
                        MetricCard {
                            title: "Emergency Releases"
                            value: getEmergencyReleases().toString()
                            target: "0"
                            status: getEmergencyReleases() === 0 ? "good" : "danger"
                            icon: ""
                        }
                    }
                }
            }
            
            // === DETAILED METRICS TABLE ===
            Rectangle {
                width: parent.width
                height: 200
                color: "#2d3748"
                border.color: "#4a5568"
                border.width: 1
                radius: 6
                
                Column {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8
                    
                    Text {
                        text: "Detailed Metrics"
                        color: "#ffffff"
                        font.pixelSize: 14
                        font.weight: Font.Bold
                    }
                    
                    Rectangle {
                        width: parent.width
                        height: 1
                        color: "#4a5568"
                    }
                    
                    // Table Header
                    Row {
                        width: parent.width
                        spacing: 0
                        
                        Text {
                            width: parent.width * 0.4
                            text: "Metric"
                            color: "#a0aec0"
                            font.pixelSize: 11
                            font.weight: Font.Bold
                        }
                        
                        Text {
                            width: parent.width * 0.2
                            text: "Current"
                            color: "#a0aec0"
                            font.pixelSize: 11
                            font.weight: Font.Bold
                        }
                        
                        Text {
                            width: parent.width * 0.2
                            text: "Average"
                            color: "#a0aec0"
                            font.pixelSize: 11
                            font.weight: Font.Bold
                        }
                        
                        Text {
                            width: parent.width * 0.2
                            text: "Target"
                            color: "#a0aec0"
                            font.pixelSize: 11
                            font.weight: Font.Bold
                        }
                    }
                    
                    Rectangle {
                        width: parent.width
                        height: 1
                        color: "#4a5568"
                    }
                    
                    // Metrics Rows
                    Column {
                        width: parent.width
                        spacing: 4
                        
                        MetricRow {
                            width: parent.width
                            metricName: "Processing Time (ms)"
                            currentValue: getCurrentProcessingTime().toFixed(0)
                            averageValue: getAverageSetupTime().toFixed(0)
                            targetValue: targetResponseTime.toFixed(0)
                            isGood: getCurrentProcessingTime() <= targetResponseTime
                        }
                        
                        MetricRow {
                            width: parent.width
                            metricName: "Queue Size"
                            currentValue: getPendingRequests().toString()
                            averageValue: getAverageQueueSize().toFixed(1)
                            targetValue: "< 5"
                            isGood: getPendingRequests() < 5
                        }
                        
                        MetricRow {
                            width: parent.width
                            metricName: "Success Rate (%)"
                            currentValue: getSuccessRate().toFixed(1)
                            averageValue: getSuccessRate().toFixed(1)
                            targetValue: targetSuccessRate.toFixed(1)
                            isGood: getSuccessRate() >= targetSuccessRate
                        }
                        
                        MetricRow {
                            width: parent.width
                            metricName: "Route Conflicts"
                            currentValue: getRouteConflicts().toString()
                            averageValue: getAverageConflicts().toFixed(1)
                            targetValue: "0"
                            isGood: getRouteConflicts() === 0
                        }
                        
                        MetricRow {
                            width: parent.width
                            metricName: "System Load (%)"
                            currentValue: getSystemLoad().toFixed(1)
                            averageValue: getAverageLoad().toFixed(1)
                            targetValue: "< 80"
                            isGood: getSystemLoad() < 80
                        }
                    }
                }
            }
            
            // === ALERT HISTORY ===
            Rectangle {
                width: parent.width
                height: 180
                color: "#2d3748"
                border.color: "#4a5568"
                border.width: 1
                radius: 6
                
                Column {
                    anchors.fill: parent
                    anchors.margins: 12
                    spacing: 8
                    
                    Text {
                        text: "Recent Alerts (Last 24 Hours)"
                        color: "#ffffff"
                        font.pixelSize: 14
                        font.weight: Font.Bold
                    }
                    
                    Rectangle {
                        width: parent.width
                        height: 1
                        color: "#4a5568"
                    }
                    
                    ListView {
                        width: parent.width
                        height: parent.height - 40
                        model: getRecentAlerts()
                        
                        delegate: Rectangle {
                            width: parent.width
                            height: 24
                            color: "transparent"
                            
                            Row {
                                anchors.fill: parent
                                spacing: 8
                                
                                Text {
                                    text: getAlertIcon(modelData.severity)
                                    color: getAlertColor(modelData.severity)
                                    font.pixelSize: 12
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                
                                Text {
                                    text: modelData.time
                                    color: "#a0aec0"
                                    font.pixelSize: 10
                                    width: 60
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                
                                Text {
                                    text: modelData.message
                                    color: "#ffffff"
                                    font.pixelSize: 10
                                    width: parent.width - 100
                                    elide: Text.ElideRight
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }
                        }
                    }
                }
            }
        }
    }
    
    // === METRIC CARD COMPONENT ===
    component MetricCard: Rectangle {
        property string title
        property string value
        property string target
        property string status: "good"  // good, warning, danger
        property string icon: ""
        
        width: (parent.width - 36) / 4
        height: 60
        color: "#1a1a1a"
        border.color: status === "good" ? "#38a169" : status === "warning" ? "#d69e2e" : "#e53e3e"
        border.width: 1
        radius: 4
        
        Column {
            anchors.centerIn: parent
            spacing: 2
            
            Text {
                text: icon + " " + title
                color: "#a0aec0"
                font.pixelSize: 9
                anchors.horizontalCenter: parent.horizontalCenter
            }
            
            Text {
                text: value
                color: "#ffffff"
                font.pixelSize: 14
                font.weight: Font.Bold
                anchors.horizontalCenter: parent.horizontalCenter
            }
            
            Text {
                text: "Target: " + target
                color: "#718096"
                font.pixelSize: 8
                anchors.horizontalCenter: parent.horizontalCenter
            }
        }
    }
    
    // === METRIC ROW COMPONENT ===
    component MetricRow: Row {
        property string metricName
        property string currentValue
        property string averageValue
        property string targetValue
        property bool isGood: true
        
        spacing: 0
        
        Text {
            width: parent.width * 0.4
            text: metricName
            color: "#ffffff"
            font.pixelSize: 10
        }
        
        Text {
            width: parent.width * 0.2
            text: currentValue
            color: isGood ? "#38a169" : "#e53e3e"
            font.pixelSize: 10
            font.weight: Font.Bold
        }
        
        Text {
            width: parent.width * 0.2
            text: averageValue
            color: "#a0aec0"
            font.pixelSize: 10
        }
        
        Text {
            width: parent.width * 0.2
            text: targetValue
            color: "#718096"
            font.pixelSize: 10
        }
    }
    
    // === HELPER FUNCTIONS ===
    function getSystemHealthColor() {
        var avgTime = getAverageSetupTime()
        var successRate = getSuccessRate()
        var emergencies = getEmergencyReleases()
        
        if (emergencies > 0 || successRate < 90) return "#e53e3e"  // Red
        if (avgTime > warningResponseTime || successRate < 95) return "#d69e2e"  // Yellow
        return "#38a169"  // Green
    }
    
    function getSystemHealthText() {
        var color = getSystemHealthColor()
        if (color === "#e53e3e") return "Critical"
        if (color === "#d69e2e") return "Warning"
        return "Healthy"
    }
    
    function getSuccessRate() {
        if (!operationalStats.totalRequests) return 100.0
        return (operationalStats.successfulRoutes / operationalStats.totalRequests) * 100
    }
    
    function getAverageSetupTime() {
        return performanceStats.averageProcessingTimeMs || 0
    }
    
    function getCurrentProcessingTime() {
        return performanceStats.lastProcessingTimeMs || 0
    }
    
    function getActiveRouteCount() {
        return systemMetrics.activeRoutes || 0
    }
    
    function getEmergencyReleases() {
        return operationalStats.emergencyReleases || 0
    }
    
    function getPendingRequests() {
        return systemMetrics.pendingRequests || 0
    }
    
    function getAverageQueueSize() {
        return performanceStats.averageQueueSize || 0
    }
    
    function getRouteConflicts() {
        return systemMetrics.routeConflicts || 0
    }
    
    function getAverageConflicts() {
        return performanceStats.averageConflicts || 0
    }
    
    function getSystemLoad() {
        return systemMetrics.systemLoad || 0
    }
    
    function getAverageLoad() {
        return performanceStats.averageLoad || 0
    }
    
    function getRecentAlerts() {
        // Mock data for now - would come from safety monitor service
        return [
            { time: "14:23:15", severity: "warning", message: "Processing time exceeded 2 seconds for route R-1234" },
            { time: "14:20:08", severity: "info", message: "Route successfully assigned: S01 → S05" },
            { time: "14:18:45", severity: "danger", message: "Emergency release triggered for route R-1230" },
            { time: "14:15:22", severity: "warning", message: "Queue size approaching maximum (8/10)" },
            { time: "14:12:10", severity: "info", message: "System performance within normal parameters" }
        ]
    }
    
    function getAlertIcon(severity) {
        switch(severity) {
            case "danger": return ""
            case "warning": return ""
            case "info": return ""
            default: return ""
        }
    }
    
    function getAlertColor(severity) {
        switch(severity) {
            case "danger": return "#e53e3e"
            case "warning": return "#d69e2e"
            case "info": return "#3182ce"
            default: return "#a0aec0"
        }
    }
    
    function refreshMetrics() {
        console.log(" PerformanceDashboard: Refreshing metrics")
        
        try {
            if (routeAssignmentService) {
                performanceStats = routeAssignmentService.getPerformanceStatistics()
                operationalStats = routeAssignmentService.getOperationalStatistics()
                systemMetrics = routeAssignmentService.getSystemStatus()
            }
            
            lastUpdateTime = Qt.formatDateTime(new Date(), "hh:mm:ss")
            console.log(" Metrics refreshed successfully")
        } catch (error) {
            console.error(" Failed to refresh metrics:", error)
            lastUpdateTime = "Error"
        }
    }
    
    // === COMPONENT LIFECYCLE ===
    Component.onCompleted: {
        console.log(" PerformanceDashboard initialized")
        refreshMetrics()
    }
    
    // === AUTO-REFRESH TIMER ===
    Timer {
        interval: 1000  // Refresh every second
        running: true
        repeat: true
        onTriggered: refreshMetrics()
    }
}
