/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery/imagery_source_snapshot.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSet>

namespace OpenOrienteering::imagery {

namespace {

bool fail(QString* error, const QString& message)
{
	if (error)
		*error = message;
	return false;
}

template<typename Result>
std::optional<Result> failOptional(QString* error, const QString& message)
{
	fail(error, message);
	return std::nullopt;
}

bool hasOnlyKeys(const QJsonObject& object,
	             std::initializer_list<QString> allowed,
	             const QString& path,
	             QString* error)
{
	QSet<QString> keys(allowed.begin(), allowed.end());
	for (auto it = object.begin(); it != object.end(); ++it)
	{
		if (!keys.contains(it.key()))
			return fail(error, QStringLiteral("%1 contains unknown member %2").arg(path, it.key()));
	}
	return true;
}

bool requiredObject(const QJsonObject& parent,
	                const QString& name,
	                QJsonObject* output,
	                const QString& path,
	                QString* error)
{
	auto const value = parent.value(name);
	if (!value.isObject())
		return fail(error, QStringLiteral("%1.%2 must be an object").arg(path, name));
	*output = value.toObject();
	return true;
}

bool requiredArray(const QJsonObject& parent,
	               const QString& name,
	               QJsonArray* output,
	               const QString& path,
	               QString* error)
{
	auto const value = parent.value(name);
	if (!value.isArray())
		return fail(error, QStringLiteral("%1.%2 must be an array").arg(path, name));
	*output = value.toArray();
	return true;
}

bool requiredString(const QJsonObject& parent,
	                const QString& name,
	                QString* output,
	                const QString& path,
	                QString* error)
{
	auto const value = parent.value(name);
	if (!value.isString())
		return fail(error, QStringLiteral("%1.%2 must be a string").arg(path, name));
	*output = value.toString();
	return true;
}

bool integerValue(const QJsonValue& value,
	              qint64 minimum,
	              qint64 maximum,
	              qint64* output)
{
	if (!value.isDouble())
		return false;
	auto const number = value.toDouble();
	if (!std::isfinite(number) || std::floor(number) != number
	    || number < double(minimum) || number > double(maximum))
	{
		return false;
	}
	*output = qint64(number);
	return true;
}

bool requiredInteger(const QJsonObject& parent,
	                 const QString& name,
	                 qint64 minimum,
	                 qint64 maximum,
	                 qint64* output,
	                 const QString& path,
	                 QString* error)
{
	if (!integerValue(parent.value(name), minimum, maximum, output))
	{
		return fail(error, QStringLiteral("%1.%2 must be an integer in range").arg(path, name));
	}
	return true;
}

bool requiredFinite(const QJsonObject& parent,
	                const QString& name,
	                double* output,
	                const QString& path,
	                QString* error)
{
	auto const value = parent.value(name);
	if (!value.isDouble() || !std::isfinite(value.toDouble()))
		return fail(error, QStringLiteral("%1.%2 must be finite").arg(path, name));
	*output = value.toDouble();
	return true;
}

void insertIfNotEmpty(QJsonObject& object, const QString& name, const QString& value)
{
	if (!value.isEmpty())
		object.insert(name, value);
}

void insertIfValid(QJsonObject& object, const QString& name, const QDate& value)
{
	if (value.isValid())
		object.insert(name, value.toString(Qt::ISODate));
}

void insertIfValid(QJsonObject& object, const QString& name, const QUrl& value)
{
	if (!value.isEmpty())
		object.insert(name, value.toString(QUrl::FullyEncoded));
}

QJsonObject metadataObject(const ImageryMetadata& metadata)
{
	QJsonObject object {
		{ QStringLiteral("category"), categoryName(metadata.category) },
		{ QStringLiteral("id"), metadata.id },
		{ QStringLiteral("name"), metadata.name },
	};
	insertIfNotEmpty(object, QStringLiteral("description"), metadata.description);
	insertIfValid(object, QStringLiteral("startDate"), metadata.start_date);
	insertIfValid(object, QStringLiteral("endDate"), metadata.end_date);
	return object;
}

QJsonObject noticesObject(const ImageryNotices& notices)
{
	QJsonObject object;
	insertIfNotEmpty(object, QStringLiteral("attributionText"), notices.attribution_text);
	insertIfValid(object, QStringLiteral("attributionUrl"), notices.attribution_url);
	insertIfValid(object, QStringLiteral("sourceUrl"), notices.source_url);
	insertIfValid(object, QStringLiteral("termsUrl"), notices.terms_url);
	insertIfValid(object, QStringLiteral("privacyUrl"), notices.privacy_url);
	insertIfNotEmpty(object, QStringLiteral("notes"), notices.notes);
	return object;
}

QJsonObject requestObject(const ImageryRequestPolicy& request)
{
	QJsonObject object;
	insertIfValid(object, QStringLiteral("referer"), request.referer);
	auto codes = request.empty_http_status_codes;
	std::sort(codes.begin(), codes.end());
	QJsonArray array;
	for (auto const code : codes)
		array.push_back(code);
	object.insert(QStringLiteral("emptyHttpStatusCodes"), array);
	return object;
}

QJsonValue catalogProvenanceValue(
	const std::optional<CatalogSourceProvenance>& provenance)
{
	if (!provenance)
		return QJsonValue(QJsonValue::Null);
	auto const& value = *provenance;
	return QJsonObject {
		{ QStringLiteral("catalogId"), value.catalog_id },
		{ QStringLiteral("catalogRevision"), value.catalog_revision },
		{ QStringLiteral("catalogSha256"), QString::fromLatin1(value.catalog_sha256) },
		{ QStringLiteral("fullFingerprint"), QString::fromLatin1(value.full_fingerprint) },
		{ QStringLiteral("operationalFingerprint"), QString::fromLatin1(value.operational_fingerprint) },
		{ QStringLiteral("sourceId"), value.source_id },
	};
}

QJsonObject provenanceObject(const ImageryProvenance& provenance)
{
	QJsonObject object;
	insertIfNotEmpty(object, QStringLiteral("method"), provenance.method);
	insertIfValid(object, QStringLiteral("observed"), provenance.observed);
	insertIfNotEmpty(object, QStringLiteral("author"), provenance.author);
	if (provenance.rms_error)
		object.insert(QStringLiteral("rmsError"), *provenance.rms_error);
	insertIfNotEmpty(object, QStringLiteral("notes"), provenance.notes);
	return object;
}

QJsonValue registrationValue(const std::optional<TranslationRegistration>& registration)
{
	if (!registration)
		return QJsonValue(QJsonValue::Null);
	auto const& value = *registration;
	QJsonObject target_frame {
		{ QStringLiteral("crs"), value.target_crs },
	};
	insertIfNotEmpty(target_frame, QStringLiteral("id"), value.target_frame_id);
	return QJsonObject {
		{ QStringLiteral("direction"), QStringLiteral("source-to-corrected") },
		{ QStringLiteral("operation"), QJsonObject {
			{ QStringLiteral("dx"), value.dx },
			{ QStringLiteral("dy"), value.dy },
			{ QStringLiteral("type"), QStringLiteral("translation2d") },
			{ QStringLiteral("unit"), QStringLiteral("crs") },
		} },
		{ QStringLiteral("provenance"), provenanceObject(value.provenance) },
		{ QStringLiteral("sourceFrame"), QJsonObject {
			{ QStringLiteral("crs"), value.source_crs },
		} },
		{ QStringLiteral("targetFrame"), target_frame },
	};
}

QJsonObject matrixSetObject(const TileMatrixSet& matrix_set)
{
	QJsonArray matrices;
	for (auto const& matrix : matrix_set.matrices)
	{
		matrices.push_back(QJsonObject {
			{ QStringLiteral("cellSize"), matrix.cell_size },
			{ QStringLiteral("id"), matrix.id },
			{ QStringLiteral("matrixHeight"), double(matrix.matrix_height) },
			{ QStringLiteral("matrixWidth"), double(matrix.matrix_width) },
			{ QStringLiteral("pointOfOrigin"), QJsonArray {
				matrix.point_of_origin.x(), matrix.point_of_origin.y()
			} },
			{ QStringLiteral("tileHeight"), matrix.tile_size.height() },
			{ QStringLiteral("tileWidth"), matrix.tile_size.width() },
			{ QStringLiteral("zoom"), matrix.zoom },
		});
	}
	return {
		{ QStringLiteral("crs"), matrix_set.crs },
		{ QStringLiteral("id"), matrix_set.id },
		{ QStringLiteral("matrices"), matrices },
	};
}

QJsonArray limitsArray(QVector<TileMatrixLimits> limits)
{
	std::sort(limits.begin(), limits.end(), [](auto const& first, auto const& second) {
		return first.zoom < second.zoom;
	});
	QJsonArray result;
	for (auto const& limit : limits)
	{
		result.push_back(QJsonObject {
			{ QStringLiteral("maxColumn"), double(limit.max_column) },
			{ QStringLiteral("maxRow"), double(limit.max_row) },
			{ QStringLiteral("minColumn"), double(limit.min_column) },
			{ QStringLiteral("minRow"), double(limit.min_row) },
			{ QStringLiteral("zoom"), limit.zoom },
		});
	}
	return result;
}

QJsonObject sourceObject(const ResolvedImagerySource& source)
{
	QJsonArray tiles;
	for (auto const& url : source.tile_urls)
		tiles.push_back(url.value);
	return {
		{ QStringLiteral("catalogProvenance"), catalogProvenanceValue(source.catalog_provenance) },
		{ QStringLiteral("format"), ImagerySourceSnapshotCodec::formatIdentifier() },
		{ QStringLiteral("limits"), limitsArray(source.tile_limits) },
		{ QStringLiteral("maxZoom"), source.max_zoom },
		{ QStringLiteral("mediaType"), source.media_type },
		{ QStringLiteral("metadata"), metadataObject(source.metadata) },
		{ QStringLiteral("minZoom"), source.min_zoom },
		{ QStringLiteral("notices"), noticesObject(source.notices) },
		{ QStringLiteral("registration"), registrationValue(source.registration) },
		{ QStringLiteral("request"), requestObject(source.request) },
		{ QStringLiteral("scheme"), tileRowSchemeName(source.row_scheme) },
		{ QStringLiteral("tileMatrixSet"), matrixSetObject(source.tile_matrix_set) },
		{ QStringLiteral("tiles"), tiles },
		{ QStringLiteral("version"), ImagerySourceSnapshotCodec::version },
	};
}

QByteArray scalarJson(const QJsonValue& value)
{
	auto encoded = QJsonDocument(QJsonArray { value }).toJson(QJsonDocument::Compact);
	return encoded.mid(1, encoded.size() - 2);
}

bool appendDeterministicJson(const QJsonValue& value, QByteArray& output, QString* error)
{
	switch (value.type())
	{
	case QJsonValue::Null:
		output += "null";
		return true;
	case QJsonValue::Bool:
		output += value.toBool() ? "true" : "false";
		return true;
	case QJsonValue::Double:
	{
		auto const number = value.toDouble();
		if (!std::isfinite(number))
			return fail(error, QStringLiteral("Snapshot contains a nonfinite number"));
		if (number == 0)
			output += '0';
		else
			output += scalarJson(value);
		return true;
	}
	case QJsonValue::String:
		output += scalarJson(value);
		return true;
	case QJsonValue::Array:
	{
		output += '[';
		auto const array = value.toArray();
		for (int index = 0; index < array.size(); ++index)
		{
			if (index)
				output += ',';
			if (!appendDeterministicJson(array.at(index), output, error))
				return false;
		}
		output += ']';
		return true;
	}
	case QJsonValue::Object:
	{
		output += '{';
		auto const object = value.toObject();
		auto keys = object.keys();
		std::sort(keys.begin(), keys.end());
		for (int index = 0; index < keys.size(); ++index)
		{
			if (index)
				output += ',';
			output += scalarJson(keys.at(index));
			output += ':';
			if (!appendDeterministicJson(object.value(keys.at(index)), output, error))
				return false;
		}
		output += '}';
		return true;
	}
	case QJsonValue::Undefined:
		return fail(error, QStringLiteral("Snapshot contains an undefined JSON value"));
	}
	return fail(error, QStringLiteral("Snapshot contains an unknown JSON value"));
}

QByteArray encodeSource(const ResolvedImagerySource& source, QString* error)
{
	QByteArray output;
	if (!appendDeterministicJson(sourceObject(source), output, error))
		output.clear();
	return output;
}

bool optionalString(const QJsonObject& object,
	                const QString& name,
	                QString* output,
	                const QString& path,
	                QString* error)
{
	auto const value = object.value(name);
	if (value.isUndefined())
		return true;
	if (!value.isString())
		return fail(error, QStringLiteral("%1.%2 must be a string").arg(path, name));
	*output = value.toString();
	return true;
}

bool optionalDate(const QJsonObject& object,
	              const QString& name,
	              QDate* output,
	              const QString& path,
	              QString* error)
{
	auto const value = object.value(name);
	if (value.isUndefined())
		return true;
	if (!value.isString())
		return fail(error, QStringLiteral("%1.%2 must be an ISO date").arg(path, name));
	auto const date = QDate::fromString(value.toString(), Qt::ISODate);
	if (!date.isValid())
		return fail(error, QStringLiteral("%1.%2 must be an ISO date").arg(path, name));
	*output = date;
	return true;
}

bool optionalUrl(const QJsonObject& object,
	             const QString& name,
	             QUrl* output,
	             const QString& path,
	             QString* error)
{
	auto const value = object.value(name);
	if (value.isUndefined())
		return true;
	if (!value.isString())
		return fail(error, QStringLiteral("%1.%2 must be a URL string").arg(path, name));
	*output = QUrl(value.toString(), QUrl::StrictMode);
	return true;
}

bool decodeMetadata(const QJsonObject& object, ImageryMetadata* metadata, QString* error)
{
	auto const path = QStringLiteral("$.metadata");
	if (!hasOnlyKeys(object, {
		QStringLiteral("id"), QStringLiteral("name"), QStringLiteral("description"),
		QStringLiteral("category"), QStringLiteral("startDate"), QStringLiteral("endDate")
	}, path, error))
	{
		return false;
	}
	QString category;
	if (!requiredString(object, QStringLiteral("id"), &metadata->id, path, error)
	    || !requiredString(object, QStringLiteral("name"), &metadata->name, path, error)
	    || !requiredString(object, QStringLiteral("category"), &category, path, error)
	    || !optionalString(object, QStringLiteral("description"), &metadata->description, path, error)
	    || !optionalDate(object, QStringLiteral("startDate"), &metadata->start_date, path, error)
	    || !optionalDate(object, QStringLiteral("endDate"), &metadata->end_date, path, error))
	{
		return false;
	}
	auto parsed = categoryFromName(category);
	if (!parsed)
		return fail(error, QStringLiteral("$.metadata.category is unsupported"));
	metadata->category = *parsed;
	return true;
}

bool decodeNotices(const QJsonObject& object, ImageryNotices* notices, QString* error)
{
	auto const path = QStringLiteral("$.notices");
	if (!hasOnlyKeys(object, {
		QStringLiteral("attributionText"), QStringLiteral("attributionUrl"),
		QStringLiteral("sourceUrl"), QStringLiteral("termsUrl"),
		QStringLiteral("privacyUrl"), QStringLiteral("notes")
	}, path, error))
	{
		return false;
	}
	return optionalString(object, QStringLiteral("attributionText"), &notices->attribution_text, path, error)
	       && optionalUrl(object, QStringLiteral("attributionUrl"), &notices->attribution_url, path, error)
	       && optionalUrl(object, QStringLiteral("sourceUrl"), &notices->source_url, path, error)
	       && optionalUrl(object, QStringLiteral("termsUrl"), &notices->terms_url, path, error)
	       && optionalUrl(object, QStringLiteral("privacyUrl"), &notices->privacy_url, path, error)
	       && optionalString(object, QStringLiteral("notes"), &notices->notes, path, error);
}

bool decodeRequest(const QJsonObject& object, ImageryRequestPolicy* request, QString* error)
{
	auto const path = QStringLiteral("$.request");
	if (!hasOnlyKeys(object, {
		QStringLiteral("referer"), QStringLiteral("emptyHttpStatusCodes")
	}, path, error)
	    || !optionalUrl(object, QStringLiteral("referer"), &request->referer, path, error))
	{
		return false;
	}
	QJsonArray codes;
	if (!requiredArray(object, QStringLiteral("emptyHttpStatusCodes"), &codes, path, error)
	    || codes.size() > 32)
	{
		return false;
	}
	request->empty_http_status_codes.clear();
	for (int index = 0; index < codes.size(); ++index)
	{
		qint64 code = 0;
		if (!integerValue(codes.at(index), 100, 599, &code))
			return fail(error, QStringLiteral("$.request.emptyHttpStatusCodes contains an invalid code"));
		request->empty_http_status_codes.push_back(int(code));
	}
	return true;
}

bool decodeCatalogProvenance(
	const QJsonValue& value,
	std::optional<CatalogSourceProvenance>* provenance,
	QString* error)
{
	if (value.isNull())
		return true;
	if (!value.isObject())
		return fail(error, QStringLiteral("$.catalogProvenance must be null or an object"));
	auto const object = value.toObject();
	auto const path = QStringLiteral("$.catalogProvenance");
	if (!hasOnlyKeys(object, {
		QStringLiteral("catalogId"), QStringLiteral("catalogRevision"),
		QStringLiteral("catalogSha256"), QStringLiteral("sourceId"),
		QStringLiteral("fullFingerprint"), QStringLiteral("operationalFingerprint")
	}, path, error))
	{
		return false;
	}

	CatalogSourceProvenance decoded;
	QString catalog_sha256;
	QString full_fingerprint;
	QString operational_fingerprint;
	qint64 revision = 0;
	if (!requiredString(object, QStringLiteral("catalogId"), &decoded.catalog_id, path, error)
	    || !requiredInteger(object, QStringLiteral("catalogRevision"), 1,
	                        std::numeric_limits<int>::max(), &revision, path, error)
	    || !requiredString(object, QStringLiteral("catalogSha256"), &catalog_sha256, path, error)
	    || !requiredString(object, QStringLiteral("sourceId"), &decoded.source_id, path, error)
	    || !requiredString(object, QStringLiteral("fullFingerprint"), &full_fingerprint, path, error)
	    || !requiredString(object, QStringLiteral("operationalFingerprint"), &operational_fingerprint, path, error))
	{
		return false;
	}
	decoded.catalog_revision = int(revision);
	decoded.catalog_sha256 = catalog_sha256.toLatin1();
	decoded.full_fingerprint = full_fingerprint.toLatin1();
	decoded.operational_fingerprint = operational_fingerprint.toLatin1();
	*provenance = std::move(decoded);
	return true;
}

bool decodeProvenance(const QJsonObject& object, ImageryProvenance* provenance, QString* error)
{
	auto const path = QStringLiteral("$.registration.provenance");
	if (!hasOnlyKeys(object, {
		QStringLiteral("method"), QStringLiteral("observed"),
		QStringLiteral("author"), QStringLiteral("rmsError"), QStringLiteral("notes")
	}, path, error)
	    || !optionalString(object, QStringLiteral("method"), &provenance->method, path, error)
	    || !optionalDate(object, QStringLiteral("observed"), &provenance->observed, path, error)
	    || !optionalString(object, QStringLiteral("author"), &provenance->author, path, error)
	    || !optionalString(object, QStringLiteral("notes"), &provenance->notes, path, error))
	{
		return false;
	}
	auto const rms = object.value(QStringLiteral("rmsError"));
	if (!rms.isUndefined())
	{
		if (!rms.isDouble() || !std::isfinite(rms.toDouble()))
			return fail(error, QStringLiteral("$.registration.provenance.rmsError must be finite"));
		provenance->rms_error = rms.toDouble();
	}
	return true;
}

bool decodeRegistration(const QJsonValue& value,
	                    std::optional<TranslationRegistration>* registration,
	                    QString* error)
{
	if (value.isNull())
		return true;
	if (!value.isObject())
		return fail(error, QStringLiteral("$.registration must be null or an object"));
	auto const object = value.toObject();
	auto const path = QStringLiteral("$.registration");
	if (!hasOnlyKeys(object, {
		QStringLiteral("direction"), QStringLiteral("sourceFrame"),
		QStringLiteral("targetFrame"), QStringLiteral("operation"),
		QStringLiteral("provenance")
	}, path, error))
	{
		return false;
	}

	QString direction;
	QJsonObject source_frame;
	QJsonObject target_frame;
	QJsonObject operation;
	QJsonObject provenance;
	if (!requiredString(object, QStringLiteral("direction"), &direction, path, error)
	    || direction != QLatin1String("source-to-corrected")
	    || !requiredObject(object, QStringLiteral("sourceFrame"), &source_frame, path, error)
	    || !requiredObject(object, QStringLiteral("targetFrame"), &target_frame, path, error)
	    || !requiredObject(object, QStringLiteral("operation"), &operation, path, error)
	    || !requiredObject(object, QStringLiteral("provenance"), &provenance, path, error))
	{
		if (direction != QLatin1String("source-to-corrected") && error)
			*error = QStringLiteral("$.registration.direction is unsupported");
		return false;
	}
	if (!hasOnlyKeys(source_frame, { QStringLiteral("crs") },
	                 QStringLiteral("$.registration.sourceFrame"), error)
	    || !hasOnlyKeys(target_frame, { QStringLiteral("crs"), QStringLiteral("id") },
	                    QStringLiteral("$.registration.targetFrame"), error))
	{
		return false;
	}

	QString type;
	QString unit;
	TranslationRegistration decoded;
	if (!requiredString(operation, QStringLiteral("type"), &type,
	                    QStringLiteral("$.registration.operation"), error))
	{
		return false;
	}
	if (type != QLatin1String("translation2d"))
	{
		if (type == QLatin1String("affine2d") || type == QLatin1String("gridShift"))
			return fail(error, QStringLiteral("Resolved runtime does not support %1 registration").arg(type));
		return fail(error, QStringLiteral("Resolved runtime registration operation is unknown"));
	}
	if (!hasOnlyKeys(operation, {
		QStringLiteral("type"), QStringLiteral("unit"),
		QStringLiteral("dx"), QStringLiteral("dy")
	}, QStringLiteral("$.registration.operation"), error))
	{
		return false;
	}
	if (!requiredString(operation, QStringLiteral("unit"), &unit,
	                    QStringLiteral("$.registration.operation"), error)
	    || unit != QLatin1String("crs")
	    || !requiredFinite(operation, QStringLiteral("dx"), &decoded.dx,
	                       QStringLiteral("$.registration.operation"), error)
	    || !requiredFinite(operation, QStringLiteral("dy"), &decoded.dy,
	                       QStringLiteral("$.registration.operation"), error)
	    || !requiredString(source_frame, QStringLiteral("crs"), &decoded.source_crs,
	                       QStringLiteral("$.registration.sourceFrame"), error)
	    || !requiredString(target_frame, QStringLiteral("crs"), &decoded.target_crs,
	                       QStringLiteral("$.registration.targetFrame"), error)
	    || !optionalString(target_frame, QStringLiteral("id"), &decoded.target_frame_id,
	                       QStringLiteral("$.registration.targetFrame"), error)
	    || !decodeProvenance(provenance, &decoded.provenance, error))
	{
		if (unit != QLatin1String("crs") && error)
			*error = QStringLiteral("$.registration.operation.unit is unsupported");
		return false;
	}
	*registration = std::move(decoded);
	return true;
}

bool decodeMatrixSet(const QJsonObject& object, TileMatrixSet* matrix_set, QString* error)
{
	auto const path = QStringLiteral("$.tileMatrixSet");
	if (!hasOnlyKeys(object, {
		QStringLiteral("id"), QStringLiteral("crs"), QStringLiteral("matrices")
	}, path, error)
	    || !requiredString(object, QStringLiteral("id"), &matrix_set->id, path, error)
	    || !requiredString(object, QStringLiteral("crs"), &matrix_set->crs, path, error))
	{
		return false;
	}
	QJsonArray matrices;
	if (!requiredArray(object, QStringLiteral("matrices"), &matrices, path, error)
	    || matrices.isEmpty() || matrices.size() > 63)
	{
		return fail(error, QStringLiteral("$.tileMatrixSet.matrices has an invalid size"));
	}
	for (int index = 0; index < matrices.size(); ++index)
	{
		if (!matrices.at(index).isObject())
			return fail(error, QStringLiteral("$.tileMatrixSet.matrices contains a non-object"));
		auto const matrix_object = matrices.at(index).toObject();
		auto const matrix_path = QStringLiteral("$.tileMatrixSet.matrices[%1]").arg(index);
		if (!hasOnlyKeys(matrix_object, {
			QStringLiteral("id"), QStringLiteral("zoom"), QStringLiteral("cellSize"),
			QStringLiteral("pointOfOrigin"), QStringLiteral("tileWidth"),
			QStringLiteral("tileHeight"), QStringLiteral("matrixWidth"),
			QStringLiteral("matrixHeight")
		}, matrix_path, error))
		{
			return false;
		}
		TileMatrix matrix;
		qint64 zoom = 0;
		qint64 tile_width = 0;
		qint64 tile_height = 0;
		if (!requiredString(matrix_object, QStringLiteral("id"), &matrix.id, matrix_path, error)
		    || !requiredInteger(matrix_object, QStringLiteral("zoom"), 0, 62, &zoom, matrix_path, error)
		    || !requiredFinite(matrix_object, QStringLiteral("cellSize"), &matrix.cell_size, matrix_path, error)
		    || !requiredInteger(matrix_object, QStringLiteral("tileWidth"), 1,
		                        std::numeric_limits<int>::max(), &tile_width, matrix_path, error)
		    || !requiredInteger(matrix_object, QStringLiteral("tileHeight"), 1,
		                        std::numeric_limits<int>::max(), &tile_height, matrix_path, error)
		    || !requiredInteger(matrix_object, QStringLiteral("matrixWidth"), 1,
		                        9007199254740991LL, &matrix.matrix_width, matrix_path, error)
		    || !requiredInteger(matrix_object, QStringLiteral("matrixHeight"), 1,
		                        9007199254740991LL, &matrix.matrix_height, matrix_path, error))
		{
			return false;
		}
		auto const origin = matrix_object.value(QStringLiteral("pointOfOrigin"));
		if (!origin.isArray() || origin.toArray().size() != 2
		    || !origin.toArray().at(0).isDouble()
		    || !origin.toArray().at(1).isDouble()
		    || !std::isfinite(origin.toArray().at(0).toDouble())
		    || !std::isfinite(origin.toArray().at(1).toDouble()))
		{
			return fail(error, QStringLiteral("%1.pointOfOrigin must contain two finite numbers").arg(matrix_path));
		}
		matrix.zoom = int(zoom);
		matrix.tile_size = QSize(int(tile_width), int(tile_height));
		matrix.point_of_origin = QPointF(
			origin.toArray().at(0).toDouble(),
			origin.toArray().at(1).toDouble()
		);
		matrix_set->matrices.push_back(std::move(matrix));
	}
	return true;
}

bool decodeLimits(const QJsonArray& array,
	              QVector<TileMatrixLimits>* limits,
	              QString* error)
{
	if (array.size() > 63)
		return fail(error, QStringLiteral("$.limits exceeds the supported zoom count"));
	for (int index = 0; index < array.size(); ++index)
	{
		if (!array.at(index).isObject())
			return fail(error, QStringLiteral("$.limits contains a non-object"));
		auto const object = array.at(index).toObject();
		auto const path = QStringLiteral("$.limits[%1]").arg(index);
		if (!hasOnlyKeys(object, {
			QStringLiteral("zoom"), QStringLiteral("minColumn"),
			QStringLiteral("maxColumn"), QStringLiteral("minRow"),
			QStringLiteral("maxRow")
		}, path, error))
		{
			return false;
		}
		TileMatrixLimits limit;
		qint64 zoom = 0;
		if (!requiredInteger(object, QStringLiteral("zoom"), 0, 62, &zoom, path, error)
		    || !requiredInteger(object, QStringLiteral("minColumn"), 0, 9007199254740991LL,
		                        &limit.min_column, path, error)
		    || !requiredInteger(object, QStringLiteral("maxColumn"), 0, 9007199254740991LL,
		                        &limit.max_column, path, error)
		    || !requiredInteger(object, QStringLiteral("minRow"), 0, 9007199254740991LL,
		                        &limit.min_row, path, error)
		    || !requiredInteger(object, QStringLiteral("maxRow"), 0, 9007199254740991LL,
		                        &limit.max_row, path, error))
		{
			return false;
		}
		limit.zoom = int(zoom);
		limits->push_back(limit);
	}
	return true;
}

bool decodeSourceObject(const QJsonObject& object,
	                    ResolvedImagerySource* source,
	                    QString* error)
{
	if (!hasOnlyKeys(object, {
		QStringLiteral("format"), QStringLiteral("version"), QStringLiteral("catalogProvenance"),
		QStringLiteral("metadata"), QStringLiteral("notices"),
		QStringLiteral("tiles"), QStringLiteral("scheme"),
		QStringLiteral("mediaType"), QStringLiteral("tileMatrixSet"),
		QStringLiteral("minZoom"), QStringLiteral("maxZoom"),
		QStringLiteral("limits"), QStringLiteral("request"),
		QStringLiteral("registration")
	}, QStringLiteral("$"), error))
	{
		return false;
	}
	QString format;
	QString scheme;
	QJsonObject metadata;
	QJsonObject notices;
	QJsonObject matrix_set;
	QJsonObject request;
	QJsonArray tiles;
	QJsonArray limits;
	qint64 version = 0;
	qint64 min_zoom = 0;
	qint64 max_zoom = 0;
	if (!requiredString(object, QStringLiteral("format"), &format, QStringLiteral("$"), error)
	    || format != ImagerySourceSnapshotCodec::formatIdentifier()
	    || !requiredInteger(object, QStringLiteral("version"), 1, 1, &version,
	                        QStringLiteral("$"), error)
	    || !requiredObject(object, QStringLiteral("metadata"), &metadata, QStringLiteral("$"), error)
	    || !requiredObject(object, QStringLiteral("notices"), &notices, QStringLiteral("$"), error)
	    || !requiredArray(object, QStringLiteral("tiles"), &tiles, QStringLiteral("$"), error)
	    || !requiredString(object, QStringLiteral("scheme"), &scheme, QStringLiteral("$"), error)
	    || !requiredString(object, QStringLiteral("mediaType"), &source->media_type,
	                       QStringLiteral("$"), error)
	    || !requiredObject(object, QStringLiteral("tileMatrixSet"), &matrix_set,
	                       QStringLiteral("$"), error)
	    || !requiredInteger(object, QStringLiteral("minZoom"), 0, 62, &min_zoom,
	                        QStringLiteral("$"), error)
	    || !requiredInteger(object, QStringLiteral("maxZoom"), 0, 62, &max_zoom,
	                        QStringLiteral("$"), error)
	    || !requiredArray(object, QStringLiteral("limits"), &limits, QStringLiteral("$"), error)
	    || !requiredObject(object, QStringLiteral("request"), &request, QStringLiteral("$"), error))
	{
		if (format != ImagerySourceSnapshotCodec::formatIdentifier() && error)
			*error = QStringLiteral("Snapshot format identifier is unsupported");
		return false;
	}
	auto parsed_scheme = tileRowSchemeFromName(scheme);
	if (!parsed_scheme)
		return fail(error, QStringLiteral("$.scheme is unsupported"));
	source->row_scheme = *parsed_scheme;
	source->min_zoom = int(min_zoom);
	source->max_zoom = int(max_zoom);

	if (tiles.isEmpty() || tiles.size() > 8)
		return fail(error, QStringLiteral("$.tiles has an invalid size"));
	for (auto const& value : tiles)
	{
		if (!value.isString())
			return fail(error, QStringLiteral("$.tiles contains a non-string"));
		source->tile_urls.push_back({ value.toString() });
	}

	return decodeMetadata(metadata, &source->metadata, error)
	       && decodeNotices(notices, &source->notices, error)
	       && decodeMatrixSet(matrix_set, &source->tile_matrix_set, error)
	       && decodeLimits(limits, &source->tile_limits, error)
	       && decodeRequest(request, &source->request, error)
	       && decodeCatalogProvenance(object.value(QStringLiteral("catalogProvenance")),
	                                  &source->catalog_provenance, error)
	       && decodeRegistration(object.value(QStringLiteral("registration")),
	                             &source->registration, error);
}

QByteArray fingerprint(const QByteArray& bytes)
{
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

}  // namespace

QString ImagerySourceSnapshotCodec::formatIdentifier()
{
	return QStringLiteral("org.openorienteering.imagery-source-snapshot");
}

std::optional<ImagerySourceSnapshot> ImagerySourceSnapshotCodec::encode(
	const ResolvedImagerySource& source,
	QString* error)
{
	if (!source.validate(error))
		return std::nullopt;
	auto json = encodeSource(source, error);
	if (json.isEmpty())
		return std::nullopt;
	if (json.size() > maximum_size)
		return failOptional<ImagerySourceSnapshot>(error, QStringLiteral("Resolved imagery snapshot exceeds the size limit"));
	if (error)
		error->clear();
	return ImagerySourceSnapshot {
		source,
		json,
		fingerprint(json),
	};
}

std::optional<ImagerySourceSnapshot> ImagerySourceSnapshotCodec::decode(
	const QByteArray& json,
	QString* error)
{
	if (json.isEmpty() || json.size() > maximum_size)
		return failOptional<ImagerySourceSnapshot>(error, QStringLiteral("Resolved imagery snapshot has an invalid size"));

	QJsonParseError parse_error;
	auto const document = QJsonDocument::fromJson(json, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !document.isObject())
	{
		return failOptional<ImagerySourceSnapshot>(
			error,
			QStringLiteral("Resolved imagery snapshot is invalid JSON: %1")
				.arg(parse_error.errorString())
		);
	}

	ResolvedImagerySource source;
	if (!decodeSourceObject(document.object(), &source, error)
	    || !source.validate(error))
	{
		return std::nullopt;
	}
	auto canonical = encodeSource(source, error);
	if (canonical.isEmpty())
		return std::nullopt;
	if (error)
		error->clear();
	return ImagerySourceSnapshot {
		std::move(source),
		canonical,
		fingerprint(canonical),
	};
}

}  // namespace OpenOrienteering::imagery
