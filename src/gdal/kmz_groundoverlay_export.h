/*
 *    Copyright 2020-2021 Kai Pastor
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

#ifndef OPENORIENTEERING_KMZ_GROUNDOVERLAY_EXPORT_H
#define OPENORIENTEERING_KMZ_GROUNDOVERLAY_EXPORT_H

#include <vector>

#include <QtGlobal>
#include <QByteArray>
#include <QCoreApplication>
#include <QRectF>
#include <QSize>
#include <QSizeF>
#include <QString>

class QImage;
class QProgressDialog;
class QTransform;

// IWYU pragma: no_forward_declare QRectF

namespace OpenOrienteering {

class Map;
class MapPrinter;


/**
 * A class which generates KML/KMZ files with ground overlay raster tiles.
 * 
 * Apart from square tiles, a non-positive tile size makes the export create
 * a single raster image for the whole print area.
 * 
 * This class makes use of GDAL's virtual file systems for handling KMZ
 * (a zipped archive of a KML document and a set of image files).
 */
class KmzGroundOverlayExport
{
	Q_DECLARE_TR_FUNCTIONS(OpenOrienteering::KmzGroundOverlayExport)
	
	struct Tile
	{
		QByteArray name;
		QByteArray filepath;
		QRectF  rect_lonlat;
		QRectF  rect_map;
	};
	
	struct Metrics
	{
		QSize tile_size_px = {512, 512};
		qreal resolution_dpi = 300;
		qreal units_per_mm = resolution_dpi / 25.4;
		QSizeF tile_size_mm = QSizeF(tile_size_px) / units_per_mm;
	};
	
	static Metrics makeMetrics(const QSizeF& area_size, qreal resolution_dpi, int tile_width_px);
	
public:
	~KmzGroundOverlayExport();
	
	KmzGroundOverlayExport(const QString& path, const Map& map);
	
	void setProgressObserver(QProgressDialog* observer) noexcept;
	
	QString errorString() const;
	bool wasCanceled() const;
	
	bool doExport(MapPrinter& map_printer, int tile_width_px = 512);
	
	
protected:
	bool doExport(MapPrinter& map_printer, const Metrics& metrics, const std::vector<Tile>& tiles);
	
	std::vector<Tile> makeTiles(const QRectF& extent_map, const Metrics& metrics) const;

	/** Returns the actual overscanned map clip painted for a tile. */
	static QRectF renderClip(const Tile& tile) noexcept;

	/** Returns the union of all map clips painted by the export. */
	static QRectF renderExtent(const std::vector<Tile>& tiles) noexcept;
	
	void writeKml(QByteArray& buffer, const std::vector<Tile>& tiles) const;
	
	
	static QTransform makeTileTransform(const QRectF& tile_map, const Metrics& metrics, qreal declination);
	
	static void saveToBuffer(const QImage& image, QByteArray& data);
	
	static void writeToVSI(const QByteArray& filepath, const QByteArray& data);
	
	void mkdir(const QByteArray& path) const;
	
	
	void setMaximumProgress(int value);
	
	void setProgress(int value) const;

	bool beginStagedOutput(std::vector<Tile>& tiles);
	bool commitStagedOutput();
	void cleanupPartialOutput(const std::vector<Tile>& tiles);
	void pruneStagedOutputFiles();
	void pruneDirectAssetDirectories(
		const QByteArray& keep_relative_path = {});
	
private:
	const Map& map;
	QProgressDialog* progress_observer = nullptr;
	QByteArray output_filepath_utf8;
	QByteArray basepath_utf8;
	QByteArray doc_filepath_utf8;
	QByteArray staging_filepath_utf8;
	QByteArray direct_assets_relative_utf8;
	QString error_message;
	qreal overlap = 0.000000000001;
	int precision = 13;
	int tile_progress_maximum = 1;
	bool is_kmz = false;
	bool cancelled = false;
	bool files_directory_existed = false;
	
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_KMZ_GROUNDOVERLAY_EXPORT_H
