// Copyright 2026 Ethan O'Connor
// This file is part of OpenOrienteering (GPLv3+).

import QtQuick
import QtQuick.Controls
import QtQuick.Controls.Material
import QtQuick.Layouts

Rectangle {
    id: root
    color: Material.background

    Material.theme: Material.System
    Material.accent: "#e65100"   // OpenOrienteering orange
    Material.primary: "#37474f"  // blue-gray toolbar

    Component.onCompleted: {
        // Temporary UI-iteration hook.
        if (settingsBridge.debugPage === "gnss") stack.push(gnssPage)
        else if (settingsBridge.debugPage === "sketch") stack.push(sketchPage)
        else if (settingsBridge.debugPage === "general") stack.push(generalPage)
        else if (settingsBridge.debugPage === "ntrip") stack.push(ntripEditPage, { originalName: "" })
    }

    // ---------- Reusable building blocks ----------

    component SectionLabel: Label {
        Layout.fillWidth: true
        Layout.topMargin: 20
        Layout.leftMargin: 20
        Layout.rightMargin: 20
        font.pixelSize: 13
        font.weight: Font.DemiBold
        font.capitalization: Font.AllUppercase
        font.letterSpacing: 1.1
        color: Material.accent
    }

    component SettingSwitch: ItemDelegate {
        id: switchRow
        property alias value: control.checked
        property string title
        property string subtitle
        signal edited(bool value)
        Layout.fillWidth: true
        implicitHeight: Math.max(60, column.implicitHeight + 20)
        onClicked: control.toggle()
        contentItem: RowLayout {
            spacing: 12
            ColumnLayout {
                id: column
                Layout.fillWidth: true
                spacing: 2
                Label {
                    text: switchRow.title
                    font.pixelSize: 16
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
                Label {
                    text: switchRow.subtitle
                    visible: text.length > 0
                    font.pixelSize: 13
                    opacity: 0.6
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                }
            }
            Switch {
                id: control
                onToggled: switchRow.edited(checked)
            }
        }
    }

    component NavRow: ItemDelegate {
        id: navRow
        property string title
        property string value
        Layout.fillWidth: true
        implicitHeight: 60
        contentItem: RowLayout {
            spacing: 12
            Label {
                text: navRow.title
                font.pixelSize: 16
                Layout.fillWidth: true
            }
            Label {
                text: navRow.value
                font.pixelSize: 15
                opacity: 0.6
                elide: Text.ElideRight
                Layout.maximumWidth: navRow.width * 0.45
            }
            Label {
                text: "›"   // ›
                font.pixelSize: 22
                opacity: 0.4
            }
        }
    }

    component PageScaffold: Page {
        id: scaffold
        default property alias pageContent: contentColumn.data
        header: ToolBar {
            Material.elevation: 1
            RowLayout {
                anchors.fill: parent
                spacing: 0
                ToolButton {
                    text: stack.depth > 1 ? "‹" : "×"   // back / close
                    font.pixelSize: 20
                    onClicked: stack.depth > 1 ? stack.pop()
                                               : settingsBridge.requestClose()
                }
                Label {
                    text: scaffold.title
                    font.pixelSize: 19
                    font.weight: Font.Medium
                    elide: Text.ElideRight
                    Layout.fillWidth: true
                }
            }
        }
        Flickable {
            anchors.fill: parent
            contentHeight: contentColumn.implicitHeight + 24
            clip: true
            boundsBehavior: Flickable.StopAtBounds
            ScrollIndicator.vertical: ScrollIndicator {}
            ColumnLayout {
                id: contentColumn
                width: parent.width
                spacing: 0
            }
        }
    }

    // ---------- Navigation ----------

    StackView {
        id: stack
        anchors.fill: parent
        initialItem: categoriesPage
    }

    // ---------- Categories ----------

    Component {
        id: categoriesPage
        PageScaffold {
            title: qsTr("Settings")
            NavRow {
                title: qsTr("GNSS & corrections")
                value: settingsBridge.receiverSummary
                onClicked: stack.push(gnssPage)
            }
            NavRow {
                title: qsTr("Sketch palette")
                onClicked: stack.push(sketchPage)
            }
            NavRow {
                title: qsTr("General")
                onClicked: stack.push(generalPage)
            }
        }
    }

    // ---------- GNSS ----------

    Component {
        id: gnssPage
        PageScaffold {
            title: qsTr("GNSS & corrections")

            SectionLabel { text: qsTr("Receiver") }
            NavRow {
                title: qsTr("Receiver")
                value: settingsBridge.receiverSummary
                onClicked: settingsBridge.chooseReceiver()
            }
            ItemDelegate {
                Layout.fillWidth: true
                visible: settingsBridge.hasSavedReceiver
                implicitHeight: 52
                contentItem: Label {
                    text: qsTr("Forget receiver, use phone location")
                    font.pixelSize: 15
                    color: Material.accent
                    verticalAlignment: Text.AlignVCenter
                }
                onClicked: settingsBridge.useSystemLocation()
            }
            SettingSwitch {
                title: qsTr("Connect automatically")
                subtitle: qsTr("Connect to the saved receiver when live GNSS starts")
                value: settingsBridge.gnssAutoConnect
                onEdited: (value) => settingsBridge.gnssAutoConnect = value
            }
            SettingSwitch {
                title: qsTr("Record raw log")
                subtitle: qsTr("Keep NMEA and correction data for later analysis")
                value: settingsBridge.rawLogging
                onEdited: (value) => settingsBridge.rawLogging = value
            }

            SectionLabel { text: qsTr("NTRIP corrections") }
            SettingSwitch {
                title: qsTr("Start corrections automatically")
                subtitle: qsTr("Connect to the active caster profile with the receiver")
                value: settingsBridge.ntripAutoStart
                onEdited: (value) => settingsBridge.ntripAutoStart = value
            }
            Repeater {
                model: settingsBridge.ntripProfiles
                delegate: ItemDelegate {
                    id: profileRow
                    required property string modelData
                    Layout.fillWidth: true
                    implicitHeight: 56
                    onClicked: stack.push(ntripEditPage, { originalName: profileRow.modelData })
                    contentItem: RowLayout {
                        spacing: 12
                        RadioButton {
                            checked: settingsBridge.activeNtripProfile === profileRow.modelData
                            onClicked: settingsBridge.activeNtripProfile = profileRow.modelData
                        }
                        Label {
                            text: profileRow.modelData
                            font.pixelSize: 16
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Label { text: "›"; font.pixelSize: 22; opacity: 0.4 }
                    }
                }
            }
            Button {
                Layout.leftMargin: 20
                Layout.topMargin: 8
                flat: true
                text: qsTr("Add caster profile")
                onClicked: stack.push(ntripEditPage, { originalName: "" })
            }
        }
    }

    // ---------- NTRIP profile editor ----------

    Component {
        id: ntripEditPage
        PageScaffold {
            id: editPage
            property string originalName: ""
            property var profile: originalName.length > 0
                                  ? settingsBridge.loadNtripProfile(originalName) : ({})
            title: originalName.length > 0 ? originalName : qsTr("New caster profile")

            component FieldRow: ColumnLayout {
                property alias label: fieldLabel.text
                property alias text: field.text
                property alias placeholder: field.placeholderText
                property alias echoMode: field.echoMode
                property alias inputMethodHints: field.inputMethodHints
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                Layout.topMargin: 10
                spacing: 0
                Label { id: fieldLabel; font.pixelSize: 13; opacity: 0.6 }
                TextField { id: field; Layout.fillWidth: true; font.pixelSize: 16 }
            }

            FieldRow { id: nameField; label: qsTr("Profile name"); text: editPage.profile.name ?? "" }
            FieldRow {
                id: hostField; label: qsTr("Caster host")
                text: editPage.profile.host ?? ""
                placeholder: "caster.example.org"
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            }
            FieldRow {
                id: portField; label: qsTr("Port")
                text: (editPage.profile.port ?? 2101).toString()
                inputMethodHints: Qt.ImhDigitsOnly
            }
            FieldRow {
                id: mountField; label: qsTr("Mountpoint")
                text: editPage.profile.mountpoint ?? ""
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            }
            FieldRow {
                id: userField; label: qsTr("Username")
                text: editPage.profile.username ?? ""
                inputMethodHints: Qt.ImhNoPredictiveText | Qt.ImhNoAutoUppercase
            }
            FieldRow {
                id: passwordField
                label: (editPage.profile.hasPassword ?? false)
                       ? qsTr("Password (leave empty to keep saved)") : qsTr("Password")
                echoMode: TextInput.Password
            }
            SettingSwitch {
                id: tlsSwitch
                Layout.topMargin: 8
                title: qsTr("Use TLS")
                value: editPage.profile.useTls ?? false
            }
            SettingSwitch {
                id: ggaSwitch
                title: qsTr("Send position to caster")
                subtitle: qsTr("Required by VRS network services")
                value: editPage.profile.sendGga ?? true
            }
            Label {
                id: errorLabel
                visible: text.length > 0
                color: Material.color(Material.Red)
                font.pixelSize: 14
                Layout.fillWidth: true
                Layout.leftMargin: 20
                Layout.rightMargin: 20
                wrapMode: Text.WordWrap
            }
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 20
                spacing: 12
                Button {
                    visible: editPage.originalName.length > 0
                    flat: true
                    Material.foreground: Material.color(Material.Red)
                    text: qsTr("Delete")
                    onClicked: {
                        settingsBridge.removeNtripProfile(editPage.originalName)
                        stack.pop()
                    }
                }
                Item { Layout.fillWidth: true }
                Button {
                    highlighted: true
                    text: qsTr("Save")
                    onClicked: {
                        const error = settingsBridge.saveNtripProfile(editPage.originalName, {
                            name: nameField.text,
                            host: hostField.text,
                            port: parseInt(portField.text) || 2101,
                            mountpoint: mountField.text,
                            username: userField.text,
                            password: passwordField.text,
                            useTls: tlsSwitch.value,
                            sendGga: ggaSwitch.value
                        })
                        if (error.length > 0)
                            errorLabel.text = error
                        else
                            stack.pop()
                    }
                }
            }
        }
    }

    // ---------- Sketch palette ----------

    Component {
        id: sketchPage
        PageScaffold {
            title: qsTr("Sketch palette")

            SectionLabel { text: qsTr("Colors") }
            Flow {
                Layout.fillWidth: true
                Layout.margins: 20
                spacing: 12
                Repeater {
                    model: settingsBridge.sketchColors
                    delegate: Rectangle {
                        required property string modelData
                        width: 44; height: 44; radius: 22
                        color: modelData
                        border.width: 1
                        border.color: Qt.rgba(0.5, 0.5, 0.5, 0.4)
                    }
                }
            }
            SectionLabel { text: qsTr("Presets") }
            Repeater {
                model: settingsBridge.sketchPresets()
                delegate: ItemDelegate {
                    id: presetRow
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 56
                    onClicked: settingsBridge.sketchColors = presetRow.modelData.colors
                    contentItem: RowLayout {
                        spacing: 12
                        Label {
                            text: presetRow.modelData.name
                            font.pixelSize: 16
                            Layout.preferredWidth: 130
                        }
                        Row {
                            spacing: 4
                            Repeater {
                                model: presetRow.modelData.colors
                                delegate: Rectangle {
                                    required property string modelData
                                    width: 20; height: 20; radius: 10
                                    color: modelData
                                }
                            }
                        }
                        Item { Layout.fillWidth: true }
                    }
                }
            }
        }
    }

    // ---------- General ----------

    Component {
        id: generalPage
        PageScaffold {
            title: qsTr("General")

            SectionLabel { text: qsTr("Startup") }
            SettingSwitch {
                title: qsTr("Open last map on start")
                value: settingsBridge.openMruFile
                onEdited: (value) => settingsBridge.openMruFile = value
            }

            SectionLabel { text: qsTr("Language") }
            Repeater {
                model: settingsBridge.languages
                delegate: ItemDelegate {
                    id: languageRow
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 52
                    onClicked: settingsBridge.languageCode = languageRow.modelData.code
                    contentItem: RowLayout {
                        spacing: 12
                        RadioButton {
                            checked: settingsBridge.languageCode === languageRow.modelData.code
                            onClicked: settingsBridge.languageCode = languageRow.modelData.code
                        }
                        Label {
                            text: languageRow.modelData.name
                            font.pixelSize: 16
                            Layout.fillWidth: true
                        }
                    }
                }
            }
            Label {
                Layout.fillWidth: true
                Layout.margins: 20
                text: qsTr("Language changes take effect after restarting Mapper.")
                font.pixelSize: 13
                opacity: 0.6
                wrapMode: Text.WordWrap
            }
        }
    }
}
