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

    // Unified logging helpers to match C++ logger format: [date time] [qml] [L] message
    function _pad(n) { return n < 10 ? '0'+n : ''+n; }
    function _ts() {
        var d = new Date();
        return d.getFullYear() + '-' + _pad(d.getMonth()+1) + '-' + _pad(d.getDate()) + ' '
             + _pad(d.getHours()) + ':' + _pad(d.getMinutes()) + ':' + _pad(d.getSeconds());
    }
    function logD(msg) { console.debug('['+_ts()+'] [qml] [D] ' + msg); }
    function logI(msg) { console.log('['+_ts()+'] [qml] [I] ' + msg); }
    function logW(msg) { console.warn('['+_ts()+'] [qml] [W] ' + msg); }
    function logE(msg) { console.error('['+_ts()+'] [qml] [E] ' + msg); }

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
    logD(`INPUT mv=${inputState.move} turn=${inputState.turn} turret=${inputState.turretTurn} fire=${inputState.fire}`)
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
    case Qt.Key_G: rootItem.followCamera = !rootItem.followCamera; logD('followCamera='+rootItem.followCamera); break;
    case Qt.Key_H: rootItem.showGrid = !rootItem.showGrid; logD('showGrid='+rootItem.showGrid); break;
    case Qt.Key_M: mouseAimEnabled = !mouseAimEnabled; logD('mouseAimEnabled='+mouseAimEnabled); if (!mouseAimEnabled) { inputState.turretTurn = 0; } break;
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
            const ownIndex = entityModel.count()>0 ? 0 : -1;
            const tankWorldRadius = 1.0;
            const targetScreenRadius = Math.min(width, height) * 0.10;
            const baseScale = targetScreenRadius / tankWorldRadius;
            const scale = baseScale * rootItem.userZoom;
            ctx.save();
            ctx.translate(width/2, height/2);
            if (rootItem.followCamera && ownIndex>=0) {
                ctx.translate(-entityModel.interpX(ownIndex,a), -entityModel.interpY(ownIndex,a));
            } else {
                ctx.translate(-rootItem.cameraOffsetX, -rootItem.cameraOffsetY);
            }
            ctx.scale(scale, scale);
            // Grid
            if (rootItem.showGrid) {
                const gridSpacing = 5; const halfW = width/scale/2; const halfH = height/scale/2;
                ctx.save();
                ctx.strokeStyle = '#23333c'; ctx.lineWidth = 0.02; ctx.beginPath();
                for(let gx=-halfW;gx<=halfW;gx+=gridSpacing){ctx.moveTo(gx,-halfH);ctx.lineTo(gx,halfH);} 
                for(let gy=-halfH;gy<=halfH;gy+=gridSpacing){ctx.moveTo(-halfW,gy);ctx.lineTo(halfW,gy);} 
                ctx.stroke(); ctx.restore();
            }
            function drawRoundedRect(ctx,x,y,w,h,r,fill,stroke,lw){ctx.beginPath();const rr=Math.min(r,w/2,h/2);ctx.moveTo(x+rr,y);ctx.lineTo(x+w-rr,y);ctx.quadraticCurveTo(x+w,y,x+w,y+rr);ctx.lineTo(x+w,y+h-rr);ctx.quadraticCurveTo(x+w,y+h,x+w-rr,y+h);ctx.lineTo(x+rr,y+h);ctx.quadraticCurveTo(x,y+h,x,y+h-rr);ctx.lineTo(x,y+rr);ctx.quadraticCurveTo(x,y,x+rr,y);if(fill){ctx.fillStyle=fill;ctx.fill();}if(stroke){ctx.lineWidth=lw||1;ctx.strokeStyle=stroke;ctx.stroke();}}
            const SPRITE_FRONT_OFFSET = -Math.PI/2; // Protocol: 0° = +X (right). Art points up, so subtract 90°.
            function drawTank(ctx, wx, wy, hullRad, turretRad, isOwn){
                // Convert protocol angle (0°=+X) to sprite angle (0 sprite up) via SPRITE_FRONT_OFFSET.
                const W=480,H=640;const scalePix=6.4/H;
                ctx.save();
                ctx.translate(wx,wy);
                ctx.rotate(hullRad + SPRITE_FRONT_OFFSET);
                ctx.scale(scalePix,scalePix);
                ctx.translate(-W/2,-H/2);
                ctx.fillStyle='#424141';ctx.fillRect(0,0,140,H);ctx.fillRect(342,0,140,H);
                ctx.fillStyle=isOwn?'#5c6e5c':'#6f6e6e';ctx.strokeStyle='#2e2e2e';ctx.lineWidth=2;ctx.beginPath();ctx.rect(28,41,424,558);ctx.fill();ctx.stroke();
                ctx.fillStyle='#9b0101';ctx.fillRect(46,576,80,23);ctx.fillRect(358,576,80,23);
                drawRoundedRect(ctx,65,49,43,30,15,'#f1f0f0');drawRoundedRect(ctx,377,49,43,30,15,'#f1f0f0');
                ctx.save();
                ctx.translate(W/2,H/2);
                ctx.rotate(turretRad - hullRad); // both already protocol-space; relative rotation unaffected by offset
                ctx.translate(-W/2,-H/2);
                drawRoundedRect(ctx,140,195,200,250,32,'#bfbfbf','#363434',8);
                ctx.lineWidth=8;ctx.fillStyle='#bfbfbf';ctx.strokeStyle='#363434';ctx.beginPath();ctx.rect((W-30)/2,-80,30,320);ctx.fill();ctx.stroke();
                ctx.restore();
                const hpY=31;ctx.fillStyle='#202828';ctx.fillRect(28,hpY,424,6);ctx.fillStyle=isOwn?'#6cff5d':'#3fa7ff';ctx.fillRect(28,hpY,424,6);
                ctx.restore();
            }
            function drawBullet(ctx, wx, wy, vx, vy){
                // Legacy bullet 60x20 px mapped to world 0.45x0.15 -> scale factors below
                const PX_W=60, PX_H=20; const WORLD_W=0.45, WORLD_H=0.15;
                const sx = WORLD_W / PX_W; const sy = WORLD_H / PX_H;
                const ang=Math.atan2(vy,vx);
                ctx.save();
                ctx.translate(wx,wy);
                ctx.rotate(ang);
                ctx.scale(sx, sy);
                ctx.translate(-PX_W/2,-PX_H/2);
                const grad=ctx.createLinearGradient(0,0,PX_W,0);
                grad.addColorStop(0,'#2c2c2c');
                grad.addColorStop(0.445,'#d0d0d1');
                grad.addColorStop(0.61,'#d0d0d1');
                grad.addColorStop(1,'#2c2c2c');
                ctx.fillStyle=grad;
                ctx.fillRect(0,0,PX_W,PX_H);
                ctx.restore();
            }
            for(let i=0;i<entityModel.count();++i){const wx=entityModel.interpX(i,a);const wy=entityModel.interpY(i,a);const hullRad=entityModel.interpHullAngle(i,a)*Math.PI/180.0;const turretRad=entityModel.interpTurretAngle(i,a)*Math.PI/180.0;drawTank(ctx,wx,wy,hullRad,turretRad,i===ownIndex);} 
            for(let j=0;j<projectileModel.count();++j){const wx=projectileModel.interpX(j,a);const wy=projectileModel.interpY(j,a);const vx=projectileModel.interpVx(j,a);const vy=projectileModel.interpVy(j,a);drawBullet(ctx,wx,wy,vx,vy);} 
            ctx.restore();
        }
        Timer { interval: 16; running: true; repeat: true; onTriggered: { timingState.update(); scene.requestPaint(); } }
        MouseArea { // retain interaction overlay
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.RightButton | Qt.MiddleButton
            onWheel: function(ev) { if (ev.angleDelta.y > 0) rootItem.userZoom /= 0.9; else rootItem.userZoom *= 0.9; rootItem.userZoom = Math.max(0.1, Math.min(5.0, rootItem.userZoom)); ev.accepted = true; }
            onPressed: function(ev) { if (ev.button === Qt.RightButton) { rootItem.followCamera = !rootItem.followCamera; if (rootItem.followCamera) { rootItem.cameraOffsetX = 0; rootItem.cameraOffsetY = 0; } } else if (ev.button === Qt.MiddleButton) { rootItem.isMiddleDragging = true; rootItem.dragStartX = ev.x; rootItem.dragStartY = ev.y; rootItem.dragOrigOffsetX = rootItem.cameraOffsetX; rootItem.dragOrigOffsetY = rootItem.cameraOffsetY; } else if (ev.button === Qt.LeftButton) { inputState.fire = true; } }
            onReleased: function(ev) { if (ev.button === Qt.MiddleButton) { rootItem.isMiddleDragging = false; } else if (ev.button === Qt.LeftButton) { inputState.fire = false; } }
            onPositionChanged: function(ev) { const a = timingState.alpha; let ownIndex = entityModel.count()>0?0:-1; if (ownIndex<0)return; const baseScale = Math.min(width,height)*0.10/1.0; const scale = baseScale * rootItem.userZoom; const cx = entityModel.interpX(ownIndex,a); const cy = entityModel.interpY(ownIndex,a); let worldX = (ev.x - width/2)/scale; let worldY = (ev.y - height/2)/scale; if (rootItem.followCamera){ worldX += cx; worldY += cy; } else { worldX += rootItem.cameraOffsetX; worldY += rootItem.cameraOffsetY; } if (rootItem.isMiddleDragging && !rootItem.followCamera){ let startWorldX = (rootItem.dragStartX - width/2)/scale + rootItem.cameraOffsetX; let startWorldY = (rootItem.dragStartY - height/2)/scale + rootItem.cameraOffsetY; let dxWorld = worldX - startWorldX; let dyWorld = worldY - startWorldY; rootItem.cameraOffsetX = rootItem.dragOrigOffsetX - dxWorld; rootItem.cameraOffsetY = rootItem.dragOrigOffsetY - dyWorld; } if (rootItem.mouseAimEnabled){ const dx = worldX - cx; const dy = worldY - cy; if (Math.abs(dx)>1e-4 || Math.abs(dy)>1e-4){ let desiredRad = Math.atan2(dy,dx); let desiredDeg = desiredRad*180.0/Math.PI; rootItem.desiredTurretAngleDeg = desiredDeg; let curDeg = entityModel.interpTurretAngle(ownIndex,a); let diff = (desiredDeg - curDeg + 540.0)%360.0 - 180.0; let turnCmd = 0.0; const deadZone = 1.0; if (Math.abs(diff)>deadZone){ turnCmd = Math.max(-1.0, Math.min(1.0, diff/45.0)); } if (inputState.turretTurn !== turnCmd) inputState.turretTurn = turnCmd; } } }
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
