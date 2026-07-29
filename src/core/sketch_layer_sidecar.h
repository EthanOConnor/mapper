/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_SKETCH_LAYER_SIDECAR_H
#define OPENORIENTEERING_SKETCH_LAYER_SIDECAR_H

#include <QString>

namespace OpenOrienteering {

class Map;

/**
 * Atomic local persistence for field sketches drawn over an immutable map.
 *
 * Only sketch-layer metadata and vector strokes are stored. The source map is
 * never copied or modified on disk.
 */
class SketchLayerSidecar
{
public:
	static QString pathForKey(const QString& storage_key);
	static bool load(Map& map, const QString& storage_key,
	                 QString* error = nullptr);
	static bool save(const Map& map, const QString& storage_key,
	                 QString* error = nullptr);
};

}  // namespace OpenOrienteering

#endif
