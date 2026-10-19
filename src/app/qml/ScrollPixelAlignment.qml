import QtQuick

// Round the resting row and its scroll displacement separately. Rounding
// their sum lets an endpoint at half a device pixel change sides only when
// the wheel animation finally reaches it, after a visibly motionless tail.
// The view rests at its top, or at its bottom when it is a list laid out
// bottom to top (ListWheelScroll.restY): its wheel stops are whole device
// pixels from there.
Translate {
    required property Flickable view
    required property real viewY
    required property real itemY
    property real pixelBias: 0 // text with half-pixel local offsets needs a fixed rounding side

    readonly property real dpr: view.Screen.devicePixelRatio
    readonly property bool bottomUp: view instanceof ListView
                                     && (view as ListView).verticalLayoutDirection === ListView.BottomToTop
    readonly property real restY: bottomUp ? view.originY + view.contentHeight + view.bottomMargin - view.height
                                           : view.originY - view.topMargin
    readonly property real restingY: viewY + itemY - restY
    readonly property real displacement: view.contentItem.y + restY

    y: (Math.round(restingY * dpr) + Math.round(displacement * dpr) + pixelBias) / dpr
       - restingY - displacement
}
