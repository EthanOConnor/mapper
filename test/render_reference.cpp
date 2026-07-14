/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    Manual visual-parity helper for the immutable-snapshot reference path.
 *    The optional final argument expands only the render request, which
 *    isolates viewport pre-clipping effects.
 */

#include <algorithm>

#include <QApplication>
#include <QImage>
#include <QPainter>

#include "global.h"
#include "core/map.h"
#include "core/renderables/renderable.h"
#include "render/qpainter_renderer.h"
#include "render/qt_render_bridge.h"

using namespace OpenOrienteering;

int main(int argc, char** argv)
{
	QApplication app(argc, argv);
	Q_INIT_RESOURCE(resources);
	doStaticInitializations();
	if (app.arguments().size() < 4 || app.arguments().size() > 5)
		return 2;

	bool valid_scale = false;
	auto const pixels_per_mm = app.arguments()[3].toDouble(&valid_scale);
	if (!valid_scale || pixels_per_mm <= 0)
		return 2;
	auto render_padding = 0.0;
	if (app.arguments().size() == 5)
		render_padding = app.arguments()[4].toDouble(&valid_scale);
	if (!valid_scale || render_padding < 0)
		return 2;

	Map map;
	if (!map.loadFrom(app.arguments()[1]))
		return 3;
	auto const extent = map.calculateExtent(true).toAlignedRect().adjusted(-1, -1, 1, 1);
	auto const pixel_size = QSize(
		std::max(1, qCeil(extent.width() * pixels_per_mm)),
		std::max(1, qCeil(extent.height() * pixels_per_mm))
	);
	if (pixel_size.width() > 16384 || pixel_size.height() > 16384)
		return 4;

	QImage image(pixel_size, QImage::Format_ARGB32_Premultiplied);
	image.fill(Qt::white);
	QPainter painter(&image);
	painter.setRenderHint(QPainter::Antialiasing, true);
	painter.setRenderHint(QPainter::TextAntialiasing, true);
	painter.scale(pixels_per_mm, pixels_per_mm);
	painter.translate(-extent.topLeft());
	painter.setClipRect(extent);
	auto const snapshot = map.publishRenderSnapshot();
	render::QPainterRenderer().draw(painter, *snapshot, {
		render::fromQRectF(
			extent.adjusted(-render_padding, -render_padding, render_padding, render_padding)
		),
		pixels_per_mm,
		RenderConfig::HelperSymbols,
		1,
	});
	painter.end();
	return image.save(app.arguments()[2], "PNG", 1) ? 0 : 5;
}
