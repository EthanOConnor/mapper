/*
 *    Copyright 2012-2017 Kai Pastor
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


#include "mapper_resource.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QLatin1String>
#include <QString>
#include <QStringList>

#include "mapper_config.h"


namespace OpenOrienteering {

namespace MapperResource {

namespace {

QString environmentPath(const char* name)
{
	auto value = qEnvironmentVariable(name);
	if (value.isEmpty())
		return {};
	
	auto info = QFileInfo{value};
	return info.exists() ? info.absoluteFilePath() : QString{};
}

}

void setSeachPaths()
{
	QStringList data_paths;
	data_paths.reserve(5);
	
	QStringList doc_paths;
	doc_paths.reserve(5);
	
#if defined(MAPPER_DEVELOPMENT_BUILD) && defined(MAPPER_DEVELOPMENT_RES_DIR)
	// Use the directory where Mapper is built during development, 
	// even for the unit tests located in other directories.
	auto build_dir = QString::fromLocal8Bit(MAPPER_DEVELOPMENT_RES_DIR);
	data_paths.append(build_dir);
	doc_paths.append(build_dir + QLatin1String("/doc/licensing"));
	doc_paths.append(build_dir + QLatin1String("/doc/manual"));
#endif
	
#if defined(Q_OS_MACOS)
	// Mac OS X: load resources from the Resources directory of the bundle
	auto app_dir = QDir { QCoreApplication::applicationDirPath() };
	auto resources_dir = app_dir.absoluteFilePath(QLatin1String("../Resources"));
	data_paths.append(resources_dir);
	doc_paths.append(resources_dir + QLatin1String("/doc"));
#elif defined(Q_OS_WIN)
	// Windows: load resources from the application directory
	auto app_dir = QDir(QCoreApplication::applicationDirPath()).absolutePath();
	data_paths.append(app_dir);
	doc_paths.append(app_dir + QLatin1String("/doc"));
#elif defined(Q_OS_ANDROID)
	// Android: load resources from the assets directory
	auto assets = QString::fromLatin1("assets:");
	data_paths.append(assets);
	doc_paths.append(assets + QLatin1String("/doc"));
#else
	auto const data_override = environmentPath("MAPPER_DATA_PATH");
	if (!data_override.isEmpty())
		data_paths.append(data_override);
	
	auto const doc_override = environmentPath("MAPPER_DOC_PATH");
	if (!doc_override.isEmpty())
		doc_paths.append(doc_override);
	
#ifdef MAPPER_PACKAGE_NAME
	auto app_dir = QDir{ QCoreApplication::applicationDirPath() };
	auto relative_data_dir = QFileInfo{
	  app_dir.absoluteFilePath(QLatin1String("../share/") + QLatin1String(MAPPER_PACKAGE_NAME))
	};
	if (relative_data_dir.isDir())
		data_paths.append(relative_data_dir.absoluteFilePath());
	
	auto relative_doc_dir = QFileInfo{
	  app_dir.absoluteFilePath(QLatin1String("../share/doc/") + QLatin1String(MAPPER_PACKAGE_NAME))
	};
	if (relative_doc_dir.isDir())
		doc_paths.append(relative_doc_dir.absoluteFilePath());
#endif
	
	data_paths.append(QString::fromLatin1(MAPPER_DATA_DESTINATION));
	doc_paths.append(QString::fromLatin1(MAPPER_ABOUT_DESTINATION));
#endif
	
	// General default path: Qt resource system
	auto resources = QString{ QLatin1String{":"} };
	data_paths.append(resources);
	doc_paths.append(resources + QLatin1String("/doc"));
	
	QDir::setSearchPaths(QLatin1String("data"), data_paths);
	QDir::setSearchPaths(QLatin1String("doc"), doc_paths);
}


}  // namespace MapperResource


}  // namespace OpenOrienteering
