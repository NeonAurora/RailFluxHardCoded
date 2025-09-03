//  IMPROVED: components/SignalContextMenu.qml - White Overlay + No Scroll for Subsidiary
import QtQuick
import QtQuick.Controls

Item {
    id: contextMenu
    anchors.fill: parent
    visible: false
    z: 999

    property string signalId: ""
    property string signalName: ""
    property string signalType: ""
    property string currentAspect: ""
    property var possibleAspects: []
    property string currentCallingOnAspect: ""
    property string currentLoopAspect: ""
    property real menuX: 0
    property real menuY: 0

    //  INCREASED: Height to accommodate all subsidiary options
    readonly property int maxMenuHeight: Math.min(parent.height * 0.9, 480)  // Increased from 400 to 480
    readonly property int menuWidth: 320
    readonly property int headerHeight: 36
    readonly property int itemHeight: 36
    readonly property int compactItemHeight: 30  // Slightly more compact for efficiency
    readonly property int sectionSpacing: 10    // Reduced spacing for efficiency

    //  SOFTER: Muted color scheme
    readonly property string primaryBg: "#1a1a1a"
    readonly property string controlBg: "#2d3748"
    readonly property string accentBlue: "#4299e1"
    readonly property string successGreen: "#48bb78"
    readonly property string warningYellow: "#ed8936"
    readonly property string dangerRed: "#f56565"
    readonly property string textPrimary: "#ffffff"
    readonly property string textSecondary: "#a0aec0"
    readonly property string borderColor: "#4a5568"
    readonly property string subtleBorder: "#2d3748"

    signal aspectSelected(string signalId, string aspectType, string selectedAspect)
    signal closeRequested()

    readonly property bool isHomeSignal: signalType === "HOME"

    //   Height calculation to show all subsidiary options without scrolling
    function calculateRequiredHeight() {
        var requiredHeight = headerHeight + 16  // Header + margins

        var mainSectionHeight = 28 + (possibleAspects.length * itemHeight) + 16  // Title + items + padding

        var subsidiarySectionHeight = 0
        if (isHomeSignal) {
            // Each subsidiary section: 24px header + (2 options * compactItemHeight) + 12px padding
            var singleSubsidiaryHeight = 24 + (2 * compactItemHeight) + 12
            subsidiarySectionHeight = (2 * singleSubsidiaryHeight) + 8  // Two sections + spacing between them
        }

        requiredHeight += Math.max(mainSectionHeight, subsidiarySectionHeight)
        requiredHeight += 16  // Bottom padding

        return Math.min(requiredHeight, maxMenuHeight)
    }

    function show(x, y, sigId, sigName, current, possible, callingOn, loop) {
        signalId = sigId
        signalName = sigName
        currentAspect = current
        possibleAspects = possible || []

        if (arguments.length >= 8) {
            currentCallingOnAspect = callingOn || "OFF"
            currentLoopAspect = loop || "OFF"
            signalType = "HOME"
        } else {
            currentCallingOnAspect = "OFF"
            currentLoopAspect = "OFF"
            signalType = ""
            fetchSignalDetails()
        }

        var calculatedHeight = calculateRequiredHeight()
        menuX = Math.min(x, parent.width - menuWidth - 16)
        menuY = Math.min(y, parent.height - calculatedHeight - 16)

        visible = true
        showAnimation.start()
    }

    function fetchSignalDetails() {
        var dbMgr = null
        var currentParent = parent
        while (currentParent && !dbMgr) {
            if (currentParent.dbManager) {
                dbMgr = currentParent.dbManager
                break
            }
            currentParent = currentParent.parent
        }

        if (!dbMgr || !dbMgr.isConnected) return

        var signalData = dbMgr.getSignalById(signalId)
        if (signalData && Object.keys(signalData).length > 0) {
            signalType = signalData.signal_type || ""
            currentCallingOnAspect = signalData.calling_on_aspect || "OFF"
            currentLoopAspect = signalData.loop_aspect || "OFF"
        }
    }

    function hide() {
        hideAnimation.start()
    }

    // Background overlay
    Rectangle {
        anchors.fill: parent
        color: "transparent"
        MouseArea {
            anchors.fill: parent
            onClicked: contextMenu.hide()
        }
    }

    //  IMPROVED: Main menu container with white overlay
    Rectangle {
        id: menuContainer
        x: menuX
        y: menuY
        width: menuWidth
        height: calculateRequiredHeight()
        color: controlBg
        border.color: borderColor
        border.width: 1
        radius: 6

        //  WHITE OVERLAY: Slight white overlay on background
        Rectangle {
            anchors.fill: parent
            color: "#ffffff"
            opacity: 0.05  // Very subtle white overlay
            radius: parent.radius
        }

        // Subtle shadow
        Rectangle {
            anchors.fill: parent
            anchors.margins: -1
            color: "#15000000"
            radius: parent.radius + 1
            z: -1
        }

        //  Header
        Rectangle {
            id: headerSection
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: parent.top
            anchors.margins: 12
            height: headerHeight
            color: primaryBg
            border.color: subtleBorder
            border.width: 1
            radius: 4

            //  WHITE OVERLAY: On header too
            Rectangle {
                anchors.fill: parent
                color: "#ffffff"
                opacity: 0.03
                radius: parent.radius
            }

            Row {
                anchors.centerIn: parent
                spacing: 8

                Text {
                    text: signalName + " (" + signalId + ")"
                    font.pixelSize: 12
                    font.weight: Font.Bold
                    color: textPrimary
                    anchors.verticalCenter: parent.verticalCenter
                }

                Rectangle {
                    width: 60
                    height: 18
                    color: isHomeSignal ? accentBlue : successGreen
                    radius: 9
                    anchors.verticalCenter: parent.verticalCenter

                    Text {
                        anchors.centerIn: parent
                        text: isHomeSignal ? "HOME" : "SIG"
                        font.pixelSize: 9
                        font.weight: Font.Bold
                        color: "#ffffff"
                    }
                }
            }
        }

        //  SIDE-BY-SIDE: Content layout
        Row {
            id: contentRow
            anchors.left: parent.left
            anchors.right: parent.right
            anchors.top: headerSection.bottom
            anchors.bottom: parent.bottom
            anchors.margins: 12
            anchors.topMargin: 6
            spacing: 12

            //  MAIN SIGNAL SECTION (Left side)
            MinimalSignalSection {
                id: mainSection
                width: isHomeSignal ? parent.width * 0.6 : parent.width
                height: parent.height
                sectionTitle: "MAIN"
                titleColor: dangerRed
                currentAspect: contextMenu.currentAspect
                possibleAspects: contextMenu.possibleAspects
                aspectType: "MAIN"
                itemHeight: contextMenu.itemHeight
                showScrollbar: possibleAspects.length > 4  // Only show scrollbar if many options

                onAspectClicked: function(aspectType, selectedAspect) {
                    contextMenu.aspectSelected(signalId, aspectType, selectedAspect)
                    contextMenu.hide()
                }
            }

            //  EFFICIENT: Subsidiary signals with optimized spacing
            Column {
                width: parent.width * 0.4 - 6
                height: parent.height
                spacing: 6  // Reduced spacing for efficiency
                visible: isHomeSignal

                //  CALLING-ON SIGNAL (no scrolling needed)
                MinimalSignalSection {
                    width: parent.width
                    height: (parent.height - parent.spacing) / 2
                    sectionTitle: "CALL"
                    titleColor: contextMenu.textPrimary
                    currentAspect: contextMenu.currentCallingOnAspect
                    possibleAspects: ["WHITE", "OFF"]
                    aspectType: "CALLING_ON"
                    itemHeight: contextMenu.compactItemHeight
                    showScrollbar: false  // Never needs scrolling (only 2 options)
                    isCompact: true

                    onAspectClicked: function(aspectType, selectedAspect) {
                        contextMenu.aspectSelected(signalId, aspectType, selectedAspect)
                        contextMenu.hide()
                    }
                }

                //  LOOP SIGNAL (no scrolling needed)
                MinimalSignalSection {
                    width: parent.width
                    height: (parent.height - parent.spacing) / 2
                    sectionTitle: "LOOP"
                    titleColor: contextMenu.warningYellow
                    currentAspect: contextMenu.currentLoopAspect
                    possibleAspects: ["YELLOW", "OFF"]
                    aspectType: "LOOP"
                    itemHeight: contextMenu.compactItemHeight
                    showScrollbar: false  // Never needs scrolling (only 2 options)
                    isCompact: true

                    onAspectClicked: function(aspectType, selectedAspect) {
                        contextMenu.aspectSelected(signalId, aspectType, selectedAspect)
                        contextMenu.hide()
                    }
                }
            }
        }
    }

    //  OPTIMIZED: Signal section component with efficient spacing
    component MinimalSignalSection: Rectangle {
        id: section
        color: "#242b36"
        border.color: Qt.rgba(0.5, 0.5, 0.5, 0.3)
        border.width: 1
        radius: 4

        property string sectionTitle: ""
        property string titleColor: contextMenu.dangerRed
        property string currentAspect: ""
        property var possibleAspects: []
        property string aspectType: ""
        property int itemHeight: 36
        property bool showScrollbar: true
        property bool isCompact: false

        signal aspectClicked(string aspectType, string selectedAspect)

        //  WHITE OVERLAY: On sections too
        Rectangle {
            anchors.fill: parent
            color: "#ffffff"
            opacity: 0.02
            radius: parent.radius
        }

        Column {
            anchors.fill: parent
            anchors.margins: isCompact ? 6 : 8  // Reduced margins for compact sections

            //  COMPACT: Section header
            Rectangle {
                width: parent.width
                height: isCompact ? 18 : 20  // Smaller header for compact sections
                color: "transparent"
                border.color: Qt.rgba(0.4, 0.4, 0.4, 0.4)
                border.width: 1
                radius: 2

                Row {
                    anchors.centerIn: parent
                    spacing: 4  // Reduced spacing

                    Text {
                        text: sectionTitle
                        font.pixelSize: isCompact ? 9 : 10
                        font.weight: Font.Bold
                        color: titleColor
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Rectangle {
                        width: 4
                        height: 4
                        radius: 2
                        color: contextMenu.getAspectColor(currentAspect)
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Text {
                        text: currentAspect
                        font.pixelSize: isCompact ? 8 : 9
                        color: contextMenu.textSecondary
                        anchors.verticalCenter: parent.verticalCenter
                    }
                }
            }

            //  EFFICIENT: Content area - scrollable only when needed
            ScrollView {
                width: parent.width
                height: parent.height - (isCompact ? 22 : 28)  // Adjusted for compact header
                clip: true

                ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                ScrollBar.vertical.policy: showScrollbar ? ScrollBar.AsNeeded : ScrollBar.AlwaysOff

                Column {
                    width: parent.width
                    spacing: isCompact ? 1 : 2  // Tighter spacing for compact sections

                    Repeater {
                        model: possibleAspects

                        Rectangle {
                            id: aspectItem
                            width: parent.width
                            height: section.itemHeight
                            color: aspectMouseArea.containsMouse ?
                                   (isCurrent ? "#3a4553" : Qt.rgba(0.25, 0.57, 0.88, 0.7)) :
                                   (isCurrent ? "#323944" : "transparent")
                            border.color: isCurrent ? Qt.rgba(0.63, 0.68, 0.75, 0.5) : "transparent"
                            border.width: 1
                            radius: 3

                            property string aspectName: modelData
                            property bool isCurrent: aspectName === currentAspect

                            Row {
                                anchors.left: parent.left
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.leftMargin: isCompact ? 6 : 8
                                spacing: isCompact ? 6 : 8

                                //  Aspect indicator
                                Rectangle {
                                    width: isCompact ? 10 : 12
                                    height: isCompact ? 10 : 12
                                    radius: 1
                                    color: "transparent"
                                    border.color: contextMenu.getAspectColor(aspectItem.aspectName)
                                    border.width: aspectItem.isCurrent ? 3 : 2
                                    anchors.verticalCenter: parent.verticalCenter

                                    Rectangle {
                                        anchors.centerIn: parent
                                        width: isCompact ? 4 : 6
                                        height: isCompact ? 4 : 6
                                        radius: 1
                                        color: aspectItem.isCurrent ?
                                               contextMenu.getAspectColor(aspectItem.aspectName) : "transparent"
                                    }
                                }

                                Text {
                                    text: aspectItem.aspectName
                                    font.pixelSize: isCompact ? 10 : 11
                                    font.weight: aspectItem.isCurrent ? Font.Normal : Font.Bold
                                    color: aspectItem.isCurrent ? contextMenu.textSecondary : contextMenu.textPrimary
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            MouseArea {
                                id: aspectMouseArea
                                anchors.fill: parent
                                hoverEnabled: true
                                cursorShape: aspectItem.isCurrent ? Qt.ArrowCursor : Qt.PointingHandCursor
                                enabled: !aspectItem.isCurrent

                                onClicked: {
                                    if (!aspectItem.isCurrent) {
                                        section.aspectClicked(aspectType, aspectItem.aspectName)
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    // Animations
    NumberAnimation {
        id: showAnimation
        target: menuContainer
        property: "scale"
        from: 0.9
        to: 1.0
        duration: 150
        easing.type: Easing.OutQuad
    }

    SequentialAnimation {
        id: hideAnimation
        NumberAnimation {
            target: menuContainer
            property: "scale"
            from: 1.0
            to: 0.9
            duration: 100
            easing.type: Easing.InQuad
        }
        ScriptAction {
            script: {
                contextMenu.visible = false
                contextMenu.closeRequested()
            }
        }
    }

    function getAspectColor(aspect) {
        switch(aspect) {
            case "RED": return "#f56565"
            case "YELLOW": return "#ed8936"
            case "SINGLE_YELLOW": return "#ed8936"
            case "DOUBLE_YELLOW": return "#d69e2e"
            case "GREEN": return "#48bb78"
            case "WHITE": return "#e2e8f0"
            case "BLUE": return "#4299e1"
            case "OFF": return "#718096"
            default: return "#718096"
        }
    }
}
