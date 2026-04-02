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


#include "gnss_detail_panel.h"

#include <cmath>

#include <QComboBox>
#include <QFormLayout>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QScreen>
#include <QGuiApplication>
#include <QVBoxLayout>

#include "gnss/gnss_state.h"
#include "gui/util_gui.h"


namespace OpenOrienteering {

namespace {

/// Format a float value with the given precision, or "--" if NaN.
QString formatFloat(float value, char format, int precision)
{
	if (std::isnan(value))
		return QStringLiteral("--");
	return QString::number(static_cast<double>(value), format, precision);
}

/// Format an accuracy value as "0.03m (P95)" or "--".
QString formatAccuracy(float p95)
{
	if (std::isnan(p95))
		return QStringLiteral("--");
	return QString::number(static_cast<double>(p95), 'f', 2) + QLatin1String("m (P95)");
}

/// Format altitude as "123.4m (MSL)" or "-- (MSL)".
QString formatAltitude(double msl)
{
	if (std::isnan(msl))
		return QStringLiteral("-- (MSL)");
	return QString::number(msl, 'f', 1) + QLatin1String("m (MSL)");
}

/// Format a DOP value with 1 decimal, or "--" if NaN.
QString formatDop(float value)
{
	return formatFloat(value, 'f', 1);
}

/// Format data rate: convert to KB/s when >= 1024 bytes/sec.
QString formatDataRate(float bytesPerSec)
{
	if (std::isnan(bytesPerSec) || bytesPerSec <= 0.0f)
		return QStringLiteral("--");
	if (bytesPerSec >= 1024.0f)
		return QString::number(static_cast<double>(bytesPerSec / 1024.0f), 'f', 1) + QLatin1String(" KB/s");
	return QString::number(static_cast<double>(bytesPerSec), 'f', 0) + QLatin1String(" B/s");
}

/// Convert GnssFixType to a human-readable string.
QString fixTypeString(GnssFixType type)
{
	switch (type)
	{
	case GnssFixType::RtkFixed: return QStringLiteral("RTK Fixed");
	case GnssFixType::RtkFloat: return QStringLiteral("RTK Float");
	case GnssFixType::DGPS:     return QStringLiteral("DGPS");
	case GnssFixType::Fix3D:    return QStringLiteral("3D Fix");
	case GnssFixType::Fix2D:    return QStringLiteral("2D Fix");
	case GnssFixType::NoFix:    return QStringLiteral("No Fix");
	}
	return QStringLiteral("No Fix");
}

/// Convert GnssCorrectionState to a human-readable string.
QString correctionStateString(GnssCorrectionState state)
{
	switch (state)
	{
	case GnssCorrectionState::Flowing:      return QStringLiteral("Flowing");
	case GnssCorrectionState::Connected:    return QStringLiteral("Connected");
	case GnssCorrectionState::Stale:        return QStringLiteral("Stale");
	case GnssCorrectionState::Reconnecting: return QStringLiteral("Reconnecting");
	case GnssCorrectionState::Connecting:   return QStringLiteral("Connecting");
	case GnssCorrectionState::Disconnected: return QStringLiteral("Disconnected");
	case GnssCorrectionState::Disabled:     return QStringLiteral("Disabled");
	}
	return QStringLiteral("Disabled");
}

/// Short name for a GNSS constellation.
const char* constellationShortName(int index)
{
	switch (static_cast<GnssConstellation>(index))
	{
	case GnssConstellation::GPS:     return "GPS";
	case GnssConstellation::SBAS:    return "SBAS";
	case GnssConstellation::Galileo: return "GAL";
	case GnssConstellation::BeiDou:  return "BDS";
	case GnssConstellation::IMES:    return "IMES";
	case GnssConstellation::QZSS:    return "QZSS";
	case GnssConstellation::GLONASS: return "GLO";
	case GnssConstellation::NavIC:   return "NavIC";
	}
	return "?";
}

}  // anonymous namespace


GnssDetailPanel::GnssDetailPanel(QWidget* parent)
    : QWidget(parent)
{
	setupUi();
}


GnssDetailPanel::~GnssDetailPanel() = default;


QSize GnssDetailPanel::sizeHint() const
{
	auto* screen = QGuiApplication::primaryScreen();
	if (screen)
	{
		auto geom = screen->availableGeometry();
		return { geom.width(), static_cast<int>(geom.height() * 0.4) };
	}
	return { 360, 480 };
}


void GnssDetailPanel::setupUi()
{
	setAutoFillBackground(true);
	auto pal = palette();
	pal.setColor(QPalette::Window, Qt::white);
	setPalette(pal);

	// Content widget inside scroll area
	auto* content_widget = new QWidget();
	auto* form = new QFormLayout(content_widget);
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);

	// --- Position section ---
	form->addRow(Util::Headline::create(tr("Position")));

	fix_type_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Fix type:"), fix_type_label);

	h_accuracy_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("H accuracy:"), h_accuracy_label);

	v_accuracy_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("V accuracy:"), v_accuracy_label);

	altitude_label = new QLabel(QStringLiteral("-- (MSL)"));
	form->addRow(tr("Altitude:"), altitude_label);

	coordinates_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Coordinates:"), coordinates_label);

	// --- Satellites section ---
	form->addRow(Util::Headline::create(tr("Satellites")));

	satellites_label = new QLabel(QStringLiteral("-- / --"));
	form->addRow(tr("Used / visible:"), satellites_label);

	constellation_label = new QLabel(QStringLiteral("--"));
	constellation_label->setWordWrap(true);
	form->addRow(tr("Constellations:"), constellation_label);

	// --- Quality section ---
	form->addRow(Util::Headline::create(tr("Quality")));

	pdop_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("PDOP:"), pdop_label);

	hdop_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("HDOP:"), hdop_label);

	vdop_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("VDOP:"), vdop_label);

	// --- Corrections section ---
	form->addRow(Util::Headline::create(tr("Corrections")));

	ntrip_profile_combo = new QComboBox();
	form->addRow(tr("NTRIP profile:"), ntrip_profile_combo);
	connect(ntrip_profile_combo, &QComboBox::currentTextChanged,
	        this, &GnssDetailPanel::ntripProfileChangeRequested);

	correction_status_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Status:"), correction_status_label);

	correction_age_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Correction age:"), correction_age_label);

	correction_rate_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Data rate:"), correction_rate_label);

	mountpoint_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Mountpoint:"), mountpoint_label);

	// --- Receiver section ---
	form->addRow(Util::Headline::create(tr("Receiver")));

	device_name_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Device:"), device_name_label);

	connection_type_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Connection:"), connection_type_label);

	connect_button = new QPushButton(tr("Connect"));
	form->addRow(connect_button);
	connect(connect_button, &QPushButton::clicked, this, [this]() {
		if (connect_button->text() == tr("Disconnect"))
			emit disconnectRequested();
		else
			emit connectRequested();
	});

	// Scroll area wrapping the content
	auto* scroll_area = new QScrollArea();
	scroll_area->setWidget(content_widget);
	scroll_area->setWidgetResizable(true);
	scroll_area->setFrameShape(QFrame::NoFrame);

	auto* main_layout = new QVBoxLayout(this);
	main_layout->setContentsMargins(0, 0, 0, 0);
	main_layout->addWidget(scroll_area);
}


void GnssDetailPanel::updateState(const GnssState& state)
{
	const auto& pos = state.position;

	// Position section
	fix_type_label->setText(fixTypeString(pos.fixType));
	h_accuracy_label->setText(formatAccuracy(pos.hAccuracyP95));
	v_accuracy_label->setText(formatAccuracy(pos.vAccuracyP95));
	altitude_label->setText(formatAltitude(pos.altitudeMsl));

	if (pos.valid)
	{
		coordinates_label->setText(
		    QString::number(pos.latitude, 'f', 8)
		    + QLatin1String(", ")
		    + QString::number(pos.longitude, 'f', 8));
	}
	else
	{
		coordinates_label->setText(QStringLiteral("--"));
	}

	// Satellites section
	satellites_label->setText(
	    QString::number(pos.satellitesUsed)
	    + QLatin1String(" / ")
	    + QString::number(pos.satellitesVisible));

	QStringList parts;
	for (int i = 0; i < GnssState::kMaxConstellations; ++i)
	{
		const auto& info = state.constellations[i];
		if (info.visible > 0 || info.used > 0)
		{
			parts.append(
			    QLatin1String(constellationShortName(i))
			    + QLatin1Char(' ')
			    + QString::number(info.used)
			    + QLatin1Char('/')
			    + QString::number(info.visible));
		}
	}
	constellation_label->setText(parts.isEmpty() ? QStringLiteral("--") : parts.join(QLatin1String(" | ")));

	// Quality section
	pdop_label->setText(formatDop(pos.pDOP));
	hdop_label->setText(formatDop(pos.hDOP));
	vdop_label->setText(formatDop(pos.vDOP));

	// Corrections section
	correction_status_label->setText(correctionStateString(state.correctionState));
	correction_age_label->setText(
	    std::isnan(pos.correctionAge)
	        ? QStringLiteral("--")
	        : QString::number(static_cast<double>(pos.correctionAge), 'f', 1) + QLatin1String("s"));
	correction_rate_label->setText(formatDataRate(state.correctionDataRate));
	mountpoint_label->setText(state.ntripMountpoint.isEmpty() ? QStringLiteral("--") : state.ntripMountpoint);

	// Receiver section
	device_name_label->setText(state.deviceName.isEmpty() ? QStringLiteral("--") : state.deviceName);
	connection_type_label->setText(state.transportType.isEmpty() ? QStringLiteral("--") : state.transportType);

	bool connected = (state.transportState == GnssTransportState::Connected
	                  || state.transportState == GnssTransportState::Reconnecting);
	connect_button->setText(connected ? tr("Disconnect") : tr("Connect"));
}


}  // namespace OpenOrienteering
