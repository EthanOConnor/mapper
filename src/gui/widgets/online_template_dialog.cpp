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

#include <Qt>
#include <QComboBox>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRectF>
#include <QSettings>
#include <QStringList>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include "core/georeferencing.h"
#include "core/map.h"
#include "gdal/online_imagery_template_builder.h"
#include "gui/main_window.h"
#include "gui/map/map_editor.h"


namespace OpenOrienteering {

OnlineTemplateDialog::OnlineTemplateDialog(Map& map, MapEditorController& controller, QWidget* parent)
: QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint)
, map(map)
, controller(controller)
{
	setWindowTitle(tr("Add online imagery template"));
	setMinimumWidth(450);

	auto* layout = new QVBoxLayout(this);

	auto* url_label = new QLabel(tr("Paste an imagery link:"), this);
	layout->addWidget(url_label);

	url_edit = new QLineEdit(this);
	url_edit->setPlaceholderText(tr("https://tile.example.com/{z}/{x}/{y}.png"));
	layout->addWidget(url_edit);

	source_chooser = new QComboBox(this);
	populateSourceChooser();
	layout->addWidget(source_chooser);
	connect(source_chooser, QOverload<int>::of(&QComboBox::activated),
	        this, &OnlineTemplateDialog::onSourceChosen);

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
{
	// Abort any in-flight network request so the finished signal
	// doesn't fire after our members are destroyed.
	if (network)
	{
		disconnect(network, nullptr, this, nullptr);
		network->deleteLater();
		network = nullptr;
	}
}


void OnlineTemplateDialog::onAddClicked()
{
	clearStatus();

	// Precondition: map must be saved.
	auto map_path = controller.getWindow()->currentPath();
	if (map_path.isEmpty())
	{
		showError(tr("Save the map first to add online imagery."));
		return;
	}

	// Precondition: map must be georeferenced.
	if (map.getGeoreferencing().getState() != Georeferencing::Geospatial)
	{
		showError(tr("This map is not georeferenced yet. Online imagery needs a georeferenced map."));
		return;
	}

	// Precondition: map must have objects.
	auto extent = map.calculateExtent(false, false, nullptr);
	if (extent.isEmpty())
	{
		showError(tr("The map has no objects yet. Draw something first so the imagery area can be determined."));
		return;
	}

	// Classify the URL.
	auto classify_result = OnlineImageryTemplateBuilder::classifyUrl(url_edit->text());
	if (!classify_result.error.isEmpty())
	{
		showError(classify_result.error);
		return;
	}

	pending_source = classify_result.source;

	if (pending_source.kind == OnlineImagerySource::Kind::ArcGisTiledMapServer)
	{
		// Need to fetch metadata asynchronously.
		showProgress(tr("Checking imagery source..."));
		add_button->setEnabled(false);

		if (!network)
			network = new QNetworkAccessManager(this);

		auto metadata_url = QUrl(pending_source.normalized_url + QStringLiteral("?f=pjson"));
		auto request = QNetworkRequest(metadata_url);
		request.setTransferTimeout(15000);

		connect(network, &QNetworkAccessManager::finished,
		        this, &OnlineTemplateDialog::onMetadataReplyFinished);
		network->get(request);
		return;
	}

	// XYZ sources don't need metadata fetch — generate directly.
	generateAndAccept();
}


void OnlineTemplateDialog::onMetadataReplyFinished(QNetworkReply* reply)
{
	add_button->setEnabled(true);
	reply->deleteLater();

	// Disconnect to avoid double-handling if user clicks Add again.
	disconnect(network, &QNetworkAccessManager::finished,
	           this, &OnlineTemplateDialog::onMetadataReplyFinished);

	if (reply->error() != QNetworkReply::NoError)
	{
		showError(tr("Could not read imagery metadata: %1").arg(reply->errorString()));
		return;
	}

	auto data = reply->readAll();
	QJsonParseError parse_error;
	auto doc = QJsonDocument::fromJson(data, &parse_error);
	if (doc.isNull())
	{
		showError(tr("Could not read imagery metadata: invalid response from server."));
		return;
	}

	auto root = doc.object();

	// Require singleFusedMapCache for tiled access.
	if (!root.value(QStringLiteral("singleFusedMapCache")).toBool())
	{
		showError(tr("This ArcGIS service is not a cached tile service."));
		return;
	}

	// Extract tile info.
	auto tile_info = root.value(QStringLiteral("tileInfo")).toObject();
	int rows = tile_info.value(QStringLiteral("rows")).toInt(256);
	int cols = tile_info.value(QStringLiteral("cols")).toInt(256);
	pending_source.tile_size = QSize(cols, rows);

	auto origin = tile_info.value(QStringLiteral("origin")).toObject();
	pending_source.tile_origin = QPointF(
		origin.value(QStringLiteral("x")).toDouble(),
		origin.value(QStringLiteral("y")).toDouble());

	// Extract max LOD.
	auto lods = tile_info.value(QStringLiteral("lods")).toArray();
	int max_level = 0;
	for (auto const& lod : lods)
	{
		int level = lod.toObject().value(QStringLiteral("level")).toInt();
		if (level > max_level)
			max_level = level;
	}
	pending_source.max_tile_level = max_level;

	// Extract spatial reference.
	auto spatial_ref = root.value(QStringLiteral("spatialReference")).toObject();
	int wkid = spatial_ref.value(QStringLiteral("latestWkid")).toInt(
		spatial_ref.value(QStringLiteral("wkid")).toInt());
	if (wkid == 102100 || wkid == 3857)
		pending_source.crs_spec = QStringLiteral("EPSG:3857");
	else
	{
		showError(tr("Unsupported coordinate system (WKID %1). Only Web Mercator (3857) is supported.").arg(wkid));
		return;
	}

	// Extract full extent.
	auto full_ext = root.value(QStringLiteral("fullExtent")).toObject();
	pending_source.full_extent = QRectF(
		QPointF(full_ext.value(QStringLiteral("xmin")).toDouble(),
		        full_ext.value(QStringLiteral("ymax")).toDouble()),
		QPointF(full_ext.value(QStringLiteral("xmax")).toDouble(),
		        full_ext.value(QStringLiteral("ymin")).toDouble()));

	generateAndAccept();
}


void OnlineTemplateDialog::generateAndAccept()
{
	showProgress(tr("Preparing a map-sized template..."));

	auto map_path = controller.getWindow()->currentPath();
	auto extent = map.calculateExtent(false, false, nullptr);

	auto result = OnlineImageryTemplateBuilder::generateXml(
		pending_source, extent, map.getGeoreferencing(), map_path);

	if (!result.error.isEmpty())
	{
		showError(result.error);
		return;
	}

	// Warn if area is large.
	if (result.area_km2 > OnlineImageryTemplateBuilder::area_warning_threshold_km2)
	{
		auto answer = QMessageBox::warning(
			this,
			tr("Large coverage area"),
			tr("This online template would cover about %1 km² around the map. Continue?")
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
	saveRecentSource(pending_source.original_input, pending_source.display_name);
	accept();
}


void OnlineTemplateDialog::populateSourceChooser()
{
	source_chooser->clear();

	// Placeholder
	source_chooser->addItem(tr("Presets and recently used"), QString{});

	// Built-in presets
	source_chooser->insertSeparator(source_chooser->count());
	source_chooser->addItem(
		tr("OpenStreetMap"),
		QStringLiteral("https://tile.openstreetmap.org/{z}/{x}/{y}.png"));
	source_chooser->addItem(
		tr("Esri World Imagery"),
		QStringLiteral("https://server.arcgisonline.com/ArcGIS/rest/services/World_Imagery/MapServer/tile/{z}/{y}/{x}"));

	// Recent sources from settings
	QSettings settings;
	auto recent_urls = settings.value(QStringLiteral("onlineImagery/recentUrls")).toStringList();
	auto recent_names = settings.value(QStringLiteral("onlineImagery/recentNames")).toStringList();
	if (!recent_urls.isEmpty())
	{
		source_chooser->insertSeparator(source_chooser->count());
		for (int i = 0; i < recent_urls.size(); ++i)
		{
			auto name = (i < recent_names.size() && !recent_names[i].isEmpty())
			            ? recent_names[i] : recent_urls[i];
			source_chooser->addItem(name, recent_urls[i]);
		}
	}
}


void OnlineTemplateDialog::onSourceChosen(int index)
{
	auto url = source_chooser->itemData(index).toString();
	if (!url.isEmpty())
		url_edit->setText(url);
	// Reset to placeholder so re-selecting the same item works
	source_chooser->setCurrentIndex(0);
}


// static
void OnlineTemplateDialog::saveRecentSource(const QString& url, const QString& display_name)
{
	constexpr int max_recent = 5;
	QSettings settings;
	auto urls = settings.value(QStringLiteral("onlineImagery/recentUrls")).toStringList();
	auto names = settings.value(QStringLiteral("onlineImagery/recentNames")).toStringList();

	// Remove duplicate if already present
	int existing = urls.indexOf(url);
	if (existing >= 0)
	{
		urls.removeAt(existing);
		if (existing < names.size())
			names.removeAt(existing);
	}

	// Prepend
	urls.prepend(url);
	names.prepend(display_name);

	// Trim
	while (urls.size() > max_recent)
		urls.removeLast();
	while (names.size() > max_recent)
		names.removeLast();

	settings.setValue(QStringLiteral("onlineImagery/recentUrls"), urls);
	settings.setValue(QStringLiteral("onlineImagery/recentNames"), names);
}


void OnlineTemplateDialog::showError(const QString& message)
{
	status_label->setText(message);
	status_label->setStyleSheet(QStringLiteral("color: red;"));
	status_label->show();
}

void OnlineTemplateDialog::showProgress(const QString& message)
{
	status_label->setText(message);
	status_label->setStyleSheet(QStringLiteral("color: gray; font-style: italic;"));
	status_label->show();
}

void OnlineTemplateDialog::clearStatus()
{
	status_label->hide();
	status_label->clear();
}


}  // namespace OpenOrienteering
