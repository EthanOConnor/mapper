/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/raster_layer_planner.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <unordered_map>
#include <utility>

#include <QImage>
#include <QPainter>
#include <QPointF>
#include <QRectF>
#include <QSizeF>

#include "core/map.h"
#include "core/map_view.h"
#include "render/qt_render_bridge.h"
#include "templates/template.h"
#include "templates/template_image.h"

namespace OpenOrienteering::render {

namespace {

constexpr std::size_t max_new_images_per_frame = 4;

struct ImageKey
{
	quint64 cache_key = 0;
	int width = 0;
	int height = 0;

	auto operator<=>(const ImageKey&) const = default;
};

struct TileKey
{
	ImageKey image;
	Rect target;
	Rect source;

	bool operator==(const TileKey& other) const
	{
		return image == other.image
		       && target.x == other.target.x && target.y == other.target.y
		       && target.width == other.target.width && target.height == other.target.height
		       && source.x == other.source.x && source.y == other.source.y
		       && source.width == other.source.width && source.height == other.source.height;
	}
};

struct LayerKey
{
	Transform template_to_map;
	std::vector<TileKey> tiles;

	bool operator==(const LayerKey& other) const
	{
		return template_to_map.m11 == other.template_to_map.m11
		       && template_to_map.m12 == other.template_to_map.m12
		       && template_to_map.m21 == other.template_to_map.m21
		       && template_to_map.m22 == other.template_to_map.m22
		       && template_to_map.dx == other.template_to_map.dx
		       && template_to_map.dy == other.template_to_map.dy
		       && tiles == other.tiles;
	}
};

Transform templateToMapTransform(const Template& source)
{
	auto const origin = source.templateToMap(QPointF(0, 0));
	auto const x = source.templateToMap(QPointF(1, 0));
	auto const y = source.templateToMap(QPointF(0, 1));
	return {
		x.x() - origin.x(), x.y() - origin.y(),
		y.x() - origin.x(), y.y() - origin.y(),
		origin.x(), origin.y(),
	};
}

bool imageIsOpaque(const QImage& source)
{
	if (!source.hasAlphaChannel())
		return true;

	auto const image = source.convertToFormat(QImage::Format_RGBA8888);
	if (image.isNull())
		return false;
	auto const row_bytes = std::size_t(image.width()) * 4;
	for (int y = 0; y < image.height(); ++y)
	{
		auto const* row = image.constScanLine(y);
		for (std::size_t x = 3; x < row_bytes; x += 4)
		{
			if (row[x] != 255)
				return false;
		}
	}
	return true;
}

std::shared_ptr<const ImageData> snapshotImage(const QImage& source)
{
	auto const image = source.convertToFormat(QImage::Format_RGBA8888);
	if (image.isNull())
		return {};

	auto bytes = std::make_shared<std::vector<std::uint8_t>>();
	auto const row_bytes = std::size_t(image.width()) * 4;
	bytes->reserve(row_bytes * std::size_t(image.height()));
	for (int y = 0; y < image.height(); ++y)
	{
		auto const* row = image.constScanLine(y);
		bytes->insert(bytes->end(), row, row + row_bytes);
	}
	return std::make_shared<const ImageData>(ImageData {
		std::uint32_t(image.width()),
		std::uint32_t(image.height()),
		std::uint32_t(row_bytes),
		std::move(bytes),
	});
}

struct SourceTile
{
	TileKey key;
	QImage image;
};

struct RasterMosaic
{
	std::shared_ptr<const ImageData> image;
	Rect target;
};

RasterMosaic transparentMosaic(const std::vector<SourceTile>& tiles)
{
	if (tiles.empty())
		return {};

	auto left = std::numeric_limits<double>::max();
	auto top = std::numeric_limits<double>::max();
	auto right = std::numeric_limits<double>::lowest();
	auto bottom = std::numeric_limits<double>::lowest();
	auto density_x = 0.0;
	auto density_y = 0.0;
	for (auto const& tile : tiles)
	{
		left = std::min(left, tile.key.target.x);
		top = std::min(top, tile.key.target.y);
		right = std::max(right, tile.key.target.x + tile.key.target.width);
		bottom = std::max(bottom, tile.key.target.y + tile.key.target.height);
		density_x = std::max(density_x, tile.key.source.width / tile.key.target.width);
		density_y = std::max(density_y, tile.key.source.height / tile.key.target.height);
	}
	auto const target_width = right - left;
	auto const target_height = bottom - top;
	if (!std::isfinite(target_width) || !std::isfinite(target_height)
	    || !std::isfinite(density_x) || !std::isfinite(density_y)
	    || target_width <= 0 || target_height <= 0
	    || density_x <= 0 || density_y <= 0)
	{
		return {};
	}

	constexpr double max_dimension = 8192;
	constexpr double max_pixels = 64.0 * 1024 * 1024;
	auto desired_width = std::ceil(target_width * density_x);
	auto desired_height = std::ceil(target_height * density_y);
	auto reduction = std::min({
		1.0,
		max_dimension / desired_width,
		max_dimension / desired_height,
		std::sqrt(max_pixels / (desired_width * desired_height)),
	});
	auto const width = std::max(1, int(std::floor(desired_width * reduction)));
	auto const height = std::max(1, int(std::floor(desired_height * reduction)));

	QImage mosaic(width, height, QImage::Format_RGBA8888_Premultiplied);
	if (mosaic.isNull())
		return {};
	mosaic.fill(Qt::transparent);
	QPainter painter(&mosaic);
	painter.setCompositionMode(QPainter::CompositionMode_Source);
	painter.setRenderHint(QPainter::SmoothPixmapTransform, true);
	auto const scale_x = width / target_width;
	auto const scale_y = height / target_height;
	for (auto const& tile : tiles)
	{
		auto const target = QRectF(
			(tile.key.target.x - left) * scale_x,
			(tile.key.target.y - top) * scale_y,
			tile.key.target.width * scale_x,
			tile.key.target.height * scale_y
		);
		painter.drawImage(target, tile.image, toQRectF(tile.key.source));
	}
	painter.end();

	return {
		snapshotImage(mosaic),
		{ left, top, target_width, target_height },
	};
}

Rect fullImageTarget(const TileKey& tile)
{
	auto const scale_x = tile.target.width / tile.source.width;
	auto const scale_y = tile.target.height / tile.source.height;
	return {
		tile.target.x - tile.source.x * scale_x,
		tile.target.y - tile.source.y * scale_y,
		tile.image.width * scale_x,
		tile.image.height * scale_y,
	};
}

}  // namespace

class RasterLayerPlanner::Impl
{
public:
	struct CachedLayer
	{
		LayerKey key;
		std::shared_ptr<const RenderIR> scene;
	};

	RasterLayerPlan plan(const Map& map,
	                     const MapView& view,
	                     Rect visible_map_rect,
	                     double view_scale,
	                     bool on_screen)
	{
		RasterLayerPlan result;
		std::set<const TemplateImage*> live_layers;
		auto const first_above = map.getFirstFrontTemplate();
		for (int index = 0; index < map.getNumTemplates(); ++index)
		{
			auto const* source = dynamic_cast<const TemplateImage*>(map.getTemplate(index));
			if (!source || source->getTemplateState() != Template::Loaded)
				continue;
			auto const visibility = view.getTemplateVisibility(source);
			if (!visibility.visible || visibility.opacity <= 0)
				continue;

			live_layers.insert(source);
			QVector<RasterTemplateTile> source_tiles;
			source->collectRasterTiles(
				toQRectF(visible_map_rect), view_scale, on_screen, source_tiles
			);
			auto layer = layerFor(*source, source_tiles, result);
			if (!layer)
				continue;
			VectorPass pass {
				std::move(layer),
				BlendMode::SourceOver,
				std::clamp(double(visibility.opacity), 0.0, 1.0),
				visibility.opacity < 1,
			};
			auto& destination = index < first_above ? result.below_map : result.above_map;
			destination.push_back(std::move(pass));
		}

		for (auto it = layers_.begin(); it != layers_.end(); )
		{
			if (!live_layers.contains(it->first))
				it = layers_.erase(it);
			else
				++it;
		}
		for (auto it = images_.begin(); it != images_.end(); )
		{
			if (it->second.expired())
				it = images_.erase(it);
			else
				++it;
		}
		return result;
	}

private:
	std::shared_ptr<const RenderIR> layerFor(const TemplateImage& source,
	                                        const QVector<RasterTemplateTile>& source_tiles,
	                                        RasterLayerPlan& result)
	{
		LayerKey full_key;
		full_key.template_to_map = templateToMapTransform(source);
		std::vector<SourceTile> source_images;
		source_images.reserve(std::size_t(source_tiles.size()));
		for (auto const& tile : source_tiles)
		{
			if (tile.provisional)
				result.complete = false;
			if (tile.missing)
			{
				result.complete = false;
				continue;
			}
			if (tile.image.isNull() || !tile.template_rect.isValid()
			    || !tile.source_rect.isValid() || tile.source_rect.isEmpty())
				continue;

			ImageKey image_key {
				tile.cache_key ? tile.cache_key : quint64(tile.image.cacheKey()),
				tile.image.width(),
				tile.image.height(),
			};
			TileKey tile_key {
				image_key,
				fromQRectF(tile.template_rect),
				fromQRectF(tile.source_rect),
			};
			full_key.tiles.push_back(tile_key);
			source_images.push_back({ std::move(tile_key), tile.image });
		}

		auto const found = layers_.find(&source);
		if (found != layers_.end() && found->second.key == full_key)
			return found->second.scene;
		if (source_images.empty())
		{
			layers_.erase(&source);
			return {};
		}

		auto const has_transparency = std::ranges::any_of(
			source_images,
			[this](auto const& tile) {
				return !isOpaque(tile.key.image, tile.image);
			}
		);
		if (has_transparency)
		{
			if (result.newly_resident_images >= max_new_images_per_frame)
			{
				result.complete = false;
				return {};
			}
			auto mosaic = transparentMosaic(source_images);
			if (!mosaic.image)
			{
				result.complete = false;
				return {};
			}

			if (next_revision_ == std::numeric_limits<Revision>::max())
				qFatal("Raster layer revision space exhausted");
			RenderIRBuilder builder(next_revision_++);
			builder.pushTransform(full_key.template_to_map);
			builder.drawImage(mosaic.image, mosaic.target);
			builder.popTransform();
			auto scene = builder.finish();
			++result.newly_resident_images;
			layers_[&source] = { std::move(full_key), scene };
			return scene;
		}

		LayerKey key;
		key.template_to_map = full_key.template_to_map;
		std::vector<std::pair<TileKey, std::shared_ptr<const ImageData>>> tiles;
		tiles.reserve(source_images.size());
		for (auto const& tile : source_images)
		{
			auto image = imageFor(tile.key.image, tile.image, result);
			if (!image)
			{
				result.complete = false;
				continue;
			}
			key.tiles.push_back(tile.key);
			tiles.emplace_back(tile.key, std::move(image));
		}
		if (found != layers_.end() && found->second.key == key)
			return found->second.scene;
		if (tiles.empty())
		{
			layers_.erase(&source);
			return {};
		}

		if (next_revision_ == std::numeric_limits<Revision>::max())
			qFatal("Raster layer revision space exhausted");
		RenderIRBuilder builder(next_revision_++);
		builder.pushTransform(key.template_to_map);
		for (auto const& [tile, image] : tiles)
			builder.drawImage(image, fullImageTarget(tile));
		builder.popTransform();
		auto scene = builder.finish();
		layers_[&source] = { std::move(key), scene };
		return scene;
	}

	bool isOpaque(const ImageKey& key, const QImage& image)
	{
		if (auto const found = opacity_.find(key); found != opacity_.end())
			return found->second;
		if (opacity_.size() >= 4096)
			opacity_.clear();
		return opacity_.emplace(key, imageIsOpaque(image)).first->second;
	}

	std::shared_ptr<const ImageData> imageFor(const ImageKey& key,
	                                          const QImage& image,
	                                          RasterLayerPlan& result)
	{
		auto const found = images_.find(key);
		if (found != images_.end())
		{
			if (auto existing = found->second.lock())
				return existing;
		}
		if (result.newly_resident_images >= max_new_images_per_frame)
			return {};

		auto snapshot = snapshotImage(image);
		if (!snapshot)
			return {};
		images_[key] = snapshot;
		++result.newly_resident_images;
		return snapshot;
	}

	Revision next_revision_ = 1;
	std::unordered_map<const TemplateImage*, CachedLayer> layers_;
	std::map<ImageKey, std::weak_ptr<const ImageData>> images_;
	std::map<ImageKey, bool> opacity_;
};

RasterLayerPlanner::RasterLayerPlanner()
 : impl_(std::make_unique<Impl>())
{}

RasterLayerPlanner::~RasterLayerPlanner() = default;

RasterLayerPlan RasterLayerPlanner::plan(const Map& map,
	                                     const MapView& view,
	                                     Rect visible_map_rect,
	                                     double view_scale,
	                                     bool on_screen)
{
	return impl_->plan(map, view, visible_map_rect, view_scale, on_screen);
}

}  // namespace OpenOrienteering::render
