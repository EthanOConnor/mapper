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

#ifndef OPENORIENTEERING_HOME_SCREEN_WIDGET_QUICK_H
#define OPENORIENTEERING_HOME_SCREEN_WIDGET_QUICK_H

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>

#include "gui/widgets/home_screen_widget.h"

class QQuickWidget;

namespace OpenOrienteering {

/**
 * The data and actions behind the QML mobile home screen.
 */
class HomeScreenBridge : public QObject
{
	Q_OBJECT
	Q_PROPERTY(QVariantList recentFiles READ recentFiles NOTIFY recentFilesChanged)
	Q_PROPERTY(QString appVersion READ appVersion CONSTANT)

public:
	explicit HomeScreenBridge(HomeScreenController* controller, QObject* parent = nullptr);

	QVariantList recentFiles() const { return recent_files; }
	void setRecentFilePaths(const QStringList& files);
	QString appVersion() const;

	Q_INVOKABLE void newMap();
	Q_INVOKABLE void openMap();
	Q_INVOKABLE void mapHub();
	Q_INVOKABLE void openFile(const QString& path);
	Q_INVOKABLE void showSettings();
	Q_INVOKABLE void showHelp();
	Q_INVOKABLE void showAbout();

signals:
	void recentFilesChanged();

private:
	HomeScreenController* controller;
	QVariantList recent_files;
};


/**
 * Touch-first Qt Quick home screen for mobile mode.
 */
class HomeScreenWidgetQuick : public AbstractHomeScreenWidget
{
	Q_OBJECT

public:
	explicit HomeScreenWidgetQuick(HomeScreenController* controller,
	                               QWidget* parent = nullptr);
	~HomeScreenWidgetQuick() override;

public slots:
	void setRecentFiles(const QStringList& files) override;
	void setOpenMRUFileChecked(bool state) override;
	void setTipOfTheDay(const QString& text) override;
	void setTipsVisible(bool state) override;

private:
	QQuickWidget* quick_widget;
	HomeScreenBridge* bridge;
};

}  // namespace OpenOrienteering

#endif
