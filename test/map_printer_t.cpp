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


#include <memory>

#include <QtGlobal>
#include <QtTest>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPaintDevice>
#include <QPaintEngine>
#include <QPainter>
#include <QPdfWriter>
#include <QPixmap>
#include <QPrinterInfo>
#include <QProgressDialog>
#include <QRectF>
#include <QString>
#include <QTemporaryDir>

#include "core/georeferencing.h"
#include "core/map.h"
#include "core/map_color.h"
#include "core/map_printer.h"
#include "core/objects/object.h"
#include "core/symbols/line_symbol.h"
#include "gui/print_widget.h"
#include "templates/template_image.h"
#include "templates/template_map.h"
#include "templates/world_file.h"

#ifdef MAPPER_USE_GDAL
#include "gdal/kmz_groundoverlay_export.h"
#endif

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

class PreparingTemplate final : public TemplateImage
{
public:
	explicit PreparingTemplate(Map* map, int pending_calls, bool fail = false)
	 : TemplateImage(QString {}, map)
	 , pending_calls(pending_calls)
	 , fail(fail)
	{
		image = QImage(2, 2, QImage::Format_RGB32);
		image.fill(Qt::red);
		setTemplateState(Loaded);
	}

	OutputRenderPreparation prepareForOutput(
		const QRectF& map_rect,
		double pixels_per_map_unit) override
	{
		++prepare_calls;
		last_map_rect = map_rect;
		last_pixels_per_map_unit = pixels_per_map_unit;
		if (fail)
		{
			return {
				OutputRenderPreparation::State::Failed,
				0,
				1,
				QStringLiteral("synthetic preparation failure"),
			};
		}
		if (prepare_calls <= pending_calls)
		{
			return {
				OutputRenderPreparation::State::Pending,
				0,
				1,
				{},
			};
		}
		return {
			OutputRenderPreparation::State::Ready,
			1,
			1,
			{},
		};
	}

	void finishOutputPreparation(bool cancelled) override
	{
		++finish_calls;
		finished_cancelled = cancelled;
	}

	void collectRasterTiles(
		const QRectF& map_clip_rect,
		double scale,
		bool on_screen,
		QVector<RasterTemplateTile>& out) const override
	{
		rendered_map_rects.push_back(map_clip_rect);
		TemplateImage::collectRasterTiles(
			map_clip_rect, scale, on_screen, out);
	}

	int pending_calls = 0;
	bool fail = false;
	int prepare_calls = 0;
	int finish_calls = 0;
	bool finished_cancelled = false;
	QRectF last_map_rect;
	double last_pixels_per_map_unit = 0;
	mutable QVector<QRectF> rendered_map_rects;
};

class NestedTemplateMap final : public TemplateMap
{
public:
	NestedTemplateMap(Map* map, std::unique_ptr<Map> child_map)
	 : TemplateMap(QString {}, map)
	{
		setTemplateMap(std::move(child_map));
		setTemplateState(Loaded);
	}

	bool includesChildTemplates() const noexcept override
	{
		return true;
	}
};

class MissingRasterTemplate final : public TemplateImage
{
public:
	explicit MissingRasterTemplate(Map* map)
	 : TemplateImage(QString {}, map)
	{
		image = QImage(1, 1, QImage::Format_RGBA8888);
		image.fill(Qt::transparent);
		setTemplateState(Loaded);
	}

	void collectRasterTiles(
		const QRectF& map_clip_rect,
		double,
		bool,
		QVector<RasterTemplateTile>& out) const override
	{
		out.push_back({
			{}, map_clip_rect, {}, 0, true, false
		});
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

void georeferenceFixtureMap(Map& map)
{
	Georeferencing georef;
	georef.setScaleDenominator(10'000);
	auto const valid = georef.setProjectedCRS(
		QStringLiteral("EPSG:3857"), QStringLiteral("EPSG:3857"));
	Q_ASSERT(valid);
	georef.setProjectedRefPoint({ 0, 0 }, false, false);
	map.setGeoreferencing(georef);
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

	void exactTemplatePreparationIsPolledAndFinished()
	{
		Map map;
		auto source = std::make_unique<PreparingTemplate>(&map, 2);
		auto* source_ptr = source.get();
		map.addTemplate(0, std::move(source));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);

		QVERIFY(printer.prepareOutput());
		QCOMPARE(source_ptr->prepare_calls, 3);
		QCOMPARE(source_ptr->finish_calls, 0);
		printer.finishOutput(false);
		QCOMPARE(source_ptr->finish_calls, 1);
		QVERIFY(!source_ptr->finished_cancelled);
	}

	void exactTemplatePreparationReportsFailure()
	{
		Map map;
		auto source = std::make_unique<PreparingTemplate>(&map, 0, true);
		auto* source_ptr = source.get();
		map.addTemplate(0, std::move(source));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);

		QVERIFY(!printer.prepareOutput());
		QCOMPARE(
			printer.outputError(),
			QStringLiteral("synthetic preparation failure"));
		QVERIFY(!printer.outputWasCanceled());
		QCOMPARE(source_ptr->finish_calls, 1);
		QVERIFY(source_ptr->finished_cancelled);
	}

	void exactTemplatePreparationReportsCancellation()
	{
		Map map;
		auto source = std::make_unique<PreparingTemplate>(
			&map, 100);
		auto* source_ptr = source.get();
		map.addTemplate(0, std::move(source));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);
		connect(
			&printer,
			&MapPrinter::printProgress,
			&printer,
			[&printer](int value, const QString&) {
				if (value > 0 && value < 100)
					printer.cancelPrintMap();
			});

		QVERIFY(!printer.prepareOutput());
		QVERIFY(printer.outputWasCanceled());
		QCOMPARE(
			printer.outputError(),
			QStringLiteral("Canceled"));
		QCOMPARE(source_ptr->finish_calls, 1);
		QVERIFY(source_ptr->finished_cancelled);
	}

	void pageRenderingCancellationIsNotSuccess()
	{
		Map map;
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(false);

		QBuffer bytes;
		QVERIFY(bytes.open(QIODevice::WriteOnly));
		auto writer = printer.makePdfWriter(&bytes);
		connect(
			&printer,
			&MapPrinter::printProgress,
			&printer,
			[&printer](int value, const QString&) {
				if (value < 100)
					printer.cancelPrintMap();
			});

		QVERIFY(!printer.printMap(writer.get()));
		QVERIFY(printer.outputWasCanceled());
		QCOMPARE(
			printer.outputError(),
			QStringLiteral("Canceled"));
	}

	void imageAndWorldFileExportCommitTogether()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto image_path =
			directory.filePath(QStringLiteral("map.png"));
		const auto world_path =
			WorldFile::pathForImage(image_path);

		QFile previous_image(image_path);
		QVERIFY(previous_image.open(QIODevice::WriteOnly));
		QCOMPARE(
			previous_image.write("previous image"),
			qint64(sizeof("previous image") - 1));
		previous_image.close();
		QFile previous_world(world_path);
		QVERIFY(previous_world.open(QIODevice::WriteOnly));
		QCOMPARE(
			previous_world.write("previous world"),
			qint64(sizeof("previous world") - 1));
		previous_world.close();

		QImage image(3, 2, QImage::Format_ARGB32_Premultiplied);
		image.fill(QColor(Qt::red));
		const WorldFile expected(1, 2, 3, 4, 5, 6);
		QString error;
		QVERIFY2(
			PrintWidgetUtil::saveImageExport(
				image_path,
				image,
				QByteArray("PNG"),
				&expected,
				&error),
			qPrintable(error));

		const QImage actual_image(image_path);
		QCOMPARE(actual_image.size(), image.size());
		QCOMPARE(actual_image.pixelColor(1, 1), QColor(Qt::red));
		WorldFile actual_world;
		QVERIFY(actual_world.load(world_path));
		for (auto i = 0; i < 6; ++i)
			QCOMPARE(actual_world.parameters[i], expected.parameters[i]);
	}

	void worldFileFailurePreservesExistingPair()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		const auto image_path =
			directory.filePath(QStringLiteral("map.png"));
		const auto world_path =
			WorldFile::pathForImage(image_path);
		const QByteArray previous_image("previous image");
		const QByteArray previous_world(1024 * 1024 + 1, 'w');

		QFile image_file(image_path);
		QVERIFY(image_file.open(QIODevice::WriteOnly));
		QCOMPARE(
			image_file.write(previous_image),
			qint64(previous_image.size()));
		image_file.close();
		QFile world_file_output(world_path);
		QVERIFY(world_file_output.open(QIODevice::WriteOnly));
		QCOMPARE(
			world_file_output.write(previous_world),
			qint64(previous_world.size()));
		world_file_output.close();

		QImage image(3, 2, QImage::Format_ARGB32_Premultiplied);
		image.fill(QColor(Qt::blue));
		const WorldFile world_file(1, 0, 0, -1, 5, 6);
		QString error;
		QVERIFY(!PrintWidgetUtil::saveImageExport(
			image_path,
			image,
			QByteArray("PNG"),
			&world_file,
			&error));
		QVERIFY(!error.isEmpty());

		QVERIFY(image_file.open(QIODevice::ReadOnly));
		QCOMPARE(image_file.readAll(), previous_image);
		QVERIFY(world_file_output.open(QIODevice::ReadOnly));
		QCOMPARE(world_file_output.readAll(), previous_world);
	}

	void imageWorldTransactionRecoversInterruptedCommit()
	{
		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const image_path =
			directory.filePath(
				QStringLiteral("recover.png"));
		auto const world_path =
			WorldFile::pathForImage(image_path);
		auto const backup_name =
			QStringLiteral(
				".recover.png.mapper-backup-"
				"0123456789abcdef0123456789abcdef");
		auto const backup_path =
			directory.filePath(backup_name);
		auto const journal_path =
			image_path
			+ QStringLiteral(
				".mapper-export.json");
		QImage old_image(
			8, 8, QImage::Format_RGBA8888);
		old_image.fill(Qt::red);
		QVERIFY(old_image.save(image_path, "PNG"));
		QFile old_image_file(image_path);
		QVERIFY(old_image_file.open(QIODevice::ReadOnly));
		auto const old_image_bytes =
			old_image_file.readAll();
		old_image_file.close();
		QFile old_world(world_path);
		QVERIFY(old_world.open(QIODevice::WriteOnly));
		QByteArray const old_world_bytes(
			"old world");
		QCOMPARE(
			old_world.write(old_world_bytes),
			qint64(old_world_bytes.size()));
		auto const old_permissions =
			old_world.permissions();
		old_world.close();

		auto writeJournal = [&] {
			QFile journal(journal_path);
			if (!journal.open(
				    QIODevice::WriteOnly))
				return false;
			auto const bytes =
				QJsonDocument(QJsonObject {
					{ QStringLiteral("format"),
					  QStringLiteral(
						  "org.openorienteering."
						  "image-export-transaction") },
					{ QStringLiteral("version"), 1 },
					{ QStringLiteral(
						  "oldImageExisted"),
					  true },
					{ QStringLiteral(
						  "imageBackupName"),
					  backup_name },
					{ QStringLiteral(
						  "worldExisted"),
					  true },
					{ QStringLiteral(
						  "worldContentsBase64"),
					  QString::fromLatin1(
						  old_world_bytes
							  .toBase64()) },
					{ QStringLiteral(
						  "worldPermissions"),
					  int(old_permissions) },
				}).toJson(
					QJsonDocument::Compact);
			return journal.write(bytes)
			       == bytes.size();
		};

		// Crash after publishing the new world file but before the image:
		// recovery must restore the old image and sidecar before attempting a
		// new export.
		QVERIFY(QFile::rename(
			image_path, backup_path));
		QFile interrupted_world(world_path);
		QVERIFY(interrupted_world.open(
			QIODevice::WriteOnly
			| QIODevice::Truncate));
		QVERIFY(
			interrupted_world.write("new world")
			> 0);
		interrupted_world.close();
		QVERIFY(writeJournal());
		QImage replacement(
			8, 8, QImage::Format_RGBA8888);
		replacement.fill(Qt::green);
		QString error;
		WorldFile world_file(
			1, 0, 0, -1, 5, 6);

		// A failed sidecar rollback must leave the old image unpublished and
		// preserve both the backup and journal for a later retry.
		QVERIFY(QFile::remove(world_path));
		QVERIFY(QDir().mkpath(world_path));
		QVERIFY(!PrintWidgetUtil::saveImageExport(
			image_path,
			replacement,
			QByteArray("not-an-image-format"),
			&world_file,
			&error));
		QVERIFY(!error.isEmpty());
		QVERIFY(!QFileInfo::exists(image_path));
		QVERIFY(QFileInfo::exists(backup_path));
		QVERIFY(QFileInfo::exists(journal_path));
		QVERIFY(QFileInfo(world_path).isDir());

		QVERIFY(QDir(world_path).removeRecursively());
		error.clear();
		QVERIFY(!PrintWidgetUtil::saveImageExport(
			image_path,
			replacement,
			QByteArray("not-an-image-format"),
			&world_file,
			&error));
		QVERIFY(!error.isEmpty());
		QVERIFY(!QFileInfo::exists(backup_path));
		QVERIFY(!QFileInfo::exists(journal_path));
		QFile restored_image(image_path);
		QVERIFY(restored_image.open(
			QIODevice::ReadOnly));
		QCOMPARE(
			restored_image.readAll(),
			old_image_bytes);
		restored_image.close();
		QFile restored_world(world_path);
		QVERIFY(restored_world.open(
			QIODevice::ReadOnly));
		QCOMPARE(
			restored_world.readAll(),
			old_world_bytes);
		restored_world.close();

		// Crash after both new files were published: recovery keeps the new
		// pair and only removes the old backup and journal.
		QVERIFY(QFile::rename(
			image_path, backup_path));
		QImage published(
			8, 8, QImage::Format_RGBA8888);
		published.fill(Qt::blue);
		QVERIFY(published.save(image_path, "PNG"));
		QFile published_image(image_path);
		QVERIFY(published_image.open(
			QIODevice::ReadOnly));
		auto const published_bytes =
			published_image.readAll();
		published_image.close();
		QFile published_world(world_path);
		QVERIFY(published_world.open(
			QIODevice::WriteOnly
			| QIODevice::Truncate));
		QByteArray const published_world_bytes(
			"published world");
		QCOMPARE(
			published_world.write(
				published_world_bytes),
			qint64(
				published_world_bytes.size()));
		published_world.close();
		QVERIFY(writeJournal());
		error.clear();
		QVERIFY(!PrintWidgetUtil::saveImageExport(
			image_path,
			replacement,
			QByteArray("not-an-image-format"),
			&world_file,
			&error));
		QVERIFY(!QFileInfo::exists(backup_path));
		QVERIFY(!QFileInfo::exists(journal_path));
		QVERIFY(published_image.open(
			QIODevice::ReadOnly));
		QCOMPARE(
			published_image.readAll(),
			published_bytes);
		QVERIFY(published_world.open(
			QIODevice::ReadOnly));
		QCOMPARE(
			published_world.readAll(),
			published_world_bytes);
	}

	void exactTemplatePreparationIncludesNestedTemplates()
	{
		Map map;
		map.setScaleDenominator(10'000);
		auto child_map = std::make_unique<Map>();
		auto child = std::make_unique<PreparingTemplate>(
			child_map.get(), 1);
		auto* child_ptr = child.get();
		child_map->addTemplate(0, std::move(child));
		map.addTemplate(
			0,
			std::make_unique<NestedTemplateMap>(
				&map, std::move(child_map)));

		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);
		QVERIFY(printer.prepareOutput());
		QCOMPARE(child_ptr->prepare_calls, 2);
		QCOMPARE(
			child_ptr->last_map_rect,
			QRectF(0, 0, test_page_mm, test_page_mm));
		QCOMPARE(
			child_ptr->last_pixels_per_map_unit,
			double(test_dpi) / 25.4);
		printer.finishOutput(false);
		QCOMPARE(child_ptr->finish_calls, 1);
		QVERIFY(!child_ptr->finished_cancelled);
	}

	void exactTemplatePreparationAcceptsAreaOverride()
	{
		Map map;
		auto source = std::make_unique<PreparingTemplate>(&map, 0);
		auto* source_ptr = source.get();
		map.addTemplate(0, std::move(source));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);
		const QRectF requested(-7, -9, 44, 48);

		QVERIFY(printer.prepareOutput(requested));
		QCOMPARE(source_ptr->last_map_rect, requested);
		printer.finishOutput(false);
	}

	void exactOutputRejectsUnpreparedMapClip()
	{
		Map map;
		auto source = std::make_unique<PreparingTemplate>(&map, 0);
		map.addTemplate(0, std::move(source));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);
		QVERIFY(printer.prepareOutput(QRectF(0, 0, 10, 10)));

		QImage image(128, 128, QImage::Format_ARGB32_Premultiplied);
		image.fill(Qt::white);
		QPainter painter(&image);
		QVERIFY(painter.isActive());
		printer.drawPage(&painter, QRectF(20, 20, 5, 5), &image);
		QVERIFY(!painter.isActive());
		QCOMPARE(
			printer.outputError(),
			QStringLiteral(
				"A page was rendered outside the prepared exact-output area."));
		printer.finishOutput(true);
	}

#ifdef MAPPER_USE_GDAL
	void kmzPreflightMatchesActuallyRenderedExtent()
	{
		Map map;
		georeferenceFixtureMap(map);
		auto source = std::make_unique<PreparingTemplate>(&map, 0);
		auto* source_ptr = source.get();
		map.addTemplate(0, std::move(source));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);

		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const path =
			directory.filePath(QStringLiteral("coverage.kmz"));
		KmzGroundOverlayExport exporter(path, map);
		QVERIFY2(exporter.doExport(printer, 256),
		         qPrintable(exporter.errorString()));

		auto const print_area = printer.getPrintArea();
		QCOMPARE(source_ptr->last_map_rect, print_area);
		QVERIFY(source_ptr->rendered_map_rects.size() > 1);
		for (auto const& rendered : source_ptr->rendered_map_rects)
			QVERIFY(source_ptr->last_map_rect.contains(rendered));
		QCOMPARE(source_ptr->finish_calls, 1);
		QVERIFY(!source_ptr->finished_cancelled);
	}

	void kmzRenderFailureIsPropagatedAndCleaned()
	{
		Map map;
		georeferenceFixtureMap(map);
		map.addTemplate(
			0, std::make_unique<MissingRasterTemplate>(&map));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);

		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const path = directory.filePath(QStringLiteral("failed.kmz"));
		KmzGroundOverlayExport exporter(path, map);
		QVERIFY(!exporter.doExport(printer, 256));
		QVERIFY(!exporter.errorString().isEmpty());
		QVERIFY(!QFile::exists(path));
	}

	void failedKmzPreservesExistingDestination()
	{
		Map map;
		georeferenceFixtureMap(map);
		map.addTemplate(
			0, std::make_unique<MissingRasterTemplate>(&map));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);

		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const path = directory.filePath(
			QStringLiteral("existing.kmz"));
		QFile existing(path);
		QVERIFY(existing.open(QIODevice::WriteOnly));
		QCOMPARE(
			existing.write("original archive"),
			qint64(sizeof("original archive") - 1));
		existing.close();

		KmzGroundOverlayExport exporter(path, map);
		QVERIFY(!exporter.doExport(printer, 256));
		QVERIFY(existing.open(QIODevice::ReadOnly));
		QCOMPARE(
			existing.readAll(),
			QByteArray("original archive"));
	}

	void failedKmlPreservesExistingDestinationAndCleansTiles()
	{
		Map map;
		georeferenceFixtureMap(map);
		map.addTemplate(
			0, std::make_unique<MissingRasterTemplate>(&map));
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(true);

		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const path = directory.filePath(
			QStringLiteral("existing.kml"));
		QFile existing(path);
		QVERIFY(existing.open(QIODevice::WriteOnly));
		QCOMPARE(
			existing.write("original kml"),
			qint64(sizeof("original kml") - 1));
		existing.close();

		KmzGroundOverlayExport exporter(path, map);
		QVERIFY(!exporter.doExport(printer, 256));
		QVERIFY(existing.open(QIODevice::ReadOnly));
		QCOMPARE(
			existing.readAll(),
			QByteArray("original kml"));
		QVERIFY(!QFileInfo::exists(
			directory.filePath(QStringLiteral("files"))));
	}

	void directKmlPublishesCrashRecoverableAssetDirectory()
	{
		Map map;
		georeferenceFixtureMap(map);
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(false);

		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const files_path =
			directory.filePath(
				QStringLiteral("files"));
		auto const path = directory.filePath(
			QStringLiteral("transactional.kml"));
		KmzGroundOverlayExport exporter(path, map);
		QVERIFY2(
			exporter.doExport(printer, 256),
			qPrintable(exporter.errorString()));

		QDir files(files_path);
		auto assets = files.entryList(
			{ QStringLiteral(
				".mapper-kml-*-assets-*") },
			QDir::Dirs | QDir::Hidden
				| QDir::NoDotAndDotDot);
		QCOMPARE(assets.size(), 1);
		auto const marker =
			QStringLiteral("-assets-");
		auto const marker_position =
			assets.front().lastIndexOf(marker);
		QVERIFY(marker_position > 0);
		auto const orphan_name =
			assets.front().left(
				marker_position
				+ marker.size())
			+ QStringLiteral("orphan");
		auto const orphan_path =
			files.filePath(orphan_name);
		QVERIFY(QDir().mkpath(orphan_path));
		QFile partial(
			orphan_path
			+ QStringLiteral("/partial.jpg"));
		QVERIFY(partial.open(QIODevice::WriteOnly));
		QVERIFY(partial.write("partial") > 0);
		partial.close();

		KmzGroundOverlayExport replacement(path, map);
		QVERIFY2(
			replacement.doExport(printer, 256),
			qPrintable(
				replacement.errorString()));
		QVERIFY(!QFileInfo::exists(orphan_path));
		assets = files.entryList(
			{ QStringLiteral(
				".mapper-kml-*-assets-*") },
			QDir::Dirs | QDir::Hidden
				| QDir::NoDotAndDotDot);
		QCOMPARE(assets.size(), 1);
		QFile kml(path);
		QVERIFY(kml.open(QIODevice::ReadOnly));
		auto const document = kml.readAll();
		QByteArray const asset_reference =
			QByteArray("files/")
			+ assets.front().toUtf8()
			+ QByteArray("/tile_");
		QVERIFY(document.contains(asset_reference));
		QVERIFY(
			QDir(
				files.filePath(
					assets.front()))
				.entryList(
					{ QStringLiteral(
						"tile_*.jpg") },
					QDir::Files)
				.size()
			> 0);

		auto const primary_assets =
			files.filePath(assets.front());
		auto const second_path =
			directory.filePath(
				QStringLiteral("second.kml"));
		KmzGroundOverlayExport second(
			second_path, map);
		QVERIFY2(
			second.doExport(printer, 256),
			qPrintable(second.errorString()));
		QVERIFY(QFileInfo::exists(primary_assets));
		QCOMPARE(
			files.entryList(
				{ QStringLiteral(
					".mapper-kml-*-assets-*") },
				QDir::Dirs | QDir::Hidden
					| QDir::NoDotAndDotDot)
				.size(),
			2);
	}

	void kmlAndKmzPruneOnlyDestinationScopedStagingFiles()
	{
		Map map;
		georeferenceFixtureMap(map);
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(false);

		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto stagePath = [&directory](
			const QString& destination,
			bool kmz,
			const QString& marker) {
			auto const absolute = QFileInfo(destination)
				.absoluteFilePath()
				.toUtf8();
			auto const digest = QCryptographicHash::hash(
				absolute,
				QCryptographicHash::Sha256).toHex();
			return directory.filePath(
				QStringLiteral(".mapper-%1-%2-stage-%3%4")
					.arg(
						kmz ? QStringLiteral("kmz")
						    : QStringLiteral("kml"),
						QString::fromLatin1(digest),
						marker,
						kmz ? QStringLiteral(".kmz")
						    : QStringLiteral(".part")));
		};
		auto createFile = [](const QString& path) {
			QFile file(path);
			return file.open(QIODevice::WriteOnly)
			       && file.write("stale") == 5;
		};

		auto const kml_path = directory.filePath(
			QStringLiteral("scoped.kml"));
		auto const other_kml_path = directory.filePath(
			QStringLiteral("other.kml"));
		auto const stale_kml = stagePath(
			kml_path, false, QStringLiteral("orphan"));
		auto const other_kml = stagePath(
			other_kml_path, false, QStringLiteral("orphan"));
		auto const staging_directory = stagePath(
			kml_path, false, QStringLiteral("directory"));
		QVERIFY(createFile(stale_kml));
		QVERIFY(createFile(other_kml));
		QVERIFY(QDir().mkpath(staging_directory));

#ifdef Q_OS_UNIX
		auto const symlink_target = directory.filePath(
			QStringLiteral("symlink-target"));
		auto const staging_symlink = stagePath(
			kml_path, false, QStringLiteral("symlink"));
		QVERIFY(createFile(symlink_target));
		QVERIFY(QFile::link(symlink_target, staging_symlink));
		QVERIFY(QFileInfo(staging_symlink).isSymLink());
#endif

		KmzGroundOverlayExport kml_exporter(kml_path, map);
		QVERIFY2(
			kml_exporter.doExport(printer, 256),
			qPrintable(kml_exporter.errorString()));
		QVERIFY(!QFileInfo::exists(stale_kml));
		QVERIFY(QFileInfo::exists(other_kml));
		QVERIFY(QFileInfo::exists(staging_directory));
#ifdef Q_OS_UNIX
		QVERIFY(QFileInfo::exists(staging_symlink));
		QVERIFY(QFileInfo(staging_symlink).isSymLink());
#endif

		auto const kmz_path = directory.filePath(
			QStringLiteral("scoped.kmz"));
		auto const other_kmz_path = directory.filePath(
			QStringLiteral("other.kmz"));
		auto const stale_kmz = stagePath(
			kmz_path, true, QStringLiteral("orphan"));
		auto const other_kmz = stagePath(
			other_kmz_path, true, QStringLiteral("orphan"));
		QVERIFY(createFile(stale_kmz));
		QVERIFY(createFile(other_kmz));
		KmzGroundOverlayExport kmz_exporter(kmz_path, map);
		QVERIFY2(
			kmz_exporter.doExport(printer, 256),
			qPrintable(kmz_exporter.errorString()));
		QVERIFY(!QFileInfo::exists(stale_kmz));
		QVERIFY(QFileInfo::exists(other_kmz));
	}

	void kmzCancellationIsReportedWithoutOutput()
	{
		Map map;
		georeferenceFixtureMap(map);
		TestMapPrinter printer(map, nullptr);
		printer.configurePrecisionFixture();
		printer.setPrintTemplates(false);

		QTemporaryDir directory;
		QVERIFY(directory.isValid());
		auto const path = directory.filePath(
			QStringLiteral("cancelled.kmz"));
		QProgressDialog progress;
		progress.setAutoReset(false);
		progress.cancel();
		QVERIFY(progress.wasCanceled());

		KmzGroundOverlayExport exporter(path, map);
		exporter.setProgressObserver(&progress);
		QVERIFY(!exporter.doExport(printer, 256));
		QVERIFY(exporter.wasCanceled());
		QVERIFY(exporter.errorString().contains(
			QStringLiteral("Canceled"), Qt::CaseInsensitive));
		QVERIFY(!QFile::exists(path));
	}
#endif

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
