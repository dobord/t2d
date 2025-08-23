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

    Item { // focusable container for key handling
        id: rootItem
        anchors.fill: parent
        focus: true

    // Keyboard state flags (simple set of currently pressed movement keys)
    property bool keyW: false
    property bool keyS: false
    property bool keyA: false
    property bool keyD: false
    property bool keyQ: false   // turret left
    property bool keyE: false   // turret right
    property bool keySpace: false

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
        if (keyE && !keyQ) tt = 1
        else if (keyQ && !keyE) tt = -1
        if (inputState.turretTurn !== tt) inputState.turretTurn = tt
        // Fire (held space)
        var fr = keySpace
        if (inputState.fire !== fr) inputState.fire = fr
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
            const scale = targetScreenRadius / tankWorldRadius;
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
            ctx.translate(-centerX, -centerY);
            // Draw tanks with geometry: hull 3x6 (width x length), tracks 6x0.3, turret radius 1.3, barrel 4x0.1
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
                // Tracks: length 6 along X, width 0.3 along Y. Centered at +/- (1.5 - 0.15) = 1.35
                ctx.fillStyle = '#202a32';
                const trackLen = 6.0; const trackWidth = 0.3; const halfHullWidth = 1.5; const trackOffset = 1.35;
                ctx.fillRect(-trackLen/2, trackOffset - trackWidth/2, trackLen, trackWidth); // upper (+Y)
                ctx.fillRect(-trackLen/2, -trackOffset - trackWidth/2, trackLen, trackWidth); // lower (-Y)
                // Hull body
                ctx.fillStyle = (i===ownIndex)? '#6cff5d' : '#3fa7ff';
                ctx.fillRect(-3.0, -1.5, 6.0, 3.0); // length 6 along X, width 3 along Y (centered)
                // Turret (independent rotation around center)
                ctx.save();
                ctx.rotate(turretRad - hullRad); // apply relative rotation
                ctx.fillStyle = '#44525d';
                ctx.beginPath(); ctx.arc(0,0,1.3,0,Math.PI*2); ctx.fill();
                // Barrel
                ctx.fillStyle = '#cccccc';
                ctx.fillRect(0, -0.05, 4.0, 0.1);
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
        color: "#8098a8"
        font.pixelSize: 14
    }
    } // end rootItem
}
