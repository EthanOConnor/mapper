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


#ifndef OPENORIENTEERING_GNSS_SETTINGS_PAGE_H
#define OPENORIENTEERING_GNSS_SETTINGS_PAGE_H

#include <QObject>
#include <QString>

#include "gui/widgets/settings_page.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QWidget;


namespace OpenOrienteering {

class NtripSettingsWidget;
struct GnssState;


class GnssSettingsPage : public SettingsPage
{
	Q_OBJECT
public:
	explicit GnssSettingsPage(QWidget* parent = nullptr);
	~GnssSettingsPage() override;

	QString title() const override;
	void apply() override;
	void reset() override;

	/// Update the map-independent live preflight presentation. Public so the
	/// application-wide session and focused UI tests share the same path.
	void updatePreflightState(const GnssState& state);

private:
	void saveConfiguration();
	void bindSession();
	void startPreflight();
	void updateWidgets();
	void updateDeviceSelector();
	void updateCorrectionControls();
	void updatePreflightAvailability();

	QComboBox* receiver_mode_box;
	QComboBox* device_selector;
	QPushButton* device_refresh_button;
	QCheckBox* auto_connect_box;
	QCheckBox* corrections_box;
	QCheckBox* raw_logging_box;
	NtripSettingsWidget* ntrip_widget;
	QLabel* preflight_title_label;
	QLabel* preflight_detail_label;
	QLabel* preflight_receiver_label;
	QLabel* preflight_corrections_label;
	QLabel* preflight_error_label;
	QPushButton* preflight_start_button;
	QPushButton* preflight_disconnect_button;
};


}  // namespace OpenOrienteering

#endif
