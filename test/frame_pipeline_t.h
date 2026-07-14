/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_FRAME_PIPELINE_T_H
#define OPENORIENTEERING_FRAME_PIPELINE_T_H

#include <QObject>

class FramePipelineTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase();
	void packetIsCompleteAndMonotonic();
	void qpainterConsumesTheFrameContract();
	void overprintingUsesIsolatedPasses();
	void viewportOverlayUsesTheSharedFrameContract();
	void overlayPatternsAndImagesStayRetained();
	void mapWidgetUsesTheFrameContract();
	void nativeSurfacePublishesOrderedLifecycle();
};

#endif
