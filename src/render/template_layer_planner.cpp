/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "render/template_layer_planner.h"

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
#include "templates/template.h"
#include "templates/template_image.h"
#include "templates/template_map.h"
#include "templates/template_track.h"
#include "util/util.h"

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
	QRectF target;
	QRectF source;

	bool operator==(const TileKey& other) const
	{
		return image == other.image && target == other.target && source == other.source;
	}
};

struct LayerKey
{
	QTransform template_to_map;
	std::vector<TileKey> tiles;

	bool operator==(const LayerKey& other) const
	{
		return template_to_map == other.template_to_map && tiles == other.tiles;
	}
};

QTransform templateToMapTransform(const Template& source)
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

std::shared_ptr<const QImage> snapshotImage(const QImage& source)
{
	if (source.isNull())
		return {};
	return std::make_shared<const QImage>(source);
}

struct SourceTile
{
	TileKey key;
	QImage image;
};

struct RasterMosaic
{
	std::shared_ptr<const QImage> image;
	QRectF target;
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
		left = std::min(left, tile.key.target.left());
		top = std::min(top, tile.key.target.top());
		right = std::max(right, tile.key.target.right());
		bottom = std::max(bottom, tile.key.target.bottom());
		density_x = std::max(density_x, tile.key.source.width() / tile.key.target.width());
		density_y = std::max(density_y, tile.key.source.height() / tile.key.target.height());
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
			(tile.key.target.x() - left) * scale_x,
			(tile.key.target.y() - top) * scale_y,
			tile.key.target.width() * scale_x,
			tile.key.target.height() * scale_y
		);
		painter.drawImage(target, tile.image, tile.key.source);
	}
	painter.end();

	return {
		snapshotImage(mosaic),
		{ left, top, target_width, target_height },
	};
}

QRectF fullImageTarget(const TileKey& tile)
{
	auto const scale_x = tile.target.width() / tile.source.width();
	auto const scale_y = tile.target.height() / tile.source.height();
	return {
		tile.target.x() - tile.source.x() * scale_x,
		tile.target.y() - tile.source.y() * scale_y,
		tile.image.width * scale_x,
		tile.image.height * scale_y,
	};
}

}  // namespace

class TemplateLayerPlanner::Impl
{
public:
	struct CachedLayer
	{
		LayerKey key;
		std::shared_ptr<const QtRenderScene> scene;
	};

	TemplateLayerPlan plan(const Map& map,
	                     const MapView* view,
	                     QRectF visible_map_rect,
	                     double view_scale,
	                     bool on_screen)
	{
		TemplateLayerPlan result;
		std::set<const TemplateImage*> live_layers;
		std::set<const TemplateMap*> live_child_maps;
		auto const first_above = map.getFirstFrontTemplate();
		for (int index = 0; index < map.getNumTemplates(); ++index)
		{
			auto const* source = map.getTemplate(index);
			if (source->getTemplateState() != Template::Loaded)
				continue;
			auto const visibility = view ? view->getTemplateVisibility(source)
			                             : TemplateVisibility { 1, true };
			if (!visibility.visible || visibility.opacity <= 0)
				continue;

			auto const template_scale = std::max(1.0e-9, view_scale * std::max(
				std::abs(source->getTemplateScaleX()),
				std::abs(source->getTemplateScaleY())
			));
			std::shared_ptr<const QtRenderScene> layer;
			if (auto const* image = dynamic_cast<const TemplateImage*>(source))
			{
				live_layers.insert(image);
				QVector<RasterTemplateTile> source_tiles;
				image->collectRasterTiles(
					visible_map_rect, template_scale, on_screen, source_tiles
				);
				layer = layerFor(*image, source_tiles, result, on_screen);
			}
			else if (auto const* map_template = dynamic_cast<const TemplateMap*>(source))
			{
				layer = map_template->buildQtRenderScene(
					visible_map_rect, template_scale, on_screen
				);
				if (map_template->includesChildTemplates() && map_template->templateMap())
				{
					live_child_maps.insert(map_template);
					auto& planner = child_planners_[map_template];
					if (!planner)
						planner = std::make_unique<TemplateLayerPlanner>();
					auto template_clip = templateClip(*map_template, visible_map_rect);
					auto children = planner->plan(
						*map_template->templateMap(), nullptr, template_clip,
						template_scale, on_screen
					);
					result.complete &= children.complete;
					result.newly_resident_images += children.newly_resident_images;
					auto& destination = index < first_above
					                  ? result.below_map : result.above_map;
					appendNested(destination, std::move(children.below_map),
					             *map_template, visible_map_rect, visibility.opacity);
					if (layer)
						destination.push_back(passFor(
							std::move(layer), visibility.opacity
						));
					appendNested(destination, std::move(children.above_map),
					             *map_template, visible_map_rect, visibility.opacity);
					continue;
				}
			}
			else if (auto const* track_template = dynamic_cast<const TemplateTrack*>(source))
			{
				if (next_revision_ == std::numeric_limits<Revision>::max())
					qFatal("Template layer revision space exhausted");
				layer = track_template->buildQtRenderScene(
					on_screen, template_scale, next_revision_++
				);
			}
			if (!layer)
				continue;
			VectorPass pass = passFor(std::move(layer), visibility.opacity);
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
		for (auto it = child_planners_.begin(); it != child_planners_.end(); )
		{
			if (!live_child_maps.contains(it->first))
				it = child_planners_.erase(it);
			else
				++it;
		}
		return result;
	}

private:
	static VectorPass passFor(std::shared_ptr<const QtRenderScene> scene, double opacity)
	{
		auto const clamped = std::clamp(opacity, 0.0, 1.0);
		return { std::move(scene), QPainter::CompositionMode_SourceOver, clamped, clamped < 1 };
	}

	static QRectF templateClip(const TemplateMap& source, QRectF map_clip)
	{
		if (source.isTemplateGeoreferenced())
			return map_clip;
		QRectF result;
		auto const& clip = map_clip;
		rectIncludeSafe(result, source.mapToTemplate(MapCoordF(clip.topLeft())));
		rectIncludeSafe(result, source.mapToTemplate(MapCoordF(clip.topRight())));
		rectIncludeSafe(result, source.mapToTemplate(MapCoordF(clip.bottomLeft())));
		rectIncludeSafe(result, source.mapToTemplate(MapCoordF(clip.bottomRight())));
		return result;
	}

	std::shared_ptr<const QtRenderScene> nestedToHost(const TemplateMap& source,
	                                             std::shared_ptr<const QtRenderScene> scene,
	                                             QRectF host_clip)
	{
		if (!scene || source.isTemplateGeoreferenced())
			return scene;
		auto const origin = source.templateToMap(QPointF(0, 0));
		auto const x_axis = source.templateToMap(QPointF(1, 0));
		auto const y_axis = source.templateToMap(QPointF(0, 1));
		QtRenderSceneBuilder builder(scene->revision, host_clip);
		builder.pushTransform({
			x_axis.x() - origin.x(), x_axis.y() - origin.y(),
			y_axis.x() - origin.x(), y_axis.y() - origin.y(),
			origin.x(), origin.y(),
		});
		builder.append(*scene);
		builder.popTransform();
		return builder.finish();
	}

	void appendNested(std::vector<VectorPass>& destination,
	                  std::vector<VectorPass> passes,
	                  const TemplateMap& source,
	                  QRectF host_clip,
	                  double outer_opacity)
	{
		for (auto& pass : passes)
		{
			pass.scene = nestedToHost(source, std::move(pass.scene), host_clip);
			pass.opacity *= std::clamp(outer_opacity, 0.0, 1.0);
			pass.isolated |= pass.opacity < 1;
			destination.push_back(std::move(pass));
		}
	}

	std::shared_ptr<const QtRenderScene> layerFor(const TemplateImage& source,
	                                        const QVector<RasterTemplateTile>& source_tiles,
	                                        TemplateLayerPlan& result,
	                                        bool on_screen)
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
				tile.template_rect,
				tile.source_rect,
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
			if (on_screen && result.newly_resident_images >= max_new_images_per_frame)
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
			QtRenderSceneBuilder builder(next_revision_++);
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
		std::vector<std::pair<TileKey, std::shared_ptr<const QImage>>> tiles;
		tiles.reserve(source_images.size());
		for (auto const& tile : source_images)
		{
			auto image = imageFor(tile.key.image, tile.image, result, on_screen);
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
		QtRenderSceneBuilder builder(next_revision_++);
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

	std::shared_ptr<const QImage> imageFor(const ImageKey& key,
	                                          const QImage& image,
	                                          TemplateLayerPlan& result,
	                                          bool on_screen)
	{
		auto const found = images_.find(key);
		if (found != images_.end())
		{
			if (auto existing = found->second.lock())
				return existing;
		}
		if (on_screen && result.newly_resident_images >= max_new_images_per_frame)
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
	std::map<ImageKey, std::weak_ptr<const QImage>> images_;
	std::map<ImageKey, bool> opacity_;
	std::unordered_map<const TemplateMap*, std::unique_ptr<TemplateLayerPlanner>> child_planners_;
};

TemplateLayerPlanner::TemplateLayerPlanner()
 : impl_(std::make_unique<Impl>())
{}

TemplateLayerPlanner::~TemplateLayerPlanner() = default;

TemplateLayerPlan TemplateLayerPlanner::plan(const Map& map,
	                                     const MapView& view,
	                                     QRectF visible_map_rect,
	                                     double view_scale,
	                                     bool on_screen)
{
	return impl_->plan(map, &view, visible_map_rect, view_scale, on_screen);
}

TemplateLayerPlan TemplateLayerPlanner::plan(const Map& map,
	                                     const MapView* view,
	                                     QRectF visible_map_rect,
	                                     double view_scale,
	                                     bool on_screen)
{
	return impl_->plan(map, view, visible_map_rect, view_scale, on_screen);
}

}  // namespace OpenOrienteering::render
