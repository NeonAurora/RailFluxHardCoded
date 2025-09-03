import QtQuick

Rectangle {
    id: signalDot
    width: 16
    height: 16
    radius: 8
    
    // === PROPERTIES ===
    property string signalId
    property string signalRole: "SOURCE"  // "SOURCE" or "DESTINATION"
    property color routeColor: "#FFD700"
    property bool isActive: false
    
    // === VISUAL PROPERTIES ===
    color: getSignalColor()
    border.color: getBorderColor()
    border.width: 2
    
    function getSignalColor() {
        if (signalRole === "SOURCE") {
            return isActive ? routeColor : Qt.lighter(routeColor, 1.3)
        } else {
            return isActive ? "#FF4500" : "#FF8C00"  // OrangeRed / DarkOrange
        }
    }
    
    function getBorderColor() {
        return isActive ? "#ffffff" : "#000000"
    }
    
    // === CENTER INDICATOR ===
    Rectangle {
        anchors.centerIn: parent
        width: 6
        height: 6
        radius: 3
        color: signalRole === "SOURCE" ? "#ffffff" : "#000000"
        opacity: 0.8
    }
    
    // === ROLE INDICATOR ===
    Text {
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.bottom
        anchors.topMargin: 2
        text: signalRole === "SOURCE" ? "S" : "D"
        color: routeColor
        font.pixelSize: 8
        font.weight: Font.Bold
        horizontalAlignment: Text.AlignHCenter
    }
    
    // === PULSING ANIMATION FOR ACTIVE ROUTES ===
    SequentialAnimation on scale {
        running: isActive
        loops: Animation.Infinite
        NumberAnimation { to: 1.2; duration: 800; easing.type: Easing.InOutQuad }
        NumberAnimation { to: 1.0; duration: 800; easing.type: Easing.InOutQuad }
    }
    
    // === MOUSE INTERACTION ===
    MouseArea {
        anchors.fill: parent
        hoverEnabled: true
        
        ToolTip.visible: containsMouse
        ToolTip.text: signalRole + " Signal: " + signalId + "\n" +
                     "Role: " + (signalRole === "SOURCE" ? "Route Origin" : "Route Destination") + "\n" +
                     "Status: " + (isActive ? "ACTIVE" : "RESERVED")
        
        onClicked: {
            console.log(" Route signal dot clicked:", signalRole, signalId)
            // Could trigger signal inspection or route details
        }
    }
    
    // === PROPERTY CHANGE HANDLERS ===
    onIsActiveChanged: {
        color = getSignalColor()
        border.color = getBorderColor()
    }
    
    onRouteColorChanged: {
        color = getSignalColor()
    }
    
    Component.onCompleted: {
        console.log(" RouteSignalDot created:", signalRole, signalId)
    }
}
