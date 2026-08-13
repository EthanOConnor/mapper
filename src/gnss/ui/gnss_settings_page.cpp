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

#include <algorithm>
#include <cmath>

#include <QCheckBox>
#include <QColor>
#include <QComboBox>
#include <QFormLayout>
#include <QFrame>
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
#include "gnss/gnss_controller.h"
#include "gnss/gnss_session.h"
#include "gnss/gnss_state.h"
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


QString protocolName(GnssProtocol protocol)
{
	switch (protocol)
	{
	case GnssProtocol::UBX: return QStringLiteral("UBX");
	case GnssProtocol::NMEA: return QStringLiteral("NMEA");
	case GnssProtocol::Mixed: return QStringLiteral("UBX + NMEA");
	case GnssProtocol::RTCM3: return QStringLiteral("RTCM 3 corrections only");
	case GnssProtocol::BINEX: return QStringLiteral("BINEX observations");
	case GnssProtocol::BYNAV: return QStringLiteral("BYNAV native");
	case GnssProtocol::Unknown: return GnssSettingsPage::tr("not detected");
	}
	return GnssSettingsPage::tr("not detected");
}


QString correctionStateName(GnssCorrectionState state)
{
	switch (state)
	{
	case GnssCorrectionState::Disabled: return GnssSettingsPage::tr("off");
	case GnssCorrectionState::Disconnected: return GnssSettingsPage::tr("disconnected");
	case GnssCorrectionState::Connecting: return GnssSettingsPage::tr("connecting");
	case GnssCorrectionState::Connected: return GnssSettingsPage::tr("connected; waiting for data");
	case GnssCorrectionState::Flowing: return GnssSettingsPage::tr("flowing");
	case GnssCorrectionState::Stale: return GnssSettingsPage::tr("stale");
	case GnssCorrectionState::Reconnecting: return GnssSettingsPage::tr("reconnecting");
	}
	return GnssSettingsPage::tr("off");
}


QString fixTypeName(GnssFixType type)
{
	switch (type)
	{
	case GnssFixType::RtkFixed: return GnssSettingsPage::tr("RTK fixed");
	case GnssFixType::RtkFloat: return GnssSettingsPage::tr("RTK float");
	case GnssFixType::DGPS: return GnssSettingsPage::tr("Differential fix");
	case GnssFixType::Fix3D: return GnssSettingsPage::tr("3D position");
	case GnssFixType::Fix2D: return GnssSettingsPage::tr("2D position");
	case GnssFixType::NoFix: return GnssSettingsPage::tr("No fix");
	}
	return GnssSettingsPage::tr("No fix");
}


QString healthLine(const QColor& color, const QString& text)
{
	return QStringLiteral("<span style='color:%1'>&#9679;</span>&nbsp; %2")
	       .arg(color.name(), text.toHtmlEscaped());
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
	layout->addRow(Util::Headline::create(tr("Live preflight:")));

	auto* preflight_card = new QFrame(this);
	preflight_card->setObjectName(QStringLiteral("gnssPreflightCard"));
	preflight_card->setStyleSheet(QStringLiteral(
	  "QFrame#gnssPreflightCard { background: #16202b; border-radius: 12px; }"
	  "QFrame#gnssPreflightCard QLabel { color: white; background: transparent; }"));
	auto* preflight_layout = new QVBoxLayout(preflight_card);
	preflight_layout->setContentsMargins(
	  qRound(Util::mmToPixelLogical(4.0)), qRound(Util::mmToPixelLogical(3.0)),
	  qRound(Util::mmToPixelLogical(4.0)), qRound(Util::mmToPixelLogical(3.0)));
	preflight_layout->setSpacing(qRound(Util::mmToPixelLogical(1.3)));
	preflight_title_label = new QLabel(tr("Test GNSS before opening a map"), preflight_card);
	preflight_title_label->setObjectName(QStringLiteral("gnssPreflightTitle"));
	auto title_font = preflight_title_label->font();
	title_font.setBold(true);
	title_font.setPointSizeF(std::max(14.0, title_font.pointSizeF() + 3.0));
	preflight_title_label->setFont(title_font);
	preflight_title_label->setWordWrap(true);
	preflight_detail_label = new QLabel(
	  tr("Connect the real receiver and correction stream here. The session stays ready when you open a map."),
	  preflight_card);
	preflight_detail_label->setObjectName(QStringLiteral("gnssPreflightDetail"));
	preflight_detail_label->setWordWrap(true);
	preflight_receiver_label = new QLabel(preflight_card);
	preflight_receiver_label->setObjectName(QStringLiteral("gnssPreflightReceiver"));
	preflight_receiver_label->setWordWrap(true);
	preflight_corrections_label = new QLabel(preflight_card);
	preflight_corrections_label->setObjectName(QStringLiteral("gnssPreflightCorrections"));
	preflight_corrections_label->setWordWrap(true);
	preflight_error_label = new QLabel(preflight_card);
	preflight_error_label->setObjectName(QStringLiteral("gnssPreflightError"));
	preflight_error_label->setStyleSheet(QStringLiteral("color: #ffb4ab;"));
	preflight_error_label->setWordWrap(true);
	preflight_error_label->hide();
	preflight_layout->addWidget(preflight_title_label);
	preflight_layout->addWidget(preflight_detail_label);
	preflight_layout->addSpacing(qRound(Util::mmToPixelLogical(0.8)));
	preflight_layout->addWidget(preflight_receiver_label);
	preflight_layout->addWidget(preflight_corrections_label);
	preflight_layout->addWidget(preflight_error_label);
	layout->addRow(preflight_card);

	auto* preflight_actions = new QHBoxLayout();
	preflight_start_button = new QPushButton(tr("Connect & test"), this);
	preflight_start_button->setObjectName(QStringLiteral("gnssPreflightStart"));
	preflight_disconnect_button = new QPushButton(tr("Disconnect"), this);
	preflight_disconnect_button->setObjectName(QStringLiteral("gnssPreflightDisconnect"));
	for (auto* button : {preflight_start_button, preflight_disconnect_button})
		button->setMinimumHeight(qRound(Util::mmToPixelLogical(10.0)));
	preflight_actions->addWidget(preflight_start_button, 1);
	preflight_actions->addWidget(preflight_disconnect_button);
	layout->addRow(preflight_actions);

	layout->addItem(Util::SpacerItem::create(this));
	layout->addRow(Util::Headline::create(tr("Logging:")));

	raw_logging_box = new QCheckBox(tr("Log raw GNSS data stream"), this);
	layout->addRow(raw_logging_box);

	layout->addItem(Util::SpacerItem::create(this));

	connect(receiver_mode_box, QOverload<int>::of(&QComboBox::currentIndexChanged),
	        this, [this]() {
		updateDeviceSelector();
		updateCorrectionControls();
		updatePreflightAvailability();
	});
	connect(device_refresh_button, &QPushButton::clicked, this, [this]() {
		auto source = normalizedReceiverSource(
		  receiver_mode_box->currentData().toString());
		if (source == receiver_ble_source)
			GnssController::instance().chooseExternalReceiver(this);
		else
			updateDeviceSelector();
	});
	connect(&GnssController::instance(), &GnssController::sessionChanged,
	        this, [this] {
		updateDeviceSelector();
		bindSession();
	});
	connect(&GnssController::instance(), &GnssController::errorOccurred,
	        this, [this](const QString& source, const QString& message) {
		preflight_error_label->setText(tr("%1: %2").arg(source, message));
		preflight_error_label->show();
	});
	connect(corrections_box, &QCheckBox::toggled,
	        this, &GnssSettingsPage::updateCorrectionControls);
	connect(preflight_start_button, &QPushButton::clicked,
	        this, &GnssSettingsPage::startPreflight);
	connect(preflight_disconnect_button, &QPushButton::clicked,
	        &GnssController::instance(), &GnssController::disconnectExternal);

	updateWidgets();
	bindSession();
	updatePreflightAvailability();
}

GnssSettingsPage::~GnssSettingsPage() = default;


QString GnssSettingsPage::title() const
{
	return tr("GNSS");
}


void GnssSettingsPage::apply()
{
	saveConfiguration();
	Settings::getInstance().applySettings();
}


void GnssSettingsPage::saveConfiguration()
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

}


void GnssSettingsPage::startPreflight()
{
	auto source = normalizedReceiverSource(receiver_mode_box->currentData().toString());
	if (!isExternalReceiverSource(source))
		return;

	// This explicit action commits only this page's GNSS choices. It must use
	// the same persisted configuration as a later map-window connection.
	saveConfiguration();
	Settings::getInstance().applySettings();
	preflight_error_label->hide();

	auto& controller = GnssController::instance();
	controller.configureNtrip(corrections_box->isChecked(),
	                          ntrip_widget->selectedProfileName());
	controller.connectExternal(this);
	bindSession();
}


void GnssSettingsPage::bindSession()
{
	auto* session = GnssController::instance().session();
	if (!session)
	{
		GnssState state;
		updatePreflightState(state);
		return;
	}
	connect(session, &GnssSession::stateChanged,
	        this, &GnssSettingsPage::updatePreflightState,
	        Qt::UniqueConnection);
	updatePreflightState(session->currentState());
}


void GnssSettingsPage::updatePreflightState(const GnssState& state)
{
	const auto connected = state.transportState == GnssTransportState::Connected;
	const auto active = connected
	                 || state.transportState == GnssTransportState::Connecting
	                 || state.transportState == GnssTransportState::Reconnecting;
	const auto has_data = state.receiverBytesReceived > 0;
	const auto has_position = state.solution.hasFreshPosition;
	const auto& position = state.solution.position;

	if (state.transportState == GnssTransportState::Connecting)
	{
		preflight_title_label->setText(tr("Connecting to receiver…"));
		preflight_detail_label->setText(tr("Keep this screen open while Mapper establishes the live data path."));
	}
	else if (state.transportState == GnssTransportState::Reconnecting)
	{
		preflight_title_label->setText(tr("Reconnecting to receiver…"));
		preflight_detail_label->setText(tr("The receiver link was interrupted; Mapper is recovering it automatically."));
	}
	else if (!connected)
	{
		preflight_title_label->setText(tr("Test GNSS before opening a map"));
		preflight_detail_label->setText(
		  tr("Connect the real receiver and correction stream here. The session stays ready when you open a map."));
	}
	else if (!has_data)
	{
		preflight_title_label->setText(tr("Connected, but no receiver data"));
		preflight_detail_label->setText(
		  tr("Bluetooth is connected, but no position bytes are reaching Mapper. Check the receiver output port or choose another receiver."));
	}
	else if (!has_position && state.protocol == GnssProtocol::Unknown)
	{
		preflight_title_label->setText(tr("Receiver data is not recognized"));
		preflight_detail_label->setText(tr("Bytes are arriving, but they are not a supported UBX or NMEA position stream."));
	}
	else if (!has_position && state.protocol != GnssProtocol::UBX
	         && state.protocol != GnssProtocol::NMEA
	         && state.protocol != GnssProtocol::Mixed)
	{
		preflight_title_label->setText(tr("Receiver output needs configuration"));
		preflight_detail_label->setText(
		  tr("Mapper is receiving %1, but not a UBX or NMEA position stream. Choose a receiver profile or enable position output on this port.")
		    .arg(protocolName(state.protocol)));
	}
	else if (!has_position)
	{
		preflight_title_label->setText(tr("Waiting for a usable position"));
		preflight_detail_label->setText(tr("%1 messages are arriving, but the receiver does not currently have a valid fix.")
		                                .arg(protocolName(state.protocol)));
	}
	else
	{
		auto detail = tr("%1 satellites used").arg(position.satellitesUsed);
		if (std::isfinite(position.hAccuracyP95))
			detail = tr("%1 m horizontal accuracy (P95) · %2 satellites used")
			           .arg(position.hAccuracyP95, 0, 'f', 2)
			           .arg(position.satellitesUsed);
		preflight_title_label->setText(fixTypeName(position.fixType));
		preflight_detail_label->setText(
		  tr("%1. Live location and track recording are ready.").arg(detail));
	}

	QString receiver_text;
	if (!connected)
		receiver_text = tr("Receiver link: disconnected");
	else if (!has_data)
		receiver_text = tr("Receiver link: connected · no bytes received");
	else
		receiver_text = tr("Receiver: %1 · %2 bytes received")
		                  .arg(protocolName(state.protocol))
		                  .arg(state.receiverBytesReceived);
	preflight_receiver_label->setText(healthLine(
	  has_position ? QColor(0x4C, 0xAF, 0x50)
	               : connected ? QColor(0xFF, 0xA7, 0x26) : QColor(0xEF, 0x53, 0x50),
	  receiver_text));

	auto correction_text = tr("Corrections: %1").arg(correctionStateName(state.correctionState));
	if (state.ntripBytesReceived > 0 || state.ntripBytesSentToReceiver > 0
	    || state.ntripBytesDroppedToReceiver > 0)
	{
		correction_text += tr(" · %1 received · %2 sent")
		                     .arg(state.ntripBytesReceived)
		                     .arg(state.ntripBytesSentToReceiver);
		if (state.ntripBytesDroppedToReceiver > 0)
			correction_text += tr(" · %1 dropped").arg(state.ntripBytesDroppedToReceiver);
	}
	preflight_corrections_label->setText(healthLine(
	  state.correctionState == GnssCorrectionState::Flowing
	    && state.ntripBytesSentToReceiver > 0
	    ? QColor(0x4C, 0xAF, 0x50)
	    : state.correctionState == GnssCorrectionState::Disabled
	      ? QColor(0x9E, 0x9E, 0x9E) : QColor(0xFF, 0xA7, 0x26),
	  correction_text));

	preflight_start_button->setText(active ? tr("Apply & test") : tr("Connect & test"));
	preflight_disconnect_button->setEnabled(active);
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
	updatePreflightAvailability();
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
	if (source == receiver_ble_source)
		device_refresh_button->setText(tr("Scan"));
	else
		device_refresh_button->setText(tr("Refresh"));
	device_refresh_button->setEnabled(
	  source == receiver_ble_source || source == receiver_serial_source);
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


void GnssSettingsPage::updatePreflightAvailability()
{
	auto source = normalizedReceiverSource(receiver_mode_box->currentData().toString());
	auto external = isExternalReceiverSource(source);
	preflight_start_button->setEnabled(external);
	if (!external)
	{
		preflight_title_label->setText(tr("System location uses the iPhone"));
		preflight_detail_label->setText(
		  tr("External receiver preflight is available when a Bluetooth or serial receiver is selected."));
	}
}


}  // namespace OpenOrienteering
