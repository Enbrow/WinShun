import QtQuick

// One timer per window; short Ctrl gestures and other chords must not flash the rows.
Item {
    id: hints

    property string mode: "held"
    property int key: -1
    property bool listening: false
    property bool held: false
    property bool ready: false
    readonly property bool shown: key >= 0 && (mode === "always" || (mode === "held" && listening && ready))
    property real hintOpacity: shown ? 1 : 0

    Behavior on hintOpacity { NumberAnimation { duration: hints.shown ? 80 : 60 } }

    function reset() {
        delay.stop()
        held = false
        ready = false
    }
    function press(event) {
        if (!listening || mode !== "held" || key < 0 || event.isAutoRepeat)
            return
        if (event.key !== key) {
            reset()
            return
        }
        const modifier = key === Qt.Key_Control ? Qt.ControlModifier : Qt.AltModifier
        if ((event.modifiers & ~modifier) !== 0)
            return
        if (!held) {
            held = true
            delay.start()
        }
    }
    function release(event) {
        if (event.key === key && !event.isAutoRepeat)
            reset()
    }

    onListeningChanged: reset()
    onModeChanged: reset()
    onKeyChanged: reset()

    Timer {
        id: delay
        interval: 180
        onTriggered: hints.ready = hints.held && hints.listening
    }
}
