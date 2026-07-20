/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 */

#include <algorithm>
#include <array>
#include <cmath>

#include <QApplication>
#include <QCommandLineOption>
#include <QCommandLineParser>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QSaveFile>
#include <QTextStream>

#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map.h"
#include "core/map_coord.h"
#include "core/map_printer.h"
#include "core/renderables/renderable.h"
#include "global.h"
#include "mapper_config.h"
#include "render/qpainter_renderer.h"
#include "render/qt_render_bridge.h"

using namespace OpenOrienteering;

namespace {

constexpr int maximum_image_dimension = 16384;
constexpr qint64 maximum_image_pixels = 64LL * 1024 * 1024;

int fail(const QString& message, int code)
{
	QTextStream(stderr) << message << Qt::endl;
	return code;
}

QByteArray sha256File(const QString& path, bool* ok)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
	{
		*ok = false;
		return {};
	}
	QCryptographicHash hash(QCryptographicHash::Sha256);
	if (!hash.addData(&file))
	{
		*ok = false;
		return {};
	}
	*ok = true;
	return hash.result().toHex();
}

QJsonArray coordinateJson(const LatLon& coordinate)
{
	return {coordinate.longitude(), coordinate.latitude()};
}

}  // namespace

int main(int argc, char** argv)
{
	// The renderer is a server-side companion tool and must work without a
	// window system. An explicitly configured Qt platform still wins.
	if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
		qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
	if (qEnvironmentVariableIsEmpty("QT_LOGGING_RULES"))
		qputenv("QT_LOGGING_RULES", QByteArrayLiteral("*.debug=false;*.info=false"));

	QApplication app(argc, argv);
	QCoreApplication::setApplicationName(QStringLiteral("mapper-map-render"));
	QCoreApplication::setApplicationVersion(QStringLiteral(APP_VERSION));
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();

	QCommandLineParser parser;
	parser.setApplicationDescription(
		QStringLiteral("Render a georeferenced OMAP or OCAD map for a Map Hub public snapshot."));
	parser.addHelpOption();
	parser.addVersionOption();
	QCommandLineOption max_dimension_option(
		{QStringLiteral("m"), QStringLiteral("max-dimension")},
		QStringLiteral("Maximum output width or height in pixels (256-16384)."),
		QStringLiteral("pixels"),
		QStringLiteral("4096"));
	QCommandLineOption pixels_per_mm_option(
		{QStringLiteral("p"), QStringLiteral("pixels-per-mm")},
		QStringLiteral("Fixed render resolution. Overrides --max-dimension."),
		QStringLiteral("resolution"));
	parser.addOption(max_dimension_option);
	parser.addOption(pixels_per_mm_option);
	parser.addPositionalArgument(QStringLiteral("input"), QStringLiteral("Input .omap or .ocd file."));
	parser.addPositionalArgument(QStringLiteral("image"), QStringLiteral("Output PNG image."));
	parser.addPositionalArgument(QStringLiteral("manifest"), QStringLiteral("Output JSON placement manifest."));
	parser.process(app);

	auto const arguments = parser.positionalArguments();
	if (arguments.size() != 3)
	{
		parser.showHelp(2);
		return 2;
	}
	auto const input_path = arguments[0];
	auto const image_path = arguments[1];
	auto const manifest_path = arguments[2];
	auto const suffix = QFileInfo(input_path).suffix().toLower();
	if (suffix != QLatin1String("omap") && suffix != QLatin1String("ocd"))
		return fail(QStringLiteral("Input must be an .omap or .ocd map."), 2);
	if (QFileInfo(image_path).suffix().compare(QLatin1String("png"), Qt::CaseInsensitive) != 0)
		return fail(QStringLiteral("Image output must use the .png extension."), 2);
	if (QFileInfo(manifest_path).suffix().compare(QLatin1String("json"), Qt::CaseInsensitive) != 0)
		return fail(QStringLiteral("Manifest output must use the .json extension."), 2);

	bool valid_number = false;
	auto const max_dimension = parser.value(max_dimension_option).toInt(&valid_number);
	if (!valid_number || max_dimension < 256 || max_dimension > maximum_image_dimension)
		return fail(QStringLiteral("--max-dimension must be between 256 and 16384."), 2);

	Map map;
	if (!map.loadFrom(input_path))
		return fail(QStringLiteral("The map could not be loaded."), 3);
	auto const& georeferencing = map.getGeoreferencing();
	if (georeferencing.getState() != Georeferencing::Geospatial)
		return fail(QStringLiteral("The map must have usable georeferencing before it can be published."), 4);

	// The saved print area is the public product boundary. If the author has
	// never configured printing, Map::printerConfig() deliberately defaults it
	// to the map extent.
	auto const extent = map.printerConfig().print_area;
	if (!extent.isValid() || extent.isEmpty())
		return fail(QStringLiteral("The map contains no renderable map objects."), 5);

	double pixels_per_mm = 0;
	if (parser.isSet(pixels_per_mm_option))
	{
		pixels_per_mm = parser.value(pixels_per_mm_option).toDouble(&valid_number);
		if (!valid_number || !std::isfinite(pixels_per_mm) || pixels_per_mm <= 0)
			return fail(QStringLiteral("--pixels-per-mm must be a positive number."), 2);
	}
	else
	{
		pixels_per_mm = max_dimension / std::max(extent.width(), extent.height());
	}
	auto const pixel_size = QSize(
		std::max(1, qCeil(extent.width() * pixels_per_mm)),
		std::max(1, qCeil(extent.height() * pixels_per_mm)));
	if (pixel_size.width() > maximum_image_dimension || pixel_size.height() > maximum_image_dimension
	    || qint64(pixel_size.width()) * pixel_size.height() > maximum_image_pixels)
	{
		return fail(QStringLiteral("The requested render exceeds the image safety limit."), 6);
	}

	std::array<MapCoordF, 4> const map_corners = {
		MapCoordF(extent.topLeft()),
		MapCoordF(extent.topRight()),
		MapCoordF(extent.bottomRight()),
		MapCoordF(extent.bottomLeft()),
	};
	QJsonArray geographic_corners;
	for (auto const& corner : map_corners)
	{
		bool converted = false;
		auto const coordinate = georeferencing.toGeographicCoords(corner, &converted);
		if (!converted || !std::isfinite(coordinate.latitude()) || !std::isfinite(coordinate.longitude())
		    || std::abs(coordinate.latitude()) > 90 || std::abs(coordinate.longitude()) > 180)
		{
			return fail(QStringLiteral("The rendered map extent could not be transformed to WGS84."), 4);
		}
		geographic_corners.append(coordinateJson(coordinate));
	}

	QImage image(pixel_size, QImage::Format_ARGB32_Premultiplied);
	if (image.isNull())
		return fail(QStringLiteral("The output image could not be allocated."), 6);
	image.fill(Qt::white);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.scale(pixels_per_mm, pixels_per_mm);
	painter.translate(-extent.topLeft());
	painter.setClipRect(extent);
	auto const snapshot = map.publishRenderSnapshot();
	render::QPainterRenderer().draw(painter, *snapshot, {
		render::fromQRectF(extent),
		pixels_per_mm,
		RenderConfig::NoOptions,
		1,
	});
	painter.end();

	QSaveFile image_file(image_path);
	if (!image_file.open(QIODevice::WriteOnly) || !image.save(&image_file, "PNG") || !image_file.commit())
		return fail(QStringLiteral("The PNG output could not be written atomically."), 8);

	bool hash_ok = false;
	auto const source_sha256 = sha256File(input_path, &hash_ok);
	if (!hash_ok)
		return fail(QStringLiteral("The input map could not be hashed."), 8);
	auto const image_sha256 = sha256File(image_path, &hash_ok);
	if (!hash_ok)
		return fail(QStringLiteral("The output image could not be hashed."), 8);

	QJsonObject manifest{
		{QStringLiteral("schema"), QStringLiteral("org.openorienteering.mapper.map-render/v1")},
		{QStringLiteral("renderer"), QStringLiteral("OpenOrienteering Mapper %1").arg(QStringLiteral(APP_VERSION))},
		{QStringLiteral("source_name"), QFileInfo(input_path).fileName()},
		{QStringLiteral("source_sha256"), QString::fromLatin1(source_sha256)},
		{QStringLiteral("image_name"), QFileInfo(image_path).fileName()},
		{QStringLiteral("image_sha256"), QString::fromLatin1(image_sha256)},
		{QStringLiteral("width"), pixel_size.width()},
		{QStringLiteral("height"), pixel_size.height()},
		{QStringLiteral("pixels_per_mm"), pixels_per_mm},
		{QStringLiteral("coordinates"), geographic_corners},
		{QStringLiteral("map_extent"), QJsonArray{extent.left(), extent.top(), extent.right(), extent.bottom()}},
		{QStringLiteral("extent_kind"), QStringLiteral("print_area")},
		{QStringLiteral("background"), QStringLiteral("white")},
		{QStringLiteral("helper_symbols"), false},
		{QStringLiteral("templates"), false},
	};
	QSaveFile manifest_file(manifest_path);
	if (!manifest_file.open(QIODevice::WriteOnly)
	    || manifest_file.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented)) < 0
	    || !manifest_file.commit())
	{
		return fail(QStringLiteral("The JSON manifest could not be written atomically."), 8);
	}
	return 0;
}
