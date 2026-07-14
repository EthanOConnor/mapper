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
#include <QIcon>
#include <QObject>
#include <QSize>
#include <QString>
#include <QStyle>

#include "gui/widgets/mapper_proxystyle.h"

using namespace OpenOrienteering;



/**
 * @test Tests style customizations.
 */
class StyleTest : public QObject
{
Q_OBJECT
private slots:
	void resourceIconTest();
	void standardIconTest();
};

void StyleTest::resourceIconTest()
{
	Q_INIT_RESOURCE(resources);
	const QIcon icon(QStringLiteral(":/images/help.png"));
	QVERIFY(!icon.isNull());
	QVERIFY(!icon.pixmap(QSize(32, 32)).isNull());
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
	QVERIFY(size.isValid());
	QVERIFY(size.width() <= large.width());
	QVERIFY(size.height() <= large.height());
}


/*
 * We select a non-standard QPA because we don't need a real GUI window.
 */
namespace  {
	[[maybe_unused]] const auto qpa_selected = qputenv("QT_QPA_PLATFORM", "offscreen");  // clazy:exclude=non-pod-global-static
}


QTEST_MAIN(StyleTest)

#include "style_t.moc"  // IWYU pragma: keep
