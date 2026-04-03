/*
 *    Copyright 2019 Kai Pastor
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


#include "app_permissions.h"

#ifdef MAPPER_MOBILE

#include <QCoreApplication>
#include <QPermission>


namespace AppPermissions
{

PermissionResult checkPermission(AppPermission permission)
{
	switch (permission)
	{
	case LocationAccess:
		{
			QLocationPermission loc;
			loc.setAccuracy(QLocationPermission::Precise);
			if (qApp->checkPermission(loc) == Qt::PermissionStatus::Granted)
				return Granted;
			return Denied;
		}

	case StorageAccess:
		// Qt6 scoped storage: app-specific directories don't need permission
		return Granted;
	}

	return Denied;
}


PermissionResult requestPermissionSync(AppPermission permission)
{
	// Qt6 does not provide synchronous permission requests.
	// Check the current state instead.
	return checkPermission(permission);
}


}  // namespace AppPermissions

#endif
