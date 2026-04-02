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


#ifndef OPENORIENTEERING_GNSS_DEVICE_DIALOG_H
#define OPENORIENTEERING_GNSS_DEVICE_DIALOG_H

#include <QDialog>
#include <QString>

class QLabel;
class QListView;
class QPushButton;
class QStackedWidget;
class QWidget;

namespace OpenOrienteering {

class BleDeviceModel;  // from gnss/transport/ble_device_model.h


/// BLE device discovery and connection dialog for GNSS receivers.
///
/// Three-page stacked widget:
///   Page 0 — Device selection (scan results in a QListView)
///   Page 1 — Connecting (shows device name, cancel button)
///   Page 2 — Connected (shows device name + firmware, done button)
class GnssDeviceDialog : public QDialog
{
	Q_OBJECT
public:
	explicit GnssDeviceDialog(QWidget* parent = nullptr);
	~GnssDeviceDialog() override;

	/// Set the device model for scan results.
	void setDeviceModel(BleDeviceModel* model);

	/// Switch to connecting page with device name and attempt counter.
	void showConnecting(const QString& deviceName, int attempt = 0, int maxAttempts = 0);

	/// Switch to connected page with receiver info.
	void showConnected(const QString& deviceName, const QString& firmware);

	/// Revert to scan page (e.g., on connection failure).
	void showScanPage(const QString& errorMessage = {});

signals:
	void deviceSelected(int index);    // from scan list
	void cancelConnection();
	void dialogCompleted();

private:
	void setupUi();
	void setupScanPage();
	void setupConnectingPage();
	void setupConnectedPage();

	QStackedWidget* stack;

	// Page 0: Device selection
	QWidget* scan_page;
	QListView* device_list;
	QLabel* scan_status_label;
	QPushButton* scan_button;

	// Page 1: Connecting
	QWidget* connecting_page;
	QLabel* connecting_label;
	QPushButton* cancel_button;

	// Page 2: Connected
	QWidget* connected_page;
	QLabel* connected_device_label;
	QLabel* firmware_label;
	QPushButton* done_button;
	QString m_connectingDeviceName;
};


}  // namespace OpenOrienteering

#endif
