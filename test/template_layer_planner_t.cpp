/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "template_layer_planner_t.h"

#include <algorithm>
#include <memory>
#include <variant>

#include <QtTest>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QVector>

#include "global.h"
#include "test_config.h"
#include "core/map.h"
#include "core/map_view.h"
#include "presentation/native_surface.h"
#include "render/frame_pipeline.h"
#include "render/qpainter_frame_renderer.h"
#include "render/qt_render_bridge.h"
#include "render/template_layer_planner.h"
#include "render/vello_renderer.h"
#include "templates/template_image.h"
#include "templates/template_map.h"
#include "templates/template_track.h"

using namespace OpenOrienteering;

namespace {

class SyntheticRasterTemplate final : public TemplateImage
{
public:
	SyntheticRasterTemplate(Map* map, QVector<RasterTemplateTile> tiles, QRectF extent)
	 : TemplateImage(QString(), map)
	 , tiles_(std::move(tiles))
	 , extent_(extent)
	{
		image = QImage(1, 1, QImage::Format_RGBA8888);
		image.fill(Qt::transparent);
		setTemplateState(Loaded);
	}

	QRectF getTemplateExtent() const override
	{
		return extent_;
	}

	void collectRasterTiles(const QRectF&, double, bool,
	                        QVector<RasterTemplateTile>& out) const override
	{
		out += tiles_;
	}

	void setTiles(QVector<RasterTemplateTile> tiles)
	{
		tiles_ = std::move(tiles);
	}

private:
	QVector<RasterTemplateTile> tiles_;
	QRectF extent_;
};

class CountingRasterLease final : public RasterMemoryLease
{
public:
	CountingRasterLease(qint64* counter, qint64 bytes)
	 : counter(counter)
	 , bytes(bytes)
	{
		*counter += bytes;
	}

	~CountingRasterLease() override
	{
		*counter -= bytes;
	}

	void shrinkTo(qint64 retained_bytes) noexcept override
	{
		retained_bytes = std::clamp<qint64>(
			retained_bytes, 0, bytes);
		*counter -= bytes - retained_bytes;
		bytes = retained_bytes;
	}

	qint64* counter = nullptr;
	qint64 bytes = 0;
};

QImage solidImage(QSize size, QColor color)
{
	QImage image(size, QImage::Format_RGBA8888);
	image.fill(color);
	return image;
}

RasterTemplateTile tile(QImage image, QRectF target)
{
	return {
		image,
		target,
		QRectF(QPointF(0, 0), QSizeF(image.size())),
		quint64(image.cacheKey()),
		false,
	};
}

RasterTemplateTile tileRegion(QImage image, QRectF source, QRectF target)
{
	return {
		image,
		target,
		source,
		quint64(image.cacheKey()),
		false,
	};
}

std::unique_ptr<SyntheticRasterTemplate> singleTile(
	Map* map, QColor color, QRectF target)
{
	auto image = solidImage(target.size().toSize(), color);
	return std::make_unique<SyntheticRasterTemplate>(
		map, QVector<RasterTemplateTile> { tile(std::move(image), target) }, target
	);
}

render::FramePacketPtr frameFor(
	const render::MapRenderSnapshot& snapshot,
	render::FramePlanner& planner,
	render::TemplateLayerPlan raster)
{
	render::FrameRequest request {
		{ 16, 16, 1, { 1, 0, 0, 1, 8, 8 } },
		{ { -8, -8, 16, 16 }, 1, RenderConfig::Screen, 1 },
		false,
	};
	request.below_map = std::move(raster.below_map);
	request.above_map = std::move(raster.above_map);
	return planner.plan(snapshot, request);
}

render::FramePacketPtr scaledFrameFor(
	const render::MapRenderSnapshot& snapshot,
	render::FramePlanner& planner,
	render::TemplateLayerPlan raster)
{
	render::FrameRequest request {
		{ 64, 40, 1, { 3.2, 0, 0, 3.2, 32.25, 20.35 } },
		{ { -10, -7, 20, 14 }, 1, RenderConfig::Screen, 1 },
		false,
	};
	request.below_map = std::move(raster.below_map);
	request.above_map = std::move(raster.above_map);
	return planner.plan(snapshot, request);
}

std::size_t imageCommandCount(const render::VectorPass& pass)
{
	return std::ranges::count_if(pass.scene->commands, [](auto const& command) {
		return std::holds_alternative<render::DrawImage>(command);
	});
}

QImage renderReference(const render::FramePacket& frame,
                       QColor background = Qt::white)
{
	auto const width = qCeil(frame.view.width * frame.view.device_pixel_ratio);
	auto const height = qCeil(frame.view.height * frame.view.device_pixel_ratio);
	QImage image(width, height, QImage::Format_ARGB32_Premultiplied);
	image.setDevicePixelRatio(frame.view.device_pixel_ratio);
	image.fill(background);
	QPainter painter(&image);
	auto const completion = render::QPainterFrameRenderer().render(painter, frame);
	Q_ASSERT(completion.status == render::FrameStatus::Presented);
	return image;
}

QColor velloPixel(const render::VelloImage& image, QPoint point)
{
	QImage view(
		image.rgba8.data(), int(image.width), int(image.height), int(image.width * 4),
		QImage::Format_RGBA8888
	);
	return view.pixelColor(point);
}

QImage renderVello(
	const render::FramePacketPtr& frame,
	render::Color background = { 65535, 65535, 65535, 65535 })
{
	render::VelloRenderer renderer;
	auto const rendered = renderer.renderOffscreen(frame, background);
	Q_ASSERT_X(rendered, Q_FUNC_INFO, renderer.lastError().c_str());
	QImage view(
		rendered->rgba8.data(), int(rendered->width), int(rendered->height),
		int(rendered->width * 4), QImage::Format_RGBA8888
	);
	return view.copy();
}

}  // namespace

void TemplateLayerPlannerTest::initTestCase()
{
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
}

void TemplateLayerPlannerTest::preservesLayerOrderAndRetainedScenes()
{
	Map map;
	MapView view { &map };
	map.addTemplate(0, singleTile(&map, Qt::red, { -8, -8, 16, 16 }));
	map.addTemplate(1, singleTile(&map, Qt::blue, { -8, -8, 16, 16 }));
	map.setFirstFrontTemplate(1);
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });
	view.setTemplateVisibility(map.getTemplate(1), { 0.5f, true });

	render::TemplateLayerPlanner template_planner;
	auto first = template_planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(first.complete);
	QCOMPARE(first.newly_resident_images, std::size_t(2));
	QCOMPARE(first.below_map.size(), std::size_t(1));
	QCOMPARE(first.above_map.size(), std::size_t(1));
	QCOMPARE(first.below_map.front().opacity, 1.0);
	QVERIFY(!first.below_map.front().isolated);
	QCOMPARE(first.above_map.front().opacity, 0.5);
	QVERIFY(first.above_map.front().isolated);
	auto const below_scene = first.below_map.front().scene;
	auto const above_scene = first.above_map.front().scene;

	auto second = template_planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QCOMPARE(second.newly_resident_images, std::size_t(0));
	QCOMPARE(second.below_map.front().scene, below_scene);
	QCOMPARE(second.above_map.front().scene, above_scene);

	auto const snapshot = map.publishRenderSnapshot();
	QVERIFY(snapshot);
	render::FramePlanner frame_planner;
	auto const frame = frameFor(*snapshot, frame_planner, std::move(second));
	QCOMPARE(frame->vector_passes.size(), std::size_t(3));
	QCOMPARE(frame->vector_passes.front().scene, below_scene);
	QCOMPARE(frame->vector_passes.back().scene, above_scene);

	auto const reference = renderReference(*frame);
	render::VelloRenderer renderer;
	auto const rendered = renderer.renderOffscreen(frame);
	QVERIFY2(rendered, renderer.lastError().c_str());
	auto const expected = reference.pixelColor(8, 8);
	auto const actual = velloPixel(*rendered, { 8, 8 });
	QVERIFY(std::abs(actual.red() - expected.red()) <= 2);
	QVERIFY(std::abs(actual.green() - expected.green()) <= 2);
	QVERIFY(std::abs(actual.blue() - expected.blue()) <= 2);
}

void TemplateLayerPlannerTest::
	retainsRasterMemoryLeaseWithImmutableScene()
{
	Map map;
	MapView view { &map };
	qint64 reserved_bytes = 0;
	auto image = solidImage({ 4, 4 }, Qt::red);
	auto const target = QRectF(-2, -2, 4, 4);
	RasterTemplateTile source_tile(
		image,
		target,
		QRectF(QPointF(0, 0), QSizeF(image.size())),
		quint64(image.cacheKey()),
		false,
		false,
		{},
		false,
		{},
		[&reserved_bytes](qint64 bytes) {
			return std::make_shared<CountingRasterLease>(
				&reserved_bytes, bytes);
		});
	map.addTemplate(
		0,
		std::make_unique<SyntheticRasterTemplate>(
			&map,
			QVector<RasterTemplateTile> {
				std::move(source_tile)
			},
			target));
	view.setTemplateVisibility(
		map.getTemplate(0), { 1, true });

	{
		render::TemplateLayerPlanner planner;
		auto plan = planner.plan(
			map, view,
			render::fromQRectF(target), 1);
		QVERIFY(plan.complete);
		QCOMPARE(reserved_bytes, qint64(4 * 4 * 4));
		plan = {};
		QCOMPARE(reserved_bytes, qint64(4 * 4 * 4));
	}
	QCOMPARE(reserved_bytes, qint64(0));
}

void TemplateLayerPlannerTest::
	leasesTransparentMosaicPeakAndRetainedMemory()
{
	Map map;
	MapView view { &map };
	qint64 reserved_bytes = 0;
	qint64 requested_bytes = 0;
	auto reserve = [&reserved_bytes, &requested_bytes](qint64 bytes) {
		requested_bytes = bytes;
		return std::make_shared<CountingRasterLease>(
			&reserved_bytes, bytes);
	};
	auto left = solidImage({ 4, 4 }, QColor(255, 0, 0, 128));
	auto right = solidImage({ 4, 4 }, QColor(0, 0, 255, 128));
	auto left_tile = tile(left, { -4, -2, 4, 4 });
	auto right_tile = tile(right, { 0, -2, 4, 4 });
	left_tile.reserve_render_memory = reserve;
	right_tile.reserve_render_memory = reserve;
	map.addTemplate(
		0,
		std::make_unique<SyntheticRasterTemplate>(
			&map,
			QVector<RasterTemplateTile> {
				std::move(left_tile),
				std::move(right_tile),
			},
			QRectF(-4, -2, 8, 4)));
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });

	{
		render::TemplateLayerPlanner planner;
		auto plan = planner.plan(
			map, view, { -4, -2, 8, 4 }, 1, false);
		QVERIFY(plan.complete);
		QCOMPARE(plan.newly_resident_images, std::size_t(1));
		QCOMPARE(requested_bytes, qint64(2 * 8 * 4 * 4));
		QCOMPARE(reserved_bytes, qint64(8 * 4 * 4));
		plan = {};
		planner.clear();
		QCOMPARE(reserved_bytes, qint64(0));
	}
	QCOMPARE(reserved_bytes, qint64(0));
}

void TemplateLayerPlannerTest::recordsVectorMapAndTrackTemplates()
{
	auto const test_dir = QDir(QString::fromUtf8(MAPPER_TEST_SOURCE_DIR));
	Map map;
	MapView view { &map };
	auto map_template = std::make_unique<TemplateMap>(
		test_dir.absoluteFilePath(QStringLiteral("../examples/overprinting.omap")), &map
	);
	map_template->setTemplateState(Template::Unloaded);
	QVERIFY(map_template->loadTemplateFile());
	auto track_template = std::make_unique<TemplateTrack>(
		test_dir.absoluteFilePath(QStringLiteral("data/templates/template-track.gpx")), &map
	);
	track_template->setTemplateState(Template::Unloaded);
	QVERIFY(track_template->loadTemplateFile());
	map.addTemplate(0, std::move(map_template));
	map.addTemplate(1, std::move(track_template));
	map.setFirstFrontTemplate(1);
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });
	view.setTemplateVisibility(map.getTemplate(1), { 1, true });

	auto visible = map.getTemplate(0)->calculateTemplateBoundingBox()
	             .united(map.getTemplate(1)->calculateTemplateBoundingBox());
	QVERIFY(visible.isValid());
	render::TemplateLayerPlanner planner;
	auto plan = planner.plan(map, view, render::fromQRectF(visible), 4);
	QVERIFY(plan.complete);
	QCOMPARE(plan.newly_resident_images, std::size_t(0));
	QCOMPARE(plan.below_map.size(), std::size_t(1));
	QCOMPARE(plan.above_map.size(), std::size_t(1));
	QVERIFY(plan.below_map.front().scene);
	QVERIFY(!plan.below_map.front().scene->commands.empty());
	QVERIFY(plan.above_map.front().scene);
	QVERIFY(!plan.above_map.front().scene->commands.empty());
	QVERIFY(std::ranges::none_of(
		plan.below_map.front().scene->commands,
		[](auto const& command) { return std::holds_alternative<render::DrawImage>(command); }
	));
}

void TemplateLayerPlannerTest::boundsImageAdmissionAndPreservesVelloIdentity()
{
	Map map;
	MapView view { &map };
	QVector<RasterTemplateTile> tiles;
	for (int index = 0; index < 6; ++index)
	{
		auto image = solidImage({ 2, 2 }, QColor::fromHsv(index * 30, 255, 255));
		tiles.push_back(tile(std::move(image), { -6.0 + index * 2.0, -1, 2, 2 }));
	}
	map.addTemplate(0, std::make_unique<SyntheticRasterTemplate>(
		&map, std::move(tiles), QRectF(-6, -1, 12, 2)
	));
	map.setFirstFrontTemplate(1);
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });
	auto const snapshot = map.publishRenderSnapshot();
	QVERIFY(snapshot);

	render::TemplateLayerPlanner template_planner;
	render::FramePlanner frame_planner;
	render::VelloRenderer renderer;
	presentation::NativeSurfaceState surface;
	surface.sequence = 1;
	surface.phase = presentation::SurfacePhase::Exposed;
	surface.native.window = 1;
	surface.physical_width = 16;
	surface.physical_height = 16;
	QVERIFY(renderer.setSurface(surface));

	{
		auto first = template_planner.plan(map, view, { -8, -8, 16, 16 }, 1);
		QVERIFY(!first.complete);
		QCOMPARE(first.newly_resident_images, std::size_t(4));
		QCOMPARE(first.below_map.size(), std::size_t(1));
		QCOMPARE(imageCommandCount(first.below_map.front()), std::size_t(4));
		auto const frame = frameFor(*snapshot, frame_planner, std::move(first));
		QVERIFY(renderer.submit(frame, surface));
		QCOMPARE(renderer.cachedImageCount(), std::size_t(4));
	}

	auto second = template_planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(second.complete);
	QCOMPARE(second.newly_resident_images, std::size_t(2));
	QCOMPARE(second.below_map.size(), std::size_t(1));
	QCOMPARE(imageCommandCount(second.below_map.front()), std::size_t(6));
	auto const complete_scene = second.below_map.front().scene;
	{
		auto const frame = frameFor(*snapshot, frame_planner, std::move(second));
		QVERIFY(renderer.submit(frame, surface));
		QCOMPARE(renderer.cachedImageCount(), std::size_t(6));
	}

	auto third = template_planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(third.complete);
	QCOMPARE(third.newly_resident_images, std::size_t(0));
	QCOMPARE(third.below_map.front().scene, complete_scene);
}

void TemplateLayerPlannerTest::marksFallbackLayersIncomplete()
{
	Map map;
	MapView view { &map };
	auto fallback = tile(solidImage({ 4, 4 }, Qt::red), { -2, -2, 4, 4 });
	fallback.provisional = true;
	map.addTemplate(0, std::make_unique<SyntheticRasterTemplate>(
		&map, QVector<RasterTemplateTile> { std::move(fallback) }, QRectF(-2, -2, 4, 4)
	));
	map.setFirstFrontTemplate(1);
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });

	render::TemplateLayerPlanner planner;
	auto const plan = planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(!plan.complete);
	QCOMPARE(plan.below_map.size(), std::size_t(1));
	QCOMPARE(imageCommandCount(plan.below_map.front()), std::size_t(1));
}

void TemplateLayerPlannerTest::rebuildsRasterSceneForCurrentZoomGeometry()
{
	Map map;
	MapView view { &map };
	auto raster = std::make_unique<SyntheticRasterTemplate>(
		&map,
		QVector<RasterTemplateTile> {
			tile(solidImage({ 4, 4 }, Qt::red), { -2, -2, 4, 4 })
		},
		QRectF(-2, -2, 4, 4));
	auto* raster_ptr = raster.get();
	map.addTemplate(0, std::move(raster));
	map.setFirstFrontTemplate(0);
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });

	render::TemplateLayerPlanner planner;
	auto first = planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(first.complete);
	QCOMPARE(first.above_map.size(), std::size_t(1));
	auto const complete_scene = first.above_map.front().scene;

	raster_ptr->setTiles({ RasterTemplateTile {
		{}, { -4, -4, 8, 8 }, {}, 0, true, false
	} });
	auto loading = planner.plan(map, view, { -8, -8, 16, 16 }, 2);
	QVERIFY(!loading.complete);
	QVERIFY(loading.above_map.empty());

	auto provisional = tile(
		solidImage({ 8, 4 }, Qt::blue), { -4, -2, 8, 4 }
	);
	provisional.provisional = true;
	raster_ptr->setTiles({ std::move(provisional) });
	loading = planner.plan(map, view, { -8, -8, 16, 16 }, 2);
	QVERIFY(!loading.complete);
	QCOMPARE(loading.above_map.size(), std::size_t(1));
	QCOMPARE(imageCommandCount(loading.above_map.front()), std::size_t(1));
	QVERIFY(loading.above_map.front().scene != complete_scene);
	auto const snapshot = map.publishRenderSnapshot();
	QVERIFY(snapshot);
	render::FramePlanner frame_planner;
	auto const transition_frame = frameFor(
		*snapshot, frame_planner, std::move(loading)
	);
	auto const transition = renderReference(*transition_frame);
	QCOMPARE(transition.pixelColor(6, 8), QColor(Qt::blue));
	QCOMPARE(transition.pixelColor(10, 8), QColor(Qt::blue));

	QVector<RasterTemplateTile> next_zoom;
	for (int index = 0; index < 6; ++index)
	{
		next_zoom.push_back(tile(
			solidImage({ 2, 2 }, QColor::fromHsv(index * 30, 255, 255)),
			{ -6.0 + index * 2.0, -1, 2, 2 }
		));
	}
	raster_ptr->setTiles(std::move(next_zoom));
	loading = planner.plan(map, view, { -8, -8, 16, 16 }, 2);
	QVERIFY(!loading.complete);
	QCOMPARE(loading.newly_resident_images, std::size_t(4));
	QCOMPARE(loading.above_map.size(), std::size_t(1));
	QVERIFY(loading.above_map.front().scene != complete_scene);
	auto ready = planner.plan(map, view, { -8, -8, 16, 16 }, 2);
	QVERIFY(ready.complete);
	QCOMPARE(ready.newly_resident_images, std::size_t(2));
	QCOMPARE(ready.above_map.size(), std::size_t(1));
	QVERIFY(ready.above_map.front().scene != complete_scene);
}

void TemplateLayerPlannerTest::reusesResidentPixelsWhileRebuildingProvisionalGeometry()
{
	Map map;
	MapView view { &map };
	auto pixels = solidImage({ 4, 4 }, Qt::blue);
	auto initial = tile(pixels, { -2, -2, 4, 4 });
	initial.image_to_map = QTransform(1, 0, 0, 1, -2, -2);
	initial.has_image_to_map = true;
	auto raster = std::make_unique<SyntheticRasterTemplate>(
		&map, QVector<RasterTemplateTile> { std::move(initial) },
		QRectF(-2, -2, 4, 4));
	auto* raster_ptr = raster.get();
	map.addTemplate(0, std::move(raster));
	map.setFirstFrontTemplate(1);
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });

	render::TemplateLayerPlanner planner;
	auto const first = planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(first.complete);
	QCOMPARE(first.newly_resident_images, std::size_t(1));
	QCOMPARE(first.below_map.size(), std::size_t(1));
	auto const first_scene = first.below_map.front().scene;

	auto fallback = tile(pixels, { -2, -2, 4, 4 });
	fallback.image_to_map = QTransform(0.75, 0.1, -0.05, 0.9, -1.5, -2.25);
	fallback.has_image_to_map = true;
	fallback.provisional = true;
	raster_ptr->setTiles({ std::move(fallback) });
	auto const second = planner.plan(map, view, { -8, -8, 16, 16 }, 2);
	QVERIFY(!second.complete);
	QCOMPARE(second.newly_resident_images, std::size_t(0));
	QCOMPARE(second.below_map.size(), std::size_t(1));
	QVERIFY(second.below_map.front().scene != first_scene);
	auto const command = std::ranges::find_if(
		second.below_map.front().scene->commands,
		[](auto const& value) { return std::holds_alternative<render::DrawImage>(value); });
	QVERIFY(command != second.below_map.front().scene->commands.end());
	QCOMPARE(std::get<render::DrawImage>(*command).image_to_scene.dx, -1.5);
	QCOMPARE(std::get<render::DrawImage>(*command).image_to_scene.dy, -2.25);
}

void TemplateLayerPlannerTest::drawsProvisionalDirectTilesBeforeExactTiles()
{
	Map map;
	MapView view { &map };
	auto exact = tile(solidImage({ 4, 4 }, Qt::blue), { -2, -2, 4, 4 });
	exact.image_to_map = QTransform(1, 0, 0, 1, -2, -2);
	exact.has_image_to_map = true;
	auto fallback = tile(solidImage({ 4, 4 }, Qt::red), { -2, -2, 4, 4 });
	fallback.provisional = true;
	fallback.image_to_map = exact.image_to_map;
	fallback.has_image_to_map = true;
	map.addTemplate(0, std::make_unique<SyntheticRasterTemplate>(
		&map,
		QVector<RasterTemplateTile> {
			std::move(exact),
			std::move(fallback),
		},
		QRectF(-2, -2, 4, 4)
	));
	map.setFirstFrontTemplate(1);
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });

	render::TemplateLayerPlanner template_planner;
	auto plan = template_planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(!plan.complete);
	QCOMPARE(plan.below_map.size(), std::size_t(1));
	QCOMPARE(imageCommandCount(plan.below_map.front()), std::size_t(2));

	auto const snapshot = map.publishRenderSnapshot();
	QVERIFY(snapshot);
	render::FramePlanner frame_planner;
	auto const frame = frameFor(*snapshot, frame_planner, std::move(plan));
	auto const rendered = renderReference(*frame, Qt::transparent);
	QCOMPARE(rendered.pixelColor(8, 8), QColor(Qt::blue));
}

void TemplateLayerPlannerTest::respectsExplicitImageToMapTransform()
{
	Map map;
	MapView view { &map };
	auto direct = tile(solidImage({ 4, 3 }, Qt::magenta), { 100, 200, 4, 3 });
	direct.image_to_map = QTransform(1.25, 0.2, -0.15, 0.9, -2.5, -1.25);
	direct.has_image_to_map = true;
	auto raster = std::make_unique<SyntheticRasterTemplate>(
		&map, QVector<RasterTemplateTile> { std::move(direct) }, QRectF(-4, -3, 8, 6)
	);
	raster->setTemplateX(5000);
	raster->setTemplateY(-3000);
	raster->setTemplateScaleX(7);
	raster->setTemplateRotation(0.35);
	map.addTemplate(0, std::move(raster));
	map.setFirstFrontTemplate(1);
	view.setTemplateVisibility(map.getTemplate(0), { 1, true });

	render::TemplateLayerPlanner planner;
	auto const plan = planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(plan.complete);
	QCOMPARE(plan.below_map.size(), std::size_t(1));
	QCOMPARE(imageCommandCount(plan.below_map.front()), std::size_t(1));
	auto const command = std::ranges::find_if(
		plan.below_map.front().scene->commands,
		[](auto const& value) { return std::holds_alternative<render::DrawImage>(value); }
	);
	QVERIFY(command != plan.below_map.front().scene->commands.end());
	auto const& image = std::get<render::DrawImage>(*command);
	QCOMPARE(image.source.x, 0.0);
	QCOMPARE(image.source.y, 0.0);
	QCOMPARE(image.source.width, 4.0);
	QCOMPARE(image.source.height, 3.0);
	QCOMPARE(image.image_to_scene.m11, 1.25);
	QCOMPARE(image.image_to_scene.m12, 0.2);
	QCOMPARE(image.image_to_scene.m21, -0.15);
	QCOMPARE(image.image_to_scene.m22, 0.9);
	QCOMPARE(image.image_to_scene.dx, -2.5);
	QCOMPARE(image.image_to_scene.dy, -1.25);
}

namespace {

void verifyGutterSeams(
	QImage source,
	std::size_t expected_images,
	int max_channel_delta)
{
	Map tiled_map;
	MapView tiled_view { &tiled_map };
	auto left = source.copy(0, 0, 9, 8);
	auto right = source.copy(7, 0, 9, 8);
	tiled_map.addTemplate(0, std::make_unique<SyntheticRasterTemplate>(
		&tiled_map,
		QVector<RasterTemplateTile> {
			tileRegion(std::move(left), { 0, 0, 8, 8 }, { -8, -4, 8, 8 }),
			tileRegion(std::move(right), { 1, 0, 8, 8 }, { 0, -4, 8, 8 }),
		},
		QRectF(-8, -4, 16, 8)
	));
	tiled_map.setFirstFrontTemplate(1);
	tiled_view.setTemplateVisibility(tiled_map.getTemplate(0), { 1, true });

	Map whole_map;
	MapView whole_view { &whole_map };
	whole_map.addTemplate(0, std::make_unique<SyntheticRasterTemplate>(
		&whole_map,
		QVector<RasterTemplateTile> {
			tileRegion(source, { 0, 0, 16, 8 }, { -8, -4, 16, 8 }),
		},
		QRectF(-8, -4, 16, 8)
	));
	whole_map.setFirstFrontTemplate(1);
	whole_view.setTemplateVisibility(whole_map.getTemplate(0), { 1, true });

	render::TemplateLayerPlanner tiled_planner;
	render::TemplateLayerPlanner whole_planner;
	auto tiled_plan = tiled_planner.plan(tiled_map, tiled_view, { -10, -7, 20, 14 }, 3.2);
	auto whole_plan = whole_planner.plan(whole_map, whole_view, { -10, -7, 20, 14 }, 3.2);
	QVERIFY(tiled_plan.complete);
	QVERIFY(whole_plan.complete);
	QCOMPARE(tiled_plan.newly_resident_images, expected_images);
	QCOMPARE(tiled_plan.below_map.size(), std::size_t(1));
	QCOMPARE(imageCommandCount(tiled_plan.below_map.front()), expected_images);

	auto const tiled_snapshot = tiled_map.publishRenderSnapshot();
	auto const whole_snapshot = whole_map.publishRenderSnapshot();
	QVERIFY(tiled_snapshot);
	QVERIFY(whole_snapshot);
	render::FramePlanner tiled_frame_planner;
	render::FramePlanner whole_frame_planner;
	auto const tiled_frame = scaledFrameFor(
		*tiled_snapshot, tiled_frame_planner, std::move(tiled_plan)
	);
	auto const whole_frame = scaledFrameFor(
		*whole_snapshot, whole_frame_planner, std::move(whole_plan)
	);
	auto const transparent = render::Color { 0, 0, 0, 0 };
	auto const tiled = renderVello(tiled_frame, transparent);
	auto const whole = renderVello(whole_frame, transparent);
	auto const tiled_reference = renderReference(*tiled_frame, Qt::transparent);
	auto const whole_reference = renderReference(*whole_frame, Qt::transparent);

	// The source boundary projects to x=32.25. Compare a narrow band across
	// it against one monolithic image, including fractional transform coverage.
	for (int y = 9; y <= 31; ++y)
	{
		for (int x = 29; x <= 35; ++x)
		{
			auto const actual = tiled.pixelColor(x, y);
			auto const expected = whole.pixelColor(x, y);
			auto const reference_actual = tiled_reference.pixelColor(x, y);
			auto const reference_expected = whole_reference.pixelColor(x, y);
			QVERIFY2(
				std::abs(actual.red() - expected.red()) <= max_channel_delta
				&& std::abs(actual.green() - expected.green()) <= max_channel_delta
				&& std::abs(actual.blue() - expected.blue()) <= max_channel_delta
				&& std::abs(actual.alpha() - expected.alpha()) <= max_channel_delta,
				qPrintable(QStringLiteral("tile seam at %1,%2: %3 vs %4")
				           .arg(x).arg(y).arg(actual.name(QColor::HexArgb),
				                              expected.name(QColor::HexArgb)))
			);
			QVERIFY2(
				std::abs(reference_actual.red() - reference_expected.red()) <= max_channel_delta
				&& std::abs(reference_actual.green() - reference_expected.green()) <= max_channel_delta
				&& std::abs(reference_actual.blue() - reference_expected.blue()) <= max_channel_delta
				&& std::abs(reference_actual.alpha() - reference_expected.alpha()) <= max_channel_delta,
				qPrintable(QStringLiteral("reference tile seam at %1,%2: %3 vs %4")
				           .arg(x).arg(y).arg(reference_actual.name(QColor::HexArgb),
				                              reference_expected.name(QColor::HexArgb)))
			);
		}
	}
}

}  // namespace

void TemplateLayerPlannerTest::preservesOpaqueGuttersWithoutTileSeams()
{
	QImage source(16, 8, QImage::Format_RGB32);
	for (int y = 0; y < source.height(); ++y)
	{
		for (int x = 0; x < source.width(); ++x)
		{
			source.setPixelColor(
				x, y,
				QColor(20 + x * 12, 190 - x * 7, 30 + y * 20)
			);
		}
	}
	verifyGutterSeams(std::move(source), 2, 8);
}

void TemplateLayerPlannerTest::preservesTransparentGuttersWithoutTileSeams()
{
	QImage source(16, 8, QImage::Format_RGBA8888);
	for (int y = 0; y < source.height(); ++y)
	{
		for (int x = 0; x < source.width(); ++x)
		{
			source.setPixelColor(
				x, y,
				QColor(20 + x * 12, 190 - x * 7, 30 + y * 20, 80 + x * 8)
			);
		}
	}
	verifyGutterSeams(std::move(source), 1, 2);
}

QTEST_MAIN(TemplateLayerPlannerTest)
