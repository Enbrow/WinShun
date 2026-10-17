import QtQuick

// Round the resting row and its scroll displacement separately. Rounding
// their sum lets an endpoint at half a device pixel change sides only when
// the wheel animation finally reaches it, after a visibly motionless tail.
Translate {
    required property Flickable view
    required property real viewY
    required property real itemY
    property real pixelBias: 0 // text with half-pixel local offsets needs a fixed rounding side

    readonly property real dpr: view.Screen.devicePixelRatio
    readonly property real restingY: viewY + view.topMargin - view.originY + itemY
    readonly property real displacement: view.contentItem.y - view.topMargin + view.originY

    y: (Math.round(restingY * dpr) + Math.round(displacement * dpr) + pixelBias) / dpr
       - restingY - displacement
}
