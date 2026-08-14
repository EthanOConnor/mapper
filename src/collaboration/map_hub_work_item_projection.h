/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_WORK_ITEM_PROJECTION_H
#define OPENORIENTEERING_MAP_HUB_WORK_ITEM_PROJECTION_H

#include <QJsonObject>
#include <QString>
#include <QVector>

namespace OpenOrienteering {

struct MapHubWorkItemRow {
  enum class Bucket { Active, WaitingReview, History, Reconciliation };

  Bucket bucket = Bucket::Reconciliation;
  QString project_id;
  QString work_item_id;
  QString workspace_id;
  QString assignment_id;
  QString task_title;
  QString package_type;
  QString assignment_status;
  QString due_on;
  QString problem;
  bool actionable = false;
};

struct MapHubWorkItemProjection {
  bool contract_available = false;
  QVector<MapHubWorkItemRow> rows;

  /**
   * Projects Map Hub's durable work_items contract into user-visible rows.
   * Historical task/assignment shells remain inside one collapsed work-item
   * row instead of resurfacing as duplicate assignments.
   */
  static MapHubWorkItemProjection fromLibrary(const QJsonObject &library);
};

} // namespace OpenOrienteering

#endif
