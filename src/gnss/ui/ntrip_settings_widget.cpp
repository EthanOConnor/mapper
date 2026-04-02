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

#include "ntrip_settings_widget.h"

#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QString>
#include <QStringList>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>


namespace OpenOrienteering {

NtripSettingsWidget::NtripSettingsWidget(QWidget* parent)
 : QWidget(parent)
{
	setupUi();
	loadProfiles();
}

NtripSettingsWidget::~NtripSettingsWidget() = default;


void NtripSettingsWidget::setupUi()
{
	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	profile_list = new QListWidget(this);
	layout->addWidget(profile_list);

	auto* button_layout = new QHBoxLayout();

	add_button = new QPushButton(tr("Add"), this);
	edit_button = new QPushButton(tr("Edit"), this);
	remove_button = new QPushButton(tr("Remove"), this);
	test_button = new QPushButton(tr("Test Connection"), this);

	button_layout->addWidget(add_button);
	button_layout->addWidget(edit_button);
	button_layout->addWidget(remove_button);
	button_layout->addWidget(test_button);

	layout->addLayout(button_layout);

	test_status_label = new QLabel(this);
	test_status_label->setWordWrap(true);
	layout->addWidget(test_status_label);

	connect(add_button, &QPushButton::clicked, this, &NtripSettingsWidget::addProfile);
	connect(edit_button, &QPushButton::clicked, this, [this]() {
		auto row = profile_list->currentRow();
		if (row >= 0)
			editProfile(row);
	});
	connect(remove_button, &QPushButton::clicked, this, &NtripSettingsWidget::removeProfile);
	connect(test_button, &QPushButton::clicked, this, &NtripSettingsWidget::testConnection);
}


QString NtripSettingsWidget::selectedProfileName() const
{
	auto* item = profile_list->currentItem();
	return item ? item->text() : QString{};
}

void NtripSettingsWidget::selectProfile(const QString& name)
{
	for (int i = 0; i < profile_list->count(); ++i) {
		if (profile_list->item(i)->text() == name) {
			profile_list->setCurrentRow(i);
			return;
		}
	}
}


void NtripSettingsWidget::loadProfiles()
{
	profile_list->clear();

	QSettings settings;
	auto names = settings.value(QStringLiteral("Gnss/ntrip_profiles")).toStringList();
	for (const auto& name : names)
		profile_list->addItem(name);
}


void NtripSettingsWidget::saveProfiles()
{
	QStringList names;
	names.reserve(profile_list->count());
	for (int i = 0; i < profile_list->count(); ++i)
		names.append(profile_list->item(i)->text());

	QSettings settings;
	settings.setValue(QStringLiteral("Gnss/ntrip_profiles"), names);
}


void NtripSettingsWidget::addProfile()
{
	bool ok = false;
	auto name = QInputDialog::getText(this, tr("Add NTRIP Profile"),
	                                  tr("Profile name:"),
	                                  QLineEdit::Normal, {}, &ok);
	if (!ok || name.isEmpty())
		return;

	profile_list->addItem(name);
	int row = profile_list->count() - 1;
	profile_list->setCurrentRow(row);
	saveProfiles();

	// Immediately open edit dialog so the user can fill in caster details
	editProfile(row);

	emit profilesChanged();
}


void NtripSettingsWidget::editProfile(int index)
{
	if (index < 0 || index >= profile_list->count())
		return;

	auto name = profile_list->item(index)->text();
	auto prefix = QStringLiteral("Gnss/ntrip_profile/%1/").arg(name);

	QSettings settings;

	QDialog dialog(this);
	dialog.setWindowTitle(tr("Edit Profile: %1").arg(name));

	auto* form = new QFormLayout(&dialog);

	auto* host_edit = new QLineEdit(settings.value(prefix + QStringLiteral("host")).toString(), &dialog);
	form->addRow(tr("Host:"), host_edit);

	auto* port_spin = new QSpinBox(&dialog);
	port_spin->setRange(1, 65535);
	port_spin->setValue(settings.value(prefix + QStringLiteral("port"), 2101).toInt());
	form->addRow(tr("Port:"), port_spin);

	auto* mountpoint_edit = new QLineEdit(settings.value(prefix + QStringLiteral("mountpoint")).toString(), &dialog);
	form->addRow(tr("Mountpoint:"), mountpoint_edit);

	auto* username_edit = new QLineEdit(settings.value(prefix + QStringLiteral("username")).toString(), &dialog);
	form->addRow(tr("Username:"), username_edit);

	auto* password_edit = new QLineEdit(settings.value(prefix + QStringLiteral("password")).toString(), &dialog);
	password_edit->setEchoMode(QLineEdit::Password);
	form->addRow(tr("Password:"), password_edit);

	auto* tls_box = new QCheckBox(tr("Use TLS"), &dialog);
	tls_box->setChecked(settings.value(prefix + QStringLiteral("tls"), false).toBool());
	form->addRow(tls_box);

	auto* send_gga_box = new QCheckBox(tr("Send GGA"), &dialog);
	send_gga_box->setChecked(settings.value(prefix + QStringLiteral("send_gga"), false).toBool());
	form->addRow(send_gga_box);

	auto* gga_interval_spin = new QSpinBox(&dialog);
	gga_interval_spin->setRange(1, 60);
	gga_interval_spin->setValue(settings.value(prefix + QStringLiteral("gga_interval"), 10).toInt());
	gga_interval_spin->setSuffix(tr(" s"));
	form->addRow(tr("GGA interval:"), gga_interval_spin);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	form->addRow(buttons);
	connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	if (dialog.exec() != QDialog::Accepted)
		return;

	settings.setValue(prefix + QStringLiteral("host"), host_edit->text());
	settings.setValue(prefix + QStringLiteral("port"), port_spin->value());
	settings.setValue(prefix + QStringLiteral("mountpoint"), mountpoint_edit->text());
	settings.setValue(prefix + QStringLiteral("username"), username_edit->text());
	settings.setValue(prefix + QStringLiteral("password"), password_edit->text());
	settings.setValue(prefix + QStringLiteral("tls"), tls_box->isChecked());
	settings.setValue(prefix + QStringLiteral("send_gga"), send_gga_box->isChecked());
	settings.setValue(prefix + QStringLiteral("gga_interval"), gga_interval_spin->value());

	emit profilesChanged();
}


void NtripSettingsWidget::removeProfile()
{
	auto row = profile_list->currentRow();
	if (row < 0)
		return;

	auto name = profile_list->item(row)->text();
	auto result = QMessageBox::question(this, tr("Remove Profile"),
	                                    tr("Remove profile \"%1\"?").arg(name),
	                                    QMessageBox::Yes | QMessageBox::No);
	if (result != QMessageBox::Yes)
		return;

	QSettings settings;
	settings.remove(QStringLiteral("Gnss/ntrip_profile/%1").arg(name));

	delete profile_list->takeItem(row);
	saveProfiles();
	emit profilesChanged();
}


void NtripSettingsWidget::testConnection()
{
	auto row = profile_list->currentRow();
	if (row < 0)
	{
		test_status_label->setText(tr("Select a profile first."));
		return;
	}

	auto name = profile_list->item(row)->text();
	auto prefix = QStringLiteral("Gnss/ntrip_profile/%1/").arg(name);
	QSettings settings;

	auto host = settings.value(prefix + QStringLiteral("host")).toString();
	auto port = static_cast<quint16>(settings.value(prefix + QStringLiteral("port"), 2101).toInt());
	auto mountpoint = settings.value(prefix + QStringLiteral("mountpoint")).toString();
	auto username = settings.value(prefix + QStringLiteral("username")).toString();
	auto password = settings.value(prefix + QStringLiteral("password")).toString();

	if (host.isEmpty() || mountpoint.isEmpty())
	{
		test_status_label->setText(tr("Profile incomplete — edit host and mountpoint first."));
		return;
	}

	// Clean up previous test
	if (test_socket)
	{
		test_socket->disconnect(this);
		test_socket->abort();
		test_socket->deleteLater();
		test_socket = nullptr;
	}

	test_status_label->setText(tr("Connecting to %1:%2...").arg(host).arg(port));
	test_button->setEnabled(false);

	test_socket = new QTcpSocket(this);

	// Build NTRIP v2 request (matches what the live client sends in Auto mode)
	QByteArray request;
	request.append("GET /");
	request.append(mountpoint.toLatin1());
	request.append(" HTTP/1.1\r\n");
	request.append("Host: ");
	request.append(host.toLatin1());
	request.append("\r\n");
	request.append("Ntrip-Version: Ntrip/2.0\r\n");
	request.append("User-Agent: NTRIP OpenOrienteeringMapper/1.0\r\n");
	if (!username.isEmpty())
	{
		QByteArray cred = username.toLatin1() + ':' + password.toLatin1();
		request.append("Authorization: Basic ");
		request.append(cred.toBase64());
		request.append("\r\n");
	}
	request.append("\r\n");

	// On connect → send request
	connect(test_socket, &QTcpSocket::connected, this, [this, request]() {
		test_socket->write(request);
		test_status_label->setText(tr("Connected. Waiting for response..."));
	});

	// On data → check response
	connect(test_socket, &QTcpSocket::readyRead, this, [this]() {
		auto data = test_socket->readAll();
		auto response = QString::fromLatin1(data.left(200));

		bool isSourcetable = response.startsWith(QLatin1String("SOURCETABLE 200 OK"));
		bool ok = !isSourcetable
		    && (response.startsWith(QLatin1String("ICY 200 OK"))
		        || response.startsWith(QLatin1String("HTTP/1.0 200"))
		        || response.startsWith(QLatin1String("HTTP/1.1 200")));

		if (ok)
		{
			// Extract version and server info from headers
			QString version = response.contains(QLatin1String("Ntrip/2.0"))
			    ? QStringLiteral("v2") : QStringLiteral("v1");
			if (response.contains(QLatin1String("chunked")))
				version += QLatin1String(" chunked");
			QString server;
			for (const auto& line : response.split(QLatin1String("\r\n")))
			{
				if (line.startsWith(QLatin1String("Server:"), Qt::CaseInsensitive))
				{
					server = line.mid(7).trimmed();
					break;
				}
			}
			test_status_label->setText(
			    tr("Success! NTRIP %1\nServer: %2").arg(version, server.isEmpty() ? tr("(unknown)") : server));
		}
		else if (isSourcetable)
		{
			test_status_label->setText(tr("Mountpoint not found — caster returned sourcetable. Check spelling."));
		}
		else
		{
			test_status_label->setText(tr("Rejected: %1").arg(response.left(80)));
		}

		test_socket->disconnect(this);
		test_socket->abort();
		test_socket->deleteLater();
		test_socket = nullptr;
		test_button->setEnabled(true);
	});

	// On error
	connect(test_socket, &QTcpSocket::errorOccurred, this,
	        [this](QAbstractSocket::SocketError) {
		test_status_label->setText(
		    tr("Connection failed: %1").arg(test_socket->errorString()));
		test_socket->disconnect(this);
		test_socket->deleteLater();
		test_socket = nullptr;
		test_button->setEnabled(true);
	});

	// Timeout
	QTimer::singleShot(10000, this, [this]() {
		if (!test_socket)
			return;
		test_status_label->setText(tr("Connection timed out (10s)."));
		test_socket->disconnect(this);
		test_socket->abort();
		test_socket->deleteLater();
		test_socket = nullptr;
		test_button->setEnabled(true);
	});

	test_socket->connectToHost(host, port);
}


}  // namespace OpenOrienteering
