/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_TEMPLATE_LAYER_PLANNER_T_H
#define OPENORIENTEERING_TEMPLATE_LAYER_PLANNER_T_H

#include <QObject>

class TemplateLayerPlannerTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase();
	void preservesLayerOrderAndRetainedScenes();
	void recordsVectorMapAndTrackTemplates();
	void boundsImageAdmissionAndPreservesVelloIdentity();
	void sharesImageAdmissionAcrossVisibleRasterLayers();
	void retainsRasterMemoryLeaseWithImmutableScene();
	void leasesTransparentMosaicPeakAndRetainedMemory();
	void marksFallbackLayersIncomplete();
	void rebuildsRasterSceneForCurrentZoomGeometry();
	void reusesResidentPixelsWhileRebuildingProvisionalGeometry();
	void drawsProvisionalDirectTilesBeforeExactTiles();
	void respectsExplicitImageToMapTransform();
	void preservesOpaqueGuttersWithoutTileSeams();
	void preservesTransparentGuttersWithoutTileSeams();
};

#endif
