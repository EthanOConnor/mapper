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

#include "gnss_device_dialog.h"

#include <QLabel>
#include <QListView>
#include <QPushButton>
#include <QStackedWidget>
#include <QVBoxLayout>

#include "gnss/transport/ble_device_model.h"
#include "gui/util_gui.h"

namespace OpenOrienteering {


GnssDeviceDialog::GnssDeviceDialog(QWidget* parent)
    : QDialog(parent)
{
	setWindowTitle(tr("GNSS Receiver"));
	setMinimumSize(300, 400);
	setupUi();
}

GnssDeviceDialog::~GnssDeviceDialog() = default;


void GnssDeviceDialog::setDeviceModel(BleDeviceModel* model)
{
	device_list->setModel(model);
}


void GnssDeviceDialog::showConnecting(const QString& deviceName, int attempt, int maxAttempts)
{
	if (!deviceName.isEmpty())
		m_connectingDeviceName = deviceName;
	QString text = m_connectingDeviceName;
	if (attempt > 0 && maxAttempts > 0)
		text += QStringLiteral("\n\nConnecting — attempt %1 of %2...").arg(attempt).arg(maxAttempts);
	else
		text += QStringLiteral("\n\nConnecting...");
	connecting_label->setText(text);
	stack->setCurrentWidget(connecting_page);
}


void GnssDeviceDialog::showConnected(const QString& deviceName, const QString& firmware)
{
	connected_device_label->setText(deviceName);
	firmware_label->setText(tr("Firmware: %1").arg(firmware));
	stack->setCurrentWidget(connected_page);
}


void GnssDeviceDialog::showScanPage(const QString& errorMessage)
{
	if (errorMessage.isEmpty())
		scan_status_label->setText(tr("Scanning for devices..."));
	else
		scan_status_label->setText(errorMessage);
	stack->setCurrentWidget(scan_page);
}


void GnssDeviceDialog::setupUi()
{
	stack = new QStackedWidget();

	setupScanPage();
	setupConnectingPage();
	setupConnectedPage();

	stack->addWidget(scan_page);
	stack->addWidget(connecting_page);
	stack->addWidget(connected_page);
	stack->setCurrentWidget(scan_page);

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->addWidget(stack);
}


void GnssDeviceDialog::setupScanPage()
{
	scan_page = new QWidget();
	auto* layout = new QVBoxLayout(scan_page);
	layout->setContentsMargins(16, 16, 16, 16);
	layout->setSpacing(12);

	// Title
	auto* title = new QLabel(tr("Select GNSS Receiver"));
	auto title_font = title->font();
	title_font.setBold(true);
	title_font.setPointSize(title_font.pointSize() + 4);
	title->setFont(title_font);
	layout->addWidget(title);

	// Device list
	device_list = new QListView();
	const int row_height = qRound(Util::mmToPixelLogical(14));
	device_list->setUniformItemSizes(true);
	device_list->setGridSize(QSize(-1, row_height));
	connect(device_list, &QListView::clicked, this, [this](const QModelIndex& index) {
		emit deviceSelected(index.row());
		if (auto* model = device_list->model())
			showConnecting(model->data(index, Qt::DisplayRole).toString());
	});
	layout->addWidget(device_list, 1);

	// Scan status
	scan_status_label = new QLabel(tr("Scanning for devices..."));
	auto status_font = scan_status_label->font();
	status_font.setItalic(true);
	scan_status_label->setFont(status_font);
	layout->addWidget(scan_status_label);

	// Refresh button
	scan_button = new QPushButton(tr("Refresh"));
	scan_button->setMinimumHeight(44);  // touch target
	layout->addWidget(scan_button);
}


void GnssDeviceDialog::setupConnectingPage()
{
	connecting_page = new QWidget();
	auto* layout = new QVBoxLayout(connecting_page);
	layout->setContentsMargins(16, 16, 16, 16);
	layout->setSpacing(12);

	// Title
	auto* title = new QLabel(tr("Connecting..."));
	auto title_font = title->font();
	title_font.setBold(true);
	title_font.setPointSize(title_font.pointSize() + 4);
	title->setFont(title_font);
	layout->addWidget(title);

	layout->addStretch();

	// Device name
	connecting_label = new QLabel();
	connecting_label->setAlignment(Qt::AlignCenter);
	layout->addWidget(connecting_label);

	layout->addStretch();

	// Cancel button
	cancel_button = new QPushButton(tr("Cancel"));
	cancel_button->setMinimumHeight(44);
	connect(cancel_button, &QPushButton::clicked, this, [this]() {
		emit cancelConnection();
		showScanPage();
	});
	layout->addWidget(cancel_button);
}


void GnssDeviceDialog::setupConnectedPage()
{
	connected_page = new QWidget();
	auto* layout = new QVBoxLayout(connected_page);
	layout->setContentsMargins(16, 16, 16, 16);
	layout->setSpacing(12);

	// Title
	auto* title = new QLabel(tr("Connected"));
	auto title_font = title->font();
	title_font.setBold(true);
	title_font.setPointSize(title_font.pointSize() + 4);
	title->setFont(title_font);
	title->setStyleSheet(QStringLiteral("color: green"));
	layout->addWidget(title);

	layout->addStretch();

	// Device name
	connected_device_label = new QLabel();
	connected_device_label->setAlignment(Qt::AlignCenter);
	layout->addWidget(connected_device_label);

	// Firmware
	firmware_label = new QLabel();
	firmware_label->setAlignment(Qt::AlignCenter);
	layout->addWidget(firmware_label);

	layout->addStretch();

	// Done button
	done_button = new QPushButton(tr("Done"));
	done_button->setMinimumHeight(44);
	connect(done_button, &QPushButton::clicked, this, [this]() {
		emit dialogCompleted();
		accept();
	});
	layout->addWidget(done_button);
}


}  // namespace OpenOrienteering
