/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_ACTION_ICON_H
#define OPENORIENTEERING_ACTION_ICON_H

#include <QIcon>
#include <QStringView>

namespace OpenOrienteering::ActionIcon {

/** Maximum physical extent materialized for an application icon. */
inline constexpr auto maxRasterExtent = 256;

/** Returns a native SVG action icon with bounded rasterization. */
QIcon fromName(QStringView name);

/** Applies the same bounded scaling contract to a native or style icon. */
QIcon bounded(QIcon source);

}  // namespace OpenOrienteering::ActionIcon

#endif
