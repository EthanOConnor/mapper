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
#include <QRectF>
#include <QString>

#include "gdal/online_imagery_source.h"

class QColor;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QRadioButton;

namespace OpenOrienteering {

class Map;


/**
 * Dialog for adding an online imagery template.
 */
class OnlineTemplateDialog : public QDialog
{
	Q_OBJECT

public:
	enum class CoverageMode {
		CurrentView,
		FullMap,
	};

	OnlineTemplateDialog(
		Map& map,
		QString map_path,
		QRectF current_view_extent,
		QWidget* parent = nullptr);
	~OnlineTemplateDialog() override;

	const QString& generatedPath() const { return generated_path; }

private slots:
	void onAddClicked();

private:
	void setStatus(const QString& message, const QColor& color, bool italic);
	void clearStatus();
	void generateAndAccept();
	void populateSourceChooser();
	void onUrlEdited(const QString& text);
	void onSourceChosen(int index);
	void setSuggestedTemplateName(const QString& source_name);
	QString mapDisplayName() const;
	QString inferredSourceNameForUrl() const;
	QString suggestedTemplateName(const QString& source_name) const;
	QString effectiveTemplateName(const OnlineImagerySource& source) const;
	QRectF selectedCoverageExtent(QString* error = nullptr) const;
	static void saveRecentSource(const QString& url, const QString& display_name);

	Map& map;
	QString map_path;
	QRectF current_view_extent;

	QLineEdit* url_edit = nullptr;
	QLineEdit* name_edit = nullptr;
	QComboBox* source_chooser = nullptr;
	QRadioButton* current_view_radio = nullptr;
	QRadioButton* full_map_radio = nullptr;
	QLabel* status_label = nullptr;
	QPushButton* add_button = nullptr;

	OnlineImagerySource pending_source;
	QString generated_path;
	QString source_name_hint;
};


}  // namespace OpenOrienteering

#endif // OPENORIENTEERING_ONLINE_TEMPLATE_DIALOG_H
