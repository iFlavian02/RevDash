pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import RevDash 1.0

ApplicationWindow {
    id: root
    required property var controller
    width: 1180; height: 720; minimumWidth: 760; minimumHeight: 520
    visible: true; title: qsTr("RevDash")
    color: root.controller.darkTheme ? "#101418" : "#eef2f5"
    readonly property color panelColor: root.controller.darkTheme ? "#192127" : "#ffffff"
    readonly property color primaryText: root.controller.darkTheme ? "#f4f7f8" : "#172027"
    readonly property color secondaryText: root.controller.darkTheme ? "#9aa8b1" : "#586872"
    readonly property color accent: "#39d98a"
    property bool compact: width < 900

    component Panel: Rectangle { radius: 10; color: root.panelColor }
    component Caption: Label { color: root.secondaryText; font.pixelSize: 12 }
    component ValueText: Label { color: root.primaryText; font.pixelSize: 15; font.bold: true }

    header: Rectangle {
        height: 72; color: root.panelColor
        RowLayout {
            anchors { fill: parent; leftMargin: 28; rightMargin: 24 }
            spacing: 20
            Label { text: qsTr("REVDASH"); color: root.primaryText; font.pixelSize: 22; font.bold: true; font.letterSpacing: 2 }
            TabBar {
                id: workspaceTabs; objectName: "workspaceTabs"; Layout.fillWidth: true
                background: Item {}
                TabButton { text: qsTr("Connect") }
                TabButton { text: qsTr("Live Dashboard") }
            }
            Label { text: root.controller.connectionState; color: root.controller.connectionState === "Ready" ? root.accent : root.secondaryText }
            Switch { text: qsTr("Imperial"); onToggled: root.controller.setImperial(checked) }
            Button { text: root.controller.darkTheme ? qsTr("Light") : qsTr("Dark"); onClicked: root.controller.setDarkTheme(!root.controller.darkTheme) }
        }
    }

    StackLayout {
        anchors { fill: parent; margins: 24 }
        currentIndex: workspaceTabs.currentIndex

        Item {
            objectName: "connectWorkspace"
            RowLayout {
                anchors.fill: parent; spacing: 20
                Panel {
                    Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 650
                    ColumnLayout {
                        anchors { fill: parent; margins: 24 }
                        spacing: 14
                        Label { text: qsTr("Connect a data source"); color: root.primaryText; font.pixelSize: 24; font.bold: true }
                        Caption { text: qsTr("Use an ELM327 over USB or Bluetooth Classic, or run a deterministic simulation."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        TabBar {
                            id: sourceTabs; objectName: "sourceTabs"; Layout.fillWidth: true
                            enabled: root.controller.sourceOperationsEnabled
                            TabButton { text: qsTr("ELM327") }
                            TabButton { text: qsTr("Synthetic") }
                        }
                        StackLayout {
                            Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: sourceTabs.currentIndex
                            ColumnLayout {
                                spacing: 12
                                Caption { text: qsTr("COM port") }
                                RowLayout {
                                    Layout.fillWidth: true
                                    ComboBox {
                                        id: portBox; objectName: "portBox"; Layout.fillWidth: true
                                        model: root.controller.serialPortModel; textRole: "portName"; valueRole: "portName"
                                        enabled: root.controller.sourceOperationsEnabled && count > 0
                                        delegate: ItemDelegate {
                                            required property string portName
                                            required property string friendlyName
                                            required property string transport
                                            width: portBox.width
                                            text: qsTr("%1 — %2 (%3)").arg(portName).arg(friendlyName).arg(transport)
                                        }
                                        displayText: count > 0 ? currentText : qsTr("No serial adapters found")
                                    }
                                    Button { objectName: "refreshPortsButton"; text: qsTr("Refresh"); enabled: root.controller.sourceOperationsEnabled; onClicked: root.controller.refreshSerialPorts() }
                                }
                                Caption { text: qsTr("Baud rate") }
                                ComboBox { id: baudBox; objectName: "baudBox"; model: [9600, 38400, 115200]; currentIndex: 1; enabled: root.controller.sourceOperationsEnabled }
                                Item { Layout.fillHeight: true }
                                Button {
                                    objectName: "connectSerialButton"; text: qsTr("Connect ELM327"); highlighted: true
                                    enabled: root.controller.sourceOperationsEnabled && portBox.count > 0
                                    onClicked: root.controller.connectSerial(portBox.currentValue, Number(baudBox.currentText))
                                }
                            }
                            ColumnLayout {
                                spacing: 12
                                Caption { text: qsTr("Simulation preset") }
                                ComboBox { id: presetBox; objectName: "presetBox"; Layout.fillWidth: true; model: root.controller.simulationPresets; enabled: root.controller.sourceOperationsEnabled }
                                Caption { text: qsTr("Deterministic seed") }
                                SpinBox { id: seedBox; objectName: "seedBox"; from: 0; to: 999999999; value: 12345; editable: true; enabled: root.controller.sourceOperationsEnabled }
                                Caption { text: qsTr("The preset and seed are applied before the source connects, making runs reproducible."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                                Item { Layout.fillHeight: true }
                                Button {
                                    objectName: "connectSyntheticButton"; text: qsTr("Start simulation"); highlighted: true
                                    enabled: root.controller.sourceOperationsEnabled
                                    onClicked: root.controller.connectSynthetic(presetBox.currentText, seedBox.value)
                                }
                            }
                        }
                    }
                }
                ColumnLayout {
                    Layout.fillHeight: true; Layout.preferredWidth: root.compact ? 290 : 390; spacing: 16
                    Panel {
                        Layout.fillWidth: true; implicitHeight: statusColumn.implicitHeight + 48
                        ColumnLayout {
                            id: statusColumn
                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 24 }
                            spacing: 10
                            Label { text: qsTr("Connection status"); color: root.primaryText; font.pixelSize: 18; font.bold: true }
                            Caption { text: qsTr("State") }
                            ValueText { objectName: "connectionStateValue"; text: root.controller.connectionState; color: root.controller.connectionState === "Ready" ? root.accent : root.primaryText }
                            Caption { text: qsTr("Adapter") }
                            ValueText { text: root.controller.adapterIdentity || qsTr("—"); wrapMode: Text.Wrap; Layout.fillWidth: true }
                            Caption { text: qsTr("Protocol") }
                            ValueText { text: root.controller.protocolIdentity || qsTr("—"); wrapMode: Text.Wrap; Layout.fillWidth: true }
                            GridLayout {
                                columns: 2; columnSpacing: 28; rowSpacing: 4
                                Caption { text: qsTr("Last RTT") } Caption { text: qsTr("EWMA RTT") }
                                ValueText { text: qsTr("%1 ms").arg(root.controller.lastRttMs) } ValueText { text: qsTr("%1 ms").arg(root.controller.ewmaRttMs) }
                                Caption { text: qsTr("Retries") } Caption { text: qsTr("Errors") }
                                ValueText { text: root.controller.retryCount } ValueText { text: root.controller.errorCount }
                            }
                            Button { objectName: "disconnectButton"; text: qsTr("Disconnect"); enabled: root.controller.sourceOperationsEnabled && root.controller.connectionState !== "Disconnected"; onClicked: root.controller.disconnectSource() }
                        }
                    }
                    Rectangle {
                        visible: root.controller.clearConfirmationPending; Layout.fillWidth: true
                        implicitHeight: guardText.implicitHeight + 28; radius: 8; color: "#3b2d12"; border.color: "#d99b39"
                        Label { id: guardText; anchors { fill: parent; margins: 14 } text: qsTr("Source changes are locked while a clear-DTC confirmation is pending."); color: "#ffd58a"; wrapMode: Text.WordWrap }
                    }
                    Rectangle {
                        visible: root.controller.lastError.length > 0; Layout.fillWidth: true
                        implicitHeight: errorText.implicitHeight + 28; radius: 8; color: "#3d1d22"; border.color: "#e05b6f"
                        Label { id: errorText; objectName: "connectionError"; anchors { fill: parent; margins: 14 } text: root.controller.lastError; color: "#ffb7c1"; wrapMode: Text.WordWrap }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }

        Item {
            GridView {
                id: metrics
                anchors { top: parent.top; left: parent.left; right: root.compact ? parent.right : chartPanel.left; bottom: parent.bottom; rightMargin: root.compact ? 0 : 24 }
                cellWidth: Math.max(190, width / Math.max(1, Math.floor(width / 220))); cellHeight: 110
                clip: true; model: root.controller.telemetryModel
                delegate: Item {
                    id: metricDelegate
                    required property string name; required property real value; required property string unit; required property bool valid
                    width: metrics.cellWidth; height: metrics.cellHeight
                    Panel {
                        anchors { fill: parent; margins: 6 }
                        Caption { anchors { left: parent.left; leftMargin: 16; top: parent.top; topMargin: 14 } text: metricDelegate.name }
                        Label { anchors { left: parent.left; leftMargin: 16; bottom: parent.bottom; bottomMargin: 15 } text: metricDelegate.valid ? Number(metricDelegate.value).toLocaleString(Qt.locale(), 'f', 1) : "--"; color: metricDelegate.valid ? root.primaryText : root.secondaryText; font.pixelSize: 28; font.bold: true }
                        Caption { anchors { right: parent.right; rightMargin: 16; bottom: parent.bottom; bottomMargin: 20 } text: metricDelegate.unit }
                    }
                }
            }
            Panel {
                id: chartPanel; visible: !root.compact; width: Math.min(420, parent.width * 0.38)
                anchors { top: parent.top; right: parent.right; bottom: parent.bottom }
                Label { id: chartTitle; anchors { top: parent.top; left: parent.left; margins: 20 } text: qsTr("RPM history"); color: root.primaryText; font.pixelSize: 18; font.bold: true }
                TelemetryChartItem { id: chart; anchors { top: chartTitle.bottom; left: parent.left; right: parent.right; bottom: parent.bottom; margins: 20; topMargin: 30 } metricId: 0; historyCapacity: 600; color: root.accent }
                Connections { target: root.controller; function onChartSample(metricId, value) { chart.appendSample(metricId, value) } }
            }
        }
    }
}
