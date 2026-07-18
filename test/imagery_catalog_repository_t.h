/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_IMAGERY_CATALOG_REPOSITORY_T_H
#define OPENORIENTEERING_IMAGERY_CATALOG_REPOSITORY_T_H

#include <QObject>

class ImageryCatalogRepositoryTest : public QObject
{
Q_OBJECT

private slots:
	void importsPublishesAndRemovesStableSources();
	void fetchesConditionallyAndMarksChecks();
	void rejectsMismatchedCatalogUpdates();
	void offersPrivateNetworkApprovalAndRetries();
	void offersApprovalForExactPrivateRedirectTarget();
	void requiresHttpConsentAndCancels();
};

#endif
