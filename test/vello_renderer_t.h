/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_VELLO_RENDERER_T_H
#define OPENORIENTEERING_VELLO_RENDERER_T_H

#include <QObject>

class VelloRendererTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase();
	void typedEncoderRetainsImmutableScenes();
	void missingNativeTargetIsRetriable();
	void offscreenGpuMatchesReference();
	void affineImageSourceCropMatchesReference();
	void miterLimitOneMatchesReference();
	void selectionHandleGlyphsSurviveHighDpi();
	void mapCorpusMatchesReference();
	void nativeSurfaceLifecyclePresentsCurrentFrame();
};

#endif
