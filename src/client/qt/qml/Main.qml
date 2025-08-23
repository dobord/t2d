// SPDX-License-Identifier: Apache-2.0
import QtQuick 2.15
import QtQuick.Window 2.15
import QtQuick.Controls 2.15

Window {
    id: root
    width: 800; height: 600
    visible: true
    title: "t2d Qt Client"

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
            // draw tanks
            for(let i=0;i<entityModel.count();++i){
                const ixp = entityModel.interpX(i,a);
                const iyp = entityModel.interpY(i,a);
                ctx.fillStyle = '#3fa7ff';
                ctx.beginPath(); ctx.arc(ixp, iyp, 6, 0, Math.PI*2); ctx.fill();
            }
            // draw projectiles
            ctx.fillStyle = '#ffcf40';
            for(let j=0;j<projectileModel.count();++j){
                const ixp = projectileModel.interpX(j,a);
                const iyp = projectileModel.interpY(j,a);
                ctx.fillRect(ixp-2, iyp-2, 4, 4);
            }
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
}
