// SPDX-License-Identifier: Apache-2.0
import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15

Window {
    id: root
    width: 800; height: 600
    visible: true
    title: "t2d Qt Client"
    Component.onCompleted: root.requestActivate()

    // Global key handlers as fallback if inner item loses focus
    Keys.onPressed: function(ev) {
        if (!rootItem.focus) rootItem.forceActiveFocus();
        rootItem.Keys.onPressed(ev);
    }
    Keys.onReleased: function(ev) { rootItem.Keys.onReleased(ev); }

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
    // Internal cached last computed desired turret angle (degrees)
    property real desiredTurretAngleDeg: 0
    Component.onCompleted: { rootItem.forceActiveFocus(); }

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
        var mv = 0
        if (keyW && !keyS) mv = 1
        else if (keyS && !keyW) mv = -1
        if (inputState.move !== mv) inputState.move = mv
        // Hull turn
        var tr = 0
        if (keyD && !keyA) tr = 1
        else if (keyA && !keyD) tr = -1
        if (inputState.turn !== tr) inputState.turn = tr
        // Turret turn
        var tt = 0
        if (!mouseAimEnabled) { // keyboard turret control only when mouse aim disabled
            if (keyE && !keyQ) tt = 1
            else if (keyQ && !keyE) tt = -1
        }
        if (inputState.turretTurn !== tt) inputState.turretTurn = tt
    // Fire (held space)
        var fr = keySpace
        if (inputState.fire !== fr) inputState.fire = fr
    // Brake (held Shift)
    var br = keyShift
    if (inputState.brake !== br) inputState.brake = br
    // Debug log
    console.debug(`INPUT mv=${inputState.move} turn=${inputState.turn} turret=${inputState.turretTurn} fire=${inputState.fire}`)
    }

    Keys.onPressed: function(ev) {
        switch(ev.key) {
        case Qt.Key_W: keyW = true; break;
        case Qt.Key_S: keyS = true; break;
        case Qt.Key_A: keyA = true; break;
        case Qt.Key_D: keyD = true; break;
        case Qt.Key_Q: keyQ = true; break;
        case Qt.Key_E: keyE = true; break;
    case Qt.Key_Space: keySpace = true; break;
    case Qt.Key_Shift: keyShift = true; break;
    case Qt.Key_G: rootItem.followCamera = !rootItem.followCamera; console.debug('followCamera='+rootItem.followCamera); break;
    case Qt.Key_H: rootItem.showGrid = !rootItem.showGrid; console.debug('showGrid='+rootItem.showGrid); break;
    case Qt.Key_M: mouseAimEnabled = !mouseAimEnabled; console.debug('mouseAimEnabled='+mouseAimEnabled); if (!mouseAimEnabled) { inputState.turretTurn = 0; } break;
    case Qt.Key_Plus:
    case Qt.Key_Equal: userZoom /= 0.9; userZoom = Math.min(userZoom, 5.0); break;
    case Qt.Key_Minus: userZoom *= 0.9; userZoom = Math.max(userZoom, 0.1); break;
        default: return;
        }
        ev.accepted = true;
        recomputeInput();
    }

    Keys.onReleased: function(ev) {
        switch(ev.key) {
        case Qt.Key_W: keyW = false; break;
        case Qt.Key_S: keyS = false; break;
        case Qt.Key_A: keyA = false; break;
        case Qt.Key_D: keyD = false; break;
        case Qt.Key_Q: keyQ = false; break;
        case Qt.Key_E: keyE = false; break;
    case Qt.Key_Space: keySpace = false; break;
    case Qt.Key_Shift: keyShift = false; break;
        default: return;
        }
        ev.accepted = true;
        recomputeInput();
    }

    Rectangle { anchors.fill: parent; color: "#202830" }

    Canvas {
        id: scene
        anchors { left: tankList.right; right: projectileList.left; top: parent.top; bottom: controls.top; leftMargin: 8; rightMargin: 8; topMargin: 8; bottomMargin: 8 }
        onPaint: {
            const ctx = getContext('2d');
            ctx.reset();
            ctx.fillStyle = '#162028';
            ctx.fillRect(0,0,width,height);
            const a = timingState.alpha;
            // Determine own tank (heuristic: first tank id if list non-empty). Real client should match session id.
            let ownIndex = entityModel.count() > 0 ? 0 : -1;
            // World units currently arbitrary; assume tank nominal radius = 1 world unit.
            const tankWorldRadius = 1.0;
            const targetScreenRadius = Math.min(width, height) * 0.10; // 10% of min dimension
            const baseScale = targetScreenRadius / tankWorldRadius;
            const scale = baseScale * rootItem.userZoom;
            let centerX = 0;
            let centerY = 0;
            if (ownIndex >= 0) {
                centerX = entityModel.interpX(ownIndex,a);
                centerY = entityModel.interpY(ownIndex,a);
            }
            // Transform: translate so own tank is centered, then scale.
            ctx.save();
            ctx.translate(width/2, height/2);
            ctx.scale(scale, scale);
            if (rootItem.followCamera) {
                ctx.translate(-centerX, -centerY);
            } else {
                ctx.translate(-rootItem.cameraOffsetX, -rootItem.cameraOffsetY);
            }
            // Optional background grid for movement perception
            if (rootItem.showGrid) {
                const gridSpacing = 5; // world units
                const halfW = width/scale/2;
                const halfH = height/scale/2;
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
            // Draw tanks (composite hull + turret)
            for(let i=0;i<entityModel.count();++i){
                const wx = entityModel.interpX(i,a);
                const wy = entityModel.interpY(i,a);
                const hullDeg = entityModel.interpHullAngle(i,a);
                const turretDeg = entityModel.interpTurretAngle(i,a);
                const hullRad = hullDeg * Math.PI/180.0;
                const turretRad = turretDeg * Math.PI/180.0;
                ctx.save();
                ctx.translate(wx, wy);
                ctx.rotate(hullRad);
                ctx.fillStyle = '#202a32';
                const trackLen = 6.4; const trackWidth = 0.35; const trackOffset = 2.4 - trackWidth/2;
                ctx.fillRect(-trackLen/2, trackOffset - trackWidth/2, trackLen, trackWidth);
                ctx.fillRect(-trackLen/2, -trackOffset - trackWidth/2, trackLen, trackWidth);
                ctx.fillStyle = (i===ownIndex)? '#6cff5d' : '#3fa7ff';
                ctx.beginPath();
                ctx.moveTo(-3.2, -2.4);
                ctx.lineTo(-3.2, -1.0);
                ctx.lineTo(-2.79, -1.0);
                ctx.lineTo(-2.79, 1.0);
                ctx.lineTo(-3.2, 1.0);
                ctx.lineTo(-3.2, 2.4);
                ctx.lineTo(3.2, 2.4);
                ctx.lineTo(3.2, 1.0);
                ctx.lineTo(2.79, 1.0);
                ctx.lineTo(2.79, -1.0);
                ctx.lineTo(3.2, -1.0);
                ctx.lineTo(3.2, -2.4);
                ctx.closePath();
                ctx.fill();
                ctx.save();
                ctx.rotate(turretRad - hullRad);
                ctx.fillStyle = '#44525d';
                ctx.beginPath(); ctx.arc(0,0,1.2,0,Math.PI*2); ctx.fill();
                ctx.fillStyle = '#cccccc';
                ctx.fillRect(0, -0.05, 3.3, 0.1);
                ctx.restore();
                ctx.restore();
            }
            // Draw projectiles (assume small square 0.3 world units)
            ctx.fillStyle = '#ffcf40';
            const pr = 0.3;
            for(let j=0;j<projectileModel.count();++j){
                const wx = projectileModel.interpX(j,a);
                const wy = projectileModel.interpY(j,a);
                ctx.fillRect(wx-pr, wy-pr, pr*2, pr*2);
            }
            ctx.restore();
        }
        Timer { interval: 16; running: true; repeat: true; onTriggered: { timingState.update(); scene.requestPaint(); } }

        // Mouse interaction overlay for aiming, zoom, and panning
        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            onWheel: function(ev) {
                if (ev.angleDelta.y > 0) rootItem.userZoom /= 0.9; else rootItem.userZoom *= 0.9;
                rootItem.userZoom = Math.max(0.1, Math.min(5.0, rootItem.userZoom));
                ev.accepted = true;
            }
            onPressed: function(ev) {
                if (ev.button === Qt.RightButton) {
                    // toggle follow camera
                    rootItem.followCamera = !rootItem.followCamera;
                    if (rootItem.followCamera) { rootItem.cameraOffsetX = 0; rootItem.cameraOffsetY = 0; }
                } else if (ev.button === Qt.MiddleButton) {
                    rootItem.isMiddleDragging = true;
                    rootItem.dragStartX = ev.x; rootItem.dragStartY = ev.y;
                    rootItem.dragOrigOffsetX = rootItem.cameraOffsetX;
                    rootItem.dragOrigOffsetY = rootItem.cameraOffsetY;
                } else if (ev.button === Qt.LeftButton) {
                    // left button can act as fire (hold)
                    inputState.fire = true;
                }
            }
            onReleased: function(ev) {
                if (ev.button === Qt.MiddleButton) {
                    rootItem.isMiddleDragging = false;
                } else if (ev.button === Qt.LeftButton) {
                    inputState.fire = false;
                }
            }
            onPositionChanged: function(ev) {
                const a = timingState.alpha;
                let ownIndex = entityModel.count() > 0 ? 0 : -1;
                if (ownIndex < 0) return;
                // World transform parameters must match paint() logic
                const tankWorldRadius = 1.0;
                const targetScreenRadius = Math.min(width, height) * 0.10;
                const baseScale = targetScreenRadius / tankWorldRadius;
                const scale = baseScale * rootItem.userZoom;
                const cx = entityModel.interpX(ownIndex,a);
                const cy = entityModel.interpY(ownIndex,a);
                // Inverse transform from screen to world
                let worldX = (ev.x - width/2) / scale;
                let worldY = (ev.y - height/2) / scale;
                if (rootItem.followCamera) {
                    worldX += cx;
                    worldY += cy;
                } else {
                    worldX += rootItem.cameraOffsetX;
                    worldY += rootItem.cameraOffsetY;
                }
                if (rootItem.isMiddleDragging && !rootItem.followCamera) {
                    // Update pan offsets; compute delta in world units relative to drag start
                    let startWorldX = (rootItem.dragStartX - width/2) / scale + rootItem.cameraOffsetX;
                    let startWorldY = (rootItem.dragStartY - height/2) / scale + rootItem.cameraOffsetY;
                    let dxWorld = worldX - startWorldX;
                    let dyWorld = worldY - startWorldY;
                    rootItem.cameraOffsetX = rootItem.dragOrigOffsetX - dxWorld;
                    rootItem.cameraOffsetY = rootItem.dragOrigOffsetY - dyWorld;
                }
                if (rootItem.mouseAimEnabled) {
                    // Compute desired turret angle
                    const dx = worldX - cx;
                    const dy = worldY - cy;
                    if (Math.abs(dx) > 1e-4 || Math.abs(dy) > 1e-4) {
                        let desiredRad = Math.atan2(dy, dx);
                        let desiredDeg = desiredRad * 180.0 / Math.PI;
                        rootItem.desiredTurretAngleDeg = desiredDeg;
                        // Current turret angle
                        let curDeg = entityModel.interpTurretAngle(ownIndex,a);
                        // Shortest diff
                        let diff = (desiredDeg - curDeg + 540.0) % 360.0 - 180.0;
                        let turnCmd = 0.0;
                        const deadZone = 1.0; // degrees
                        if (Math.abs(diff) > deadZone) {
                            // Scale to [-1,1] relative to 45 deg error
                            turnCmd = Math.max(-1.0, Math.min(1.0, diff / 45.0));
                        }
                        if (inputState.turretTurn !== turnCmd) inputState.turretTurn = turnCmd;
                    }
                }
            }
            onCanceled: { rootItem.isMiddleDragging = false; }
        }
    }

    ListView {
        id: tankList
        anchors { left: parent.left; top: parent.top; bottom: parent.bottom; leftMargin: 8; topMargin: 8; bottomMargin: 8 }
        width: 260
        model: entityModel
        clip: true
        delegate: Rectangle {
            width: tankList.width; height: 32
            color: index % 2 === 0 ? "#2b3642" : "#23303a"
            Row {
                anchors.fill: parent; anchors.margins: 4; spacing: 8
                Text { text: entityId; color: "#d0d3d6"; width: 40 }
                Text { text: `x:${x.toFixed(1)} y:${y.toFixed(1)}`; color: "#9fb2c3"; width: 140 }
                Text { text: `hp:${hp} a:${ammo}`; color: "#c5a96a" }
            }
        }
        ScrollBar.vertical: ScrollBar {}
    }

    ListView {
        id: projectileList
        anchors { right: parent.right; top: parent.top; bottom: parent.bottom; rightMargin: 8; topMargin: 8; bottomMargin: 120 }
        width: 180
        model: projectileModel
        delegate: Rectangle {
            width: projectileList.width; height: 24
            color: index % 2 === 0 ? "#303b46" : "#28323d"
            Row { anchors.fill: parent; anchors.margins: 4; spacing: 6
                Text { text: projId; color: "#d0d3d6"; width: 50 }
                Text { text: `x:${x.toFixed(1)}`; color: "#9fb2c3"; width: 60 }
                Text { text: `y:${y.toFixed(1)}`; color: "#9fb2c3" }
            }
        }
        ScrollBar.vertical: ScrollBar {}
    }

    Rectangle {
        id: controls
        anchors { left: parent.left; right: parent.right; bottom: parent.bottom; leftMargin: 8; rightMargin: 8; bottomMargin: 8 }
        height: 100
        radius: 6
        color: "#25303a"
        Row {
            anchors.fill: parent; anchors.margins: 8; spacing: 12
            Column {
                spacing: 4
                Text { text: "Move"; color: "#c0c8cf" }
                Slider { from: -1; to: 1; value: inputState.move; onValueChanged: inputState.move = value; width: 140 }
            }
            Column {
                spacing: 4
                Text { text: "Turn"; color: "#c0c8cf" }
                Slider { from: -1; to: 1; value: inputState.turn; onValueChanged: inputState.turn = value; width: 140 }
            }
            Column {
                spacing: 4
                Text { text: "Turret"; color: "#c0c8cf" }
                Slider { from: -1; to: 1; value: inputState.turretTurn; onValueChanged: inputState.turretTurn = value; width: 140 }
            }
            Column {
                spacing: 4
                Text { text: "Fire"; color: "#c0c8cf" }
                CheckBox { checked: inputState.fire; onToggled: inputState.fire = checked }
            }
            Column {
                spacing: 4
                Text { text: `Tanks: ${tankList.count}`; color: "#9fb2c3" }
                Text { text: `Projectiles: ${projectileList.count}`; color: "#9fb2c3" }
            }
        }
    }

    Text {
        anchors.bottom: parent.bottom
        anchors.right: parent.right
        anchors.bottomMargin: 112
        anchors.rightMargin: 12
    text: "Snapshot tanks=" + tankList.count
          + "  follow=" + (rootItem.followCamera?"on":"off") + "(G)"
          + "  grid=" + (rootItem.showGrid?"on":"off") + "(H)"
          + "  mouseAim=" + (rootItem.mouseAimEnabled?"on":"off") + "(M)"
          + "  zoom=" + rootItem.userZoom.toFixed(2) + "(+/- wheel)"
          + "  focus=" + rootItem.focus
        color: "#8098a8"
        font.pixelSize: 14
    }
    } // end rootItem
}
