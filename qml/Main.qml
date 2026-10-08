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
                TabButton { text: qsTr("Diagnostics") }
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
            objectName: "dashboardWorkspace"
            ColumnLayout {
                anchors.fill: parent; spacing: 16
                Panel {
                    objectName: "telemetryHealthPanel"
                    Layout.fillWidth: true; implicitHeight: 82
                    RowLayout {
                        anchors { fill: parent; margins: 16 }
                        spacing: 28
                        ColumnLayout { spacing: 3; Caption { text: qsTr("Actual poll rate") } ValueText { objectName: "actualPollRateValue"; text: qsTr("%1 Hz").arg(root.controller.actualPollRate.toFixed(1)) } }
                        ColumnLayout { spacing: 3; Caption { text: qsTr("EWMA RTT") } ValueText { text: qsTr("%1 ms").arg(root.controller.ewmaRttMs) } }
                        ColumnLayout { spacing: 3; Caption { text: qsTr("Oldest sample") } ValueText { objectName: "sampleAgeValue"; text: qsTr("%1 ms").arg(root.controller.latestSampleAgeMs) } }
                        ColumnLayout { spacing: 3; Caption { text: qsTr("Source drops") } ValueText { objectName: "sourceDropsValue"; text: root.controller.sourceQueueDrops; color: root.controller.sourceQueueDrops > 0 ? "#ffb454" : root.primaryText } }
                        ColumnLayout { spacing: 3; Caption { text: qsTr("Recorder drops") } ValueText { text: root.controller.recorderQueueDrops; color: root.controller.recorderQueueDrops > 0 ? "#ffb454" : root.primaryText } }
                        Item { Layout.fillWidth: true }
                    }
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 16
                    GridView {
                        id: metrics; objectName: "primaryTelemetryGrid"
                        Layout.fillWidth: true; Layout.fillHeight: true; Layout.preferredWidth: 650
                        cellWidth: Math.max(175, width / Math.max(1, Math.floor(width / 205))); cellHeight: 120
                        clip: true; model: root.controller.telemetryModel
                        delegate: Item {
                            id: metricDelegate
                            required property int metricId
                            required property string name
                            required property real value
                            required property string unit
                            required property string quality
                            required property bool valid
                            required property int sampleAgeMs
                            required property string stateLabel
                            width: metrics.cellWidth; height: metrics.cellHeight
                            Panel {
                                anchors { fill: parent; margins: 5 }
                                Caption { anchors { left: parent.left; leftMargin: 14; top: parent.top; topMargin: 12 } text: metricDelegate.name }
                                Label {
                                    anchors { left: parent.left; leftMargin: 14; bottom: parent.bottom; bottomMargin: 27 }
                                    text: metricDelegate.valid ? Number(metricDelegate.value).toLocaleString(Qt.locale(), 'f', 1) : "--"
                                    color: metricDelegate.valid ? root.primaryText : root.secondaryText; font.pixelSize: 26; font.bold: true
                                }
                                Caption { anchors { right: parent.right; rightMargin: 14; bottom: parent.bottom; bottomMargin: 31 } text: metricDelegate.unit }
                                Caption {
                                    objectName: "metricStateLabel"
                                    anchors { left: parent.left; leftMargin: 14; bottom: parent.bottom; bottomMargin: 9 }
                                    text: metricDelegate.valid ? qsTr("Live / %1 ms").arg(metricDelegate.sampleAgeMs) : metricDelegate.stateLabel
                                    color: metricDelegate.valid ? root.accent : (metricDelegate.quality === "Stale" ? "#ffb454" : root.secondaryText)
                                }
                            }
                        }
                    }
                    Panel {
                        id: chartPanel; objectName: "rollingChartPanel"
                        Layout.fillHeight: true; Layout.preferredWidth: root.compact ? 280 : 410
                        ColumnLayout {
                            anchors { fill: parent; margins: 18 }
                            spacing: 12
                            Label { text: qsTr("Rolling telemetry"); color: root.primaryText; font.pixelSize: 18; font.bold: true }
                            RowLayout {
                                Layout.fillWidth: true
                                ComboBox {
                                    id: chartMetric; objectName: "chartMetricSelector"; Layout.fillWidth: true
                                    textRole: "text"; valueRole: "metricId"
                                    model: [
                                        { "text": qsTr("RPM"), "metricId": 0 },
                                        { "text": qsTr("Speed"), "metricId": 1 },
                                        { "text": qsTr("Throttle"), "metricId": 2 },
                                        { "text": qsTr("Coolant"), "metricId": 7 },
                                        { "text": qsTr("Engine load"), "metricId": 5 },
                                        { "text": qsTr("MAP"), "metricId": 3 },
                                        { "text": qsTr("MAF"), "metricId": 4 },
                                        { "text": qsTr("Module voltage"), "metricId": 14 }
                                    ]
                                    onCurrentValueChanged: { chart.metricId = Number(currentValue); chart.clear() }
                                }
                                ComboBox {
                                    id: chartRange; objectName: "chartRangeSelector"; Layout.preferredWidth: 105
                                    model: [qsTr("10 s"), qsTr("30 s"), qsTr("120 s")]
                                    currentIndex: 1
                                    onCurrentIndexChanged: chart.historySeconds = currentIndex === 0 ? 10 : (currentIndex === 1 ? 30 : 120)
                                }
                            }
                            TelemetryChartItem {
                                id: chart; objectName: "telemetryChart"
                                Layout.fillWidth: true; Layout.fillHeight: true
                                metricId: 0; historySeconds: 30; color: root.accent
                            }
                            Caption { text: qsTr("History is sampled at 10 Hz and bounded to the selected window."); wrapMode: Text.WordWrap; Layout.fillWidth: true }
                        }
                        Connections {
                            target: root.controller
                            function onChartSample(metricId, value) { chart.appendSample(metricId, value) }
                            function onPresentationUnitsChanged() { chart.clear() }
                        }
                    }
                }
            }
        }

        Item {
            objectName: "diagnosticsWorkspace"
            ColumnLayout {
                anchors.fill: parent; spacing: 14
                RowLayout {
                    Layout.fillWidth: true
                    Label { text: qsTr("Diagnostics"); color: root.primaryText; font.pixelSize: 24; font.bold: true }
                    Item { Layout.fillWidth: true }
                    Button {
                        objectName: "scanDiagnosticsButton"; text: root.controller.diagnosticBusy ? qsTr("Scanning…") : qsTr("Scan stored + pending")
                        enabled: root.controller.connectionState === "Ready" && !root.controller.diagnosticBusy && !root.controller.clearConfirmationPending
                        onClicked: root.controller.scanDiagnostics()
                    }
                    Button {
                        objectName: "clearDtcButton"; text: qsTr("Clear diagnostic information"); highlighted: true
                        enabled: root.controller.clearDtcEnabled
                        onClicked: root.controller.prepareClearDiagnostics()
                    }
                }
                Caption { objectName: "clearPreconditionStatus"; text: root.controller.clearPreconditionStatus; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                Label {
                    objectName: "clearResultLabel"; visible: root.controller.clearResult.length > 0
                    text: root.controller.clearResult; color: root.controller.clearResult.indexOf("completed") >= 0 ? root.accent : "#ffb454"
                    Layout.fillWidth: true; wrapMode: Text.WordWrap
                }
                RowLayout {
                    Layout.fillWidth: true; Layout.fillHeight: true; spacing: 14
                    Panel {
                        Layout.fillHeight: true; Layout.fillWidth: true; Layout.preferredWidth: 580
                        ColumnLayout {
                            anchors { fill: parent; margins: 16 }
                            spacing: 10
                            Label { text: qsTr("Trouble codes"); color: root.primaryText; font.pixelSize: 18; font.bold: true }
                            ListView {
                                id: dtcList; objectName: "dtcList"; Layout.fillWidth: true; Layout.fillHeight: true
                                model: root.controller.dtcModel; clip: true; spacing: 8
                                section.property: "group"; section.criteria: ViewSection.FullString
                                section.delegate: Rectangle {
                                    required property string section
                                    width: dtcList.width; height: 32; color: "transparent"
                                    Label { anchors.verticalCenter: parent.verticalCenter; text: parent.section; color: root.accent; font.bold: true }
                                }
                                delegate: Rectangle {
                                    id: dtcRow
                                    required property string code; required property string status; required property string severity
                                    required property string description; required property string ecu; required property string advisory
                                    required property var failurePoints; required property bool hasFreezeFrame
                                    required property string freezeFrameTitle; required property var freezeFrameSamples
                                    width: dtcList.width; height: dtcContent.implicitHeight + 20; radius: 7
                                    color: root.controller.darkTheme ? "#222c33" : "#f3f6f8"
                                    ColumnLayout {
                                        id: dtcContent
                                        anchors { left: parent.left; right: parent.right; top: parent.top; margins: 10 }
                                        spacing: 3
                                        RowLayout { Layout.fillWidth: true
                                            ValueText { text: dtcRow.code }
                                            Caption { text: dtcRow.status + " · " + (dtcRow.ecu || qsTr("Unknown ECU")) }
                                            Item { Layout.fillWidth: true }
                                            Label { text: dtcRow.severity; color: dtcRow.severity === "Critical" ? "#ff667a" : (dtcRow.severity === "Warning" ? "#ffb454" : root.accent); font.bold: true }
                                        }
                                        Label { text: dtcRow.description || qsTr("No catalog description available"); color: root.primaryText; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                        Caption { text: dtcRow.advisory; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                        Caption { visible: dtcRow.failurePoints.length > 0; text: qsTr("Likely checks: %1").arg(dtcRow.failurePoints.join(", ")); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                        Button {
                                            objectName: "freezeFrameButton"; visible: dtcRow.hasFreezeFrame; text: qsTr("Inspect freeze frame")
                                            onClicked: { freezeFrameDialog.titleText = dtcRow.freezeFrameTitle; freezeFrameDialog.samples = dtcRow.freezeFrameSamples; freezeFrameDialog.open() }
                                        }
                                    }
                                }
                                Label { anchors.centerIn: parent; visible: dtcList.count === 0; text: qsTr("No diagnostic scan results yet."); color: root.secondaryText }
                            }
                        }
                    }
                    ColumnLayout {
                        Layout.fillHeight: true; Layout.preferredWidth: 430; spacing: 14
                        Panel {
                            Layout.fillWidth: true; Layout.fillHeight: true
                            ColumnLayout {
                                anchors { fill: parent; margins: 14 }
                                spacing: 8
                                Label { text: qsTr("Heuristic findings"); color: root.primaryText; font.pixelSize: 18; font.bold: true }
                                ListView {
                                    id: findingList; objectName: "findingList"; Layout.fillWidth: true; Layout.fillHeight: true; model: root.controller.findingModel; clip: true; spacing: 8
                                    delegate: Rectangle {
                                        id: findingRow
                                        required property string ruleId; required property string title; required property string description
                                        required property string severity; required property string status; required property var evidence; required property string limitations
                                        width: findingList.width; height: findingContent.implicitHeight + 18; radius: 7; color: root.controller.darkTheme ? "#222c33" : "#f3f6f8"
                                        ColumnLayout { id: findingContent
                                            anchors { left: parent.left; right: parent.right; top: parent.top; margins: 9 }
                                            spacing: 3
                                            RowLayout { Layout.fillWidth: true
                                                ValueText { text: findingRow.title }
                                                Item { Layout.fillWidth: true }
                                                Caption { text: findingRow.status + " · " + findingRow.severity }
                                            }
                                            Caption { text: qsTr("Rule: %1").arg(findingRow.ruleId); Layout.fillWidth: true }
                                            Label { text: findingRow.description; color: root.primaryText; Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                            Caption { text: qsTr("Evidence: %1").arg(findingRow.evidence.length ? findingRow.evidence.join("; ") : qsTr("insufficient evidence")); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                            Caption { text: qsTr("Limitations: %1").arg(findingRow.limitations); Layout.fillWidth: true; wrapMode: Text.WordWrap }
                                        }
                                    }
                                }
                            }
                        }
                        Panel {
                            objectName: "rawDiagnosticTerminal"; Layout.fillWidth: true; Layout.preferredHeight: 210
                            ColumnLayout {
                                anchors { fill: parent; margins: 12 }
                                spacing: 6
                                RowLayout { Layout.fillWidth: true
                                    Label { text: qsTr("Raw diagnostic terminal"); color: root.primaryText; font.bold: true }
                                    Item { Layout.fillWidth: true }
                                    TextField { objectName: "rawHexFilter"; Layout.preferredWidth: 115; placeholderText: qsTr("Hex filter"); onTextChanged: root.controller.setRawTerminalHexFilter(text) }
                                    ToolButton { objectName: "pauseRawTerminalButton"; checkable: true; text: checked ? qsTr("Resume") : qsTr("Pause"); onToggled: root.controller.setRawTerminalPaused(checked) }
                                    ToolButton { objectName: "copyRawTerminalButton"; text: qsTr("Copy"); onClicked: root.controller.copyRawTerminal() }
                                }
                                ListView {
                                    objectName: "rawTerminalList"; Layout.fillWidth: true; Layout.fillHeight: true; clip: true
                                    model: root.controller.rawDiagnosticModel
                                    delegate: Label { required property string line; width: ListView.view.width; text: line; color: root.secondaryText; font.family: "monospace"; font.pixelSize: 12 }
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    Dialog {
        id: freezeFrameDialog; objectName: "freezeFrameDialog"; modal: true; anchors.centerIn: parent
        width: Math.min(520, root.width - 60); height: Math.min(500, root.height - 60)
        property string titleText: ""; property var samples: []
        title: qsTr("Freeze frame — %1").arg(titleText); standardButtons: Dialog.Close
        ListView {
            anchors.fill: parent; model: freezeFrameDialog.samples; clip: true
            delegate: RowLayout { width: ListView.view.width
                required property var modelData
                Label { text: modelData.name; color: root.primaryText; Layout.fillWidth: true }
                Label { text: Number(modelData.value).toLocaleString(Qt.locale(), 'f', 2) + " " + modelData.unit; color: root.primaryText }
                Caption { text: modelData.quality }
            }
        }
    }

    Dialog {
        id: clearDialog; objectName: "clearDtcDialog"; modal: true; anchors.centerIn: parent
        width: Math.min(560, root.width - 60); visible: root.controller.clearConfirmationPending
        title: qsTr("Confirm Mode 04 clear")
        onClosed: root.controller.cancelClearDiagnostics()
        ColumnLayout {
            width: parent.width; spacing: 10
            Label { text: root.controller.clearWarning; color: "#ffb454"; Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Caption { text: qsTr("The engine revalidates source, vehicle identity, fresh speed, and stationary state immediately before transmission."); Layout.fillWidth: true; wrapMode: Text.WordWrap }
            Label { text: qsTr("Confirmation expires in %1 s").arg(root.controller.clearCountdownSeconds); color: root.controller.clearCountdownSeconds > 0 ? root.primaryText : "#ff667a"; font.bold: true }
            Caption { text: qsTr("Type the single-use token exactly:") }
            ValueText { objectName: "clearConfirmationToken"; text: root.controller.clearConfirmationToken; font.family: "monospace" }
            TextField { id: clearTokenInput; objectName: "clearTokenInput"; Layout.fillWidth: true; placeholderText: qsTr("Confirmation token") }
            RowLayout { Layout.alignment: Qt.AlignRight
                Button { text: qsTr("Cancel"); enabled: !root.controller.diagnosticBusy; onClicked: clearDialog.close() }
                Button {
                    objectName: "confirmClearButton"; text: root.controller.diagnosticBusy ? qsTr("Clearing and rescanning…") : qsTr("Clear and rescan"); highlighted: true
                    enabled: !root.controller.diagnosticBusy && root.controller.clearCountdownSeconds > 0 && clearTokenInput.text === root.controller.clearConfirmationToken
                    onClicked: root.controller.confirmClearDiagnostics(clearTokenInput.text)
                }
            }
        }
    }
}
