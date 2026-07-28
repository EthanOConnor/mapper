/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "map_hub_workspace.h"

#include <QCoreApplication>
#include <QDir>
#include <QStandardPaths>

namespace OpenOrienteering {

QString defaultMapHubWorkspaceRoot() {
#ifdef Q_OS_ANDROID
  return QDir(QStandardPaths::writableLocation(QStandardPaths::AppDataLocation))
      .filePath(QStringLiteral("map-hub-workspaces"));
#else
  return QDir(QStandardPaths::writableLocation(
                  QStandardPaths::DocumentsLocation))
      .filePath(
          QCoreApplication::translate("MapHubDialog", "Mapper Workspaces"));
#endif
}

QString relocatedIosMapHubWorkspaceRoot(const QString &configured_root,
                                        const QString &documents_root) {
  auto root = QDir::cleanPath(configured_root.trimmed());
  auto current_documents = QDir::cleanPath(documents_root.trimmed());
  if (root.isEmpty() || current_documents.isEmpty())
    return configured_root;

  const auto container_marker = QStringLiteral("/Containers/Data/Application/");
  const auto marker_position = root.indexOf(container_marker);
  if (marker_position < 0 ||
      (!root.startsWith(
           QLatin1String("/var/mobile/Containers/Data/Application/")) &&
       !root.startsWith(
           QLatin1String("/private/var/mobile/Containers/Data/Application/"))))
    return configured_root;

  const auto documents_marker = QStringLiteral("/Documents");
  const auto documents_position =
      root.indexOf(documents_marker, marker_position + container_marker.size());
  if (documents_position < 0)
    return configured_root;
  const auto suffix_position = documents_position + documents_marker.size();
  if (suffix_position < root.size() &&
      root.at(suffix_position) != QLatin1Char('/'))
    return configured_root;

  return QDir::cleanPath(current_documents + root.mid(suffix_position));
}

QString normalizedMapHubWorkspaceRoot(const QString &configured_root) {
  if (configured_root.trimmed().isEmpty())
    return defaultMapHubWorkspaceRoot();
#ifdef Q_OS_IOS
  return relocatedIosMapHubWorkspaceRoot(
      configured_root,
      QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation));
#else
  return configured_root;
#endif
}

} // namespace OpenOrienteering
