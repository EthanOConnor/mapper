/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    any later version.
 */

#include "imagery/imagery_network_permissions.h"

#include <algorithm>

#include <QApplicationStatic>
#include <QCoreApplication>
#include <QSettings>
#include <QThread>

#include "imagery/tile_network_manager.h"

namespace OpenOrienteering::imagery {

namespace {

constexpr auto settings_key =
	"onlineImagery/approvedPrivateOrigins";
// Blocked requests are untrusted input. Keep review candidates memory-only and
// bounded; only an explicit approval is persisted.
constexpr qsizetype max_pending_origins = 128;

}  // namespace


ImageryNetworkPermissions::ImageryNetworkPermissions(
	TileNetworkManager& network,
	QObject* parent)
 : QObject(parent)
 , network_(network)
{
	auto const stored_origins = QSettings {}
		.value(QString::fromLatin1(settings_key))
		.toStringList();
	approved_origins_ = stored_origins;
	approved_origins_.removeDuplicates();
	std::sort(
		approved_origins_.begin(),
		approved_origins_.end());
	connect(
		&network_,
		&TileNetworkManager::privateOriginApprovalChanged,
		this,
		[this](const QString& origin, bool approved) {
			auto changed = false;
			auto pending_changed = false;
			if (approved)
			{
				if (!approved_origins_.contains(origin))
				{
					approved_origins_.push_back(origin);
					std::sort(
						approved_origins_.begin(),
						approved_origins_.end());
					changed = true;
				}
				pending_changed =
					pending_origins_.removeAll(origin) > 0;
			}
			else
			{
				changed =
					approved_origins_.removeAll(origin) > 0;
			}
			if (changed)
			{
				save();
				emit approvalsChanged();
			}
			if (pending_changed)
				emit pendingOriginsChanged();
		});
	connect(
		&network_,
		&TileNetworkManager::finished,
		this,
		[this](
			TileNetworkManager::Token,
			const TileNetworkResult& result) {
			if (!result.private_network_rejected
			    || result.private_network_permission_revoked
			    || result.private_network_rejected_url.isEmpty())
				return;
			auto const origin =
				TileNetworkManager::canonicalOrigin(
					result.private_network_rejected_url);
			auto const url = QUrl(origin);
			auto const scheme = url.scheme().toLower();
			if (!url.isValid() || url.host().isEmpty()
			    || !url.userInfo().isEmpty()
			    || (scheme != QLatin1String("http")
			        && scheme != QLatin1String("https"))
			    || network_.isPrivateOriginApproved(url)
			    || pending_origins_.contains(origin))
				return;
			if (pending_origins_.size() >= max_pending_origins)
				pending_origins_.removeFirst();
			pending_origins_.push_back(origin);
			emit pendingOriginsChanged();
		});
	QStringList valid;
	for (auto const& origin : std::as_const(approved_origins_))
	{
		auto const url = QUrl(origin);
		if (network_.approvePrivateOrigin(url))
			valid.push_back(
				TileNetworkManager::canonicalOrigin(url));
	}
	valid.removeDuplicates();
	std::sort(valid.begin(), valid.end());
	approved_origins_ = std::move(valid);
	if (approved_origins_ != stored_origins)
		save();
}


Q_APPLICATION_STATIC(
	ImageryNetworkPermissions,
	application_imagery_network_permissions,
	TileNetworkManager::instance())

ImageryNetworkPermissions&
ImageryNetworkPermissions::instance()
{
	auto* application = QCoreApplication::instance();
	Q_ASSERT(application);
	Q_ASSERT(QThread::currentThread() == application->thread());
	return *application_imagery_network_permissions;
}


QStringList ImageryNetworkPermissions::approvedOrigins() const
{
	return approved_origins_;
}


QStringList ImageryNetworkPermissions::pendingOrigins() const
{
	return pending_origins_;
}


bool ImageryNetworkPermissions::isApproved(
	const QUrl& url) const
{
	return network_.isPrivateOriginApproved(url);
}


bool ImageryNetworkPermissions::approve(
	const QUrl& url)
{
	if (!network_.approvePrivateOrigin(url))
		return false;
	auto const origin =
		TileNetworkManager::canonicalOrigin(url);
	if (!approved_origins_.contains(origin))
	{
		approved_origins_.push_back(origin);
		std::sort(
			approved_origins_.begin(),
			approved_origins_.end());
		save();
		emit approvalsChanged();
	}
	if (pending_origins_.removeAll(origin) > 0)
		emit pendingOriginsChanged();
	return true;
}


bool ImageryNetworkPermissions::revoke(
	const QUrl& url)
{
	auto const origin =
		TileNetworkManager::canonicalOrigin(url);
	auto const removed =
		approved_origins_.removeAll(origin) > 0;
	network_.revokePrivateOrigin(url);
	if (removed)
	{
		save();
		emit approvalsChanged();
	}
	return removed;
}


bool ImageryNetworkPermissions::dismissPending(
	const QUrl& url)
{
	auto const origin =
		TileNetworkManager::canonicalOrigin(url);
	if (pending_origins_.removeAll(origin) == 0)
		return false;
	emit pendingOriginsChanged();
	return true;
}


void ImageryNetworkPermissions::save()
{
	QSettings {}.setValue(
		QString::fromLatin1(settings_key),
		approved_origins_);
}

}  // namespace OpenOrienteering::imagery
