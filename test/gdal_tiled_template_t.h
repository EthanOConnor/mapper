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

#include <array>

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

	/**
	 * Verify that the tiled renderer interprets clip_rect in map coordinates,
	 * independent of the painter's current view transform.
	 */
	void tiledDrawUsesMapClipRect();

	/**
	 * Verify that tiled and non-tiled GDAL rasters with the same geotransform
	 * produce the same template placement, including across CRS conversion.
	 */
	void tiledGeoreferencingMatchesNonTiled();

	/**
	 * Verify that a georeferenced tiled raster recomputes its placement when
	 * the map becomes geospatial after the template has already loaded.
	 */
	void tiledTemplateUpdatesAfterMapBecomesGeospatial();

	/**
	 * Verify that a zoomed-out draw requests subsampled tile images rather than
	 * always caching full native blocks.
	 */
	void zoomedOutDrawUsesSubsampledTiles();

	/**
	 * Verify that a zoomed-out draw schedules and caches larger logical
	 * macrotiles rather than the native full-resolution block grid.
	 */
	void zoomedOutDrawUsesMacroTileGrid();

	/**
	 * Verify that a cropped tiled source keeps coarse logical tiles aligned
	 * to the native TMS tile matrix origin, including partial edge tiles.
	 */
	void croppedTmsOriginKeepsCoarseGridAligned();

	/**
	 * Verify that switching to a higher-resolution zoom level drops queued
	 * requests from the previous subsampling level.
	 */
	void zoomChangeDropsQueuedLowerResolutionRequests();

	/**
	 * Verify that a generation change keeps relevant in-flight requests
	 * deduplicated while dropping stale in-flight keys.
	 */
	void generationChangeKeepsRelevantInFlightTilesAndDropsStaleOnes();

	/**
	 * Verify that re-requesting an already queued tile promotes it to the
	 * front so newly visible tiles are not stranded behind older overscan work.
	 */
	void requeuedTileMovesToFront();

	/**
	 * Verify that an overscan request yields when visible tiles are still
	 * queued, so workers stay focused on filling the viewport first.
	 */
	void overscanRequestYieldsToQueuedVisibleTiles();

	/**
	 * Verify that overview selection uses on-screen pixel scale rather than
	 * raw map zoom, avoiding overly coarse level choices.
	 */
	void onScreenScaleChoosesSharperLevel();

	/**
	 * Verify that a zoomed-out tiled render stays vertically aligned with the
	 * non-tiled path when overview rendering kicks in.
	 */
	void zoomedOutTiledRenderMatchesNonTiledPosition();

	/**
	 * Verify that the overview threshold is biased slightly toward coarser
	 * levels so near-1:1 draws do not over-request native tiles.
	 */
	void overviewThresholdSlightlyPrefersCoarserLevel();

	/**
	 * Verify that visible tiles are queued ahead of overscan tiles so the
	 * current viewport fills before speculative border loading.
	 */
	void visibleTilesArePrioritizedAheadOfOverscan();

	/**
	 * Verify that byte-budget eviction removes the least-recently-used cache
	 * entry rather than scanning for or dropping a recently touched tile.
	 */
	void cacheEvictsLeastRecentlyUsedTile();

	/**
	 * Verify that cropping from a coarser cached edge tile uses the actual
	 * cached image size, avoiding visible jumps when overview levels switch.
	 */
	void coarserFallbackUsesActualCachedImageScale();

	/**
	 * Verify that a visible unloaded tiled template gets scheduled for loading
	 * when the map widget redraws template caches after reopen.
	 */
	void visibleUnloadedTiledTemplateLoadsOnPaint();

private:
	/**
	 * Create a tiled GeoTIFF in GDAL's virtual filesystem for testing.
	 * Returns the /vsimem/ path.
	 */
	QString createTiledTestRaster(
		int width,
		int height,
		int block_w,
		int block_h,
		int epsg_code = 32632,
		const std::array<double, 6>& geotransform = { 500000.0, 1.0, 0.0, 6000000.0, 0.0, -1.0 }
	);

	/**
	 * Create a non-tiled (striped) GeoTIFF in GDAL's virtual filesystem.
	 * Returns the /vsimem/ path.
	 */
	QString createNonTiledTestRaster(
		int width,
		int height,
		int epsg_code = 32632,
		const std::array<double, 6>& geotransform = { 500000.0, 1.0, 0.0, 6000000.0, 0.0, -1.0 }
	);

	int test_counter = 0;
};

#endif // OPENORIENTEERING_GDAL_TILED_TEMPLATE_T_H
