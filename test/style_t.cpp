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


#include <cmath>

#include <Qt>
#include <QtGlobal>
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QIcon>
#include <QImage>
#include <QLabel>
#include <QObject>
#include <QPalette>
#include <QPainter>
#include <QSize>
#include <QString>
#include <QStyle>
#include <QStyleOption>

#include "gui/action_icon.h"
#include "gui/widgets/mapper_proxystyle.h"
#include "gui/widgets/toast.h"
#include "settings.h"

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
	void touchModePaletteLifecycleTest();
	void checkedToolCueContrastTest();
	void toastLinkPaletteTest();
	void standardIconTest();
};


namespace {

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

void compareSentinelPalette(const QPalette& expected)
{
	const auto actual = QApplication::palette();
	for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
	{
		QCOMPARE(actual.color(group, QPalette::Window),
		         expected.color(group, QPalette::Window));
		QCOMPARE(actual.color(group, QPalette::WindowText),
		         expected.color(group, QPalette::WindowText));
		QCOMPARE(actual.color(group, QPalette::Base),
		         expected.color(group, QPalette::Base));
		QCOMPARE(actual.color(group, QPalette::Text),
		         expected.color(group, QPalette::Text));
		QCOMPARE(actual.color(group, QPalette::Button),
		         expected.color(group, QPalette::Button));
		QCOMPARE(actual.color(group, QPalette::ButtonText),
		         expected.color(group, QPalette::ButtonText));
	}
}

class ApplicationPaletteGuard
{
public:
	ApplicationPaletteGuard()
	 : original{QApplication::palette()}
	{}

	~ApplicationPaletteGuard()
	{
		QApplication::setPalette(original);
	}

private:
	QPalette original;
};

class TouchModeGuard
{
public:
	TouchModeGuard()
	 : original{Settings::getInstance().touchModeEnabled()}
	{}

	~TouchModeGuard()
	{
		Settings::getInstance().setTouchModeEnabled(original);
	}

	bool originalValue() const noexcept
	{
		return original;
	}

private:
	bool original;
};

qreal linearColorChannel(int channel)
{
	auto const value = channel / 255.0;
	return value <= 0.04045
	       ? value / 12.92
	       : std::pow((value + 0.055) / 1.055, 2.4);
}

qreal relativeLuminance(const QColor& color)
{
	return 0.2126 * linearColorChannel(color.red())
	       + 0.7152 * linearColorChannel(color.green())
	       + 0.0722 * linearColorChannel(color.blue());
}

qreal contrastRatio(const QColor& first, const QColor& second)
{
	auto const first_luminance = relativeLuminance(first);
	auto const second_luminance = relativeLuminance(second);
	auto const lighter = qMax(first_luminance, second_luminance);
	auto const darker = qMin(first_luminance, second_luminance);
	return (lighter + 0.05) / (darker + 0.05);
}

QColor firstOpaquePixel(const QPixmap& pixmap)
{
	auto const image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
	for (auto y = 0; y < image.height(); ++y)
	{
		auto const* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
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

QColor mostOpaquePixel(const QPixmap& pixmap)
{
	auto const image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
	auto most_opaque = QRgb{};
	for (auto y = 0; y < image.height(); ++y)
	{
		auto const* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
		for (auto x = 0; x < image.width(); ++x)
		{
			if (qAlpha(row[x]) > qAlpha(most_opaque))
				most_opaque = row[x];
		}
	}
	auto color = QColor::fromRgba(most_opaque);
	if (color.alpha() != 0)
		color.setAlpha(255);
	return color;
}

bool containsOpaqueColor(const QPixmap& pixmap, const QColor& expected)
{
	auto const image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
	for (auto y = 0; y < image.height(); ++y)
	{
		auto const* row = reinterpret_cast<const QRgb*>(image.constScanLine(y));
		for (auto x = 0; x < image.width(); ++x)
		{
			if (qAlpha(row[x]) > 240
			    && QColor::fromRgba(row[x]).toRgb() == expected.toRgb())
				return true;
		}
	}
	return false;
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
	dark.setColor(QPalette::Active, QPalette::HighlightedText, Qt::yellow);
	dark.setColor(QPalette::Disabled, QPalette::ButtonText, Qt::gray);
	dark.setColor(QPalette::Active, QPalette::Accent, Qt::cyan);
	QApplication::setPalette(dark);

	auto const close = ActionIcon::fromName(u"close");
	auto const thin_arrow = ActionIcon::fromName(u"arrow-thin-upleft");
	auto const map_information = ActionIcon::fromName(u"map-information");
	auto const semantic_color = ActionIcon::fromName(u"control");
	auto const neutral_multitone = ActionIcon::fromName(u"three-dots");
	auto const text_alignment = ActionIcon::fromName(u"text-align-baseline");
	auto const grid = ActionIcon::fromName(u"grid");
	auto const bounded_native = ActionIcon::bounded(
		QIcon{QStringLiteral(":/icons/close.svg")});
	auto const dark_close = close.pixmap(QSize{32, 32});
	auto const dark_thin_arrow = thin_arrow.pixmap(QSize{32, 32});
	auto const dark_map_information = map_information.pixmap(QSize{32, 32});
	auto const dark_semantic = semantic_color.pixmap(QSize{32, 32});
	auto const dark_neutral_multitone =
		neutral_multitone.pixmap(QSize{32, 32});
	auto const dark_text_alignment = text_alignment.pixmap(QSize{32, 32});
	auto const dark_bounded_native = bounded_native.pixmap(QSize{32, 32});
	QCOMPARE(firstOpaquePixel(dark_close), QColor{Qt::white});
	QCOMPARE(firstOpaquePixel(dark_thin_arrow), QColor{Qt::white});
	QCOMPARE(firstOpaquePixel(dark_map_information), QColor{Qt::white});
	QCOMPARE(firstOpaquePixel(close.pixmap(QSize{32, 32}, QIcon::Disabled)),
	         QColor{Qt::gray});
	QCOMPARE(firstOpaquePixel(close.pixmap(QSize{32, 32}, QIcon::Selected)),
	         QColor{Qt::yellow});
	QCOMPARE(mostOpaquePixel(grid.pixmap(QSize{32, 32})), QColor{Qt::white});
	QVERIFY(containsOpaqueColor(dark_text_alignment, Qt::white));
	QVERIFY(containsOpaqueColor(dark_text_alignment, Qt::cyan));

	auto light = sentinelPalette(Qt::white, Qt::black);
	QApplication::setPalette(light);
	QCOMPARE(firstOpaquePixel(close.pixmap(QSize{32, 32})), QColor{Qt::black});
	QCOMPARE(firstOpaquePixel(thin_arrow.pixmap(QSize{32, 32})),
	         QColor{Qt::black});
	QCOMPARE(firstOpaquePixel(map_information.pixmap(QSize{32, 32})),
	         QColor{Qt::black});
	QVERIFY(dark_close.toImage() != close.pixmap(QSize{32, 32}).toImage());
	QCOMPARE(dark_semantic.toImage(),
	         semantic_color.pixmap(QSize{32, 32}).toImage());
	QCOMPARE(dark_bounded_native.toImage(),
	         bounded_native.pixmap(QSize{32, 32}).toImage());
	QCOMPARE(dark_neutral_multitone.toImage(),
	         neutral_multitone.pixmap(QSize{32, 32}).toImage());
	QVERIFY(firstOpaquePixel(dark_neutral_multitone) != QColor{Qt::white}
	        || dark_neutral_multitone.toImage().pixelColor(16, 16)
	             != QColor{Qt::white});
}


void StyleTest::paletteLifecycleTest()
{
	ApplicationPaletteGuard guard;
	const auto construction_palette =
		sentinelPalette(QColor{0x12, 0x34, 0x56}, QColor{0xED, 0xCB, 0xA9});
	const auto installation_palette =
		sentinelPalette(QColor{0x24, 0x35, 0x46}, QColor{0xDB, 0xCA, 0xB9});
	const auto replacement_palette =
		sentinelPalette(QColor{0x35, 0x46, 0x57}, QColor{0xCA, 0xB9, 0xA8});

	QApplication::setPalette(construction_palette);
	auto* style = new MapperProxyStyle();
	QApplication::setPalette(installation_palette);
	QApplication::setStyle(style);
	compareSentinelPalette(installation_palette);

	// Construct the replacement before the palette changes. Replacing the
	// style must neither restore the old style's palette nor the replacement's
	// construction-time palette.
	auto* replacement = new MapperProxyStyle();
	QApplication::setPalette(replacement_palette);
	QApplication::setStyle(replacement);
	compareSentinelPalette(replacement_palette);
}


void StyleTest::touchModePaletteLifecycleTest()
{
	ApplicationPaletteGuard palette_guard;
	TouchModeGuard touch_mode_guard;
	if (Settings::mobileModeEnforced())
		QSKIP("Touch mode cannot be changed when mobile mode is enforced");

	const auto construction_palette =
		sentinelPalette(QColor{0x46, 0x57, 0x68}, QColor{0xB9, 0xA8, 0x97});
	const auto runtime_palette =
		sentinelPalette(QColor{0x18, 0x29, 0x3A}, QColor{0xE7, 0xD6, 0xC5});

	QApplication::setPalette(construction_palette);
	QApplication::setStyle(new MapperProxyStyle());
	QApplication::setPalette(runtime_palette);
	auto* const previous_style = QApplication::style();
	Settings::getInstance().setTouchModeEnabled(!touch_mode_guard.originalValue());
	QVERIFY(QApplication::style() != previous_style);
	compareSentinelPalette(runtime_palette);

	Settings::getInstance().setTouchModeEnabled(touch_mode_guard.originalValue());
	compareSentinelPalette(runtime_palette);
}


void StyleTest::checkedToolCueContrastTest()
{
	ApplicationPaletteGuard palette_guard;
	TouchModeGuard touch_mode_guard;
	Settings::getInstance().setTouchModeEnabled(true);
	QApplication::setStyle(new MapperProxyStyle());

	for (auto const& background : {QColor{Qt::black}, QColor{Qt::white}})
	{
		QImage image{QSize{40, 40}, QImage::Format_ARGB32_Premultiplied};
		image.fill(background);

		QStyleOption option;
		option.rect = image.rect();
		option.state = QStyle::State_Enabled | QStyle::State_On;
		option.palette.setColor(QPalette::Window, background);
		QPainter painter{&image};
		QApplication::style()->drawPrimitive(QStyle::PE_PanelButtonTool,
		                                     &option,
		                                     &painter);
		painter.end();

		auto const cue = image.pixelColor(image.rect().center());
		auto const contrast = contrastRatio(background, cue);
		QVERIFY2(contrast >= 3.0,
		         qPrintable(QStringLiteral("Checked tool cue contrast is %1 for %2")
		                      .arg(contrast)
		                      .arg(background.name())));
	}
}


void StyleTest::toastLinkPaletteTest()
{
	ApplicationPaletteGuard guard;

	for (const auto& surface : {QColor{Qt::white}, QColor{Qt::black}})
	{
		QApplication::setPalette(sentinelPalette(surface, surface == Qt::white
		                                                   ? QColor{Qt::black}
		                                                   : QColor{Qt::white}));
		Toast toast;
		auto* const label = toast.findChild<QLabel*>();
		QVERIFY(label);

		const auto toast_palette = toast.palette();
		const auto label_palette = label->palette();
		for (auto group : {QPalette::Active, QPalette::Inactive, QPalette::Disabled})
		{
			const auto foreground = toast_palette.color(group, QPalette::WindowText);
			QVERIFY(foreground != toast_palette.color(group, QPalette::Window));
			QCOMPARE(label_palette.color(group, QPalette::Link), foreground);
			QCOMPARE(label_palette.color(group, QPalette::LinkVisited), foreground);
		}
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
