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
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QObject>
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
	void standardIconTest();
};

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
	QCOMPARE(files.size(), 97);

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
