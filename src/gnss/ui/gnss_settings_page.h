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
class QWidget;


namespace OpenOrienteering {

class NtripSettingsWidget;


class GnssSettingsPage : public SettingsPage
{
	Q_OBJECT
public:
	explicit GnssSettingsPage(QWidget* parent = nullptr);
	~GnssSettingsPage() override;

	QString title() const override;
	void apply() override;
	void reset() override;

private:
	void updateWidgets();

	QComboBox* device_selector;
	QCheckBox* auto_connect_box;
	QCheckBox* auto_start_ntrip_box;
	QCheckBox* raw_logging_box;
	NtripSettingsWidget* ntrip_widget;
};


}  // namespace OpenOrienteering

#endif
