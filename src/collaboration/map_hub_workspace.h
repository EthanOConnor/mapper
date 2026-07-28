/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_MAP_HUB_WORKSPACE_H
#define OPENORIENTEERING_MAP_HUB_WORKSPACE_H

#include <QString>

namespace OpenOrienteering {

QString defaultMapHubWorkspaceRoot();
QString normalizedMapHubWorkspaceRoot(const QString &configured_root);
QString relocatedIosMapHubWorkspaceRoot(const QString &configured_root,
                                        const QString &documents_root);

} // namespace OpenOrienteering

#endif
