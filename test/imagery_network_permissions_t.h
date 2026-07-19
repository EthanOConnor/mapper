/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_IMAGERY_NETWORK_PERMISSIONS_T_H
#define OPENORIENTEERING_IMAGERY_NETWORK_PERMISSIONS_T_H

#include <QObject>

class ImageryNetworkPermissionsTest : public QObject
{
Q_OBJECT

private slots:
	void tracksBlockedOriginsAndPersistsExplicitDecisions();
	void persistsIpv6OriginsExactly();
};

#endif
