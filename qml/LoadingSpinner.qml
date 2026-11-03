import QtQuick

Image {
    id: spinner
    property real angle: 0
    width: 48; height: 48
    source: "qrc:/icons/lucide/loader-circle.svg"
    sourceSize.width: 48; sourceSize.height: 48
    fillMode: Image.PreserveAspectFit
    rotation: angle
    NumberAnimation on angle {
        from: 0; to: 360; duration: 1000; loops: Animation.Infinite
        running: spinner.visible
    }
    Accessible.name: "読み込み中"
}
