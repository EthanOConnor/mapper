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
#include <QHBoxLayout>
#include <QLabel>
#include <QLatin1String>
#include <QPushButton>
#include <QSignalBlocker>
#include <QSpacerItem>
#include <QWidget>

#if defined(MAPPER_GNSS_ANDROID_USB_SERIAL)
#  include "gnss/transport/android_usb_serial_transport.h"
#endif
#if defined(MAPPER_GNSS_SERIAL)
#  include <QSerialPortInfo>
#endif

#include "settings.h"
#include "gui/util_gui.h"
#include "gui/widgets/settings_page.h"
#include "gnss/ui/ntrip_settings_widget.h"


namespace OpenOrienteering {

namespace {

const QLatin1String receiver_system_source("");
const QLatin1String receiver_ble_source("external_ble");
const QLatin1String receiver_ble_ntrip_legacy_source("external_ble_ntrip");
const QLatin1String receiver_spp_source("external_spp");
const QLatin1String receiver_serial_source("external_serial");

bool isExternalReceiverSource(const QString& source)
{
	return source == receiver_ble_source
	    || source == receiver_ble_ntrip_legacy_source
	    || source == receiver_spp_source
	    || source == receiver_serial_source;
}


QString normalizedReceiverSource(const QString& source)
{
	if (source == receiver_ble_ntrip_legacy_source)
		return receiver_ble_source;
	return source;
}


#if defined(MAPPER_GNSS_SERIAL)
const QLatin1String serial_address_prefix("serial:");

QString serialDeviceAddress(const QString& endpoint)
{
	return serial_address_prefix + endpoint;
}


QString displayNameForSerialPort(const QSerialPortInfo& port)
{
	auto port_name = port.portName();
	if (port_name.isEmpty())
		port_name = port.systemLocation();

	if (port.description().isEmpty())
		return port_name;

	return port.description() + QLatin1String(" (") + port_name + QLatin1Char(')');
}


QString endpointForSerialPort(const QSerialPortInfo& port)
{
	return port.systemLocation().isEmpty() ? port.portName() : port.systemLocation();
}
#endif


#if defined(MAPPER_GNSS_ANDROID_USB_SERIAL)
const QLatin1String android_usb_serial_address_prefix("android-usb:");

QString androidUsbSerialDeviceAddress(const QString& endpoint)
{
	return android_usb_serial_address_prefix + endpoint;
}
#endif

}  // namespace


GnssSettingsPage::GnssSettingsPage(QWidget* parent)
 : SettingsPage(parent)
{
	auto* layout = new QFormLayout(this);

	layout->addRow(Util::Headline::create(tr("Receiver:")));

	receiver_mode_box = new QComboBox(this);
	layout->addRow(tr("Connection:"), receiver_mode_box);

	auto* device_layout = new QHBoxLayout();
	device_selector = new QComboBox(this);
	device_selector->setEditable(false);
	device_refresh_button = new QPushButton(tr("Refresh"), this);
	device_layout->addWidget(device_selector, 1);
	device_layout->addWidget(device_refresh_button);
	layout->addRow(tr("Device:"), device_layout);

	auto_connect_box = new QCheckBox(tr("Connect when live GNSS starts"), this);
	layout->addRow(auto_connect_box);

	layout->addItem(Util::SpacerItem::create(this));
	layout->addRow(Util::Headline::create(tr("Corrections:")));

	corrections_box = new QCheckBox(tr("Use NTRIP corrections"), this);
	layout->addRow(corrections_box);

	ntrip_widget = new NtripSettingsWidget(this);
	layout->addRow(ntrip_widget);

	layout->addItem(Util::SpacerItem::create(this));
	layout->addRow(Util::Headline::create(tr("Logging:")));

	raw_logging_box = new QCheckBox(tr("Log raw GNSS data stream"), this);
	layout->addRow(raw_logging_box);

	layout->addItem(Util::SpacerItem::create(this));

	connect(receiver_mode_box, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [this]() {
		updateDeviceSelector();
		updateCorrectionControls();
	});
	connect(device_refresh_button, &QPushButton::clicked,
	        this, &GnssSettingsPage::updateDeviceSelector);
	connect(corrections_box, &QCheckBox::toggled,
	        this, &GnssSettingsPage::updateCorrectionControls);

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

	auto source = normalizedReceiverSource(receiver_mode_box->currentData().toString());
	settings.setPositionSource(source);

	if (isExternalReceiverSource(source))
	{
		auto address = device_selector->currentData().toString();
		settings.setGnssDeviceAddress(address);
		settings.setGnssDeviceName(address.isEmpty() ? QString{} : device_selector->currentText());
	}

	settings.setGnssAutoConnect(auto_connect_box->isChecked());
	settings.setGnssAutoStartNtrip(corrections_box->isChecked() && isExternalReceiverSource(source));
	settings.setGnssRawLogging(raw_logging_box->isChecked());

	settings.setGnssNtripActiveProfile(ntrip_widget->selectedProfileName());

	settings.applySettings();
}


void GnssSettingsPage::reset()
{
	updateWidgets();
}


void GnssSettingsPage::updateWidgets()
{
	auto& settings = Settings::getInstance();

	const QSignalBlocker mode_blocker(receiver_mode_box);
	const QSignalBlocker corrections_blocker(corrections_box);

	auto source = settings.positionSource();
	auto receiver_source = normalizedReceiverSource(source);
	auto corrections_enabled = isExternalReceiverSource(receiver_source)
	    && (settings.gnssAutoStartNtrip() || source == receiver_ble_ntrip_legacy_source);

	receiver_mode_box->clear();
	receiver_mode_box->addItem(tr("System location"), receiver_system_source);
	if (!isExternalReceiverSource(receiver_source) && !receiver_source.isEmpty())
	{
		// Preserve an explicitly selected Qt positioning backend while this
		// unified page owns the location-source setting.
		receiver_mode_box->addItem(
		  tr("System location (%1)").arg(receiver_source), receiver_source);
	}
#if defined(MAPPER_GNSS_BLE_COREBLUETOOTH) || defined(MAPPER_GNSS_BLE)
	receiver_mode_box->addItem(tr("Bluetooth LE receiver"), receiver_ble_source);
#endif
#ifdef MAPPER_GNSS_SPP
	receiver_mode_box->addItem(tr("Bluetooth Classic receiver"), receiver_spp_source);
#endif
#if defined(MAPPER_GNSS_SERIAL) || defined(MAPPER_GNSS_ANDROID_USB_SERIAL)
	receiver_mode_box->addItem(tr("USB serial receiver"), receiver_serial_source);
#endif

	auto mode_index = receiver_mode_box->findData(receiver_source);
	if (mode_index < 0)
		mode_index = 0;
	receiver_mode_box->setCurrentIndex(mode_index);

	corrections_box->setChecked(corrections_enabled);
	auto_connect_box->setChecked(settings.gnssAutoConnect());
	raw_logging_box->setChecked(settings.gnssRawLogging());

	auto activeProfile = settings.gnssNtripActiveProfile();
	if (!activeProfile.isEmpty())
		ntrip_widget->selectProfile(activeProfile);

	updateDeviceSelector();
	updateCorrectionControls();
}


void GnssSettingsPage::updateDeviceSelector()
{
	auto& settings = Settings::getInstance();
	auto source = normalizedReceiverSource(receiver_mode_box->currentData().toString());
	auto saved_address = settings.gnssDeviceAddress();
	auto saved_name = settings.gnssDeviceName();

	const QSignalBlocker blocker(device_selector);
	device_selector->clear();

	auto add_device = [this, &saved_address](const QString& name, const QString& address) {
		if (address.isEmpty())
			return false;
		auto display_name = name.isEmpty() ? address : name;
		device_selector->addItem(display_name, address);
		if (address == saved_address)
			device_selector->setCurrentIndex(device_selector->count() - 1);
		return true;
	};

	if (source == receiver_serial_source)
	{
#if defined(MAPPER_GNSS_ANDROID_USB_SERIAL)
		for (const auto& device : AndroidUsbSerialTransport::availableDevices())
			add_device(device.name, androidUsbSerialDeviceAddress(device.address));
#endif

#if defined(MAPPER_GNSS_SERIAL)
		for (const auto& port : QSerialPortInfo::availablePorts())
		{
			auto endpoint = endpointForSerialPort(port);
			if (endpoint.isEmpty())
				continue;
			add_device(displayNameForSerialPort(port), serialDeviceAddress(endpoint));
		}
#endif
	}
	else if (source == receiver_ble_source || source == receiver_spp_source)
	{
		if (!saved_address.isEmpty())
			add_device(saved_name, saved_address);
	}

	if (!saved_address.isEmpty() && device_selector->findData(saved_address) < 0)
	{
		auto display_name = saved_name.isEmpty()
		    ? saved_address
		    : saved_name + QLatin1String(" (not connected)");
		add_device(display_name, saved_address);
	}

	if (device_selector->count() == 0)
	{
		auto text = source == receiver_serial_source
		    ? tr("No USB serial receiver found")
		    : tr("No saved receiver");
		device_selector->addItem(text, QString{});
	}

	auto external = isExternalReceiverSource(source);
	device_selector->setEnabled(external);
	device_refresh_button->setEnabled(source == receiver_serial_source);
	auto_connect_box->setEnabled(external && !device_selector->currentData().toString().isEmpty());
}


void GnssSettingsPage::updateCorrectionControls()
{
	auto source = normalizedReceiverSource(receiver_mode_box->currentData().toString());
	auto external = isExternalReceiverSource(source);
	if (!external && corrections_box->isChecked())
		corrections_box->setChecked(false);
	corrections_box->setEnabled(external);
	ntrip_widget->setEnabled(external && corrections_box->isChecked());
}


}  // namespace OpenOrienteering
