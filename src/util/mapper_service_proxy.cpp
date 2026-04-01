/*
 *    Copyright 2019 Kai Pastor
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


#include "mapper_service_proxy.h"

#include <QWidget>

#ifdef Q_OS_ANDROID
#include <QCoreApplication>
#include <QJniObject>
#include <QtCore/qnativeinterface.h>
#include <QtCore/qcoreapplication_platform.h>
#include <QPermission>
#endif

#include "gui/main_window.h"


namespace OpenOrienteering {


MapperServiceProxy::~MapperServiceProxy()
{
	setActiveWindow(nullptr);
}


void MapperServiceProxy::setActiveWindow(QWidget* window)
{
	if (active_window == window)
		return;
	
	if (active_window != nullptr)
		stopService();
		
	active_window = window;
		
	if (active_window == nullptr)
		return;
	
#ifdef Q_OS_ANDROID
	if (QNativeInterface::QAndroidApplication::sdkVersion() >= 28)
	{
		// On Android 9+, foreground service permission is granted automatically
		// via the manifest declaration. Just start the service.
	}
#endif
	
	startService();
}


void MapperServiceProxy::startService()
{
	Q_ASSERT(active_window);
	
#ifdef Q_OS_ANDROID
	auto const file_path = active_window->windowFilePath();
	auto const prefix_length = file_path.lastIndexOf(QLatin1Char('/')) + 1;
	QJniObject java_string = QJniObject::fromString(file_path.mid(prefix_length));
	QJniObject::callStaticMethod<void>("org/openorienteering/mapper/MapperActivity",
	                                   "startService",
	                                   "(Ljava/lang/String;)V",
	                                   java_string.object<jstring>());
#endif
}


void MapperServiceProxy::stopService()
{
	Q_ASSERT(active_window);
	
#ifdef Q_OS_ANDROID
	QJniObject::callStaticMethod<void>("org/openorienteering/mapper/MapperActivity",
	                                   "stopService",
	                                   "()V");
#endif
}

}  // namespace OpenOrienteering
