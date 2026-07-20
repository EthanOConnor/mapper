/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_IMAGERY_SOURCE_MODEL_T_H
#define OPENORIENTEERING_IMAGERY_SOURCE_MODEL_T_H

#include <QObject>

class ImagerySourceModelTest : public QObject
{
Q_OBJECT

private slots:
	void publishesHierarchyAndRecursiveSearch();
	void catalogOriginTooltipRedactsSecrets();
	void catalogTooltipsEscapeRichText();
	void stableHandlesDetectCatalogUpdates();
	void invalidSourcesUseStableIndexIdentity();
};

#endif
