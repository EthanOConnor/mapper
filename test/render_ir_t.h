/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_RENDER_IR_T_H
#define OPENORIENTEERING_RENDER_IR_T_H

#include <QObject>

class RenderIrTest : public QObject
{
Q_OBJECT

private slots:
	void initTestCase();
	void immutableSnapshotSurvivesEdit();
	void curvedLineKeepsBothBorders();
	void cosmeticAndMinimumStrokesRemainVisible();
	void referenceRendererInterpretsIr();
	void antialiasPolicyPreservesCallerIntent();
};

#endif
