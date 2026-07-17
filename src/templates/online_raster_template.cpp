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

#include "templates/online_raster_template.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <functional>
#include <limits>
#include <utility>

#include <QBuffer>
#include <QCryptographicHash>
#include <QImageReader>
#include <QPainter>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "core/georeferencing.h"
#include "core/map.h"
#include "core/map_coord.h"
#include "gui/util_gui.h"
#include "imagery/imagery_network_permissions.h"
#include "util/transformation.h"
#include "util/util.h"

namespace OpenOrienteering {

namespace {

// One payload is valid. Retaining a second preserves a useful malformed-file
// diagnostic without allowing an attacker to accumulate an unbounded number
// of near-limit strings in one template record.
constexpr qsizetype maximum_embedded_snapshot_payloads = 2;
constexpr qsizetype maximum_snapshot_version_size = 32;
constexpr qsizetype maximum_snapshot_checksum_size = 64;
constexpr qsizetype maximum_snapshot_encoding_size = 32;

struct BoundedElementText
{
	QString text;
	bool too_large = false;
};

BoundedElementText readBoundedElementText(
	QXmlStreamReader& xml,
	qsizetype maximum_size)
{
	BoundedElementText result;
	while (!xml.atEnd())
	{
		auto const token = xml.readNext();
		if (token == QXmlStreamReader::EndElement)
			break;
		if (token == QXmlStreamReader::StartElement)
		{
			xml.raiseError(
				OnlineRasterTemplate::tr(
					"The embedded imagery source contains an unexpected element."));
			break;
		}
		if (token != QXmlStreamReader::Characters
		    && token != QXmlStreamReader::EntityReference)
			continue;
		auto const text = xml.text();
		if (result.too_large
		    || text.size() > maximum_size - result.text.size())
		{
			result.too_large = true;
			continue;
		}
		result.text.append(text);
	}
	return result;
}

int cacheCostKiB(const QImage& image)
{
	auto const bytes = qsizetype(image.bytesPerLine()) * image.height();
	return int(std::min<qsizetype>(std::numeric_limits<int>::max(), (bytes + 1023) / 1024));
}

bool imageIsOpaque(
	const QImage& source,
	const RasterResourceManager::CancellationToken& cancellation,
	const std::shared_ptr<std::atomic_bool>& source_cancelled)
{
	if (!source.hasAlphaChannel())
		return true;
	auto const image = source.convertToFormat(QImage::Format_RGBA8888);
	for (int y = 0; y < image.height(); ++y)
	{
		if (cancellation.isCancelled()
			|| source_cancelled->load(std::memory_order_relaxed))
		{
			return false;
		}
		auto const* row = image.constScanLine(y);
		for (int x = 0; x < image.width(); ++x)
		{
			if (row[4 * x + 3] != 255)
				return false;
		}
	}
	return true;
}

QImage addGutter(QImage source)
{
	if (source.isNull())
		return {};
	if (source.format() != QImage::Format_RGBA8888_Premultiplied)
		source = source.convertToFormat(QImage::Format_RGBA8888_Premultiplied);
	auto const& core = source;
	QImage padded(core.width() + 2, core.height() + 2, QImage::Format_RGBA8888);
	if (padded.isNull())
		return {};

	QPainter painter(&padded);
	painter.setCompositionMode(QPainter::CompositionMode_Source);
	painter.drawImage(1, 1, core);
	painter.drawImage(QRect(0, 1, 1, core.height()), core, QRect(0, 0, 1, core.height()));
	painter.drawImage(QRect(core.width() + 1, 1, 1, core.height()), core,
					  QRect(core.width() - 1, 0, 1, core.height()));
	painter.drawImage(QRect(1, 0, core.width(), 1), core, QRect(0, 0, core.width(), 1));
	painter.drawImage(QRect(1, core.height() + 1, core.width(), 1), core,
					  QRect(0, core.height() - 1, core.width(), 1));
	painter.drawImage(QPoint(0, 0), core, QRect(0, 0, 1, 1));
	painter.drawImage(QPoint(core.width() + 1, 0), core, QRect(core.width() - 1, 0, 1, 1));
	painter.drawImage(QPoint(0, core.height() + 1), core, QRect(0, core.height() - 1, 1, 1));
	painter.drawImage(QPoint(core.width() + 1, core.height() + 1), core,
					  QRect(core.width() - 1, core.height() - 1, 1, 1));
	painter.end();
	return padded;
}

double pointDistance(const QPointF& first, const QPointF& second)
{
	return std::hypot(first.x() - second.x(), first.y() - second.y());
}

QRectF boundsOf(std::initializer_list<QPointF> points)
{
	QRectF result;
	for (auto const& point : points)
		rectIncludeSafe(result, point);
	return result;
}

qint64 boundedFloor(double value)
{
	if (!std::isfinite(value))
		return 0;
	if (value <= double(std::numeric_limits<qint64>::min()))
		return std::numeric_limits<qint64>::min();
	if (value >= double(std::numeric_limits<qint64>::max()))
		return std::numeric_limits<qint64>::max();
	return qint64(std::floor(value));
}

quint64 mixSignature(quint64 value, quint64 input)
{
	value ^= input + 0x9e3779b97f4a7c15ULL + (value << 6) + (value >> 2);
	return value;
}

std::optional<qint64> rgbaImageBytes(qint64 width, qint64 height)
{
	if (width <= 0 || height <= 0 || width > std::numeric_limits<qint64>::max() / height
		|| width * height > std::numeric_limits<qint64>::max() / 4)
	{
		return std::nullopt;
	}
	return width * height * 4;
}

std::shared_ptr<std::atomic<qint64>> sharedDecodeBytesInFlight()
{
	static auto counter = std::make_shared<std::atomic<qint64>>(0);
	return counter;
}

std::shared_ptr<std::atomic<qint64>> sharedRetainedRasterBytes()
{
	static auto counter = std::make_shared<std::atomic<qint64>>(0);
	return counter;
}

QSet<OnlineRasterTemplate*>& liveOnlineRasterTemplates()
{
	static QSet<OnlineRasterTemplate*> templates;
	return templates;
}

quint64 nextRetainedAccess()
{
	static quint64 next = 1;
	if (next == std::numeric_limits<quint64>::max())
		qFatal("Online imagery retained-access space exhausted");
	return next++;
}

} // namespace

bool OnlineRasterTemplate::TileWindow::isEmpty() const noexcept
{
	return zoom < 0 || min_column > max_column || min_row > max_row;
}

bool OnlineRasterTemplate::TileWindow::intersects(const TileWindow& other) const noexcept
{
	return zoom == other.zoom && !isEmpty() && !other.isEmpty() && min_column <= other.max_column
		   && max_column >= other.min_column && min_row <= other.max_row
		   && max_row >= other.min_row;
}

bool OnlineRasterTemplate::TileWindow::contains(const TileWindow& other) const noexcept
{
	return zoom == other.zoom && !isEmpty() && !other.isEmpty() && min_column <= other.min_column
		   && max_column >= other.max_column && min_row <= other.min_row
		   && max_row >= other.max_row;
}

qint64 OnlineRasterTemplate::TileWindow::width() const noexcept
{
	return isEmpty() ? 0 : max_column - min_column + 1;
}

qint64 OnlineRasterTemplate::TileWindow::height() const noexcept
{
	return isEmpty() ? 0 : max_row - min_row + 1;
}

void OnlineRasterTemplate::AtlasCache::clear()
{
	window = {};
	signature.clear();
	image = {};
	image_to_map = {};
	map_bounds = {};
	pixels_per_map_unit = 0;
	provisional = false;
	output_owned = false;
	memory.reset();
	render_memory.reset();
}

OnlineRasterTemplate::MemoryReservation::~MemoryReservation()
{
	if (counter && bytes > 0)
		counter->fetch_sub(bytes, std::memory_order_relaxed);
}

void OnlineRasterTemplate::MemoryReservation::shrinkTo(
	qint64 retained_bytes) noexcept
{
	retained_bytes = std::clamp<qint64>(
		retained_bytes, 0, bytes);
	auto const released = bytes - retained_bytes;
	bytes = retained_bytes;
	if (counter && released > 0)
		counter->fetch_sub(released, std::memory_order_relaxed);
}

OnlineRasterTemplate::EncodedTilePayload::~EncodedTilePayload()
{
	if (bytes_in_flight && byte_count > 0)
		bytes_in_flight->fetch_sub(byte_count, std::memory_order_relaxed);
}

OnlineRasterTemplate::OnlineRasterTemplate(imagery::ImagerySourceSnapshot snapshot, Map* map,
										   imagery::TileNetworkManager* network)
	: OnlineRasterTemplate(QString{}, map, network)
{
	setSnapshot(std::move(snapshot));
}

OnlineRasterTemplate::OnlineRasterTemplate(const QString& path, Map* map,
										   imagery::TileNetworkManager* network)
	: TemplateImage(path, map),
	  network_(network ? network : &imagery::TileNetworkManager::instance()),
	  network_client_id_(imagery::TileNetworkManager::nextClientId()),
	  decode_bytes_in_flight_(sharedDecodeBytesInFlight())
{
	if (!network)
		(void)imagery::ImageryNetworkPermissions::instance();
	initializeConnections();
	liveOnlineRasterTemplates().insert(this);
}

OnlineRasterTemplate::OnlineRasterTemplate(const OnlineRasterTemplate& prototype)
	: TemplateImage(prototype),
	  snapshot_(prototype.snapshot_),
	  stored_snapshot_json_(prototype.stored_snapshot_json_),
	  stored_snapshot_sha256_(prototype.stored_snapshot_sha256_),
	  stored_snapshot_version_(prototype.stored_snapshot_version_),
	  stored_snapshot_payloads_(prototype.stored_snapshot_payloads_),
	  stored_version_attribute_(prototype.stored_version_attribute_),
	  stored_checksum_attribute_(prototype.stored_checksum_attribute_),
	  snapshot_error_(prototype.snapshot_error_),
	  source_(prototype.source_),
	  network_(prototype.network_),
	  network_client_id_(imagery::TileNetworkManager::nextClientId()),
	  decode_bytes_in_flight_(sharedDecodeBytesInFlight())
{
	if (source_)
		source_projection_ = std::make_unique<ProjTransform>(source_->tile_matrix_set.crs);
	image = QImage(1, 1, QImage::Format_RGBA8888);
	image.fill(Qt::transparent);
	initializeConnections();
	liveOnlineRasterTemplates().insert(this);
}

OnlineRasterTemplate::~OnlineRasterTemplate()
{
	liveOnlineRasterTemplates().remove(this);
	if (template_state == Loaded)
		unloadTemplateFile();
	network_->cancelClient(network_client_id_);
	decode_owner_.invalidate();
}

void OnlineRasterTemplate::initializeConnections()
{
	auto& georeferencing = map->getGeoreferencing();
	disconnect(&georeferencing, &Georeferencing::projectionChanged, this,
			   &TemplateImage::updateGeoreferencing);
	disconnect(&georeferencing, &Georeferencing::transformationChanged, this,
			   &TemplateImage::updateGeoreferencing);
	connect(&georeferencing, &Georeferencing::projectionChanged, this,
			&OnlineRasterTemplate::onMapGeoreferencingChanged);
	connect(&georeferencing, &Georeferencing::transformationChanged, this,
			&OnlineRasterTemplate::onMapGeoreferencingChanged);
	connect(&georeferencing, &Georeferencing::stateChanged, this,
			&OnlineRasterTemplate::onMapGeoreferencingChanged);
	connect(network_, &imagery::TileNetworkManager::finished, this,
			&OnlineRasterTemplate::onNetworkFinished);
	connect(network_, &imagery::TileNetworkManager::privateOriginApprovalChanged, this,
				[this](const QString& origin, bool approved) {
					if (!approved || !source_)
						return;
					QVector<OnlineRasterTileKey> recovered;
					for (auto found = failed_tiles_.begin();
					     found != failed_tiles_.end(); ++found)
					{
						QVector<int> endpoints;
						for (auto rejected =
						       found->policy_rejected_origins.cbegin();
						     rejected
						       != found->policy_rejected_origins.cend();
						     ++rejected)
						{
							if (rejected.value() == origin)
								endpoints.push_back(rejected.key());
						}
						if (endpoints.isEmpty())
							continue;
						for (auto const endpoint : std::as_const(endpoints))
						{
							found->policy_rejected_origins.remove(endpoint);
							found->terminal_endpoints.remove(endpoint);
						}
						found->permanent = false;
						found->retry = QDeadlineTimer(0);
						recovered.push_back(found.key());
					}
					if (!recovered.isEmpty())
					{
					queueWindow(wanted_window_, false);
					if (output_preparation_active_)
						queueWindow(output_window_, false);
				}
			});
	connect(network_, &imagery::TileNetworkManager::offlineModeChanged, this,
			[this](bool offline) {
				if (offline || offline_tiles_.isEmpty())
					return;
				offline_tiles_.clear();
				queueWindow(wanted_window_, false);
				if (output_preparation_active_ && output_window_ != wanted_window_)
					queueWindow(output_window_, false);
				auto const bounds = calculateTemplateBoundingBox();
				if (bounds.isValid())
					map->setTemplateAreaDirty(this, bounds, getTemplateBoundingBoxPixelBorder());
			});
	retry_timer_.setSingleShot(true);
	connect(&retry_timer_, &QTimer::timeout, this, [this] {
		queueWindow(wanted_window_, false);
		scheduleNextRetry();
	});
	atlas_retry_timer_.setSingleShot(true);
	connect(&atlas_retry_timer_, &QTimer::timeout, this, [this] {
		atlas_queue_busy_ = false;
		auto const bounds = last_render_bounds_.isValid() ? last_render_bounds_
														 : calculateTemplateBoundingBox();
		if (bounds.isValid())
			map->setTemplateAreaDirty(this, bounds, getTemplateBoundingBoxPixelBorder());
	});
}

std::unique_ptr<OnlineRasterTemplate> OnlineRasterTemplate::createForType(const QString& path,
																		  Map* map)
{
	return std::unique_ptr<OnlineRasterTemplate>(new OnlineRasterTemplate(path, map, nullptr));
}

OnlineRasterTemplate* OnlineRasterTemplate::duplicate() const
{
	return new OnlineRasterTemplate(*this);
}

const char* OnlineRasterTemplate::getTemplateType() const
{
	return "OnlineRasterTemplate";
}

bool OnlineRasterTemplate::fileExists() const
{
	return true;
}

Template::LookupResult OnlineRasterTemplate::tryToFindTemplateFile(const QString&)
{
	if (template_state != Loaded)
		template_state = Unloaded;
	return FoundByAbsPath;
}

const imagery::ImagerySourceSnapshot* OnlineRasterTemplate::sourceSnapshot() const noexcept
{
	return snapshot_ ? &*snapshot_ : nullptr;
}

void OnlineRasterTemplate::setDisplayName(const QString& name)
{
	auto const trimmed = name.trimmed();
	template_file = trimmed.isEmpty() && source_ ? source_->metadata.name : trimmed;
}

bool OnlineRasterTemplate::sourceReady() const noexcept
{
	return source_ && source_projection_ && source_projection_->isValid()
		   && map->getGeoreferencing().getState() == Georeferencing::Geospatial;
}

const imagery::ResolvedImagerySource* OnlineRasterTemplate::source() const noexcept
{
	return source_.get();
}

const imagery::TileMatrix* OnlineRasterTemplate::matrix(int zoom) const noexcept
{
	return source_ ? source_->tile_matrix_set.matrixForZoom(zoom) : nullptr;
}

const imagery::TileMatrixLimits* OnlineRasterTemplate::limits(int zoom) const noexcept
{
	return source_ ? source_->limitsForZoom(zoom) : nullptr;
}

bool OnlineRasterTemplate::tileAllowed(const OnlineRasterTileKey& key) const noexcept
{
	auto const* tile_matrix = matrix(key.zoom);
	if (!tile_matrix || !tile_matrix->contains(key.column, key.row))
		return false;
	auto const* tile_limits = limits(key.zoom);
	return !tile_limits || tile_limits->contains(key.column, key.row);
}

imagery::CrsBounds OnlineRasterTemplate::tileBounds(const OnlineRasterTileKey& key) const noexcept
{
	auto const* tile_matrix = matrix(key.zoom);
	return tile_matrix ? tile_matrix->tileBounds(key.column, key.row) : imagery::CrsBounds{};
}

void OnlineRasterTemplate::setSnapshot(imagery::ImagerySourceSnapshot snapshot)
{
	stored_snapshot_json_ = snapshot.canonical_json;
	stored_snapshot_sha256_ = snapshot.sha256;
	stored_snapshot_version_ = QString::number(imagery::ImagerySourceSnapshotCodec::version);
	stored_snapshot_payloads_ = {
		{
			QStringLiteral("base64"),
			QString::fromLatin1(stored_snapshot_json_.toBase64(QByteArray::Base64Encoding)),
		},
	};
	stored_version_attribute_ = true;
	stored_checksum_attribute_ = true;
	source_ = std::make_shared<const imagery::ResolvedImagerySource>(snapshot.source);
	snapshot_ = std::move(snapshot);
	snapshot_error_.clear();
	if (template_file.isEmpty())
		template_file = source_->metadata.name;
}

bool OnlineRasterTemplate::decodeStoredSnapshot()
{
	if (!stored_version_attribute_ || stored_snapshot_version_.isEmpty())
	{
		snapshot_error_ = tr("The embedded imagery source version is missing.");
		return false;
	}
	if (stored_snapshot_version_ != QString::number(imagery::ImagerySourceSnapshotCodec::version))
	{
		snapshot_error_ = tr("The embedded imagery source version %1 is not supported.")
							  .arg(stored_snapshot_version_);
		return false;
	}
	if (stored_snapshot_payloads_.size() != 1)
	{
		snapshot_error_ = stored_snapshot_payloads_.isEmpty()
							  ? tr("The embedded imagery source is missing.")
							  : tr("The embedded imagery source contains multiple payloads.");
		return false;
	}
	auto const& payload = stored_snapshot_payloads_.front();
	if (payload.encoding != QLatin1String("base64"))
	{
		snapshot_error_ =
			tr("The embedded imagery source encoding %1 is not supported.").arg(payload.encoding);
		return false;
	}
	if (payload.text.size()
	    > imagery::ImagerySourceSnapshotCodec::maximum_base64_size)
	{
		snapshot_error_ =
			tr("The embedded imagery source exceeds the size limit.");
		return false;
	}
	auto decoded_payload = QByteArray::fromBase64Encoding(payload.text.toLatin1(),
													  QByteArray::AbortOnBase64DecodingErrors);
	if (!decoded_payload)
	{
		snapshot_error_ = tr("The embedded imagery source is not valid base64.");
		return false;
	}
	if (decoded_payload.decoded.size()
	    > imagery::ImagerySourceSnapshotCodec::maximum_size)
	{
		snapshot_error_ =
			tr("The embedded imagery source exceeds the size limit.");
		return false;
	}
	stored_snapshot_json_ = std::move(decoded_payload.decoded);
	if (stored_snapshot_json_.isEmpty())
	{
		snapshot_error_ = tr("The embedded imagery source is missing.");
		return false;
	}
	if (!stored_checksum_attribute_ || stored_snapshot_sha256_.isEmpty())
	{
		snapshot_error_ = tr("The embedded imagery source checksum is missing.");
		return false;
	}
	static const QRegularExpression sha_pattern(QStringLiteral("^[0-9a-f]{64}$"));
	if (!sha_pattern.match(QString::fromLatin1(stored_snapshot_sha256_)).hasMatch())
	{
		snapshot_error_ = tr("The embedded imagery source checksum is not valid SHA-256.");
		return false;
	}
	auto const actual_sha =
		QCryptographicHash::hash(stored_snapshot_json_, QCryptographicHash::Sha256).toHex();
	if (!stored_snapshot_sha256_.isEmpty() && actual_sha != stored_snapshot_sha256_)
	{
		snapshot_error_ = tr("The embedded imagery source checksum does not match.");
		return false;
	}
	QString error;
	auto decoded = imagery::ImagerySourceSnapshotCodec::decode(stored_snapshot_json_, &error);
	if (!decoded)
	{
		snapshot_error_ = error;
		return false;
	}
	setSnapshot(std::move(*decoded));
	return true;
}

void OnlineRasterTemplate::saveTypeSpecificTemplateConfiguration(QXmlStreamWriter& xml) const
{
	xml.writeStartElement(QStringLiteral("online_source"));
	if (stored_version_attribute_)
		xml.writeAttribute(QStringLiteral("snapshot_version"), stored_snapshot_version_);
	if (stored_checksum_attribute_)
		xml.writeAttribute(QStringLiteral("sha256"), QString::fromLatin1(stored_snapshot_sha256_));
	for (auto const& payload : stored_snapshot_payloads_)
	{
		xml.writeStartElement(QStringLiteral("snapshot_json"));
		xml.writeAttribute(QStringLiteral("encoding"), payload.encoding);
		xml.writeCharacters(payload.text);
		xml.writeEndElement();
	}
	xml.writeEndElement();
}

bool OnlineRasterTemplate::loadTypeSpecificTemplateConfiguration(QXmlStreamReader& xml)
{
	if (xml.name() != QLatin1String("online_source"))
	{
		xml.skipCurrentElement();
		return true;
	}

	auto const attributes = xml.attributes();
	stored_version_attribute_ = attributes.hasAttribute(QLatin1String("snapshot_version"));
	stored_checksum_attribute_ = attributes.hasAttribute(QLatin1String("sha256"));
	auto const version = attributes.value(QLatin1String("snapshot_version"));
	auto const checksum = attributes.value(QLatin1String("sha256"));
	stored_snapshot_version_.clear();
	stored_snapshot_sha256_.clear();
	stored_snapshot_payloads_.clear();
	stored_snapshot_json_.clear();
	snapshot_.reset();
	source_.reset();
	snapshot_error_.clear();
	if (version.size() > maximum_snapshot_version_size
	    || checksum.size() > maximum_snapshot_checksum_size)
	{
		snapshot_error_ =
			tr("The embedded imagery source metadata exceeds the size limit.");
	}
	else
	{
		stored_snapshot_version_ = version.toString();
		stored_snapshot_sha256_ = checksum.toLatin1();
	}
	while (xml.readNextStartElement())
	{
		if (xml.name() == QLatin1String("snapshot_json"))
			{
				auto const encoding =
					xml.attributes()
						.value(QLatin1String("encoding"))
						.toString();
			auto payload = readBoundedElementText(
				xml,
				imagery::ImagerySourceSnapshotCodec::maximum_base64_size);
			if (payload.too_large
			    || encoding.size() > maximum_snapshot_encoding_size)
			{
				if (snapshot_error_.isEmpty())
					snapshot_error_ = tr(
						"The embedded imagery source exceeds the size limit.");
				continue;
			}
			if (stored_snapshot_payloads_.size()
			    >= maximum_embedded_snapshot_payloads)
			{
				if (snapshot_error_.isEmpty())
					snapshot_error_ = tr(
						"The embedded imagery source contains too many payloads.");
				continue;
			}
				stored_snapshot_payloads_.push_back({
					encoding,
				std::move(payload.text),
			});
		}
		else
		{
			xml.skipCurrentElement();
		}
	}
	is_georeferenced = true;
	return true;
}

bool OnlineRasterTemplate::finishTypeSpecificTemplateConfiguration()
{
	if (snapshot_error_.isEmpty())
		decodeStoredSnapshot();
	is_georeferenced = true;
	return true;
}

bool OnlineRasterTemplate::loadTemplateFileImpl()
{
	if (!source_ && !decodeStoredSnapshot())
	{
		setErrorString(snapshot_error_);
		return false;
	}
	QString error;
	if (!source_->validate(&error))
	{
		setErrorString(error);
		return false;
	}
	if (map->getGeoreferencing().getState() != Georeferencing::Geospatial)
	{
		setErrorString(tr("Online imagery requires a georeferenced map."));
		return false;
	}
	source_projection_ = std::make_unique<ProjTransform>(source_->tile_matrix_set.crs);
	if (!source_projection_->isValid())
	{
		setErrorString(tr("The imagery coordinate reference system is not usable: %1")
						   .arg(source_->tile_matrix_set.crs));
		source_projection_.reset();
		return false;
	}

	resetRuntime(false);
	image = QImage(1, 1, QImage::Format_RGBA8888);
	image.fill(Qt::transparent);
	is_georeferenced = true;
	transform = TemplateTransform{};
	updateTransformationMatrices();
	return true;
}

bool OnlineRasterTemplate::postLoadSetup(QWidget*, bool& out_center_in_view)
{
	out_center_in_view = false;
	return true;
}

void OnlineRasterTemplate::unloadTemplateFileImpl()
{
	resetRuntime(true);
	source_projection_.reset();
	image = {};
}

void OnlineRasterTemplate::resetRuntime(bool clear_cache)
{
	auto const previous_generation = generation_;
	if (generation_ == std::numeric_limits<quint64>::max())
		qFatal("Online imagery generation space exhausted");
	++generation_;
	network_->cancelClient(network_client_id_, previous_generation);
	decode_owner_.invalidate();
	cancelAtlasBuild();
	pending_fetches_.clear();
	for (auto const& pending : std::as_const(pending_decodes_))
		pending.cancelled->store(true, std::memory_order_relaxed);
	pending_decodes_.clear();
	queued_tiles_.clear();
	failed_tiles_.clear();
	offline_tiles_.clear();
	retry_timer_.stop();
	atlas_retry_timer_.stop();
	atlas_queue_busy_ = false;
	wanted_window_ = {};
	output_window_ = {};
	output_preparation_active_ = false;
	output_preparation_error_.clear();
	output_preparation_scale_ = 0;
	output_source_tiles_released_ = false;
	output_keys_.clear();
	output_tiles_.clear();
	output_render_memory_.clear();
	atlas_.clear();
	output_atlases_.clear();
	output_uses_atlases_ = false;
	last_render_bounds_ = {};
	if (clear_cache)
		tile_cache_.clear();
	setResourceStatus({});
}

QSize OnlineRasterTemplate::getRasterPixelSize() const
{
	if (!source_)
		return {};
	auto const* deepest = matrix(source_->max_zoom);
	if (!deepest)
		return {};
	auto const width = std::min<long double>(std::numeric_limits<int>::max(),
											 static_cast<long double>(deepest->matrix_width)
												 * deepest->tile_size.width());
	auto const height = std::min<long double>(std::numeric_limits<int>::max(),
											  static_cast<long double>(deepest->matrix_height)
												  * deepest->tile_size.height());
	return { int(width), int(height) };
}

std::optional<QPointF> OnlineRasterTemplate::mapToNominalSource(const QPointF& map_point) const
{
	if (!sourceReady())
		return std::nullopt;
	bool ok = false;
	auto const lat_lon = map->getGeoreferencing().toGeographicCoords(MapCoordF(map_point), &ok);
	if (!ok)
		return std::nullopt;
	auto projected = source_projection_->forward(lat_lon, &ok);
	if (!ok || !std::isfinite(projected.x()) || !std::isfinite(projected.y()))
		return std::nullopt;
	if (source_->registration)
	{
		projected.rx() -= source_->registration->dx;
		projected.ry() -= source_->registration->dy;
	}
	return projected;
}

std::optional<QPointF> OnlineRasterTemplate::nominalSourceToMap(const QPointF& source_point) const
{
	if (!sourceReady())
		return std::nullopt;
	auto corrected = source_point;
	if (source_->registration)
	{
		corrected.rx() += source_->registration->dx;
		corrected.ry() += source_->registration->dy;
	}
	bool ok = false;
	auto const lat_lon = source_projection_->inverse(corrected, &ok);
	if (!ok)
		return std::nullopt;
	auto const result = map->getGeoreferencing().toMapCoordF(lat_lon, &ok);
	if (!ok || !std::isfinite(result.x()) || !std::isfinite(result.y()))
		return std::nullopt;
	return QPointF(result);
}

std::optional<QPointF> OnlineRasterTemplate::imagePointToMap(const OnlineRasterTileKey& image_key,
															 const QPointF& image_point) const
{
	auto const* tile_matrix = matrix(image_key.zoom);
	auto const bounds = tileBounds(image_key);
	if (!tile_matrix || !bounds.isValid())
		return std::nullopt;
	auto const source_point =
		QPointF(bounds.west + (image_point.x() - 1) * tile_matrix->cell_size,
				bounds.north - (image_point.y() - 1) * tile_matrix->cell_size);
	return nominalSourceToMap(source_point);
}

std::optional<QTransform> OnlineRasterTemplate::imageRectToMap(const OnlineRasterTileKey& image_key,
															   const QRectF& source_rect,
															   QRectF* map_bounds,
															   double* residual_map_units) const
{
	if (!source_rect.isValid() || source_rect.isEmpty())
		return std::nullopt;
	auto const top_left = imagePointToMap(image_key, source_rect.topLeft());
	auto const top_right = imagePointToMap(image_key, source_rect.topRight());
	auto const bottom_left = imagePointToMap(image_key, source_rect.bottomLeft());
	auto const bottom_right = imagePointToMap(image_key, source_rect.bottomRight());
	auto const center = imagePointToMap(image_key, source_rect.center());
	if (!top_left || !top_right || !bottom_left || !bottom_right || !center)
		return std::nullopt;

	auto const width = source_rect.width();
	auto const height = source_rect.height();
	QTransform transform(
		(top_right->x() - top_left->x()) / width, (top_right->y() - top_left->y()) / width,
		(bottom_left->x() - top_left->x()) / height, (bottom_left->y() - top_left->y()) / height,
		top_left->x() - source_rect.x() * (top_right->x() - top_left->x()) / width
			- source_rect.y() * (bottom_left->x() - top_left->x()) / height,
		top_left->y() - source_rect.x() * (top_right->y() - top_left->y()) / width
			- source_rect.y() * (bottom_left->y() - top_left->y()) / height);
	if (!transform.isInvertible())
		return std::nullopt;

	if (map_bounds)
	{
		*map_bounds = boundsOf({ *top_left, *top_right, *bottom_left, *bottom_right });
	}
	if (residual_map_units)
	{
		double residual = 0;
		for (int y = 0; y <= 4; ++y)
		{
			for (int x = 0; x <= 4; ++x)
			{
				auto const sample = QPointF(source_rect.left() + source_rect.width() * x / 4.0,
											source_rect.top() + source_rect.height() * y / 4.0);
				auto const actual = imagePointToMap(image_key, sample);
				if (!actual)
					return std::nullopt;
				residual = std::max(residual, pointDistance(transform.map(sample), *actual));
			}
		}
		*residual_map_units = residual;
	}
	return transform;
}

OnlineRasterTemplate::TileWindow OnlineRasterTemplate::tileWindowForMapRect(const QRectF& map_rect,
																			int zoom,
																			bool exact_output,
																			bool* projection_complete) const
{
	if (projection_complete)
		*projection_complete = false;
	TileWindow window;
	window.zoom = zoom;
	auto const* tile_matrix = matrix(zoom);
	if (!tile_matrix || !map_rect.isValid() || map_rect.isEmpty())
		return window;

	QRectF source_bounds;
	bool complete = true;
	double uncertainty = 0;
	if (!exact_output)
	{
		for (int y = 0; y <= 4; ++y)
		{
			for (int x = 0; x <= 4; ++x)
			{
				auto const point =
					QPointF(map_rect.left() + map_rect.width() * x / 4.0,
							map_rect.top() + map_rect.height() * y / 4.0);
				if (auto projected = mapToNominalSource(point))
					rectIncludeSafe(source_bounds, *projected);
			}
		}
	}
	else
	{
		std::function<void(const QRectF&, int)> sample_cell;
		sample_cell = [this, tile_matrix, &source_bounds, &complete, &uncertainty,
					   &sample_cell](const QRectF& cell, int depth) {
			if (!complete)
				return;
			std::array<QPointF, 9> map_points {
				cell.topLeft(),
				cell.topRight(),
				cell.bottomLeft(),
				cell.bottomRight(),
				QPointF(cell.center().x(), cell.top()),
				QPointF(cell.center().x(), cell.bottom()),
				QPointF(cell.left(), cell.center().y()),
				QPointF(cell.right(), cell.center().y()),
				cell.center(),
			};
			std::array<QPointF, 9> projected;
			for (std::size_t index = 0; index < map_points.size(); ++index)
			{
				auto value = mapToNominalSource(map_points.at(index));
				if (!value)
				{
					complete = false;
					return;
				}
				projected.at(index) = *value;
				rectIncludeSafe(source_bounds, *value);
			}

			auto error = 0.0;
			error = std::max(
				error, pointDistance(projected.at(4),
									 (projected.at(0) + projected.at(1)) / 2));
			error = std::max(
				error, pointDistance(projected.at(5),
									 (projected.at(2) + projected.at(3)) / 2));
			error = std::max(
				error, pointDistance(projected.at(6),
									 (projected.at(0) + projected.at(2)) / 2));
			error = std::max(
				error, pointDistance(projected.at(7),
									 (projected.at(1) + projected.at(3)) / 2));
			error = std::max(
				error, pointDistance(
						   projected.at(8),
						   (projected.at(0) + projected.at(1) + projected.at(2)
							+ projected.at(3))
							   / 4));
			auto const span = std::max(
				{ pointDistance(projected.at(0), projected.at(1)),
				  pointDistance(projected.at(0), projected.at(2)),
				  pointDistance(projected.at(1), projected.at(3)),
				  pointDistance(projected.at(2), projected.at(3)) });
			auto const tolerance =
				std::max(tile_matrix->cell_size * 0.05, span * 1.0e-8);
			if (depth < 2 || error > tolerance)
			{
				if (depth >= 7)
				{
					complete = false;
					return;
				}
				auto const half_width = cell.width() / 2;
				auto const half_height = cell.height() / 2;
				sample_cell(
					QRectF(cell.left(), cell.top(), half_width, half_height), depth + 1);
				sample_cell(
					QRectF(cell.center().x(), cell.top(), half_width, half_height), depth + 1);
				sample_cell(
					QRectF(cell.left(), cell.center().y(), half_width, half_height), depth + 1);
				sample_cell(
					QRectF(cell.center(), QSizeF(half_width, half_height)), depth + 1);
				return;
			}
			uncertainty = std::max(uncertainty, error);
		};
		sample_cell(map_rect, 0);
	}
	if (!source_bounds.isValid() || source_bounds.isEmpty())
		return window;
	auto const tile_width = tile_matrix->cell_size * tile_matrix->tile_size.width();
	auto const tile_height = tile_matrix->cell_size * tile_matrix->tile_size.height();
	auto const numerical_uncertainty =
		128 * std::numeric_limits<double>::epsilon()
		* std::max({
			1.0,
			std::abs(source_bounds.left()),
			std::abs(source_bounds.right()),
			std::abs(source_bounds.top()),
			std::abs(source_bounds.bottom()),
			std::abs(tile_width),
			std::abs(tile_height),
		});
	if (uncertainty <= numerical_uncertainty)
		uncertainty = 0;
	if (uncertainty > 0)
		source_bounds.adjust(-uncertainty, -uncertainty, uncertainty, uncertainty);
	if (projection_complete)
		*projection_complete = complete;

	auto min_column =
		boundedFloor((source_bounds.left() - tile_matrix->point_of_origin.x()) / tile_width);
	auto max_column = boundedFloor(
		std::nextafter((source_bounds.right() - tile_matrix->point_of_origin.x()) / tile_width,
					   -std::numeric_limits<double>::infinity()));
	auto min_row =
		boundedFloor((tile_matrix->point_of_origin.y() - source_bounds.bottom()) / tile_height);
	auto max_row = boundedFloor(
		std::nextafter((tile_matrix->point_of_origin.y() - source_bounds.top()) / tile_height,
					   -std::numeric_limits<double>::infinity()));

	qint64 allowed_min_column = 0;
	qint64 allowed_max_column = tile_matrix->matrix_width - 1;
	qint64 allowed_min_row = 0;
	qint64 allowed_max_row = tile_matrix->matrix_height - 1;
	if (auto const* tile_limits = limits(zoom))
	{
		allowed_min_column = tile_limits->min_column;
		allowed_max_column = tile_limits->max_column;
		allowed_min_row = tile_limits->min_row;
		allowed_max_row = tile_limits->max_row;
	}
	window.min_column = std::max(min_column, allowed_min_column);
	window.max_column = std::min(max_column, allowed_max_column);
	window.min_row = std::max(min_row, allowed_min_row);
	window.max_row = std::min(max_row, allowed_max_row);
	return window;
}

int OnlineRasterTemplate::chooseZoom(const QRectF& map_rect, double pixels_per_map_unit,
									 bool exact_output) const
{
	if (exact_output)
		exact_projection_failed_ = false;
	if (!source_)
		return -1;
	auto result = source_->max_zoom;
	if (!(pixels_per_map_unit > 0) || !std::isfinite(pixels_per_map_unit))
		return result;

	auto minimum_scale = std::numeric_limits<double>::infinity();
	auto const subdivisions = exact_output ? 8 : 2;
	auto const largest_dimension = std::max(map_rect.width(), map_rect.height());
	auto const derivative_step = std::clamp(largest_dimension / 4096.0, 1.0e-3, 1.0);
	bool scale_complete = true;
	for (int y = 0; y <= subdivisions; ++y)
	{
		for (int x = 0; x <= subdivisions; ++x)
		{
			auto const point =
				QPointF(map_rect.left() + map_rect.width() * x / subdivisions,
						map_rect.top() + map_rect.height() * y / subdivisions);
			auto const source = mapToNominalSource(point);
			auto const source_x =
				mapToNominalSource(point + QPointF(derivative_step, 0));
			auto const source_y =
				mapToNominalSource(point + QPointF(0, derivative_step));
			if (!source || !source_x || !source_y)
			{
				scale_complete = false;
				continue;
			}

			auto const x_vector = (*source_x - *source) / derivative_step;
			auto const y_vector = (*source_y - *source) / derivative_step;
			auto const trace = QPointF::dotProduct(x_vector, x_vector)
							   + QPointF::dotProduct(y_vector, y_vector);
			auto const determinant =
				x_vector.x() * y_vector.y() - x_vector.y() * y_vector.x();
			auto const discriminant =
				std::max(0.0, trace * trace - 4.0 * determinant * determinant);
			auto const singular_value =
				std::sqrt(std::max(0.0, 0.5 * (trace - std::sqrt(discriminant))));
			if (singular_value > 0 && std::isfinite(singular_value))
				minimum_scale = std::min(minimum_scale, singular_value);
			else
				scale_complete = false;
		}
	}
	if (exact_output && !scale_complete)
	{
		exact_projection_failed_ = true;
		return -1;
	}
	if (std::isfinite(minimum_scale))
	{
		auto const desired_cell = minimum_scale / pixels_per_map_unit;
		// Interactive rendering may tolerate modest resampling to keep panning
		// fluid. Exact output requires source pixels at least as fine as output
		// pixels, with a small numerical safety margin.
		auto const maximum_cell = desired_cell * (exact_output ? 0.98 : 1.5);
		for (int zoom = source_->min_zoom; zoom <= source_->max_zoom; ++zoom)
		{
			auto const* candidate = matrix(zoom);
			if (candidate && candidate->cell_size <= maximum_cell)
			{
				result = zoom;
				break;
			}
		}
	}

	bool projection_complete = true;
	auto const selected_window =
		tileWindowForMapRect(
			map_rect, result, exact_output,
			&projection_complete);
	if (exact_output)
	{
		if (!projection_complete)
		{
			exact_projection_failed_ = true;
			return -1;
		}
		// Never hide a resource-limit failure by exporting a coarser imagery
		// level than the requested output resolution.
		if (!selected_window.isEmpty() && !workingSetFits(selected_window))
			return -1;
		return result;
	}

	while (result > source_->min_zoom)
	{
		auto const window = withOverscan(tileWindowForMapRect(map_rect, result), 1);
		if (window.isEmpty() || workingSetFits(window))
			break;
		--result;
	}
	auto const final_window = withOverscan(tileWindowForMapRect(map_rect, result), 1);
	if (!final_window.isEmpty() && !workingSetFits(final_window))
		return -1;
	return result;
}

OnlineRasterTemplate::TileWindow OnlineRasterTemplate::withOverscan(TileWindow window,
																	qint64 tiles) const
{
	if (window.isEmpty())
		return window;
	auto const* tile_matrix = matrix(window.zoom);
	if (!tile_matrix)
		return {};
	qint64 min_column = 0;
	qint64 max_column = tile_matrix->matrix_width - 1;
	qint64 min_row = 0;
	qint64 max_row = tile_matrix->matrix_height - 1;
	if (auto const* tile_limits = limits(window.zoom))
	{
		min_column = tile_limits->min_column;
		max_column = tile_limits->max_column;
		min_row = tile_limits->min_row;
		max_row = tile_limits->max_row;
	}
	window.min_column = std::max(min_column, window.min_column - tiles);
	window.max_column = std::min(max_column, window.max_column + tiles);
	window.min_row = std::max(min_row, window.min_row - tiles);
	window.max_row = std::min(max_row, window.max_row + tiles);
	return window;
}

std::optional<qint64> OnlineRasterTemplate::tileCount(const TileWindow& window) const noexcept
{
	auto const width = window.width();
	auto const height = window.height();
	if (width <= 0 || height <= 0 || width > std::numeric_limits<qint64>::max() / height)
	{
		return std::nullopt;
	}
	return width * height;
}

bool OnlineRasterTemplate::workingSetFits(const TileWindow& window) const noexcept
{
	auto const count = tileCount(window);
	auto const* tile_matrix = matrix(window.zoom);
	if (!count || !tile_matrix || *count > max_window_tiles)
		return false;
	auto const width = qint64(tile_matrix->tile_size.width()) + 2;
	auto const height = qint64(tile_matrix->tile_size.height()) + 2;
	if (width <= 0 || height <= 0 || width > std::numeric_limits<qint64>::max() / height
		|| width * height > std::numeric_limits<qint64>::max() / 4)
	{
		return false;
	}
	auto const tile_bytes = width * height * 4;
	// Reserve half of the budget again for coarse coverage, prior windows,
	// and cache bookkeeping so the exact working set cannot churn itself out.
	auto const reserved_tiles = *count + (*count + 1) / 2;
	return reserved_tiles <= max_working_set_bytes / tile_bytes;
}

bool OnlineRasterTemplate::keyNeededForWindow(const OnlineRasterTileKey& key,
											  const TileWindow& window) const noexcept
{
	if (window.isEmpty() || key.zoom < source_->min_zoom || key.zoom > window.zoom)
	{
		return false;
	}
	auto const shift = window.zoom - key.zoom;
	return key.column >= (window.min_column >> shift) && key.column <= (window.max_column >> shift)
		   && key.row >= (window.min_row >> shift) && key.row <= (window.max_row >> shift);
}

void OnlineRasterTemplate::cancelUnwantedWork(const TileWindow& window)
{
	QVector<imagery::TileNetworkManager::Token> cancelled;
	cancelled.reserve(pending_fetches_.size());
	for (auto found = pending_fetches_.cbegin(); found != pending_fetches_.cend(); ++found)
	{
		if (!keyNeededForWindow(found->key, window) && !output_keys_.contains(found->key))
			cancelled.push_back(found.key());
	}
	for (auto token : cancelled)
	{
		auto found = pending_fetches_.find(token);
		if (found == pending_fetches_.end())
			continue;
		queued_tiles_.remove(found->key);
		pending_fetches_.erase(found);
		network_->cancel(token);
	}
	QVector<OnlineRasterTileKey> cancelled_decodes;
	cancelled_decodes.reserve(pending_decodes_.size());
	for (auto found = pending_decodes_.cbegin(); found != pending_decodes_.cend(); ++found)
	{
		if (!keyNeededForWindow(found.key(), window) && !output_keys_.contains(found.key()))
			cancelled_decodes.push_back(found.key());
	}
	for (auto const& key : cancelled_decodes)
	{
		auto found = pending_decodes_.find(key);
		if (found == pending_decodes_.end())
			continue;
		found->cancelled->store(true, std::memory_order_relaxed);
		// Keep the key admitted until the worker completion releases its
		// encoded payload. This prevents a quick pan back from duplicating a
		// still-resident decode allocation.
	}
}

void OnlineRasterTemplate::updateRenderContext(const ViewRenderContext& context)
{
	if (template_state != Loaded || !sourceReady())
		return;
	auto visible_map_rect = context.visible_map_rect;
	auto const screen_bounds = onScreenMapBounds();
	if (screen_bounds.isValid())
		visible_map_rect = visible_map_rect.intersected(screen_bounds);
	if (visible_map_rect.isEmpty())
	{
		auto const replace_pending = !wanted_window_.isEmpty();
		wanted_window_ = {};
		queueWindow(wanted_window_, replace_pending);
		if (replace_pending)
		{
			map->setTemplateAreaDirty(
				this, context.visible_map_rect,
				getTemplateBoundingBoxPixelBorder());
		}
		return;
	}
	auto const pixels_per_map_unit = Util::mmToPixelPhysical(context.view_zoom);
	auto const zoom = chooseZoom(visible_map_rect, pixels_per_map_unit);
	auto window = withOverscan(tileWindowForMapRect(visible_map_rect, zoom), 1);
	auto const replace_pending = wanted_window_ != window;
	wanted_window_ = window;
	queueWindow(window, replace_pending);
	if (replace_pending)
	{
		// View-context updates run after the first frame for a pan, zoom, or
		// visibility change. Cached tiles produce no asynchronous completion,
		// so the window change itself must request the follow-up frame.
		map->setTemplateAreaDirty(
			this, context.visible_map_rect,
			getTemplateBoundingBoxPixelBorder());
	}
}

OutputRenderPreparation OnlineRasterTemplate::prepareForOutput(const QRectF& map_rect,
															   double pixels_per_map_unit)
{
	if (template_state != Loaded || !sourceReady())
	{
		return {
			OutputRenderPreparation::State::Failed,
			0,
			0,
			tr("Online imagery requires a loaded source and a georeferenced map."),
		};
	}
	if (output_preparation_active_
	    && !output_preparation_error_.isEmpty())
	{
		return {
			OutputRenderPreparation::State::Failed,
			0,
			0,
			output_preparation_error_,
		};
	}
	auto const zoom = chooseZoom(map_rect, pixels_per_map_unit, true);
	if (zoom < 0)
	{
		return {
			OutputRenderPreparation::State::Failed,
			0,
			0,
			exact_projection_failed_
				? tr("The requested area cannot be completely reprojected into "
					 "the imagery coordinate reference system.")
				: tr("The requested imagery output exceeds the bounded working set."),
		};
	}
	bool projection_complete = false;
	auto const window = tileWindowForMapRect(map_rect, zoom, true, &projection_complete);
	if (!projection_complete)
	{
		return {
			OutputRenderPreparation::State::Failed,
			0,
			0,
			tr("The requested area cannot be completely reprojected into "
			   "the imagery coordinate reference system."),
		};
	}
	if (window.isEmpty())
	{
		if (output_preparation_active_)
			finishOutputPreparation(true);
		output_preparation_active_ = true;
		output_window_ = {};
		output_preparation_scale_ =
			pixels_per_map_unit;
		updateResourceStatus();
		return {};
	}
	if (!workingSetFits(window))
	{
		return {
			OutputRenderPreparation::State::Failed,
			0,
			0,
			tr("The requested imagery output exceeds the bounded working set."),
		};
	}
		auto const scale_reference = std::max({
			1.0,
			std::abs(output_preparation_scale_),
			std::abs(pixels_per_map_unit),
		});
		auto const output_scale_changed =
			output_preparation_active_
			&& std::abs(
				   output_preparation_scale_
				   - pixels_per_map_unit)
			       > scale_reference * 1.0e-9;
		if (output_preparation_active_
		    && (output_window_ != window
		        || output_scale_changed))
			finishOutputPreparation(true);
		output_preparation_active_ = true;
		output_window_ = window;
		output_preparation_scale_ = pixels_per_map_unit;

	auto const count = tileCount(window);
	if (!count || *count > std::numeric_limits<qsizetype>::max())
	{
		return {
			OutputRenderPreparation::State::Failed,
			0,
			0,
			tr("The requested imagery tile range is invalid."),
		};
	}

		OutputRenderPreparation result;
		result.state = OutputRenderPreparation::State::Pending;
		result.total_resources = qsizetype(*count);
		if (output_source_tiles_released_)
		{
			result.total_resources += output_atlases_.size();
			result.ready_resources = result.total_resources;
			for (auto const& cached : std::as_const(output_atlases_))
			{
				auto const scale = std::max({
					1.0,
					std::abs(cached.pixels_per_map_unit),
					std::abs(pixels_per_map_unit),
				});
				if (!cached.output_owned
				    || cached.image.isNull()
				    || cached.provisional
				    || !cached.render_memory
				    || std::abs(
					       cached.pixels_per_map_unit
					       - pixels_per_map_unit)
					       > scale * 1.0e-9)
				{
					result.state =
						OutputRenderPreparation::State::Failed;
					result.error = tr(
						"The prepared translucent imagery is no longer "
						"available for exact output.");
					updateResourceStatus();
					return result;
				}
			}
			result.state = OutputRenderPreparation::State::Ready;
			updateResourceStatus();
			return result;
		}
		auto available = std::max<qsizetype>(0, max_queued_tiles - queued_tiles_.size());
	auto const center_column = 0.5 * (window.min_column + window.max_column);
	auto const center_row = 0.5 * (window.min_row + window.max_row);
	for (qint64 row = window.min_row; row <= window.max_row; ++row)
	{
		for (qint64 column = window.min_column; column <= window.max_column; ++column)
		{
			OnlineRasterTileKey key{ window.zoom, column, row };
			output_keys_.insert(key);
			if (output_tiles_.contains(key))
			{
				++result.ready_resources;
				continue;
			}
			if (auto const* cached = tile_cache_.object(key))
			{
				output_tiles_.insert(key, *cached);
				++result.ready_resources;
				continue;
			}
			if (offline_tiles_.contains(key))
			{
				result.state = OutputRenderPreparation::State::Failed;
				result.error =
					tr("An exact imagery tile is not available in the offline cache. "
					   "Turn off offline imagery mode and try again.");
				updateResourceStatus();
				return result;
			}
			auto const failure = failed_tiles_.constFind(key);
			if (failure != failed_tiles_.cend() && failure->permanent)
			{
				result.state = OutputRenderPreparation::State::Failed;
				result.error = failure->message.isEmpty()
								   ? tr("An online imagery tile cannot be loaded.")
								   : failure->message;
				updateResourceStatus();
				return result;
			}
			if (available <= 0 || queued_tiles_.contains(key) || !retryAllowed(key))
			{
				continue;
			}
			auto const dx = double(column) - center_column;
			auto const dy = double(row) - center_row;
			queueTile(key, imagery::TileRequestPriority::Coverage, dx * dx + dy * dy);
			--available;
		}
	}
	if (result.ready_resources == result.total_resources)
	{
		bool has_transparency = false;
		bool has_missing = false;
		bool has_pixels = false;
		auto const visuals =
			visualTiles(window, false, &has_transparency, &has_missing, &has_pixels);
		if (has_missing)
			return result;
		if (has_transparency && has_pixels)
		{
			output_uses_atlases_ = true;
			output_render_memory_.clear();
			auto queued_build = false;
				for (auto const& chunk :
				     atlasChunks(window, pixels_per_map_unit))
				{
					auto source_window = withOverscan(chunk, 1);
					source_window.min_column = std::max(
						source_window.min_column,
						window.min_column);
					source_window.max_column = std::min(
						source_window.max_column,
						window.max_column);
					source_window.min_row = std::max(
						source_window.min_row,
						window.min_row);
					source_window.max_row = std::min(
						source_window.max_row,
						window.max_row);
					bool chunk_transparency = false;
					bool chunk_missing = false;
					bool chunk_pixels = false;
					auto const chunk_visuals =
						visualTiles(
							source_window, false,
							&chunk_transparency,
							&chunk_missing,
						&chunk_pixels);
				Q_UNUSED(chunk_transparency);
				if (chunk_missing)
					return result;
				if (!chunk_pixels)
					continue;

				++result.total_resources;
				auto const signature =
					atlasSignature(
						chunk, chunk_visuals, false);
				auto const ready = std::ranges::find_if(
					output_atlases_,
					[&](auto const& cached) {
						auto const scale = std::max({
							1.0,
							std::abs(cached.pixels_per_map_unit),
							std::abs(pixels_per_map_unit),
						});
						return cached.window == chunk
						       && cached.signature == signature
						       && !cached.image.isNull()
						       && !cached.provisional
						       && std::abs(
							      cached.pixels_per_map_unit
							      - pixels_per_map_unit)
							      <= scale * 1.0e-9;
					});
				if (ready != output_atlases_.cend())
				{
					++result.ready_resources;
					continue;
				}
				if (queued_build)
					continue;
				queued_build = true;
				if (!queueAtlasBuild(
					    chunk, chunk_visuals, false,
					    pixels_per_map_unit, signature, true))
				{
					if (atlas_queue_busy_)
					{
						updateResourceStatus();
						return result;
					}
					result.state =
						OutputRenderPreparation::State::Failed;
					result.error = tr(
						"The translucent imagery cannot be "
						"reprojected within the bounded output policy.");
					updateResourceStatus();
					return result;
				}
			}
		}
		else
		{
			output_uses_atlases_ = false;
			output_atlases_.clear();
			if (result.ready_resources == result.total_resources)
			{
				for (auto const& visual : visuals)
				{
					if (!visual.tile || visual.complete_empty)
						continue;
					auto const bytes = rgbaImageBytes(
						visual.tile->image.width(),
						visual.tile->image.height());
					auto const existing =
						output_render_memory_.constFind(
							visual.cached);
					if (bytes && existing != output_render_memory_.cend()
					    && *existing
					    && (*existing)->bytes >= *bytes)
					{
						continue;
					}
					auto reservation = bytes
						? reserveRetainedMemory(*bytes)
						: std::shared_ptr<MemoryReservation> {};
					if (!reservation)
					{
						output_render_memory_.clear();
						result.state =
							OutputRenderPreparation::State::Failed;
						result.error = tr(
							"The application-wide raster memory budget "
							"cannot reserve exact renderer snapshots.");
						output_preparation_error_ = result.error;
						updateResourceStatus();
						return result;
					}
					output_render_memory_.insert(
						visual.cached,
						std::move(reservation));
				}
			}
		}
			if (result.ready_resources == result.total_resources)
			{
				if (output_uses_atlases_)
				{
					qint64 render_bytes = 0;
					bool render_bytes_valid = true;
					for (auto const& cached :
					     std::as_const(output_atlases_))
					{
						auto const bytes = qint64(
							cached.image.bytesPerLine())
						                   * cached.image.height();
						if (bytes <= 0
						    || render_bytes
						           > max_retained_raster_bytes
						                 - bytes)
						{
							render_bytes_valid = false;
							break;
						}
						render_bytes += bytes;
					}
					if (!render_bytes_valid
					    || render_bytes
					           > max_retained_raster_bytes / 2)
					{
						result.state =
							OutputRenderPreparation::State::Failed;
						result.error = tr(
							"The translucent imagery and its renderer "
							"snapshot exceed the bounded raster memory "
							"policy.");
						updateResourceStatus();
						return result;
					}

					// The completed atlases are self-contained. Drop their
					// source-tile pins and ordinary cache entries before
					// reserving the straight-RGBA snapshots consumed by the
					// renderer. This makes a Ready result an actual memory
					// guarantee rather than a best-effort promise.
					for (auto const& key :
					     std::as_const(output_keys_))
						tile_cache_.remove(key);
					output_tiles_.clear();
					output_source_tiles_released_ = true;

					auto render_memory_ready = true;
					for (auto& cached : output_atlases_)
					{
						auto const bytes = qint64(
							cached.image.bytesPerLine())
						                   * cached.image.height();
						cached.render_memory =
							reserveRetainedMemory(bytes);
						if (!cached.render_memory)
						{
							render_memory_ready = false;
							break;
						}
					}
					if (!render_memory_ready)
					{
						for (auto& cached : output_atlases_)
							cached.render_memory.reset();
						result.state =
							OutputRenderPreparation::State::Failed;
						result.error = tr(
							"The application-wide raster memory budget "
							"cannot reserve exact renderer snapshots.");
						updateResourceStatus();
						return result;
					}
				}
				result.state = OutputRenderPreparation::State::Ready;
			}
	}
	updateResourceStatus();
	return result;
}

void OnlineRasterTemplate::finishOutputPreparation(bool cancelled)
{
	if (!output_preparation_active_)
		return;
	if (atlas_pending_for_output_)
		cancelAtlasBuild();
	if (cancelled)
	{
		QVector<imagery::TileNetworkManager::Token> network_tokens;
		for (auto found = pending_fetches_.cbegin(); found != pending_fetches_.cend(); ++found)
		{
			if (output_keys_.contains(found->key)
				&& !keyNeededForWindow(found->key, wanted_window_))
			{
				network_tokens.push_back(found.key());
			}
		}
		for (auto token : network_tokens)
		{
			auto found = pending_fetches_.find(token);
			if (found == pending_fetches_.end())
				continue;
			queued_tiles_.remove(found->key);
			pending_fetches_.erase(found);
			network_->cancel(token);
		}

		QVector<OnlineRasterTileKey> decode_keys;
		for (auto found = pending_decodes_.cbegin(); found != pending_decodes_.cend(); ++found)
		{
			if (output_keys_.contains(found.key())
				&& !keyNeededForWindow(found.key(), wanted_window_))
			{
				decode_keys.push_back(found.key());
			}
		}
		for (auto const& key : decode_keys)
		{
			auto found = pending_decodes_.find(key);
			if (found == pending_decodes_.end())
				continue;
			found->cancelled->store(true, std::memory_order_relaxed);
		}
	}
	QRectF released_output_bounds;
	for (auto const& output_atlas : std::as_const(output_atlases_))
		rectIncludeSafe(
			released_output_bounds,
			output_atlas.map_bounds);
	output_atlases_.clear();
	output_uses_atlases_ = false;
	output_preparation_active_ = false;
	output_preparation_error_.clear();
	output_window_ = {};
	output_preparation_scale_ = 0;
	output_source_tiles_released_ = false;
	output_keys_.clear();
	output_tiles_.clear();
	output_render_memory_.clear();
	updateResourceStatus();
	if (released_output_bounds.isValid())
	{
		map->setTemplateAreaDirty(
			this, released_output_bounds,
			getTemplateBoundingBoxPixelBorder());
	}
}

bool OnlineRasterTemplate::retryAllowed(const OnlineRasterTileKey& key) const
{
	if (offline_tiles_.contains(key))
		return false;
	auto const found = failed_tiles_.constFind(key);
	return found == failed_tiles_.cend() || (!found->permanent && found->retry.hasExpired());
}

void OnlineRasterTemplate::recordFailure(
	const OnlineRasterTileKey& key,
	bool permanent,
	QString message)
{
	offline_tiles_.remove(key);
	auto& failure = failed_tiles_[key];
	failure.attempts = std::min(failure.attempts + 1, 20);
	failure.permanent = permanent;
	if (!message.isEmpty())
		failure.message = std::move(message);
	auto const base = permanent ? 30'000 : 1000;
	auto const cap = permanent ? 5 * 60 * 1000 : 60'000;
	auto const delay = std::min(cap, base * (1 << std::min(failure.attempts - 1, 10)));
	failure.retry = QDeadlineTimer(delay);
	trimFailureHistory();
	scheduleNextRetry();
}

void OnlineRasterTemplate::recordEndpointFailure(
	const OnlineRasterTileKey& key,
	int endpoint,
	quint64 endpoint_offset,
	bool terminal,
	QString message,
	const QUrl& policy_rejected_url)
{
	auto& failure = failed_tiles_[key];
	failure.next_endpoint_offset =
		endpoint_offset == std::numeric_limits<quint64>::max()
			? 0
			: endpoint_offset + 1;
	if (terminal)
	{
		failure.terminal_endpoints.insert(endpoint);
		if (!policy_rejected_url.isEmpty())
		{
			failure.policy_rejected_origins.insert(
				endpoint,
				imagery::TileNetworkManager::canonicalOrigin(
					policy_rejected_url));
		}
		else
		{
			failure.policy_rejected_origins.remove(endpoint);
		}
	}
	auto const endpoint_count =
		source_ ? int(source_->tile_urls.size()) : 1;
	auto const permanent =
		terminal
		&& failure.terminal_endpoints.size() >= endpoint_count;
	recordFailure(
		key, permanent, std::move(message));
}

void OnlineRasterTemplate::clearFailure(const OnlineRasterTileKey& key)
{
	failed_tiles_.remove(key);
	offline_tiles_.remove(key);
	scheduleNextRetry();
}

void OnlineRasterTemplate::trimFailureHistory()
{
	if (failed_tiles_.size() <= max_failure_records)
		return;

	QVector<OnlineRasterTileKey> removable;
	removable.reserve(failed_tiles_.size() - max_failure_records);
	for (auto found = failed_tiles_.cbegin(); found != failed_tiles_.cend(); ++found)
	{
		auto const needed_for_view =
			source_ && !wanted_window_.isEmpty() && keyNeededForWindow(found.key(), wanted_window_);
		if (!needed_for_view && !output_keys_.contains(found.key()))
			removable.push_back(found.key());
	}
	for (auto const& key : std::as_const(removable))
	{
		if (failed_tiles_.size() <= max_failure_records)
			break;
		failed_tiles_.remove(key);
	}
}

qint64 OnlineRasterTemplate::encodedTileResponseLimit(const OnlineRasterTileKey& key) const noexcept
{
	auto const* tile_matrix = matrix(key.zoom);
	if (!tile_matrix)
		return 1024 * 1024;
	auto const decoded =
		rgbaImageBytes(qint64(tile_matrix->tile_size.width()) + 2,
					   qint64(tile_matrix->tile_size.height()) + 2);
	if (!decoded)
		return max_encoded_tile_response_bytes;
	return std::clamp(*decoded * 2, qint64(1024 * 1024),
					  max_encoded_tile_response_bytes);
}

std::shared_ptr<OnlineRasterTemplate::EncodedTilePayload>
OnlineRasterTemplate::reserveEncodedTilePayload(QByteArray bytes)
{
	auto const byte_count = qint64(bytes.size());
	if (byte_count <= 0 || byte_count > max_encoded_tile_response_bytes)
		return {};
	auto current = decode_bytes_in_flight_->load(std::memory_order_relaxed);
	while (current <= max_decode_encoded_bytes - byte_count)
	{
		if (decode_bytes_in_flight_->compare_exchange_weak(
				current, current + byte_count, std::memory_order_relaxed))
		{
			auto payload = std::make_shared<EncodedTilePayload>();
			payload->bytes = std::move(bytes);
			payload->bytes_in_flight = decode_bytes_in_flight_;
			payload->byte_count = byte_count;
			return payload;
		}
	}
	return {};
}

std::shared_ptr<OnlineRasterTemplate::MemoryReservation>
OnlineRasterTemplate::reserveRetainedMemory(qint64 bytes) const
{
	if (bytes <= 0 || bytes > max_retained_raster_bytes)
		return {};
	auto counter = sharedRetainedRasterBytes();
	auto try_reserve = [&]() -> std::shared_ptr<MemoryReservation> {
		auto current = counter->load(std::memory_order_relaxed);
		while (current <= max_retained_raster_bytes - bytes)
		{
			if (counter->compare_exchange_weak(
					current, current + bytes, std::memory_order_relaxed))
			{
				auto reservation = std::make_shared<MemoryReservation>();
				reservation->counter = counter;
				reservation->bytes = bytes;
				return reservation;
			}
		}
		return {};
	};
	if (auto reservation = try_reserve())
		return reservation;

	// Admission must be able to replace stale cache entries when the shared
	// budget is full. All online templates and their retained images live on
	// this application thread, so reclaim least-recently-used source tiles
	// across templates before treating the pressure as transient.
	auto const current = counter->load(std::memory_order_relaxed);
	auto remaining =
		std::max<qint64>(1, current - (max_retained_raster_bytes - bytes));
	auto candidates = liveOnlineRasterTemplates().values();
	std::sort(
		candidates.begin(),
		candidates.end(),
		[](auto* first, auto* second) {
			if (first->retained_access_ != second->retained_access_)
			{
				return first->retained_access_
				       < second->retained_access_;
			}
			return std::less<OnlineRasterTemplate*> {}(
				first, second);
		});
	for (auto* candidate : std::as_const(candidates))
	{
		if (!candidate)
			continue;
		remaining -= candidate->evictRetainedMemory(remaining);
		if (remaining <= 0)
			break;
	}
	return try_reserve();
}

qint64 OnlineRasterTemplate::evictRetainedMemory(qint64 target_bytes)
{
	if (target_bytes <= 0)
		return 0;
	auto const counter = sharedRetainedRasterBytes();
	auto const before = counter->load(std::memory_order_relaxed);

	auto const original_max_cost = tile_cache_.maxCost();
	auto released = qint64(0);
	while (released < target_bytes
	       && tile_cache_.totalCost() > 0)
	{
		auto const remaining = target_bytes - released;
		auto const target_kib = std::min<qint64>(
			std::numeric_limits<qsizetype>::max(),
			std::max<qint64>(1, (remaining + 1023) / 1024));
		auto const next_cost = std::max<qsizetype>(
			0,
			tile_cache_.totalCost()
				- std::min<qsizetype>(
					tile_cache_.totalCost(),
					qsizetype(target_kib)));
		tile_cache_.setMaxCost(next_cost);
		released =
			before - counter->load(std::memory_order_relaxed);
	}
	tile_cache_.setMaxCost(original_max_cost);

	if (released < target_bytes && atlas_.memory
	    && !atlas_.output_owned && !atlas_cancelled_)
	{
		auto const dirty_bounds = atlas_.map_bounds;
		atlas_.clear();
		if (dirty_bounds.isValid())
		{
			map->setTemplateAreaDirty(
				this, dirty_bounds,
				getTemplateBoundingBoxPixelBorder());
		}
		released =
			before - counter->load(std::memory_order_relaxed);
	}
	return std::max<qint64>(0, released);
}

void OnlineRasterTemplate::scheduleNextRetry()
{
	int next_delay = -1;
	for (auto const& failure : std::as_const(failed_tiles_))
	{
		if (failure.permanent || failure.retry.hasExpired())
			continue;
		auto const remaining = int(
			std::clamp<qint64>(failure.retry.remainingTime(), 0, std::numeric_limits<int>::max()));
		if (next_delay < 0 || remaining < next_delay)
			next_delay = remaining;
	}
	if (next_delay < 0)
		retry_timer_.stop();
	else if (!retry_timer_.isActive() || retry_timer_.remainingTime() > next_delay)
		retry_timer_.start(next_delay);
}

void OnlineRasterTemplate::updateResourceStatus()
{
	TemplateResourceStatus status;
	auto const window =
		output_preparation_active_ && !output_window_.isEmpty() ? output_window_ : wanted_window_;
	if (!sourceReady() || window.isEmpty())
	{
		setResourceStatus(std::move(status));
		return;
	}

	QString permanent_message;
	QString transient_message;
	for (qint64 row = window.min_row; row <= window.max_row; ++row)
	{
		for (qint64 column = window.min_column; column <= window.max_column; ++column)
		{
			OnlineRasterTileKey key{ window.zoom, column, row };
			if (tile_cache_.contains(key))
			{
				++status.ready_resources;
				continue;
			}
			if (offline_tiles_.contains(key))
			{
				++status.offline_resources;
				continue;
			}
			auto const failure = failed_tiles_.constFind(key);
			if (failure != failed_tiles_.cend())
			{
				if (failure->permanent)
				{
					++status.permanent_failures;
					if (permanent_message.isEmpty())
						permanent_message = failure->message;
				}
				else
				{
					++status.transient_failures;
					if (transient_message.isEmpty())
						transient_message = failure->message;
				}
				continue;
			}
			// Missing demand counts as loading even before admission to the
			// bounded network/decode queues.
			++status.loading_resources;
		}
	}
	if (atlas_cancelled_ || atlas_queue_busy_)
		++status.loading_resources;
	else if (!atlas_failed_signature_.isEmpty())
	{
		++status.permanent_failures;
		permanent_message =
			tr("The translucent imagery cannot be reprojected within the bounded policy.");
	}

	auto countForTranslation = [](qsizetype count) {
		return int(std::min<qsizetype>(count, std::numeric_limits<int>::max()));
	};
	if (status.permanent_failures > 0)
	{
		status.message =
			permanent_message.isEmpty()
				? tr("%n imagery tile(s) cannot be loaded.", nullptr,
					 countForTranslation(status.permanent_failures))
				: permanent_message;
	}
	else if (status.offline_resources > 0)
	{
		status.message = tr("%n imagery tile(s) are waiting for network access.", nullptr,
							countForTranslation(status.offline_resources));
	}
	else if (status.transient_failures > 0)
	{
		status.message =
			transient_message.isEmpty()
				? tr("%n imagery tile(s) will be retried.", nullptr,
					 countForTranslation(status.transient_failures))
				: transient_message;
	}
	else if (status.loading_resources > 0)
	{
		status.message = tr("Loading online imagery...");
	}
	setResourceStatus(std::move(status));
}

void OnlineRasterTemplate::queueWindow(const TileWindow& window, bool replace_pending)
{
	if (replace_pending)
		cancelUnwantedWork(window);
	if (window.isEmpty())
	{
		updateResourceStatus();
		return;
	}

	struct Missing
	{
		OnlineRasterTileKey key;
		imagery::TileRequestPriority priority = imagery::TileRequestPriority::Visible;
		double distance = 0;
	};
	QVector<Missing> missing;
	QSet<OnlineRasterTileKey> planned;
	auto const center_column = 0.5 * (window.min_column + window.max_column);
	auto const center_row = 0.5 * (window.min_row + window.max_row);

	auto plan = [this, &missing, &planned](const OnlineRasterTileKey& key,
										   imagery::TileRequestPriority priority, double distance) {
		if (!tileAllowed(key) || tile_cache_.contains(key) || queued_tiles_.contains(key)
			|| planned.contains(key) || !retryAllowed(key))
		{
			return;
		}
		planned.insert(key);
		missing.push_back({ key, priority, distance });
	};

	for (qint64 row = window.min_row; row <= window.max_row; ++row)
	{
		for (qint64 column = window.min_column; column <= window.max_column; ++column)
		{
			OnlineRasterTileKey key{ window.zoom, column, row };
			if (tile_cache_.contains(key))
				continue;
			auto const dx = double(column) - center_column;
			auto const dy = double(row) - center_row;
			auto const distance = dx * dx + dy * dy;
			OnlineRasterTileKey cached_key;
			if (!bestCachedTile(key, &cached_key, nullptr))
			{
				for (int zoom = source_->min_zoom; zoom < key.zoom; ++zoom)
				{
					auto const shift = key.zoom - zoom;
					plan({ zoom, column >> shift, row >> shift },
						 imagery::TileRequestPriority::Coverage, distance);
				}
			}
			plan(key, imagery::TileRequestPriority::Visible, distance);
		}
	}

	std::stable_sort(missing.begin(), missing.end(), [](auto const& first, auto const& second) {
		if (first.priority != second.priority)
			return first.priority < second.priority;
		if (first.priority == imagery::TileRequestPriority::Coverage
			&& first.key.zoom != second.key.zoom)
		{
			return first.key.zoom < second.key.zoom;
		}
		return first.distance < second.distance;
	});

	auto available = std::max<qsizetype>(0, max_queued_tiles - queued_tiles_.size());
	if (decode_bytes_in_flight_->load(std::memory_order_relaxed) >= max_decode_encoded_bytes)
		available = 0;
	for (auto const& item : missing)
	{
		if (available == 0)
			break;
		queueTile(item.key, item.priority, item.distance);
		--available;
	}
	updateResourceStatus();
}

void OnlineRasterTemplate::queueTile(const OnlineRasterTileKey& key,
									 imagery::TileRequestPriority priority,
									 double distance_priority)
{
	if (!source_ || source_->tile_urls.isEmpty())
		return;
	auto signature = quint64(key.zoom);
	signature = mixSignature(signature, quint64(key.column));
	signature = mixSignature(signature, quint64(key.row));
	auto const failure = failed_tiles_.constFind(key);
	auto endpoint_offset =
		failure == failed_tiles_.cend()
			? quint64(0)
			: failure->next_endpoint_offset;
	auto endpoint = -1;
	for (qsizetype probe = 0;
	     probe < source_->tile_urls.size();
	     ++probe)
	{
		auto const candidate_offset =
			endpoint_offset + quint64(probe);
		auto const candidate = int(
			(signature + candidate_offset)
			% quint64(source_->tile_urls.size()));
		if (failure == failed_tiles_.cend()
		    || !failure->terminal_endpoints.contains(candidate))
		{
			endpoint = candidate;
			endpoint_offset = candidate_offset;
			break;
		}
	}
	if (endpoint < 0)
	{
		recordFailure(
			key, true,
			tr("All equivalent imagery endpoints failed."));
		return;
	}
	QString error;
	auto const url = source_->tileUrl(endpoint, key.zoom, key.column, key.row, &error);
	if (!url.isValid() || url.isEmpty())
	{
		recordEndpointFailure(
			key, endpoint, endpoint_offset, true, error);
		return;
	}

	imagery::TileNetworkRequest request;
	request.url = url;
	request.client_id = network_client_id_;
	request.generation = generation_;
	request.priority = priority;
	request.distance_priority = distance_priority;
	request.referer = source_->request.referer.toString(QUrl::FullyEncoded);
	request.empty_http_status_codes = source_->request.empty_http_status_codes;
	request.max_response_bytes = encodedTileResponseLimit(key);
	auto const token = network_->submit(std::move(request));
	pending_fetches_.insert(
		token,
		{ key, generation_, endpoint, endpoint_offset });
	queued_tiles_.insert(key);
}

void OnlineRasterTemplate::onNetworkFinished(imagery::TileNetworkManager::Token token,
											 const imagery::TileNetworkResult& result)
{
	auto const found = pending_fetches_.find(token);
	if (found == pending_fetches_.end())
		return;
	auto const pending = *found;
	pending_fetches_.erase(found);
	if (pending.generation != generation_ || result.generation != pending.generation)
	{
		queued_tiles_.remove(pending.key);
		return;
	}

		switch (result.outcome)
	{
	case imagery::TileNetworkResult::Outcome::Success:
		queueDecode(
			pending.key,
			result.body,
			pending.generation,
			pending.endpoint,
			pending.endpoint_offset);
		return;
	case imagery::TileNetworkResult::Outcome::NotModified:
		queued_tiles_.remove(pending.key);
		recordEndpointFailure(
			pending.key,
			pending.endpoint,
			pending.endpoint_offset,
			true,
			tr("The imagery server returned an unusable not-modified response."));
		queueWindow(wanted_window_, false);
		return;
	case imagery::TileNetworkResult::Outcome::EmptyTile:
		insertEmptyTile(pending.key);
		return;
	case imagery::TileNetworkResult::Outcome::Cancelled:
		queued_tiles_.remove(pending.key);
		queueWindow(wanted_window_, false);
		return;
	case imagery::TileNetworkResult::Outcome::OfflineMiss:
		queued_tiles_.remove(pending.key);
		failed_tiles_.remove(pending.key);
		offline_tiles_.insert(pending.key);
		if (offline_tiles_.size() > max_failure_records)
		{
			for (auto found = offline_tiles_.begin();
				 found != offline_tiles_.end() && offline_tiles_.size() > max_failure_records;)
			{
				auto const needed_for_view =
					!wanted_window_.isEmpty() && keyNeededForWindow(*found, wanted_window_);
				if (!needed_for_view && !output_keys_.contains(*found))
					found = offline_tiles_.erase(found);
				else
					++found;
			}
		}
		scheduleNextRetry();
		updateResourceStatus();
		return;
	case imagery::TileNetworkResult::Outcome::Busy:
		queued_tiles_.remove(pending.key);
		recordFailure(pending.key, false, result.error_string);
		queueWindow(wanted_window_, false);
		return;
	case imagery::TileNetworkResult::Outcome::TransientError:
		queued_tiles_.remove(pending.key);
		recordEndpointFailure(
			pending.key,
			pending.endpoint,
			pending.endpoint_offset,
			false,
			result.error_string);
		queueWindow(wanted_window_, false);
		return;
	case imagery::TileNetworkResult::Outcome::PermanentError:
		queued_tiles_.remove(pending.key);
		recordEndpointFailure(
			pending.key,
			pending.endpoint,
			pending.endpoint_offset,
			true,
			result.error_string);
		queueWindow(wanted_window_, false);
		return;
	case imagery::TileNetworkResult::Outcome::Rejected:
		queued_tiles_.remove(pending.key);
		recordEndpointFailure(
			pending.key,
			pending.endpoint,
			pending.endpoint_offset,
			true,
			result.error_string,
			result.private_network_rejected
				? result.private_network_rejected_url
				: QUrl {});
		queueWindow(wanted_window_, false);
		return;
	}
}

std::optional<OnlineRasterTemplate::CachedTile>
OnlineRasterTemplate::decodeTile(const QByteArray& bytes, const QSize& expected_size,
								 const RasterResourceManager::CancellationToken& cancellation,
								 const std::shared_ptr<std::atomic_bool>& source_cancelled)
{
	auto const is_cancelled = [&] {
		return cancellation.isCancelled()
			   || source_cancelled->load(std::memory_order_relaxed);
	};
	if (is_cancelled() || bytes.isEmpty() || expected_size.isEmpty())
		return std::nullopt;
	if (expected_size.width() > max_tile_dimension || expected_size.height() > max_tile_dimension
		|| qint64(expected_size.width()) * expected_size.height() > max_tile_pixels)
	{
		return std::nullopt;
	}
	QBuffer buffer;
	buffer.setData(bytes);
	if (!buffer.open(QIODevice::ReadOnly))
		return std::nullopt;
	QImageReader reader(&buffer);
	reader.setAutoTransform(false);
	reader.setDecideFormatFromContent(true);
	auto const advertised_size = reader.size();
	if (!advertised_size.isValid() || advertised_size != expected_size)
		return std::nullopt;
	if (is_cancelled())
		return std::nullopt;
	auto decoded = reader.read();
	if (decoded.size() != expected_size || is_cancelled())
		return std::nullopt;
	auto const opaque = imageIsOpaque(decoded, cancellation, source_cancelled);
	if (is_cancelled())
		return std::nullopt;
	auto padded = addGutter(std::move(decoded));
	if (padded.isNull() || is_cancelled())
		return std::nullopt;
	return CachedTile{ std::move(padded), opaque, false, {} };
}

void OnlineRasterTemplate::queueDecode(
	const OnlineRasterTileKey& key,
	QByteArray bytes,
	quint64 generation,
	int endpoint,
	quint64 endpoint_offset)
{
	auto const* tile_matrix = matrix(key.zoom);
	if (!tile_matrix)
	{
		queued_tiles_.remove(key);
		return;
	}
	if (bytes.isEmpty() || qint64(bytes.size()) > encodedTileResponseLimit(key))
	{
		queued_tiles_.remove(key);
		recordEndpointFailure(
			key,
			endpoint,
			endpoint_offset,
			true,
			tr("The imagery tile response is empty or exceeds the "
			   "bounded decode policy."));
		queueWindow(wanted_window_, false);
		return;
	}
	auto payload = reserveEncodedTilePayload(std::move(bytes));
	if (!payload)
	{
		queued_tiles_.remove(key);
		recordFailure(key, false, tr("The imagery decode memory budget is temporarily full."));
		queueWindow(wanted_window_, false);
		return;
	}
	auto const expected_size = tile_matrix->tile_size;
	auto const core_bytes = rgbaImageBytes(
		expected_size.width(), expected_size.height());
	auto const padded_bytes = rgbaImageBytes(
		qint64(expected_size.width()) + 2,
		qint64(expected_size.height()) + 2);
	auto working_memory =
		core_bytes && padded_bytes
			? reserveRetainedMemory(
			      *core_bytes + *padded_bytes)
			: std::shared_ptr<MemoryReservation> {};
	if (!working_memory)
	{
		queued_tiles_.remove(key);
		recordFailure(
			key, false,
			tr("The imagery decode memory budget is temporarily full."));
		queueWindow(wanted_window_, false);
		return;
	}
	auto cancelled = std::make_shared<std::atomic_bool>(false);
	pending_decodes_.insert(
		key,
		{ cancelled, endpoint, endpoint_offset });
	auto const accepted = RasterResourceManager::instance().submit(
		decode_owner_, RasterResourceManager::Lane::Decode,
		RasterResourceManager::Priority::Visible, this,
		[payload = std::move(payload), expected_size, key, generation, cancelled,
		 working_memory = std::move(working_memory),
		 receiver = this](const RasterResourceManager::CancellationToken& cancellation) mutable {
			auto tile = cancelled->load(std::memory_order_relaxed)
							? std::optional<CachedTile>{}
							: decodeTile(
								  payload->bytes, expected_size, cancellation, cancelled);
			if (tile)
			{
				auto const retained_bytes =
					qint64(tile->image.bytesPerLine())
					* tile->image.height();
				working_memory->shrinkTo(retained_bytes);
				tile->memory = std::move(working_memory);
			}
			return RasterResourceManager::Completion{ [receiver, key, tile = std::move(tile),
													   generation, cancelled]() mutable {
				receiver->finishDecode(key, std::move(tile), generation, cancelled);
			} };
		});
	if (!accepted)
	{
		pending_decodes_.remove(key);
		queued_tiles_.remove(key);
		recordFailure(key, false, tr("The imagery decode queue is temporarily full."));
		queueWindow(wanted_window_, false);
	}
}

void OnlineRasterTemplate::finishDecode(const OnlineRasterTileKey& key,
										std::optional<CachedTile> tile, quint64 generation,
										const std::shared_ptr<std::atomic_bool>& cancelled)
{
	auto found = pending_decodes_.find(key);
	if (found == pending_decodes_.end()
	    || found->cancelled != cancelled)
		return;
	auto const endpoint = found->endpoint;
	auto const endpoint_offset = found->endpoint_offset;
	pending_decodes_.erase(found);
	if (generation != generation_)
	{
		queued_tiles_.remove(key);
		return;
	}
	if (cancelled->load(std::memory_order_relaxed))
	{
		queued_tiles_.remove(key);
		queueWindow(wanted_window_, false);
		return;
	}
	if (!tile)
	{
		queued_tiles_.remove(key);
		recordEndpointFailure(
			key,
			endpoint,
			endpoint_offset,
			true,
			tr("The tile image is invalid or has unexpected dimensions."));
		queueWindow(wanted_window_, false);
		return;
	}
	insertTile(key, std::move(*tile));
}

void OnlineRasterTemplate::insertTile(const OnlineRasterTileKey& key, CachedTile tile)
{
	queued_tiles_.remove(key);
	clearFailure(key);
	if (!tile.empty && !tile.memory)
	{
		auto const bytes = qint64(tile.image.bytesPerLine()) * tile.image.height();
		tile.memory = reserveRetainedMemory(bytes);
		if (!tile.memory)
		{
			if (output_preparation_active_
			    && output_keys_.contains(key))
			{
				output_preparation_error_ = tr(
					"Exact imagery cannot fit within the "
					"application-wide raster memory budget.");
				updateResourceStatus();
				return;
			}
			recordFailure(
				key, false,
				tr("The application-wide imagery memory budget is temporarily full."));
			queueWindow(wanted_window_, false);
			return;
		}
	}
	auto const cost = tile.empty ? 1 : cacheCostKiB(tile.image);
	if (cost <= 0 || cost > tile_cache_.maxCost()
		|| !tile_cache_.insert(key, new CachedTile(std::move(tile)), cost))
	{
		recordFailure(key, true, tr("The decoded tile exceeds the imagery memory-cache policy."));
		return;
	}
	retained_access_ = nextRetainedAccess();
	if (output_preparation_active_
	    && !output_source_tiles_released_
	    && output_keys_.contains(key))
	{
		if (auto const* cached = tile_cache_.object(key))
			output_tiles_.insert(key, *cached);
	}
	if (!(output_preparation_active_ && (atlas_.output_owned || atlas_pending_for_output_)))
	{
		cancelAtlasBuild();
		atlas_.clear();
	}
	markTileDirty(key);
	queueWindow(wanted_window_, false);
}

void OnlineRasterTemplate::insertEmptyTile(const OnlineRasterTileKey& key)
{
	auto const* tile_matrix = matrix(key.zoom);
	if (!tile_matrix)
	{
		queued_tiles_.remove(key);
		return;
	}
	insertTile(key, { {}, true, true, {} });
}

const OnlineRasterTemplate::CachedTile*
OnlineRasterTemplate::bestCachedTile(const OnlineRasterTileKey& requested,
									 OnlineRasterTileKey* cached_key, QRectF* source_rect) const
{
	auto const* requested_matrix = matrix(requested.zoom);
	if (!requested_matrix)
		return nullptr;
	if (output_preparation_active_)
	{
		auto const pinned = output_tiles_.constFind(requested);
		if (pinned != output_tiles_.cend())
		{
			if (cached_key)
				*cached_key = requested;
			if (source_rect)
			{
				*source_rect = pinned->empty
								   ? QRectF{}
								   : QRectF(1, 1, requested_matrix->tile_size.width(),
											requested_matrix->tile_size.height());
			}
			return &*pinned;
		}
	}
	if (auto const* exact = tile_cache_.object(requested))
	{
		if (cached_key)
			*cached_key = requested;
		if (source_rect)
		{
			*source_rect = exact->empty ? QRectF{}
										: QRectF(1, 1, requested_matrix->tile_size.width(),
												 requested_matrix->tile_size.height());
		}
		return exact;
	}
	for (int zoom = requested.zoom - 1; zoom >= source_->min_zoom; --zoom)
	{
		auto const shift = requested.zoom - zoom;
		OnlineRasterTileKey candidate{ zoom, requested.column >> shift, requested.row >> shift };
		auto const* cached = tile_cache_.object(candidate);
		auto const* candidate_matrix = matrix(zoom);
		if (!cached || cached->empty || !candidate_matrix)
			continue;
		auto const divisor = double(qint64(1) << shift);
		auto const width = candidate_matrix->tile_size.width() / divisor;
		auto const height = candidate_matrix->tile_size.height() / divisor;
		auto const local_column = requested.column - (candidate.column << shift);
		auto const local_row = requested.row - (candidate.row << shift);
		if (cached_key)
			*cached_key = candidate;
		if (source_rect)
		{
			*source_rect = QRectF(1 + local_column * width, 1 + local_row * height, width, height);
		}
		return cached;
	}
	return nullptr;
}

QVector<OnlineRasterTemplate::VisualTile>
OnlineRasterTemplate::visualTiles(const TileWindow& window, bool allow_provisional,
								  bool* has_transparency, bool* has_missing, bool* has_pixels) const
{
	QVector<VisualTile> result;
	if (has_transparency)
		*has_transparency = false;
	if (has_missing)
		*has_missing = false;
	if (has_pixels)
		*has_pixels = false;
	if (window.isEmpty())
		return result;
	auto const count = tileCount(window);
	if (!count || *count > std::numeric_limits<int>::max())
		return result;
	result.reserve(int(*count));
	auto touched_pixels = false;
	for (qint64 row = window.min_row; row <= window.max_row; ++row)
	{
		for (qint64 column = window.min_column; column <= window.max_column; ++column)
		{
			OnlineRasterTileKey requested{ window.zoom, column, row };
			OnlineRasterTileKey cached;
			QRectF source_rect;
			auto const pinned = output_preparation_active_
			                      ? output_tiles_.constFind(requested)
			                      : output_tiles_.cend();
			auto const* tile = pinned != output_tiles_.cend()
			                     ? &*pinned
			                     : tile_cache_.object(requested);
			if (tile)
			{
				cached = requested;
				if (tile->empty)
				{
					result.push_back({ requested, cached, tile, {}, false, true });
					continue;
				}
				auto const* requested_matrix = matrix(requested.zoom);
				source_rect = QRectF(1, 1, requested_matrix->tile_size.width(),
									 requested_matrix->tile_size.height());
			}
			else if (allow_provisional)
			{
				tile = bestCachedTile(requested, &cached, &source_rect);
			}
			if (!tile)
			{
				if (has_missing)
					*has_missing = true;
				result.push_back({ requested, {}, nullptr, {}, false, false });
				continue;
			}
			if (has_pixels)
				*has_pixels = true;
			touched_pixels = true;
			if (has_transparency && !tile->opaque)
				*has_transparency = true;
			result.push_back(
				{ requested, cached, tile, source_rect, cached.zoom != requested.zoom, false });
		}
	}
	if (touched_pixels)
		retained_access_ = nextRetainedAccess();
	return result;
}

bool OnlineRasterTemplate::appendOpaquePatches(
	const VisualTile& visual,
	double pixels_per_map_unit,
	const std::shared_ptr<MemoryReservation>& output_render_memory,
	QVector<RasterTemplateTile>& out) const
{
	if (!visual.tile)
		return false;
	auto const first = out.size();
	if (appendOpaquePatch(
		visual, visual.source_rect, pixels_per_map_unit, 0,
		output_render_memory, out))
	{
		return true;
	}
	out.resize(first);
	return false;
}

bool OnlineRasterTemplate::appendOpaquePatch(const VisualTile& visual, QRectF source_rect,
											 double pixels_per_map_unit, int depth,
											 const std::shared_ptr<MemoryReservation>& output_render_memory,
											 QVector<RasterTemplateTile>& out) const
{
	double residual = 0;
	auto transform = imageRectToMap(visual.cached, source_rect, nullptr, &residual);
	if (!transform)
		return false;
	auto const exceeds_tolerance = residual * pixels_per_map_unit > 0.35;
	auto const columns = source_rect.width() > 16 ? 2 : 1;
	auto const rows = source_rect.height() > 16 ? 2 : 1;
	if (depth < 6 && exceeds_tolerance && (columns > 1 || rows > 1))
	{
		auto const patch_width = source_rect.width() / columns;
		auto const patch_height = source_rect.height() / rows;
		for (int y = 0; y < rows; ++y)
		{
			for (int x = 0; x < columns; ++x)
			{
				auto patch = QRectF(source_rect.x() + x * patch_width,
									source_rect.y() + y * patch_height, patch_width, patch_height);
				if (!appendOpaquePatch(
					visual, patch, pixels_per_map_unit, depth + 1,
					output_render_memory, out))
				{
					return false;
				}
			}
		}
		return true;
	}
	if (exceeds_tolerance)
		return false;

	auto padded_rect = source_rect.adjusted(-0.75, -0.75, 0.75, 0.75);
	padded_rect = padded_rect.intersected(QRectF(QPointF(0, 0), QSizeF(visual.tile->image.size())));
	QRectF map_bounds;
	transform = imageRectToMap(visual.cached, padded_rect, &map_bounds, nullptr);
	if (!transform)
		return false;
	RasterMemoryReserver reserve_render_memory;
	if (output_render_memory)
	{
		reserve_render_memory =
			[memory = output_render_memory](qint64 bytes)
				-> std::shared_ptr<RasterMemoryLease> {
				if (bytes <= 0 || bytes > memory->bytes)
					return {};
				return memory;
			};
	}
	else
	{
		reserve_render_memory =
			[this](qint64 bytes)
				-> std::shared_ptr<RasterMemoryLease> {
				return reserveRetainedMemory(bytes);
			};
	}
	out.push_back({
		visual.tile->image,
		map_bounds,
		padded_rect,
		quint64(visual.tile->image.cacheKey()),
		false,
		visual.provisional,
			*transform,
			true,
			visual.tile->memory,
			std::move(reserve_render_memory),
		});
	return true;
}

QVector<quint64> OnlineRasterTemplate::atlasSignature(const TileWindow& window,
													  const QVector<VisualTile>& visuals,
													  bool has_missing) const
{
	QVector<quint64> signature;
	signature.reserve(visuals.size() + 6);
	signature.push_back(quint64(window.zoom));
	signature.push_back(quint64(window.min_column));
	signature.push_back(quint64(window.min_row));
	signature.push_back(quint64(window.max_column));
	signature.push_back(quint64(window.max_row));
	signature.push_back(has_missing ? 1 : 0);
	for (auto const& visual : visuals)
	{
		auto value = visual.tile ? quint64(visual.tile->image.cacheKey()) : 0;
		value = mixSignature(value, visual.complete_empty ? 1 : 0);
		value = mixSignature(value, quint64(visual.cached.zoom + 1));
		value = mixSignature(value, quint64(visual.cached.column));
		value = mixSignature(value, quint64(visual.cached.row));
		signature.push_back(value);
	}
	return signature;
}

QVector<OnlineRasterTemplate::TileWindow>
OnlineRasterTemplate::atlasChunks(
	const TileWindow& window,
	double pixels_per_map_unit) const
{
	QVector<TileWindow> result;
	auto const* tile_matrix = matrix(window.zoom);
	if (window.isEmpty() || !tile_matrix)
		return result;

	auto const target_pixels =
		std::max<qint64>(1, max_atlas_pixels / 4);
	auto const target_dimension = std::max<qint64>(
		1,
		std::min<qint64>(
			max_atlas_dimension - 2,
			qint64(std::floor(std::sqrt(
				double(target_pixels))))));
	auto projectedSize = [&](const TileWindow& chunk)
		-> std::optional<QSizeF> {
		if (!(pixels_per_map_unit > 0)
		    || !std::isfinite(pixels_per_map_unit))
			return std::nullopt;
		auto const top_left = tileBounds({
			chunk.zoom,
			chunk.min_column,
			chunk.min_row,
		});
		if (!top_left.isValid())
			return std::nullopt;
		auto const source_bounds = imagery::CrsBounds {
			top_left.west,
			top_left.north
				- chunk.height()
				      * tile_matrix->tile_size.height()
				      * tile_matrix->cell_size,
			top_left.west
				+ chunk.width()
				      * tile_matrix->tile_size.width()
				      * tile_matrix->cell_size,
			top_left.north,
		};
		auto const map_bounds =
			mapBoundsForSourceBounds(source_bounds);
		if (!map_bounds.isValid() || map_bounds.isEmpty())
			return std::nullopt;
		return QSizeF(
			std::ceil(
				map_bounds.width()
				* pixels_per_map_unit),
			std::ceil(
				map_bounds.height()
				* pixels_per_map_unit));
	};

	std::function<void(const TileWindow&)> appendChunk =
		[&](const TileWindow& chunk) {
			auto const source_width =
				chunk.width()
				* tile_matrix->tile_size.width();
			auto const source_height =
				chunk.height()
				* tile_matrix->tile_size.height();
			auto const projected = projectedSize(chunk);
			auto const projected_width =
				projected
					? projected->width()
					: 0;
			auto const projected_height =
				projected
					? projected->height()
					: 0;
			auto const source_fits =
				source_width <= target_dimension
				&& source_height <= target_dimension
				&& source_width
					   <= target_pixels
						      / std::max<qint64>(
							      1, source_height);
			auto const projected_fits =
				!projected
				|| (projected_width
					    <= target_dimension
				    && projected_height
					       <= target_dimension
				    && projected_width
					       <= double(target_pixels)
						      / std::max(
							      1.0,
							      projected_height));
			if ((source_fits && projected_fits)
			    || (chunk.width() == 1
			        && chunk.height() == 1))
			{
				result.push_back(chunk);
				return;
			}

			auto const column_pressure = std::max(
				double(source_width)
					/ target_dimension,
				projected_width
					/ target_dimension);
			auto const row_pressure = std::max(
				double(source_height)
					/ target_dimension,
				projected_height
					/ target_dimension);
			if (chunk.width() > 1
			    && (chunk.height() == 1
			        || column_pressure >= row_pressure))
			{
				auto const middle =
					chunk.min_column
					+ (chunk.max_column
					   - chunk.min_column)
						  / 2;
				appendChunk({
					chunk.zoom,
					chunk.min_column,
					middle,
					chunk.min_row,
					chunk.max_row,
				});
				appendChunk({
					chunk.zoom,
					middle + 1,
					chunk.max_column,
					chunk.min_row,
					chunk.max_row,
				});
			}
			else
			{
				auto const middle =
					chunk.min_row
					+ (chunk.max_row
					   - chunk.min_row)
						  / 2;
				appendChunk({
					chunk.zoom,
					chunk.min_column,
					chunk.max_column,
					chunk.min_row,
					middle,
				});
				appendChunk({
					chunk.zoom,
					chunk.min_column,
					chunk.max_column,
					middle + 1,
					chunk.max_row,
				});
			}
		};
	appendChunk(window);
	return result;
}

std::optional<OnlineRasterTemplate::AtlasBuildRequest> OnlineRasterTemplate::makeAtlasBuildRequest(
	const TileWindow& window, const QVector<VisualTile>& visuals, bool has_missing,
	double pixels_per_map_unit, const QVector<quint64>& signature) const
{
	auto const* tile_matrix = matrix(window.zoom);
	if (!tile_matrix || !(pixels_per_map_unit > 0) || !std::isfinite(pixels_per_map_unit))
	{
		return std::nullopt;
	}
	auto const tile_count = tileCount(window);
	auto const tile_width = qint64(tile_matrix->tile_size.width());
	auto const tile_height = qint64(tile_matrix->tile_size.height());
	if (!tile_count || tile_width <= 0 || tile_height <= 0
		|| window.width() > std::numeric_limits<qint64>::max() / tile_width
		|| window.height() > std::numeric_limits<qint64>::max() / tile_height)
	{
		return std::nullopt;
	}
	auto const width = window.width() * tile_width;
	auto const height = window.height() * tile_height;
	if (width <= 0 || height <= 0
	    || width > max_atlas_dimension - 2
	    || height > max_atlas_dimension - 2
	    || width + 2 > max_atlas_pixels / (height + 2)
	    || width > std::numeric_limits<int>::max() - 2
	    || height > std::numeric_limits<int>::max() - 2)
	{
		return std::nullopt;
	}

	AtlasBuildRequest request;
	request.window = window;
	request.signature = signature;
	request.core_size = { int(width), int(height) };
	request.pixels_per_map_unit = pixels_per_map_unit;
	bool provisional = has_missing;
	for (auto const& visual : visuals)
	{
		request.has_left_neighbor |=
			visual.requested.column < window.min_column;
		request.has_right_neighbor |=
			visual.requested.column > window.max_column;
		request.has_top_neighbor |=
			visual.requested.row < window.min_row;
		request.has_bottom_neighbor |=
			visual.requested.row > window.max_row;
		if (!visual.tile || visual.complete_empty)
			continue;
		request.visuals.push_back({
			QRectF(1 + (visual.requested.column - window.min_column)
				         * tile_matrix->tile_size.width(),
				   1 + (visual.requested.row - window.min_row)
				         * tile_matrix->tile_size.height(),
				   tile_matrix->tile_size.width(), tile_matrix->tile_size.height()),
				visual.tile->image,
				visual.source_rect,
				visual.tile->memory,
			});
		provisional |= visual.provisional;
	}
	request.provisional = provisional;

	auto const top_left_key = OnlineRasterTileKey{ window.zoom, window.min_column, window.min_row };
	auto const bounds = tileBounds(top_left_key);
	if (!bounds.isValid())
		return std::nullopt;
	auto const core_west = bounds.west;
	auto const core_north = bounds.north;
	auto const core_east =
		bounds.west + window.width() * tile_matrix->tile_size.width() * tile_matrix->cell_size;
	auto const core_south =
		bounds.north - window.height() * tile_matrix->tile_size.height() * tile_matrix->cell_size;
	auto const source_west = core_west - tile_matrix->cell_size;
	auto const source_north = core_north + tile_matrix->cell_size;
	auto const source_east = core_east + tile_matrix->cell_size;
	auto const source_south = core_south - tile_matrix->cell_size;
	auto const map_top_left = nominalSourceToMap({ source_west, source_north });
	auto const map_top_right = nominalSourceToMap({ source_east, source_north });
	auto const map_bottom_left = nominalSourceToMap({ source_west, source_south });
	auto const map_bottom_right = nominalSourceToMap({ source_east, source_south });
	if (!map_top_left || !map_top_right || !map_bottom_left || !map_bottom_right)
		return std::nullopt;

	auto const padded_size = QSize(int(width) + 2, int(height) + 2);
	QTransform transform((map_top_right->x() - map_top_left->x()) / padded_size.width(),
						 (map_top_right->y() - map_top_left->y()) / padded_size.width(),
						 (map_bottom_left->x() - map_top_left->x()) / padded_size.height(),
						 (map_bottom_left->y() - map_top_left->y()) / padded_size.height(),
						 map_top_left->x(), map_top_left->y());
	if (!transform.isInvertible())
		return std::nullopt;

	double residual = 0;
	for (int y = 0; y <= 4; ++y)
	{
		for (int x = 0; x <= 4; ++x)
		{
			auto const sample =
				QPointF(padded_size.width() * x / 4.0, padded_size.height() * y / 4.0);
			auto const actual =
				nominalSourceToMap({ source_west + sample.x() * tile_matrix->cell_size,
									 source_north - sample.y() * tile_matrix->cell_size });
			if (!actual)
				return std::nullopt;
			residual = std::max(residual, pointDistance(transform.map(sample), *actual));
		}
	}

	if (residual * pixels_per_map_unit <= 0.35)
	{
		auto const padded_bytes = rgbaImageBytes(width + 2, height + 2);
		if (!padded_bytes
		    || *padded_bytes > max_atlas_peak_bytes)
			return std::nullopt;
		request.working_memory =
			reserveRetainedMemory(*padded_bytes);
		if (!request.working_memory)
				return std::nullopt;
		request.image_to_map = transform;
		request.map_bounds =
			boundsOf({ *map_top_left, *map_top_right, *map_bottom_left, *map_bottom_right });
		return request;
	}

	auto map_bounds = mapBoundsForSourceBounds({ core_west, core_south, core_east, core_north });
	if (!map_bounds.isValid() || map_bounds.isEmpty())
		return std::nullopt;
	auto const output_width = qint64(std::ceil(map_bounds.width() * pixels_per_map_unit));
	auto const output_height = qint64(std::ceil(map_bounds.height() * pixels_per_map_unit));
	if (output_width <= 0 || output_height <= 0 || output_width > max_atlas_dimension
		|| output_height > max_atlas_dimension || output_width > max_atlas_pixels / output_height
		|| output_width > std::numeric_limits<int>::max()
		|| output_height > std::numeric_limits<int>::max())
	{
		return std::nullopt;
	}
	auto const source_bytes = rgbaImageBytes(width + 2, height + 2);
	auto const output_bytes = rgbaImageBytes(output_width, output_height);
	auto const padded_bytes = rgbaImageBytes(output_width + 2, output_height + 2);
	if (!source_bytes || !output_bytes || !padded_bytes
	    || *source_bytes > max_atlas_peak_bytes - *output_bytes
	    || *source_bytes + *output_bytes
	           > max_atlas_peak_bytes - *padded_bytes)
		return std::nullopt;
	request.working_memory = reserveRetainedMemory(
		*source_bytes + *output_bytes + *padded_bytes);
	if (!request.working_memory)
			return std::nullopt;

	AtlasWarpGrid accepted_grid;
	for (int cells = 8; cells <= 64; cells *= 2)
	{
		AtlasWarpGrid grid;
		grid.columns = cells;
		grid.rows = cells;
		grid.output_size = { int(output_width), int(output_height) };
		grid.source_points.reserve((cells + 1) * (cells + 1));
		bool valid = true;
		for (int y = 0; y <= cells && valid; ++y)
		{
			for (int x = 0; x <= cells; ++x)
			{
				auto const map_point = QPointF(map_bounds.left() + map_bounds.width() * x / cells,
											   map_bounds.top() + map_bounds.height() * y / cells);
				auto const source_point = mapToNominalSource(map_point);
				if (!source_point)
				{
					valid = false;
					break;
				}
					grid.source_points.push_back({
						1 + (source_point->x() - core_west)
						        / tile_matrix->cell_size,
						1 + (core_north - source_point->y())
						        / tile_matrix->cell_size,
					});
			}
		}
		if (!valid)
			continue;

		double maximum_error = 0;
		for (int y = 0; y < cells && valid; ++y)
		{
			for (int x = 0; x < cells; ++x)
			{
				auto const map_point =
					QPointF(map_bounds.left() + map_bounds.width() * (x + 0.5) / cells,
							map_bounds.top() + map_bounds.height() * (y + 0.5) / cells);
				auto const exact_source = mapToNominalSource(map_point);
				if (!exact_source)
				{
					valid = false;
					break;
				}
					auto const exact =
						QPointF(
							1 + (exact_source->x() - core_west)
							        / tile_matrix->cell_size,
							1 + (core_north - exact_source->y())
							        / tile_matrix->cell_size);
				auto const row_stride = cells + 1;
				auto const interpolated = 0.25
										  * (grid.source_points.at(y * row_stride + x)
											 + grid.source_points.at(y * row_stride + x + 1)
											 + grid.source_points.at((y + 1) * row_stride + x)
											 + grid.source_points.at((y + 1) * row_stride + x + 1));
				maximum_error = std::max(maximum_error, pointDistance(exact, interpolated));
			}
		}
		if (valid && maximum_error <= 0.25)
		{
			grid.maximum_interpolation_error = maximum_error;
			accepted_grid = std::move(grid);
			break;
		}
	}
	if (accepted_grid.columns == 0)
		return std::nullopt;

	auto const x_scale = map_bounds.width() / output_width;
	auto const y_scale = map_bounds.height() / output_height;
	auto const& map_georeferencing = map->getGeoreferencing();
	accepted_grid.exact_ownership = true;
	accepted_grid.map_crs =
		map_georeferencing.getProjectedCRSSpec();
	accepted_grid.source_crs = source_->tile_matrix_set.crs;
	accepted_grid.map_to_projected =
		map_georeferencing.mapToProjected();
	accepted_grid.map_bounds = map_bounds;
	accepted_grid.source_registration = source_->registration
		? QPointF(
			source_->registration->dx,
			source_->registration->dy)
		: QPointF {};
	accepted_grid.core_west = core_west;
	accepted_grid.core_north = core_north;
	accepted_grid.cell_size = tile_matrix->cell_size;
	request.warp = std::move(accepted_grid);
	request.image_to_map =
		QTransform(x_scale, 0, 0, y_scale, map_bounds.left() - x_scale, map_bounds.top() - y_scale);
	request.map_bounds = map_bounds.adjusted(-x_scale, -y_scale, x_scale, y_scale);
	return request;
}

std::optional<OnlineRasterTemplate::AtlasBuildResult>
OnlineRasterTemplate::buildAtlas(AtlasBuildRequest request,
								 const std::shared_ptr<std::atomic_bool>& cancelled,
								 const RasterResourceManager::CancellationToken& cancellation)
{
	auto is_cancelled = [&] {
		return cancellation.isCancelled() || cancelled->load(std::memory_order_relaxed);
	};
	if (is_cancelled())
		return std::nullopt;

	QImage source(
		request.core_size + QSize(2, 2),
		QImage::Format_RGBA8888_Premultiplied);
	if (source.isNull())
		return std::nullopt;
	source.fill(Qt::transparent);
	{
		QPainter painter(&source);
		painter.setCompositionMode(QPainter::CompositionMode_Source);
		painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
		for (auto const& visual : request.visuals)
		{
			if (is_cancelled())
				return std::nullopt;
			painter.drawImage(visual.target_rect, visual.image, visual.source_rect);
		}
	}
	auto copyPixel = [&source](int destination_x,
	                           int destination_y,
	                           int source_x,
	                           int source_y) {
		std::copy_n(
			source.constScanLine(source_y)
				+ 4 * source_x,
			4,
			source.scanLine(destination_y)
				+ 4 * destination_x);
	};
	if (!request.has_left_neighbor)
	{
		for (int y = 0; y < source.height(); ++y)
			copyPixel(0, y, 1, y);
	}
	if (!request.has_right_neighbor)
	{
		for (int y = 0; y < source.height(); ++y)
			copyPixel(
				source.width() - 1,
				y,
				source.width() - 2,
				y);
	}
	if (!request.has_top_neighbor)
	{
		for (int x = 0; x < source.width(); ++x)
			copyPixel(x, 0, x, 1);
	}
	if (!request.has_bottom_neighbor)
	{
		for (int x = 0; x < source.width(); ++x)
			copyPixel(
				x,
				source.height() - 1,
				x,
				source.height() - 2);
	}

	QImage output;
	if (!request.warp)
	{
		output = std::move(source);
	}
	else
	{
		auto const& grid = *request.warp;
		output = QImage(grid.output_size, QImage::Format_RGBA8888_Premultiplied);
		if (output.isNull())
			return std::nullopt;
		output.fill(Qt::transparent);
		auto const row_stride = grid.columns + 1;
		std::optional<ProjTransform> map_projection;
		std::optional<ProjTransform> source_projection;
		if (grid.exact_ownership)
		{
			map_projection.emplace(grid.map_crs);
			source_projection.emplace(grid.source_crs);
			if (!map_projection->isValid()
			    || !source_projection->isValid()
			    || !grid.map_to_projected.isInvertible()
			    || !grid.map_bounds.isValid()
			    || grid.map_bounds.isEmpty()
			    || !(grid.cell_size > 0)
			    || !std::isfinite(grid.cell_size))
			{
				return std::nullopt;
			}
		}
		auto sample = [&source](int x, int y, int channel) {
			if (x < 0 || y < 0 || x >= source.width() || y >= source.height())
			{
				return 0.0;
			}
			return double(source.constScanLine(y)[4 * x + channel]);
		};
		for (int y = 0; y < output.height(); ++y)
		{
			if (is_cancelled())
				return std::nullopt;
			auto* destination = output.scanLine(y);
			auto const grid_y = (y + 0.5) * grid.rows / output.height();
			auto const cell_y = std::min(grid.rows - 1, int(std::floor(grid_y)));
			auto const fraction_y = grid_y - cell_y;
			for (int x = 0; x < output.width(); ++x)
			{
				auto const grid_x = (x + 0.5) * grid.columns / output.width();
				auto const cell_x = std::min(grid.columns - 1, int(std::floor(grid_x)));
				auto const fraction_x = grid_x - cell_x;
				auto const top_left = grid.source_points.at(cell_y * row_stride + cell_x);
				auto const top_right = grid.source_points.at(cell_y * row_stride + cell_x + 1);
				auto const bottom_left = grid.source_points.at((cell_y + 1) * row_stride + cell_x);
				auto const bottom_right =
					grid.source_points.at((cell_y + 1) * row_stride + cell_x + 1);
					auto const top = top_left * (1 - fraction_x) + top_right * fraction_x;
					auto const bottom = bottom_left * (1 - fraction_x) + bottom_right * fraction_x;
					auto const source_point =
						top * (1 - fraction_y)
						+ bottom * fraction_y;
					auto ownership_point = source_point;
					if (grid.exact_ownership)
					{
						auto const right =
							request.core_size.width() + 1.0;
						auto const bottom_edge =
							request.core_size.height() + 1.0;
						auto const edge_distance = std::min({
							std::abs(source_point.x() - 1),
							std::abs(source_point.x() - right),
							std::abs(source_point.y() - 1),
							std::abs(source_point.y() - bottom_edge),
						});
						auto const exact_margin = std::max(
							1.0,
							grid.maximum_interpolation_error + 0.75);
						if (edge_distance <= exact_margin)
						{
							auto const map_point = QPointF(
								grid.map_bounds.left()
									+ grid.map_bounds.width()
										* (x + 0.5)
										/ output.width(),
								grid.map_bounds.top()
									+ grid.map_bounds.height()
										* (y + 0.5)
										/ output.height());
							bool inverse_ok = false;
							bool forward_ok = false;
							auto const lat_lon =
								map_projection->inverse(
									grid.map_to_projected.map(
										map_point),
									&inverse_ok);
							auto nominal_source =
								source_projection->forward(
									lat_lon, &forward_ok);
							if (!inverse_ok || !forward_ok
							    || !std::isfinite(
								   nominal_source.x())
							    || !std::isfinite(
								   nominal_source.y()))
							{
								continue;
							}
							nominal_source -=
								grid.source_registration;
							ownership_point = {
								1
									+ (nominal_source.x()
									   - grid.core_west)
										/ grid.cell_size,
								1
									+ (grid.core_north
									   - nominal_source.y())
										/ grid.cell_size,
							};
						}
					}
					// Neighbor pixels exist only as bilinear filter support.
					// Exact inverse ownership near every edge makes adjacent
					// chunks share one half-open nominal-source partition.
					if (ownership_point.x() < 1
					    || ownership_point.y() < 1
					    || ownership_point.x()
					           >= request.core_size.width() + 1
					    || ownership_point.y()
					           >= request.core_size.height() + 1)
					{
						continue;
					}
					auto const source_x = source_point.x() - 0.5;
				auto const source_y = source_point.y() - 0.5;
				auto const x0 = int(std::floor(source_x));
				auto const y0 = int(std::floor(source_y));
				auto const fx = source_x - x0;
				auto const fy = source_y - y0;
				for (int channel = 0; channel < 4; ++channel)
				{
					auto const top_sample =
						sample(x0, y0, channel) * (1 - fx) + sample(x0 + 1, y0, channel) * fx;
					auto const bottom_sample = sample(x0, y0 + 1, channel) * (1 - fx)
											   + sample(x0 + 1, y0 + 1, channel) * fx;
					destination[4 * x + channel] = uchar(std::clamp(
						int(std::lround(top_sample * (1 - fy) + bottom_sample * fy)), 0, 255));
				}
			}
		}
	}
	auto padded = request.warp
		? addGutter(std::move(output))
		: std::move(output);
	if (padded.isNull() || is_cancelled())
		return std::nullopt;
	return AtlasBuildResult{
		request.window,
		std::move(request.signature),
		std::move(padded),
		request.image_to_map,
		request.map_bounds,
		request.pixels_per_map_unit,
		request.provisional,
		std::move(request.working_memory),
	};
}

void OnlineRasterTemplate::cancelAtlasBuild(bool clear_failure) const
{
	atlas_owner_.invalidate();
	if (atlas_cancelled_)
		atlas_cancelled_->store(true, std::memory_order_relaxed);
	atlas_cancelled_.reset();
	atlas_pending_signature_.clear();
	atlas_pending_scale_ = 0;
	atlas_pending_for_output_ = false;
	atlas_queue_busy_ = false;
	atlas_retry_timer_.stop();
	if (clear_failure)
	{
		atlas_failed_signature_.clear();
		atlas_failed_scale_ = 0;
	}
}

bool OnlineRasterTemplate::queueAtlasBuild(const TileWindow& window,
										   const QVector<VisualTile>& visuals, bool has_missing,
										   double pixels_per_map_unit,
										   const QVector<quint64>& signature, bool for_output) const
{
	auto* receiver = const_cast<OnlineRasterTemplate*>(this);
	auto scale_matches = [](double first, double second) {
		auto const scale = std::max({ 1.0, std::abs(first), std::abs(second) });
		return std::abs(first - second) <= scale * 1.0e-9;
	};
	if (atlas_cancelled_ && atlas_pending_signature_ == signature
		&& scale_matches(atlas_pending_scale_, pixels_per_map_unit))
	{
		atlas_queue_busy_ = false;
		atlas_pending_for_output_ |= for_output;
		return true;
	}
	if (atlas_failed_signature_ == signature
		&& scale_matches(atlas_failed_scale_, pixels_per_map_unit))
	{
		return false;
	}

	cancelAtlasBuild(false);
	atlas_queue_busy_ = false;
	auto request =
		makeAtlasBuildRequest(window, visuals, has_missing, pixels_per_map_unit, signature);
	if (!request)
	{
		atlas_failed_signature_ = signature;
		atlas_failed_scale_ = pixels_per_map_unit;
		receiver->updateResourceStatus();
		return false;
	}

	auto cancelled = std::make_shared<std::atomic_bool>(false);
	atlas_cancelled_ = cancelled;
	atlas_pending_signature_ = signature;
	atlas_pending_scale_ = pixels_per_map_unit;
	atlas_pending_for_output_ = for_output;
	auto const accepted = RasterResourceManager::instance().submit(
		atlas_owner_, RasterResourceManager::Lane::Decode, RasterResourceManager::Priority::Visible,
		receiver,
		[request = std::move(*request), cancelled,
		 receiver](const RasterResourceManager::CancellationToken& cancellation) mutable {
			auto result = buildAtlas(std::move(request), cancelled, cancellation);
			return RasterResourceManager::Completion{ [receiver, result = std::move(result),
													   cancelled]() mutable {
				receiver->finishAtlasBuild(std::move(result), cancelled);
			} };
		});
	if (!accepted)
	{
		cancelAtlasBuild(false);
		atlas_queue_busy_ = true;
		atlas_retry_timer_.start(100);
		receiver->updateResourceStatus();
		return false;
	}
	receiver->updateResourceStatus();
	return true;
}

void OnlineRasterTemplate::finishAtlasBuild(std::optional<AtlasBuildResult> result,
											const std::shared_ptr<std::atomic_bool>& cancelled)
{
	if (atlas_cancelled_ != cancelled)
		return;
	auto const pending_signature = atlas_pending_signature_;
	auto const pending_scale = atlas_pending_scale_;
	auto const pending_for_output = atlas_pending_for_output_;
	atlas_cancelled_.reset();
	atlas_pending_signature_.clear();
	atlas_pending_scale_ = 0;
	atlas_pending_for_output_ = false;
	atlas_queue_busy_ = false;
	if (cancelled->load(std::memory_order_relaxed))
	{
		updateResourceStatus();
		return;
	}
	if (!result)
	{
		atlas_failed_signature_ = pending_signature;
		atlas_failed_scale_ = pending_scale;
		updateResourceStatus();
		return;
	}
	auto const retained_bytes =
		qint64(result->image.bytesPerLine()) * result->image.height();
	auto memory = std::move(result->working_memory);
	if (!memory)
	{
		if (pending_for_output)
		{
			output_preparation_error_ = tr(
				"Exact translucent imagery cannot fit within the "
				"application-wide raster memory budget.");
			updateResourceStatus();
			return;
		}
		atlas_queue_busy_ = true;
		atlas_retry_timer_.start(250);
		updateResourceStatus();
		return;
	}
	memory->shrinkTo(retained_bytes);

	AtlasCache completed;
	completed.window = result->window;
	completed.signature = std::move(result->signature);
	completed.image = std::move(result->image);
	completed.image_to_map = result->image_to_map;
	completed.map_bounds = result->map_bounds;
	completed.pixels_per_map_unit =
		result->pixels_per_map_unit;
	completed.provisional = result->provisional;
	completed.output_owned = pending_for_output;
	completed.memory = std::move(memory);
	auto const dirty_bounds = completed.map_bounds;
	if (pending_for_output)
	{
		auto found = std::ranges::find(
			output_atlases_, completed.window,
			&AtlasCache::window);
		if (found == output_atlases_.end())
			output_atlases_.push_back(std::move(completed));
		else
			*found = std::move(completed);
	}
	else
	{
		atlas_ = std::move(completed);
	}
	retained_access_ = nextRetainedAccess();
	atlas_failed_signature_.clear();
	atlas_failed_scale_ = 0;
	if (dirty_bounds.isValid())
		map->setTemplateAreaDirty(
			this, dirty_bounds,
			getTemplateBoundingBoxPixelBorder());
	updateResourceStatus();
}

bool OnlineRasterTemplate::appendTransparentAtlas(const TileWindow& window,
												  const QVector<VisualTile>& visuals,
												  bool has_missing, double pixels_per_map_unit,
												  bool on_screen,
												  QVector<RasterTemplateTile>& out) const
{
	auto const signature = atlasSignature(window, visuals, has_missing);
	auto const scale =
		std::max({ 1.0, std::abs(atlas_.pixels_per_map_unit), std::abs(pixels_per_map_unit) });
	auto const scale_matches =
		std::abs(atlas_.pixels_per_map_unit - pixels_per_map_unit) <= scale * 1.0e-9;
	if (atlas_.window != window || atlas_.signature != signature || atlas_.image.isNull()
		|| !scale_matches)
	{
		if (on_screen && output_preparation_active_
		    && (output_uses_atlases_
		        || atlas_pending_for_output_))
		{
			return false;
		}
		queueAtlasBuild(window, visuals, has_missing, pixels_per_map_unit, signature,
						!on_screen && output_preparation_active_);
		return false;
	}
	auto const source_rect = QRectF(
		1, 1,
		std::max(0, atlas_.image.width() - 2),
		std::max(0, atlas_.image.height() - 2));
	out.push_back({
		atlas_.image,
		atlas_.image_to_map.mapRect(source_rect),
		source_rect,
		quint64(atlas_.image.cacheKey()),
		false,
		atlas_.provisional,
			atlas_.image_to_map,
			true,
			atlas_.memory,
			[this](qint64 bytes)
				-> std::shared_ptr<RasterMemoryLease> {
				return reserveRetainedMemory(bytes);
			},
		});
	return true;
}

bool OnlineRasterTemplate::appendPreparedOutputAtlases(
	const TileWindow& window,
	double pixels_per_map_unit,
	QVector<RasterTemplateTile>& out) const
{
	if (!output_preparation_active_
	    || !output_uses_atlases_
	    || !output_window_.contains(window))
		return false;

	for (auto const& cached : std::as_const(output_atlases_))
	{
		if (!cached.window.intersects(window))
			continue;
		auto const scale = std::max({
			1.0,
			std::abs(cached.pixels_per_map_unit),
			std::abs(pixels_per_map_unit),
		});
			if (!cached.output_owned
			    || cached.image.isNull()
			    || cached.provisional
			    || !cached.render_memory
			    || std::abs(
			       cached.pixels_per_map_unit
			       - pixels_per_map_unit)
			       > scale * 1.0e-9)
			return false;
		auto const source_rect = QRectF(
			1, 1,
			std::max(0, cached.image.width() - 2),
			std::max(0, cached.image.height() - 2));
		out.push_back({
			cached.image,
			cached.image_to_map.mapRect(source_rect),
			source_rect,
			quint64(cached.image.cacheKey()),
			false,
			false,
			cached.image_to_map,
			true,
			cached.memory,
			[render_memory = cached.render_memory](
				qint64 bytes)
				-> std::shared_ptr<RasterMemoryLease> {
				if (!render_memory
				    || bytes <= 0
				    || bytes > render_memory->bytes)
					return {};
				return render_memory;
			},
		});
	}
	retained_access_ = nextRetainedAccess();
	return true;
}

void OnlineRasterTemplate::collectRasterTiles(const QRectF& map_clip_rect, double scale,
											  bool on_screen,
											  QVector<RasterTemplateTile>& out) const
{
	if (template_state != Loaded || !sourceReady())
		return;
	auto clipped_map_rect = map_clip_rect;
	if (on_screen)
	{
		auto const screen_bounds = onScreenMapBounds();
		if (screen_bounds.isValid())
			clipped_map_rect = clipped_map_rect.intersected(screen_bounds);
		if (clipped_map_rect.isEmpty())
			return;
	}
	auto zoom = on_screen && !wanted_window_.isEmpty()
					? wanted_window_.zoom
					: (!on_screen && output_preparation_active_ && !output_window_.isEmpty()
						   ? output_window_.zoom
						   : chooseZoom(clipped_map_rect, scale, !on_screen));
	if (zoom < 0)
	{
		out.push_back({ {}, map_clip_rect, {}, 0, true, false });
		return;
	}
	auto const window = on_screen && !wanted_window_.isEmpty()
					? wanted_window_
					: tileWindowForMapRect(clipped_map_rect, zoom, !on_screen);
	if (window.isEmpty())
		return;
	if (!on_screen
	    && appendPreparedOutputAtlases(
		    window, scale, out))
	{
		last_render_bounds_ = map_clip_rect;
		return;
	}

	bool has_transparency = false;
	bool has_missing = false;
	bool has_pixels = false;
	auto const visuals =
		visualTiles(window, on_screen, &has_transparency, &has_missing, &has_pixels);
	if (!has_pixels)
	{
		for (auto const& visual : visuals)
		{
			if (visual.complete_empty)
				continue;
			out.push_back({
				{},
				mapBoundsForSourceBounds(tileBounds(visual.requested)),
				{},
				0,
				true,
				false,
			});
		}
		return;
	}

	if (has_transparency)
	{
		if (appendTransparentAtlas(window, visuals, has_missing, scale, on_screen, out))
		{
			last_render_bounds_ = atlas_.map_bounds;
			return;
		}
		// A pathological atlas size remains incomplete rather than exposing
		// translucent per-tile antialias seams.
		out.push_back({
			{},
			mapBoundsForSourceBounds(
				tileBounds({ window.zoom, window.min_column, window.min_row })),
			{},
			0,
			true,
			false,
		});
		return;
	}

	QRectF render_bounds;
	for (auto const& visual : visuals)
	{
		if (visual.complete_empty)
			continue;
		if (!visual.tile)
		{
			auto const bounds = mapBoundsForSourceBounds(tileBounds(visual.requested));
			out.push_back({ {}, bounds, {}, 0, true, false });
			rectIncludeSafe(render_bounds, bounds);
			continue;
		}
		auto const before = out.size();
		std::shared_ptr<MemoryReservation> output_render_memory;
		if (!on_screen && output_preparation_active_)
		{
			auto const found =
				output_render_memory_.constFind(visual.cached);
			if (found == output_render_memory_.cend() || !*found)
			{
				auto const bounds =
					mapBoundsForSourceBounds(
						tileBounds(visual.requested));
				out.push_back(
					{ {}, bounds, {}, 0, true, false });
				rectIncludeSafe(render_bounds, bounds);
				continue;
			}
			output_render_memory = *found;
		}
		if (!appendOpaquePatches(
			visual, scale, output_render_memory, out))
		{
			auto const bounds = mapBoundsForSourceBounds(tileBounds(visual.requested));
			out.push_back({ {}, bounds, {}, 0, true, false });
			rectIncludeSafe(render_bounds, bounds);
			continue;
		}
		for (auto index = before; index < out.size(); ++index)
			rectIncludeSafe(render_bounds, out.at(index).template_rect);
	}
	last_render_bounds_ = render_bounds;
}

QRectF OnlineRasterTemplate::mapBoundsForSourceBounds(const imagery::CrsBounds& bounds) const
{
	if (!bounds.isValid())
		return {};
	QRectF result;
	for (int index = 0; index <= 32; ++index)
	{
		auto const fraction = index / 32.0;
		for (auto const point : {
				 QPointF(bounds.west + (bounds.east - bounds.west) * fraction, bounds.north),
				 QPointF(bounds.west + (bounds.east - bounds.west) * fraction, bounds.south),
				 QPointF(bounds.west, bounds.south + (bounds.north - bounds.south) * fraction),
				 QPointF(bounds.east, bounds.south + (bounds.north - bounds.south) * fraction),
			 })
		{
			if (auto mapped = nominalSourceToMap(point))
				rectIncludeSafe(result, *mapped);
		}
	}
	return result;
}

QRectF OnlineRasterTemplate::sourceMapBounds() const
{
	if (!sourceReady())
		return last_render_bounds_;
	auto const* tile_matrix = matrix(source_->min_zoom);
	if (!tile_matrix)
		return last_render_bounds_;
	qint64 min_column = 0;
	qint64 max_column = tile_matrix->matrix_width - 1;
	qint64 min_row = 0;
	qint64 max_row = tile_matrix->matrix_height - 1;
	if (auto const* tile_limits = limits(source_->min_zoom))
	{
		min_column = tile_limits->min_column;
		max_column = tile_limits->max_column;
		min_row = tile_limits->min_row;
		max_row = tile_limits->max_row;
	}
	auto const first = tile_matrix->tileBounds(min_column, min_row);
	auto const last = tile_matrix->tileBounds(max_column, max_row);
	return mapBoundsForSourceBounds({ first.west, last.south, last.east, first.north });
}

QRectF OnlineRasterTemplate::onScreenMapBounds() const
{
	auto const source_bounds = sourceMapBounds();
	if (!map)
		return source_bounds;
	auto const content_bounds = map->calculateExtent(false, false).normalized();
	if (!content_bounds.isValid() || content_bounds.isEmpty())
		return source_bounds;
	auto const padded = content_bounds.adjusted(
		-content_bounds.width() * 0.2,
		-content_bounds.height() * 0.2,
		content_bounds.width() * 0.2,
		content_bounds.height() * 0.2
	);
	return source_bounds.isValid() ? source_bounds.intersected(padded) : padded;
}

QRectF OnlineRasterTemplate::calculateTemplateBoundingBox() const
{
	return onScreenMapBounds();
}

QRectF OnlineRasterTemplate::getRasterRenderClip(bool on_screen) const
{
	return on_screen ? onScreenMapBounds() : QRectF {};
}

QRectF OnlineRasterTemplate::getTemplateExtent() const
{
	return calculateTemplateBoundingBox();
}

void OnlineRasterTemplate::markTileDirty(const OnlineRasterTileKey& key)
{
	auto const bounds = mapBoundsForSourceBounds(tileBounds(key));
	if (bounds.isValid())
		map->setTemplateAreaDirty(this, bounds, getTemplateBoundingBoxPixelBorder());
}

void OnlineRasterTemplate::onMapGeoreferencingChanged()
{
	if (template_state != Loaded)
		return;
	auto const old_bounds = last_render_bounds_;
	cancelAtlasBuild();
	atlas_.clear();
	wanted_window_ = {};
	if (old_bounds.isValid())
		map->setTemplateAreaDirty(this, old_bounds, getTemplateBoundingBoxPixelBorder());
	auto const new_bounds = calculateTemplateBoundingBox();
	if (new_bounds.isValid())
		map->setTemplateAreaDirty(this, new_bounds, getTemplateBoundingBoxPixelBorder());
}

} // namespace OpenOrienteering
