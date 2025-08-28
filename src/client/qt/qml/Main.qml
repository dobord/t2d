// SPDX-License-Identifier: Apache-2.0
import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15
import QtQuick.Layouts 1.15
import "." // ensure local QML (Joystick, CustomButton)

Window {
    id: root
    width: 800
    height: 600
    visible: true
    title: "t2d Qt Client"
    Component.onCompleted: root.requestActivate()

    // Removed Keys attached handlers on Window (not an Item) to avoid runtime warning.

    Item { // focusable container for key handling
        id: rootItem
        objectName: "rootItem"
        anchors.fill: parent
        focus: true
        // FPS tracking
        property bool showFps: true
        property int _fpsFrameCount: 0
        property double _fpsLastMs: 0
        property double fpsValue: 0.0
        property bool followCamera: true // default on: camera follows own tank
        property bool showGrid: true
        // Map dimensions (static per match, received from snapshot). When zero, unknown/not yet received.
        property real mapWidth: 0
        property real mapHeight: 0
        // Mouse / camera control state
        property bool mouseAimEnabled: true
        property real userZoom: 1.0
        property real cameraOffsetX: 0.0 // world units pan (used when followCamera == false)
        property real cameraOffsetY: 0.0
        property bool isMiddleDragging: false
        property real dragStartX: 0
        property real dragStartY: 0
        property real dragOrigOffsetX: 0
        property real dragOrigOffsetY: 0
        // Global scale parameters (world -> screen). We target a tank radius (1.0 world unit) occupying
        // targetTankScreenFraction of the shorter screen dimension at zoom=1. userZoom multiplies this.
        property real tankWorldRadius: 3.0
        property real targetTankScreenFraction: 0.10
        // Effective world->screen scale used everywhere (painting & camera drag math).
        property real worldToScreenScale: Math.min(scene.width, scene.height) * targetTankScreenFraction / tankWorldRadius * userZoom
        // Internal cached last computed desired turret angle (degrees)
        property real desiredTurretAngleDeg: 0
        // Last known mouse position (scene coords) for continuous aim updates while tank/camera move.
        property real lastMouseX: 0
        property real lastMouseY: 0
        property bool lastMouseValid: false

        // QML logging helpers & level filtering
        property string qmlLogLevel: "INFO" // default (override via --qml-log-level or --log-level; supports TRACE|DEBUG|INFO|WARN|ERROR)
        function _levelValue(lv) {
            switch (lv.toUpperCase()) {
            case 'TRACE':
                return 5;
            case 'DEBUG':
                return 10;
            case 'INFO':
                return 20;
            case 'WARN':
                return 30;
            case 'ERROR':
                return 40;
            }
            return 20;
        }
        function _shouldLog(lv) {
            return _levelValue(lv) >= _levelValue(qmlLogLevel);
        }
        function _pad(n) {
            return n < 10 ? '0' + n : '' + n;
        }
        function _ms3(n) {
            return n < 10 ? '00' + n : (n < 100 ? '0' + n : '' + n);
        }
        function _ts() {
            var d = new Date();
            return d.getFullYear() + '-' + _pad(d.getMonth() + 1) + '-' + _pad(d.getDate()) + ' ' + _pad(d.getHours()) + ':' + _pad(d.getMinutes()) + ':' + _pad(d.getSeconds()) + '.' + _ms3(d.getMilliseconds());
        }
        function logT(msg) {
            if (_shouldLog('TRACE'))
                console.debug('[' + _ts() + '] [qml] [T] ' + msg);
        }
        function logD(msg) {
            if (_shouldLog('DEBUG'))
                console.debug('[' + _ts() + '] [qml] [D] ' + msg);
        }
        function logI(msg) {
            if (_shouldLog('INFO'))
                console.log('[' + _ts() + '] [qml] [I] ' + msg);
        }
        function logW(msg) {
            if (_shouldLog('WARN'))
                console.warn('[' + _ts() + '] [qml] [W] ' + msg);
        }
        function logE(msg) {
            if (_shouldLog('ERROR'))
                console.error('[' + _ts() + '] [qml] [E] ' + msg);
        }

        // Initialization (focus + log level arg parsing)
        Component.onCompleted: {
            rootItem.forceActiveFocus();
            var foundArg = false;
            for (var i = 0; i < Qt.application.arguments.length; ++i) {
                var a = Qt.application.arguments[i];
                if (a.indexOf('--qml-log-level=') === 0) {
                    var v = a.substring('--qml-log-level='.length);
                    if (v.length > 0) {
                        qmlLogLevel = v.toUpperCase();
                        foundArg = true;
                    }
                }
            }
            if (!foundArg) {
                for (var j = 0; j < Qt.application.arguments.length; ++j) {
                    var b = Qt.application.arguments[j];
                    if (b.indexOf('--log-level=') === 0) {
                        var vv = b.substring('--log-level='.length);
                        if (vv.length > 0)
                            qmlLogLevel = vv.toUpperCase();
                    }
                }
            }
            logD('QML initialized with log level ' + qmlLogLevel);
        }

        // Keyboard state flags (simple set of currently pressed movement keys)
        property bool keyW: false
        property bool keyS: false
        property bool keyA: false
        property bool keyD: false
        property bool keyQ: false   // turret left
        property bool keyE: false   // turret right
        property bool keySpace: false
        property bool keyShift: false // brake

        function recomputeInput() {
            // Movement forward/backward
            // If joystick actively providing movement, do not override with keyboard
            var joystickActive = joystick && (!joystick.drive_empty);
            if (!joystickActive) {
                var mv = 0;
                if (keyW && !keyS)
                    mv = 1;
                else if (keyS && !keyW)
                    mv = -1;
                if (inputState.move !== mv)
                    inputState.move = mv;
                // Turn mapping: positive -> CCW (left), negative -> CW (right)
                var tr = 0;
                if (keyA && !keyD)
                    tr = 1;
                else
                // CCW
                if (keyD && !keyA)
                    tr = -1;  // CW
                if (inputState.turn !== tr)
                    inputState.turn = tr;
            }
            // Turret turn
            var tt = 0;
            if (!mouseAimEnabled) {
                // keyboard turret control only when mouse aim disabled
                if (keyE && !keyQ)
                    tt = 1;
                else if (keyQ && !keyE)
                    tt = -1;
            }
            // If joystick provides turret aim (second stick) and mouse aim disabled, skip keyboard turret keys
            var joystickTurretActive = joystick && (!mouseAimEnabled) && (!joystick.target_empty);
            if (!joystickTurretActive) {
                if (inputState.turretTurn !== tt)
                    inputState.turretTurn = tt;
            }
            // Fire (held space)
            var fr = keySpace;
            if (inputState.fire !== fr)
                inputState.fire = fr;
            // Brake (held Shift)
            var br = keyShift;
            if (inputState.brake !== br)
                inputState.brake = br;
            // Debug log (will show only if qmlLogLevel <= DEBUG)
            logD(`INPUT mv=${inputState.move} turn=${inputState.turn} turret=${inputState.turretTurn} fire=${inputState.fire}`);
        }

        // Joystick-driven input mapping (called from frame timer)
        function joystick_update() {
            if (!joystick)
                return;
            // Movement axes override keyboard while active
            if (!joystick.drive_empty) {
                if (Math.abs(joystick.drive_y - inputState.move) > 0.001)
                    inputState.move = joystick.drive_y;
                // Invert joystick X to align with keyboard: left (negative drive_x) -> positive turn (CCW)
                if (Math.abs((-joystick.drive_x) - inputState.turn) > 0.001)
                    inputState.turn = -joystick.drive_x;
            } else
            // When joystick released, allow keyboard recompute to reassert state
            // (Do nothing here; recomputeInput() will run on key events.)
            {}
            // Turret aim: use joystick right stick only when mouse aim disabled
            if (!mouseAimEnabled) {
                if (!joystick.target_empty) {
                    var desiredRad = Math.atan2(joystick.target_y, joystick.target_x);
                    var ownIndex = timingState.myEntityId > 0 ? entityModel.rowForEntity(timingState.myEntityId) : -1;
                    if (ownIndex >= 0) {
                        var curDeg = entityModel.interpTurretAngle(ownIndex, timingState.alpha);
                        var desiredDeg = desiredRad * 180.0 / Math.PI;
                        var diff = (desiredDeg - curDeg + 540.0) % 360.0 - 180.0;
                        var turnCmd = 0.0;
                        var deadZone = 1.0;
                        if (Math.abs(diff) > deadZone)
                            turnCmd = Math.max(-1.0, Math.min(1.0, diff / 45.0));
                        if (inputState.turretTurn !== turnCmd)
                            inputState.turretTurn = turnCmd;
                    }
                } else {
                    if (inputState.turretTurn !== 0)
                        inputState.turretTurn = 0;
                }
            }
        }

        // Recompute turret turn command based on last stored mouse position, keeping aim stable
        function updateMouseAim() {
            if (!mouseAimEnabled || !lastMouseValid)
                return;
            const a = timingState.alpha;
            const ownIndex = timingState.myEntityId > 0 ? entityModel.rowForEntity(timingState.myEntityId) : -1;
            if (ownIndex < 0)
                return;
            const scale = rootItem.worldToScreenScale;
            const cx = entityModel.interpX(ownIndex, a);
            const cy = entityModel.interpY(ownIndex, a);
            // Reconstruct world coords under cursor using current camera transforms
            let worldX = (lastMouseX - scene.width / 2) / scale;
            let worldY = (lastMouseY - scene.height / 2) / scale;
            if (rootItem.followCamera) {
                worldX += cx;
                worldY += cy;
            } else {
                worldX += rootItem.cameraOffsetX;
                worldY += rootItem.cameraOffsetY;
            }
            const dx = worldX - cx;
            const dy = worldY - cy;
            if (Math.abs(dx) < 1e-6 && Math.abs(dy) < 1e-6)
                return;
            const desiredRad = Math.atan2(dy, dx);
            const desiredDeg = desiredRad * 180.0 / Math.PI;
            rootItem.desiredTurretAngleDeg = desiredDeg;
            const curDeg = entityModel.interpTurretAngle(ownIndex, a);
            const diff = (desiredDeg - curDeg + 540.0) % 360.0 - 180.0;
            const deadZone = 0.75; // slightly smaller deadzone for more responsive fine aim
            let turnCmd = 0.0;
            if (Math.abs(diff) > deadZone) {
                // Scale: full speed for >= 50 deg difference, smooth near center.
                turnCmd = Math.max(-1.0, Math.min(1.0, diff / 50.0));
            }
            if (inputState.turretTurn !== turnCmd)
                inputState.turretTurn = turnCmd;
        }

        Keys.onPressed: function (ev) {
            switch (ev.key) {
            case Qt.Key_W:
                keyW = true;
                break;
            case Qt.Key_S:
                keyS = true;
                break;
            case Qt.Key_A:
                keyA = true;
                break;
            case Qt.Key_D:
                keyD = true;
                break;
            case Qt.Key_Q:
                keyQ = true;
                break;
            case Qt.Key_E:
                keyE = true;
                break;
            case Qt.Key_Space:
                keySpace = true;
                break;
            case Qt.Key_Shift:
                keyShift = true;
                break;
            case Qt.Key_G:
                rootItem.followCamera = !rootItem.followCamera;
                logD('followCamera=' + rootItem.followCamera);
                break;
            case Qt.Key_H:
                rootItem.showGrid = !rootItem.showGrid;
                logD('showGrid=' + rootItem.showGrid);
                break;
            case Qt.Key_M:
                mouseAimEnabled = !mouseAimEnabled;
                logD('mouseAimEnabled=' + mouseAimEnabled);
                if (!mouseAimEnabled) {
                    inputState.turretTurn = 0;
                }
                break;
            case Qt.Key_Plus:
            case Qt.Key_Equal:
                userZoom /= 0.9;
                userZoom = Math.min(userZoom, 5.0);
                break;
            case Qt.Key_Minus:
                userZoom *= 0.9;
                userZoom = Math.max(userZoom, 0.1);
                break;
            case Qt.Key_F:
                rootItem.showFps = !rootItem.showFps; // toggle FPS overlay
                break;
            default:
                return;
            }
            ev.accepted = true;
            recomputeInput();
        }

        Keys.onReleased: function (ev) {
            switch (ev.key) {
            case Qt.Key_W:
                keyW = false;
                break;
            case Qt.Key_S:
                keyS = false;
                break;
            case Qt.Key_A:
                keyA = false;
                break;
            case Qt.Key_D:
                keyD = false;
                break;
            case Qt.Key_Q:
                keyQ = false;
                break;
            case Qt.Key_E:
                keyE = false;
                break;
            case Qt.Key_Space:
                keySpace = false;
                break;
            case Qt.Key_Shift:
                keyShift = false;
                break;
            default:
                return;
            }
            ev.accepted = true;
            recomputeInput();
        }

        Rectangle {
            anchors.fill: parent
            color: "#202830"
        }

        Canvas {
            id: scene
            property bool profileEnabled: false
            property double _lastPaintStartMs: 0
            Component.onCompleted: {
                // Enable with --qml-log-level=DEBUG plus env T2D_QML_PROFILE=1 (approximated by presence of argument)
                for (var i = 0; i < Qt.application.arguments.length; ++i) {
                    if (Qt.application.arguments[i].indexOf('--qml-profile') === 0)
                        profileEnabled = true;
                }
            }
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                // Fill full area; joystick now overlays on top (z-order higher)
                bottom: parent.bottom
                leftMargin: 8
                rightMargin: 8
                topMargin: 8
                bottomMargin: 8
            }
            onPaint: {
                if (profileEnabled) {
                    var now = Date.now();
                    if (scene._lastPaintStartMs !== 0) {
                        var frameMs = now - scene._lastPaintStartMs;
                        if (rootItem._shouldLog('DEBUG'))
                            rootItem.logD('frame paint_ms=' + frameMs);
                    }
                    scene._lastPaintStartMs = now;
                }
                const ctx = getContext('2d');
                ctx.reset();
                ctx.fillStyle = '#162028';
                ctx.fillRect(0, 0, width, height);
                const a = timingState.alpha;
                const ownIndex = timingState.myEntityId > 0 ? entityModel.rowForEntity(timingState.myEntityId) : -1;
                const scale = rootItem.worldToScreenScale;
                ctx.save();
                ctx.translate(width / 2, height / 2);
                ctx.scale(scale, scale);
                if (rootItem.followCamera && ownIndex >= 0) {
                    ctx.translate(-entityModel.interpX(ownIndex, a), -entityModel.interpY(ownIndex, a));
                } else {
                    ctx.translate(-rootItem.cameraOffsetX, -rootItem.cameraOffsetY);
                }
                // Map boundary rectangle (draw before grid and entities). Map centered at origin.
                if (rootItem.mapWidth > 0 && rootItem.mapHeight > 0) {
                    const hw = rootItem.mapWidth * 0.5;
                    const hh = rootItem.mapHeight * 0.5;
                    ctx.strokeStyle = '#4a7688';
                    ctx.lineWidth = 0.15; // in world units
                    ctx.setLineDash([hw * 0.02, hw * 0.02]); // subtle dash relative to size
                    ctx.beginPath();
                    ctx.rect(-hw, -hh, rootItem.mapWidth, rootItem.mapHeight);
                    ctx.stroke();
                    ctx.setLineDash([]); // reset dash pattern
                }
                // Grid (anchored in world space; accounts for camera / follow transform & zoom)
                if (rootItem.showGrid) {
                    const gridSpacing = 5;            // world units between minor lines
                    const majorEvery = 5;             // every N minor lines draw a thicker (major) line
                    const majorSpacing = gridSpacing * majorEvery; // world units between major lines (stable in world space)
                    // Determine world-space center the camera is focused on
                    let camX, camY;
                    if (rootItem.followCamera && ownIndex >= 0) {
                        camX = entityModel.interpX(ownIndex, a);
                        camY = entityModel.interpY(ownIndex, a);
                    } else {
                        camX = rootItem.cameraOffsetX;
                        camY = rootItem.cameraOffsetY;
                    }
                    // World-space viewport bounds
                    const halfW = (width * 0.5) / scale;
                    const halfH = (height * 0.5) / scale;
                    const worldMinX = camX - halfW;
                    const worldMaxX = camX + halfW;
                    const worldMinY = camY - halfH;
                    const worldMaxY = camY + halfH;
                    // Find first grid lines at or before min bounds
                    const startGX = Math.floor(worldMinX / gridSpacing) * gridSpacing;
                    const startGY = Math.floor(worldMinY / gridSpacing) * gridSpacing;
                    // Avoid excessive lines when zoomed far out: skip if projected spacing < 3px
                    const pixelSpacing = gridSpacing * scale;
                    if (pixelSpacing >= 3) {
                        // Horizontal (vertical lines): iterate world X positions starting at startGX.
                        // Use pre-declared counters to avoid qmlformat merging tokens (workaround for formatting issue).
                        let gx = startGX;
                        while (gx <= worldMaxX) {
                            // Major line if gx is (within epsilon) a multiple of majorSpacing.
                            const isMajor = Math.abs((gx / majorSpacing) - Math.round(gx / majorSpacing)) < 1e-6;
                            ctx.strokeStyle = isMajor ? '#2d4752' : '#23333c';
                            ctx.lineWidth = isMajor ? 0.04 : 0.02;
                            ctx.beginPath();
                            ctx.moveTo(gx, worldMinY);
                            ctx.lineTo(gx, worldMaxY);
                            ctx.stroke();
                            gx += gridSpacing;
                        }
                        // Vertical (horizontal lines): iterate world Y positions starting at startGY.
                        let gy = startGY;
                        while (gy <= worldMaxY) {
                            const isMajor = Math.abs((gy / majorSpacing) - Math.round(gy / majorSpacing)) < 1e-6;
                            ctx.strokeStyle = isMajor ? '#2d4752' : '#23333c';
                            ctx.lineWidth = isMajor ? 0.04 : 0.02;
                            ctx.beginPath();
                            ctx.moveTo(worldMinX, gy);
                            ctx.lineTo(worldMaxX, gy);
                            ctx.stroke();
                            gy += gridSpacing;
                        }
                    }
                }
                function drawRoundedRect(ctx, x, y, w, h, r, fill, stroke, lw) {
                    ctx.beginPath();
                    const rr = Math.min(r, w / 2, h / 2);
                    ctx.moveTo(x + rr, y);
                    ctx.lineTo(x + w - rr, y);
                    ctx.quadraticCurveTo(x + w, y, x + w, y + rr);
                    ctx.lineTo(x + w, y + h - rr);
                    ctx.quadraticCurveTo(x + w, y + h, x + w - rr, y + h);
                    ctx.lineTo(x + rr, y + h);
                    ctx.quadraticCurveTo(x, y + h, x, y + h - rr);
                    ctx.lineTo(x, y + rr);
                    ctx.quadraticCurveTo(x, y, x + rr, y);
                    if (fill) {
                        ctx.fillStyle = fill;
                        ctx.fill();
                    }
                    if (stroke) {
                        ctx.lineWidth = lw || 1;
                        ctx.strokeStyle = stroke;
                        ctx.stroke();
                    }
                }
                const SPRITE_FRONT_OFFSET = Math.PI / 2; // Protocol: 0° = +X (right). Sprite points up, so rotate +90° to align sprite front with +X.
                // Draw a single tank with animated treads. Track animation speed matches linear velocity at the
                // midpoint of each track: v_track = v_forward ± omega * trackLateralOffset.
                function drawTank(ctx, wx, wy, hullRad, turretRad, isOwn, isDead, treadOffsetL, treadOffsetR) {
                    // Convert protocol angle (0°=+X) to sprite angle (0 sprite up) via SPRITE_FRONT_OFFSET.
                    const W = 480, H = 640;
                    const scalePix = 6.4 / H;
                    ctx.save();
                    ctx.translate(wx, wy);
                    ctx.rotate(hullRad + SPRITE_FRONT_OFFSET);
                    ctx.scale(scalePix, scalePix);
                    ctx.translate(-W / 2, -H / 2);
                    // Base track rectangles
                    ctx.fillStyle = isDead ? '#2a2a2a' : '#424141';
                    ctx.fillRect(0, 0, 140, H);
                    ctx.fillRect(342, 0, 140, H);
                    // Animated tread pattern (skip if dead to reduce visual noise)
                    if (!isDead) {
                        const PATTERN_STEP_PX = 22; // pixel spacing between tread rungs
                        const rungHeight = 6;
                        const light = '#d9d9d9';
                        const dark = '#202020';
                        function drawTreadColumn(x0, w, offsetPx) {
                            // Clamp and wrap offset
                            offsetPx = ((offsetPx % PATTERN_STEP_PX) + PATTERN_STEP_PX) % PATTERN_STEP_PX;
                            for (let y = -PATTERN_STEP_PX; y < H + PATTERN_STEP_PX; y += PATTERN_STEP_PX) {
                                const yy = y + offsetPx;
                                if (yy < -rungHeight || yy > H)
                                    continue;
                                // Rung base
                                ctx.fillStyle = dark;
                                ctx.fillRect(x0 + 6, yy, w - 12, rungHeight);
                                // Highlight stripe
                                ctx.fillStyle = light;
                                ctx.fillRect(x0 + 6, yy + 1, w - 12, 2);
                            }
                        }
                        drawTreadColumn(0, 140, treadOffsetL);
                        drawTreadColumn(342, 140, treadOffsetR);
                    }
                    ctx.fillStyle = isDead ? (isOwn ? '#2f3a2f' : '#323232') : (isOwn ? '#5c6e5c' : '#6f6e6e');
                    ctx.strokeStyle = isDead ? '#1e1e1e' : '#2e2e2e';
                    ctx.lineWidth = 2;
                    ctx.beginPath();
                    ctx.rect(28, 41, 424, 558);
                    ctx.fill();
                    ctx.stroke();
                    // Rear brake lights: brighten & glow when local player braking
                    const brakeOn = isOwn && inputState.brake && !isDead;
                    if (brakeOn) {
                        ctx.save();
                        ctx.fillStyle = '#ff2727';
                        ctx.shadowColor = '#ff4545';
                        ctx.shadowBlur = 32;
                        ctx.fillRect(46, 576, 80, 23);
                        ctx.fillRect(358, 576, 80, 23);
                        ctx.restore();
                    } else {
                        ctx.fillStyle = isDead ? '#1c1c1c' : '#5c1010'; // dim when not braking or dead
                        ctx.fillRect(46, 576, 80, 23);
                        ctx.fillRect(358, 576, 80, 23);
                    }
                    // Front headlights: disable when dead (no white glow after destruction)
                    if (!isDead) {
                        drawRoundedRect(ctx, 65, 49, 43, 30, 15, '#f1f0f0');
                        drawRoundedRect(ctx, 377, 49, 43, 30, 15, '#f1f0f0');
                    } else {
                        // Optionally draw dimmed placeholder to retain silhouette (very dark)
                        drawRoundedRect(ctx, 65, 49, 43, 30, 15, '#1a1a1a');
                        drawRoundedRect(ctx, 377, 49, 43, 30, 15, '#1a1a1a');
                    }
                    ctx.save();
                    ctx.translate(W / 2, H / 2);
                    ctx.rotate(turretRad - hullRad); // both already protocol-space; relative rotation unaffected by offset
                    ctx.translate(-W / 2, -H / 2);
                    drawRoundedRect(ctx, 140, 195, 200, 250, 32, isDead ? '#5e5e5e' : '#bfbfbf', isDead ? '#2a2828' : '#363434', 8);
                    ctx.lineWidth = 8;
                    ctx.fillStyle = isDead ? '#5e5e5e' : '#bfbfbf';
                    ctx.strokeStyle = isDead ? '#2a2828' : '#363434';
                    ctx.beginPath();
                    ctx.rect((W - 30) / 2, -80, 30, 320);
                    ctx.fill();
                    ctx.stroke();
                    ctx.restore();
                    const hpY = 31;
                    if (!isDead) {
                        ctx.fillStyle = '#202828';
                        ctx.fillRect(28, hpY, 424, 6);
                        ctx.fillStyle = isOwn ? '#6cff5d' : '#3fa7ff';
                        ctx.fillRect(28, hpY, 424, 6);
                    } else {
                        // Dead: burnt overlay bar
                        ctx.fillStyle = '#1a1d1d';
                        ctx.fillRect(28, hpY, 424, 6);
                        ctx.fillStyle = '#302f2f';
                        ctx.fillRect(28, hpY, 424, 6);
                    }
                    if (isDead) {
                        // Subtle scorch mark overlay
                        ctx.save();
                        ctx.globalCompositeOperation = 'lighter';
                        const grad = ctx.createRadialGradient(W / 2, H / 2, 60, W / 2, H / 2, 260);
                        grad.addColorStop(0, 'rgba(80,80,80,0.35)');
                        grad.addColorStop(1, 'rgba(10,10,10,0)');
                        ctx.fillStyle = grad;
                        ctx.fillRect(0, 0, W, H);
                        ctx.restore();
                    }
                    ctx.restore();
                }
                // Crates (simple brown squares with rotation)
                if (typeof crateModel !== 'undefined') {
                    for (let c = 0; c < crateModel.count(); ++c) {
                        const cr = crateModel.get(c);
                        const wx = cr.x;
                        const wy = cr.y;
                        const ang = crateModel.angleRad(c);
                        ctx.save();
                        ctx.translate(wx, wy);
                        ctx.rotate(ang);
                        const h = 1.2; // half extent used on server
                        ctx.fillStyle = '#5a4a32';
                        ctx.strokeStyle = '#c8a060';
                        ctx.lineWidth = 0.12;
                        ctx.beginPath();
                        ctx.rect(-h, -h, h * 2, h * 2);
                        ctx.fill();
                        ctx.stroke();
                        // subtle plank lines
                        ctx.strokeStyle = '#7d6a4d';
                        ctx.lineWidth = 0.06;
                        ctx.beginPath();
                        ctx.moveTo(-h * 0.6, -h);
                        ctx.lineTo(-h * 0.6, h);
                        ctx.moveTo(0, -h);
                        ctx.lineTo(0, h);
                        ctx.moveTo(h * 0.6, -h);
                        ctx.lineTo(h * 0.6, h);
                        ctx.stroke();
                        ctx.restore();
                    }
                }
                function drawBullet(ctx, wx, wy, vx, vy) {
                    // Legacy bullet 60x20 px mapped to world 0.45x0.15 -> scale factors below
                    const PX_W = 60, PX_H = 20;
                    const WORLD_W = 0.45, WORLD_H = 0.15;
                    const sx = WORLD_W / PX_W;
                    const sy = WORLD_H / PX_H;
                    const ang = Math.atan2(vy, vx);
                    ctx.save();
                    ctx.translate(wx, wy);
                    ctx.rotate(ang);
                    ctx.scale(sx, sy);
                    ctx.translate(-PX_W / 2, -PX_H / 2);
                    const grad = ctx.createLinearGradient(0, 0, PX_W, 0);
                    grad.addColorStop(0, '#2c2c2c');
                    grad.addColorStop(0.445, '#d0d0d1');
                    grad.addColorStop(0.61, '#d0d0d1');
                    grad.addColorStop(1, '#2c2c2c');
                    ctx.fillStyle = grad;
                    ctx.fillRect(0, 0, PX_W, PX_H);
                    ctx.restore();
                }
                // Persistent per-frame tracking arrays (attached to rootItem for lifetime)
                if (!rootItem._treadInit) {
                    rootItem._treadInit = true;
                    rootItem._treadPrevX = [];
                    rootItem._treadPrevY = [];
                    rootItem._treadPrevHull = [];
                    rootItem._treadOffL = [];
                    rootItem._treadOffR = [];
                    rootItem._treadLastMs = Date.now();
                }
                const nowMsTracks = Date.now();
                let dtTracks = (nowMsTracks - rootItem._treadLastMs) / 1000.0;
                if (dtTracks <= 0 || dtTracks > 0.25)
                    dtTracks = 1 / 60; // fallback clamp
                rootItem._treadLastMs = nowMsTracks;
                const TRACK_LATERAL_OFFSET_WORLD = 1.7; // distance from center to each track midpoint (approx)
                const WORLD_LENGTH_PER_TEXTURE_REPEAT = 0.55; // tune visual density
                const PX_PER_WORLD = 640 / 6.4; // sprite mapping (H / worldHeight)
                // Ensure arrays sized
                const ec = entityModel.count();
                function normAngle(a) {
                    while (a > Math.PI)
                        a -= Math.PI * 2;
                    while (a < -Math.PI)
                        a += Math.PI * 2;
                    return a;
                }
                for (let i = 0; i < ec; ++i) {
                    if (rootItem._treadPrevX.length <= i) {
                        rootItem._treadPrevX[i] = entityModel.interpX(i, a);
                        rootItem._treadPrevY[i] = entityModel.interpY(i, a);
                        rootItem._treadPrevHull[i] = entityModel.interpHullAngleRad(i, a);
                        rootItem._treadOffL[i] = 0;
                        rootItem._treadOffR[i] = 0;
                    }
                    const x = entityModel.interpX(i, a);
                    const y = entityModel.interpY(i, a);
                    const hullRad = entityModel.interpHullAngleRad(i, a);
                    const turretRad = entityModel.interpTurretAngleRad(i, a);
                    const dead = entityModel.isDead(i);
                    const dx = x - rootItem._treadPrevX[i];
                    const dy = y - rootItem._treadPrevY[i];
                    const forwardX = Math.cos(hullRad);
                    const forwardY = Math.sin(hullRad);
                    const vForward = (dx * forwardX + dy * forwardY) / dtTracks; // world units / s
                    const dHull = normAngle(hullRad - rootItem._treadPrevHull[i]);
                    const omega = dHull / dtTracks; // rad/s
                    // Track forward speeds at midpoint (world units / s)
                    const vLeft = vForward - omega * TRACK_LATERAL_OFFSET_WORLD;
                    const vRight = vForward + omega * TRACK_LATERAL_OFFSET_WORLD;
                    // Convert incremental displacement to pixel offset for pattern
                    rootItem._treadOffL[i] += (vLeft * dtTracks / WORLD_LENGTH_PER_TEXTURE_REPEAT) * (640); // scale then wrap
                    rootItem._treadOffR[i] += (vRight * dtTracks / WORLD_LENGTH_PER_TEXTURE_REPEAT) * (640);
                    // Wrap to keep numbers bounded
                    if (rootItem._treadOffL[i] > 100000 || rootItem._treadOffL[i] < -100000)
                        rootItem._treadOffL[i] = rootItem._treadOffL[i] % 100000;
                    if (rootItem._treadOffR[i] > 100000 || rootItem._treadOffR[i] < -100000)
                        rootItem._treadOffR[i] = rootItem._treadOffR[i] % 100000;
                    drawTank(ctx, x, y, hullRad, turretRad, i === ownIndex, dead, rootItem._treadOffL[i], rootItem._treadOffR[i]);
                    rootItem._treadPrevX[i] = x;
                    rootItem._treadPrevY[i] = y;
                    rootItem._treadPrevHull[i] = hullRad;
                }
                // Ammo boxes (simple square with plus sign)
                if (typeof ammoBoxModel !== 'undefined') {
                    for (let b = 0; b < ammoBoxModel.count(); ++b) {
                        const wx = ammoBoxModel.get(b).x;
                        const wy = ammoBoxModel.get(b).y;
                        const active = ammoBoxModel.get(b).active;
                        if (!active)
                            continue;
                        ctx.save();
                        ctx.translate(wx, wy);
                        const r = 0.9; // match server radius
                        ctx.fillStyle = '#3b5d1d';
                        ctx.strokeStyle = '#92ff5d';
                        ctx.lineWidth = 0.12;
                        ctx.beginPath();
                        ctx.rect(-r, -r, r * 2, r * 2);
                        ctx.fill();
                        ctx.stroke();
                        // plus sign
                        ctx.strokeStyle = '#ffffff';
                        ctx.lineWidth = 0.18;
                        ctx.beginPath();
                        ctx.moveTo(-r * 0.6, 0);
                        ctx.lineTo(r * 0.6, 0);
                        ctx.moveTo(0, -r * 0.6);
                        ctx.lineTo(0, r * 0.6);
                        ctx.stroke();
                        ctx.restore();
                    }
                }
                for (let j = 0; j < projectileModel.count(); ++j) {
                    const wx = projectileModel.interpX(j, a);
                    const wy = projectileModel.interpY(j, a);
                    const vx = projectileModel.interpVx(j, a);
                    const vy = projectileModel.interpVy(j, a);
                    drawBullet(ctx, wx, wy, vx, vy);
                }
                ctx.restore(); // pop world transform (no outside mask)
            }
            Connections {
                target: timingState
                function onFrameTick() {
                    scene.requestPaint();
                    rootItem.joystick_update();
                    rootItem.updateMouseAim();
                    // FPS accumulation (simple 1s rolling window)
                    rootItem._fpsFrameCount += 1;
                    var now = Date.now();
                    if (rootItem._fpsLastMs === 0)
                        rootItem._fpsLastMs = now;
                    else if (now - rootItem._fpsLastMs >= 1000) {
                        rootItem.fpsValue = rootItem._fpsFrameCount * 1000.0 / (now - rootItem._fpsLastMs);
                        rootItem._fpsFrameCount = 0;
                        rootItem._fpsLastMs = now;
                    }
                }
            }
            MouseArea {
                // retain interaction overlay
                anchors.fill: parent
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
                onWheel: function (ev) {
                    if (ev.angleDelta.y > 0)
                        rootItem.userZoom /= 0.9;
                    else
                        rootItem.userZoom *= 0.9;
                    rootItem.userZoom = Math.max(0.1, Math.min(5.0, rootItem.userZoom));
                    ev.accepted = true;
                }
                onPressed: function (ev) {
                    if (ev.button === Qt.RightButton) {
                        rootItem.followCamera = !rootItem.followCamera;
                        if (rootItem.followCamera) {
                            rootItem.cameraOffsetX = 0;
                            rootItem.cameraOffsetY = 0;
                        }
                    } else if (ev.button === Qt.MiddleButton) {
                        rootItem.isMiddleDragging = true;
                        rootItem.dragStartX = ev.x;
                        rootItem.dragStartY = ev.y;
                        rootItem.dragOrigOffsetX = rootItem.cameraOffsetX;
                        rootItem.dragOrigOffsetY = rootItem.cameraOffsetY;
                    } else if (ev.button === Qt.LeftButton) {
                        inputState.fire = true;
                    }
                }
                onReleased: function (ev) {
                    if (ev.button === Qt.MiddleButton) {
                        rootItem.isMiddleDragging = false;
                    } else if (ev.button === Qt.LeftButton) {
                        inputState.fire = false;
                    }
                }
                onPositionChanged: function (ev) {
                    const a = timingState.alpha;
                    let ownIndex = timingState.myEntityId > 0 ? entityModel.rowForEntity(timingState.myEntityId) : -1;
                    if (ownIndex < 0)
                        return;
                    const scale = rootItem.worldToScreenScale;
                    const cx = entityModel.interpX(ownIndex, a);
                    const cy = entityModel.interpY(ownIndex, a);
                    // Store last mouse position for per-frame updates
                    rootItem.lastMouseX = ev.x;
                    rootItem.lastMouseY = ev.y;
                    rootItem.lastMouseValid = true;
                    let worldX = (ev.x - width / 2) / scale;
                    let worldY = (ev.y - height / 2) / scale;
                    if (rootItem.followCamera) {
                        worldX += cx;
                        worldY += cy;
                    } else {
                        worldX += rootItem.cameraOffsetX;
                        worldY += rootItem.cameraOffsetY;
                    }
                    if (rootItem.isMiddleDragging && !rootItem.followCamera) {
                        // 1:1 pixel mapping: shifting mouse by N pixels moves world N pixels on screen.
                        // Since screen shift = (deltaWorld * scale), need deltaWorld = deltaScreen / scale.
                        let dxScreen = ev.x - rootItem.dragStartX;
                        let dyScreen = ev.y - rootItem.dragStartY;
                        let dxWorld = dxScreen / scale;
                        let dyWorld = dyScreen / scale;
                        rootItem.cameraOffsetX = rootItem.dragOrigOffsetX - dxWorld;
                        rootItem.cameraOffsetY = rootItem.dragOrigOffsetY - dyWorld;
                    }
                    if (rootItem.mouseAimEnabled) {
                        // Immediate update this event; subsequent frames handled by updateMouseAim()
                        rootItem.updateMouseAim();
                    }
                }
                onCanceled: {
                    rootItem.isMiddleDragging = false;
                    rootItem.lastMouseValid = false;
                }
            }
        }

        // Overlay panels for statistics (tanks & projectiles) placed over the scene with collapse/expand buttons
        Rectangle {
            id: tankPanel
            z: 10
            color: tankPanel.collapsed ? "#223038" : "#263540cc" // semi-transparent when expanded
            radius: 6
            anchors.left: parent.left
            anchors.top: parent.top
            anchors.margins: 10
            property bool collapsed: true
            width: collapsed ? 34 : 270
            height: collapsed ? 34 : Math.min(parent.height * 0.55, 380)
            border.color: "#35505c"
            border.width: 1
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                RowLayout {
                    id: tankHeader
                    height: 24
                    spacing: 6
                    Layout.fillWidth: true
                    Text {
                        text: tankPanel.collapsed ? "" : "Tanks"
                        color: "#c7d4df"
                        font.pixelSize: 14
                        visible: !tankPanel.collapsed
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Button {
                        id: tankToggle
                        width: 24
                        height: 24
                        Layout.alignment: Qt.AlignRight
                        opacity: hovered ? 0.9 : 0.45
                        hoverEnabled: true
                        background: Rectangle {
                            radius: 4
                            color: tankPanel.collapsed ? "#4d6c7a" : "#5a7582"
                            border.color: "#86a9b6"
                            opacity: 0.6
                        }
                        contentItem: Text {
                            text: tankPanel.collapsed ? "+" : "−"
                            color: "#e0eef5"
                            font.pixelSize: 16
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            anchors.fill: parent
                        }
                        onClicked: {
                            tankPanel.collapsed = !tankPanel.collapsed;
                            // Force focus back to main game input container to keep WASD working
                            rootItem.forceActiveFocus();
                        }
                    }
                }
                ListView {
                    id: tankList
                    visible: !tankPanel.collapsed
                    model: entityModel
                    clip: true
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    delegate: Rectangle {
                        width: tankList.width
                        height: 30
                        color: index % 2 === 0 ? "#2b3642" : "#23303a"
                        Row {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 6
                            Text {
                                text: entityId
                                color: "#d0d3d6"
                                width: 38
                            }
                            Text {
                                // Use model.x/model.y to access model roles; plain x/y would refer to delegate Item position.
                                text: `x:${model.x.toFixed(1)} y:${model.y.toFixed(1)}`
                                color: "#9fb2c3"
                                width: 138
                            }
                            Text {
                                text: `hp:${hp} a:${model.ammo}`
                                color: "#c5a96a"
                            }
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }

        Rectangle {
            id: projectilePanel
            z: 10
            color: projectilePanel.collapsed ? "#223038" : "#263540cc"
            radius: 6
            anchors.top: parent.top
            anchors.right: parent.right
            anchors.margins: 10
            property bool collapsed: true
            width: collapsed ? 34 : 200
            height: collapsed ? 34 : Math.min(parent.height * 0.45, 300)
            border.color: "#35505c"
            border.width: 1
            ColumnLayout {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                RowLayout {
                    id: projHeader
                    height: 24
                    spacing: 6
                    Layout.fillWidth: true
                    Text {
                        text: projectilePanel.collapsed ? "" : "Projectiles"
                        color: "#c7d4df"
                        font.pixelSize: 14
                        visible: !projectilePanel.collapsed
                    }
                    Item {
                        Layout.fillWidth: true
                    }
                    Button {
                        id: projToggle
                        width: 24
                        height: 24
                        Layout.alignment: Qt.AlignRight
                        opacity: hovered ? 0.9 : 0.45
                        hoverEnabled: true
                        background: Rectangle {
                            radius: 4
                            color: projectilePanel.collapsed ? "#4d6c7a" : "#5a7582"
                            border.color: "#86a9b6"
                            opacity: 0.6
                        }
                        contentItem: Text {
                            text: projectilePanel.collapsed ? "+" : "−"
                            color: "#e0eef5"
                            font.pixelSize: 16
                            horizontalAlignment: Text.AlignHCenter
                            verticalAlignment: Text.AlignVCenter
                            anchors.fill: parent
                        }
                        onClicked: {
                            projectilePanel.collapsed = !projectilePanel.collapsed;
                            rootItem.forceActiveFocus();
                        }
                    }
                }
                ListView {
                    id: projectileList
                    visible: !projectilePanel.collapsed
                    model: projectileModel
                    clip: true
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    delegate: Rectangle {
                        width: projectileList.width
                        height: 24
                        color: index % 2 === 0 ? "#303b46" : "#28323d"
                        Row {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 6
                            Text {
                                text: projId
                                color: "#d0d3d6"
                                width: 46
                            }
                            Text {
                                text: `x:${x.toFixed(1)}`
                                color: "#9fb2c3"
                                width: 56
                            }
                            Text {
                                text: `y:${y.toFixed(1)}`
                                color: "#9fb2c3"
                            }
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }

        // FPS overlay
        Text {
            id: fpsLabel
            visible: rootItem.showFps
            z: 15
            anchors.top: tankPanel.bottom
            anchors.left: tankPanel.left
            anchors.topMargin: 6
            text: "FPS: " + rootItem.fpsValue.toFixed(1)
            color: "#c8e4f0"
            font.pixelSize: 14
            Rectangle {
                anchors.fill: parent
                anchors.margins: -4
                radius: 4
                color: "#0d161dcc"
                border.color: "#2e4450"
                z: -1
            }
        }

        // Frame timing stats overlay (appears under FPS when enabled)
        Item {
            id: frameStatsBox
            visible: rootItem.showFps
            z: 15
            anchors.top: fpsLabel.bottom
            anchors.left: fpsLabel.left
            anchors.topMargin: 4
            property string _lastText: ""
            Timer {
                interval: 250
                running: frameStatsBox.visible
                repeat: true
                onTriggered: {
                    statsLast.text = "Last: " + timingState.lastFrameMs.toFixed(2) + " ms";
                    statsMax.text = "Max: " + timingState.maxFrameMs.toFixed(2) + " ms";
                    statsLong.text = "Long: " + timingState.longFrameCount;
                }
            }
            Row {
                id: frameStatsRow
                spacing: 8
                Text {
                    id: statsLast
                    color: "#afc9d6"
                    font.pixelSize: 12
                    text: "Last: --"
                }
                Text {
                    id: statsMax
                    color: "#afc9d6"
                    font.pixelSize: 12
                    text: "Max: --"
                }
                Text {
                    id: statsLong
                    color: "#afc9d6"
                    font.pixelSize: 12
                    text: "Long: --"
                }
                Button {
                    id: resetStatsBtn
                    text: "Reset"
                    font.pixelSize: 10
                    padding: 2
                    opacity: hovered ? 0.85 : 0.45
                    scale: hovered ? 1.05 : 1.0
                    background: Rectangle {
                        radius: 3
                        color: resetStatsBtn.pressed ? "#28424c" : "#1d3038"
                        border.color: "#3a5866"
                        border.width: 1
                        opacity: 0.9
                    }
                    contentItem: Text {
                        text: parent.text
                        color: "#d0dde5"
                        font.pixelSize: parent.font.pixelSize
                        horizontalAlignment: Text.AlignHCenter
                        verticalAlignment: Text.AlignVCenter
                        opacity: parent.opacity
                    }
                    onClicked: timingState.resetFrameStats()
                    ToolTip.visible: hovered
                    ToolTip.text: "Reset frame stats"
                }
            }
            Rectangle {
                id: frameStatsBg
                z: -1
                color: "#0d161dcc"
                border.color: "#2e4450"
                radius: 4
                anchors.top: frameStatsRow.top
                anchors.bottom: frameStatsRow.bottom
                anchors.left: frameStatsRow.left
                anchors.right: frameStatsRow.right
                anchors.margins: -4
            }
        }

        // Joystick composite control (two sticks + fire/brake buttons)
        Joystick {
            id: joystick
            z: 20 // overlay above scene
            anchors {
                left: parent.left
                right: parent.right
                bottom: parent.bottom
                leftMargin: 8
                rightMargin: 8
                bottomMargin: 8
            }
            height: 180
            onButton_state: function (name, pressed) {
                if (name === 'fire')
                    inputState.fire = pressed;
                else if (name === 'brake')
                    inputState.brake = pressed;
            }
        }

        Text {
            id: hudStats
            z: 11
            anchors.top: parent.top
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.topMargin: 10
            text: "Snapshot tanks=" + tankList.count + "  follow=" + (rootItem.followCamera ? "on" : "off") + "(G)" + "  grid=" + (rootItem.showGrid ? "on" : "off") + "(H)" + "  mouseAim=" + (rootItem.mouseAimEnabled ? "on" : "off") + "(M)" + "  zoom=" + rootItem.userZoom.toFixed(2) + "(+/- wheel)" + (timingState.matchOver ? "  MATCH OVER" : (timingState.remainingHardCapSeconds > 0 ? ("  time=" + timingState.remainingHardCapSeconds + "s") : ""))
            color: "#d0dde5"
            font.pixelSize: 14
            Rectangle {
                // backdrop for readability
                anchors.fill: parent
                anchors.margins: -6
                radius: 6
                color: "#101820cc"
                border.color: "#2e4450"
                z: -1
            }
        }

        // Center match end overlay
        Item {
            id: matchEndOverlay
            z: 30
            anchors.fill: parent
            visible: timingState.matchOver
            Rectangle {
                id: overlayPanel
                width: Math.min(parent.width * 0.5, 420)
                height: 200
                radius: 12
                color: "#101820dd"
                border.color: "#3a5866"
                anchors.horizontalCenter: parent.horizontalCenter
                anchors.verticalCenter: parent.verticalCenter
                ColumnLayout {
                    anchors.fill: parent
                    anchors.margins: 24
                    spacing: 18
                    Text {
                        id: resultText
                        Layout.alignment: Qt.AlignHCenter
                        font.pixelSize: 28
                        text: timingState.matchOutcome === 1 ? "Victory" : (timingState.matchOutcome === -1 ? "Defeat" : "Draw")
                        color: timingState.matchOutcome === 1 ? "#6dff6d" : (timingState.matchOutcome === -1 ? "#ff6464" : "#e0e0e0")
                    }
                    Text {
                        Layout.alignment: Qt.AlignHCenter
                        text: timingState.autoReturnSeconds > 0 ? ("Returning to lobby in " + timingState.autoReturnSeconds + "s") : "Returning..."
                        color: "#c7d4df"
                        font.pixelSize: 18
                    }
                    RowLayout {
                        Layout.alignment: Qt.AlignHCenter
                        spacing: 14
                        Button {
                            id: lobbyNowBtn
                            text: "Return to Lobby"
                            onClicked: timingState.returnToLobbyNow()
                        }
                    }
                }
            }
        }

        // Lobby status panel (shown when not in active match end screen)
        Rectangle {
            id: lobbyPanel
            // Hide lobby status while an active match is running; show only in lobby (pre-match) or post-return.
            visible: lobbyState && !timingState.matchActive
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: 54
            width: Math.min(parent.width * 0.6, 480)
            color: "#1b262ecc"
            radius: 8
            border.color: "#35505c"
            Column {
                anchors.fill: parent
                anchors.margins: 10
                spacing: 4
                Text {
                    id: lobbyStateText
                    color: "#d0dde5"
                    font.pixelSize: 16
                    text: {
                        if (!lobbyState)
                            return "";
                        var s = "";
                        switch (lobbyState.state) {
                        case 0:
                            s = "Waiting for Players";
                            break;
                        case 1:
                            s = "Forming Match";
                            break;
                        case 2:
                            s = "Starting Match";
                            break;
                        default:
                            s = "Lobby";
                            break;
                        }
                        return s;
                    }
                }
                Text {
                    color: "#b9c7d2"
                    font.pixelSize: 14
                    text: lobbyState ? ("Queue: " + lobbyState.position + "  players: " + lobbyState.playersInQueue + "  needed: " + lobbyState.neededForMatch) : ""
                }
                Text {
                    visible: lobbyState && lobbyState.lobbyCountdown > 0
                    color: "#c7d4df"
                    font.pixelSize: 14
                    text: lobbyState ? ("Start in: " + lobbyState.lobbyCountdown + "s") : ""
                }
                Text {
                    visible: lobbyState && lobbyState.projectedBotFill > 0
                    color: "#c59f4a"
                    font.pixelSize: 13
                    text: lobbyState ? ("Bots to add: " + lobbyState.projectedBotFill) : ""
                }
            }
        }
    } // end rootItem
}
