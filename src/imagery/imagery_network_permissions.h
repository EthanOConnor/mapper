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

#ifndef OPENORIENTEERING_IMAGERY_NETWORK_PERMISSIONS_H
#define OPENORIENTEERING_IMAGERY_NETWORK_PERMISSIONS_H

#include <QObject>
#include <QStringList>
#include <QUrl>

namespace OpenOrienteering::imagery {

class TileNetworkManager;

/**
 * Installation-local approvals for private-network imagery origins.
 *
 * Catalogs and map documents cannot mutate this policy. Only an explicit UI
 * action calls approve(), after which the canonical origin is persisted in
 * local application settings and applied to the shared network manager.
 */
class ImageryNetworkPermissions final : public QObject
{
Q_OBJECT

public:
	explicit ImageryNetworkPermissions(
		TileNetworkManager& network,
		QObject* parent = nullptr);

	static ImageryNetworkPermissions& instance();

	QStringList approvedOrigins() const;
	QStringList pendingOrigins() const;
	bool isApproved(const QUrl& url) const;
	bool approve(const QUrl& url);
	bool revoke(const QUrl& url);
	bool dismissPending(const QUrl& url);

signals:
	void approvalsChanged();
	void pendingOriginsChanged();

private:
	void save();

	TileNetworkManager& network_;
	QStringList approved_origins_;
	QStringList pending_origins_;
};

}  // namespace OpenOrienteering::imagery

#endif
