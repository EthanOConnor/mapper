/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_OIC_CATALOG_T_H
#define OPENORIENTEERING_OIC_CATALOG_T_H

#include <QObject>

class OicCatalogTest : public QObject
{
Q_OBJECT

private slots:
	void parsesMinimalCatalog();
	void preservesHistoricalFixturesAndFingerprints();
	void acceptsDocumentedTemplateAliases();
	void preservesMetadataNoticesLicenseAndTranslationProvenance();
	void retainsAffineAsUnsupported();
	void rejectsSingularAffine();
	void retainsNonDyadicAndUnknownGridsAsUnsupported();
	void retainsExternalMatrixLimitsUntilResolution();
	void gatesUnsupportedAxisOrder();
	void recognizesOnlyExactWebMercatorQuadUris();
	void validatesFrameIdsEvenWhenNotRetained();
	void rejectsNonfiniteDerivedMatrixExtents();
	void rejectsUnsafeOrAmbiguousTemplates_data();
	void rejectsUnsafeOrAmbiguousTemplates();
	void lexicalGuardrails_data();
	void lexicalGuardrails();
	void documentSizeLimit();
	void boundsResourceProcessing();
	void invalidSourceDoesNotDiscardValidSource();
	void capabilitiesAreGatedAtTheCorrectLevel();
	void validatesTileLimitsAndDiagnostics();
	void fingerprintsAreDeterministic();
	void fullFingerprintRetainsMatrixSetDescription();
};

#endif
