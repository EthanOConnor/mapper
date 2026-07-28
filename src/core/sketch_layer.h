/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_SKETCH_LAYER_H
#define OPENORIENTEERING_SKETCH_LAYER_H

#include <vector>

#include <QColor>

#include "core/map_coord.h"

namespace OpenOrienteering {

class Map;
class MapPart;
class Object;
class PathObject;
class SketchSymbol;
class Symbol;

/**
 * Map-native model helpers for Mapper's single seamless sketch layer.
 */
class SketchLayer
{
public:
	enum class Width
	{
		Fine,
		Medium,
		Broad
	};

	static QString layerName();
	static qreal widthMillimeters(Width width) noexcept;
	static QString widthName(Width width);
	static Width widthFromSetting(int value) noexcept;
	static int widthSetting(Width width) noexcept;

	static bool isSketchSymbol(const Symbol* symbol) noexcept;
	static bool isSketchObject(const Object* object) noexcept;
	static bool isSketchPart(const MapPart* part) noexcept;

	static MapPart* find(Map& map) noexcept;
	static const MapPart* find(const Map& map) noexcept;
	static MapPart* ensure(Map& map);
	static int partIndex(const Map& map) noexcept;

	static SketchSymbol* findSymbol(
	    Map& map, const QColor& color, Width width) noexcept;
	static SketchSymbol* ensureSymbol(
	    Map& map, const QColor& color, Width width);

	/**
	 * Simplifies a sampled stroke to the requested map-coordinate tolerance
	 * while retaining both endpoints.
	 */
	static std::vector<MapCoordF> simplify(
	    const std::vector<MapCoordF>& points, qreal tolerance);

	/**
	 * Returns whether a continuous eraser path comes within tolerance of a
	 * sketch path, including between sparse input events.
	 */
	static bool intersectsStroke(
	    const std::vector<MapCoordF>& eraser,
	    const PathObject& stroke, qreal tolerance);
};

}  // namespace OpenOrienteering

#endif
