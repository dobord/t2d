// SPDX-License-Identifier: Apache-2.0
import QtQuick 2.15

Rectangle {
    id: custom_button
    radius: 16
    property alias captionFontPointSize: caption.font.pointSize
    property alias captionFontFamily: caption.font.family
    property alias text: caption.text
    property int touchCount: 0 + point1.pressed + point2.pressed + point3.pressed
    property bool buttonPressed: ma.pressed || (touchCount > 0)

    signal click
    signal click2
    signal click3
    signal pressed
    signal released

    border.width: 2
    border.color: "#dddddd"

    Behavior on border.color {
        ColorAnimation {
            duration: 200
        }
    }

    onButtonPressedChanged: {
        if (buttonPressed)
            pressed();
        else
            released();
    }

    gradient: Gradient {
        GradientStop {
            position: 0
            color: "#ffffff"
        }
        GradientStop {
            position: 0.7
            color: "#c2c2c2"
        }
        GradientStop {
            id: gradientStop
            position: 1
            color: "#c2c2c2"
            Behavior on color {
                ColorAnimation {
                    duration: 200
                }
            }
        }
    }

    Text {
        id: caption
        font.family: "Arial"
        font.pointSize: 14
        text: "Button"
        anchors.centerIn: parent
    }

    MouseArea {
        id: ma
        anchors.fill: parent
        hoverEnabled: true
        acceptedButtons: Qt.AllButtons
        onReleased: function (ev) {
            switch (ev.button) {
            case Qt.LeftButton:
                click();
                break;
            case Qt.RightButton:
                click2();
                break;
            case Qt.MiddleButton:
                click3();
                break;
            }
        }
    }

    MultiPointTouchArea {
        id: mpta
        anchors.fill: parent
        maximumTouchPoints: 3
        mouseEnabled: false
        function clickHandler(n) {
            switch (n) {
            case 1:
                click();
                break;
            case 2:
                click2();
                break;
            case 3:
                click3();
                break;
            }
        }
        touchPoints: [
            TouchPoint {
                id: point1
                onPressedChanged: {
                    if (!pressed)
                        mpta.clickHandler(1 + point1.pressed + point2.pressed + point3.pressed);
                }
            },
            TouchPoint {
                id: point2
                onPressedChanged: {
                    if (!pressed)
                        mpta.clickHandler(1 + point1.pressed + point2.pressed + point3.pressed);
                }
            },
            TouchPoint {
                id: point3
                onPressedChanged: {
                    if (!pressed)
                        mpta.clickHandler(1 + point1.pressed + point2.pressed + point3.pressed);
                }
            }
        ]
    }

    states: [
        State {
            name: "default"
            when: !ma.containsMouse && (!ma.pressed) && (touchCount == 0)
            PropertyChanges {
                target: custom_button
                border.color: "#dddddd"
            }
            PropertyChanges {
                target: gradientStop
                color: "#c2c2c2"
            }
        },
        State {
            name: "hover"
            when: ma.containsMouse && (!ma.pressed) && (touchCount == 0)
            PropertyChanges {
                target: custom_button
                border.color: "#ffd700"
            }
            PropertyChanges {
                target: gradientStop
                color: "#c2c2c2"
            }
        },
        State {
            name: "pressed_1"
            when: (ma.pressedButtons == Qt.LeftButton) || (touchCount == 1)
            PropertyChanges {
                target: custom_button
                border.color: "#ffd700"
            }
            PropertyChanges {
                target: gradientStop
                color: "#fffcda"
            }
        },
        State {
            name: "pressed_2"
            when: (ma.pressedButtons == Qt.RightButton) || (touchCount == 2)
            PropertyChanges {
                target: custom_button
                border.color: "#ffd700"
            }
            PropertyChanges {
                target: gradientStop
                color: "#dbffda"
            }
        },
        State {
            name: "pressed_3"
            when: (ma.pressedButtons == Qt.MiddleButton) || (touchCount == 3)
            PropertyChanges {
                target: custom_button
                border.color: "#ffd700"
            }
            PropertyChanges {
                target: gradientStop
                color: "#dae6ff"
            }
        }
    ]
}
