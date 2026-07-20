/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_ONLINE_IMAGERY_DIALOG_T_H
#define OPENORIENTEERING_ONLINE_IMAGERY_DIALOG_T_H

#include <QObject>

class OnlineImageryDialogTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase();
	void resolvesDirectManualSource();
	void exposesAdvancedTmsAndTileSize();
	void selectsCatalogSourceByStableHandle();
	void switchingToCatalogCancelsStaleDiscovery();
	void arcGisDiscoveryUsesConfiguredReferer();
};

#endif
