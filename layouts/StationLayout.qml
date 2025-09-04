import QtQuick
import "../components"

Rectangle {
    id: stationLayout
    color: "#1e1e1e"

    signal databaseResetRequested()

    property var dbManager
    property int cellSize: Math.floor(width / 320)
    property bool showGrid: true
    property bool hasInitialDataLoaded: false
    
    // === ROUTE VISUALIZATION PROPERTIES ===
    property bool isRouteVisualizationEnabled: false
    property bool isRouteManagementVisible: false
    property bool isPerformanceDashboardVisible: false

    //  NEW: Data properties from database (replaces StationData.js)
    property var trackSegmentsModel: []
    property var outerSignalsModel: []
    property var homeSignalsModel: []
    property var starterSignalsModel: []
    property var advanceStarterSignalsModel: []
    property var pointMachinesModel: []
    property var textLabelsModel: []

    // App timing properties
    property var appStartTime: new Date()
    property string appUptime: "00:00:00"

    //  NEW: Data refresh functions (replaces signalRefreshTrigger)
    function refreshAllData() {
        if (!dbManager || !dbManager.isConnected) {
            console.log("Database not connected - cannot refresh data")
            return
        }

        console.log("Refreshing all station data from database")
        refreshTrackSegmentData()
        refreshSignalData()
        refreshPointMachineData()
        refreshTextLabelData()
    }

    function getTrackSegmentDataById(trackSegmentId) {
        for (var i = 0; i < trackSegmentsModel.length; i++) {
            if (trackSegmentsModel[i].id === trackSegmentId) {
                return trackSegmentsModel[i];
            }
        }
        return null;
    }

    //  UPDATED: Position mapping function
    function mapDatabasePosition(dbPosition) {
        // Database returns "1" for NORMAL, "2" for REVERSE
        if (dbPosition === "1") return "NORMAL";
        if (dbPosition === "2") return "REVERSE";
        // Fallback for string values
        if (dbPosition === "NORMAL" || dbPosition === "REVERSE") return dbPosition;
        return "NORMAL"; // Safe default
    }

    function refreshTrackSegmentData() {
        if (!dbManager || !dbManager.isConnected) return

        console.log("Refreshing trackSegment segments from database")
        trackSegmentsModel = dbManager.getTrackSegmentsList()
        console.log("Loaded", trackSegmentsModel.length, "trackSegment segments")
    }

    function refreshSignalData() {
        if (!dbManager || !dbManager.isConnected) return

        console.log("Refreshing signals from database")
        var allSignals = dbManager.getAllSignalsList()

        // Filter signals by type
        outerSignalsModel = allSignals.filter(signal => signal.type === "OUTER")
        homeSignalsModel = allSignals.filter(signal => signal.type === "HOME")
        starterSignalsModel = allSignals.filter(signal => signal.type === "STARTER")
        advanceStarterSignalsModel = allSignals.filter(signal => signal.type === "ADVANCED_STARTER")

        console.log("Loaded signals - Outer:", outerSignalsModel.length,
                   "Home:", homeSignalsModel.length,
                   "Starter:", starterSignalsModel.length,
                   "Advanced:", advanceStarterSignalsModel.length)
    }

    function refreshPointMachineData() {
        if (!dbManager || !dbManager.isConnected) return

        console.log("Refreshing point machines from database")
        pointMachinesModel = dbManager.getAllPointMachinesList()
        console.log("Loaded", pointMachinesModel.length, "point machines")
    }

    function refreshTextLabelData() {
        if (!dbManager || !dbManager.isConnected) return

        console.log("Refreshing text labels from database")
        textLabelsModel = dbManager.getTextLabelsList()
        console.log("Loaded", textLabelsModel.length, "text labels")
    }

    //  UPDATED: Signal handlers now update database instead of StationData.js
    //  REFACTORED: Direct hardware simulation (no validation)
    function handleTrackSegmentClick(trackSegmentId, currentState) {
        console.log(" HARDWARE SIMULATION: Track Segment section", trackSegmentId, "occupancy changed to:", !currentState)

        if (!dbManager || !dbManager.isConnected) {
            console.error(" CRITICAL: Database not connected - trackSegment occupancy change lost!")
            return
        }

        //  SIMULATE: Hardware directly updates database (no validation)
        var newState = !currentState
        var success = dbManager.updateTrackSegmentOccupancy(trackSegmentId, newState)

        if (!success) {
            console.error(" CRITICAL: Failed to update trackSegment occupancy - system may be unsafe!")
        }
        //  Reactive interlocking will be triggered automatically by database change
    }

    function updateDisplay() {
        console.log("Station display update requested")
        refreshAllData()
    }

    function handleOuterSignalClick(signalId, currentAspect) {
        console.log("Outer signal control:", signalId, "Current aspect:", currentAspect)

        if (!dbManager || !dbManager.isConnected) {
            showToast("Database Error",
                     "Cannot update signal - database not connected",
                     signalId,
                     "Database connection lost. Check network connectivity.",
                     "ERROR",
                     true)
            return
        }

        var signalData = dbManager.getSignalById(signalId)
        if (!signalData || !signalData.possibleAspects) {
            showToast("Signal Error", "Could not get signal configuration data",
                     signalId, "SIGNAL_DATA_NOT_FOUND")
            return
        }

        var possibleAspects = signalData.possibleAspects
        var currentIndex = possibleAspects.indexOf(currentAspect)
        var nextIndex = (currentIndex + 1) % possibleAspects.length
        var nextAspect = possibleAspects[nextIndex]

        console.log("Changing outer signal", signalId, "from", currentAspect, "to", nextAspect)

        var success = dbManager.updateSignalAspect(signalId, "MAIN", nextAspect)
        if (!success) {
            console.error("Failed to update outer signal aspect")
        }
    }

    function handleHomeSignalClick(signalId, currentAspect) {
        console.log("Home signal control:", signalId, "Current aspect:", currentAspect)

        if (!dbManager || !dbManager.isConnected) {
            console.warn("Database not connected - cannot update signal")
            return
        }

        var signalData = dbManager.getSignalById(signalId)
        if (!signalData || !signalData.possibleAspects) {
            console.error("Could not get signal data for", signalId)
            return
        }

        var possibleAspects = signalData.possibleAspects
        var currentIndex = possibleAspects.indexOf(currentAspect)
        var nextIndex = (currentIndex + 1) % possibleAspects.length
        var nextAspect = possibleAspects[nextIndex]

        console.log("Changing home signal", signalId, "from", currentAspect, "to", nextAspect)

        var success = dbManager.updateSignalAspect(signalId, "MAIN", nextAspect)
        if (success) {
            console.log("Home signal aspect updated successfully")
        } else {
            console.error("Failed to update home signal aspect")
        }
    }

    function handleStarterSignalClick(signalId, currentAspect) {
        console.log("Starter signal control:", signalId, "Current aspect:", currentAspect)

        if (!dbManager || !dbManager.isConnected) {
            console.warn("Database not connected - cannot update signal")
            return
        }

        var signalData = dbManager.getSignalById(signalId)
        if (!signalData || !signalData.possibleAspects) {
            console.error("Could not get signal data for", signalId)
            return
        }

        var possibleAspects = signalData.possibleAspects
        var currentIndex = possibleAspects.indexOf(currentAspect)
        var nextIndex = (currentIndex + 1) % possibleAspects.length
        var nextAspect = possibleAspects[nextIndex]

        console.log("Changing starter signal", signalId, "from", currentAspect, "to", nextAspect)

        var success = dbManager.updateSignalAspect(signalId, "MAIN", nextAspect)
        if (success) {
            console.log("Starter signal aspect updated successfully")
        } else {
            console.error("Failed to update starter signal aspect")
        }
    }

    //   Handle point machine operation (keep as strings)
    function handlePointMachineClick(machineId, currentPosition) {
        console.log("Point machine operation requested:", machineId, "Current position:", currentPosition)

        if (!dbManager || !dbManager.isConnected) {
            console.warn("Database not connected - cannot operate point machine")
            return
        }

        //   Toggle between string values, don't convert to numbers
        var targetPosition = (currentPosition === "NORMAL") ? "REVERSE" : "NORMAL"

        console.log("Operating point machine", machineId, "from", currentPosition, "to", targetPosition)

        //   Send string position codes, not numbers
        var success = dbManager.updatePointMachinePosition(machineId, targetPosition)
        if (success) {
            console.log("Point machine operation initiated successfully")
        } else {
            console.error("Failed to operate point machine")
        }
    }


    function handleAdvanceStarterSignalClick(signalId, currentAspect) {
        console.log("Advanced starter signal control:", signalId, "Current aspect:", currentAspect)

        if (!dbManager || !dbManager.isConnected) {
            console.warn("Database not connected - cannot update signal")
            return
        }

        var signalData = dbManager.getSignalById(signalId)
        if (!signalData || !signalData.possibleAspects) {
            console.error("Could not get signal data for", signalId)
            return
        }

        var possibleAspects = signalData.possibleAspects
        var currentIndex = possibleAspects.indexOf(currentAspect)
        var nextIndex = (currentIndex + 1) % possibleAspects.length
        var nextAspect = possibleAspects[nextIndex]

        console.log("Changing advanced starter signal", signalId, "from", currentAspect, "to", nextAspect)

        var success = dbManager.updateSignalAspect(signalId, "MAIN", nextAspect)
        if (success) {
            console.log("Advanced starter signal aspect updated successfully", nextAspect)
        } else {
            console.error("Failed to update advanced starter signal aspect")
        }
    }

    function updateSignalAspectDirect(signalId, aspectType, selectedAspect) {
        console.log("Direct signal aspect change:", signalId, aspectType, "to", selectedAspect)

        if (!dbManager || !dbManager.isConnected) {
            showToast("Database Error", "Cannot update signal - database not connected",
                     signalId, "DATABASE_DISCONNECTED")
            return
        }

        //  NEW: Call with aspect type
        var success = dbManager.updateSignalAspect(signalId, aspectType, selectedAspect)
        if (!success) {
            console.error("Failed to update signal aspect:", signalId, aspectType, selectedAspect)
        }
    }

    function showToast(title, message, entityId, details, type, autoHide) {
        toastNotification.show(
            title || "Notification",
            message || "",
            entityId || "",
            details || "",
            type || "INFO",
            autoHide !== undefined ? autoHide : true
        )
    }

    function showCriticalAlert(title, message, entityId, details) {
        toastNotification.show(title, message, entityId, details, "CRITICAL", false)
    }

    function showSignalBlockedToast(title, message, signalId, reason) {
        toastNotification.show(title, message, signalId, reason, "ERROR", true)
    }

    function getPollingStatusColor() {
        if (!dbManager || !dbManager.isConnected) return "#ef4444" // Red - disconnected

        var interval = dbManager.currentPollingInterval
        if (interval === 0) return "#ef4444"        // Red - not polling
        if (interval <= 11000) return "#f6ad55"      // Orange - fast polling
        if (interval <= 50000) return "#38a169"     // Green - normal polling
        return "#3182ce"                            // Blue - slow polling (notifications working)
    }

    //  NEW: Initialize data when component loads or database connects
    Component.onCompleted: {
        console.log("StationLayout: Component completed")
        if (dbManager && dbManager.isConnected && !hasInitialDataLoaded) {
            console.log("StationLayout: Loading initial data")
            refreshAllData()
            hasInitialDataLoaded = true
        } else {
            console.log("StationLayout: Data already loaded or DB not connected")
        }
    }

    //  NEW: Watch for database connection and data changes
    Connections {
        target: dbManager

        function onConnectionStateChanged(isConnected) {
            console.log("StationLayout: Database connection state changed:", isConnected)
            if (isConnected && !hasInitialDataLoaded) {
                console.log("StationLayout: First connection - loading data")
                refreshAllData()
                hasInitialDataLoaded = true
            } else if (isConnected) {
                console.log("StationLayout: Reconnected - data already loaded")
            } else {
                console.log("Database disconnected - clearing data models")
                trackSegmentsModel = []
                outerSignalsModel = []
                homeSignalsModel = []
                starterSignalsModel = []
                advanceStarterSignalsModel = []
                pointMachinesModel = []
                textLabelsModel = []
                hasInitialDataLoaded = false  //  Reset for next connection
            }
        }

        function onOperationBlocked(signalId, reason) {
            console.log(" Operation blocked signal received:", signalId, reason)
            showToast("Signal Operation Blocked",
                     "The requested signal operation could not be completed.",
                     signalId, reason)
        }

        //  Handle database polling updates
        function onDataUpdated() {
            console.log("StationLayout: Database data updated (polling)")
            refreshAllData()
        }

        //  Handle real-time notifications (if available)
        function onTrackSegmentUpdated(segmentId) {
            console.log("StationLayout: Track segment updated:", segmentId)
            refreshTrackSegmentData()
        }

        function onSignalUpdated(signalId) {
            console.log("StationLayout: Signal updated:", signalId)
            refreshSignalData()
        }

        function onPointMachineUpdated(machineId) {
            console.log("StationLayout: Point machine updated:", machineId)
            refreshPointMachineData()
        }

        function onPairedMachinesUpdated(machineIds) {
            console.log("Paired machines updated together:", machineIds)
            // Update UI for all affected machines
            for (let i = 0; i < machineIds.length; i++) {
                updatePointMachineDisplay(machineIds[i])
            }
        }

        function onPositionMismatchCorrected(machineId, pairedMachineId) {
                // Show critical warning to operator
                showCriticalAlert("Position Mismatch Corrected",
                    `Point machines ${machineId} and ${pairedMachineId} had different positions. ` +
                    `${machineId} has been synchronized to match its pair.`)
            }

        //  Handle batch updates
        function onTrackSegmentsChanged() {
            console.log("StationLayout: Track Segment segments changed")
            refreshTrackSegmentData()
        }

        function onSignalsChanged() {
            console.log("StationLayout: Signals changed")
            refreshSignalData()
        }

        function onPointMachinesChanged() {
            console.log("StationLayout: Point machines changed")
            refreshPointMachineData()
        }

        function onTextLabelsChanged() {
            console.log("StationLayout: Text labels changed")
            refreshTextLabelData()
        }
    }

    ToastNotification {
        id: toastNotification
        anchors.fill: parent
    }

    SignalContextMenu {
        id: signalContextMenu
        anchors.fill: parent

        onAspectSelected: function(signalId, aspectType, selectedAspect) {
                console.log("Context menu aspect selected:", signalId, aspectType, "→", selectedAspect)
                updateSignalAspectDirect(signalId, aspectType, selectedAspect)
        }
    }

    // Main grid canvas
    GridCanvas {
        id: canvas
        anchors.fill: parent
        gridSize: stationLayout.cellSize
        showGrid: stationLayout.showGrid

        //  UPDATED: Track Segment segments from database
        Repeater {
            model: trackSegmentsModel

            TrackSegment {
                segmentId: modelData.id
                segmentName: modelData.name || ""  //  NEW
                startRow: modelData.startRow
                startCol: modelData.startCol
                endRow: modelData.endRow
                endCol: modelData.endCol
                trackSegmentType: modelData.trackSegmentType || "STRAIGHT"  //  NEW
                cellSize: stationLayout.cellSize
                isOccupied: modelData.occupied
                isAssigned: modelData.assigned
                isOverlap: modelData.isOverlap
                occupiedBy: modelData.occupiedBy || ""  //  NEW
                isActive: modelData.isActive !== false  //  NEW
                onTrackSegmentClicked: stationLayout.handleTrackSegmentClick(segmentId, isOccupied)
            }
        }

        //  UPDATED: Point machines from database
        Repeater {
            model: pointMachinesModel

            PointMachine {
                machineId: modelData.id
                machineName: modelData.name || ""
                position: modelData.position //  Convert 1/2 to NORMAL/REVERSE
                operatingStatus: modelData.operatingStatus
                junctionPoint: modelData.junctionPoint
                rootTrackSegment: modelData.rootTrackSegment
                normalTrackSegment: modelData.normalTrackSegment
                reverseTrackSegment: modelData.reverseTrackSegment
                transitionTime: modelData.transitionTime || 3000
                isLocked: modelData.isLocked || false
                lockReason: modelData.lockReason || ""
                cellSize: stationLayout.cellSize

                //  CRITICAL: Pass trackSegment lookup function
                trackSegmentDataLookup: stationLayout.getTrackSegmentDataById

                onPointMachineClicked: function(machineId, currentPosition) {
                    stationLayout.handlePointMachineClick(machineId, currentPosition)
                }
            }
        }

        //  UPDATED: Outer signals from database
        Repeater {
            model: outerSignalsModel

            OuterSignal {
                x: modelData.col * stationLayout.cellSize
                y: modelData.row * stationLayout.cellSize
                signalId: modelData.id
                signalName: modelData.name
                currentAspect: modelData.currentAspect
                aspectCount: modelData.aspectCount || 4  //  NEW
                possibleAspects: modelData.possibleAspects || []  //  NEW
                direction: modelData.direction
                isActive: modelData.isActive
                locationDescription: modelData.location || ""  //  NEW
                cellSize: stationLayout.cellSize
                onSignalClicked: stationLayout.handleOuterSignalClick(signalId, currentAspect)

                onContextMenuRequested: function(signalId, signalName, currentAspect, possibleAspects, x, y) {
                    signalContextMenu.show(x, y, signalId, signalName, currentAspect, possibleAspects)
                }
            }
        }

        //  UPDATED: Home signals from database
        //  UPDATED: Home signals from database
        Repeater {
            model: homeSignalsModel

            HomeSignal {
                x: modelData.col * stationLayout.cellSize
                y: modelData.row * stationLayout.cellSize
                signalId: modelData.id
                signalName: modelData.name
                currentAspect: modelData.currentAspect
                aspectCount: modelData.aspectCount || 3
                possibleAspects: modelData.possibleAspects || []
                callingOnAspect: modelData.callingOnAspect
                loopAspect: modelData.loopAspect
                loopSignalConfiguration: modelData.loopSignalConfiguration
                direction: modelData.direction
                isActive: modelData.isActive
                locationDescription: modelData.location || ""
                cellSize: stationLayout.cellSize
                onSignalClicked: stationLayout.handleHomeSignalClick(signalId, currentAspect)

                //  ADD THIS CONTEXT MENU HANDLER:
                onContextMenuRequested: function(signalId, signalName, currentAspect, possibleAspects,
                                                       callingOnAspect, loopAspect, x, y) {
                            console.log(" DEBUG: Home signal context menu requested:")
                            console.log("  - Signal:", signalId, signalName)
                            console.log("  - Main aspect:", currentAspect)
                            console.log("  - Calling-On:", callingOnAspect)
                            console.log("  - Loop:", loopAspect)

                            //  Pass all parameters including subsidiary signals
                            signalContextMenu.show(x, y, signalId, signalName, currentAspect, possibleAspects,
                                                  callingOnAspect, loopAspect)
                        }
            }
        }

        //  UPDATED: Starter signals from database
        Repeater {
            model: starterSignalsModel

            StarterSignal {
                x: modelData.col * stationLayout.cellSize
                y: modelData.row * stationLayout.cellSize
                signalId: modelData.id
                signalName: modelData.name
                currentAspect: modelData.currentAspect
                aspectCount: modelData.aspectCount
                possibleAspects: modelData.possibleAspects || []  //  NEW
                direction: modelData.direction
                isActive: modelData.isActive
                locationDescription: modelData.location || ""  //  NEW
                cellSize: stationLayout.cellSize
                onSignalClicked: stationLayout.handleStarterSignalClick(signalId, currentAspect)

                onContextMenuRequested: function(signalId, signalName, currentAspect, possibleAspects, x, y) {
                    signalContextMenu.show(x, y, signalId, signalName, currentAspect, possibleAspects)
                }
            }
        }

        //  UPDATED: Advanced starter signals from database
        Repeater {
            model: advanceStarterSignalsModel

            AdvanceStarterSignal {
                x: modelData.col * stationLayout.cellSize
                y: modelData.row * stationLayout.cellSize
                signalId: modelData.id
                signalName: modelData.name
                currentAspect: modelData.currentAspect
                aspectCount: modelData.aspectCount || 2  // Always 2 for advanced starter
                possibleAspects: modelData.possibleAspects || []  //  NEW
                direction: modelData.direction
                isActive: modelData.isActive
                locationDescription: modelData.location || ""  //  NEW
                cellSize: stationLayout.cellSize
                onSignalClicked: stationLayout.handleAdvanceStarterSignalClick(signalId, currentAspect)
                onContextMenuRequested: function(signalId, signalName, currentAspect, possibleAspects, x, y) {
                    signalContextMenu.show(x, y, signalId, signalName, currentAspect, possibleAspects)
                }
            }
        }

        //  UPDATED: Text labels from database
        Repeater {
            model: textLabelsModel

            Text {
                x: modelData.col * stationLayout.cellSize
                y: modelData.row * stationLayout.cellSize
                text: modelData.text
                color: modelData.color || "#ffffff"
                font.pixelSize: modelData.fontSize || 12
                font.family: modelData.fontFamily || "Arial"
                visible: modelData.isVisible !== false
            }
        }
    }

    // Uptime timer (unchanged)
    Timer {
        id: uptimeTimer
        interval: 1000
        running: true
        repeat: true

        onTriggered: {
            var currentTime = new Date()
            var uptimeMs = currentTime.getTime() - stationLayout.appStartTime.getTime()
            var totalSeconds = Math.floor(uptimeMs / 1000)
            var hours = Math.floor(totalSeconds / 3600)
            var minutes = Math.floor((totalSeconds % 3600) / 60)
            var seconds = totalSeconds % 60
            var hoursStr = hours.toString().padStart(2, '0')
            var minutesStr = minutes.toString().padStart(2, '0')
            var secondsStr = seconds.toString().padStart(2, '0')
            stationLayout.appUptime = hoursStr + ":" + minutesStr + ":" + secondsStr
        }
    }

    //  ENHANCED: Status panel with database integration
    Rectangle {
        id: statusPanel
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.margins: 10

        property bool isExpanded: false
        property int collapsedWidth: 220
        property int collapsedHeight: 60
        property int expandedWidth: 400
        property int expandedHeight: 350

        width: isExpanded ? expandedWidth : collapsedWidth
        height: isExpanded ? expandedHeight : collapsedHeight

        color: "#2d3748"
        border.color: isExpanded ? "#3182ce" : "#4a5568"
        border.width: 2
        radius: 6

        Behavior on width { NumberAnimation { duration: 300; easing.type: Easing.OutQuart } }
        Behavior on height { NumberAnimation { duration: 300; easing.type: Easing.OutQuart } }
        Behavior on border.color { ColorAnimation { duration: 300 } }

        Rectangle {
            id: expandButton
            anchors.top: parent.top
            anchors.left: parent.left
            anchors.margins: 8
            width: 24
            height: 24
            color: expandButtonMouse.pressed ? "#3182ce" : (expandButtonMouse.containsMouse ? "#4a90c2" : "#5a6478")
            border.color: "#ffffff"
            border.width: 1
            radius: 4

            Text {
                anchors.centerIn: parent
                text: statusPanel.isExpanded ? "▼" : "▲"
                color: "#ffffff"
                font.pixelSize: 12
                font.bold: true
            }

            MouseArea {
                id: expandButtonMouse
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: statusPanel.isExpanded = !statusPanel.isExpanded
            }
        }

        Text {
            id: panelTitle
            anchors.top: parent.top
            anchors.left: expandButton.right
            anchors.right: parent.right
            anchors.margins: 8
            text: statusPanel.isExpanded ? "Railway Control Panel - Database Mode" : "Controls"
            color: "#ffffff"
            font.pixelSize: statusPanel.isExpanded ? 14 : 12
            font.weight: Font.Bold
            horizontalAlignment: Text.AlignHCenter
            height: 24
        }

        Grid {
            id: sectionsGrid
            anchors.top: panelTitle.bottom
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            anchors.margins: 12
            visible: statusPanel.isExpanded

            columns: 2
            rows: 2
            spacing: 8

            //  UPDATED: Database-aware system info
            Rectangle {
                width: (sectionsGrid.width - sectionsGrid.spacing) / 2
                height: (sectionsGrid.height - sectionsGrid.spacing) / 2
                color: "#374151"
                radius: 4
                border.color: "#4a5568"
                border.width: 1

                Column {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    Text {
                        text: "Database Status"
                        color: "#ffffff"
                        font.pixelSize: 11
                        font.weight: Font.Bold
                    }

                    Row {
                        width: parent.width
                        Text {
                            text: "Connection:"
                            color: "#a0aec0"
                            font.pixelSize: 9
                            width: 60
                        }
                        Text {
                            text: dbManager && dbManager.isConnected ? "Connected" : "Offline"
                            color: dbManager && dbManager.isConnected ? "#38a169" : "#ef4444"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }

                    Row {
                        width: parent.width
                        Text {
                            text: "Track Segments:"
                            color: "#a0aec0"
                            font.pixelSize: 9
                            width: 60
                        }
                        Text {
                            text: trackSegmentsModel.length.toString()
                            color: "#38a169"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }

                    Row {
                        width: parent.width
                        Text {
                            text: "Signals:"
                            color: "#a0aec0"
                            font.pixelSize: 9
                            width: 60
                        }
                        Text {
                            text: (outerSignalsModel.length + homeSignalsModel.length +
                                  starterSignalsModel.length + advanceStarterSignalsModel.length).toString()
                            color: "#38a169"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }

                    Row {
                        width: parent.width
                        Text {
                            text: "Points:"
                            color: "#a0aec0"
                            font.pixelSize: 9
                            width: 60
                        }
                        Text {
                            text: pointMachinesModel.length.toString()
                            color: "#38a169"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }
                }
            }

            // View controls section
            Rectangle {
                width: (sectionsGrid.width - sectionsGrid.spacing) / 2
                height: (sectionsGrid.height - sectionsGrid.spacing) / 2
                color: "#374151"
                radius: 4
                border.color: "#4a5568"
                border.width: 1

                Column {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 6

                    Text {
                        text: "View Controls"
                        color: "#ffffff"
                        font.pixelSize: 11
                        font.weight: Font.Bold
                    }

                    Rectangle {
                        width: parent.width - 10
                        height: 20
                        color: showGrid ? "#3182ce" : "#4a5568"
                        radius: 3

                        Text {
                            anchors.centerIn: parent
                            text: showGrid ? "Hide Grid" : "Show Grid"
                            color: "#ffffff"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: stationLayout.showGrid = !stationLayout.showGrid
                        }
                    }

                    //  NEW: Manual refresh button
                    Rectangle {
                        width: parent.width - 10
                        height: 20
                        color: refreshMouse.pressed ? "#2c5aa0" : "#3182ce"
                        radius: 3

                        Text {
                            anchors.centerIn: parent
                            text: " Refresh Data"
                            color: "#ffffff"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }

                        MouseArea {
                            id: refreshMouse
                            anchors.fill: parent
                            onClicked: {
                                console.log("Manual data refresh requested")
                                stationLayout.refreshAllData()
                            }
                        }
                    }
                }
            }

            // Data source info section
            Rectangle {
                width: (sectionsGrid.width - sectionsGrid.spacing) / 2
                height: (sectionsGrid.height - sectionsGrid.spacing) / 2
                color: "#374151"
                radius: 4
                border.color: "#4a5568"
                border.width: 1

                Column {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4

                    Text {
                        text: "Data Source"
                        color: "#ffffff"
                        font.pixelSize: 11
                        font.weight: Font.Bold
                    }

                    Row {
                        width: parent.width
                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: "#3182ce"
                        }
                        Text {
                            text: "PostgreSQL Database"
                            color: "#a0aec0"
                            font.pixelSize: 9
                            leftPadding: 6
                        }
                    }

                    Row {
                        width: parent.width
                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: getPollingStatusColor() //  Dynamic color based on interval
                        }
                        Text {
                            //  FIX: Use property instead of function call
                            text: "Polling: " + (dbManager ? dbManager.pollingIntervalDisplay : "Unknown")
                            color: "#a0aec0"
                            font.pixelSize: 9
                            leftPadding: 6
                        }
                    }

                    Row {
                        width: parent.width
                        Rectangle {
                            width: 8
                            height: 8
                            radius: 4
                            color: "#f6ad55"
                        }
                        Text {
                            text: "Real-time updates"
                            color: "#a0aec0"
                            font.pixelSize: 9
                            leftPadding: 6
                        }
                    }

                    Row {
                        width: parent.width
                        Text {
                            text: "Uptime:"
                            color: "#a0aec0"
                            font.pixelSize: 9
                            width: 40
                        }
                        Text {
                            text: stationLayout.appUptime
                            color: "#ffffff"
                            font.pixelSize: 9
                            font.weight: Font.Bold
                        }
                    }
                }
            }

            // Database controls section
            Rectangle {
                width: (sectionsGrid.width - sectionsGrid.spacing) / 2
                height: (sectionsGrid.height - sectionsGrid.spacing) / 2
                color: "#374151"
                radius: 4
                border.color: "#4a5568"
                border.width: 1
                Column {
                    anchors.fill: parent
                    anchors.margins: 8
                    spacing: 4
                    Text {
                        text: "Database Controls"
                        color: "#ffffff"
                        font.pixelSize: 11
                        font.weight: Font.Bold
                    }
                    Rectangle {
                        width: parent.width - 10
                        height: 18
                        color: testConnectionMouse.pressed ? "#2c5aa0" : "#3182ce"
                        radius: 3
                        Text {
                            anchors.centerIn: parent
                            text: "Test Connection"
                            color: "#ffffff"
                            font.pixelSize: 8
                            font.weight: Font.Bold
                        }
                        MouseArea {
                            id: testConnectionMouse
                            anchors.fill: parent
                            onClicked: {
                                console.log("Testing database connection...")
                                if (globalDatabaseInitializer) {
                                    globalDatabaseInitializer.testConnection()
                                }
                            }
                        }
                    }
                    //  NEW: Debug Connection Test Button
                    Rectangle {
                        width: parent.width - 10
                        height: 18
                        color: debugConnectionMouse.pressed ? "#805ad5" : "#9f7aea"
                        radius: 3
                        Text {
                            anchors.centerIn: parent
                            text: " Debug Connection"
                            color: "#ffffff"
                            font.pixelSize: 8
                            font.weight: Font.Bold
                        }
                        MouseArea {
                            id: debugConnectionMouse
                            anchors.fill: parent
                            onClicked: {
                                console.log("Running debug connection test...")
                                if (globalDatabaseInitializer) {
                                    globalDatabaseInitializer.debugConnectionTest()
                                }
                            }
                        }
                    }
                    Rectangle {
                        width: parent.width - 10
                        height: 20
                        color: resetButtonMouse.pressed ? "#c53030" : "#e53e3e"
                        radius: 3
                        Text {
                            anchors.centerIn: parent
                            text: " Reset Database"
                            color: "#ffffff"
                            font.pixelSize: 8
                            font.weight: Font.Bold
                        }
                        MouseArea {
                            id: resetButtonMouse
                            anchors.fill: parent
                            onClicked: {
                                console.log("Database reset requested from status panel")
                                stationLayout.databaseResetRequested()
                            }
                        }
                    }
                }
            }
        }
    }
    RouteAssignmentDialog {
        id: routeAssignmentDialog
    }
}
