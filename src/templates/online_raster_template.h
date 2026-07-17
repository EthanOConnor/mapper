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

#ifndef OPENORIENTEERING_ONLINE_RASTER_TEMPLATE_H
#define OPENORIENTEERING_ONLINE_RASTER_TEMPLATE_H

#include <atomic>
#include <memory>
#include <optional>
#include <utility>

#include <QCache>
#include <QDeadlineTimer>
#include <QHash>
#include <QImage>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QTimer>
#include <QTransform>
#include <QVector>

#include "imagery/imagery_source_snapshot.h"
#include "imagery/tile_network_manager.h"
#include "templates/raster_resource_manager.h"
#include "templates/template_image.h"

class QXmlStreamReader;
class QXmlStreamWriter;

namespace OpenOrienteering {

class Map;
struct ProjTransform;
class OnlineRasterTemplateTest;

struct OnlineRasterTileKey
{
	int zoom = -1;
	qint64 column = 0;
	qint64 row = 0;

	auto operator<=>(const OnlineRasterTileKey&) const = default;
};

inline size_t qHash(const OnlineRasterTileKey& key, size_t seed = 0)
{
	seed = ::qHash(key.zoom, seed);
	seed = ::qHash(key.column, seed);
	return ::qHash(key.row, seed);
}

/**
 * Native tiled imagery template backed by an embedded resolved source.
 *
 * Network I/O is shared and bounded by imagery::TileNetworkManager. Image
 * decoding and gutter construction use RasterResourceManager's decode lane.
 * Opaque tiles render as direct affine patches; windows containing alpha are
 * first composed into one retained atlas to preserve alpha across tile seams.
 */
class OnlineRasterTemplate final : public TemplateImage
{
	Q_OBJECT

  public:
	explicit OnlineRasterTemplate(imagery::ImagerySourceSnapshot snapshot, Map* map,
								  imagery::TileNetworkManager* network = nullptr);
	~OnlineRasterTemplate() override;

	static std::unique_ptr<OnlineRasterTemplate> createForType(const QString& path, Map* map);

	const char* getTemplateType() const override;
	bool fileExists() const override;
	LookupResult tryToFindTemplateFile(const QString& map_path) override;
	QSize getRasterPixelSize() const override;
	QRectF calculateTemplateBoundingBox() const override;
	QRectF getRasterRenderClip(bool on_screen) const override;

	const imagery::ImagerySourceSnapshot* sourceSnapshot() const noexcept;
	void setDisplayName(const QString& name);
	bool sourceReady() const noexcept;
	OutputRenderPreparation prepareForOutput(const QRectF& map_rect,
											 double pixels_per_map_unit) override;
	void finishOutputPreparation(bool cancelled) override;

  protected:
	OnlineRasterTemplate(const OnlineRasterTemplate& prototype);
	OnlineRasterTemplate* duplicate() const override;

	bool loadTemplateFileImpl() override;
	bool postLoadSetup(QWidget* dialog_parent, bool& out_center_in_view) override;
	void unloadTemplateFileImpl() override;
	void saveTypeSpecificTemplateConfiguration(QXmlStreamWriter& xml) const override;
	bool loadTypeSpecificTemplateConfiguration(QXmlStreamReader& xml) override;
	bool finishTypeSpecificTemplateConfiguration() override;

	void updateRenderContext(const ViewRenderContext& context) override;
	QRectF getTemplateExtent() const override;
	void collectRasterTiles(const QRectF& map_clip_rect, double scale, bool on_screen,
							QVector<RasterTemplateTile>& out) const override;

  private slots:
	void onNetworkFinished(imagery::TileNetworkManager::Token token,
						   const imagery::TileNetworkResult& result);
	void onMapGeoreferencingChanged();

  private:
	friend class OnlineRasterTemplateTest;

	struct TileWindow
	{
		int zoom = -1;
		qint64 min_column = 0;
		qint64 max_column = -1;
		qint64 min_row = 0;
		qint64 max_row = -1;

		bool isEmpty() const noexcept;
		bool intersects(const TileWindow& other) const noexcept;
		bool contains(const TileWindow& other) const noexcept;
		qint64 width() const noexcept;
		qint64 height() const noexcept;
		auto operator<=>(const TileWindow&) const = default;
	};

	struct MemoryReservation final : public RasterMemoryLease
	{
		std::shared_ptr<std::atomic<qint64>> counter;
		qint64 bytes = 0;

		~MemoryReservation();
		void shrinkTo(qint64 bytes) noexcept override;
	};

	struct CachedTile
	{
		CachedTile() = default;
		CachedTile(QImage image, bool opaque, bool empty = false,
				   std::shared_ptr<MemoryReservation> memory = {})
		 : image(std::move(image))
		 , opaque(opaque)
		 , empty(empty)
		 , memory(std::move(memory))
		{}

		QImage image;
		bool opaque = false;
		bool empty = false;
		std::shared_ptr<MemoryReservation> memory;
	};

	struct PendingFetch
	{
		OnlineRasterTileKey key;
		quint64 generation = 0;
		int endpoint = 0;
		quint64 endpoint_offset = 0;
	};

	struct PendingDecode
	{
		std::shared_ptr<std::atomic_bool> cancelled;
		int endpoint = 0;
		quint64 endpoint_offset = 0;
	};

	struct EncodedTilePayload
	{
		QByteArray bytes;
		std::shared_ptr<std::atomic<qint64>> bytes_in_flight;
		qint64 byte_count = 0;

		~EncodedTilePayload();
	};

	struct TileFailure
	{
		int attempts = 0;
		quint64 next_endpoint_offset = 0;
		QSet<int> terminal_endpoints;
		QHash<int, QString> policy_rejected_origins;
		QDeadlineTimer retry;
		bool permanent = false;
		QString message;
	};

	struct StoredSnapshotPayload
	{
		QString encoding;
		QString text;
	};

	struct VisualTile
	{
		OnlineRasterTileKey requested;
		OnlineRasterTileKey cached;
		const CachedTile* tile = nullptr;
		QRectF source_rect;
		bool provisional = false;
		bool complete_empty = false;
	};

	struct AtlasCache
	{
		TileWindow window;
		QVector<quint64> signature;
		QImage image;
		QTransform image_to_map;
		QRectF map_bounds;
		double pixels_per_map_unit = 0;
		bool provisional = false;
		bool output_owned = false;
		std::shared_ptr<MemoryReservation> memory;
		std::shared_ptr<MemoryReservation> render_memory;

		void clear();
	};

	struct AtlasBuildVisual
	{
		QRectF target_rect;
		QImage image;
		QRectF source_rect;
		std::shared_ptr<MemoryReservation> memory;
	};

	struct AtlasWarpGrid
	{
		int columns = 0;
		int rows = 0;
		QSize output_size;
		QVector<QPointF> source_points;
		// Sampling may use the bounded interpolation grid, but ownership of the
		// half-open source chunk is resolved with this common exact inverse near
		// its edges. Adjacent chunks therefore make the same boundary decision.
		bool exact_ownership = false;
		QString map_crs;
		QString source_crs;
		QTransform map_to_projected;
		QRectF map_bounds;
		QPointF source_registration;
		double core_west = 0;
		double core_north = 0;
		double cell_size = 0;
		double maximum_interpolation_error = 0;
	};

	struct AtlasBuildRequest
	{
		TileWindow window;
		QVector<quint64> signature;
		QSize core_size;
		QVector<AtlasBuildVisual> visuals;
		std::optional<AtlasWarpGrid> warp;
		QTransform image_to_map;
		QRectF map_bounds;
		double pixels_per_map_unit = 0;
		bool provisional = false;
		bool has_left_neighbor = false;
		bool has_right_neighbor = false;
		bool has_top_neighbor = false;
		bool has_bottom_neighbor = false;
		std::shared_ptr<MemoryReservation> working_memory;
	};

	struct AtlasBuildResult
	{
		TileWindow window;
		QVector<quint64> signature;
		QImage image;
		QTransform image_to_map;
		QRectF map_bounds;
		double pixels_per_map_unit = 0;
		bool provisional = false;
		std::shared_ptr<MemoryReservation> working_memory;
	};

	explicit OnlineRasterTemplate(const QString& path, Map* map,
								  imagery::TileNetworkManager* network);

	void initializeConnections();
	void resetRuntime(bool clear_cache);
	void setSnapshot(imagery::ImagerySourceSnapshot snapshot);
	bool decodeStoredSnapshot();

	const imagery::ResolvedImagerySource* source() const noexcept;
	const imagery::TileMatrix* matrix(int zoom) const noexcept;
	const imagery::TileMatrixLimits* limits(int zoom) const noexcept;
	bool tileAllowed(const OnlineRasterTileKey& key) const noexcept;
	imagery::CrsBounds tileBounds(const OnlineRasterTileKey& key) const noexcept;

	std::optional<QPointF> mapToNominalSource(const QPointF& map_point) const;
	std::optional<QPointF> nominalSourceToMap(const QPointF& source_point) const;
	std::optional<QPointF> imagePointToMap(const OnlineRasterTileKey& image_key,
										   const QPointF& image_point) const;
	std::optional<QTransform> imageRectToMap(const OnlineRasterTileKey& image_key,
											 const QRectF& source_rect,
											 QRectF* map_bounds = nullptr,
											 double* residual_map_units = nullptr) const;
	QRectF sourceMapBounds() const;
	QRectF onScreenMapBounds() const;

	TileWindow tileWindowForMapRect(const QRectF& map_rect, int zoom,
								   bool exact_output = false,
								   bool* projection_complete = nullptr) const;
	int chooseZoom(const QRectF& map_rect, double pixels_per_map_unit,
				   bool exact_output = false) const;
	TileWindow withOverscan(TileWindow window, qint64 tiles) const;
	std::optional<qint64> tileCount(const TileWindow& window) const noexcept;
	bool workingSetFits(const TileWindow& window) const noexcept;
	bool keyNeededForWindow(const OnlineRasterTileKey& key,
							const TileWindow& window) const noexcept;
	void cancelUnwantedWork(const TileWindow& window);

	void queueWindow(const TileWindow& window, bool replace_pending);
	void queueTile(const OnlineRasterTileKey& key, imagery::TileRequestPriority priority,
				   double distance_priority);
	void recordFailure(
		const OnlineRasterTileKey& key,
		bool permanent,
		QString message = {});
	void recordEndpointFailure(
		const OnlineRasterTileKey& key,
		int endpoint,
		quint64 endpoint_offset,
		bool terminal,
		QString message,
		const QUrl& policy_rejected_url = {});
	void clearFailure(const OnlineRasterTileKey& key);
	bool retryAllowed(const OnlineRasterTileKey& key) const;
	void scheduleNextRetry();
	void trimFailureHistory();
	void updateResourceStatus();
	qint64 encodedTileResponseLimit(const OnlineRasterTileKey& key) const noexcept;
	std::shared_ptr<EncodedTilePayload> reserveEncodedTilePayload(QByteArray bytes);
	std::shared_ptr<MemoryReservation> reserveRetainedMemory(qint64 bytes) const;
	qint64 evictRetainedMemory(qint64 target_bytes);

	static std::optional<CachedTile>
	decodeTile(const QByteArray& bytes, const QSize& expected_size,
			   const RasterResourceManager::CancellationToken& cancellation,
			   const std::shared_ptr<std::atomic_bool>& source_cancelled);
	void queueDecode(
		const OnlineRasterTileKey& key,
		QByteArray bytes,
		quint64 generation,
		int endpoint = 0,
		quint64 endpoint_offset = 0);
	void finishDecode(const OnlineRasterTileKey& key, std::optional<CachedTile> tile,
					  quint64 generation, const std::shared_ptr<std::atomic_bool>& cancelled);
	void insertTile(const OnlineRasterTileKey& key, CachedTile tile);
	void insertEmptyTile(const OnlineRasterTileKey& key);

	const CachedTile* bestCachedTile(const OnlineRasterTileKey& requested,
									 OnlineRasterTileKey* cached_key, QRectF* source_rect) const;
	QVector<VisualTile> visualTiles(const TileWindow& window, bool allow_provisional,
									bool* has_transparency, bool* has_missing,
									bool* has_pixels) const;
	bool appendOpaquePatches(
		const VisualTile& visual,
		double pixels_per_map_unit,
		const std::shared_ptr<MemoryReservation>& output_render_memory,
		QVector<RasterTemplateTile>& out) const;
	bool appendOpaquePatch(const VisualTile& visual, QRectF source_rect, double pixels_per_map_unit,
								   int depth,
								   const std::shared_ptr<MemoryReservation>& output_render_memory,
								   QVector<RasterTemplateTile>& out) const;
	bool appendTransparentAtlas(const TileWindow& window, const QVector<VisualTile>& visuals,
								bool has_missing, double pixels_per_map_unit, bool on_screen,
								QVector<RasterTemplateTile>& out) const;
	bool appendPreparedOutputAtlases(
		const TileWindow& window,
		double pixels_per_map_unit,
		QVector<RasterTemplateTile>& out) const;
	QVector<quint64> atlasSignature(const TileWindow& window, const QVector<VisualTile>& visuals,
									bool has_missing) const;
	QVector<TileWindow> atlasChunks(
		const TileWindow& window,
		double pixels_per_map_unit) const;
	std::optional<AtlasBuildRequest> makeAtlasBuildRequest(const TileWindow& window,
														   const QVector<VisualTile>& visuals,
														   bool has_missing,
														   double pixels_per_map_unit,
														   const QVector<quint64>& signature) const;
	static std::optional<AtlasBuildResult>
	buildAtlas(AtlasBuildRequest request, const std::shared_ptr<std::atomic_bool>& cancelled,
			   const RasterResourceManager::CancellationToken& cancellation);
	bool queueAtlasBuild(const TileWindow& window, const QVector<VisualTile>& visuals,
						 bool has_missing, double pixels_per_map_unit,
						 const QVector<quint64>& signature, bool for_output) const;
	void finishAtlasBuild(std::optional<AtlasBuildResult> result,
						  const std::shared_ptr<std::atomic_bool>& cancelled);
	void cancelAtlasBuild(bool clear_failure = true) const;

	void markTileDirty(const OnlineRasterTileKey& key);
	QRectF mapBoundsForSourceBounds(const imagery::CrsBounds& bounds) const;

	std::optional<imagery::ImagerySourceSnapshot> snapshot_;
	QByteArray stored_snapshot_json_;
	QByteArray stored_snapshot_sha256_;
	QString stored_snapshot_version_;
	QVector<StoredSnapshotPayload> stored_snapshot_payloads_;
	bool stored_version_attribute_ = true;
	bool stored_checksum_attribute_ = true;
	QString snapshot_error_;
	std::shared_ptr<const imagery::ResolvedImagerySource> source_;
	std::unique_ptr<ProjTransform> source_projection_;

	imagery::TileNetworkManager* network_ = nullptr;
	quint64 network_client_id_ = 0;
	quint64 generation_ = 1;
	TileWindow wanted_window_;
	TileWindow output_window_;
	bool output_preparation_active_ = false;
	QString output_preparation_error_;
	QSet<OnlineRasterTileKey> output_keys_;
	QHash<OnlineRasterTileKey, CachedTile> output_tiles_;
	QHash<OnlineRasterTileKey, std::shared_ptr<MemoryReservation>>
		output_render_memory_;
	bool output_source_tiles_released_ = false;
	double output_preparation_scale_ = 0;
	QHash<imagery::TileNetworkManager::Token, PendingFetch> pending_fetches_;
	QSet<OnlineRasterTileKey> queued_tiles_;
	QHash<OnlineRasterTileKey, PendingDecode> pending_decodes_;
	std::shared_ptr<std::atomic<qint64>> decode_bytes_in_flight_;
	QHash<OnlineRasterTileKey, TileFailure> failed_tiles_;
	QSet<OnlineRasterTileKey> offline_tiles_;
	QTimer retry_timer_;
	RasterResourceManager::Owner decode_owner_ = RasterResourceManager::instance().createOwner(2);
	mutable RasterResourceManager::Owner atlas_owner_ =
		RasterResourceManager::instance().createOwner(1);

#ifdef Q_OS_ANDROID
	QCache<OnlineRasterTileKey, CachedTile> tile_cache_{ 64 * 1024 };
#else
	QCache<OnlineRasterTileKey, CachedTile> tile_cache_{ 256 * 1024 };
	#endif
	mutable AtlasCache atlas_;
	mutable QVector<AtlasCache> output_atlases_;
	mutable bool output_uses_atlases_ = false;
	mutable std::shared_ptr<std::atomic_bool> atlas_cancelled_;
	mutable QVector<quint64> atlas_pending_signature_;
	mutable double atlas_pending_scale_ = 0;
	mutable bool atlas_pending_for_output_ = false;
	mutable QVector<quint64> atlas_failed_signature_;
	mutable double atlas_failed_scale_ = 0;
	mutable bool atlas_queue_busy_ = false;
	mutable QTimer atlas_retry_timer_;
	mutable QRectF last_render_bounds_;
	mutable QRectF screen_map_bounds_cache_;
	mutable bool screen_map_bounds_dirty_ = true;
	mutable bool exact_projection_failed_ = false;
	mutable quint64 retained_access_ = 0;

	static constexpr qsizetype max_queued_tiles = 96;
	#ifdef Q_OS_ANDROID
	static constexpr qint64 max_window_tiles = 512;
	static constexpr qint64 max_working_set_bytes = qint64(32) * 1024 * 1024;
	static constexpr qint64 max_decode_encoded_bytes = qint64(24) * 1024 * 1024;
	static constexpr qint64 max_retained_raster_bytes = qint64(128) * 1024 * 1024;
	static constexpr qint64 max_encoded_tile_response_bytes = qint64(4) * 1024 * 1024;
	static constexpr qint64 max_atlas_pixels = qint64(6) * 1024 * 1024;
	static constexpr qint64 max_atlas_peak_bytes = qint64(48) * 1024 * 1024;
	#else
	static constexpr qint64 max_window_tiles = 1024;
	static constexpr qint64 max_working_set_bytes = qint64(384) * 1024 * 1024;
	static constexpr qint64 max_decode_encoded_bytes = qint64(64) * 1024 * 1024;
	static constexpr qint64 max_retained_raster_bytes = qint64(512) * 1024 * 1024;
	static constexpr qint64 max_encoded_tile_response_bytes = qint64(8) * 1024 * 1024;
	static constexpr qint64 max_atlas_pixels = qint64(16) * 1024 * 1024;
	static constexpr qint64 max_atlas_peak_bytes = qint64(128) * 1024 * 1024;
#endif
	static constexpr qint64 max_tile_pixels =
		imagery::maximum_runtime_tile_pixels;
	static constexpr int max_tile_dimension =
		imagery::maximum_runtime_tile_dimension;
	static constexpr int max_atlas_dimension = 8192;
	static constexpr qsizetype max_failure_records = 2048;
};

} // namespace OpenOrienteering

#endif
