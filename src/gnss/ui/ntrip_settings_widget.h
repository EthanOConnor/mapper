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


#ifndef OPENORIENTEERING_NTRIP_SETTINGS_WIDGET_H
#define OPENORIENTEERING_NTRIP_SETTINGS_WIDGET_H

#include <QObject>
#include <QString>
#include <QWidget>

class QLabel;
class QListWidget;
class QPushButton;
class QTcpSocket;


namespace OpenOrienteering {


class NtripSettingsWidget : public QWidget
{
	Q_OBJECT
public:
	explicit NtripSettingsWidget(QWidget* parent = nullptr);
	~NtripSettingsWidget() override;

	/// Name of the currently selected (active) profile, or empty string.
	QString selectedProfileName() const;

	/// Select the profile with the given name in the list.
	void selectProfile(const QString& name);

signals:
	void profilesChanged();

private slots:
	void addProfile();
	void editProfile(int index);
	void removeProfile();
	void testConnection();

private:
	void setupUi();
	void loadProfiles();
	void saveProfiles();

	QListWidget* profile_list;
	QPushButton* add_button;
	QPushButton* edit_button;
	QPushButton* remove_button;
	QPushButton* test_button;
	QLabel* test_status_label;
	QTcpSocket* test_socket = nullptr;
};


}  // namespace OpenOrienteering

#endif
