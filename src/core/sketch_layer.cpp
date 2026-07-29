/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "core/sketch_layer.h"

#include <algorithm>
#include <cmath>

#include <QString>
#include <QCoreApplication>
#include <QLineF>
#include <QLocale>

#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/symbols/sketch_symbol.h"
#include "core/symbols/symbol.h"

namespace OpenOrienteering {

namespace {

constexpr auto layer_name = "Sketch";

qreal distanceSquaredToSegment(
        const MapCoordF& point, const MapCoordF& start, const MapCoordF& end)
{
	const auto segment = end - start;
	const auto length_squared = segment.lengthSquared();
	if (length_squared <= 0)
		return point.distanceSquaredTo(start);
	const auto projection = std::clamp(
	    MapCoordF::dotProduct(point - start, segment) / length_squared,
	    qreal(0), qreal(1));
	const auto nearest = start + projection * segment;
	return point.distanceSquaredTo(nearest);
}

qreal distanceSquaredBetweenSegments(
        const MapCoordF& a0, const MapCoordF& a1,
        const MapCoordF& b0, const MapCoordF& b1)
{
	QPointF intersection;
	if (QLineF(a0, a1).intersects(QLineF(b0, b1), &intersection)
	    == QLineF::BoundedIntersection)
		return 0;
	return std::min({
	    distanceSquaredToSegment(a0, b0, b1),
	    distanceSquaredToSegment(a1, b0, b1),
	    distanceSquaredToSegment(b0, a0, a1),
	    distanceSquaredToSegment(b1, a0, a1),
	});
}

void retainDouglasPeucker(
        const std::vector<MapCoordF>& points, std::size_t first,
        std::size_t last, qreal tolerance_squared,
        std::vector<bool>& retained)
{
	if (last <= first + 1)
		return;
	auto best_index = first;
	auto best_distance = qreal(0);
	for (auto i = first + 1; i < last; ++i)
	{
		const auto distance =
		    distanceSquaredToSegment(points[i], points[first], points[last]);
		if (distance > best_distance)
		{
			best_distance = distance;
			best_index = i;
		}
	}
	if (best_distance <= tolerance_squared)
		return;
	retained[best_index] = true;
	retainDouglasPeucker(
	    points, first, best_index, tolerance_squared, retained);
	retainDouglasPeucker(
	    points, best_index, last, tolerance_squared, retained);
}

}  // namespace

QString SketchLayer::layerName()
{
	return QString::fromLatin1(layer_name);
}

QString SketchLayer::defaultLayerName(const QDate& date)
{
	return QCoreApplication::translate(
	           "SketchLayer", "Field Sketches — %1")
	    .arg(QLocale().toString(date, QLocale::ShortFormat));
}

qreal SketchLayer::widthMillimeters(Width width) noexcept
{
	switch (width)
	{
	case Width::Fine: return 0.18;
	case Width::Broad: return 0.70;
	case Width::Medium: return 0.35;
	}
	return 0.35;
}

QString SketchLayer::widthName(Width width)
{
	switch (width)
	{
	case Width::Fine: return QStringLiteral("Fine");
	case Width::Broad: return QStringLiteral("Broad");
	case Width::Medium: return QStringLiteral("Medium");
	}
	return QStringLiteral("Medium");
}

SketchLayer::Width SketchLayer::widthFromSetting(int value) noexcept
{
	switch (value)
	{
	case 0: return Width::Fine;
	case 2: return Width::Broad;
	default: return Width::Medium;
	}
}

int SketchLayer::widthSetting(Width width) noexcept
{
	return static_cast<int>(width);
}

bool SketchLayer::isSketchSymbol(const Symbol* symbol) noexcept
{
	return dynamic_cast<const SketchSymbol*>(symbol);
}

bool SketchLayer::isSketchObject(const Object* object) noexcept
{
	return object && isSketchSymbol(object->getSymbol());
}

bool SketchLayer::isSketchPart(const MapPart* part) noexcept
{
	if (!part)
		return false;
	if (part->hasSketchLayerMetadata())
		return true;
	if (part->getName() == QLatin1String(layer_name))
		return true;
	return part->existsObject(
	    [](const Object* object) { return isSketchObject(object); });
}

MapPart* SketchLayer::find(Map& map) noexcept
{
	return const_cast<MapPart*>(find(static_cast<const Map&>(map)));
}

const MapPart* SketchLayer::find(const Map& map) noexcept
{
	for (int i = 0; i < map.getNumParts(); ++i)
	{
		const auto* part = map.getPart(i);
		if (part->existsObject(
		        [](const Object* object) { return isSketchObject(object); }))
			return part;
	}
	for (int i = 0; i < map.getNumParts(); ++i)
	{
		const auto* part = map.getPart(i);
		if (part->getName() == QLatin1String(layer_name))
			return part;
	}
	return nullptr;
}

std::vector<MapPart*> SketchLayer::all(Map& map)
{
	std::vector<MapPart*> result;
	for (int i = 0; i < map.getNumParts(); ++i)
		if (isSketchPart(map.getPart(i)))
			result.push_back(map.getPart(i));
	return result;
}

std::vector<const MapPart*> SketchLayer::all(const Map& map)
{
	std::vector<const MapPart*> result;
	for (int i = 0; i < map.getNumParts(); ++i)
		if (isSketchPart(map.getPart(i)))
			result.push_back(map.getPart(i));
	return result;
}

MapPart* SketchLayer::findById(Map& map, const QString& id) noexcept
{
	for (auto* part : all(map))
		if (part->persistentId() == id)
			return part;
	return nullptr;
}

MapPart* SketchLayer::create(
        Map& map, const QString& name, const QString& owner_id,
        const QString& owner_name, const QDate& created_on)
{
	auto* part = new MapPart(name, &map);
	part->setSketchLayerMetadata(owner_id, owner_name, created_on);
	map.addPart(part, map.getNumParts());
	return part;
}

MapPart* SketchLayer::ensure(Map& map)
{
	if (auto* existing = find(map))
		return existing;
	auto* part = new MapPart(layerName(), &map);
	map.addPart(part, map.getNumParts());
	return part;
}

int SketchLayer::partIndex(const Map& map) noexcept
{
	const auto* part = find(map);
	return part ? map.findPartIndex(part) : -1;
}

SketchSymbol* SketchLayer::findSymbol(
        Map& map, const QColor& color, Width width) noexcept
{
	const auto width_native =
	    qRound(1000 * widthMillimeters(width));
	for (int i = 0; i < map.getNumSymbols(); ++i)
	{
		auto* symbol = dynamic_cast<SketchSymbol*>(map.getSymbol(i));
		if (symbol && symbol->strokeColor().rgba64() == color.rgba64()
		    && symbol->getLineWidth() == width_native)
			return symbol;
	}
	return nullptr;
}

SketchSymbol* SketchLayer::ensureSymbol(
        Map& map, const QColor& color, Width width)
{
	if (auto* existing = findSymbol(map, color, width))
		return existing;
	auto* symbol = new SketchSymbol;
	symbol->setStrokeColor(color.isValid() ? color : QColor(Qt::black));
	symbol->setLineWidth(widthMillimeters(width));
	symbol->setName(
	    QStringLiteral("Sketch %1 — %2")
	        .arg(symbol->strokeColor().name(QColor::HexRgb).toUpper(),
	             widthName(width)));
	symbol->setDescription(QStringLiteral("mapper-sketch-v1"));
	map.addSymbol(symbol, map.getNumSymbols());
	return symbol;
}

std::vector<MapCoordF> SketchLayer::simplify(
        const std::vector<MapCoordF>& points, qreal tolerance)
{
	if (points.size() <= 2 || tolerance <= 0)
		return points;
	std::vector<bool> retained(points.size(), false);
	retained.front() = true;
	retained.back() = true;
	retainDouglasPeucker(
	    points, 0, points.size() - 1, tolerance * tolerance, retained);
	std::vector<MapCoordF> result;
	result.reserve(points.size());
	for (std::size_t i = 0; i < points.size(); ++i)
	{
		if (retained[i])
			result.push_back(points[i]);
	}
	return result;
}

bool SketchLayer::intersectsStroke(
        const std::vector<MapCoordF>& eraser,
        const PathObject& stroke, qreal tolerance)
{
	if (eraser.empty() || tolerance < 0)
		return false;
	stroke.update();
	const auto tolerance_squared = tolerance * tolerance;
	for (const auto& part : stroke.parts())
	{
		const auto& path_coords = part.path_coords;
		if (path_coords.empty())
			continue;
		if (path_coords.size() == 1)
		{
			for (std::size_t e = 1; e < eraser.size(); ++e)
			{
				if (distanceSquaredToSegment(
				        path_coords.front().pos, eraser[e - 1], eraser[e])
				    <= tolerance_squared)
					return true;
			}
			continue;
		}
		for (std::size_t p = 1; p < path_coords.size(); ++p)
		{
			if (eraser.size() == 1
			    && distanceSquaredToSegment(
			           eraser.front(), path_coords[p - 1].pos,
			           path_coords[p].pos)
			           <= tolerance_squared)
				return true;
			for (std::size_t e = 1; e < eraser.size(); ++e)
			{
				if (distanceSquaredBetweenSegments(
				        eraser[e - 1], eraser[e],
				        path_coords[p - 1].pos, path_coords[p].pos)
				    <= tolerance_squared)
					return true;
			}
		}
	}
	return false;
}

}  // namespace OpenOrienteering
