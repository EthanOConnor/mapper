/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_work_item_projection.h"

#include <algorithm>

#include <QDateTime>
#include <QHash>
#include <QJsonArray>
#include <QObject>
#include <QSet>
#include <QUuid>

#include "collaboration/map_hub_api_client.h"

namespace OpenOrienteering {

namespace {

bool validId(const QString &value) {
  const QUuid id(value);
  return !id.isNull() && id.toString(QUuid::WithoutBraces).toLower() == value;
}

QString expectedWorkItemId(const QJsonObject &item, const QJsonArray &tasks) {
  const auto workspace_id =
      item.value(QStringLiteral("workspace_id")).toString();
  if (validId(workspace_id))
    return QStringLiteral("workspace:%1").arg(workspace_id);
  const auto workstream_id =
      item.value(QStringLiteral("workstream_id")).toString();
  if (validId(workstream_id))
    return QStringLiteral("workstream:%1").arg(workstream_id);
  if (tasks.size() == 1) {
    const auto package_id = tasks.first()
                                .toObject()
                                .value(QStringLiteral("work_package_id"))
                                .toString();
    if (validId(package_id))
      return QStringLiteral("work-package:%1").arg(package_id);
  }
  return {};
}

bool openParticipation(const QJsonObject &participation) {
  const auto status = participation.value(QStringLiteral("status")).toString();
  return status == QLatin1String("offered") ||
         status == QLatin1String("accepted") ||
         status == QLatin1String("active");
}

QJsonObject taskFor(const QJsonArray &tasks, const QString &package_id,
                    bool *unique = nullptr) {
  QJsonObject result;
  int matches = 0;
  for (const auto &value : tasks) {
    const auto task = value.toObject();
    if (task.value(QStringLiteral("work_package_id")).toString() ==
        package_id) {
      result = task;
      ++matches;
    }
  }
  if (unique)
    *unique = matches == 1;
  return result;
}

QJsonObject mostRecentTask(const QJsonArray &tasks) {
  QJsonObject latest;
  for (const auto &value : tasks) {
    const auto task = value.toObject();
    if (latest.isEmpty() ||
        task.value(QStringLiteral("updated_at")).toString() >
            latest.value(QStringLiteral("updated_at")).toString())
      latest = task;
  }
  return latest;
}

MapHubWorkItemRow rowFor(const QJsonObject &item,
                         const QJsonObject &participation,
                         const QJsonObject &task,
                         MapHubWorkItemRow::Bucket bucket) {
  MapHubWorkItemRow row;
  row.bucket = bucket;
  row.project_id = item.value(QStringLiteral("project_id")).toString();
  row.work_item_id = item.value(QStringLiteral("id")).toString();
  row.workspace_id = item.value(QStringLiteral("workspace_id")).toString();
  row.assignment_id =
      participation.value(QStringLiteral("assignment_id")).toString();
  row.task_title = task.value(QStringLiteral("title")).toString();
  row.package_type = task.value(QStringLiteral("type")).toString();
  row.assignment_status =
      participation.value(QStringLiteral("status")).toString();
  row.due_on = task.value(QStringLiteral("due_on")).toString();
  return row;
}

} // namespace

MapHubWorkItemProjection
MapHubWorkItemProjection::fromLibrary(const QJsonObject &library) {
  MapHubWorkItemProjection projection;
  const auto work_items_value = library.value(QStringLiteral("work_items"));
  if (!work_items_value.isArray())
    return projection;
  projection.contract_available = true;

  QHash<QString, int> counts;
  for (const auto &value : work_items_value.toArray()) {
    if (value.isObject())
      ++counts[value.toObject().value(QStringLiteral("id")).toString()];
  }

  QSet<QString> emitted;
  for (const auto &value : work_items_value.toArray()) {
    if (!value.isObject())
      continue;
    const auto item = value.toObject();
    const auto item_id = item.value(QStringLiteral("id")).toString();
    if (emitted.contains(item_id))
      continue;
    emitted.insert(item_id);

    const auto project_id = item.value(QStringLiteral("project_id")).toString();
    const auto workspace_id =
        item.value(QStringLiteral("workspace_id")).toString();
    const auto tasks = item.value(QStringLiteral("task_history")).toArray();
    const auto participations =
        item.value(QStringLiteral("participations")).toArray();
    const auto lifecycle =
        item.value(QStringLiteral("lifecycle_bucket")).toString();

    const auto invalid_identity =
        item_id != expectedWorkItemId(item, tasks) || !validId(project_id) ||
        (!workspace_id.isEmpty() && !validId(workspace_id)) ||
        counts.value(item_id) != 1;
    if (invalid_identity || lifecycle == QLatin1String("reconciliation")) {
      auto row = rowFor(item, {}, mostRecentTask(tasks),
                        MapHubWorkItemRow::Bucket::Reconciliation);
      row.problem =
          invalid_identity
              ? QObject::tr("Map Hub returned duplicate or incomplete "
                            "workstream identity. Refresh after it is "
                            "reconciled in Map Hub.")
              : QObject::tr("This map workstream needs reconciliation "
                            "in Map Hub before it can be opened.");
      projection.rows.append(row);
      continue;
    }

    if (lifecycle != QLatin1String("active")) {
      const auto bucket = lifecycle == QLatin1String("waiting_review")
                              ? MapHubWorkItemRow::Bucket::WaitingReview
                              : MapHubWorkItemRow::Bucket::History;
      projection.rows.append(rowFor(item, {}, mostRecentTask(tasks), bucket));
      continue;
    }

    const auto current =
        item.value(QStringLiteral("current_participation")).toObject();
    const auto current_error =
        item.value(QStringLiteral("current_participation_error")).toObject();
    if (current.isEmpty() || !current_error.isEmpty()) {
      auto row = rowFor(item, {}, mostRecentTask(tasks),
                        MapHubWorkItemRow::Bucket::Reconciliation);
      row.problem =
          current_error.value(QStringLiteral("code")).toString() ==
                  QLatin1String("ambiguous_current_participation")
              ? QObject::tr(
                    "Choose the current task in Map Hub before editing.")
              : QObject::tr("Map Hub could not identify current work for this "
                            "project.");
      projection.rows.append(row);
      continue;
    }

    const auto assignment_id =
        current.value(QStringLiteral("assignment_id")).toString();
    const auto package_id =
        current.value(QStringLiteral("work_package_id")).toString();
    bool unique_task = false;
    const auto task = taskFor(tasks, package_id, &unique_task);
    int participation_matches = 0;
    for (const auto &participation_value : participations) {
      if (participation_value.toObject() == current)
        ++participation_matches;
    }
    const auto task_status = task.value(QStringLiteral("status")).toString();
    const auto superseded =
        task.value(QStringLiteral("superseded_by_work_package_id"));
    auto row = rowFor(item, current, task, MapHubWorkItemRow::Bucket::Active);
    row.actionable =
        item.value(QStringLiteral("actionable")).toBool() &&
        validId(assignment_id) && validId(package_id) && unique_task &&
        participation_matches == 1 && openParticipation(current) &&
        QDateTime::fromString(
            current.value(QStringLiteral("assigned_at")).toString(),
            Qt::ISODate)
            .isValid() &&
        !current.value(QStringLiteral("start_url")).toString().isEmpty() &&
        !current.value(QStringLiteral("editing_context_url"))
             .toString()
             .isEmpty() &&
        (task_status == QLatin1String("ready") ||
         task_status == QLatin1String("active")) &&
        (superseded.isNull() || superseded.isUndefined()) &&
        MapHubApiClient::isMapperWorkspacePackageType(row.package_type);
    if (!row.actionable)
      row.problem = QObject::tr(
          "Map Hub returned incomplete current-work details. Refresh before "
          "opening this map.");
    projection.rows.append(row);
  }

  std::stable_sort(projection.rows.begin(), projection.rows.end(),
                   [](const auto &left, const auto &right) {
                     if (left.project_id != right.project_id)
                       return left.project_id < right.project_id;
                     if (left.bucket != right.bucket)
                       return left.bucket < right.bucket;
                     return left.task_title.localeAwareCompare(
                                right.task_title) < 0;
                   });
  return projection;
}

} // namespace OpenOrienteering
