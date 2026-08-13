/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "gnss_controller.h"

#include <memory>

#include <QAbstractItemModel>
#include <QApplication>
#include <QDialog>
#include <QTimer>
#include <QWidget>

#include "settings.h"
#include "gnss/correction/ntrip_client.h"
#include "gnss/correction/ntrip_profile_store.h"
#include "gnss/gnss_session.h"
#include "gnss/transport/ble_device_model.h"
#include "gnss/ui/gnss_device_dialog.h"
#if defined(MAPPER_GNSS_BLE_COREBLUETOOTH)
#  include "gnss/transport/ble_discovery_agent.h"
#endif

namespace OpenOrienteering {

GnssController& GnssController::instance()
{
	static GnssController controller;
	return controller;
}

GnssController::GnssController(QObject* parent)
 : QObject(parent)
 , m_device_model(new BleDeviceModel(this))
{
	connect(qApp, &QGuiApplication::applicationStateChanged,
	        this, &GnssController::handleApplicationStateChanged);
}

GnssController::~GnssController()
{
	disconnectExternal();
}

bool GnssController::isActive() const
{
	return m_session && m_session->isActive();
}

void GnssController::ensureSession()
{
	if (m_session)
		return;

	m_session = new GnssSession(this);
	m_session->setAutoReconnect(true);
	m_session->setRawLogging(Settings::getInstance().gnssRawLogging());
	connect(m_session, &GnssSession::errorOccurred,
	        this, &GnssController::errorOccurred);
	loadActiveNtripProfile();
	emit sessionChanged(m_session);
}

void GnssController::loadActiveNtripProfile()
{
	if (!m_session || !Settings::getInstance().gnssAutoStartNtrip())
		return;

	auto name = Settings::getInstance().gnssNtripActiveProfile();
	if (name.isEmpty())
		return;
	QString error;
	auto profile = NtripProfileStore::load(name, &error);
	if (!profile)
	{
		emit errorOccurred(tr("NTRIP"),
		                   error.isEmpty()
		                     ? tr("The active correction profile is unavailable.")
		                     : error);
		return;
	}
	auto client = std::make_unique<NtripClient>(m_session);
	client->setProfile(*profile);
	m_session->setNtripClient(std::move(client));
}

void GnssController::connectExternal(QWidget* parent)
{
	ensureSession();
	if (m_session->isActive())
	{
		emit sessionChanged(m_session);
		return;
	}
	startDiscovery(parent);
}

void GnssController::chooseExternalReceiver(QWidget* parent)
{
	ensureSession();
	if (m_session->isActive())
		m_session->stop();
	startDiscovery(parent, true);
}

void GnssController::startDiscovery(QWidget* parent, bool force_picker)
{
	m_picker_parent = parent;
	++m_discovery_generation;
	m_device_model->clear();

#if defined(MAPPER_GNSS_BLE_COREBLUETOOTH)
	delete m_discovery;
	m_discovery = new BleDiscoveryAgent(m_device_model, this);
	m_discovery->startScan();

	auto generation = m_discovery_generation;
	auto saved_address = Settings::getInstance().gnssDeviceAddress();
	auto auto_connect = !force_picker
	                 && Settings::getInstance().gnssAutoConnect()
	                 && !saved_address.isEmpty();
	connect(m_device_model, &QAbstractItemModel::rowsInserted,
	        m_discovery, [this, generation, saved_address, auto_connect](
	                       const QModelIndex&, int first, int last) {
		if (!auto_connect || generation != m_discovery_generation
		    || !m_discovery || isActive())
			return;
		for (int row = first; row <= last; ++row)
		{
			if (m_device_model->deviceAt(row).address == saved_address)
			{
				connectDevice(row);
				return;
			}
		}
	});

	if (!auto_connect)
		showDevicePicker(parent);
	else
	{
		// A saved receiver normally appears immediately. If it does not, make
		// the state visible rather than leaving the GPS action apparently idle.
		QTimer::singleShot(2500, this, [this, generation] {
			if (generation == m_discovery_generation && !isActive()
			    && m_discovery)
				showDevicePicker(m_picker_parent);
		});
	}
#else
	Q_UNUSED(parent)
	emit errorOccurred(
	  tr("GNSS"),
	  tr("This build does not include a native Bluetooth GNSS transport."));
	emit connectionCancelled();
#endif
}

void GnssController::showDevicePicker(QWidget* parent)
{
	if (m_device_dialog)
	{
		m_device_dialog->raise();
		m_device_dialog->activateWindow();
		return;
	}

	auto* dialog = new GnssDeviceDialog(parent);
	m_device_dialog = dialog;
	dialog->setAttribute(Qt::WA_DeleteOnClose);
	dialog->setDeviceModel(m_device_model);
	dialog->showScanPage();
	connect(dialog, &GnssDeviceDialog::deviceSelected,
	        this, &GnssController::connectDevice);
	connect(dialog, &GnssDeviceDialog::refreshRequested,
	        this, [this, parent] { startDiscovery(parent, true); });
	connect(dialog, &GnssDeviceDialog::internalLocationRequested,
	        this, [this, dialog] {
			dialog->accept();
			Settings::getInstance().setPositionSource(QString{});
			disconnectExternal();
			emit internalLocationRequested();
		});
	connect(dialog, &GnssDeviceDialog::cancelConnection,
	        this, [this] {
			if (m_session)
				m_session->stop();
			if (m_device_dialog)
				m_device_dialog->showScanPage();
		});
	connect(dialog, &GnssDeviceDialog::dialogCompleted,
	        dialog, &QDialog::accept);
	connect(dialog, &QDialog::rejected, this, [this] {
		finishDiscovery();
		emit connectionCancelled();
	});
	connect(dialog, &QObject::destroyed, this, [this] {
		m_device_dialog = nullptr;
	});

	if (m_session)
	{
		connect(m_session, &GnssSession::stateChanged, dialog,
		        [this, dialog](const GnssState& state) {
			if (state.transportState == GnssTransportState::Connected)
				dialog->showConnected(state.deviceName,
				                      state.receiverSwVersion);
			else if (state.transportState
			         == GnssTransportState::Disconnected
			         && !state.deviceName.isEmpty())
				dialog->showScanPage(
				  tr("Could not connect. Make sure the receiver is on and nearby."));
		});
	}
	dialog->show();
	dialog->raise();
}

void GnssController::connectDevice(int row)
{
	if (row < 0 || row >= m_device_model->rowCount())
		return;
	auto device = m_device_model->deviceAt(row);
	if (m_device_dialog)
		m_device_dialog->showConnecting(device.name);

#if defined(MAPPER_GNSS_BLE_COREBLUETOOTH)
	if (!m_discovery)
		return;
	auto transport = m_discovery->createTransportForDevice(
	  device.address, device.name);
	if (!transport)
	{
		if (m_device_dialog)
			m_device_dialog->showScanPage(
			  tr("That receiver is no longer available. Scan again and try again."));
		return;
	}

	delete m_discovery;
	m_discovery = nullptr;
	m_session->setTransport(std::move(transport));
	Settings::getInstance().setGnssDeviceAddress(device.address);
	Settings::getInstance().setGnssDeviceName(device.name);
	m_session->start();
	emit sessionChanged(m_session);
#else
	Q_UNUSED(device)
#endif
}

void GnssController::finishDiscovery()
{
	++m_discovery_generation;
#if defined(MAPPER_GNSS_BLE_COREBLUETOOTH)
	if (m_discovery)
	{
		m_discovery->stopScan();
		delete m_discovery;
		m_discovery = nullptr;
	}
#endif
}

void GnssController::disconnectExternal()
{
	finishDiscovery();
	if (m_device_dialog)
	{
		m_device_dialog->disconnect(this);
		m_device_dialog->close();
		m_device_dialog = nullptr;
	}
	if (m_session)
		m_session->stop();
}

void GnssController::useNtripProfile(const QString& name)
{
	configureNtrip(true, name);
}

void GnssController::configureNtrip(bool enabled, const QString& name)
{
	auto& settings = Settings::getInstance();
	settings.setGnssAutoStartNtrip(enabled);
	if (!enabled)
	{
		if (m_session)
			m_session->clearNtripClient();
		return;
	}

	if (name.isEmpty())
	{
		emit errorOccurred(tr("NTRIP"),
		                   tr("Choose a correction profile before starting the live test."));
		return;
	}
	ensureSession();
	QString error;
	auto profile = NtripProfileStore::load(name, &error);
	if (!profile)
	{
		emit errorOccurred(tr("NTRIP"), error);
		return;
	}
	auto client = std::make_unique<NtripClient>(m_session);
	client->setProfile(*profile);
	m_session->setNtripClient(std::move(client));
	settings.setGnssNtripActiveProfile(name);
	if (m_session->isActive())
		m_session->startNtrip();
}

void GnssController::handleApplicationStateChanged(Qt::ApplicationState state)
{
	if (state == Qt::ApplicationActive && m_session)
		m_session->handleForegroundResume();
	if (state != Qt::ApplicationActive)
		finishDiscovery();
}

}  // namespace OpenOrienteering
