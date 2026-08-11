/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 */

#include "gui/action_icon.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <QApplication>
#include <QColor>
#include <QIconEngine>
#include <QImage>
#include <QPalette>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QtGlobal>

namespace OpenOrienteering::ActionIcon {

namespace {

enum class TintPolicy
{
	Never,
	Always,
	PaletteComponents,
};

constexpr auto neutral_channel_tolerance = 12;

QColor paletteColor(QIcon::Mode mode)
{
	auto const palette = QApplication::palette();
	if (mode == QIcon::Disabled)
		return palette.color(QPalette::Disabled, QPalette::ButtonText);
	if (mode == QIcon::Selected)
		return palette.color(QPalette::Active, QPalette::HighlightedText);
	return palette.color(QPalette::Active, QPalette::ButtonText);
}

QColor paletteAccentColor(QIcon::Mode mode)
{
	auto const palette = QApplication::palette();
	if (mode == QIcon::Disabled)
		return palette.color(QPalette::Disabled, QPalette::ButtonText);
	if (mode == QIcon::Selected)
		return palette.color(QPalette::Active, QPalette::HighlightedText);
	return palette.color(QPalette::Active, QPalette::Accent);
}

void tint(QPixmap& pixmap, const QColor& color)
{
	QPainter painter{&pixmap};
	painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
	painter.fillRect(pixmap.rect(), color);
}

void tintPaletteComponents(QPixmap& pixmap, QIcon::Mode mode)
{
	auto image = pixmap.toImage().convertToFormat(QImage::Format_ARGB32);
	auto const neutral = paletteColor(mode);
	auto const accent = paletteAccentColor(mode);
	for (auto y = 0; y < image.height(); ++y)
	{
		auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
		for (auto x = 0; x < image.width(); ++x)
		{
			auto const pixel = row[x];
			if (qAlpha(pixel) == 0)
				continue;
			auto const darkest = std::min({qRed(pixel), qGreen(pixel), qBlue(pixel)});
			auto const lightest = std::max({qRed(pixel), qGreen(pixel), qBlue(pixel)});
			auto const color = lightest - darkest <= neutral_channel_tolerance
			                   ? neutral
			                   : accent;
			row[x] = qRgba(color.red(), color.green(), color.blue(), qAlpha(pixel));
		}
	}
	auto const device_pixel_ratio = pixmap.devicePixelRatio();
	pixmap = QPixmap::fromImage(std::move(image));
	pixmap.setDevicePixelRatio(device_pixel_ratio);
}

QSize boundedSize(QSize requested) noexcept
{
	if (requested.isEmpty())
		return requested;
	if (requested.width() > maxRasterExtent || requested.height() > maxRasterExtent)
		requested.scale(maxRasterExtent, maxRasterExtent, Qt::KeepAspectRatio);
	return requested;
}

QSize scaledSourceSize(const QIcon& source, QSize requested,
	                    QIcon::Mode mode, QIcon::State state)
{
	requested = boundedSize(requested);
	auto source_size = source.actualSize(requested, mode, state);
	if (source_size.isEmpty())
		return {};
	source_size.scale(requested, Qt::KeepAspectRatio);
	return source_size;
}

class BoundedIconEngine final : public QIconEngine
{
public:
	explicit BoundedIconEngine(QIcon source, TintPolicy tint_policy = TintPolicy::Never)
	 : source_{std::move(source)},
	   tint_policy_{tint_policy}
	{}

	BoundedIconEngine* clone() const override
	{
		return new BoundedIconEngine(*this);
	}

	QString key() const override
	{
		return tint_policy_ == TintPolicy::Never
		         ? QStringLiteral("MapperBoundedIcon")
		         : QStringLiteral("MapperPaletteActionIcon");
	}

	bool isNull() override
	{
		return source_.isNull();
	}

	QSize actualSize(const QSize& size, QIcon::Mode mode, QIcon::State state) override
	{
		return scaledSourceSize(source_, size, mode, state);
	}

	QPixmap pixmap(const QSize& size, QIcon::Mode mode, QIcon::State state) override
	{
		auto const target_size = scaledSourceSize(source_, size, mode, state);
		if (target_size.isEmpty())
			return {};
		auto const palette_tinted = isPaletteTinted();
		auto pixmap = source_.pixmap(
			target_size, palette_tinted ? QIcon::Normal : mode, state);
		if (!pixmap.isNull() && pixmap.size() != target_size)
			pixmap = pixmap.scaled(target_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
		if (palette_tinted && !pixmap.isNull())
		{
			if (tint_policy_ == TintPolicy::PaletteComponents)
				tintPaletteComponents(pixmap, mode);
			else
				tint(pixmap, paletteColor(mode));
		}
		return pixmap;
	}

	QPixmap scaledPixmap(const QSize& size, QIcon::Mode mode,
	                    QIcon::State state, qreal scale) override
	{
		auto physical_size = QSize{
			int(std::ceil(size.width() * scale)),
			int(std::ceil(size.height() * scale)),
		};
		auto pixmap = this->pixmap(physical_size, mode, state);
		if (!pixmap.isNull())
			pixmap.setDevicePixelRatio(scale);
		return pixmap;
	}

	void paint(QPainter* painter, const QRect& rect,
	           QIcon::Mode mode, QIcon::State state) override
	{
		auto const pixmap = this->scaledPixmap(
			rect.size(), mode, state, painter->device()->devicePixelRatioF());
		if (pixmap.isNull())
			return;
		auto target = QRectF{QPointF{}, pixmap.deviceIndependentSize()};
		target.moveCenter(QRectF{rect}.center());
		painter->drawPixmap(target.topLeft(), pixmap);
	}

private:
	bool isPaletteTinted() const
	{
		return tint_policy_ != TintPolicy::Never;
	}

	QIcon source_;
	TintPolicy tint_policy_ = TintPolicy::Never;
};

}  // namespace

QIcon fromName(QStringView name)
{
	auto source = QIcon{QStringLiteral(":/icons/") + name + QStringLiteral(".svg")};
	Q_ASSERT_X(!source.isNull(), "ActionIcon::fromName", "missing scalable action icon");
	auto tint_policy = TintPolicy::Never;
	if (name.startsWith(QStringView{u"text-align-"}))
		tint_policy = TintPolicy::PaletteComponents;
	else if (name == QStringView{u"close"}
	         || name == QStringView{u"arrow-thin-upleft"}
	         || name == QStringView{u"arrow-thin-downright"}
	         || name == QStringView{u"grid"}
	         || name == QStringView{u"map-information"})
		tint_policy = TintPolicy::Always;
	return QIcon{new BoundedIconEngine(std::move(source), tint_policy)};
}

QIcon bounded(QIcon source)
{
	if (source.isNull())
		return {};
	return QIcon{new BoundedIconEngine(std::move(source))};
}

}  // namespace OpenOrienteering::ActionIcon
