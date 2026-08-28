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

#include <algorithm>
#include <cmath>

#include <QComboBox>
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFormLayout>
#include <QFrame>
#include <QLabel>
#include <QPalette>
#include <QPushButton>
#include <QScrollArea>
#include <QScroller>
#include <QScreen>
#include <QSettings>
#include <QSpacerItem>
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

QString formatFieldSource(const GnssFieldSource& field)
{
	if (!field.available)
		return QStringLiteral("--");

	auto text = gnssObservationSourceName(field.source);
	if (!std::isnan(field.nominalResolutionM))
	{
		text += QStringLiteral("  [res >= %1m]")
		        .arg(static_cast<double>(field.nominalResolutionM), 0, 'f', field.nominalResolutionM < 1.0f ? 3 : 1);
	}
	if (field.derived)
		text += QStringLiteral("  [derived]");
	if (!field.timestampComplete)
		text += QStringLiteral("  [partial time]");
	return text;
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

void GnssDetailPanel::setDumpStatus(const QString& message)
{
	dump_status_label->setText(message);
}


void GnssDetailPanel::setRawCaptureActive(bool active)
{
	dump_button->setText(active ? tr("Dump Raw Capture") : tr("Start Raw Capture"));
}


bool GnssDetailPanel::event(QEvent* e)
{
	// On mobile, this panel floats over the map widget. We must accept
	// touch events to prevent them from falling through to the map.
	// QScroller on the scroll area viewport handles the actual scrolling
	// via gesture recognition, which runs before our event handler.
	if (e->type() == QEvent::TouchBegin)
	{
		e->accept();
		// Don't return true — let QWidget::event deliver to children
	}
	return QWidget::event(e);
}


QSize GnssDetailPanel::sizeHint() const
{
	auto* screen = QGuiApplication::primaryScreen();
	if (screen)
	{
		auto geom = screen->availableGeometry();
		return { geom.width(), static_cast<int>(geom.height() * 0.78) };
	}
	return { 360, 480 };
}


void GnssDetailPanel::setupUi()
{
	setAutoFillBackground(true);

	// Accept all touch/mouse events so they don't pass through to the map
	setAttribute(Qt::WA_AcceptTouchEvents);
	setFocusPolicy(Qt::StrongFocus);

	// Content widget inside scroll area
	auto* content_widget = new QWidget();
	auto* form = new QFormLayout(content_widget);
	form->setFieldGrowthPolicy(QFormLayout::ExpandingFieldsGrow);
	form->setRowWrapPolicy(QFormLayout::WrapLongRows);
	form->setHorizontalSpacing(qRound(Util::mmToPixelLogical(3.0)));
	form->setVerticalSpacing(qRound(Util::mmToPixelLogical(1.7)));
	form->setContentsMargins(qRound(Util::mmToPixelLogical(4.0)),
	                         qRound(Util::mmToPixelLogical(3.0)),
	                         qRound(Util::mmToPixelLogical(4.0)),
	                         qRound(Util::mmToPixelLogical(6.0)));

	// --- At-a-glance health ---
	auto* summary_card = new QFrame();
	summary_card->setObjectName(QStringLiteral("gnssSummaryCard"));
	summary_card->setStyleSheet(QStringLiteral(
	  "QFrame#gnssSummaryCard { background: #16202b; border-radius: 12px; }"
	  "QFrame#gnssSummaryCard QLabel { color: white; background: transparent; }"));
	auto* summary_layout = new QVBoxLayout(summary_card);
	summary_layout->setContentsMargins(qRound(Util::mmToPixelLogical(4.0)),
	                                  qRound(Util::mmToPixelLogical(3.0)),
	                                  qRound(Util::mmToPixelLogical(4.0)),
	                                  qRound(Util::mmToPixelLogical(3.0)));
	summary_layout->setSpacing(qRound(Util::mmToPixelLogical(1.2)));
	status_summary_label = new QLabel(tr("GNSS is not connected"));
	status_summary_label->setObjectName(QStringLiteral("gnssStatusSummary"));
	auto summary_font = status_summary_label->font();
	summary_font.setBold(true);
	summary_font.setPointSizeF(std::max(14.0, summary_font.pointSizeF() + 3.0));
	status_summary_label->setFont(summary_font);
	status_summary_label->setWordWrap(true);
	status_detail_label = new QLabel(tr("Connect a receiver to use live position and track recording."));
	status_detail_label->setObjectName(QStringLiteral("gnssStatusDetail"));
	status_detail_label->setWordWrap(true);
	auto detail_font = status_detail_label->font();
	detail_font.setPointSizeF(std::max(11.0, detail_font.pointSizeF() + 1.0));
	status_detail_label->setFont(detail_font);
	receiver_health_label = new QLabel();
	correction_health_label = new QLabel();
	receiver_health_label->setObjectName(QStringLiteral("gnssReceiverHealth"));
	correction_health_label->setObjectName(QStringLiteral("gnssCorrectionHealth"));
	receiver_health_label->setWordWrap(true);
	correction_health_label->setWordWrap(true);
	summary_layout->addWidget(status_summary_label);
	summary_layout->addWidget(status_detail_label);
	summary_layout->addSpacing(qRound(Util::mmToPixelLogical(1.0)));
	summary_layout->addWidget(receiver_health_label);
	summary_layout->addWidget(correction_health_label);
	form->addRow(summary_card);

	auto* action_row = new QWidget();
	auto* action_layout = new QHBoxLayout(action_row);
	action_layout->setContentsMargins(0, 0, 0, 0);
	action_layout->setSpacing(qRound(Util::mmToPixelLogical(2.0)));
	change_receiver_button = new QPushButton(tr("Change receiver"));
	settings_button = new QPushButton(tr("GNSS settings"));
	connect_button = new QPushButton(tr("Connect"));
	change_receiver_button->setObjectName(QStringLiteral("gnssChangeReceiver"));
	settings_button->setObjectName(QStringLiteral("gnssSettings"));
	connect_button->setObjectName(QStringLiteral("gnssConnect"));
	for (auto* button : {change_receiver_button, settings_button, connect_button})
		button->setMinimumHeight(qRound(Util::mmToPixelLogical(10.0)));
	action_layout->addWidget(change_receiver_button);
	action_layout->addWidget(settings_button);
	action_layout->addWidget(connect_button);
	form->addRow(action_row);
	connect(change_receiver_button, &QPushButton::clicked,
	        this, &GnssDetailPanel::receiverChangeRequested);
	connect(settings_button, &QPushButton::clicked,
	        this, &GnssDetailPanel::settingsRequested);
	connect(connect_button, &QPushButton::clicked, this, [this]() {
		if (connect_button->text() == tr("Disconnect"))
			emit disconnectRequested();
		else
			emit connectRequested();
	});

	// --- Position section ---
	form->addItem(Util::SpacerItem::create(this));
	form->addRow(Util::Headline::create(tr("Position details")));

	fix_time_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Fix time:"), fix_time_label);

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

	source_label = new QLabel(QStringLiteral("--"));
	source_label->setWordWrap(true);
	form->addRow(tr("Primary source:"), source_label);

	limitation_label = new QLabel(QStringLiteral("--"));
	limitation_label->setWordWrap(true);
	form->addRow(tr("Limitations:"), limitation_label);

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

	ntrip_version_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("NTRIP version:"), ntrip_version_label);

	ntrip_server_label = new QLabel(QStringLiteral("--"));
	ntrip_server_label->setWordWrap(true);
	form->addRow(tr("Server:"), ntrip_server_label);

	local_age_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Local age:"), local_age_label);

	correction_age_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Receiver age:"), correction_age_label);

	correction_rate_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Data rate:"), correction_rate_label);

	ntrip_bytes_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Caster / queued / dropped:"), ntrip_bytes_label);

	gga_count_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("GGA sent:"), gga_count_label);

	mountpoint_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Mountpoint:"), mountpoint_label);

	// --- Receiver section ---
	form->addRow(Util::Headline::create(tr("Receiver")));

	device_name_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Device:"), device_name_label);

	connection_type_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Connection:"), connection_type_label);

	receiver_data_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Receiver traffic:"), receiver_data_label);
	receiver_protocol_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Position protocol:"), receiver_protocol_label);
	receiver_last_data_label = new QLabel(QStringLiteral("--"));
	form->addRow(tr("Last receiver data:"), receiver_last_data_label);

	// --- GEO-PULSE section ---
	// The GEO-PULSE reports facts no generic receiver does — the rate it
	// actually accepted, which link it expects corrections on, and whether its
	// dead-reckoning is calibrated. The rows stay hidden for other receivers.
	{
		auto* hyfix_form = new QFormLayout();
		hyfix_form->setContentsMargins(0, 0, 0, 0);
		hyfix_form->addRow(Util::Headline::create(tr("HYFIX GEO-PULSE")));

		hyfix_firmware_label = new QLabel(QStringLiteral("--"));
		hyfix_form->addRow(tr("Firmware:"), hyfix_firmware_label);
		hyfix_rate_label = new QLabel(QStringLiteral("--"));
		hyfix_form->addRow(tr("Position rate:"), hyfix_rate_label);
		hyfix_mode_label = new QLabel(QStringLiteral("--"));
		hyfix_form->addRow(tr("Work mode:"), hyfix_mode_label);
		hyfix_dr_label = new QLabel(QStringLiteral("--"));
		hyfix_form->addRow(tr("Dead reckoning:"), hyfix_dr_label);

		hyfix_section = new QWidget();
		hyfix_section->setLayout(hyfix_form);
		hyfix_section->setVisible(false);
		form->addRow(hyfix_section);
	}

	// --- Messages section ---
	form->addRow(Util::Headline::create(tr("Messages")));

	messages_label = new QLabel(QStringLiteral("--"));
	messages_label->setWordWrap(true);
	messages_label->setFont(QFont(QStringLiteral("Menlo"), 10));
	form->addRow(messages_label);

	dump_button = new QPushButton(tr("Start Raw Capture"));
	dump_status_label = new QLabel();
	dump_status_label->setWordWrap(true);
	form->addRow(dump_button);
	form->addRow(dump_status_label);
	connect(dump_button, &QPushButton::clicked, this, [this]() {
		emit rawCaptureActionRequested();
	});

	// Bottom spacer so content clears the toolbar
	form->addItem(new QSpacerItem(0, 80));

	// Scroll area wrapping the content
	auto* scroll_area = new QScrollArea();
	scroll_area->setWidget(content_widget);
	scroll_area->setWidgetResizable(true);
	scroll_area->setFrameShape(QFrame::NoFrame);
	// Enable touch scrolling and consume touch events so the map doesn't scroll
	QScroller::grabGesture(scroll_area->viewport(), QScroller::TouchGesture);

	auto* main_layout = new QVBoxLayout(this);
	main_layout->setContentsMargins(0, 0, 0, 0);
	main_layout->addWidget(scroll_area);
}


void GnssDetailPanel::updateState(const GnssState& state)
{
	const auto& solution = state.solution;
	const auto& pos = solution.position;
	const auto connected = state.transportState == GnssTransportState::Connected;
	const auto has_receiver_data = state.receiverBytesReceived > 0;
	const auto has_position = solution.hasFreshPosition;
	auto healthHtml = [](const QColor& color, const QString& text) {
		return QStringLiteral("<span style='color:%1'>●</span>&nbsp; %2")
		       .arg(color.name(), text.toHtmlEscaped());
	};

	if (!connected)
	{
		status_summary_label->setText(tr("Receiver disconnected"));
		status_detail_label->setText(
		  tr("Live location and track recording are paused."));
	}
	else if (!has_receiver_data)
	{
		status_summary_label->setText(tr("Connected, but no position data"));
		status_detail_label->setText(
		  tr("Bluetooth is ready, but Mapper has not received bytes from the receiver. Check the receiver port or choose another receiver."));
	}
	else if (!has_position && state.protocol == GnssProtocol::Unknown)
	{
		status_summary_label->setText(tr("Receiver data is not recognized"));
		status_detail_label->setText(
		  tr("Bytes are arriving, but they are not a supported UBX or NMEA position stream."));
	}
	else if (!has_position && state.protocol != GnssProtocol::UBX
	         && state.protocol != GnssProtocol::NMEA
	         && state.protocol != GnssProtocol::Mixed)
	{
		status_summary_label->setText(tr("Receiver output needs configuration"));
		status_detail_label->setText(
		  tr("The connected port is carrying a recognized GNSS format, but not a UBX or NMEA position stream."));
	}
	else if (!has_position)
	{
		status_summary_label->setText(tr("Waiting for a usable position fix"));
		status_detail_label->setText(
		  tr("Receiver messages are arriving, but they do not currently contain a valid location."));
	}
	else
	{
		status_summary_label->setText(fixTypeString(pos.fixType));
		QStringList facts;
		if (std::isfinite(pos.hAccuracyP95))
			facts.append(tr("%1 m horizontal accuracy (P95)").arg(pos.hAccuracyP95, 0, 'f', 2));
		facts.append(tr("%1 satellites used").arg(pos.satellitesUsed));
		status_detail_label->setText(facts.join(QStringLiteral(" · ")));
	}

	receiver_health_label->setText(healthHtml(
	  has_position ? QColor(0x4C, 0xAF, 0x50)
	               : connected ? QColor(0xFF, 0xA7, 0x26) : QColor(0xEF, 0x53, 0x50),
	  !connected ? tr("Receiver link disconnected")
	             : !has_receiver_data ? tr("Receiver linked; no data stream")
	             : has_position ? tr("Position stream active")
	                            : tr("Receiver data active; position unavailable")));
	correction_health_label->setText(healthHtml(
	  state.correctionState == GnssCorrectionState::Flowing
	    ? QColor(0x4C, 0xAF, 0x50)
	    : state.correctionState == GnssCorrectionState::Connecting
	      || state.correctionState == GnssCorrectionState::Reconnecting
	      || state.correctionState == GnssCorrectionState::Connected
	        ? QColor(0xFF, 0xA7, 0x26) : QColor(0x9E, 0x9E, 0x9E),
	  tr("Corrections: %1").arg(correctionStateString(state.correctionState))));

	// Position section
	if (pos.timestamp.isValid())
		fix_time_label->setText(pos.timestamp.toLocalTime().toString(QStringLiteral("HH:mm:ss.zzz")));
	else
		fix_time_label->setText(QStringLiteral("--"));

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

	source_label->setText(formatFieldSource(solution.positionSource));
	limitation_label->setText(solution.summaryLimitation.isEmpty()
	                          ? QStringLiteral("--")
	                          : solution.summaryLimitation);

	// Show ellipse info alongside H accuracy
	if (pos.ellipseAvailable)
	{
		h_accuracy_label->setText(
		    formatAccuracy(pos.hAccuracyP95)
		    + QStringLiteral("  [%1 x %2m @%3\u00b0]")
		        .arg(static_cast<double>(pos.ellipseSemiMajorP95), 0, 'f', 3)
		        .arg(static_cast<double>(pos.ellipseSemiMinorP95), 0, 'f', 3)
		        .arg(static_cast<double>(pos.ellipseOrientationDeg), 0, 'f', 0));
	}

	// Show raw V accuracy even if P95 computation fails
	if (!std::isnan(pos.vAccuracy) && std::isnan(pos.vAccuracyP95))
	{
		v_accuracy_label->setText(
		    QString::number(static_cast<double>(pos.vAccuracy), 'f', 3)
		    + QLatin1String("m (raw)"));
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

	// Corrections section — populate profile combo if needed
	if (ntrip_profile_combo->count() == 0)
	{
		QSettings settings;
		auto names = settings.value(QStringLiteral("Gnss/ntrip_profiles")).toStringList();
		ntrip_profile_combo->blockSignals(true);
		for (const auto& name : names)
			ntrip_profile_combo->addItem(name);
		if (!state.ntripProfileName.isEmpty())
		{
			int idx = ntrip_profile_combo->findText(state.ntripProfileName);
			if (idx >= 0)
				ntrip_profile_combo->setCurrentIndex(idx);
		}
		ntrip_profile_combo->blockSignals(false);
	}

	correction_status_label->setText(correctionStateString(state.correctionState));

	ntrip_version_label->setText(state.ntripVersion.isEmpty() ? QStringLiteral("--") : state.ntripVersion);
	ntrip_server_label->setText(state.ntripServer.isEmpty() ? QStringLiteral("--") : state.ntripServer);

	local_age_label->setText(
	    state.localCorrectionAge < 0.0f
	        ? QStringLiteral("--")
	        : QString::number(static_cast<double>(state.localCorrectionAge), 'f', 1) + QLatin1String("s"));
	correction_age_label->setText(
	    std::isnan(pos.correctionAge)
	        ? QStringLiteral("--")
	        : QString::number(static_cast<double>(pos.correctionAge), 'f', 1) + QLatin1String("s"));
	correction_rate_label->setText(formatDataRate(state.correctionDataRate));

	// Bytes received from caster / accepted by transport / rejected by queue.
	if (state.ntripBytesReceived > 0 || state.ntripBytesSentToReceiver > 0
	    || state.ntripBytesDroppedToReceiver > 0)
	{
		auto fmtBytes = [](qint64 b) -> QString {
			if (b >= 1024 * 1024)
				return QString::number(static_cast<double>(b) / (1024.0 * 1024.0), 'f', 1) + QLatin1String(" MB");
			if (b >= 1024)
				return QString::number(static_cast<double>(b) / 1024.0, 'f', 1) + QLatin1String(" KB");
			return QString::number(b) + QLatin1String(" B");
		};
		ntrip_bytes_label->setText(fmtBytes(state.ntripBytesReceived)
		    + QLatin1String(" / ") + fmtBytes(state.ntripBytesSentToReceiver)
		    + QLatin1String(" / ") + fmtBytes(state.ntripBytesDroppedToReceiver));
	}
	else
	{
		ntrip_bytes_label->setText(QStringLiteral("--"));
	}

	gga_count_label->setText(
	    state.ggaSentCount > 0
	        ? QString::number(state.ggaSentCount)
	        : QStringLiteral("--"));
	mountpoint_label->setText(state.ntripMountpoint.isEmpty() ? QStringLiteral("--") : state.ntripMountpoint);

	// Messages section
	{
		auto now = QDateTime::currentMSecsSinceEpoch();
		QStringList lines;
		for (int i = 0; i < state.messageStatCount; ++i)
		{
			const auto& s = state.messageStats[i];
			float ageSec = (s.lastTimeMs > 0) ? (now - s.lastTimeMs) * 0.001f : -1.0f;
			QString ageStr = (ageSec < 0.0f) ? QStringLiteral("--")
			    : (ageSec < 10.0f) ? QString::number(static_cast<double>(ageSec), 'f', 1) + QLatin1String("s")
			    : QStringLiteral(">10s");
			QString hzStr = (s.avgHz > 0.01f)
			    ? QString::number(static_cast<double>(s.avgHz), 'f', 1) + QLatin1String("Hz")
			    : QStringLiteral("--");
			lines.append(QStringLiteral("%1  n=%2  %3  %4")
			    .arg(s.name, -16)
			    .arg(s.count, 5)
			    .arg(hzStr, 7)
			    .arg(ageStr, 6));
		}
		messages_label->setText(lines.isEmpty() ? QStringLiteral("--") : lines.join(QLatin1Char('\n')));
	}

	// Receiver section
	device_name_label->setText(state.deviceName.isEmpty() ? QStringLiteral("--") : state.deviceName);
	connection_type_label->setText(state.transportType.isEmpty() ? QStringLiteral("--") : state.transportType);
	receiver_data_label->setText(
	  state.receiverBytesReceived > 0
	    ? tr("%1 bytes received").arg(state.receiverBytesReceived)
	    : tr("No bytes received"));
	switch (state.protocol) {
	case GnssProtocol::UBX: receiver_protocol_label->setText(QStringLiteral("UBX")); break;
	case GnssProtocol::NMEA: receiver_protocol_label->setText(QStringLiteral("NMEA")); break;
	case GnssProtocol::Mixed: receiver_protocol_label->setText(tr("UBX + NMEA")); break;
	case GnssProtocol::RTCM3: receiver_protocol_label->setText(tr("RTCM 3 corrections only")); break;
	case GnssProtocol::BINEX: receiver_protocol_label->setText(tr("BINEX observations")); break;
	case GnssProtocol::BYNAV: receiver_protocol_label->setText(tr("BYNAV native")); break;
	case GnssProtocol::Unknown: receiver_protocol_label->setText(tr("Not detected")); break;
	}
	receiver_last_data_label->setText(
	  state.lastReceiverDataTime.isValid()
	    ? state.lastReceiverDataTime.toLocalTime().toString(QStringLiteral("HH:mm:ss.zzz"))
	    : QStringLiteral("--"));

	// GEO-PULSE section
	const auto& hyfix = state.hyfix;
	hyfix_section->setVisible(hyfix.identified);
	if (hyfix.identified)
	{
		QString firmware = hyfix.productFirmware.isEmpty() ? QStringLiteral("--") : hyfix.productFirmware;
		if (!hyfix.gnssFirmware.isEmpty())
			//: %1 is the product firmware version, %2 the GNSS module firmware version
			firmware = tr("%1 (GNSS %2)").arg(firmware, hyfix.gnssFirmware);
		hyfix_firmware_label->setText(firmware);

		if (hyfix.nmeaIntervalMs > 0)
		{
			hyfix_rate_label->setText(tr("%1 ms (%2 Hz)")
			  .arg(hyfix.nmeaIntervalMs)
			  .arg(1000.0 / hyfix.nmeaIntervalMs, 0, 'g', 2));
		}
		else
		{
			hyfix_rate_label->setText(QStringLiteral("--"));
		}

		QString mode = hyfix.workMode.isEmpty() ? QStringLiteral("--") : hyfix.workMode;
		if (!hyfix.correctionLink.isEmpty())
			//: %1 is a receiver work mode, %2 the link its corrections arrive on
			mode = tr("%1, corrections via %2").arg(mode, hyfix.correctionLink);
		hyfix_mode_label->setText(mode);

		hyfix_dr_label->setText(hyfixDeadReckoningText(hyfix));
	}

	bool connection_active = connected
	                      || state.transportState == GnssTransportState::Reconnecting;
	connect_button->setText(connection_active ? tr("Disconnect") : tr("Connect"));
}


QString GnssDetailPanel::hyfixDeadReckoningText(const HyfixDeviceInfo& info)
{
	QString calibration;
	switch (info.drCalibrationState) {
	case 0:  calibration = tr("not calibrated"); break;
	case 1:  calibration = tr("lightly calibrated"); break;
	case 2:  calibration = tr("calibrated"); break;
	case 3:  calibration = tr("calibrated, high-precision heading"); break;
	default: calibration = tr("unknown"); break;
	}

	QString navigation;
	switch (info.drNavType) {
	case 0:  navigation = tr("no position"); break;
	case 1:  navigation = tr("GNSS only"); break;
	case 2:  navigation = tr("dead reckoning only"); break;
	case 3:  navigation = tr("GNSS + dead reckoning"); break;
	default: return calibration;
	}
	//: %1 is a dead-reckoning calibration state, %2 how the solution is computed
	return tr("%1, %2").arg(calibration, navigation);
}


}  // namespace OpenOrienteering
