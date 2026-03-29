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

#ifndef OPENORIENTEERING_ONLINE_TEMPLATE_DIALOG_H
#define OPENORIENTEERING_ONLINE_TEMPLATE_DIALOG_H

#include <QDialog>
#include <QString>

#include "gdal/online_imagery_source.h"

class QLabel;
class QLineEdit;
class QNetworkAccessManager;
class QNetworkReply;
class QPushButton;

namespace OpenOrienteering {

class Map;
class MapEditorController;


/**
 * Dialog for adding an online imagery template.
 *
 * Collects a URL from the user, classifies the source, fetches metadata
 * if needed, and generates a local GDAL XML file.
 */
class OnlineTemplateDialog : public QDialog
{
Q_OBJECT
public:
	OnlineTemplateDialog(Map& map, MapEditorController& controller, QWidget* parent = nullptr);
	~OnlineTemplateDialog() override;

	/**
	 * Returns the path to the generated GDAL XML file, or empty on cancel/error.
	 */
	const QString& generatedPath() const { return generated_path; }

private slots:
	void onAddClicked();
	void onMetadataReplyFinished(QNetworkReply* reply);

private:
	void showError(const QString& message);
	void showProgress(const QString& message);
	void clearStatus();
	void generateAndAccept();

	Map& map;
	MapEditorController& controller;

	QLineEdit* url_edit;
	QLabel* status_label;
	QPushButton* add_button;

	QNetworkAccessManager* network = nullptr;
	OnlineImagerySource pending_source;
	QString generated_path;
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_ONLINE_TEMPLATE_DIALOG_H
