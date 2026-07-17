/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_QT_CANVAS_RENDERER_T_H
#define OPENORIENTEERING_QT_CANVAS_RENDERER_T_H

#include <QObject>

class QtCanvasRendererTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase();
	void scenePublishesDirectCanvasGeometry();
	void multiplySelectsSoftwareFallback();
	void widgetPresentsCurrentFrame();
	void transformedStrokeWidthIsNotDoubleScaled();
	void transformedFillAntialiasIsDeviceSized();
	void linePatternDoesNotLeakStencilClip();
	void sceneClipPopRestoresStencilState();
	void retainedImagesStayResident();
};

#endif
