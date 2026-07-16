/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_IMAGERY_CORE_T_H
#define OPENORIENTEERING_IMAGERY_CORE_T_H

#include <QObject>

class ImageryCoreTest : public QObject
{
Q_OBJECT

private slots:
	void webMercatorQuadIsDyadic();
	void expandsXyzAndTmsRows();
	void rejectsUnsafeUrlTemplates();
	void validatesTileLimits();
	void requestPolicyHasExplicitDefaults();
	void snapshotRoundTripsDeterministically();
	void snapshotRejectsUnsupportedRegistrations();
};

#endif
