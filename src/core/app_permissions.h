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


#ifndef OPENORIENTEERING_APP_PERMISSIONS_H
#define OPENORIENTEERING_APP_PERMISSIONS_H

#include <QtGlobal>

#ifdef MAPPER_MOBILE
#  include <functional>
#  include <QCoreApplication>
#  include <QObject>
#  include <QPermission>
#  include <QPointer>
#  include <QTimer>
#  if defined(Q_OS_ANDROID)
#    include <QJniObject>
#    include <QtCore/qnativeinterface.h>
#    include <QtCore/qcoreapplication_platform.h>
#  endif
#endif


#ifdef __clang_analyzer__
#define singleShot(A, B, C) singleShot(A, B, #C) // NOLINT
#endif


/**
 * A generic utility for requesting app permissions from the user.
 *
 * On Qt 6.5+, this wraps the QPermission API. On desktop platforms
 * where permissions are always granted, the functions are inline no-ops.
 */
namespace AppPermissions
{
	/// Permissions which are required for certain features of the application.
	enum AppPermission
	{
		LocationAccess,
		StorageAccess,
	};

	/// Possible results of requesting a permission.
	enum PermissionResult
	{
		Denied,
		Granted,
	};



	/**
	 * Checks if the permission was granted or not.
	 */
	PermissionResult checkPermission(AppPermission permission);

	/**
	 * Asynchronously requests a new permission to be granted.
	 *
	 * The given member function on the receiver will be called when the
	 * permission is actually granted.
	 *
	 * This function must not be called while the requested permission is granted.
	 */
	template<class T>
	void requestPermission(AppPermission permission, T* object, void (T::* function)());

	/**
	 * Requests a permissions to be granted to the application.
	 *
	 * This function must not be called while the requested permission is granted.
	 */
	PermissionResult requestPermissionSync(AppPermission permission);



#ifdef MAPPER_MOBILE

	// Mobile platforms use the Qt 6 QPermission API.

	template<class T>
	void requestPermission(AppPermission permission, T* object, void (T::* function)())
	{
		static_assert(std::is_base_of<QObject, T>::value);

		QPointer<T> safe_object = object;
		auto callback = [safe_object, function](const QPermission& p) {
			if (safe_object && p.status() == Qt::PermissionStatus::Granted)
				QTimer::singleShot(10, safe_object, function);
		};

		switch (permission)
		{
		case LocationAccess:
			{
				QLocationPermission loc;
				loc.setAccuracy(QLocationPermission::Precise);
				qApp->requestPermission(loc, callback);
			}
			break;

		case StorageAccess:
#if defined(Q_OS_ANDROID)
			if (QNativeInterface::QAndroidApplication::sdkVersion() >= 30)
			{
				// API 30+: open system all-files-access settings
				auto uri = QJniObject::callStaticObjectMethod(
				    "android/net/Uri", "parse",
				    "(Ljava/lang/String;)Landroid/net/Uri;",
				    QJniObject::fromString(
				        QStringLiteral("package:org.openorienteering.mapper")).object<jstring>());
				QJniObject intent("android/content/Intent",
				    "(Ljava/lang/String;Landroid/net/Uri;)V",
				    QJniObject::fromString(
				        QStringLiteral("android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION")).object<jstring>(),
				    uri.object());
				intent.callObjectMethod("addFlags", "(I)Landroid/content/Intent;", 0x10000000 /*FLAG_ACTIVITY_NEW_TASK*/);
				QJniObject context = QNativeInterface::QAndroidApplication::context();
				context.callObjectMethod("startActivity",
				    "(Landroid/content/Intent;)V",
				    intent.object());
				// User returns from settings; callback fires immediately.
				// The home screen will re-check permissions when it refreshes.
				QTimer::singleShot(10, object, function);
				break;
			}
#endif
			QTimer::singleShot(10, object, function);
			break;
		}
	}

#else

	// The default implementation is fully inline,
	// in order to allow the compiler to optimize it out.

	inline PermissionResult checkPermission(AppPermission /*permission*/)
	{
		return Granted;
	}

	template<class T>
	void requestPermission(AppPermission /*permission*/, T* /*object*/, void (T::* /*function*/)())
	{
		// requestPermission() shouldn't be called because permissions are always granted.
		Q_UNREACHABLE();
	}

	inline PermissionResult requestPermissionSync(AppPermission /*permission*/)
	{
		// requestPermission() shouldn't be called because permissions are always granted.
		Q_UNREACHABLE();
		return Granted;
	}

#endif

}  // namespace AppPermissions


#ifdef __clang_analyzer__
#undef singleShot(A, B, C)
#endif


#endif  // OPENORIENTEERING_APP_PERMISSIONS_H
