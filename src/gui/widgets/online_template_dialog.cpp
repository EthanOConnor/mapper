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

#include "online_template_dialog.h"

#include <utility>

#include <Qt>
#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPalette>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QStringList>
#include <QUrl>
#include <QVBoxLayout>

#include "core/georeferencing.h"
#include "core/map.h"
#include "gdal/online_imagery_template_builder.h"

namespace OpenOrienteering {

#ifndef MAPPER_USE_GDAL

OnlineTemplateDialog::OnlineTemplateDialog(
	Map& map,
	QString map_path,
	QRectF current_view_extent,
	QWidget* parent)
: QDialog(parent)
, map(map)
, map_path(std::move(map_path))
, current_view_extent(current_view_extent)
{
}

OnlineTemplateDialog::~OnlineTemplateDialog() = default;

void OnlineTemplateDialog::onAddClicked() {}

#else

namespace {

constexpr int source_url_role = Qt::UserRole;
constexpr int source_name_role = Qt::UserRole + 1;
constexpr int max_recent_sources = 5;

}  // namespace


OnlineTemplateDialog::OnlineTemplateDialog(
	Map& map,
	QString map_path,
	QRectF current_view_extent,
	QWidget* parent)
: QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint)
, map(map)
, map_path(std::move(map_path))
, current_view_extent(std::move(current_view_extent))
{
	setWindowTitle(tr("Add online imagery template"));
	setMinimumWidth(470);

	auto* layout = new QVBoxLayout(this);

	auto* chooser_label = new QLabel(tr("Presets / recent:"), this);
	layout->addWidget(chooser_label);

	source_chooser = new QComboBox(this);
	populateSourceChooser();
	layout->addWidget(source_chooser);
	connect(source_chooser,
	        QOverload<int>::of(&QComboBox::activated),
	        this,
	        &OnlineTemplateDialog::onSourceChosen);

	auto* url_label = new QLabel(tr("Imagery Link (XYZ/TMS or ArcGIS MapServer):"), this);
	layout->addWidget(url_label);

	url_edit = new QLineEdit(this);
	url_edit->setPlaceholderText(tr("https://tile.example.com/{z}/{x}/{y}.png"));
	layout->addWidget(url_edit);
	connect(url_edit, &QLineEdit::textEdited, this, &OnlineTemplateDialog::onUrlEdited);

	auto* name_label = new QLabel(tr("Template Name:"), this);
	layout->addWidget(name_label);

	name_edit = new QLineEdit(this);
	name_edit->setFrame(true);
	name_edit->setMinimumHeight(url_edit->sizeHint().height());
	layout->addWidget(name_edit);

	auto* coverage_label = new QLabel(tr("Coverage:"), this);
	layout->addWidget(coverage_label);

	auto* coverage_layout = new QHBoxLayout();
	full_map_radio = new QRadioButton(tr("Full Map"), this);
	current_view_radio = new QRadioButton(tr("Current View"), this);
	full_map_radio->setChecked(true);
	coverage_layout->addWidget(full_map_radio);
	coverage_layout->addWidget(current_view_radio);
	coverage_layout->addStretch();
	layout->addLayout(coverage_layout);

	status_label = new QLabel(this);
	status_label->setWordWrap(true);
	status_label->hide();
	layout->addWidget(status_label);

	layout->addStretch();

	auto* button_layout = new QHBoxLayout();
	button_layout->addStretch();

	add_button = new QPushButton(tr("Add"), this);
	add_button->setDefault(true);
	button_layout->addWidget(add_button);

	auto* cancel_button = new QPushButton(tr("Cancel"), this);
	button_layout->addWidget(cancel_button);

	layout->addLayout(button_layout);

	connect(add_button, &QPushButton::clicked, this, &OnlineTemplateDialog::onAddClicked);
	connect(cancel_button, &QPushButton::clicked, this, &QDialog::reject);
	connect(url_edit, &QLineEdit::returnPressed, this, &OnlineTemplateDialog::onAddClicked);
}


OnlineTemplateDialog::~OnlineTemplateDialog()
= default;


void OnlineTemplateDialog::onAddClicked()
{
	clearStatus();

	if (map_path.isEmpty())
	{
		setStatus(tr("Save the map first to add online imagery."), Qt::red, false);
		return;
	}

	if (map.getGeoreferencing().getState() != Georeferencing::Geospatial)
	{
		setStatus(tr("This map is not georeferenced yet. Online imagery needs a georeferenced map."), Qt::red, false);
		return;
	}

	QString coverage_error;
	selectedCoverageExtent(&coverage_error);
	if (!coverage_error.isEmpty())
	{
		setStatus(coverage_error, Qt::red, false);
		return;
	}

	auto classify_result = OnlineImageryTemplateBuilder::classifyUrl(url_edit->text());
	if (!classify_result.error.isEmpty())
	{
		setStatus(classify_result.error, Qt::red, false);
		return;
	}

	pending_source = classify_result.source;
	if (name_edit->text().trimmed().isEmpty())
		setSuggestedTemplateName(pending_source.display_name);

	generateAndAccept();
}


void OnlineTemplateDialog::setStatus(const QString& message, const QColor& color, bool italic)
{
	auto palette = status_label->palette();
	palette.setColor(QPalette::WindowText, color);
	status_label->setPalette(palette);
	auto font = status_label->font();
	font.setItalic(italic);
	status_label->setFont(font);
	status_label->setText(message);
	status_label->show();
}


void OnlineTemplateDialog::clearStatus()
{
	status_label->hide();
	status_label->clear();
}


void OnlineTemplateDialog::generateAndAccept()
{
	setStatus(tr("Preparing imagery template..."), Qt::gray, true);

	QString coverage_error;
	auto extent = selectedCoverageExtent(&coverage_error);
	if (!coverage_error.isEmpty())
	{
		setStatus(coverage_error, Qt::red, false);
		return;
	}

	auto result = OnlineImageryTemplateBuilder::generateXml(
		pending_source,
		effectiveTemplateName(pending_source),
		extent,
		map.getGeoreferencing(),
		map_path);
	if (!result.error.isEmpty())
	{
		setStatus(result.error, Qt::red, false);
		return;
	}

	if (result.area_km2 > OnlineImageryTemplateBuilder::area_warning_threshold_km2)
	{
		auto answer = QMessageBox::warning(
			this,
			tr("Large coverage area"),
			tr("This online template would cover about %1 km². Continue?")
				.arg(QString::number(result.area_km2, 'f', 0)),
			QMessageBox::Yes | QMessageBox::No,
			QMessageBox::No);
		if (answer != QMessageBox::Yes)
		{
			clearStatus();
			return;
		}
	}

	generated_path = result.xml_path;
	saveRecentSource(url_edit->text().trimmed(), pending_source.display_name);
	accept();
}


void OnlineTemplateDialog::populateSourceChooser()
{
	source_chooser->clear();
	source_chooser->addItem(tr("Choose a preset or recent source"));

	source_chooser->insertSeparator(source_chooser->count());
	auto osm_index = source_chooser->count();
	source_chooser->addItem(tr("OpenStreetMap"));
	source_chooser->setItemData(osm_index,
	                            QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"),
	                            source_url_role);
	source_chooser->setItemData(osm_index, tr("OpenStreetMap"), source_name_role);

	auto esri_index = source_chooser->count();
	source_chooser->addItem(tr("Esri World Imagery"));
	source_chooser->setItemData(esri_index,
	                            QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer"),
	                            source_url_role);
	source_chooser->setItemData(esri_index, tr("Esri World Imagery"), source_name_role);

	QSettings settings;
	auto recent_urls = settings.value(QStringLiteral("onlineImagery/recentUrls")).toStringList();
	auto recent_names = settings.value(QStringLiteral("onlineImagery/recentNames")).toStringList();
	if (!recent_urls.isEmpty())
	{
		source_chooser->insertSeparator(source_chooser->count());
		for (int i = 0; i < recent_urls.size(); ++i)
		{
			auto const name = (i < recent_names.size() && !recent_names[i].isEmpty())
			                  ? recent_names[i]
			                  : recent_urls[i];
			auto index = source_chooser->count();
			source_chooser->addItem(name);
			source_chooser->setItemData(index, recent_urls[i], source_url_role);
			source_chooser->setItemData(index, name, source_name_role);
		}
	}

	source_chooser->setCurrentIndex(0);
}


void OnlineTemplateDialog::onUrlEdited(const QString& text)
{
	Q_UNUSED(text)
	source_name_hint.clear();
	if (source_chooser->currentIndex() != 0)
		source_chooser->setCurrentIndex(0);
	setSuggestedTemplateName(inferredSourceNameForUrl());
}


void OnlineTemplateDialog::onSourceChosen(int index)
{
	auto url = source_chooser->itemData(index, source_url_role).toString();
	source_name_hint = source_chooser->itemData(index, source_name_role).toString();
	if (!url.isEmpty())
	{
		url_edit->setText(url);
		setSuggestedTemplateName(source_name_hint);
	}
}


void OnlineTemplateDialog::setSuggestedTemplateName(const QString& source_name)
{
	name_edit->setText(suggestedTemplateName(source_name));
}


QString OnlineTemplateDialog::mapDisplayName() const
{
	QFileInfo map_info(map_path);
	auto name = map_info.completeBaseName().trimmed();
	name.replace(QLatin1Char('_'), QLatin1Char(' '));
	name.replace(QLatin1Char('-'), QLatin1Char(' '));
	return name;
}


QString OnlineTemplateDialog::inferredSourceNameForUrl() const
{
	if (!source_name_hint.isEmpty())
		return source_name_hint;

	auto classify_result = OnlineImageryTemplateBuilder::classifyUrl(url_edit->text());
	return classify_result.error.isEmpty() ? classify_result.source.display_name : QString{};
}


QString OnlineTemplateDialog::suggestedTemplateName(const QString& source_name) const
{
	auto const trimmed_source_name = source_name.trimmed();
	auto const map_name = mapDisplayName();
	if (trimmed_source_name.isEmpty())
		return {};
	if (map_name.isEmpty())
		return trimmed_source_name;
	return tr("%1 - %2").arg(trimmed_source_name, map_name);
}


QString OnlineTemplateDialog::effectiveTemplateName(const OnlineImagerySource& source) const
{
	auto const trimmed_name = name_edit->text().trimmed();
	return trimmed_name.isEmpty() ? suggestedTemplateName(source.display_name) : trimmed_name;
}


QRectF OnlineTemplateDialog::selectedCoverageExtent(QString* error) const
{
	auto const coverage_mode = current_view_radio->isChecked()
	                           ? CoverageMode::CurrentView
	                           : CoverageMode::FullMap;
	switch (coverage_mode)
	{
	case CoverageMode::CurrentView:
		if (current_view_extent.isEmpty())
		{
			if (error)
				*error = tr("Could not determine the current view area.");
			return {};
		}
		return current_view_extent;

	case CoverageMode::FullMap:
	{
		auto extent = map.calculateExtent(false, false, nullptr);
		if (extent.isEmpty() && error)
			*error = tr("The map has no objects yet. Use Current View instead.");
		return extent;
	}
	}

	Q_UNREACHABLE();
}


void OnlineTemplateDialog::saveRecentSource(const QString& url, const QString& display_name)
{
	QSettings settings;
	auto urls = settings.value(QStringLiteral("onlineImagery/recentUrls")).toStringList();
	auto names = settings.value(QStringLiteral("onlineImagery/recentNames")).toStringList();

	auto existing = urls.indexOf(url);
	if (existing >= 0)
	{
		urls.removeAt(existing);
		if (existing < names.size())
			names.removeAt(existing);
	}

	urls.prepend(url);
	names.prepend(display_name);

	while (urls.size() > max_recent_sources)
		urls.removeLast();
	while (names.size() > max_recent_sources)
		names.removeLast();

	settings.setValue(QStringLiteral("onlineImagery/recentUrls"), urls);
	settings.setValue(QStringLiteral("onlineImagery/recentNames"), names);
}

#endif


}  // namespace OpenOrienteering
