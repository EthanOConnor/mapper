/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2014 Thomas Schöps, Kai Pastor
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


#include "main_window_controller.h"

#include <QtGlobal>
#include <QMessageBox>

#include "settings.h"
#include "fileformats/file_format.h"
#include "fileformats/file_format_registry.h"
#include "gui/main_window.h"
#include "gui/map/map_editor.h"


namespace OpenOrienteering {

MainWindowController::~MainWindowController() = default;


bool MainWindowController::menuBarVisible()
{
	return !Settings::getInstance().touchModeEnabled();
}

bool MainWindowController::statusBarVisible()
{
	return !Settings::getInstance().touchModeEnabled();
}


bool MainWindowController::saveTo(const QString& /*path*/, const FileFormat& /*format*/)
{
	return false;
}

bool MainWindowController::markSaveCommitted(
	quint64 staged_revision,
	bool retain_external_resource_dirtiness)
{
	Q_UNUSED(staged_revision)
	Q_UNUSED(retain_external_resource_dirtiness)
	// Nothing to commit in the generic controller.
	return true;
}

quint64 MainWindowController::saveRevision() const
{
	return 0;
}

bool MainWindowController::hasDirtyExternalResources() const
{
	return false;
}

bool MainWindowController::stageSaveTo(const QString& logical_path,
	                                   const FileFormat& format,
	                                   QIODevice* target_device,
	                                   quint64* staged_revision)
{
	if (target_device)
		return false;
	if (!exportTo(logical_path, format))
		return false;
	if (staged_revision)
		*staged_revision = saveRevision();
	return true;
}

bool MainWindowController::exportTo(const QString& path)
{
	auto format = FileFormats.findFormatForFilename(path, &FileFormat::supportsWriting);
	if (!format)
		format = FileFormats.findFormat(FileFormats.defaultFormat());
	if (!format)
	{
		QMessageBox::warning(window,
		                     tr("Error"),
		                     tr("Cannot export the map as\n\"%1\"\n"
		                        "because the format is unknown.").arg(path));
		return false;
	}
	if (!format->supportsWriting())
	{
		QMessageBox::warning(window,
		                     tr("Error"),
		                     tr("Cannot export the map as\n\"%1\"\n"
		                        "because saving as %2 (.%3) is not supported.").
		                     arg(path, format->description(),
		                         format->fileExtensions().join(QLatin1String(", "))));
		return false;
	}
	return exportTo(path, *format);
}

bool MainWindowController::exportTo(const QString& /*path*/, const FileFormat& /*format*/)
{
	return false;
}

bool MainWindowController::loadFrom(const QString& /*path*/,
	                                const FileFormat& /*format*/,
	                                QWidget* /*dialog_parent*/,
	                                QIODevice* /*source_device*/)
{
	return false;
}

void MainWindowController::detach()
{
	// nothing
}

bool MainWindowController::isEditingInProgress() const
{
	return false;
}

bool MainWindowController::keyPressEventFilter(QKeyEvent* event)
{
	Q_UNUSED(event);
	return false;
}

bool MainWindowController::keyReleaseEventFilter(QKeyEvent* event)
{
	Q_UNUSED(event);
	return false;
}

MainWindowController* MainWindowController::controllerForFile(const QString& filename, const FileFormat* format)
{
	if ((format && format->supportsReading())
	    || FileFormats.findFormatForFilename(filename, &FileFormat::supportsReading))
		return new MapEditorController(MapEditorController::MapEditor);
	
	return nullptr;
}


}  // namespace OpenOrienteering
