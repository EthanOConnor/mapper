/*
 *    Copyright 2026 The OpenOrienteering developers
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef OPENORIENTEERING_IOS_LIFECYCLE_H
#define OPENORIENTEERING_IOS_LIFECYCLE_H

#include <QtGlobal>

namespace OpenOrienteering {

namespace iOS {

/// Begin a background task to get extra time when the app is suspended.
/// Returns a task identifier that must be passed to endBackgroundTask().
unsigned long beginBackgroundTask();

/// End a background task started with beginBackgroundTask().
void endBackgroundTask(unsigned long identifier);

/// Lock the screen to the current orientation.
void lockOrientation();

/// Unlock screen orientation.
void unlockOrientation();

/// Return the current interface orientation as rotation index (0/1/2/3).
int displayRotation();

}  // namespace iOS

}  // namespace OpenOrienteering

#endif  // OPENORIENTEERING_IOS_LIFECYCLE_H
