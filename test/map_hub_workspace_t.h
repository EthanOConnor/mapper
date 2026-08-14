/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_WORKSPACE_T_H
#define OPENORIENTEERING_MAP_HUB_WORKSPACE_T_H

#include <QObject>

class MapHubWorkspaceTest : public QObject {
  Q_OBJECT
private slots:
  void initTestCase();
  void storesMacCredentialsInOwnerOnlyFile();
  void recordRoundTripsWithoutSecrets();
  void recordIsBoundToCanonicalMapPath();
  void readOnlyDocumentRoundTripsAndRejectsPathSubstitution();
  void validatesServerTransport();
  void identifiesMapperWorkspacePackageTypes();
  void classifiesWorkspaceBaselines();
  void hashesArtifactsExactly();
  void completesBrowserMediatedConnection();
  void editAccessUsesNativeIdempotentEndpoints();
  void assignmentStartUsesResolvedEditingContext();
  void consolidatesWorkItemsAndKeepsHistoryCollapsed();
  void verifiedDownloadRequiresBoundRevisionHeaders();
  void checkpointCarriesStreamProjectionDigest();
  void snapshotCompressesEntityIndex();
  void transactionPostCompressesSemanticOperations();
  void canonicalizesOperationJsonExactly();
  void rejectsValuesOutsideOperationJsonProfile();
  void boundsZstdTransportFrames();
  void workspaceSyncStateDecodesBoundedZstd();
  void pendingDraftRoundTripsAndCoalesces();
  void pendingDraftRejectsMovedOrMissingSnapshots();
  void relocatesStaleIosWorkspaceRoots();
  void preservesPublishedTileMatrixLimits();
  void preservesNativeAndWebMercatorMatrixSemantics();
  void rejectsManifestLayersWithoutMatrixMetadata();
  void replacesAuthorizedProjectCatalogsWithoutStaleEntries();
};

#endif
