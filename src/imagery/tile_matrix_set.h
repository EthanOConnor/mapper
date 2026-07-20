/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    any later version.
 */

#ifndef OPENORIENTEERING_IMAGERY_TILE_MATRIX_SET_H
#define OPENORIENTEERING_IMAGERY_TILE_MATRIX_SET_H

#include <QPointF>
#include <QSize>
#include <QString>
#include <QVector>

namespace OpenOrienteering::imagery {

struct CrsBounds
{
	double west = 0;
	double south = 0;
	double east = 0;
	double north = 0;

	bool isValid() const noexcept;
	bool operator==(const CrsBounds&) const = default;
};

/**
 * One top-left-origin matrix in a dyadic tile pyramid.
 *
 * Rows are always represented in canonical top-to-bottom order. A source's
 * XYZ/TMS row convention is applied only when expanding a request URL.
 */
struct TileMatrix
{
	QString id;
	int zoom = -1;
	double cell_size = 0;
	QPointF point_of_origin;
	QSize tile_size;
	qint64 matrix_width = 0;
	qint64 matrix_height = 0;

	bool contains(qint64 column, qint64 row) const noexcept;
	CrsBounds tileBounds(qint64 column, qint64 row) const noexcept;
	bool operator==(const TileMatrix&) const = default;
};

struct TileMatrixLimits
{
	int zoom = -1;
	qint64 min_column = 0;
	qint64 max_column = -1;
	qint64 min_row = 0;
	qint64 max_row = -1;

	bool contains(qint64 column, qint64 row) const noexcept;
	bool operator==(const TileMatrixLimits&) const = default;
};

struct TileMatrixSet
{
	QString id;
	QString crs;
	QVector<TileMatrix> matrices;

	const TileMatrix* matrixForZoom(int zoom) const noexcept;
	bool validateDyadicTopLeft(QString* error = nullptr) const;

	static TileMatrixSet webMercatorQuad();

	bool operator==(const TileMatrixSet&) const = default;
};

bool validateTileMatrixLimits(const QVector<TileMatrixLimits>& limits,
	                          const TileMatrixSet& matrix_set,
	                          QString* error = nullptr);

}  // namespace OpenOrienteering::imagery

#endif
