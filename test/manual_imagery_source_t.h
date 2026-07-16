/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MANUAL_IMAGERY_SOURCE_T_H
#define OPENORIENTEERING_MANUAL_IMAGERY_SOURCE_T_H

#include <QObject>

class ManualImagerySourceTest : public QObject
{
Q_OBJECT

private slots:
	void acceptsAliasesAndBuildsDirectDefaults();
	void appliesTmsAnd512PixelAdvancedSettings();
	void classifiesDiscoveryAndUnsupportedServices();
	void tiledPlaceholdersOverrideServicePathHeuristics();
	void rejectsInvalidInputs_data();
	void rejectsInvalidInputs();
	void exposesSecretWarningsAndForbidsRecents();
};

#endif
