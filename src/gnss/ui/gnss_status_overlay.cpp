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

#include "gnss_status_overlay.h"

#include <cmath>

#include <QColor>
#include <QDebug>
#include <QFont>
#include <QFontMetricsF>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QString>

#include "gui/util_gui.h"


namespace OpenOrienteering {

namespace {

bool transportConnected(GnssTransportState state)
{
	return state == GnssTransportState::Connected;
}

QString protocolText(GnssProtocol protocol)
{
	switch (protocol) {
	case GnssProtocol::UBX: return QStringLiteral("UBX");
	case GnssProtocol::NMEA: return QStringLiteral("NMEA");
	case GnssProtocol::Mixed: return QStringLiteral("UBX + NMEA");
	case GnssProtocol::RTCM3: return QStringLiteral("RTCM 3");
	case GnssProtocol::BINEX: return QStringLiteral("BINEX");
	case GnssProtocol::BYNAV: return QStringLiteral("BYNAV");
	case GnssProtocol::Unknown: return {};
	}
	return {};
}

/// Returns the badge color for the given fix type.
QColor fixBadgeColor(GnssFixType fix)
{
	switch (fix) {
	case GnssFixType::RtkFixed: return QColor(0x22, 0x7C, 0xE8);  // blue
	case GnssFixType::RtkFloat: return QColor(0xED, 0x9A, 0x14);  // amber
	case GnssFixType::DGPS:     return QColor(0x00, 0x96, 0x88);  // teal
	case GnssFixType::Fix3D:    return QColor(0x80, 0x80, 0x80);  // gray
	case GnssFixType::Fix2D:    return QColor(0x80, 0x80, 0x80);  // gray
	case GnssFixType::NoFix:    return QColor(0x50, 0x50, 0x50);  // dark gray
	}
	return QColor(0x50, 0x50, 0x50);
}

/// Returns the badge label for the given fix type.
QString fixBadgeText(GnssFixType fix)
{
	switch (fix) {
	case GnssFixType::RtkFixed: return QStringLiteral("RTK");
	case GnssFixType::RtkFloat: return QStringLiteral("FLT");
	case GnssFixType::DGPS:     return QStringLiteral("DIF");
	case GnssFixType::Fix3D:    return QStringLiteral("3D");
	case GnssFixType::Fix2D:    return QStringLiteral("2D");
	case GnssFixType::NoFix:    return QStringLiteral("---");
	}
	return QStringLiteral("---");
}

QString fixTitle(GnssFixType fix)
{
	switch (fix) {
	case GnssFixType::RtkFixed: return GnssStatusOverlay::tr("RTK fixed");
	case GnssFixType::RtkFloat: return GnssStatusOverlay::tr("RTK float");
	case GnssFixType::DGPS: return GnssStatusOverlay::tr("Differential fix");
	case GnssFixType::Fix3D: return GnssStatusOverlay::tr("3D position");
	case GnssFixType::Fix2D: return GnssStatusOverlay::tr("2D position");
	case GnssFixType::NoFix: return GnssStatusOverlay::tr("No position fix");
	}
	return GnssStatusOverlay::tr("No position fix");
}

QString correctionSummary(GnssCorrectionState state)
{
	switch (state) {
	case GnssCorrectionState::Flowing: return GnssStatusOverlay::tr("corrections flowing");
	case GnssCorrectionState::Connected: return GnssStatusOverlay::tr("corrections waiting");
	case GnssCorrectionState::Stale: return GnssStatusOverlay::tr("corrections stale");
	case GnssCorrectionState::Connecting:
	case GnssCorrectionState::Reconnecting: return GnssStatusOverlay::tr("corrections connecting");
	case GnssCorrectionState::Disconnected: return GnssStatusOverlay::tr("corrections offline");
	case GnssCorrectionState::Disabled: return GnssStatusOverlay::tr("no corrections");
	}
	return GnssStatusOverlay::tr("no corrections");
}

}  // namespace


GnssStatusOverlay::GnssStatusOverlay(QWidget* parent)
 : QToolButton(parent)
{
	setAttribute(Qt::WA_NoSystemBackground, true);
	setAutoRaise(true);
	setCheckable(false);
	setFocusPolicy(Qt::NoFocus);
	setCursor(Qt::PointingHandCursor);
	connect(this, &QToolButton::clicked, this, [] {
		qInfo("GNSS status control activated (button)");
	});
	m_repaintTimer.setInterval(1000);
	m_repaintTimer.setSingleShot(true);
	connect(&m_repaintTimer, &QTimer::timeout, this, [this] {
		if (!m_repaintPending)
			return;
		m_repaintPending = false;
		repaintLatestState();
		m_repaintTimer.start();
	});
}

GnssStatusOverlay::~GnssStatusOverlay()
{
	// nothing, not inlined
}

void GnssStatusOverlay::updateState(const GnssState& state)
{
	m_state = state;
	QString accessible;
	if (!transportConnected(state.transportState))
		accessible = tr("GNSS receiver disconnected");
	else if (state.receiverBytesReceived == 0)
		accessible = tr("GNSS connected, waiting for receiver data");
	else if (!state.solution.hasFreshPosition)
		accessible = state.protocol == GnssProtocol::Unknown
		  ? tr("GNSS data is not recognized")
		  : tr("GNSS receiver data is arriving, but there is no position fix");
	else
		accessible = fixTitle(state.solution.position.fixType);
	setAccessibleName(accessible);
	setAccessibleDescription(tr("Tap for GNSS status and configuration"));
	setToolTip(accessible + QStringLiteral(". ")
	           + tr("Tap for status and configuration."));
	if (m_repaintTimer.isActive())
	{
		m_repaintPending = true;
		return;
	}
	repaintLatestState();
	m_repaintTimer.start();
}

void GnssStatusOverlay::repaintLatestState()
{
	update();
	if (auto* p = parentWidget())
		p->update(geometry());
}

QSize GnssStatusOverlay::sizeHint() const
{
#if defined(Q_OS_IOS)
	// Large enough to read and acquire one-handed while still fitting beside
	// the portrait Dynamic Island on current iPhones. This is also the touch
	// target, so do not shrink it back to a decorative status badge.
	auto w = qRound(Util::mmToPixelLogical(33.0));
	auto h = qRound(Util::mmToPixelLogical(11.5));
#else
	auto w = qRound(Util::mmToPixelLogical(64.0));
	auto h = qRound(Util::mmToPixelLogical(15.0));
#endif
	return QSize(w, h);
}

void GnssStatusOverlay::paintEvent(QPaintEvent*)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	auto mm = [](qreal v) { return Util::mmToPixelLogical(v); };

#if defined(Q_OS_IOS)
	{
	const auto& position = m_state.solution.position;
	const bool connected = transportConnected(m_state.transportState);
	const bool hasBytes = m_state.receiverBytesReceived > 0;
	const bool freshPosition = m_state.solution.hasFreshPosition;
	QColor healthColor;
	QString text;
	if (!connected)
	{
		healthColor = QColor(0xEF, 0x53, 0x50);
		text = tr("GNSS off");
	}
	else if (!hasBytes)
	{
		healthColor = QColor(0xFF, 0xA7, 0x26);
		text = tr("No data");
	}
	else if (!freshPosition)
	{
		healthColor = QColor(0xFF, 0xC1, 0x07);
		switch (m_state.protocol)
		{
		case GnssProtocol::RTCM3: text = tr("RTCM only"); break;
		case GnssProtocol::BINEX: text = tr("BINEX"); break;
		case GnssProtocol::BYNAV: text = tr("BYNAV"); break;
		case GnssProtocol::Unknown: text = tr("Data?"); break;
		case GnssProtocol::UBX:
		case GnssProtocol::NMEA:
		case GnssProtocol::Mixed: text = tr("No fix"); break;
		}
	}
	else
	{
		healthColor = fixBadgeColor(position.fixType);
		text = fixBadgeText(position.fixType);
		if (std::isfinite(position.hAccuracyP95))
			text += position.hAccuracyP95 < 10.0f
			      ? tr("  %1m").arg(position.hAccuracyP95, 0, 'f', 2)
			      : tr("  %1m").arg(position.hAccuracyP95, 0, 'f', 1);
	}

	QRectF bounds(QPointF(0, 0), QSizeF(size()));
	bounds.adjust(mm(0.25), mm(0.25), -mm(0.25), -mm(0.25));
	painter.setPen(QPen(QColor(255, 255, 255, 32), mm(0.18)));
	painter.setBrush(QColor(12, 16, 22, 238));
	painter.drawRoundedRect(bounds, bounds.height() / 2.0, bounds.height() / 2.0);

	const auto dotDiameter = mm(2.8);
	QRectF dot(bounds.left() + mm(1.6),
	           bounds.center().y() - dotDiameter / 2.0,
	           dotDiameter, dotDiameter);
	painter.setPen(Qt::NoPen);
	painter.setBrush(healthColor);
	painter.drawEllipse(dot);

	QFont compactFont = font();
	compactFont.setPixelSize(qRound(mm(3.6)));
	compactFont.setBold(true);
	painter.setFont(compactFont);
	painter.setPen(Qt::white);
	QRectF textRect(dot.right() + mm(1.2), bounds.top(),
	                bounds.right() - dot.right() - mm(3.0), bounds.height());
	painter.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter,
	                 QFontMetricsF(compactFont).elidedText(
	                   text, Qt::ElideRight, textRect.width()));

	QFont chevronFont = compactFont;
	chevronFont.setBold(false);
	painter.setFont(chevronFont);
	painter.setPen(QColor(220, 226, 234));
	QRectF chevronRect(bounds.right() - mm(2.9), bounds.top(),
	                   mm(2.0), bounds.height());
	painter.drawText(chevronRect, Qt::AlignCenter, QStringLiteral("›"));
	return;
	}
#endif

	auto const margin = mm(1.0);
	auto const padding = mm(1.4);
	auto const badgeRadius = mm(1.5);

	QRectF bounds = { {0, 0}, QSizeF(size()) };
	bounds.adjust(margin, margin, -margin, -margin);

	// High-contrast card remains readable over aerial imagery and map colors.
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(19, 24, 31, 225));
	painter.drawRoundedRect(bounds, mm(2.0), mm(2.0));

	const auto& solution = m_state.solution;
	const auto& position = solution.position;
	const bool connected = transportConnected(m_state.transportState);
	const bool hasBytes = m_state.receiverBytesReceived > 0;
	const bool freshPosition = solution.hasFreshPosition;
	QColor healthColor;
	QString title;
	QString detail;
	if (!connected)
	{
		healthColor = QColor(0xEF, 0x53, 0x50);
		title = tr("Receiver disconnected");
		detail = tr("Tap to reconnect or change receiver");
	}
	else if (!hasBytes)
	{
		healthColor = QColor(0xFF, 0xA7, 0x26);
		title = tr("Waiting for receiver data");
		detail = tr("BLE connected · no position stream");
	}
	else if (!freshPosition)
	{
		healthColor = QColor(0xFF, 0xC1, 0x07);
		title = m_state.protocol == GnssProtocol::Unknown
		      ? tr("Unrecognized receiver data") : tr("Waiting for position fix");
		auto protocol = protocolText(m_state.protocol);
		detail = protocol.isEmpty()
		       ? tr("Data arriving · tap for diagnostics")
		       : tr("%1 data arriving · no usable fix").arg(protocol);
	}
	else
	{
		healthColor = fixBadgeColor(position.fixType);
		title = fixTitle(position.fixType);
		if (std::isfinite(position.hAccuracyP95))
			title += position.hAccuracyP95 < 10.0f
			       ? tr(" · %1 m").arg(position.hAccuracyP95, 0, 'f', 2)
			       : tr(" · %1 m").arg(position.hAccuracyP95, 0, 'f', 1);
		detail = tr("%1 satellites · %2")
		           .arg(position.satellitesUsed)
		           .arg(correctionSummary(m_state.correctionState));
	}

	// A colored leading rail communicates overall positioning health. Receiver
	// and NTRIP connectivity alone never produce a green success state.
	QRectF rail(bounds.left(), bounds.top(), mm(1.5), bounds.height());
	painter.setBrush(healthColor);
	painter.drawRoundedRect(rail, mm(0.75), mm(0.75));

	qreal x = rail.right() + padding;
	qreal const cy = bounds.center().y();

	// Compact fix badge.
	{
		QFont badgeFont;
		badgeFont.setPixelSize(qRound(mm(4.0)));
		badgeFont.setBold(true);
		painter.setFont(badgeFont);

		QString text = freshPosition ? fixBadgeText(position.fixType) : QStringLiteral("!");
		QFontMetricsF fm(badgeFont);
		qreal textWidth = fm.horizontalAdvance(text);
		qreal badgeW = textWidth + mm(2.0);
		qreal badgeH = fm.height() + mm(1.0);
		QRectF badgeRect(x, cy - badgeH / 2.0, badgeW, badgeH);

		painter.setPen(Qt::NoPen);
		painter.setBrush(healthColor);
		painter.drawRoundedRect(badgeRect, badgeRadius, badgeRadius);

		painter.setPen(Qt::white);
		painter.drawText(badgeRect, Qt::AlignCenter, text);

		x = badgeRect.right() + padding;
	}

	const auto chevronWidth = mm(4.0);
	const auto textRight = bounds.right() - padding - chevronWidth;

	// Primary and explanatory secondary lines.
	{
		QFont titleFont = font();
		titleFont.setPixelSize(qRound(mm(3.7)));
		titleFont.setBold(true);
		painter.setFont(titleFont);
		painter.setPen(Qt::white);
		QRectF titleRect(x, bounds.top() + mm(1.7), textRight - x, mm(5.0));
		painter.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
		                 QFontMetricsF(titleFont).elidedText(title, Qt::ElideRight, titleRect.width()));

		QFont detailFont = font();
		detailFont.setPixelSize(qRound(mm(3.0)));
		painter.setFont(detailFont);
		painter.setPen(QColor(220, 226, 234));
		QRectF detailRect(x, titleRect.bottom(), textRight - x, mm(4.5));
		painter.drawText(detailRect, Qt::AlignLeft | Qt::AlignVCenter,
		                 QFontMetricsF(detailFont).elidedText(detail, Qt::ElideRight, detailRect.width()));
	}

	// Chevron makes the tap-through behavior discoverable.
	painter.setPen(QPen(QColor(220, 226, 234), mm(0.45), Qt::SolidLine,
	                    Qt::RoundCap, Qt::RoundJoin));
	auto chevronX = bounds.right() - padding - mm(1.0);
	painter.drawLine(QPointF(chevronX - mm(1.2), cy - mm(1.7)),
	                 QPointF(chevronX, cy));
	painter.drawLine(QPointF(chevronX, cy),
	                 QPointF(chevronX - mm(1.2), cy + mm(1.7)));
}

}  // namespace OpenOrienteering
