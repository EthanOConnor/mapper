/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_PROTOCOL_FIXTURE_T_H
#define OPENORIENTEERING_MAP_HUB_PROTOCOL_FIXTURE_T_H

#include <QObject>

class MapHubProtocolFixtureTest : public QObject {
  Q_OBJECT

private slots:
  void nativeBootstrapFixture();
  void rejectsMalformedEntityIndexes();
  void supportsAllAddressableEntityOperations();
  void journalsReplayAndRebasesCompactedPendingWork();
  void controllerStagesCompleteFileHandoffs();
};

#endif
