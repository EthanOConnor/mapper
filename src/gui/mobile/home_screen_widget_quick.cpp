/*
 *    Copyright 2026 Ethan O'Connor
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

#include "home_screen_widget_quick.h"

#include <QDir>
#include <QFileInfo>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQuickWidget>
#include <QTimer>
#include <QQuickStyle>
#include <QVBoxLayout>
#include <QVariantMap>

#include "mapper_config.h"  // APP_VERSION
#include "gui/home_screen_controller.h"
#include "gui/main_window.h"

namespace OpenOrienteering {

HomeScreenBridge::HomeScreenBridge(HomeScreenController* controller, QObject* parent)
: QObject(parent)
, controller(controller)
{
}

void HomeScreenBridge::setRecentFilePaths(const QStringList& files)
{
	recent_files.clear();
	for (const auto& path : files)
	{
		QFileInfo info(path);
		QVariantMap entry;
		entry[QStringLiteral("name")] = info.fileName();
		entry[QStringLiteral("path")] = path;
		entry[QStringLiteral("directory")] = info.dir().path();
		recent_files.append(entry);
	}
	emit recentFilesChanged();
}

QString HomeScreenBridge::appVersion() const
{
	return QStringLiteral(APP_VERSION);
}

void HomeScreenBridge::newMap()
{
	controller->getWindow()->showNewMapWizard();
}

void HomeScreenBridge::openMap()
{
	controller->getWindow()->showOpenDialog();
}

void HomeScreenBridge::mapHub()
{
	controller->getWindow()->showMapHub();
}

void HomeScreenBridge::openFile(const QString& path)
{
	controller->getWindow()->openPath(path);
}

void HomeScreenBridge::showSettings()
{
	controller->getWindow()->showSettings();
}

void HomeScreenBridge::showHelp()
{
	controller->getWindow()->showHelp();
}

void HomeScreenBridge::showAbout()
{
	controller->getWindow()->showAbout();
}


HomeScreenWidgetQuick::HomeScreenWidgetQuick(HomeScreenController* controller,
                                             QWidget* parent)
: AbstractHomeScreenWidget(controller, parent)
, quick_widget(nullptr)
, bridge(new HomeScreenBridge(controller, this))
{
	if (QQuickStyle::name().isEmpty())
		QQuickStyle::setStyle(QStringLiteral("Material"));

	auto* layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(0);

	quick_widget = new QQuickWidget(this);
	quick_widget->setResizeMode(QQuickWidget::SizeRootObjectToView);
	quick_widget->engine()->rootContext()->setContextProperty(
	    QStringLiteral("homeBridge"), bridge);
	quick_widget->setSource(QUrl(QStringLiteral("qrc:/mobile/HomeScreen.qml")));
	layout->addWidget(quick_widget);

	// Temporary UI-iteration hook: save a rendering of this screen.
	if (qEnvironmentVariableIsSet("MAPPER_DEBUG_SHOOT_HOME"))
	{
		auto* timer = new QTimer(this);
		timer->setInterval(1500);
		connect(timer, &QTimer::timeout, this, [this] {
			grab().save(qEnvironmentVariable("MAPPER_DEBUG_SHOOT_HOME"));
		});
		timer->start();
	}
}

HomeScreenWidgetQuick::~HomeScreenWidgetQuick() = default;

void HomeScreenWidgetQuick::setRecentFiles(const QStringList& files)
{
	bridge->setRecentFilePaths(files);
}

void HomeScreenWidgetQuick::setOpenMRUFileChecked(bool /* state */)
{
	// Managed from the mobile settings screen instead.
}

void HomeScreenWidgetQuick::setTipOfTheDay(const QString& /* text */)
{
	// Tips are not part of the mobile home screen.
}

void HomeScreenWidgetQuick::setTipsVisible(bool /* state */)
{
	// Tips are not part of the mobile home screen.
}

}  // namespace OpenOrienteering
