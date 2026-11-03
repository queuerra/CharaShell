import QtQuick
import QtQuick.Controls
import CharaShell.Rig 1.0

Item {
    id: root
    property real outputLevel: 0
    property bool thinking: false
    property string psdPath: ""
    property bool loadAvatar: true
    property real mouthOpen: Math.min(1, Math.max(0, (outputLevel - 0.004) * 14))
    property real diagnosticMouthOpen: -1
    property real diagnosticBlinkAmount: -1
    readonly property real renderedMouthOpen: rigLoader.item ? rigLoader.item.renderedMouthOpen : 0
    readonly property bool ready: rigLoader.item !== null && rigLoader.item.ready
    readonly property bool loading: loadAvatar && (rigLoader.status === Loader.Loading || (rigLoader.item !== null && rigLoader.item.loading))
    readonly property string errorString: rigLoader.item ? rigLoader.item.errorString : ""
    readonly property bool failed: loadAvatar && !loading && errorString.length > 0
    Rectangle { anchors.fill: parent; color: "#e1e6da" }
    Image {
        objectName: "avatarBackground"
        anchors.fill: parent
        source: "qrc:/backgrounds/room-daylight.png"
        fillMode: Image.PreserveAspectCrop
        asynchronous: true
    }
    Loader {
        id: rigLoader
        anchors.fill: parent
        active: root.loadAvatar
        asynchronous: true
        // Overlap the lower cinema band so the animated torso has no visible seam.
        transform: Translate { y: Math.max(8, Math.min(24, root.height * 0.04)) }
        sourceComponent: NativeRigItem {
            psdPath: root.psdPath
            mouthOpen: root.diagnosticMouthOpen >= 0 ? root.diagnosticMouthOpen : root.mouthOpen
            blinkAmount: root.diagnosticBlinkAmount
        }
    }
    AnimatedSprite {
        id: thinkingBubble
        objectName: "system2ThinkingBubble"
        // Match the square canvas fitted by NativeRigItem, including wide windows.
        readonly property real canvasSize: Math.min(root.width, root.height)
        width: Math.max(96, Math.min(176, canvasSize * 0.23))
        height: width
        x: Math.min(root.width - width - 16, (root.width - canvasSize) / 2 + canvasSize * 0.71)
        y: Math.max(48, (root.height - canvasSize) / 2 + canvasSize * 0.12)
        source: "qrc:/thinking/sprite-sheet.png"
        frameWidth: 272
        frameHeight: 272
        frameCount: 8
        frameRate: 5
        interpolate: false
        loops: AnimatedSprite.Infinite
        visible: root.thinking && root.ready
        running: visible
        onVisibleChanged: if (visible) restart()
        Accessible.role: Accessible.Indicator
        Accessible.name: "回答を考えています"
    }
    LoadingSpinner {
        anchors.centerIn: parent
        visible: root.loading
    }
    Label {
        anchors.centerIn: parent
        width: parent.width - 24
        visible: root.failed
        text: root.errorString
        wrapMode: Text.Wrap
        horizontalAlignment: Text.AlignHCenter
        color: "#8b294b"
    }
}
