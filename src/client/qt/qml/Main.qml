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
        anchors.fill: parent
        focus: true
        property bool followCamera: false // default off so movement is visible
        property bool showGrid: true
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
    property real worldToScreenScale:   Math.min(scene.width,   scene.height)    * targetTankScreenFraction /   tankWorldRadius *    userZoom
    // Internal cached last computed desired turret angle (degrees)
        property real desiredTurretAngleDeg: 0
        Component.onCompleted: {
            rootItem.forceActiveFocus();
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

        // Unified logging helpers to match C++ logger format: [date time] [qml] [L] message
        function _pad(n) {
            return n < 10 ? '0' + n : '' + n;
        }
        function _ts() {
            var d = new Date();
            return d.getFullYear() + '-' + _pad(d.getMonth() + 1) + '-' + _pad(d.getDate()) + ' ' + _pad(d.getHours()) + ':' + _pad(d.getMinutes()) + ':' + _pad(d.getSeconds());
        }
        function logD(msg) {
            console.debug('[' + _ts() + '] [qml] [D] ' + msg);
        }
        function logI(msg) {
            console.log('[' + _ts() + '] [qml] [I] ' + msg);
        }
        function logW(msg) {
            console.warn('[' + _ts() + '] [qml] [W] ' + msg);
        }
        function logE(msg) {
            console.error('[' + _ts() + '] [qml] [E] ' + msg);
        }

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
                var tr = 0;
                if (keyD && !keyA)
                    tr = 1;
                else if (keyA && !keyD)
                    tr = -1;
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
            // Debug log
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
                if (Math.abs(joystick.drive_x - inputState.turn) > 0.001)
                    inputState.turn = joystick.drive_x;
            } else
            // When joystick released, allow keyboard recompute to reassert state
            // (Do nothing here; recomputeInput() will run on key events.)
            {}
            // Turret aim: use joystick right stick only when mouse aim disabled
            if (!mouseAimEnabled) {
                if (!joystick.target_empty) {
                    var desiredRad = Math.atan2(joystick.target_y, joystick.target_x);
                    var ownIndex = entityModel.count() > 0 ? 0 : -1;
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
            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
                bottom: joystick.top
                leftMargin: 8
                rightMargin: 8
                topMargin: 8
                bottomMargin: 8
            }
            onPaint: {
                const ctx = getContext('2d');
                ctx.reset();
                ctx.fillStyle = '#162028';
                ctx.fillRect(0, 0, width, height);
                const a = timingState.alpha;
                const ownIndex = entityModel.count() > 0 ? 0 : -1;
                const scale = rootItem.worldToScreenScale;
                ctx.save();
                ctx.translate(width / 2, height / 2);
                ctx.scale(scale, scale);
                if (rootItem.followCamera && ownIndex >= 0) {
                    ctx.translate(-entityModel.interpX(ownIndex, a), -entityModel.interpY(ownIndex, a));
                } else {
                    ctx.translate(-rootItem.cameraOffsetX, -rootItem.cameraOffsetY);
                }
                // Grid
                if (rootItem.showGrid) {
                    const gridSpacing = 5;
                    const halfW = width / scale / 2;
                    const halfH = height / scale / 2;
                    ctx.save();
                    ctx.strokeStyle = '#23333c';
                    ctx.lineWidth = 0.02;
                    ctx.beginPath();
                    for (let gx = -halfW; gx <= halfW; gx += gridSpacing) {
                        ctx.moveTo(gx, -halfH);
                        ctx.lineTo(gx, halfH);
                    }
                    for (let gy = -halfH; gy <= halfH; gy += gridSpacing) {
                        ctx.moveTo(-halfW, gy);
                        ctx.lineTo(halfW, gy);
                    }
                    ctx.stroke();
                    ctx.restore();
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
                const SPRITE_FRONT_OFFSET = -Math.PI / 2; // Protocol: 0° = +X (right). Art points up, so subtract 90°.
                function drawTank(ctx, wx, wy, hullRad, turretRad, isOwn) {
                    // Convert protocol angle (0°=+X) to sprite angle (0 sprite up) via SPRITE_FRONT_OFFSET.
                    const W = 480, H = 640;
                    const scalePix = 6.4 / H;
                    ctx.save();
                    ctx.translate(wx, wy);
                    ctx.rotate(hullRad + SPRITE_FRONT_OFFSET);
                    ctx.scale(scalePix, scalePix);
                    ctx.translate(-W / 2, -H / 2);
                    ctx.fillStyle = '#424141';
                    ctx.fillRect(0, 0, 140, H);
                    ctx.fillRect(342, 0, 140, H);
                    ctx.fillStyle = isOwn ? '#5c6e5c' : '#6f6e6e';
                    ctx.strokeStyle = '#2e2e2e';
                    ctx.lineWidth = 2;
                    ctx.beginPath();
                    ctx.rect(28, 41, 424, 558);
                    ctx.fill();
                    ctx.stroke();
                    ctx.fillStyle = '#9b0101';
                    ctx.fillRect(46, 576, 80, 23);
                    ctx.fillRect(358, 576, 80, 23);
                    drawRoundedRect(ctx, 65, 49, 43, 30, 15, '#f1f0f0');
                    drawRoundedRect(ctx, 377, 49, 43, 30, 15, '#f1f0f0');
                    ctx.save();
                    ctx.translate(W / 2, H / 2);
                    ctx.rotate(turretRad - hullRad); // both already protocol-space; relative rotation unaffected by offset
                    ctx.translate(-W / 2, -H / 2);
                    drawRoundedRect(ctx, 140, 195, 200, 250, 32, '#bfbfbf', '#363434', 8);
                    ctx.lineWidth = 8;
                    ctx.fillStyle = '#bfbfbf';
                    ctx.strokeStyle = '#363434';
                    ctx.beginPath();
                    ctx.rect((W - 30) / 2, -80, 30, 320);
                    ctx.fill();
                    ctx.stroke();
                    ctx.restore();
                    const hpY = 31;
                    ctx.fillStyle = '#202828';
                    ctx.fillRect(28, hpY, 424, 6);
                    ctx.fillStyle = isOwn ? '#6cff5d' : '#3fa7ff';
                    ctx.fillRect(28, hpY, 424, 6);
                    ctx.restore();
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
                for (let i = 0; i < entityModel.count(); ++i) {
                    const wx = entityModel.interpX(i, a);
                    const wy = entityModel.interpY(i, a);
                    const hullRad = entityModel.interpHullAngle(i, a) * Math.PI / 180.0;
                    const turretRad = entityModel.interpTurretAngle(i, a) * Math.PI / 180.0;
                    drawTank(ctx, wx, wy, hullRad, turretRad, i === ownIndex);
                }
                for (let j = 0; j < projectileModel.count(); ++j) {
                    const wx = projectileModel.interpX(j, a);
                    const wy = projectileModel.interpY(j, a);
                    const vx = projectileModel.interpVx(j, a);
                    const vy = projectileModel.interpVy(j, a);
                    drawBullet(ctx, wx, wy, vx, vy);
                }
                ctx.restore();
            }
            Timer {
                interval: 16
                running: true
                repeat: true
                onTriggered: {
                    timingState.update();
                    scene.requestPaint();
                    rootItem.joystick_update();
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
                    let ownIndex = entityModel.count() > 0 ? 0 : -1;
                    if (ownIndex < 0)
                        return;
                    const scale = rootItem.worldToScreenScale;
                    const cx = entityModel.interpX(ownIndex, a);
                    const cy = entityModel.interpY(ownIndex, a);
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
                        const dx = worldX - cx;
                        const dy = worldY - cy;
                        if (Math.abs(dx) > 1e-4 || Math.abs(dy) > 1e-4) {
                            let desiredRad = Math.atan2(dy, dx);
                            let desiredDeg = desiredRad * 180.0 / Math.PI;
                            rootItem.desiredTurretAngleDeg = desiredDeg;
                            let curDeg = entityModel.interpTurretAngle(ownIndex, a);
                            let diff = (desiredDeg - curDeg + 540.0) % 360.0 - 180.0;
                            let turnCmd = 0.0;
                            const deadZone = 1.0;
                            if (Math.abs(diff) > deadZone) {
                                turnCmd = Math.max(-1.0, Math.min(1.0, diff / 45.0));
                            }
                            if (inputState.turretTurn !== turnCmd)
                                inputState.turretTurn = turnCmd;
                        }
                    }
                }
                onCanceled: {
                    rootItem.isMiddleDragging = false;
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
            Column {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                RowLayout {
                    id: tankHeader
                    height: 24
                    spacing: 6
                    anchors.left: parent.left
                    anchors.right: parent.right
                    Text { text: tankPanel.collapsed ? "" : "Tanks"; color: "#c7d4df"; font.pixelSize: 14; visible: !tankPanel.collapsed }
                    Item { Layout.fillWidth: true }
                    Button {
                        id: tankToggle
                        width: 24; height: 24
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
                        onClicked: tankPanel.collapsed = !tankPanel.collapsed
                    }
                }
                ListView {
                    id: tankList
                    visible: !tankPanel.collapsed
                    model: entityModel
                    clip: true
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.top: tankHeader.bottom
                    delegate: Rectangle {
                        width: tankList.width
                        height: 30
                        color: index % 2 === 0 ? "#2b3642" : "#23303a"
                        Row {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 6
                            Text { text: entityId; color: "#d0d3d6"; width: 38 }
                            Text { text: `x:${x.toFixed(1)} y:${y.toFixed(1)}`; color: "#9fb2c3"; width: 138 }
                            Text { text: `hp:${hp} a:${ammo}`; color: "#c5a96a" }
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
            Column {
                anchors.fill: parent
                anchors.margins: 4
                spacing: 4
                RowLayout {
                    id: projHeader
                    height: 24
                    spacing: 6
                    anchors.left: parent.left
                    anchors.right: parent.right
                    Text { text: projectilePanel.collapsed ? "" : "Projectiles"; color: "#c7d4df"; font.pixelSize: 14; visible: !projectilePanel.collapsed }
                    Item { Layout.fillWidth: true }
                    Button {
                        id: projToggle
                        width: 24; height: 24
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
                        onClicked: projectilePanel.collapsed = !projectilePanel.collapsed
                    }
                }
                ListView {
                    id: projectileList
                    visible: !projectilePanel.collapsed
                    model: projectileModel
                    clip: true
                    anchors.left: parent.left
                    anchors.right: parent.right
                    anchors.bottom: parent.bottom
                    anchors.top: projHeader.bottom
                    delegate: Rectangle {
                        width: projectileList.width
                        height: 24
                        color: index % 2 === 0 ? "#303b46" : "#28323d"
                        Row {
                            anchors.fill: parent
                            anchors.margins: 4
                            spacing: 6
                            Text { text: projId; color: "#d0d3d6"; width: 46 }
                            Text { text: `x:${x.toFixed(1)}`; color: "#9fb2c3"; width: 56 }
                            Text { text: `y:${y.toFixed(1)}`; color: "#9fb2c3" }
                        }
                    }
                    ScrollBar.vertical: ScrollBar {}
                }
            }
        }

        // Joystick composite control (two sticks + fire/brake buttons)
        Joystick {
            id: joystick
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
            text: "Snapshot tanks=" + tankList.count + "  follow=" + (rootItem.followCamera ? "on" : "off") + "(G)" + "  grid=" + (rootItem.showGrid ? "on" : "off") + "(H)" + "  mouseAim=" + (rootItem.mouseAimEnabled ? "on" : "off") + "(M)" + "  zoom=" + rootItem.userZoom.toFixed(2) + "(+/- wheel)" + "  focus=" + rootItem.focus
            color: "#d0dde5"
            font.pixelSize: 14
            Rectangle { // backdrop for readability
                anchors.fill: parent
                anchors.margins: -6
                radius: 6
                color: "#101820cc"
                border.color: "#2e4450"
                z: -1
            }
        }
    } // end rootItem
}
