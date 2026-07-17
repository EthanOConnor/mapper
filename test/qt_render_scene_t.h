/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_QT_RENDER_SCENE_T_H
#define OPENORIENTEERING_QT_RENDER_SCENE_T_H

#include <QObject>

class QtRenderSceneTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase();
	void directPathBuilderProducesBothQtPaths();
	void immutableSnapshotSurvivesEdit();
	void curvedLineKeepsBothBorders();
	void referenceRendererInterpretsScene();
	void antialiasPolicyPreservesCallerIntent();
};

#endif
