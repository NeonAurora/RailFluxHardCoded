import QtQuick 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15

Rectangle {
    id: routeAssignmentDialog

    // === PROPERTIES ===
    property string sourceSignalId: ""
    property string sourceSignalName: ""
    property var availableSignals: []
    property string selectedDestSignalId: ""
    property bool isVisible: false
    property var scanResults: ({})
    property bool isScanning: false

    // === DIALOG CONFIGURATION ===
    width: 400  // Increased width for scan results
    height: 500  // Increased height for scan results
    visible: isVisible
    z: 1000  // High z-order to appear on top

    // === MODERN COLOR PALETTE ===
    readonly property color cardBackground: "#ffffff"
    readonly property color textPrimary: "#334155"
    readonly property color textSecondary: "#64748b"
    readonly property color textMuted: "#94a3b8"
    readonly property color accentBlue: "#0ea5e9"
    readonly property color accentBlueDark: "#0284c7"
    readonly property color successGreen: "#10b981"
    readonly property color warningYellow: "#f59e0b"
    readonly property color errorRed: "#ef4444"
    readonly property color borderLight: "#e2e8f0"
    readonly property color borderMedium: "#cbd5e1"
    readonly property color hoverBackground: "#f1f5f9"

    // === DRAGGABLE FUNCTIONALITY ===
    property point dragOffset: Qt.point(0, 0)
    property bool isDragging: false

    // Position in center initially
    Component.onCompleted: {
        if (parent) {
            x = (parent.width - width) / 2
            y = (parent.height - height) / 2
        }
    }

    // Native shadow layers
    Rectangle {
        anchors.fill: parent
        anchors.margins: -6
        color: "#000000"
        opacity: 0.04
        radius: 16
        z: -3
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: -3
        color: "#000000"
        opacity: 0.04
        radius: 15
        z: -2
    }
    Rectangle {
        anchors.fill: parent
        anchors.margins: -1
        color: "#000000"
        opacity: 0.04
        radius: 14
        z: -1
    }

    // Main dialog background
    color: cardBackground
    radius: 14
    border.color: borderLight
    border.width: 1

    // Subtle gradient overlay
    Rectangle {
        anchors.fill: parent
        radius: 14
        gradient: Gradient {
            GradientStop { position: 0.0; color: "#ffffff" }
            GradientStop { position: 1.0; color: "#f8fafc" }
        }
        opacity: 0.7
    }

    // === DRAG AREA (Top portion for dragging) ===
    MouseArea {
        id: dragArea
        anchors.top: parent.top
        anchors.left: parent.left
        anchors.right: parent.right
        height: 40
        cursorShape: Qt.SizeAllCursor

        onPressed: {
            isDragging = true
            dragOffset = Qt.point(mouse.x, mouse.y)
        }

        onPositionChanged: {
            if (isDragging && pressed) {
                var newX = routeAssignmentDialog.x + mouse.x - dragOffset.x
                var newY = routeAssignmentDialog.y + mouse.y - dragOffset.y

                // Keep dialog within parent bounds
                if (routeAssignmentDialog.parent) {
                    newX = Math.max(0, Math.min(newX, routeAssignmentDialog.parent.width - routeAssignmentDialog.width))
                    newY = Math.max(0, Math.min(newY, routeAssignmentDialog.parent.height - routeAssignmentDialog.height))
                }

                routeAssignmentDialog.x = newX
                routeAssignmentDialog.y = newY
            }
        }

        onReleased: {
            isDragging = false
        }

        // Visual drag indicator
        Rectangle {
            anchors.centerIn: parent
            width: 30
            height: 4
            radius: 2
            color: borderMedium
            opacity: parent.containsMouse ? 0.8 : 0.4

            Behavior on opacity { NumberAnimation { duration: 200 } }
        }
    }

    // === CLOSE BUTTON ===
    MouseArea {
        anchors.top: parent.top
        anchors.right: parent.right
        anchors.margins: 8
        width: 24
        height: 24
        cursorShape: Qt.PointingHandCursor

        onClicked: close()

        Rectangle {
            anchors.fill: parent
            radius: 12
            color: parent.containsMouse ? "#f1f5f9" : "transparent"

            Text {
                anchors.centerIn: parent
                text: "×"
                font.pixelSize: 16
                font.weight: Font.Bold
                color: textSecondary
            }
        }
    }

    // === ANIMATIONS ===
    PropertyAnimation {
        id: showAnimation
        target: routeAssignmentDialog
        property: "scale"
        from: 0.9
        to: 1.0
        duration: 200
        easing.type: Easing.OutCubic
    }

    PropertyAnimation {
        id: fadeInAnimation
        target: routeAssignmentDialog
        property: "opacity"
        from: 0.0
        to: 1.0
        duration: 200
        easing.type: Easing.OutCubic
    }

    // === MAIN CONTENT ===
    ColumnLayout {
        anchors.fill: parent
        anchors.margins: 20
        anchors.topMargin: 50  // Space for drag area
        anchors.bottomMargin: 20
        spacing: 16

        // === FROM SIGNAL (READ-ONLY) ===
        Rectangle {
            Layout.fillWidth: true
            Layout.preferredHeight: 50
            color: cardBackground
            border.color: borderLight
            border.width: 1
            radius: 8

            RowLayout {
                anchors.fill: parent
                anchors.margins: 12
                spacing: 8

                Text {
                    text: "From:"
                    font.pixelSize: 12
                    font.weight: Font.Medium
                    color: textSecondary
                    Layout.minimumWidth: 35
                }

                Text {
                    text: sourceSignalId || "No signal"
                    font.pixelSize: 14
                    font.weight: Font.Medium
                    color: sourceSignalId ? textPrimary : textMuted
                    Layout.fillWidth: true
                }

                Rectangle {
                    width: 8
                    height: 8
                    radius: 4
                    color: sourceSignalId ? successGreen : errorRed

                    SequentialAnimation on opacity {
                        running: sourceSignalId
                        loops: Animation.Infinite
                        NumberAnimation { to: 0.5; duration: 1000 }
                        NumberAnimation { to: 1.0; duration: 1000 }
                    }
                }
            }
        }

        // === SCAN DESTINATIONS BUTTON ===
        Button {
            id: scanButton
            Layout.fillWidth: true
            Layout.preferredHeight: 44
            enabled: sourceSignalId && !isScanning

            text: isScanning ? "Scanning..." : "Scan Destinations"

            onClicked: performDestinationScan()

            background: Rectangle {
                color: {
                    if (!parent.enabled) return borderLight
                    if (parent.pressed) return accentBlueDark
                    if (parent.hovered) return accentBlue
                    return accentBlue
                }
                radius: 8

                Behavior on color { ColorAnimation { duration: 200 } }
            }

            contentItem: RowLayout {
                spacing: 8

                // Loading spinner
                Rectangle {
                    width: 16
                    height: 16
                    color: "transparent"
                    visible: isScanning

                    Rectangle {
                        anchors.centerIn: parent
                        width: 12
                        height: 12
                        radius: 6
                        border.color: "#ffffff"
                        border.width: 2
                        color: "transparent"

                        Rectangle {
                            anchors.left: parent.left
                            anchors.top: parent.top
                            width: 6
                            height: 6
                            radius: 3
                            color: "#ffffff"

                            RotationAnimation on rotation {
                                running: isScanning
                                loops: Animation.Infinite
                                duration: 1000
                                from: 0
                                to: 360
                            }
                        }
                    }
                }

                Text {
                    text: scanButton.text
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: scanButton.enabled ? "#ffffff" : textMuted
                    Layout.fillWidth: true
                    horizontalAlignment: Text.AlignHCenter
                }
            }
        }

        // === SCAN RESULTS TABS ===
        TabBar {
            id: resultsTabBar
            Layout.fillWidth: true
            visible: scanResults.success || false

            background: Rectangle {
                color: "transparent"
            }

            TabButton {
                text: "Ready (" + (scanResults.reachable_clear ? scanResults.reachable_clear.length : 0) + ")"
                background: Rectangle {
                    color: parent.checked ? successGreen : "transparent"
                    radius: 6
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    color: parent.checked ? "#ffffff" : textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            TabButton {
                text: "Needs PM (" + (scanResults.reachable_requires_pm ? scanResults.reachable_requires_pm.length : 0) + ")"
                background: Rectangle {
                    color: parent.checked ? warningYellow : "transparent"
                    radius: 6
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    color: parent.checked ? "#ffffff" : textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            TabButton {
                text: "Blocked (" + (scanResults.blocked ? scanResults.blocked.length : 0) + ")"
                background: Rectangle {
                    color: parent.checked ? errorRed : "transparent"
                    radius: 6
                }
                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 11
                    font.weight: Font.Medium
                    color: parent.checked ? "#ffffff" : textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        // === SCAN RESULTS CONTENT ===
        StackLayout {
            id: resultsStack
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: resultsTabBar.currentIndex
            visible: scanResults.success || false

            // Ready destinations
            ScrollView {
                ListView {
                    id: readyList
                    model: scanResults.reachable_clear || []
                    delegate: destinationDelegate
                    spacing: 4

                    Component {
                        id: destinationDelegate
                        Rectangle {
                            width: readyList.width
                            height: 60
                            color: mouseArea.containsMouse ? hoverBackground : "transparent"
                            border.color: selectedDestSignalId === modelData.dest_signal_id ? accentBlue : borderLight
                            border.width: selectedDestSignalId === modelData.dest_signal_id ? 2 : 1
                            radius: 8

                            MouseArea {
                                id: mouseArea
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: selectDestination(modelData)
                            }

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 8

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Text {
                                        text: modelData.display_name || modelData.dest_signal_id
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        color: textPrimary
                                    }

                                    Text {
                                        text: formatPathPreview(modelData.path_summary)
                                        font.pixelSize: 11
                                        color: textSecondary
                                    }
                                }

                                // Status chip
                                Rectangle {
                                    width: 60
                                    height: 20
                                    radius: 10
                                    color: getStatusColor(modelData.reachability)

                                    Text {
                                        anchors.centerIn: parent
                                        text: getStatusText(modelData.reachability)
                                        font.pixelSize: 10
                                        font.weight: Font.Medium
                                        color: "#ffffff"
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Requires PM destinations
            ScrollView {
                ListView {
                    id: pmList
                    model: scanResults.reachable_requires_pm || []
                    delegate: destinationDelegateWithPM
                    spacing: 4

                    Component {
                        id: destinationDelegateWithPM
                        Rectangle {
                            width: pmList.width
                            height: 80
                            color: mouseArea2.containsMouse ? hoverBackground : "transparent"
                            border.color: selectedDestSignalId === modelData.dest_signal_id ? accentBlue : borderLight
                            border.width: selectedDestSignalId === modelData.dest_signal_id ? 2 : 1
                            radius: 8

                            MouseArea {
                                id: mouseArea2
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: selectDestination(modelData)
                            }

                            ColumnLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 4

                                RowLayout {
                                    Layout.fillWidth: true

                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        spacing: 2

                                        Text {
                                            text: modelData.display_name || modelData.dest_signal_id
                                            font.pixelSize: 13
                                            font.weight: Font.Medium
                                            color: textPrimary
                                        }

                                        Text {
                                            text: formatPathPreview(modelData.path_summary)
                                            font.pixelSize: 11
                                            color: textSecondary
                                        }
                                    }

                                    Rectangle {
                                        width: 70
                                        height: 20
                                        radius: 10
                                        color: warningYellow

                                        Text {
                                            anchors.centerIn: parent
                                            text: "PM x" + (modelData.required_pm_actions ? modelData.required_pm_actions.length : 0)
                                            font.pixelSize: 10
                                            font.weight: Font.Medium
                                            color: "#ffffff"
                                        }
                                    }
                                }

                                // PM actions preview
                                Text {
                                    text: formatPMActions(modelData.required_pm_actions)
                                    font.pixelSize: 10
                                    color: textMuted
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }
            }

            // Blocked destinations
            ScrollView {
                ListView {
                    id: blockedList
                    model: scanResults.blocked || []
                    delegate: blockedDestinationDelegate
                    spacing: 4

                    Component {
                        id: blockedDestinationDelegate
                        Rectangle {
                            width: blockedList.width
                            height: 60
                            color: "#fef2f2"
                            border.color: borderLight
                            border.width: 1
                            radius: 8
                            opacity: 0.7

                            RowLayout {
                                anchors.fill: parent
                                anchors.margins: 8
                                spacing: 8

                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 2

                                    Text {
                                        text: modelData.display_name || modelData.dest_signal_id
                                        font.pixelSize: 13
                                        font.weight: Font.Medium
                                        color: textPrimary
                                    }

                                    Text {
                                        text: modelData.blocked_reason || "Blocked"
                                        font.pixelSize: 11
                                        color: errorRed
                                    }
                                }

                                Rectangle {
                                    width: 60
                                    height: 20
                                    radius: 10
                                    color: errorRed

                                    Text {
                                        anchors.centerIn: parent
                                        text: "BLOCKED"
                                        font.pixelSize: 9
                                        font.weight: Font.Medium
                                        color: "#ffffff"
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        // === SPACER ===
        Item {
            Layout.fillHeight: true
            Layout.minimumHeight: 10
            visible: !scanResults.success
        }

        // === ACTION BUTTONS ===
        RowLayout {
            Layout.fillWidth: true
            Layout.preferredHeight: 40
            spacing: 8

            // Request Route Button
            Button {
                text: "Request Route"
                Layout.fillWidth: true
                Layout.preferredHeight: 40
                enabled: sourceSignalId && selectedDestSignalId && isDestinationAssignable()

                onClicked: submitRouteRequest()

                background: Rectangle {
                    color: {
                        if (!parent.enabled) return borderLight
                        if (parent.pressed) return accentBlueDark
                        if (parent.hovered) return accentBlue
                        return accentBlue
                    }
                    radius: 6

                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: parent.enabled ? "#ffffff" : textMuted
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }

            // Cancel Button
            Button {
                text: "Cancel"
                Layout.fillWidth: true
                Layout.preferredHeight: 40

                onClicked: close()

                background: Rectangle {
                    color: {
                        if (parent.pressed) return borderMedium
                        if (parent.hovered) return hoverBackground
                        return "transparent"
                    }
                    border.color: borderMedium
                    border.width: 1
                    radius: 6

                    Behavior on color { ColorAnimation { duration: 200 } }
                }

                contentItem: Text {
                    text: parent.text
                    font.pixelSize: 13
                    font.weight: Font.Medium
                    color: textSecondary
                    horizontalAlignment: Text.AlignHCenter
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }
    }

    // === FUNCTIONS ===
    function openForSignal(signalId, signalName) {
        sourceSignalId = signalId
        sourceSignalName = signalName || signalId
        resetForm()
        open()
    }

    function open() {
        isVisible = true
        showAnimation.start()
        fadeInAnimation.start()
    }

    function close() {
        isVisible = false
        resetForm()
    }

    function resetForm() {
        selectedDestSignalId = ""
        scanResults = {}
        resultsTabBar.currentIndex = 0
    }

    function performDestinationScan() {
        if (!globalRouteAssignmentService) {
            console.error(" RouteAssignmentService not available for scanning")
            return
        }

        if (!sourceSignalId) {
            console.error(" No source signal ID for scanning")
            return
        }

        isScanning = true
        console.log(" Scanning destinations for signal:", sourceSignalId)

        // Call the new scanning API
        scanResults = globalRouteAssignmentService.scanDestinationSignals(
            sourceSignalId,
            "AUTO",  // Auto-determine direction
            true     // Include blocked destinations
        )

        isScanning = false

        if (scanResults.success) {
            console.log(" Destination scan completed:",
                "Ready:", scanResults.reachable_clear?.length || 0,
                "Needs PM:", scanResults.reachable_requires_pm?.length || 0,
                "Blocked:", scanResults.blocked?.length || 0)
        } else {
            console.error(" Destination scan failed:", scanResults.error)
        }
    }

    function selectDestination(destData) {
        selectedDestSignalId = destData.dest_signal_id
        console.log(" Selected destination:", destData.dest_signal_id, "reachability:", destData.reachability)
    }

    function isDestinationAssignable() {
        if (!selectedDestSignalId || !scanResults.success) return false

        // Find selected destination in scan results
        var allDestinations = (scanResults.reachable_clear || [])
            .concat(scanResults.reachable_requires_pm || [])

        for (var i = 0; i < allDestinations.length; i++) {
            if (allDestinations[i].dest_signal_id === selectedDestSignalId) {
                var reachability = allDestinations[i].reachability
                return reachability === "REACHABLE_CLEAR" || reachability === "REACHABLE_REQUIRES_PM"
            }
        }

        return false
    }

    function formatPathPreview(pathSummary) {
        if (!pathSummary || !pathSummary.circuits_preview) return ""

        // Handle invalid/blocked paths
        if (pathSummary.hop_count < 0) return "No path available"

        var preview = pathSummary.circuits_preview.join(" → ")
        return preview + " (" + pathSummary.hop_count + " hops)"
    }

    function formatPMActions(pmActions) {
        if (!pmActions || pmActions.length === 0) return ""

        var actions = []
        for (var i = 0; i < pmActions.length; i++) {
            actions.push(pmActions[i].machine_id + "→" + pmActions[i].target_position)
        }
        return "Requires: " + actions.join(", ")
    }

    function getStatusColor(reachability) {
        switch (reachability) {
            case "REACHABLE_CLEAR": return successGreen
            case "REACHABLE_REQUIRES_PM": return warningYellow
            case "BLOCKED": return errorRed
            default: return textMuted
        }
    }

    function getStatusText(reachability) {
        switch (reachability) {
            case "REACHABLE_CLEAR": return "READY"
            case "REACHABLE_REQUIRES_PM": return "NEEDS PM"
            case "BLOCKED": return "BLOCKED"
            default: return "UNKNOWN"
        }
    }

    function submitRouteRequest() {
        if (!globalRouteAssignmentService) {
            console.error(" RouteAssignmentService not available")
            return
        }

        console.log(" Submitting route request:")
        console.log("   From:", sourceSignalId)
        console.log("   To:", selectedDestSignalId)

        var routeId = globalRouteAssignmentService.requestRoute(
            sourceSignalId,
            selectedDestSignalId,
            "UP",        // Default direction
            "operator",  // Default operator
            {},          // Empty train data
            "NORMAL"     // Default priority
        )

        if (routeId && routeId.length > 0) {
            console.log(" Route request submitted successfully. Route ID:", routeId)
            close()
        } else {
            console.error(" Route request failed")
        }
    }
}
