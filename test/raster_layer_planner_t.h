/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_RASTER_LAYER_PLANNER_T_H
#define OPENORIENTEERING_RASTER_LAYER_PLANNER_T_H

#include <QObject>

class RasterLayerPlannerTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase();
	void preservesLayerOrderAndRetainedScenes();
	void boundsImageAdmissionAndPreservesVelloIdentity();
	void marksFallbackLayersIncomplete();
	void preservesTransparentGuttersWithoutTileSeams();
};

#endif
