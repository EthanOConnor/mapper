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


#include <Qt>
#include <QtGlobal>
#include <QtTest>
#include <QApplication>
#include <QColor>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QObject>
#include <QPalette>
#include <QSize>
#include <QString>
#include <QStyle>

#include "gui/action_icon.h"
#include "gui/widgets/mapper_proxystyle.h"

using namespace OpenOrienteering;



/**
 * @test Tests style customizations.
 */
class StyleTest : public QObject
{
Q_OBJECT
private slots:
	void scalableActionIconTest();
	void allActionIconsTest();
	void paletteActionIconTest();
	void paletteLifecycleTest();
	void standardIconTest();
};

namespace {

class ApplicationPaletteGuard
{
public:
	ApplicationPaletteGuard() : original{QApplication::palette()} {}
	~ApplicationPaletteGuard() { QApplication::setPalette(original); }

private:
	QPalette original;
};

QPalette sentinelPalette(const QColor& surface, const QColor& text)
{
	auto palette = QApplication::palette();
	for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
	{
		palette.setColor(group, QPalette::Window, surface);
		palette.setColor(group, QPalette::WindowText, text);
		palette.setColor(group, QPalette::Base, surface);
		palette.setColor(group, QPalette::Text, text);
		palette.setColor(group, QPalette::Button, surface);
		palette.setColor(group, QPalette::ButtonText, text);
	}
	return palette;
}

QColor firstOpaquePixel(const QPixmap& pixmap)
{
	const auto image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
	for (auto y = 0; y < image.height(); ++y)
	{
		const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
		for (auto x = 0; x < image.width(); ++x)
		{
			if (qAlpha(row[x]) > 240)
			{
				auto color = QColor::fromRgba(row[x]);
				color.setAlpha(255);
				return color;
			}
		}
	}
	return {};
}

}  // namespace

void StyleTest::scalableActionIconTest()
{
	Q_INIT_RESOURCE(resources);
	const auto icon = ActionIcon::fromName(u"delete");
	QVERIFY(!icon.isNull());
	for (auto const size : { QSize{32, 32}, QSize{96, 96}, QSize{256, 256} })
	{
		QCOMPARE(icon.actualSize(size), size);
		QCOMPARE(icon.pixmap(size).size(), size);
	}
	auto const pathological = QSize{1000, 1000};
	QCOMPARE(icon.actualSize(pathological), QSize(256, 256));
	QCOMPARE(icon.pixmap(pathological).size(), QSize(256, 256));
}

void StyleTest::allActionIconsTest()
{
	Q_INIT_RESOURCE(resources);
	const auto files = QDir{QStringLiteral(":/icons")}.entryList(
	  {QStringLiteral("*.svg")}, QDir::Files, QDir::Name);
	QCOMPARE(files.size(), 101);

	for (const auto& file : files)
	{
		const auto name = QFileInfo{file}.completeBaseName();
		const auto icon = ActionIcon::fromName(name);
		QVERIFY2(!icon.isNull(), qPrintable(name));
		QCOMPARE(icon.actualSize(QSize{32, 32}), QSize(32, 32));

		const auto pixmap = icon.pixmap(QSize{32, 32}, 2.0);
		QCOMPARE(pixmap.size(), QSize(64, 64));
		QCOMPARE(pixmap.devicePixelRatio(), 2.0);
		const auto image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
		bool has_visible_pixel = false;
		for (auto y = 0; y < image.height() && !has_visible_pixel; ++y)
		{
			const auto* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
			for (auto x = 0; x < image.width(); ++x)
				has_visible_pixel |= qAlpha(row[x]) != 0;
		}
		QVERIFY2(has_visible_pixel, qPrintable(name));
	}
}

void StyleTest::paletteActionIconTest()
{
	Q_INIT_RESOURCE(resources);
	ApplicationPaletteGuard guard;
	auto dark = sentinelPalette(Qt::black, Qt::white);
	dark.setColor(QPalette::Disabled, QPalette::ButtonText, Qt::gray);
	QApplication::setPalette(dark);

	const auto close = ActionIcon::fromName(u"close");
	const auto semantic = ActionIcon::fromName(u"control");
	const auto dark_close = close.pixmap(QSize{32, 32});
	const auto dark_semantic = semantic.pixmap(QSize{32, 32});
	QCOMPARE(firstOpaquePixel(dark_close), QColor{Qt::white});
	QCOMPARE(firstOpaquePixel(close.pixmap(QSize{32, 32}, QIcon::Disabled)),
	         QColor{Qt::gray});

	QApplication::setPalette(sentinelPalette(Qt::white, Qt::black));
	QCOMPARE(firstOpaquePixel(close.pixmap(QSize{32, 32})), QColor{Qt::black});
	QVERIFY(dark_close.toImage() != close.pixmap(QSize{32, 32}).toImage());
	QCOMPARE(dark_semantic.toImage(), semantic.pixmap(QSize{32, 32}).toImage());
}

void StyleTest::paletteLifecycleTest()
{
	ApplicationPaletteGuard guard;
	const auto construction = sentinelPalette(
	  QColor{0x12, 0x34, 0x56}, QColor{0xed, 0xcb, 0xa9});
	const auto installation = sentinelPalette(
	  QColor{0x24, 0x35, 0x46}, QColor{0xdb, 0xca, 0xb9});
	QApplication::setPalette(construction);
	auto* style = new MapperProxyStyle();
	QApplication::setPalette(installation);
	QApplication::setStyle(style);
	QCOMPARE(QApplication::palette().color(QPalette::Window),
	         installation.color(QPalette::Window));
	QCOMPARE(QApplication::palette().color(QPalette::WindowText),
	         installation.color(QPalette::WindowText));
}

/**
 * Tests standard icon behaviours of MapperProxyStyle
 */
void StyleTest::standardIconTest()
{
	auto const standard_icon = QStyle::SP_TitleBarMenuButton;
	auto const large = QSize(1000, 1000);
	auto const icon = MapperProxyStyle().standardIcon(standard_icon, nullptr, nullptr);
	auto const size = icon.actualSize(large);
	QVERIFY(!icon.isNull());
	QCOMPARE(size, QSize(256, 256));
	QCOMPARE(icon.pixmap(large).size(), QSize(256, 256));
}


/*
 * We select a non-standard QPA because we don't need a real GUI window.
 */
namespace  {
	[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");  // clazy:exclude=non-pod-global-static
}


QTEST_MAIN(StyleTest)

#include "style_t.moc"  // IWYU pragma: keep
