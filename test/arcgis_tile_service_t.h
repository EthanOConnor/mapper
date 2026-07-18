/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_ARCGIS_TILE_SERVICE_T_H
#define OPENORIENTEERING_ARCGIS_TILE_SERVICE_T_H

#include <QObject>

class ArcGisTileServiceTest : public QObject
{
Q_OBJECT

private slots:
	void resolvesCanonicalWebMercatorAndPreservesCredentials();
	void resolvesCustomGridWithOriginBasedLimits();
	void preservesRectangularTilesAndLodRange();
	void rejectsUnsupportedImageServerCacheType();
	void returnsUnsupportedForValidUnusableServices_data();
	void returnsUnsupportedForValidUnusableServices();
	void rejectsMalformedMetadata_data();
	void rejectsMalformedMetadata();
	void rejectsOversizedMetadata();
};

#endif
