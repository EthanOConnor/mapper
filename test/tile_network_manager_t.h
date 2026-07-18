/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_TILE_NETWORK_MANAGER_T_H
#define OPENORIENTEERING_TILE_NETWORK_MANAGER_T_H

#include <QObject>

class TileNetworkManagerTest : public QObject
{
Q_OBJECT

private slots:
	void rejectsUnsafeUrls();
	void canonicalizesIpv6OriginsExactly();
	void approvesPrivateOriginsExplicitly();
	void injectsBearerOnlyForExactOriginAndBypassesSharedCache();
	void credentialChangesCancelRequestsAndPartitionNegativeCache();
	void handlesRedirectsEmptyTilesAndBodyLimits();
	void reportsExactPrivateRedirectTarget();
	void boundsAndExpiresNegativeCache();
	void negativeCacheRespectsRepresentationPolicy();
	void digestsLongNegativeCacheRepresentationsWithoutCollision();
	void fetchesCatalogsConditionally();
	void retriesTransientFailures();
	void cancelsClientGenerations();
	void enteringOfflineModeAbortsActiveRequests();
	void offlineTransitionRejectsQueuedOnlineSuccess();
	void revokingPrivateOriginRejectsActiveAndQueuedRequests();
	void revocationRejectsQueuedPrivateSuccess();
	void enforcesFairnessAndQueueBounds();
	void boundsOutstandingResultDelivery();
	void servesFreshDiskCacheOffline();
	void servesHostnameDiskCacheOfflineWithoutDns();
	void timesOutWithoutBlockingTheCaller();
};

#endif
