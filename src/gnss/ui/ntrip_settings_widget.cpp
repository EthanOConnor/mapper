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
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QString>
#include <QStringList>
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

	connect(add_button, &QPushButton::clicked, this, &NtripSettingsWidget::addProfile);
	connect(edit_button, &QPushButton::clicked, this, [this]() {
		auto row = profile_list->currentRow();
		if (row >= 0)
			editProfile(row);
	});
	connect(remove_button, &QPushButton::clicked, this, &NtripSettingsWidget::removeProfile);
	connect(test_button, &QPushButton::clicked, this, &NtripSettingsWidget::testConnection);
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
	saveProfiles();
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
	QMessageBox::information(this, tr("Test Connection"),
	                         tr("Connection test not yet implemented."));
}


}  // namespace OpenOrienteering
