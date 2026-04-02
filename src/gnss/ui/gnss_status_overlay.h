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


#ifndef OPENORIENTEERING_GNSS_STATUS_OVERLAY_H
#define OPENORIENTEERING_GNSS_STATUS_OVERLAY_H

#include <cmath>

#include <QObject>
#include <QSize>
#include <QWidget>

#include "gnss/gnss_state.h"

class QMouseEvent;
class QPaintEvent;

namespace OpenOrienteering {


/**
 * A compact transparent overlay widget showing GNSS status at a glance.
 *
 * Displays fix type, P95 accuracy, correction state, and transport state
 * as a horizontal bar. Intended as a sibling of MapWidget with
 * WA_NoSystemBackground, following the same pattern as CompassDisplay.
 *
 * Tapping the overlay emits clicked() to open the GNSS detail panel.
 */
class GnssStatusOverlay : public QWidget
{
	Q_OBJECT
public:
	explicit GnssStatusOverlay(QWidget* parent = nullptr);
	~GnssStatusOverlay() override;

	void updateState(const GnssState& state);
	QSize sizeHint() const override;

signals:
	void clicked();  // emitted on tap to open detail panel

protected:
	void paintEvent(QPaintEvent* event) override;
	void mousePressEvent(QMouseEvent* event) override;

private:
	GnssFixType m_fixType = GnssFixType::NoFix;
	float m_accuracyP95 = NAN;
	GnssCorrectionState m_correctionState = GnssCorrectionState::Disabled;
	GnssTransportState m_transportState = GnssTransportState::Disconnected;
};


}  // namespace OpenOrienteering

#endif
