/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_IMAGERY_CATALOG_STORE_T_H
#define OPENORIENTEERING_IMAGERY_CATALOG_STORE_T_H

#include <QObject>

class ImageryCatalogStoreTest : public QObject
{
Q_OBJECT

private slots:
	void installsAndLoadsAtomicSnapshot();
	void analyzesUpdatesAndDuplicates();
	void enforcesRevisionPolicy();
	void preservesPreviousSnapshotAndMarksChecks();
	void preservesRemoteProvenanceAndSanitizesValidators();
	void recoversPreviousSnapshot();
	void isolatesCorruptEntries();
	void loadsAndMigratesLegacyLayout();
	void rejectsUnsafeRootAndRemovesOnlyCatalogEntry();
};

#endif
