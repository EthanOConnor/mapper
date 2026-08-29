/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifndef OPENORIENTEERING_MOBILE_SETTINGS_DIALOG_H
#define OPENORIENTEERING_MOBILE_SETTINGS_DIALOG_H

#include <QDialog>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class QQuickWidget;

namespace OpenOrienteering {

/**
 * The data behind the QML mobile settings screens.
 *
 * Mobile settings apply immediately: every setter writes straight to
 * Settings. The exposed surface is deliberately curated for field use;
 * everything else keeps its default and stays desktop-only.
 */
class MobileSettingsBridge : public QObject
{
	Q_OBJECT
	Q_PROPERTY(QString receiverSummary READ receiverSummary NOTIFY receiverChanged)
	Q_PROPERTY(bool hasSavedReceiver READ hasSavedReceiver NOTIFY receiverChanged)
	Q_PROPERTY(bool gnssAutoConnect READ gnssAutoConnect WRITE setGnssAutoConnect NOTIFY optionsChanged)
	Q_PROPERTY(bool ntripAutoStart READ ntripAutoStart WRITE setNtripAutoStart NOTIFY optionsChanged)
	Q_PROPERTY(bool rawLogging READ rawLogging WRITE setRawLogging NOTIFY optionsChanged)
	Q_PROPERTY(QStringList ntripProfiles READ ntripProfiles NOTIFY profilesChanged)
	Q_PROPERTY(QString activeNtripProfile READ activeNtripProfile WRITE setActiveNtripProfile NOTIFY profilesChanged)
	Q_PROPERTY(bool openMruFile READ openMruFile WRITE setOpenMruFile NOTIFY optionsChanged)
	Q_PROPERTY(QVariantList languages READ languages CONSTANT)
	Q_PROPERTY(QString languageCode READ languageCode WRITE setLanguageCode NOTIFY optionsChanged)
	Q_PROPERTY(QStringList sketchColors READ sketchColors WRITE setSketchColors NOTIFY sketchColorsChanged)
	Q_PROPERTY(QString debugPage READ debugPage CONSTANT)

public:
	explicit MobileSettingsBridge(QObject* parent = nullptr);

	QString receiverSummary() const;
	bool hasSavedReceiver() const;
	bool gnssAutoConnect() const;
	void setGnssAutoConnect(bool value);
	bool ntripAutoStart() const;
	void setNtripAutoStart(bool value);
	bool rawLogging() const;
	void setRawLogging(bool value);

	QStringList ntripProfiles() const;
	QString activeNtripProfile() const;
	void setActiveNtripProfile(const QString& name);

	bool openMruFile() const;
	void setOpenMruFile(bool value);
	QVariantList languages() const;
	QString languageCode() const;
	void setLanguageCode(const QString& code);

	QStringList sketchColors() const;
	void setSketchColors(const QStringList& colors);
	QString debugPage() const { return qEnvironmentVariable("MAPPER_DEBUG_PAGE"); }

	/// Run the BLE receiver scan and picker (existing widget dialog).
	Q_INVOKABLE void chooseReceiver();
	/// Forget the saved external receiver; positions come from the system.
	Q_INVOKABLE void useSystemLocation();

	/// Load one profile's editable fields (password omitted, but a
	/// `hasPassword` flag reports whether one is stored).
	Q_INVOKABLE QVariantMap loadNtripProfile(const QString& name) const;
	/// Create or update a profile. `original_name` empty means create.
	/// An empty password field keeps a previously stored password.
	Q_INVOKABLE QString saveNtripProfile(const QString& original_name,
	                                     const QVariantMap& fields);
	Q_INVOKABLE void removeNtripProfile(const QString& name);

	/// Built-in sketch color presets as {name, colors} maps.
	Q_INVOKABLE QVariantList sketchPresets() const;

signals:
	void receiverChanged();
	void optionsChanged();
	void profilesChanged();
	void sketchColorsChanged();
	/// Ask the hosting dialog to close.
	void closeRequested();

public slots:
	void requestClose() { emit closeRequested(); }
};


/**
 * Full-screen, touch-first settings for mobile mode, rendered with
 * Qt Quick Controls (Material).
 */
class MobileSettingsDialog : public QDialog
{
	Q_OBJECT

public:
	explicit MobileSettingsDialog(QWidget* parent = nullptr);
	~MobileSettingsDialog() override;

private:
	QQuickWidget* quick_widget;
	MobileSettingsBridge* bridge;
};

}  // namespace OpenOrienteering

#endif
