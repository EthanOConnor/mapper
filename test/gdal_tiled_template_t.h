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

#ifndef OPENORIENTEERING_GDAL_TILED_TEMPLATE_T_H
#define OPENORIENTEERING_GDAL_TILED_TEMPLATE_T_H

#include <QObject>
#include <QString>

/**
 * Tests for viewport-aware tiled raster loading in GdalTemplate.
 *
 * These tests verify that GDAL sources with a native tile/block structure
 * (WMS, MBTiles, COG) are loaded on-demand rather than fully into memory,
 * and that the existing non-tiled path is unaffected.
 */
class GdalTiledTemplateTest : public QObject
{
Q_OBJECT
public:
	explicit GdalTiledTemplateTest(QObject* parent = nullptr);

private slots:
	void initTestCase();
	void cleanupTestCase();

	/**
	 * Verify that readRasterInfo() detects block_size for tiled sources
	 * and leaves it empty for non-tiled sources.
	 */
	void blockSizeDetection();

	/**
	 * Verify that a non-tiled GeoTIFF loads via the existing full-image path.
	 */
	void nonTiledPathUnchanged();

	/**
	 * Verify that a tiled source is detected and the full raster is NOT
	 * loaded into memory.
	 */
	void tiledSourceDetected();

	/**
	 * Verify that tiles arrive asynchronously and populate the cache.
	 */
	void asyncTileLoading();

	/**
	 * Verify that unloading a tiled template cleans up the worker thread
	 * and GDAL dataset handle.
	 */
	void unloadCleanup();

	/**
	 * Verify that getTemplateExtent() returns the full raster extent
	 * for tiled sources.
	 */
	void tiledTemplateExtent();

private:
	/**
	 * Create a tiled GeoTIFF in GDAL's virtual filesystem for testing.
	 * Returns the /vsimem/ path.
	 */
	QString createTiledTestRaster(int width, int height, int block_w, int block_h);

	/**
	 * Create a non-tiled (striped) GeoTIFF in GDAL's virtual filesystem.
	 * Returns the /vsimem/ path.
	 */
	QString createNonTiledTestRaster(int width, int height);

	int test_counter = 0;
};

#endif // OPENORIENTEERING_GDAL_TILED_TEMPLATE_T_H
