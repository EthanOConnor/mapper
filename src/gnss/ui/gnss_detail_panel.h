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


#ifndef OPENORIENTEERING_GNSS_DETAIL_PANEL_H
#define OPENORIENTEERING_GNSS_DETAIL_PANEL_H

#include <QWidget>

class QComboBox;
class QEvent;
class QLabel;
class QPushButton;

namespace OpenOrienteering {

struct GnssState;


/**
 * A slide-up detail panel showing full GNSS information.
 *
 * Covers the bottom ~40% of the screen when shown. Displays position,
 * satellite, quality, correction, and receiver information from GnssState.
 */
class GnssDetailPanel : public QWidget
{
	Q_OBJECT
public:
	explicit GnssDetailPanel(QWidget* parent = nullptr);
	~GnssDetailPanel() override;

	void updateState(const GnssState& state);
	void setDumpStatus(const QString& message);

signals:
	void ntripProfileChangeRequested(const QString& profileName);
	void disconnectRequested();
	void connectRequested();
	void dumpRawRequested();

protected:
	QSize sizeHint() const override;
	bool event(QEvent* e) override;

private:
	void setupUi();

	// Position section
	QLabel* fix_time_label;
	QLabel* fix_type_label;
	QLabel* h_accuracy_label;
	QLabel* v_accuracy_label;
	QLabel* altitude_label;
	QLabel* coordinates_label;
	QLabel* source_label;
	QLabel* limitation_label;

	// Satellites section
	QLabel* satellites_label;
	QLabel* constellation_label;

	// Quality section
	QLabel* pdop_label;
	QLabel* hdop_label;
	QLabel* vdop_label;

	// Corrections section
	QComboBox* ntrip_profile_combo;
	QLabel* correction_status_label;
	QLabel* ntrip_version_label;
	QLabel* ntrip_server_label;
	QLabel* local_age_label;
	QLabel* correction_age_label;
	QLabel* correction_rate_label;
	QLabel* ntrip_bytes_label;
	QLabel* gga_count_label;
	QLabel* mountpoint_label;

	// Messages section
	QLabel* messages_label;
	QPushButton* dump_button;
	QLabel* dump_status_label;

	// Receiver section
	QLabel* device_name_label;
	QLabel* connection_type_label;
	QPushButton* connect_button;
};


}  // namespace OpenOrienteering

#endif
