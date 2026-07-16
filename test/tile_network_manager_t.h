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
	void handlesRedirectsEmptyTilesAndBodyLimits();
	void retriesTransientFailures();
	void cancelsClientGenerations();
	void enforcesFairnessAndQueueBounds();
	void servesFreshDiskCacheOffline();
	void timesOutWithoutBlockingTheCaller();
};

#endif
