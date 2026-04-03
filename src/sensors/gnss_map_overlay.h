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

#ifndef OPENORIENTEERING_GNSS_MAP_OVERLAY_H
#define OPENORIENTEERING_GNSS_MAP_OVERLAY_H

#include <vector>

#include <QObject>
#include <QPointF>

#include "core/map_coord.h"

class QPainter;
class QTimerEvent;

namespace OpenOrienteering {

class Georeferencing;
class GnssPositionBridge;
class MapWidget;


/**
 * Draws live GNSS position state and temporary GNSS markers over the map widget.
 *
 * Rendering happens in viewport coordinates derived from the current map view.
 */
class GnssMapOverlay : public QObject
{
Q_OBJECT
public:
	GnssMapOverlay(MapWidget* widget, GnssPositionBridge* position_bridge, const Georeferencing& georeferencing, QObject* parent = nullptr);
	~GnssMapOverlay() override;

	void setVisible(bool visible);
	bool isVisible() const { return visible; }

	void enableDistanceRings(bool enable);
	void enableHeadingIndicator(bool enable);

	void startBlinking(int seconds);
	void stopBlinking();

	bool addPoint();
	void startPath();
	void stopPath();
	void clear();

	void paint(QPainter* painter);

protected:
	void timerEvent(QTimerEvent* e) override;

private slots:
	void onDisplayStateChanged();
	void onPositionUpdated(const OpenOrienteering::MapCoordF& coord, float accuracy);

private:
	void updateMapWidget();

	class PulsatingOpacity
	{
	public:
		bool isActive() const { return bool(timer_id); }
		void start(QObject& object);
		void stop(QObject& object);
		int timerId() const { return timer_id; }
		bool advance();
		qreal current() const;

	private:
		int timer_id = 0;
		quint8 index = 0;
	};

	MapWidget* widget;
	GnssPositionBridge* position_bridge;
	const Georeferencing& georeferencing;
	bool visible = false;
	bool distance_rings_enabled = false;
	bool heading_indicator_enabled = false;
	bool recording_path = false;
	std::vector<QPointF> points;
	std::vector<std::vector<QPointF>> paths;
	PulsatingOpacity pulsating_opacity;
	int blink_count = 0;
};


}  // namespace OpenOrienteering

#endif
