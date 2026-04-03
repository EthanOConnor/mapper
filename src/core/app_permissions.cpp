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

#if defined(Q_OS_ANDROID)
#  include <QJniObject>
#  include <QtCore/qnativeinterface.h>
#  include <QtCore/qcoreapplication_platform.h>
#endif


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
#if defined(Q_OS_ANDROID)
		if (QNativeInterface::QAndroidApplication::sdkVersion() >= 30)
		{
			// API 30+: need MANAGE_EXTERNAL_STORAGE, checked via Environment.isExternalStorageManager()
			auto result = QJniObject::callStaticMethod<jboolean>(
			    "android/os/Environment", "isExternalStorageManager");
			return result ? Granted : Denied;
		}
		// API 28-29: requestLegacyExternalStorage + manifest permissions suffice
		return Granted;
#else
		// iOS: app-specific directories don't need permission
		return Granted;
#endif
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
