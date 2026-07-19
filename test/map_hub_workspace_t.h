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
  void recordRoundTripsWithoutSecrets();
  void recordIsBoundToCanonicalMapPath();
  void validatesServerTransport();
  void identifiesMapperWorkspacePackageTypes();
  void classifiesWorkspaceBaselines();
  void hashesArtifactsExactly();
};

#endif
