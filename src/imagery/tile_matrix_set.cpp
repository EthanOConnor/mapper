/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery/tile_matrix_set.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QRegularExpression>
#include <QtGlobal>

namespace OpenOrienteering::imagery {

namespace {

bool fail(QString* error, const QString& message)
{
	if (error)
		*error = message;
	return false;
}

bool finite(double value)
{
	return std::isfinite(value);
}

bool nearlyEqual(double first, double second)
{
	auto const scale = std::max({ 1.0, std::abs(first), std::abs(second) });
	return std::abs(first - second) <= scale * 1.0e-10;
}

bool normalizedEpsg(const QString& crs)
{
	static const QRegularExpression pattern(
		QStringLiteral("^EPSG:[1-9][0-9]{0,8}$")
	);
	return pattern.match(crs).hasMatch();
}

bool hasFiniteExtent(const TileMatrix& matrix)
{
	auto const tile_width =
		matrix.cell_size * double(matrix.tile_size.width());
	auto const tile_height =
		matrix.cell_size * double(matrix.tile_size.height());
	auto const full_width = tile_width * double(matrix.matrix_width);
	auto const full_height = tile_height * double(matrix.matrix_height);
	auto const east = matrix.point_of_origin.x() + full_width;
	auto const south = matrix.point_of_origin.y() - full_height;
	return finite(tile_width) && tile_width > 0
	       && finite(tile_height) && tile_height > 0
	       && finite(full_width) && full_width > 0
	       && finite(full_height) && full_height > 0
	       && finite(east) && east > matrix.point_of_origin.x()
	       && finite(south) && south < matrix.point_of_origin.y();
}

}  // namespace

bool CrsBounds::isValid() const noexcept
{
	return finite(west) && finite(south) && finite(east) && finite(north)
	       && west < east && south < north;
}

bool TileMatrix::contains(qint64 column, qint64 row) const noexcept
{
	return column >= 0 && row >= 0
	       && column < matrix_width && row < matrix_height;
}

CrsBounds TileMatrix::tileBounds(qint64 column, qint64 row) const noexcept
{
	if (!contains(column, row) || !finite(cell_size) || cell_size <= 0
	    || tile_size.width() <= 0 || tile_size.height() <= 0)
	{
		return {};
	}

	auto const tile_width = cell_size * double(tile_size.width());
	auto const tile_height = cell_size * double(tile_size.height());
	auto const west = point_of_origin.x() + double(column) * tile_width;
	auto const north = point_of_origin.y() - double(row) * tile_height;
	return { west, north - tile_height, west + tile_width, north };
}

bool TileMatrixLimits::contains(qint64 column, qint64 row) const noexcept
{
	return column >= min_column && column <= max_column
	       && row >= min_row && row <= max_row;
}

const TileMatrix* TileMatrixSet::matrixForZoom(int zoom) const noexcept
{
	if (zoom >= 0 && zoom < matrices.size() && matrices.at(zoom).zoom == zoom)
		return &matrices.at(zoom);
	for (auto const& matrix : matrices)
	{
		if (matrix.zoom == zoom)
			return &matrix;
	}
	return nullptr;
}

bool TileMatrixSet::validateDyadicTopLeft(QString* error) const
{
	if (id.trimmed().isEmpty())
		return fail(error, QStringLiteral("Tile matrix set ID is empty"));
	if (!normalizedEpsg(crs))
		return fail(error, QStringLiteral("Tile matrix set CRS must be a normalized EPSG code"));
	if (matrices.isEmpty())
		return fail(error, QStringLiteral("Tile matrix set has no matrices"));
	if (matrices.size() > 63)
		return fail(error, QStringLiteral("Tile matrix set exceeds the supported zoom count"));

	auto const& first = matrices.first();
	if (first.zoom != 0 || first.id != QLatin1String("0"))
		return fail(error, QStringLiteral("A dyadic tile matrix set must begin at zoom 0"));
	if (!finite(first.cell_size) || first.cell_size <= 0)
		return fail(error, QStringLiteral("Tile matrix cell size must be finite and positive"));
	if (!finite(first.point_of_origin.x()) || !finite(first.point_of_origin.y()))
		return fail(error, QStringLiteral("Tile matrix origin must be finite"));
	if (first.tile_size.width() <= 0 || first.tile_size.height() <= 0)
		return fail(error, QStringLiteral("Tile dimensions must be positive"));
	if (first.matrix_width <= 0 || first.matrix_height <= 0)
		return fail(error, QStringLiteral("Tile matrix dimensions must be positive"));

	for (int index = 0; index < matrices.size(); ++index)
	{
		auto const& matrix = matrices.at(index);
		if (matrix.zoom != index || matrix.id != QString::number(index))
			return fail(error, QStringLiteral("Dyadic matrix IDs must be contiguous decimal zooms"));
		if (!finite(matrix.cell_size) || matrix.cell_size <= 0
		    || !finite(matrix.point_of_origin.x()) || !finite(matrix.point_of_origin.y()))
		{
			return fail(error, QStringLiteral("Tile matrix geometry must be finite and positive"));
		}
		if (matrix.tile_size != first.tile_size)
			return fail(error, QStringLiteral("Dyadic matrices must use one tile size"));
		if (!nearlyEqual(matrix.point_of_origin.x(), first.point_of_origin.x())
		    || !nearlyEqual(matrix.point_of_origin.y(), first.point_of_origin.y()))
		{
			return fail(error, QStringLiteral("Dyadic matrices must use one top-left origin"));
		}
		if (matrix.matrix_width <= 0 || matrix.matrix_height <= 0)
			return fail(error, QStringLiteral("Tile matrix dimensions must be positive"));
		if (!hasFiniteExtent(matrix))
		{
			return fail(
				error,
				QStringLiteral(
					"Tile matrix spans and full extent must be finite "
					"and representable"
				)
			);
		}
		if (index == 0)
			continue;

		auto const& previous = matrices.at(index - 1);
		if (!nearlyEqual(matrix.cell_size * 2, previous.cell_size))
			return fail(error, QStringLiteral("Each matrix cell size must halve at the next zoom"));
		if (previous.matrix_width > std::numeric_limits<qint64>::max() / 2
		    || previous.matrix_height > std::numeric_limits<qint64>::max() / 2
		    || matrix.matrix_width != previous.matrix_width * 2
		    || matrix.matrix_height != previous.matrix_height * 2)
		{
			return fail(error, QStringLiteral("Each matrix dimension must double at the next zoom"));
		}
	}

	if (error)
		error->clear();
	return true;
}

TileMatrixSet TileMatrixSet::webMercatorQuad()
{
	constexpr auto max_zoom = 24;
	constexpr auto half_world = 20037508.342789244;
	constexpr auto tile_pixels = 256;
	constexpr auto base_cell_size = (2 * half_world) / tile_pixels;

	TileMatrixSet result;
	result.id = QStringLiteral("WebMercatorQuad");
	result.crs = QStringLiteral("EPSG:3857");
	result.matrices.reserve(max_zoom + 1);
	for (int zoom = 0; zoom <= max_zoom; ++zoom)
	{
		auto const dimension = qint64(1) << zoom;
		result.matrices.push_back({
			QString::number(zoom),
			zoom,
			base_cell_size / double(dimension),
			QPointF(-half_world, half_world),
			QSize(tile_pixels, tile_pixels),
			dimension,
			dimension,
		});
	}
	return result;
}

bool validateTileMatrixLimits(const QVector<TileMatrixLimits>& limits,
	                          const TileMatrixSet& matrix_set,
	                          QString* error)
{
	QVector<int> seen_zooms;
	seen_zooms.reserve(limits.size());
	for (auto const& limit : limits)
	{
		auto const* matrix = matrix_set.matrixForZoom(limit.zoom);
		if (!matrix)
			return fail(error, QStringLiteral("Tile limits refer to an unknown zoom"));
		if (seen_zooms.contains(limit.zoom))
			return fail(error, QStringLiteral("Tile limits contain a duplicate zoom"));
		seen_zooms.push_back(limit.zoom);
		if (limit.min_column < 0 || limit.min_row < 0
		    || limit.min_column > limit.max_column || limit.min_row > limit.max_row
		    || limit.max_column >= matrix->matrix_width
		    || limit.max_row >= matrix->matrix_height)
		{
			return fail(error, QStringLiteral("Tile limits fall outside their matrix"));
		}
	}
	if (error)
		error->clear();
	return true;
}

}  // namespace OpenOrienteering::imagery
