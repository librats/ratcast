pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Shapes
import "logo.js" as Logo

// ratcast's rat, as the logo draws it (docs/logo-dark.png, traced into
// logo.js by trace_logo.py), alive: it comes together part by part, the play
// button on its screen beats and sends the signal up in waves, the camera's
// light blinks, the rat blinks and twitches its whiskers, the viewers round it
// speak in turn (ringed, as the room rings who speaks), and a light runs along
// their orbit. The mouse over it moves the parts apart, the nearer more; a
// click (a tap) makes the rat hop. Still while not running: nothing is drawn
// anew for it then.
Item {
    id: logo

    /// Moving: off where it is not seen.
    property bool running: visible
    /// The drawing's own colour, and the signal's two.
    property color ink: "#e6edf3"
    property color violet: "#8b5cf6"
    property color cyan: "#22d3ee"
    property color speaking: "#4caf50"

    implicitWidth: Logo.width / 3
    implicitHeight: Logo.height / 3

    /// The drawing into the item, its shape kept.
    readonly property real fit: Math.min(width / Logo.width, height / Logo.height)

    /// One round of the signal, 0 to 1: the play button beats at its start,
    /// the arcs light from the inside out after it.
    property real wave: 0
    /// Coming together, 0 to 1, once each time it starts running.
    property real entrance: 0
    /// The light along the orbit, across the drawing.
    property real sweep: -400
    /// Who of the viewers speaks: 0 to 2, or none (-1).
    property int speaker: -1
    property real lid: 0
    property real whiskers: 0
    property real hop: 0
    property real led: 0
    /// The mouse, -1 to 1 across and down the drawing; 0 without one.
    property real lookX: hover.hovered ? (hover.point.position.x / Math.max(1, width)) * 2 - 1 : 0
    property real lookY: hover.hovered ? (hover.point.position.y / Math.max(1, height)) * 2 - 1 : 0
    Behavior on lookX {
        SmoothedAnimation {
            velocity: 2
        }
    }
    Behavior on lookY {
        SmoothedAnimation {
            velocity: 2
        }
    }

    function clamp(v) {
        return Math.max(0, Math.min(1, v))
    }
    /// How lit something is that peaks at `at` of the wave, `width` either side.
    function pulse(at, width) {
        return clamp(1 - Math.abs(logo.wave - at) / width)
    }
    /// Its part of the entrance, from `from` for `length`, eased past the end
    /// and back (OutBack).
    function arrive(from, length) {
        const t = clamp((logo.entrance - from) / length) - 1
        const s = 1.6
        return 1 + (s + 1) * t * t * t + s * t * t
    }
    function centreX(box) {
        return (box[0] + box[2]) / 2
    }
    function centreY(box) {
        return (box[1] + box[3]) / 2
    }

    onRunningChanged: if (running) entranceAnimation.restart()
    Component.onCompleted: if (running) entranceAnimation.restart()

    NumberAnimation {
        id: entranceAnimation
        target: logo
        property: "entrance"
        from: 0
        to: 1
        duration: 1600
    }
    NumberAnimation on wave {
        running: logo.running
        from: 0
        to: 1
        duration: 2600
        loops: Animation.Infinite
    }
    SequentialAnimation on sweep {
        running: logo.running
        loops: Animation.Infinite
        NumberAnimation {
            from: -400
            to: Logo.width + 400
            duration: 2800
            easing.type: Easing.InOutSine
        }
        PauseAnimation {
            duration: 2200
        }
    }
    SequentialAnimation on led {
        running: logo.running
        loops: Animation.Infinite
        NumberAnimation {
            to: 1
            duration: 500
            easing.type: Easing.OutQuad
        }
        PauseAnimation {
            duration: 400
        }
        NumberAnimation {
            to: 0
            duration: 700
            easing.type: Easing.InQuad
        }
        PauseAnimation {
            duration: 600
        }
    }

    // The viewers speak one after another, now and then nobody.
    Timer {
        running: logo.running
        repeat: true
        interval: 1500
        onTriggered: logo.speaker = logo.speaker >= 2 ? (Math.random() < 0.3 ? -1 : 0)
                                                      : logo.speaker + 1
    }

    // A blink every few seconds, now and then two.
    Timer {
        id: blinkTimer
        running: logo.running
        interval: 3000
        onTriggered: {
            blink.loops = Math.random() < 0.25 ? 2 : 1
            blink.restart()
            interval = 2500 + Math.random() * 3500
            restart()
        }
    }
    SequentialAnimation {
        id: blink
        NumberAnimation {
            target: logo
            property: "lid"
            to: 1
            duration: 70
            easing.type: Easing.InQuad
        }
        NumberAnimation {
            target: logo
            property: "lid"
            to: 0
            duration: 120
            easing.type: Easing.OutQuad
        }
    }

    Timer {
        running: logo.running
        interval: 2000
        onTriggered: {
            twitch.restart()
            interval = 1800 + Math.random() * 3200
            restart()
        }
    }
    SequentialAnimation {
        id: twitch
        NumberAnimation {
            target: logo
            property: "whiskers"
            to: 8
            duration: 70
        }
        NumberAnimation {
            target: logo
            property: "whiskers"
            to: -5
            duration: 90
        }
        NumberAnimation {
            target: logo
            property: "whiskers"
            to: 3
            duration: 80
        }
        NumberAnimation {
            target: logo
            property: "whiskers"
            to: 0
            duration: 140
            easing.type: Easing.OutQuad
        }
    }

    SequentialAnimation {
        id: hopping
        NumberAnimation {
            target: logo
            property: "hop"
            to: -70
            duration: 170
            easing.type: Easing.OutQuad
        }
        NumberAnimation {
            target: logo
            property: "hop"
            to: 0
            duration: 520
            easing.type: Easing.OutBounce
        }
    }

    HoverHandler {
        id: hover
    }
    TapHandler {
        onTapped: {
            hopping.restart()
            twitch.restart()
            blink.loops = 1
            blink.restart()
        }
    }

    // One part of the drawing: a shape in the drawing's coordinates.
    component Part: Shape {
        id: part
        property string path
        property color color: logo.ink
        property ShapeGradient gradient: null
        width: Logo.width
        height: Logo.height
        preferredRendererType: Shape.CurveRenderer

        ShapePath {
            strokeWidth: -1
            strokeColor: "transparent"
            fillColor: part.color
            fillGradient: part.gradient
            fillRule: ShapePath.OddEvenFill
            PathSvg {
                path: part.path
            }
        }
    }

    // A layer of parts, moved with the mouse as far as it is near.
    component Layer: Item {
        property real depth
        width: Logo.width
        height: Logo.height
        x: logo.lookX * depth * 26
        y: logo.lookY * depth * 16
    }

    // The signal's colours, the arcs' and the play button's.
    LinearGradient {
        id: signalColours
        x1: 350
        y1: 0
        x2: 870
        y2: 480
        GradientStop {
            position: 0
            color: logo.violet
        }
        GradientStop {
            position: 1
            color: logo.cyan
        }
    }
    // The orbit: the drawing's colour, a light running along it.
    LinearGradient {
        id: orbitColours
        x1: logo.sweep - 260
        y1: 0
        x2: logo.sweep + 260
        y2: 60
        GradientStop {
            position: 0
            color: logo.ink
        }
        GradientStop {
            position: 0.4
            color: logo.violet
        }
        GradientStop {
            position: 0.6
            color: logo.cyan
        }
        GradientStop {
            position: 1
            color: logo.ink
        }
    }

    Item {
        id: canvas
        width: Logo.width
        height: Logo.height
        x: (logo.width - Logo.width * logo.fit) / 2
        y: (logo.height - Logo.height * logo.fit) / 2
        scale: logo.fit
        transformOrigin: Item.TopLeft
        opacity: logo.clamp(logo.entrance * 4)

        // A glow behind it all, breathing with the signal.
        Shape {
            width: Logo.width
            height: Logo.height
            preferredRendererType: Shape.CurveRenderer
            opacity: 0.55 + 0.45 * logo.pulse(0.05, 0.3)

            ShapePath {
                strokeWidth: -1
                strokeColor: "transparent"
                fillGradient: RadialGradient {
                    centerX: 540
                    centerY: 380
                    centerRadius: 520
                    focalX: centerX
                    focalY: centerY
                    GradientStop {
                        position: 0
                        color: Qt.rgba(logo.violet.r, logo.violet.g, logo.violet.b, 0.22)
                    }
                    GradientStop {
                        position: 0.5
                        color: Qt.rgba(logo.cyan.r, logo.cyan.g, logo.cyan.b, 0.07)
                    }
                    GradientStop {
                        position: 1
                        color: "transparent"
                    }
                }
                PathAngleArc {
                    centerX: 540
                    centerY: 380
                    radiusX: 520
                    radiusY: 520
                    sweepAngle: 360
                }
            }
        }

        Layer {
            depth: 0.15

            Part {
                path: Logo.ringBack
                gradient: orbitColours
                opacity: logo.arrive(0.35, 0.4)
            }
        }

        // The screen: the camera's light, the play button beating.
        Layer {
            depth: 0.3
            opacity: logo.clamp((logo.entrance - 0.1) / 0.3)

            Part {
                path: Logo.monitor
            }
            Part {
                path: Logo.lens
                color: Qt.tint(logo.ink, Qt.rgba(0.94, 0.27, 0.27, logo.led))
            }
            Part {
                id: play
                readonly property real beat: logo.pulse(0.04, 0.1)
                path: Logo.play
                gradient: signalColours
                transform: Scale {
                    origin.x: logo.centreX(Logo.playBox)
                    origin.y: logo.centreY(Logo.playBox)
                    xScale: logo.arrive(0.3, 0.4) * (1 + 0.14 * play.beat)
                    yScale: xScale
                }
            }
        }

        // The signal, from the inside out, each arc lit as the wave passes.
        Layer {
            depth: 0.75

            Repeater {
                model: [Logo.wifiInner, Logo.wifiMiddle, Logo.wifiOuter]

                Part {
                    id: arc
                    required property string modelData
                    required property int index
                    readonly property real lit: logo.pulse(0.18 + 0.13 * index, 0.16)
                    path: modelData
                    gradient: signalColours
                    opacity: logo.clamp((logo.entrance - 0.55 - 0.1 * index) / 0.2)
                             * (0.35 + 0.65 * lit)
                    transform: Scale {
                        origin.x: logo.centreX(Logo.wifiInnerBox)
                        origin.y: Logo.wifiInnerBox[3] + 40
                        xScale: 1 + 0.05 * arc.lit
                        yScale: xScale
                    }
                }
            }
        }

        // The rat, rising into place; it hops when clicked.
        Layer {
            depth: 0.55

            Item {
                width: Logo.width
                height: Logo.height
                y: (1 - logo.arrive(0.05, 0.5)) * 90 + logo.hop
                opacity: logo.clamp((logo.entrance - 0.05) / 0.25)

                Part {
                    path: Logo.rat
                }
                // Its eyelid: the eye filled from the top down.
                Part {
                    path: Logo.eye
                    transform: Scale {
                        origin.y: Logo.eyeBox[1]
                        yScale: logo.lid
                    }
                }
                Item {
                    width: Logo.width
                    height: Logo.height
                    transform: Rotation {
                        origin.x: Logo.whisker1Box[0] - 6
                        origin.y: logo.centreY(Logo.whisker2Box)
                        angle: logo.whiskers
                    }

                    Part {
                        path: Logo.whisker1
                    }
                    Part {
                        path: Logo.whisker2
                    }
                    Part {
                        path: Logo.whisker3
                    }
                }
            }
        }

        // The orbit before the rat, and the viewers on it.
        Layer {
            depth: 1

            Part {
                path: Logo.ringFront
                gradient: orbitColours
                opacity: logo.arrive(0.35, 0.4)
            }
            Part {
                path: Logo.ringRight
                gradient: orbitColours
                opacity: logo.arrive(0.4, 0.4)
            }
            Part {
                path: Logo.ringEnd
                gradient: orbitColours
                opacity: logo.arrive(0.45, 0.4)
            }

            Repeater {
                // Each viewer: its shape, and its round face's centre and radius
                // (the left one's box has its speech bubble's tail too).
                model: [
                    { path: Logo.avatarLeft, x: 66.5, y: 477.5, r: 61 },
                    { path: Logo.avatarBottom, x: 449.5, y: 629.5, r: 63 },
                    { path: Logo.avatarRight, x: 923.5, y: 586.5, r: 61 }
                ]

                Item {
                    id: viewer
                    required property var modelData
                    required property int index
                    readonly property bool speaking: logo.speaker === index
                    /// Raised while it speaks.
                    property real lift: speaking ? 1 : 0
                    Behavior on lift {
                        NumberAnimation {
                            duration: 220
                            easing.type: Easing.OutBack
                        }
                    }
                    width: Logo.width
                    height: Logo.height
                    transform: Scale {
                        origin.x: viewer.modelData.x
                        origin.y: viewer.modelData.y
                        xScale: logo.arrive(0.6 + 0.12 * viewer.index, 0.35) * (1 + 0.07 * viewer.lift)
                        yScale: xScale
                    }

                    Part {
                        path: viewer.modelData.path
                    }
                    // Ringed in the gap round it, as the room rings who speaks.
                    Rectangle {
                        x: viewer.modelData.x - width / 2
                        y: viewer.modelData.y - height / 2
                        width: (viewer.modelData.r + 7) * 2
                        height: width
                        radius: width / 2
                        color: "transparent"
                        border.color: logo.speaking
                        border.width: 6
                        antialiasing: true
                        opacity: viewer.lift
                    }
                }
            }
        }
    }
}
