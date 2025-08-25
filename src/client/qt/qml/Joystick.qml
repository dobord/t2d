// SPDX-License-Identifier: Apache-2.0
import QtQuick 2.15

Item {
    id: joy
    // Width stretches with parent anchors; height externally constrained
    anchors.left: parent.left
    anchors.right: parent.right
    // Height provided by parent (e.g., 180)
    // Transparent overlay; inner elements partly translucent.
    width: parent ? parent.width : 640

    // Normalized drive (-1..1) and target axes (aim) exported
    property real drive_x: ((joy_left.x + 0.5 * joy_left.width) / mpta_left.width) * 2.0 - 1.0
    property real drive_y: -(((joy_left.y + 0.5 * joy_left.height) / mpta_left.height) * 2.0 - 1.0)
    property bool drive_empty: (Math.abs(drive_x) < 0.001) && (Math.abs(drive_y) < 0.001)

    property real target_x: ((joy_right.x + 0.5 * joy_right.width) / mpta_right.width) * 2.0 - 1.0
    property real target_y: -(((joy_right.y + 0.5 * joy_right.height) / mpta_right.height) * 2.0 - 1.0)
    property bool target_empty: (Math.abs(target_x) < 0.001) && (Math.abs(target_y) < 0.001)

    signal button_state(string name, bool pressed)

    ListModel {
        id: button_model
        ListElement {
            name: "fire"
            title: "="
        }
        ListElement {
            name: "brake"
            title: "O"
        }
    }

    // Buttons stacked above right stick, not spanning center area
    Column {
        id: buttonColumn
        spacing: 8
        anchors.right: mpta_right.left
        anchors.rightMargin: 16
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 12
        Repeater {
            model: button_model
            delegate: CustomButton {
                id: bt
                width: 56
                height: 56
                radius: 14
                text: title
                opacity: 0.85
                onButtonPressedChanged: joy.button_state(name, buttonPressed)
            }
        }
    }

    // Left stick (drive)
    MultiPointTouchArea {
        id: mpta_left
        width: 150
        height: 150
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 0
        anchors.left: parent.left
        anchors.leftMargin: 0
        maximumTouchPoints: 1
        touchPoints: [
            TouchPoint {
                id: point_joy_left
            }
        ]

        // Replace image assets with simple text arrows to avoid external resource dependency
        Text {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: "⟲"
            color: "#8098a8"
            font.pixelSize: 18
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: "⟳"
            color: "#8098a8"
            font.pixelSize: 18
        }
        Text {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            anchors.horizontalCenter: parent.horizontalCenter
            text: "↓"
            color: "#8098a8"
            font.pixelSize: 18
        }
        Text {
            anchors.top: parent.top
            anchors.topMargin: 2
            anchors.horizontalCenter: parent.horizontalCenter
            text: "↑"
            color: "#8098a8"
            font.pixelSize: 18
        }

        Rectangle {
            id: joy_left
            x: (point_joy_left.pressed ? Math.max(0, Math.min(point_joy_left.x, parent.width)) : parent.width / 2) - width / 2
            y: (point_joy_left.pressed ? Math.max(0, Math.min(point_joy_left.y, parent.height)) : parent.height / 2) - height / 2
            width: 64
            height: 64
            radius: 16
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: "#ffffff"
                }
                GradientStop {
                    position: 1
                    color: "#c2c2c2"
                }
            }
            Text {
                anchors.centerIn: parent
                font.pointSize: 14
                text: "Drive"
            }
            border.width: 2
            border.color: point_joy_left.pressed ? "gold" : "#ddd"
        }
    }

    // Right stick (aim)
    MultiPointTouchArea {
        id: mpta_right
        width: 150
        height: 150
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 0
        anchors.right: parent.right
        anchors.rightMargin: 0
        maximumTouchPoints: 1
        touchPoints: [
            TouchPoint {
                id: point_joy_right
            }
        ]

        Text {
            anchors.left: parent.left
            anchors.leftMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: "←"
            color: "#8098a8"
            font.pixelSize: 18
        }
        Text {
            anchors.top: parent.top
            anchors.topMargin: 2
            anchors.horizontalCenter: parent.horizontalCenter
            text: "↑"
            color: "#8098a8"
            font.pixelSize: 18
        }
        Text {
            anchors.bottom: parent.bottom
            anchors.bottomMargin: 2
            anchors.horizontalCenter: parent.horizontalCenter
            text: "↓"
            color: "#8098a8"
            font.pixelSize: 18
        }
        Text {
            anchors.right: parent.right
            anchors.rightMargin: 4
            anchors.verticalCenter: parent.verticalCenter
            text: "→"
            color: "#8098a8"
            font.pixelSize: 18
        }

        Rectangle {
            id: joy_right
            x: (point_joy_right.pressed ? Math.max(0, Math.min(point_joy_right.x, parent.width)) : parent.width / 2) - width / 2
            y: (point_joy_right.pressed ? Math.max(0, Math.min(point_joy_right.y, parent.height)) : parent.height / 2) - height / 2
            width: 64
            height: 64
            radius: 16
            gradient: Gradient {
                GradientStop {
                    position: 0
                    color: "#ffffff"
                }
                GradientStop {
                    position: 1
                    color: "#c2c2c2"
                }
            }
            Text {
                anchors.centerIn: parent
                font.pointSize: 14
                text: "Target"
            }
            border.width: 2
            border.color: point_joy_right.pressed ? "gold" : "#ddd"
        }
    }
}
