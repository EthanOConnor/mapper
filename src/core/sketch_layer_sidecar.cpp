/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "core/sketch_layer_sidecar.h"

#include <limits>

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

#include "core/map.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/sketch_layer.h"
#include "core/symbols/sketch_symbol.h"

namespace OpenOrienteering {

namespace {

constexpr auto schema_version = 1;
constexpr qint64 maximum_sidecar_bytes = 32 * 1024 * 1024;
constexpr int maximum_layers = 256;
constexpr int maximum_strokes = 200000;
constexpr int maximum_points_per_stroke = 200000;

QJsonArray coordinateJson(const PathObject& path)
{
	QJsonArray coordinates;
	for (MapCoordVector::size_type i = 0;
	     i < path.getCoordinateCount(); ++i)
	{
		const auto coord = path.getCoordinate(i);
		coordinates.append(QJsonArray{
		    coord.nativeX(), coord.nativeY(), int(coord.flags())});
	}
	return coordinates;
}

bool parseCoordinates(
        const QJsonArray& input, MapCoordVector& output)
{
	if (input.size() < 2 || input.size() > maximum_points_per_stroke)
		return false;
	output.reserve(std::size_t(input.size()));
	for (const auto value : input)
	{
		const auto coordinate = value.toArray();
		if (coordinate.size() != 3
		    || !coordinate[0].isDouble()
		    || !coordinate[1].isDouble()
		    || !coordinate[2].isDouble())
			return false;
		const auto x = coordinate[0].toInteger();
		const auto y = coordinate[1].toInteger();
		const auto flags = coordinate[2].toInt(-1);
		if (x < std::numeric_limits<qint32>::min()
		    || x > std::numeric_limits<qint32>::max()
		    || y < std::numeric_limits<qint32>::min()
		    || y > std::numeric_limits<qint32>::max()
		    || flags < 0 || flags > 0xff)
			return false;
		output.push_back(MapCoord::fromNative(
		    qint32(x), qint32(y), MapCoord::Flags(flags)));
	}
	return true;
}

SketchLayer::Width widthFromNative(int width)
{
	const auto fine = qRound(
	    1000 * SketchLayer::widthMillimeters(SketchLayer::Width::Fine));
	const auto broad = qRound(
	    1000 * SketchLayer::widthMillimeters(SketchLayer::Width::Broad));
	if (qAbs(width - fine) <= qAbs(width - broad)
	    && qAbs(width - fine)
	           <= qAbs(width - qRound(
	               1000 * SketchLayer::widthMillimeters(
	                          SketchLayer::Width::Medium))))
		return SketchLayer::Width::Fine;
	if (qAbs(width - broad)
	    < qAbs(width - qRound(
	        1000 * SketchLayer::widthMillimeters(
	                   SketchLayer::Width::Medium))))
		return SketchLayer::Width::Broad;
	return SketchLayer::Width::Medium;
}

}  // namespace

QString SketchLayerSidecar::pathForKey(const QString& storage_key)
{
	const auto digest = QCryptographicHash::hash(
	    storage_key.toUtf8(), QCryptographicHash::Sha256).toHex();
	auto root = qEnvironmentVariable("MAPPER_FIELD_SKETCH_ROOT");
	if (root.isEmpty())
		root = QStandardPaths::writableLocation(
		    QStandardPaths::AppDataLocation);
	return QDir(root).filePath(
	    QStringLiteral("field-sketches/%1.json")
	        .arg(QString::fromLatin1(digest)));
}

bool SketchLayerSidecar::load(
        Map& map, const QString& storage_key, QString* error)
{
	QFile file(pathForKey(storage_key));
	if (!file.exists())
		return true;
	if (!file.open(QIODevice::ReadOnly))
	{
		if (error)
			*error = file.errorString();
		return false;
	}
	if (file.size() > maximum_sidecar_bytes)
	{
		if (error)
			*error = QStringLiteral("The field-sketch file is too large.");
		return false;
	}
	QJsonParseError parse_error;
	const auto document =
	    QJsonDocument::fromJson(file.readAll(), &parse_error);
	if (parse_error.error != QJsonParseError::NoError
	    || !document.isObject())
	{
		if (error)
			*error = parse_error.errorString();
		return false;
	}
	const auto root = document.object();
	const auto layers = root.value(QStringLiteral("layers")).toArray();
	if (root.value(QStringLiteral("schema_version")).toInt()
	        != schema_version
	    || layers.size() > maximum_layers)
	{
		if (error)
			*error = QStringLiteral(
			    "The field-sketch file is unsupported.");
		return false;
	}

	int stroke_count = 0;
	for (const auto layer_value : layers)
	{
		const auto layer_json = layer_value.toObject();
		const auto layer_id =
		    layer_json.value(QStringLiteral("id")).toString();
		const auto owner_id =
		    layer_json.value(QStringLiteral("owner_id")).toString();
		const auto layer_uuid = QUuid(layer_id);
		if (layer_uuid.isNull()
		    || layer_uuid.toString(QUuid::WithoutBraces) != layer_id
		    || owner_id.isEmpty())
			continue;
		if (SketchLayer::findById(map, layer_id))
			continue;

		auto* layer = new MapPart(
		    layer_json.value(QStringLiteral("name")).toString(),
		    &map, layer_id);
		layer->setSketchLayerMetadata(
		    owner_id,
		    layer_json.value(QStringLiteral("owner_name")).toString(),
		    QDate::fromString(
		        layer_json.value(QStringLiteral("created_on")).toString(),
		        Qt::ISODate));
		map.addPart(layer, map.getNumParts());

		const auto strokes =
		    layer_json.value(QStringLiteral("strokes")).toArray();
		stroke_count += strokes.size();
		if (stroke_count > maximum_strokes)
		{
			if (error)
				*error = QStringLiteral(
				    "The field-sketch file contains too many strokes.");
			return false;
		}
		for (const auto stroke_value : strokes)
		{
			const auto stroke_json = stroke_value.toObject();
			const QColor color(
			    stroke_json.value(QStringLiteral("color")).toString());
			const auto width = widthFromNative(
			    stroke_json.value(QStringLiteral("width")).toInt());
			MapCoordVector coordinates;
			if (!color.isValid()
			    || !parseCoordinates(
			        stroke_json.value(QStringLiteral("coordinates"))
			            .toArray(),
			        coordinates))
				continue;
			auto* symbol =
			    SketchLayer::ensureSymbol(map, color, width);
			map.addObject(
			    new PathObject(symbol, std::move(coordinates), &map),
			    map.findPartIndex(layer));
		}
	}
	return true;
}

bool SketchLayerSidecar::save(
        const Map& map, const QString& storage_key, QString* error)
{
	QJsonArray layers;
	for (const auto* layer : SketchLayer::all(map))
	{
		if (!layer->hasSketchLayerMetadata())
			continue;
		QJsonArray strokes;
		for (int i = 0; i < layer->getNumObjects(); ++i)
		{
			const auto* path =
			    dynamic_cast<const PathObject*>(layer->getObject(i));
			const auto* symbol = path
			                   ? dynamic_cast<const SketchSymbol*>(
			                         path->getSymbol())
			                   : nullptr;
			if (!path || !symbol)
				continue;
			strokes.append(QJsonObject{
			    {QStringLiteral("color"),
			     symbol->strokeColor().name(QColor::HexArgb)},
			    {QStringLiteral("width"), symbol->getLineWidth()},
			    {QStringLiteral("coordinates"), coordinateJson(*path)},
			});
		}
		layers.append(QJsonObject{
		    {QStringLiteral("id"), layer->persistentId()},
		    {QStringLiteral("name"), layer->getName()},
		    {QStringLiteral("owner_id"), layer->sketchOwnerId()},
		    {QStringLiteral("owner_name"), layer->sketchOwnerName()},
		    {QStringLiteral("created_on"),
		     layer->sketchCreatedOn().toString(Qt::ISODate)},
		    {QStringLiteral("strokes"), strokes},
		});
	}

	const auto path = pathForKey(storage_key);
	if (!QDir().mkpath(QFileInfo(path).absolutePath()))
	{
		if (error)
			*error = QStringLiteral(
			    "Cannot create the field-sketch directory.");
		return false;
	}
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly))
	{
		if (error)
			*error = file.errorString();
		return false;
	}
	file.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
	const auto bytes = QJsonDocument(QJsonObject{
	    {QStringLiteral("schema_version"), schema_version},
	    {QStringLiteral("layers"), layers},
	}).toJson(QJsonDocument::Compact);
	if (bytes.size() > maximum_sidecar_bytes
	    || file.write(bytes) != bytes.size() || !file.commit())
	{
		if (error)
			*error = bytes.size() > maximum_sidecar_bytes
			         ? QStringLiteral("The field-sketch file is too large.")
			         : file.errorString();
		return false;
	}
	return true;
}

}  // namespace OpenOrienteering
