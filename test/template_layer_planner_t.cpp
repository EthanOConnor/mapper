/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "template_layer_planner_t.h"

#include <algorithm>
#include <memory>
#include <variant>
#include <vector>

#include <QtTest>
#include <QDir>
#include <QImage>
#include <QPainter>
#include <QVector>

#include "global.h"
#include "test_config.h"
#include "core/map.h"
#include "core/map_view.h"
#include "render/frame_pipeline.h"
#include "render/qpainter_frame_renderer.h"
#include "render/template_layer_planner.h"
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

private:
	QVector<RasterTemplateTile> tiles_;
	QRectF extent_;
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

QImage renderReference(const render::FramePacket& frame)
{
	QImage image(16, 16, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	QPainter painter(&image);
	auto const completion = render::QPainterFrameRenderer().render(painter, frame);
	Q_ASSERT(completion.status == render::FrameStatus::Presented);
	return image;
}

std::vector<render::ImagePtr> images(const render::VectorPass& pass)
{
	std::vector<render::ImagePtr> result;
	for (auto const& command : pass.scene->commands)
		if (auto const* image = std::get_if<render::DrawImage>(&command))
			result.push_back(image->image);
	return result;
}

QImage renderFrame(const render::FramePacketPtr& frame)
{
	QImage image(int(frame->view.width), int(frame->view.height),
	             QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	QPainter painter(&image);
	auto const completion = render::QPainterFrameRenderer().render(painter, *frame);
	Q_ASSERT(completion.status == render::FrameStatus::Presented);
	painter.end();
	return image;
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
	QVERIFY(!reference.isNull());
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
	auto plan = planner.plan(map, view, visible, 4);
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

void TemplateLayerPlannerTest::boundsImageAdmissionAndPreservesQtImageIdentity()
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
	std::vector<render::ImagePtr> first_images;

	{
		auto first = template_planner.plan(map, view, { -8, -8, 16, 16 }, 1);
		QVERIFY(!first.complete);
		QCOMPARE(first.newly_resident_images, std::size_t(4));
		QCOMPARE(first.below_map.size(), std::size_t(1));
		QCOMPARE(imageCommandCount(first.below_map.front()), std::size_t(4));
		first_images = images(first.below_map.front());
		QCOMPARE(first_images.size(), std::size_t(4));
	}

	auto second = template_planner.plan(map, view, { -8, -8, 16, 16 }, 1);
	QVERIFY(second.complete);
	QCOMPARE(second.newly_resident_images, std::size_t(2));
	QCOMPARE(second.below_map.size(), std::size_t(1));
	QCOMPARE(imageCommandCount(second.below_map.front()), std::size_t(6));
	auto const complete_images = images(second.below_map.front());
	QCOMPARE(complete_images.size(), std::size_t(6));
	for (auto index = std::size_t(0); index < first_images.size(); ++index)
		QCOMPARE(complete_images[index], first_images[index]);
	auto const complete_scene = second.below_map.front().scene;
	QVERIFY(frameFor(*snapshot, frame_planner, std::move(second)));

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

	auto const tiled_snapshot = tiled_map.publishRenderSnapshot();
	auto const whole_snapshot = whole_map.publishRenderSnapshot();
	QVERIFY(tiled_snapshot);
	QVERIFY(whole_snapshot);
	render::FramePlanner tiled_frame_planner;
	render::FramePlanner whole_frame_planner;
	auto const tiled = renderFrame(scaledFrameFor(
		*tiled_snapshot, tiled_frame_planner, std::move(tiled_plan)
	));
	auto const whole = renderFrame(scaledFrameFor(
		*whole_snapshot, whole_frame_planner, std::move(whole_plan)
	));

	// The source boundary projects to x=32.25. Compare a narrow band across
	// it against one monolithic image, including fractional transform coverage.
	for (int y = 9; y <= 31; ++y)
	{
		for (int x = 29; x <= 35; ++x)
		{
			auto const actual = tiled.pixelColor(x, y);
			auto const expected = whole.pixelColor(x, y);
			QVERIFY2(
				std::abs(actual.red() - expected.red()) <= 2
				&& std::abs(actual.green() - expected.green()) <= 2
				&& std::abs(actual.blue() - expected.blue()) <= 2,
				qPrintable(QStringLiteral("tile seam at %1,%2: %3 vs %4")
				           .arg(x).arg(y).arg(actual.name(QColor::HexArgb),
				                              expected.name(QColor::HexArgb)))
			);
		}
	}
}

QTEST_MAIN(TemplateLayerPlannerTest)
