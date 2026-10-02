pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import RevDash 1.0

Window {
    id: root
    required property var controller
    width: 1180
    height: 720
    minimumWidth: 720
    minimumHeight: 480
    visible: true
    title: qsTr("RevDash")
    color: root.controller.darkTheme ? "#101418" : "#eef2f5"

    readonly property color panelColor: root.controller.darkTheme ? "#192127" : "#ffffff"
    readonly property color primaryText: root.controller.darkTheme ? "#f4f7f8" : "#172027"
    readonly property color secondaryText: root.controller.darkTheme ? "#9aa8b1" : "#586872"
    readonly property color accent: "#39d98a"
    property bool compact: width < 900

    Rectangle {
        id: header
        anchors { left: parent.left; right: parent.right; top: parent.top }
        height: 72
        color: root.panelColor

        Text {
            anchors { left: parent.left; leftMargin: 28; verticalCenter: parent.verticalCenter }
            text: qsTr("REVDASH")
            color: root.primaryText
            font.pixelSize: 22
            font.bold: true
            font.letterSpacing: 2
        }
        Text { anchors.centerIn: parent; text: qsTr("Connection: %1").arg(root.controller.connectionState); color: root.secondaryText; font.pixelSize: 14 }

        Row {
            anchors { right: parent.right; rightMargin: 24; verticalCenter: parent.verticalCenter }
            spacing: 10
            Text { text: qsTr("Metric"); color: root.secondaryText; anchors.verticalCenter: parent.verticalCenter }
            Rectangle {
                width: 46; height: 24; radius: 12; color: units.containsMouse ? root.accent : root.secondaryText
                Rectangle { width: 18; height: 18; radius: 9; y: 3; x: units.imperial ? 25 : 3; color: root.panelColor }
                MouseArea { id: units; anchors.fill: parent; property bool imperial: false; onClicked: { imperial = !imperial; root.controller.setImperial(imperial) } }
            }
            Text { text: qsTr("Imperial"); color: root.secondaryText; anchors.verticalCenter: parent.verticalCenter }
            Rectangle {
                width: 86; height: 34; radius: 6; color: root.color; border.color: root.secondaryText
                Text { anchors.centerIn: parent; text: root.controller.darkTheme ? qsTr("Light") : qsTr("Dark"); color: root.primaryText }
                MouseArea { anchors.fill: parent; onClicked: root.controller.setDarkTheme(!root.controller.darkTheme) }
            }
        }
    }

    Item {
        anchors { top: header.bottom; bottom: parent.bottom; left: parent.left; right: parent.right; margins: 24 }

        GridView {
            id: metrics
            anchors { top: parent.top; left: parent.left; right: root.compact ? parent.right : chartPanel.left; bottom: parent.bottom; rightMargin: root.compact ? 0 : 24 }
            cellWidth: Math.max(190, width / Math.max(1, Math.floor(width / 220)))
            cellHeight: 110
            clip: true
            model: root.controller.telemetryModel
            delegate: Item {
                id: metricDelegate
                required property string name
                required property real value
                required property string unit
                required property bool valid
                width: metrics.cellWidth; height: metrics.cellHeight
                Rectangle {
                    anchors { fill: parent; margins: 6 }
                    radius: 10
                    color: root.panelColor
                    Text {
                        anchors { left: parent.left; leftMargin: 16; top: parent.top; topMargin: 14 }
                        text: metricDelegate.name
                        color: root.secondaryText
                        font.pixelSize: 13
                    }
                    Text {
                        anchors { left: parent.left; leftMargin: 16; bottom: parent.bottom; bottomMargin: 15 }
                        text: metricDelegate.valid ? Number(metricDelegate.value).toLocaleString(Qt.locale(), 'f', 1) : "--"
                        color: metricDelegate.valid ? root.primaryText : root.secondaryText
                        font.pixelSize: 28
                        font.bold: true
                    }
                    Text {
                        anchors { right: parent.right; rightMargin: 16; bottom: parent.bottom; bottomMargin: 20 }
                        text: metricDelegate.unit
                        color: root.secondaryText
                        font.pixelSize: 13
                    }
                }
            }
        }

        Rectangle {
            id: chartPanel
            visible: !root.compact
            width: Math.min(420, parent.width * 0.38)
            anchors { top: parent.top; right: parent.right; bottom: parent.bottom }
            radius: 10; color: root.panelColor
            Text {
                id: chartTitle
                anchors { top: parent.top; left: parent.left; margins: 20 }
                text: qsTr("RPM history")
                color: root.primaryText
                font.pixelSize: 18
                font.bold: true
            }
            TelemetryChartItem {
                id: chart
                anchors { top: chartTitle.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; margins: 20; topMargin: 30 }
                metricId: 0
                historyCapacity: 600
                color: root.accent
            }
            Connections { target: root.controller; function onChartSample(metricId, value) { chart.appendSample(metricId, value) } }
        }
    }
}
