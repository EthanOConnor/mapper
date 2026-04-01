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


#include "storage_location.h"

#ifdef Q_OS_IOS
#import <Foundation/Foundation.h>

#include <QDir>
#include <QStandardPaths>


namespace OpenOrienteering {

namespace iOS {

/**
 * The cache of known locations.
 */
static std::shared_ptr<const std::vector<StorageLocation>> locations_cache;


/**
 * Returns the known storage locations on iOS.
 *
 * The app's Documents directory is exposed to the user via the Files app
 * when UIFileSharingEnabled is set in Info.plist.
 */
std::vector<StorageLocation> knownLocations()
{
	std::vector<StorageLocation> locations;

	// The app's Documents directory — visible in Files app via UIFileSharingEnabled
	auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
	if (!documents.isEmpty())
	{
		QDir dir(documents);
		if (!dir.exists())
			dir.mkpath(QStringLiteral("."));
		locations.emplace_back(documents, StorageLocation::HintNormal);
	}

	return locations;
}

}  // namespace iOS

}  // namespace OpenOrienteering

#endif
