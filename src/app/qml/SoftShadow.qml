import QtQuick

// A soft shadow round a box, put behind it at the same place and size:
// rings of faint colour, each a logical pixel further out, so it fades from
// `color` at the box's edge to nothing `spread` pixels away. Without effects:
// QtQuick.Effects is not deployed.
Item {
    id: shadow

    property real radius: 0 // the box's corners
    property color color: Theme.pieShadow
    property int spread: 6
    property real offsetY: 1 // light from above

    Repeater {
        model: shadow.spread

        delegate: Rectangle {
            required property int index
            readonly property real grow: index + 1

            x: -grow
            y: -grow + shadow.offsetY
            width: shadow.width + 2 * grow
            height: shadow.height + 2 * grow
            radius: shadow.radius > 0 ? shadow.radius + grow : 0
            color: Qt.rgba(shadow.color.r, shadow.color.g, shadow.color.b, shadow.color.a / shadow.spread)
        }
    }
}
