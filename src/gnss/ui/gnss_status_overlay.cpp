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
#include <QFont>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QRectF>
#include <QString>

#include "gui/util_gui.h"


namespace OpenOrienteering {

namespace {

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

/// Returns the indicator color for the correction state.
/// The bool reference indicates whether the circle should be filled.
QColor correctionIndicatorColor(GnssCorrectionState state, bool& filled)
{
	filled = false;
	switch (state) {
	case GnssCorrectionState::Disabled:
	case GnssCorrectionState::Disconnected:
		return QColor(0x80, 0x80, 0x80);  // gray, unfilled
	case GnssCorrectionState::Connecting:
	case GnssCorrectionState::Reconnecting:
		return QColor(0xFF, 0xD6, 0x00);  // yellow outline
	case GnssCorrectionState::Flowing:
		filled = true;
		return QColor(0x4C, 0xAF, 0x50);  // green filled
	case GnssCorrectionState::Connected:
	case GnssCorrectionState::Stale:
		filled = true;
		return QColor(0xFF, 0x98, 0x00);  // orange filled
	}
	return QColor(0x80, 0x80, 0x80);
}

/// Returns the indicator color for the transport state.
QColor transportIndicatorColor(GnssTransportState state)
{
	switch (state) {
	case GnssTransportState::Disconnected:
		return QColor(0xF4, 0x43, 0x36);  // red
	case GnssTransportState::Connecting:
	case GnssTransportState::Reconnecting:
		return QColor(0xFF, 0xD6, 0x00);  // yellow
	case GnssTransportState::Connected:
		return QColor(0x4C, 0xAF, 0x50);  // green
	}
	return QColor(0xF4, 0x43, 0x36);
}

}  // namespace


GnssStatusOverlay::GnssStatusOverlay(QWidget* parent)
 : QWidget(parent)
{
	setAttribute(Qt::WA_NoSystemBackground, true);
}

GnssStatusOverlay::~GnssStatusOverlay()
{
	// nothing, not inlined
}

void GnssStatusOverlay::updateState(const GnssState& state)
{
	m_fixType = state.solution.position.fixType;
	m_accuracyP95 = state.solution.position.hAccuracyP95;
	m_correctionState = state.correctionState;
	m_transportState = state.transportState;
	update();
	if (auto* p = parentWidget())
		p->update(geometry());
}

QSize GnssStatusOverlay::sizeHint() const
{
	auto w = qRound(Util::mmToPixelLogical(60.0));
	auto h = qRound(Util::mmToPixelLogical(10.0));
	return QSize(w, h);
}

void GnssStatusOverlay::paintEvent(QPaintEvent*)
{
	QPainter painter(this);
	painter.setRenderHint(QPainter::Antialiasing);

	auto mm = [](qreal v) { return Util::mmToPixelLogical(v); };

	auto const margin = mm(1.0);
	auto const padding = mm(1.5);
	auto const indicatorRadius = mm(1.5);
	auto const badgeRadius = mm(1.5);

	QRectF bounds = { {0, 0}, QSizeF(size()) };
	bounds.adjust(margin, margin, -margin, -margin);

	// -- Background: semi-transparent dark rounded rect --
	painter.setPen(Qt::NoPen);
	painter.setBrush(QColor(0, 0, 0, 160));
	painter.drawRoundedRect(bounds, mm(2.0), mm(2.0));

	qreal x = bounds.left() + padding;
	qreal const cy = bounds.center().y();

	// -- 1. Fix badge --
	{
		QFont badgeFont;
		badgeFont.setPixelSize(qRound(mm(4.0)));
		badgeFont.setBold(true);
		painter.setFont(badgeFont);

		QString text = fixBadgeText(m_fixType);
		QFontMetricsF fm(badgeFont);
		qreal textWidth = fm.horizontalAdvance(text);
		qreal badgeW = textWidth + mm(2.0);
		qreal badgeH = fm.height() + mm(1.0);
		QRectF badgeRect(x, cy - badgeH / 2.0, badgeW, badgeH);

		painter.setPen(Qt::NoPen);
		painter.setBrush(fixBadgeColor(m_fixType));
		painter.drawRoundedRect(badgeRect, badgeRadius, badgeRadius);

		painter.setPen(Qt::white);
		painter.drawText(badgeRect, Qt::AlignCenter, text);

		x = badgeRect.right() + padding;
	}

	// -- 2. P95 accuracy text --
	{
		QFont monoFont;
		monoFont.setFamily(QStringLiteral("Menlo"));
		monoFont.setStyleHint(QFont::Monospace);
		monoFont.setPixelSize(qRound(mm(4.5)));
		painter.setFont(monoFont);

		QString accText;
		if (std::isnan(m_accuracyP95))
		{
			accText = QStringLiteral("--");
		}
		else if (m_accuracyP95 < 10.0f)
		{
			accText = QString::number(static_cast<double>(m_accuracyP95), 'f', 2) + QStringLiteral("m");
		}
		else
		{
			accText = QString::number(static_cast<double>(m_accuracyP95), 'f', 1) + QStringLiteral("m");
		}

		painter.setPen(Qt::white);
		QFontMetricsF fm(monoFont);
		qreal textWidth = fm.horizontalAdvance(accText);
		QRectF textRect(x, cy - fm.height() / 2.0, textWidth, fm.height());
		painter.drawText(textRect, Qt::AlignVCenter | Qt::AlignLeft, accText);

		x = textRect.right() + padding;
	}

	// -- 3. Correction indicator --
	{
		bool filled = false;
		QColor color = correctionIndicatorColor(m_correctionState, filled);

		if (filled)
		{
			painter.setPen(Qt::NoPen);
			painter.setBrush(color);
		}
		else
		{
			painter.setPen(QPen(color, mm(0.5)));
			painter.setBrush(Qt::NoBrush);
		}
		painter.drawEllipse(QPointF(x + indicatorRadius, cy), indicatorRadius, indicatorRadius);

		x += indicatorRadius * 2.0 + padding;
	}

	// -- 4. Transport indicator --
	{
		QColor color = transportIndicatorColor(m_transportState);
		painter.setPen(Qt::NoPen);
		painter.setBrush(color);
		painter.drawEllipse(QPointF(x + indicatorRadius, cy), indicatorRadius, indicatorRadius);
	}
}

void GnssStatusOverlay::mousePressEvent(QMouseEvent* event)
{
	event->accept();
	emit clicked();
}


}  // namespace OpenOrienteering
