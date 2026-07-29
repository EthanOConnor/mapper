/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_SKETCH_LAYER_T_H
#define OPENORIENTEERING_SKETCH_LAYER_T_H

#include <QObject>

class SketchLayerTest : public QObject
{
	Q_OBJECT

private slots:
	void createsOneLayerAndLazyStyles();
	void ownedLayerMetadataRoundTrips();
	void readOnlySidecarRoundTripsVectorLayers();
	void roundTripsAsNativeVectorData();
	void encodesAsConnectedEditingEntities();
	void simplifiesDenseInputWithoutRasterStorage();
	void erasesContinuousCrossingsBetweenSparseEvents();
	void rendersOnlyWithHelperLayersEnabled();
};

#endif
