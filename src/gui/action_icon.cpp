/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 */

#include "gui/action_icon.h"

#include <cmath>
#include <utility>

#include <QIconEngine>
#include <QPainter>
#include <QPixmap>
#include <QSize>
#include <QString>
#include <QtGlobal>

namespace OpenOrienteering::ActionIcon {

namespace {

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
	explicit BoundedIconEngine(QIcon source)
	 : source_{std::move(source)}
	{}

	BoundedIconEngine* clone() const override
	{
		return new BoundedIconEngine(source_);
	}

	QString key() const override
	{
		return QStringLiteral("MapperBoundedIcon");
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
		auto pixmap = source_.pixmap(target_size, mode, state);
		if (!pixmap.isNull() && pixmap.size() != target_size)
			pixmap = pixmap.scaled(target_size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
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
	QIcon source_;
};

}  // namespace

QIcon fromName(QStringView name)
{
	auto source = QIcon{QStringLiteral(":/icons/") + name + QStringLiteral(".svg")};
	Q_ASSERT_X(!source.isNull(), "ActionIcon::fromName", "missing scalable action icon");
	return bounded(std::move(source));
}

QIcon bounded(QIcon source)
{
	if (source.isNull())
		return {};
	return QIcon{new BoundedIconEngine(std::move(source))};
}

}  // namespace OpenOrienteering::ActionIcon
