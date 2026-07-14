/*
 *    Copyright 2020 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */


#include <QtGlobal>
#include <QtTest>
#include <QObject>
#include <QImage>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPixmap>
#include <QPrinterInfo>
#include <QRectF>
#include <QString>

#include "core/map.h"
#include "core/map_color.h"
#include "core/map_printer.h"
#include "core/objects/object.h"
#include "core/symbols/line_symbol.h"

using namespace OpenOrienteering;

namespace {

constexpr int test_dpi = 600;
constexpr qreal test_page_mm = 30;

class RecordingWindowsPrintEngine final : public QPaintEngine
{
public:
	RecordingWindowsPrintEngine()
	: QPaintEngine(QPaintEngine::AllFeatures)
	{}

	Type type() const override
	{
		return Windows;
	}

	bool begin(QPaintDevice* device) override
	{
		setPaintDevice(device);
		setActive(true);
		return true;
	}

	bool end() override
	{
		setActive(false);
		return true;
	}

	void updateState(const QPaintEngineState&) override
	{}

	void drawPixmap(const QRectF& target, const QPixmap& source, const QRectF& source_rect) override
	{
		recordRaster(target, source.toImage(), source_rect);
	}

	void drawPath(const QPainterPath&) override
	{
		++unexpected_vector_calls;
	}

	void drawImage(
		const QRectF& target, const QImage& source, const QRectF& source_rect,
		Qt::ImageConversionFlags
	) override
	{
		recordRaster(target, source, source_rect);
	}

	void recordRaster(const QRectF& target, const QImage& source, const QRectF& source_rect)
	{
		++raster_calls;
		image_target = target;
		image_source = source.copy(source_rect.toAlignedRect());
	}

	int raster_calls = 0;
	int unexpected_vector_calls = 0;
	QRectF image_target;
	QImage image_source;
};

class RecordingWindowsPrintDevice final : public QPaintDevice
{
public:
	QPaintEngine* paintEngine() const override
	{
		return const_cast<RecordingWindowsPrintEngine*>(&engine);
	}

	mutable RecordingWindowsPrintEngine engine;

protected:
	int metric(PaintDeviceMetric value) const override
	{
		const auto pixels = qCeil(test_page_mm * test_dpi / 25.4);
		switch (value)
		{
		case PdmWidth:
		case PdmHeight:
			return pixels;
		case PdmWidthMM:
		case PdmHeightMM:
			return int(test_page_mm);
		case PdmDpiX:
		case PdmDpiY:
		case PdmPhysicalDpiX:
		case PdmPhysicalDpiY:
			return test_dpi;
		case PdmNumColors:
			return 1 << 24;
		case PdmDepth:
			return 32;
		case PdmDevicePixelRatio:
			return 1;
		case PdmDevicePixelRatioScaled:
			return int(devicePixelRatioFScale());
		default:
			return 0;
		}
	}
};

class TestMapPrinter final : public MapPrinter
{
public:
	using MapPrinter::MapPrinter;

	void configurePrecisionFixture()
	{
		page_format = MapPrinterPageFormat(QSizeF(test_page_mm, test_page_mm), 0, 0);
		print_area = QRectF(0, 0, test_page_mm, test_page_mm);
		options.resolution = test_dpi;
		options.mode = MapPrinterOptions::Vector;
		scale_adjustment = 1;
		updatePageBreaks();
	}
};

void makePrecisionFixtureMap(Map& map)
{
	map.setScaleDenominator(10'000);

	auto* black = new MapColor;
	black->setCmyk(MapColorCmyk(0, 0, 0, 1));
	black->setOpacity(1);
	map.addColor(black, 0);

	auto* line_symbol = new LineSymbol;
	line_symbol->setLineWidth(500);
	line_symbol->setColor(black);
	map.addSymbol(line_symbol, 0);

	auto* line = new PathObject(line_symbol);
	line->addCoordinate(MapCoord(2.25, 15.125));
	line->addCoordinate(MapCoord(27.75, 15.125));
	map.addObject(line);
}

}  // namespace


/**
 * @test Tests printing and export features
 */
class MapPrinterTest : public QObject
{
Q_OBJECT
private slots:
	void isPrinterTest()
	{
		QPrinterInfo printer_info;
		QCOMPARE(MapPrinter::isPrinter(&printer_info), true);
		QCOMPARE(MapPrinter::isPrinter(MapPrinter::imageTarget()), false);
		QCOMPARE(MapPrinter::isPrinter(MapPrinter::kmzTarget()), false);
		QCOMPARE(MapPrinter::isPrinter(MapPrinter::pdfTarget()), false);
	}

	void windowsPrintUsesConfiguredDpiPageSpool()
	{
		Map map;
		makePrecisionFixtureMap(map);
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		QCOMPARE(printer.getOptions().resolution, test_dpi);

		RecordingWindowsPrintDevice device;
		QPainter painter(&device);
		QVERIFY(painter.isActive());
		printer.drawPage(&painter, printer.getPrintArea());
		QVERIFY(painter.isActive());
		painter.end();

		const auto expected_pixels = qCeil(test_page_mm * test_dpi / 25.4);
		QCOMPARE(device.engine.unexpected_vector_calls, 0);
		QCOMPARE(device.engine.raster_calls, 1);
		QCOMPARE(device.engine.image_target, QRectF(0, 0, expected_pixels, expected_pixels));
		QCOMPARE(device.engine.image_source.size(), QSize(expected_pixels, expected_pixels));

		bool has_ink = false;
		for (int y = 0; y < device.engine.image_source.height() && !has_ink; ++y)
		{
			for (int x = 0; x < device.engine.image_source.width(); ++x)
			{
				if (device.engine.image_source.pixelColor(x, y) != QColor(Qt::white))
				{
					has_ink = true;
					break;
				}
			}
		}
		QVERIFY2(has_ink, "The precision page spool must contain rendered map geometry");

		// One half pixel is the maximum placement quantization introduced by
		// the configured-DPI spool. At Mapper's 600 DPI default that is < 0.022 mm.
		const auto max_quantization_mm = 25.4 / (2 * test_dpi);
		QVERIFY(max_quantization_mm < 0.022);
	}
	
};



/*
 * We don't need a real GUI window.
 * 
 * But we discovered QTBUG-58768 macOS: Crash when using QPrinter
 * while running with "minimal" platform plugin.
 */
#ifndef Q_OS_MACOS
namespace  {
	[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");  // clazy:exclude=non-pod-global-static
}
#endif


QTEST_MAIN(MapPrinterTest)
#include "map_printer_t.moc"  // IWYU pragma: keep
