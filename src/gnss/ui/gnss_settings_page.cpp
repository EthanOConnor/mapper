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

#include "gnss_settings_page.h"

#include <QCheckBox>
#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QSpacerItem>
#include <QWidget>

#include "settings.h"
#include "gui/util_gui.h"
#include "gui/widgets/settings_page.h"
#include "gnss/ui/ntrip_settings_widget.h"


namespace OpenOrienteering {

GnssSettingsPage::GnssSettingsPage(QWidget* parent)
 : SettingsPage(parent)
{
	auto* layout = new QFormLayout(this);

	layout->addRow(Util::Headline::create(tr("GNSS Receiver:")));

	device_selector = new QComboBox(this);
	device_selector->setEditable(true);
	layout->addRow(tr("Device:"), device_selector);

	auto_connect_box = new QCheckBox(tr("Connect automatically when GPS enabled"), this);
	layout->addRow(auto_connect_box);

	layout->addItem(Util::SpacerItem::create(this));
	layout->addRow(Util::Headline::create(tr("NTRIP Corrections:")));

	ntrip_widget = new NtripSettingsWidget(this);
	layout->addRow(ntrip_widget);

	auto_start_ntrip_box = new QCheckBox(tr("Start corrections automatically"), this);
	layout->addRow(auto_start_ntrip_box);

	layout->addItem(Util::SpacerItem::create(this));
	layout->addRow(Util::Headline::create(tr("Logging:")));

	raw_logging_box = new QCheckBox(tr("Log raw GNSS data stream"), this);
	layout->addRow(raw_logging_box);

	layout->addItem(Util::SpacerItem::create(this));

	updateWidgets();
}

GnssSettingsPage::~GnssSettingsPage() = default;


QString GnssSettingsPage::title() const
{
	return tr("GNSS");
}


void GnssSettingsPage::apply()
{
	auto& settings = Settings::getInstance();
	settings.setGnssDeviceName(device_selector->currentText());
	settings.setGnssAutoConnect(auto_connect_box->isChecked());
	settings.setGnssAutoStartNtrip(auto_start_ntrip_box->isChecked());
	settings.setGnssRawLogging(raw_logging_box->isChecked());

	// Save the selected NTRIP profile as active
	auto active = ntrip_widget->selectedProfileName();
	settings.setGnssNtripActiveProfile(active);

	settings.applySettings();
}


void GnssSettingsPage::reset()
{
	updateWidgets();
}


void GnssSettingsPage::updateWidgets()
{
	auto& settings = Settings::getInstance();

	device_selector->clear();
	auto device_name = settings.gnssDeviceName();
	if (device_name.isEmpty())
		device_selector->addItem(tr("No device"));
	else
		device_selector->addItem(device_name);

	auto_connect_box->setChecked(settings.gnssAutoConnect());
	auto_start_ntrip_box->setChecked(settings.gnssAutoStartNtrip());
	raw_logging_box->setChecked(settings.gnssRawLogging());

	// Select the active NTRIP profile in the list
	auto activeProfile = settings.gnssNtripActiveProfile();
	if (!activeProfile.isEmpty())
		ntrip_widget->selectProfile(activeProfile);
}


}  // namespace OpenOrienteering
