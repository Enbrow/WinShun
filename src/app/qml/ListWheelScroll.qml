import QtQuick

// Plan an aligned destination before moving. Flickable's wheel animation
// rounds contentY again in its final fixup; at fractional DPI that can move
// a row backwards after it has visibly stopped. Never feed rounded frames
// back into a native flick: only discrete wheel events use this animation.
Item {
    id: scroll

    required property Flickable view
    property bool animate: true
    property int scrollLines: Application.styleHints.wheelScrollLines
    // Variable-height ListView reflows round contentY during layout. Giving
    // those views integral coordinates leaves no layout fixup to interrupt
    // the glide; ScrollPixelAlignment still aligns the rendered rows.
    property bool logicalPixelSteps: false
    readonly property bool running: glide.running
    // A list laid out bottom to top (ListView.BottomToTop: the clipboard
    // turned over) has its first row at the bottom and rests there; content
    // shorter than the view sits there too. Else all of it is at the top.
    readonly property bool bottomUp: view instanceof ListView
                                     && (view as ListView).verticalLayoutDirection === ListView.BottomToTop
    readonly property real topY: view.originY - view.topMargin
    readonly property real bottomY: view.originY + view.contentHeight + view.bottomMargin - view.height
    readonly property real minimumY: bottomUp ? Math.min(topY, bottomY) : topY
    readonly property real maximumY: bottomUp ? bottomY : Math.max(topY, bottomY)
    // Where it rests: the device-pixel grid is counted from there, as
    // ScrollPixelAlignment counts the rows' displacement.
    readonly property real restY: bottomUp ? maximumY : minimumY
    readonly property real dpr: view.Screen.devicePixelRatio
    property real requestedY: 0
    property real position: 0
    property int direction: 0
    property int writeDepth: 0
    readonly property bool writing: writeDepth > 0
    property bool boundsMoved: false

    function cancel() {
        glide.stop()
        direction = 0
    }

    function bounded(y) {
        return Math.max(minimumY, Math.min(maximumY, y))
    }

    function aligned(y) {
        if (logicalPixelSteps)
            return bounded(Math.round(y))
        return bounded(restY + Math.round((y - restY) * dpr) / dpr)
    }

    function moveTo(y) {
        glide.stop()
        const destination = aligned(y)
        if (Math.abs(destination - view.contentY) < 0.000001)
            return
        if (!animate) {
            writePosition(destination)
            // No glide follows an end the write moved: the rows it laid out
            // change the estimate. Go to the end again until it stays put.
            for (let i = 0; i < 8 && !isFinite(y); ++i) {
                const settled = aligned(y)
                if (Math.abs(settled - view.contentY) < 0.000001)
                    break
                writePosition(settled)
            }
            return
        }
        glide.from = view.contentY
        glide.to = destination
        glide.start()
    }

    function wheel(angle) {
        const lines = scrollLines
        if (lines === 0) {
            cancel()
            return
        }
        const step = -angle / 120 * (lines > 0 ? lines * 24 : view.height)
        const nextDirection = Math.sign(step)
        // Accumulate a burst, but discard its untravelled distance on reversal.
        // Keep the fractional remainder after arrival too: tiny deltas must
        // accumulate even when animations are disabled or events are sparse.
        const continuing = direction === nextDirection
            && (running || Math.abs(view.contentY - aligned(requestedY)) < 0.000001)
        const base = continuing ? requestedY : view.contentY
        cancel()
        view.cancelFlick()
        direction = nextDirection
        const goal = base + step
        // Keep an endpoint's intent when variable-height rows change the
        // view's estimate of the range during the glide.
        requestedY = goal <= minimumY ? -Infinity : goal >= maximumY ? Infinity : goal
        moveTo(requestedY)
    }

    function writePosition(y) {
        // Layout answers this assignment and can change the range several
        // times inside it: variable-height rows are released before new ones
        // are made, so an end can dip past the glide and come straight back.
        // Judge the range once it has settled, after the outermost write.
        writeDepth++
        view.contentY = logicalPixelSteps ? bounded(Math.round(y)) : y
        writeDepth--
        if (!writing && boundsMoved) {
            boundsMoved = false
            boundsChanged()
        }
    }
    onPositionChanged: if (glide.running) writePosition(position)
    // A resized view can move the endpoint while a wheel glide is running.
    // Retarget that glide now, never correct the position after it finishes.
    function boundsChanged() {
        if (!running)
            return
        if (writing) {
            boundsMoved = true
            return
        }
        const destination = aligned(requestedY)
        if (Math.abs(destination - glide.to) < 0.000001)
            return
        // A newly estimated end must not pull a running glide backwards.
        if ((destination - view.contentY) * direction < 0)
            cancel()
        else
            moveTo(requestedY)
    }
    // Rows added or removed above (bottom up: below) shift the content under
    // a glide, and its target no longer means anything. An end is still an end.
    function restMoved() {
        if (running && !writing && isFinite(requestedY))
            cancel()
        else
            boundsChanged()
    }
    onMinimumYChanged: bottomUp ? boundsChanged() : restMoved()
    onMaximumYChanged: bottomUp ? restMoved() : boundsChanged()
    onDprChanged: cancel()

    WheelHandler {
        parent: scroll.view
        target: null
        // Qt also rejects synthesized touchpad events before emitting wheel.
        // Setting WheelEvent.accepted in this signal does not forward events.
        // angleDelta already includes the OS's natural-scrolling direction.
        acceptedDevices: PointerDevice.Mouse
        enabled: scroll.view.interactive
        onWheel: event => {
            if (event.angleDelta.y)
                scroll.wheel(event.angleDelta.y)
        }
    }

    NumberAnimation {
        id: glide
        target: scroll
        property: "position"
        duration: 200
        easing.type: Easing.OutCubic
    }

    Connections {
        target: scroll.view
        function onContentYChanged() { if (!scroll.writing) scroll.cancel() }
        function onDraggingChanged() { if (scroll.view.dragging) scroll.cancel() }
        function onMovementStarted() { scroll.cancel() }
        function onVisibleChanged() { if (!scroll.view.visible) scroll.cancel() }
        function onInteractiveChanged() { if (!scroll.view.interactive) scroll.cancel() }
    }
    Connections {
        target: scroll.view instanceof ListView ? scroll.view : null
        function onCurrentIndexChanged() { scroll.cancel() }
    }
    Connections {
        target: scroll.view.Window.window
        function onVisibleChanged() { if (!target.visible) scroll.cancel() }
    }
}
