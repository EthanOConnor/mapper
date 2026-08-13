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

#include "gnss/correction/ntrip_profile.h"
#include "gnss/correction/ntrip_profile_store.h"
#include "settings.h"

#include <QCheckBox>
#include <QComboBox>
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
#include <QSslError>
#include <QSslSocket>
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
	test_button = new QPushButton(tr("Test caster login"), this);

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
	for (const auto& name : NtripProfileStore::profileNames())
		profile_list->addItem(name);
}


void NtripSettingsWidget::saveProfiles()
{
	// The profile store updates ordering atomically with each saved profile.
}


void NtripSettingsWidget::addProfile()
{
	bool ok = false;
	auto name = QInputDialog::getText(this, tr("Add NTRIP Profile"),
	                                  tr("Profile name:"),
	                                  QLineEdit::Normal, {}, &ok);
	name = name.trimmed();
	if (!ok || name.isEmpty())
		return;
	if (name.contains(QLatin1Char('/')))
	{
		QMessageBox::warning(this, tr("Invalid profile name"),
		                     tr("Profile names cannot contain “/”."));
		return;
	}
	if (NtripProfileStore::profileNames().contains(name))
	{
		QMessageBox::warning(this, tr("Profile already exists"),
		                     tr("Choose a unique profile name."));
		return;
	}

	profile_list->addItem(name);
	int row = profile_list->count() - 1;
	profile_list->setCurrentRow(row);

	// Immediately open edit dialog so the user can fill in caster details
	editProfile(row);
	if (row < profile_list->count()
	    && !NtripProfileStore::profileNames().contains(
	      profile_list->item(row)->text()))
		delete profile_list->takeItem(row);

	emit profilesChanged();
}


void NtripSettingsWidget::editProfile(int index)
{
	if (index < 0 || index >= profile_list->count())
		return;

	auto name = profile_list->item(index)->text();
	QString load_error;
	std::optional<NtripProfile> saved_profile;
	if (NtripProfileStore::profileNames().contains(name))
		saved_profile = NtripProfileStore::load(name, &load_error);
	if (!load_error.isEmpty())
	{
		QMessageBox::warning(this, tr("NTRIP profile unavailable"), load_error);
		return;
	}
	NtripProfile original;
	original.name = name;
	if (saved_profile)
		original = *saved_profile;

	QDialog dialog(this);
	dialog.setWindowTitle(tr("Edit Profile: %1").arg(name));

	auto* form = new QFormLayout(&dialog);

	auto* name_edit = new QLineEdit(original.name, &dialog);
	name_edit->setClearButtonEnabled(true);
	form->addRow(tr("Profile name:"), name_edit);

	auto* host_edit = new QLineEdit(original.casterHost, &dialog);
	host_edit->setPlaceholderText(tr("caster.example.org"));
	form->addRow(tr("Host:"), host_edit);

	auto* port_spin = new QSpinBox(&dialog);
	port_spin->setRange(1, 65535);
	port_spin->setValue(original.casterPort);
	form->addRow(tr("Port:"), port_spin);

	auto* mountpoint_edit = new QLineEdit(original.mountpoint, &dialog);
	form->addRow(tr("Mountpoint:"), mountpoint_edit);

	auto* username_edit = new QLineEdit(original.username, &dialog);
	form->addRow(tr("Username:"), username_edit);

	auto* password_edit = new QLineEdit(original.password, &dialog);
	password_edit->setEchoMode(QLineEdit::Password);
	password_edit->setClearButtonEnabled(true);
	form->addRow(tr("Password:"), password_edit);

	auto* empty_basic_auth_box = new QCheckBox(tr("Send empty Basic auth when credentials are blank"), &dialog);
	empty_basic_auth_box->setChecked(original.sendEmptyBasicAuth);
	form->addRow(empty_basic_auth_box);

	auto* tls_box = new QCheckBox(tr("Use TLS"), &dialog);
	tls_box->setChecked(original.useTls);
	form->addRow(tls_box);

	auto* version_box = new QComboBox(&dialog);
	version_box->addItem(tr("Automatic (NTRIP 2, then 1)"),
	                     static_cast<int>(NtripVersion::Auto));
	version_box->addItem(tr("NTRIP 2"),
	                     static_cast<int>(NtripVersion::V2));
	version_box->addItem(tr("NTRIP 1"),
	                     static_cast<int>(NtripVersion::V1));
	auto version_index = version_box->findData(
	  static_cast<int>(original.version));
	version_box->setCurrentIndex(version_index < 0 ? 0 : version_index);
	form->addRow(tr("Protocol:"), version_box);

	auto* send_gga_box = new QCheckBox(tr("Send GGA"), &dialog);
	send_gga_box->setChecked(original.sendGga);
	form->addRow(send_gga_box);

	auto* gga_interval_spin = new QSpinBox(&dialog);
	gga_interval_spin->setRange(1, 60);
	gga_interval_spin->setValue(original.ggaIntervalSec);
	gga_interval_spin->setSuffix(tr(" s"));
	form->addRow(tr("GGA interval:"), gga_interval_spin);

	auto* buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	form->addRow(buttons);
	connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);

	auto was_saved = saved_profile.has_value();
	auto profile_saved = false;
	connect(buttons, &QDialogButtonBox::accepted, &dialog, [&]() {
		NtripProfile profile;
		profile.name = name_edit->text();
		profile.casterHost = host_edit->text();
		profile.casterPort = static_cast<quint16>(port_spin->value());
		profile.mountpoint = mountpoint_edit->text();
		profile.username = username_edit->text();
		profile.password = password_edit->text();
		profile.sendEmptyBasicAuth = empty_basic_auth_box->isChecked();
		profile.useTls = tls_box->isChecked();
		profile.version = static_cast<NtripVersion>(
		  version_box->currentData().toInt());
		profile.sendGga = send_gga_box->isChecked();
		profile.ggaIntervalSec = gga_interval_spin->value();
		profile = ntripProfileNormalized(profile);

		QString save_error;
		auto saved = was_saved
		  ? NtripProfileStore::rename(name, profile, &save_error)
		  : NtripProfileStore::save(profile, &save_error);
		if (!saved)
		{
			QMessageBox::warning(&dialog, tr("NTRIP profile not saved"),
			                     save_error);
			return;
		}

		profile_list->item(index)->setText(profile.name);
		auto& settings = Settings::getInstance();
		if (was_saved && settings.gnssNtripActiveProfile() == name)
			settings.setGnssNtripActiveProfile(profile.name);
		profile_saved = true;
		dialog.accept();
	});

	dialog.exec();
	if (profile_saved)
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

	QString remove_error;
	if (!NtripProfileStore::remove(name, &remove_error))
	{
		QMessageBox::warning(this, tr("NTRIP profile not removed"),
		                     remove_error);
		return;
	}

	delete profile_list->takeItem(row);
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
	QString load_error;
	auto loaded = NtripProfileStore::load(name, &load_error);
	if (!loaded)
	{
		test_status_label->setText(load_error);
		return;
	}
	auto profile = *loaded;

	if (profile.casterHost.isEmpty() || profile.mountpoint.isEmpty())
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

	test_status_label->setText(tr("Connecting to %1:%2...").arg(profile.casterHost).arg(profile.casterPort));
	test_button->setEnabled(false);

	test_socket = profile.useTls
	    ? static_cast<QTcpSocket*>(new QSslSocket(this))
	    : new QTcpSocket(this);

	// Build NTRIP v2 request (matches what the live client sends in Auto mode)
	QByteArray request;
	request.append("GET /");
	request.append(profile.mountpoint.toLatin1());
	request.append(" HTTP/1.1\r\n");
	request.append("Host: ");
	request.append(profile.casterHost.toLatin1());
	request.append(':');
	request.append(QByteArray::number(profile.casterPort));
	request.append("\r\n");
	request.append("Ntrip-Version: Ntrip/2.0\r\n");
	request.append("User-Agent: NTRIP OpenOrienteeringMapper/1.0\r\n");
	if (ntripProfileShouldSendAuthorization(profile))
	{
		request.append("Authorization: Basic ");
		request.append(ntripProfileBasicAuthorizationValue(profile));
		request.append("\r\n");
	}
	request.append("\r\n");

	auto send_request = [this, request]() {
		test_socket->write(request);
		test_status_label->setText(tr("Connected. Waiting for response..."));
	};
	if (auto* ssl_socket = qobject_cast<QSslSocket*>(test_socket))
	{
		connect(ssl_socket, &QSslSocket::connected, this, [this]() {
			test_status_label->setText(tr("Connected. Securing TLS..."));
		});
		connect(ssl_socket, &QSslSocket::encrypted, this, send_request);
		connect(ssl_socket, &QSslSocket::sslErrors, this,
		        [this](const QList<QSslError>& errors) {
			if (!errors.isEmpty())
				test_status_label->setText(
				  tr("TLS verification failed: %1")
				    .arg(errors.constFirst().errorString()));
		});
	}
	else
	{
		connect(test_socket, &QTcpSocket::connected, this, send_request);
	}

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

	if (auto* ssl_socket = qobject_cast<QSslSocket*>(test_socket))
		ssl_socket->connectToHostEncrypted(
		  profile.casterHost, profile.casterPort);
	else
		test_socket->connectToHost(profile.casterHost, profile.casterPort);
}


}  // namespace OpenOrienteering
