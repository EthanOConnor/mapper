/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_GNSS_CONTROLLER_H
#define OPENORIENTEERING_GNSS_CONTROLLER_H

#include <QPointer>
#include <memory>

#include <QObject>

class QWidget;

#if defined(MAPPER_GNSS_BLE)
class QBluetoothDeviceDiscoveryAgent;
#endif

namespace OpenOrienteering {

class BleDeviceModel;
class GnssDeviceDialog;
class GnssSession;
#if defined(MAPPER_GNSS_BLE_COREBLUETOOTH)
class BleDiscoveryAgent;
#endif
class GnssTransport;

/**
 * Application-scoped owner for the external GNSS connection.
 *
 * A receiver and NTRIP stream outlive individual map windows, which avoids
 * tearing down RTK state when the user opens or switches a document. Map
 * editors subscribe to the session and keep their existing mature position
 * overlay, tracking, and point-placement behavior.
 */
class GnssController final : public QObject
{
	Q_OBJECT

public:
	static GnssController& instance();

	GnssSession* session() const { return m_session; }
	bool isActive() const;

	/// Start or resume the configured external receiver, presenting a native
	/// BLE picker when no saved receiver can be connected automatically.
	void connectExternal(QWidget* parent);
	/// Stop any current receiver and present an interactive BLE scan so the
	/// user can choose or replace the configured receiver.
	void chooseExternalReceiver(QWidget* parent);
	void disconnectExternal();
	void useNtripProfile(const QString& name);
	/// Apply correction settings to the application-wide session. This is used
	/// by GNSS Settings preflight as well as by map-window controls.
	void configureNtrip(bool enabled, const QString& profile_name);

signals:
	void sessionChanged(OpenOrienteering::GnssSession* session);
	void internalLocationRequested();
	void connectionCancelled();
	void errorOccurred(const QString& source, const QString& message);

private:
	explicit GnssController(QObject* parent = nullptr);
	~GnssController() override;

	void ensureSession();
	/// A transport for the receiver already stored in the settings, or null
	/// when none is stored or its transport is not available in this build.
	std::unique_ptr<GnssTransport> createSavedTransport();
	void loadActiveNtripProfile();
	void startDiscovery(QWidget* parent, bool force_picker = false);
	void showDevicePicker(QWidget* parent);
	void connectDevice(int row);
	void finishDiscovery();
	void handleApplicationStateChanged(Qt::ApplicationState state);

	GnssSession* m_session = nullptr;
	BleDeviceModel* m_device_model = nullptr;
	QPointer<GnssDeviceDialog> m_device_dialog;
#if defined(MAPPER_GNSS_BLE_COREBLUETOOTH)
	BleDiscoveryAgent* m_discovery = nullptr;
#endif
#if defined(MAPPER_GNSS_BLE)
	QBluetoothDeviceDiscoveryAgent* m_qt_discovery = nullptr;
#endif
	QPointer<QWidget> m_picker_parent;
	quint64 m_discovery_generation = 0;
};

}  // namespace OpenOrienteering

#endif
