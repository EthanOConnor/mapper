/*
 *    Copyright 2016 Kai Pastor
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

#include <cstddef>

#include <QtGlobal>
#include <QStandardPaths>
#include <QStringList>


namespace OpenOrienteering {

// static
std::shared_ptr<const std::vector<StorageLocation>> StorageLocation::knownLocations()
{
	auto locations = std::vector<StorageLocation>();
	auto const paths = QStandardPaths::standardLocations(QStandardPaths::DocumentsLocation);
	locations.reserve(std::size_t(paths.size()));
	for (const auto& path : paths)
	{
		locations.emplace_back(path,
#ifdef Q_OS_ANDROID
		                       HintApplication
#else
		                       HintNormal
#endif
		);
	}
	return std::make_shared<const std::vector<StorageLocation>>(std::move(locations));
}


void StorageLocation::refresh()
{
	// QStandardPaths is queried on every call to knownLocations().
}


// static
QString StorageLocation::fileHintTextTemplate(Hint hint)
{
	switch (hint)
	{
	case HintNormal:
		return {};  // No text for a regular location.
		
	case HintApplication:
		return tr("'%1' is located in app storage. The files will be removed when uninstalling the app.");
		
	case HintReadOnly:
		return tr("'%1' is not writable. Changes cannot be saved.");
		
	case HintNoAccess:
		return tr("Extra permissions are required to access '%1'.");
		
	case HintInvalid:
		return tr("'%1' is not a valid storage location.");
	}
	
	Q_UNREACHABLE();
}


QString OpenOrienteering::StorageLocation::hintText() const
{
	return hint() == HintNormal ? QString{} : fileHintTextTemplate(hint()).arg(path());
}


}  // namespace OpenOrienteering
