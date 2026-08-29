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
    Material.primary: "#37474f"

    component ActionCard: ItemDelegate {
        id: card
        property string title
        property string subtitle
        property string glyph
        Layout.fillWidth: true
        implicitHeight: 76
        background: Rectangle {
            color: card.pressed ? Qt.alpha(Material.accent, 0.12)
                                : Material.theme === Material.Dark ? Qt.lighter(Material.background, 1.35)
                                                                   : "white"
            radius: 14
            border.width: Material.theme === Material.Dark ? 0 : 1
            border.color: Qt.rgba(0.5, 0.5, 0.5, 0.15)
        }
        contentItem: RowLayout {
            spacing: 16
            Rectangle {
                width: 44; height: 44; radius: 12
                color: Qt.alpha(Material.accent, 0.14)
                Label {
                    anchors.centerIn: parent
                    text: card.glyph
                    font.pixelSize: 22
                }
            }
            ColumnLayout {
                spacing: 2
                Layout.fillWidth: true
                Label {
                    text: card.title
                    font.pixelSize: 17
                    font.weight: Font.Medium
                }
                Label {
                    text: card.subtitle
                    font.pixelSize: 13
                    opacity: 0.6
                    Layout.fillWidth: true
                    elide: Text.ElideRight
                }
            }
            Label { text: "›"; font.pixelSize: 24; opacity: 0.35 }
        }
    }

    Flickable {
        anchors.fill: parent
        contentHeight: content.implicitHeight + 40
        clip: true
        boundsBehavior: Flickable.StopAtBounds
        ScrollIndicator.vertical: ScrollIndicator {}

        ColumnLayout {
            id: content
            width: parent.width
            spacing: 0

            // ---- Hero ----
            ColumnLayout {
                Layout.fillWidth: true
                Layout.margins: 24
                Layout.bottomMargin: 8
                spacing: 4
                Label {
                    text: qsTr("Mapper")
                    font.pixelSize: 34
                    font.weight: Font.DemiBold
                }
                Label {
                    text: qsTr("OpenOrienteering field surveying")
                    font.pixelSize: 15
                    opacity: 0.6
                }
            }

            // ---- Primary actions ----
            ColumnLayout {
                Layout.fillWidth: true
                Layout.leftMargin: 16
                Layout.rightMargin: 16
                spacing: 10
                ActionCard {
                    title: qsTr("Map Hub")
                    subtitle: qsTr("Team maps, sync, and shared field work")
                    glyph: "🌐"
                    onClicked: homeBridge.mapHub()
                }
                ActionCard {
                    title: qsTr("Open map")
                    subtitle: qsTr("Browse files on this device")
                    glyph: "📂"
                    onClicked: homeBridge.openMap()
                }
                ActionCard {
                    title: qsTr("New map")
                    subtitle: qsTr("Start from a symbol set")
                    glyph: "✏️"
                    onClicked: homeBridge.newMap()
                }
            }

            // ---- Recent maps ----
            Label {
                visible: recentRepeater.count > 0
                Layout.fillWidth: true
                Layout.topMargin: 26
                Layout.leftMargin: 24
                Layout.rightMargin: 24
                Layout.bottomMargin: 4
                text: qsTr("Recent")
                font.pixelSize: 13
                font.weight: Font.DemiBold
                font.capitalization: Font.AllUppercase
                font.letterSpacing: 1.1
                color: Material.accent
            }
            Repeater {
                id: recentRepeater
                model: homeBridge.recentFiles
                delegate: ItemDelegate {
                    id: recentRow
                    required property var modelData
                    Layout.fillWidth: true
                    implicitHeight: 60
                    onClicked: homeBridge.openFile(recentRow.modelData.path)
                    contentItem: RowLayout {
                        spacing: 14
                        Label { text: "🗺"; font.pixelSize: 20; opacity: 0.8 }
                        ColumnLayout {
                            spacing: 1
                            Layout.fillWidth: true
                            Label {
                                text: recentRow.modelData.name
                                font.pixelSize: 16
                                Layout.fillWidth: true
                                elide: Text.ElideMiddle
                            }
                            Label {
                                text: recentRow.modelData.directory
                                font.pixelSize: 12
                                opacity: 0.5
                                Layout.fillWidth: true
                                elide: Text.ElideLeft
                            }
                        }
                    }
                }
            }

            // ---- Footer ----
            RowLayout {
                Layout.fillWidth: true
                Layout.margins: 16
                Layout.topMargin: 28
                spacing: 4
                Button {
                    flat: true
                    text: qsTr("Settings")
                    onClicked: homeBridge.showSettings()
                }
                Button {
                    flat: true
                    text: qsTr("Help")
                    onClicked: homeBridge.showHelp()
                }
                Item { Layout.fillWidth: true }
                Button {
                    flat: true
                    text: qsTr("About v%1").arg(homeBridge.appVersion)
                    onClicked: homeBridge.showAbout()
                }
            }
        }
    }
}
