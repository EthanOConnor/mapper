/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    any later version.
 */

#include "gui/imagery/online_imagery_dialog.h"

#include <algorithm>

#include <QComboBox>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSet>
#include <QSortFilterProxyModel>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTimer>
#include <QTreeView>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/file_dialog.h"
#include "gui/imagery/catalog_import_dialog.h"
#include "gui/imagery/catalog_manager_dialog.h"
#include "gui/imagery/imagery_network_permissions_dialog.h"
#include "gui/imagery/imagery_source_model.h"
#include "imagery/imagery_network_permissions.h"
#include "imagery/tile_network_manager.h"

namespace OpenOrienteering {

namespace {

QString sourceUrlProbe(QString value)
{
	value.replace(QStringLiteral("${z}"), QStringLiteral("0"));
	value.replace(QStringLiteral("${x}"), QStringLiteral("0"));
	value.replace(QStringLiteral("${y}"), QStringLiteral("0"));
	value.replace(QStringLiteral("{z}"), QStringLiteral("0"));
	value.replace(QStringLiteral("{x}"), QStringLiteral("0"));
	value.replace(QStringLiteral("{y}"), QStringLiteral("0"));
	return value;
}

QStringList sourceHosts(
	const imagery::ResolvedImagerySource& source)
{
	QSet<QString> hosts;
	for (auto const& tile : source.tile_urls)
	{
		auto const host =
			QUrl(sourceUrlProbe(tile.value)).host().toLower();
		if (!host.isEmpty())
			hosts.insert(host);
	}
	auto result = hosts.values();
	std::sort(result.begin(), result.end());
	return result;
}

bool potentiallyPrivate(const QUrl& url)
{
	auto host = url.host().toLower();
	QHostAddress address;
	if (address.setAddress(host))
		return !imagery::TileNetworkManager::
			isPublicDestinationAddress(address);
	return host == QLatin1String("localhost")
	       || host.endsWith(QLatin1String(".localhost"))
	       || host.endsWith(QLatin1String(".local"))
	       || host.endsWith(QLatin1String(".internal"))
	       || host.endsWith(QLatin1String(".home.arpa"));
}

QString dateRange(const imagery::ImageryMetadata& metadata)
{
	if (metadata.start_date.isValid()
	    && metadata.end_date.isValid())
	{
		return OnlineImageryDialog::tr("%1 to %2")
			.arg(
				QLocale().toString(
					metadata.start_date,
					QLocale::ShortFormat),
				QLocale().toString(
					metadata.end_date,
					QLocale::ShortFormat));
	}
	if (metadata.start_date.isValid())
	{
		return OnlineImageryDialog::tr("From %1")
			.arg(QLocale().toString(
				metadata.start_date,
				QLocale::ShortFormat));
	}
	if (metadata.end_date.isValid())
	{
		return OnlineImageryDialog::tr("Through %1")
			.arg(QLocale().toString(
				metadata.end_date,
				QLocale::ShortFormat));
	}
	return OnlineImageryDialog::tr("Not specified");
}

}  // namespace


OnlineImageryDialog::OnlineImageryDialog(
	imagery::ImageryCatalogRepository& repository,
	imagery::TileNetworkManager& network,
	QWidget* parent,
	imagery::ImageryNetworkPermissions* permissions)
 : QDialog(parent)
 , repository_(repository)
 , network_(network)
 , network_client_id_(
	   imagery::TileNetworkManager::nextClientId())
{
	setWindowTitle(tr("Add online imagery"));
	resize(940, 640);
	permissions_ = permissions
		? permissions
		: new imagery::ImageryNetworkPermissions(network_, this);

	source_model_ = new ImagerySourceModel(repository_, this);
	source_filter_ = new QSortFilterProxyModel(this);
	source_filter_->setSourceModel(source_model_);
	source_filter_->setFilterRole(
		ImagerySourceModel::SearchTextRole);
	source_filter_->setFilterCaseSensitivity(
		Qt::CaseInsensitive);
	source_filter_->setRecursiveFilteringEnabled(true);
	source_filter_->setAutoAcceptChildRows(true);

	search_ = new QLineEdit(this);
	search_->setObjectName(QStringLiteral("imagery_search"));
	search_->setPlaceholderText(tr("Search imagery sources"));
	search_->setClearButtonEnabled(true);
	source_tree_ = new QTreeView(this);
	source_tree_->setObjectName(QStringLiteral("imagery_source_tree"));
	source_tree_->setModel(source_filter_);
	source_tree_->setHeaderHidden(true);
	source_tree_->setUniformRowHeights(true);
	source_tree_->setSelectionMode(
		QAbstractItemView::SingleSelection);
	source_tree_->expandAll();

	auto* manual_button = new QPushButton(
		tr("Enter a tile or service URL…"),
		this);
	manual_button->setObjectName(QStringLiteral("manual_source_button"));
	auto* import_button = new QPushButton(
		tr("Import catalog…"),
		this);
	auto* manage_button = new QPushButton(
		tr("Manage catalogs…"),
		this);
	auto* permissions_button = new QPushButton(
		tr("Network access…"),
		this);
	auto* left_actions = new QHBoxLayout();
	left_actions->addWidget(import_button);
	left_actions->addWidget(manage_button);
	left_actions->addWidget(permissions_button);

	auto* left = new QWidget(this);
	auto* left_layout = new QVBoxLayout(left);
	left_layout->setContentsMargins(0, 0, 0, 0);
	left_layout->addWidget(search_);
	left_layout->addWidget(source_tree_, 1);
	left_layout->addWidget(manual_button);
	left_layout->addLayout(left_actions);

	pages_ = new QStackedWidget(this);
	auto* catalog_page = new QWidget(this);
	auto* catalog_layout = new QVBoxLayout(catalog_page);
	detail_title_ = new QLabel(catalog_page);
	detail_title_->setObjectName(
		QStringLiteral("imagery_detail_title"));
	detail_title_->setTextFormat(Qt::PlainText);
	auto title_font = detail_title_->font();
	title_font.setPointSizeF(title_font.pointSizeF() * 1.25);
	title_font.setBold(true);
	detail_title_->setFont(title_font);
	detail_description_ = new QLabel(catalog_page);
	detail_description_->setObjectName(
		QStringLiteral("imagery_detail_description"));
	detail_description_->setTextFormat(Qt::PlainText);
	detail_description_->setWordWrap(true);
	auto* detail_form = new QFormLayout();
	detail_status_ = new QLabel(catalog_page);
	detail_catalog_ = new QLabel(catalog_page);
	detail_dates_ = new QLabel(catalog_page);
	detail_crs_ = new QLabel(catalog_page);
	detail_hosts_ = new QLabel(catalog_page);
	detail_attribution_ = new QLabel(catalog_page);
	detail_terms_ = new QLabel(catalog_page);
	for (auto* label : {
		     detail_status_, detail_catalog_, detail_dates_,
		     detail_crs_, detail_hosts_, detail_attribution_,
		     detail_terms_ })
	{
		label->setTextFormat(Qt::PlainText);
		label->setWordWrap(true);
		label->setTextInteractionFlags(
			Qt::TextSelectableByMouse);
	}
	detail_form->addRow(tr("Status:"), detail_status_);
	detail_form->addRow(tr("Catalog:"), detail_catalog_);
	detail_form->addRow(tr("Imagery dates:"), detail_dates_);
	detail_form->addRow(tr("Grid:"), detail_crs_);
	detail_form->addRow(tr("Request hosts:"), detail_hosts_);
	detail_form->addRow(tr("Attribution:"), detail_attribution_);
	detail_form->addRow(tr("Terms:"), detail_terms_);
	catalog_layout->addWidget(detail_title_);
	catalog_layout->addWidget(detail_description_);
	catalog_layout->addLayout(detail_form);
	catalog_layout->addStretch();
	pages_->addWidget(catalog_page);

	auto* manual_page = new QWidget(this);
	auto* manual_layout = new QVBoxLayout(manual_page);
	auto* manual_intro = new QLabel(
		tr("Enter an XYZ or TMS tile template. ArcGIS REST services "
		   "are resolved from their published metadata before use."),
		manual_page);
	manual_intro->setWordWrap(true);
	manual_url_ = new QLineEdit(manual_page);
	manual_url_->setObjectName(QStringLiteral("manual_imagery_url"));
	manual_url_->setPlaceholderText(
		QStringLiteral("https://tiles.example.org/{z}/{x}/{y}.png"));
	auto* manual_form = new QFormLayout();
	manual_form->addRow(tr("URL:"), manual_url_);

	auto* advanced = new QGroupBox(
		tr("Advanced tile settings"),
		manual_page);
	auto* advanced_form = new QFormLayout(advanced);
	manual_scheme_ = new QComboBox(advanced);
	manual_scheme_->setObjectName(QStringLiteral("manual_row_scheme"));
	manual_scheme_->addItem(
		tr("XYZ (top-origin rows)"),
		int(imagery::TileRowScheme::Xyz));
	manual_scheme_->addItem(
		tr("TMS (bottom-origin rows)"),
		int(imagery::TileRowScheme::Tms));
	manual_min_zoom_ = new QSpinBox(advanced);
	manual_min_zoom_->setObjectName(QStringLiteral("manual_min_zoom"));
	manual_min_zoom_->setRange(
		0,
		imagery::ManualImagerySource::maximum_zoom);
	manual_max_zoom_ = new QSpinBox(advanced);
	manual_max_zoom_->setObjectName(QStringLiteral("manual_max_zoom"));
	manual_max_zoom_->setRange(
		0,
		imagery::ManualImagerySource::maximum_zoom);
	manual_max_zoom_->setValue(19);
	auto* zooms = new QWidget(advanced);
	auto* zoom_layout = new QHBoxLayout(zooms);
	zoom_layout->setContentsMargins(0, 0, 0, 0);
	zoom_layout->addWidget(manual_min_zoom_);
	zoom_layout->addWidget(new QLabel(tr("to"), zooms));
	zoom_layout->addWidget(manual_max_zoom_);
	manual_tile_size_ = new QComboBox(advanced);
	manual_tile_size_->setObjectName(QStringLiteral("manual_tile_size"));
	manual_tile_size_->addItem(tr("256 × 256 pixels"), 256);
	manual_tile_size_->addItem(tr("512 × 512 pixels"), 512);
	manual_referer_ = new QLineEdit(advanced);
	manual_referer_->setObjectName(QStringLiteral("manual_referer"));
	manual_referer_->setPlaceholderText(tr("None"));
	manual_empty_statuses_ = new QLineEdit(advanced);
	manual_empty_statuses_->setObjectName(
		QStringLiteral("manual_empty_statuses"));
	manual_empty_statuses_->setText(QStringLiteral("204, 404"));
	manual_attribution_ = new QLineEdit(advanced);
	manual_attribution_url_ = new QLineEdit(advanced);
	advanced_form->addRow(tr("Row scheme:"), manual_scheme_);
	advanced_form->addRow(tr("Zoom range:"), zooms);
	advanced_form->addRow(tr("Tile size:"), manual_tile_size_);
	advanced_form->addRow(tr("HTTP Referer:"), manual_referer_);
	advanced_form->addRow(
		tr("Empty HTTP statuses:"),
		manual_empty_statuses_);
	advanced_form->addRow(
		tr("Attribution text:"),
		manual_attribution_);
	advanced_form->addRow(
		tr("Attribution URL:"),
		manual_attribution_url_);

	discover_button_ = new QPushButton(
		tr("Read service metadata"),
		manual_page);
	discover_button_->setObjectName(
		QStringLiteral("discover_imagery_service"));
	discover_button_->hide();
	approve_private_button_ = new QPushButton(
		tr("Allow this local-network origin and retry…"),
		manual_page);
	approve_private_button_->hide();
	auto* discovery_actions = new QHBoxLayout();
	discovery_actions->addWidget(discover_button_);
	discovery_actions->addWidget(approve_private_button_);
	discovery_actions->addStretch();
	manual_layout->addWidget(manual_intro);
	manual_layout->addLayout(manual_form);
	manual_layout->addWidget(advanced);
	manual_layout->addLayout(discovery_actions);
	manual_layout->addStretch();
	pages_->addWidget(manual_page);

	auto* splitter = new QSplitter(this);
	splitter->addWidget(left);
	splitter->addWidget(pages_);
	splitter->setStretchFactor(0, 2);
	splitter->setStretchFactor(1, 3);

	display_name_ = new QLineEdit(this);
	display_name_->setObjectName(QStringLiteral("imagery_display_name"));
	auto* name_layout = new QFormLayout();
	name_layout->addRow(tr("Template name:"), display_name_);

	status_icon_ = new QLabel(this);
	status_icon_->setAccessibleName(
		tr("Imagery source status"));
	status_text_ = new QLabel(this);
	status_text_->setObjectName(
		QStringLiteral("imagery_status_text"));
	status_text_->setTextFormat(Qt::PlainText);
	status_text_->setWordWrap(true);
	auto* status_layout = new QHBoxLayout();
	status_layout->addWidget(status_icon_, 0, Qt::AlignTop);
	status_layout->addWidget(status_text_, 1);

	buttons_ = new QDialogButtonBox(
		QDialogButtonBox::Cancel,
		this);
	add_button_ = buttons_->addButton(
		tr("Add"),
		QDialogButtonBox::AcceptRole);
	add_button_->setObjectName(QStringLiteral("add_online_imagery"));
	add_button_->setEnabled(false);

	auto* layout = new QVBoxLayout(this);
	layout->addWidget(splitter, 1);
	layout->addLayout(name_layout);
	layout->addLayout(status_layout);
	layout->addWidget(buttons_);

	classify_timer_ = new QTimer(this);
	classify_timer_->setSingleShot(true);
	classify_timer_->setInterval(300);

	connect(
		search_,
		&QLineEdit::textChanged,
		source_filter_,
		&QSortFilterProxyModel::setFilterFixedString);
	connect(
		source_tree_->selectionModel(),
		&QItemSelectionModel::currentChanged,
		this,
		[this](const QModelIndex& current) {
			selectCatalogIndex(current);
		});
	connect(
		source_tree_,
		&QTreeView::doubleClicked,
		this,
		[this](const QModelIndex& index) {
			selectCatalogIndex(index);
			if (selected_source_)
				acceptSelection();
		});
	connect(
		manual_button,
		&QPushButton::clicked,
		this,
		&OnlineImageryDialog::showManualPage);
	connect(
		import_button,
		&QPushButton::clicked,
		this,
		&OnlineImageryDialog::importCatalog);
	connect(
		manage_button,
		&QPushButton::clicked,
		this,
		&OnlineImageryDialog::manageCatalogs);
	connect(
		permissions_button,
		&QPushButton::clicked,
		this,
		&OnlineImageryDialog::manageNetworkPermissions);
	connect(
		classify_timer_,
		&QTimer::timeout,
		this,
		&OnlineImageryDialog::classifyManual);
	connect(
		discover_button_,
		&QPushButton::clicked,
		this,
		&OnlineImageryDialog::discoverArcGis);
	connect(
		approve_private_button_,
		&QPushButton::clicked,
		this,
		&OnlineImageryDialog::approvePrivateDiscoveryOrigin);
	connect(
		&network_,
		&imagery::TileNetworkManager::finished,
		this,
		&OnlineImageryDialog::onNetworkFinished);
	connect(
		&repository_,
		&imagery::ImageryCatalogRepository::snapshotChanged,
		this,
		&OnlineImageryDialog::updateCatalogSelectionAfterReload);
	connect(
		buttons_,
		&QDialogButtonBox::rejected,
		this,
		&QDialog::reject);
	connect(
		add_button_,
		&QPushButton::clicked,
		this,
		&OnlineImageryDialog::acceptSelection);

	connect(
		manual_url_,
		&QLineEdit::textEdited,
		this,
		&OnlineImageryDialog::scheduleManualClassification);
	for (auto* edit : {
		     manual_referer_, manual_empty_statuses_,
		     manual_attribution_, manual_attribution_url_ })
	{
		connect(
			edit,
			&QLineEdit::textEdited,
			this,
			&OnlineImageryDialog::scheduleManualClassification);
	}
	for (auto* combo : { manual_scheme_, manual_tile_size_ })
	{
		connect(
			combo,
			qOverload<int>(&QComboBox::currentIndexChanged),
			this,
			&OnlineImageryDialog::scheduleManualClassification);
	}
	for (auto* spin : { manual_min_zoom_, manual_max_zoom_ })
	{
		connect(
			spin,
			qOverload<int>(&QSpinBox::valueChanged),
			this,
			&OnlineImageryDialog::scheduleManualClassification);
	}

	if (source_model_->rowCount() > 0)
	{
		source_tree_->setCurrentIndex(
			source_filter_->index(0, 0));
	}
	else
	{
		showManualPage();
	}
}


OnlineImageryDialog::~OnlineImageryDialog()
{
	if (discovery_token_)
		network_.cancel(discovery_token_);
	network_.cancelClient(network_client_id_);
}


const imagery::ResolvedImagerySource&
OnlineImageryDialog::selectedSource() const
{
	Q_ASSERT(selected_source_);
	return *selected_source_;
}


QString OnlineImageryDialog::displayName() const
{
	return display_name_->text().trimmed();
}


void OnlineImageryDialog::selectCatalogIndex(
	const QModelIndex& proxy_index)
{
	auto const source_index =
		source_filter_->mapToSource(proxy_index);
	auto const handle =
		source_model_->sourceHandle(source_index);
	if (!handle)
	{
		cancelManualDiscovery();
		pages_->setCurrentIndex(0);
		clearSelectedSource();
		return;
	}
	showCatalogSource(*handle);
}


void OnlineImageryDialog::cancelManualDiscovery()
{
	classify_timer_->stop();
	if (discovery_token_)
	{
		network_.cancel(discovery_token_);
		discovery_token_ = 0;
	}
	++discovery_generation_;
	insecure_discovery_consent_.clear();
	private_discovery_approval_url_.clear();
	discover_button_->setEnabled(true);
	discover_button_->hide();
	approve_private_button_->hide();
}


void OnlineImageryDialog::showCatalogSource(
	const imagery::ImagerySourceHandle& handle)
{
	cancelManualDiscovery();
	pages_->setCurrentIndex(0);
	auto const snapshot = repository_.snapshot();
	const imagery::InstalledImageryCatalog* installed = nullptr;
	auto const* definition = snapshot
		? snapshot->source(handle, &installed)
		: nullptr;
	if (!definition || !installed)
	{
		clearSelectedSource();
		setStatus(
			tr("This source is no longer available."),
			QStyle::SP_MessageBoxWarning);
		return;
	}
	selected_handle_ = handle;
	detail_title_->setText(
		definition->metadata.name.isEmpty()
		    && definition->metadata.id.isEmpty()
			? tr("Invalid source %1").arg(handle.source_index + 1)
			: definition->metadata.name.isEmpty()
			? definition->metadata.id
			: definition->metadata.name);
	detail_description_->setText(
		definition->metadata.description.isEmpty()
			? tr("No description was provided.")
			: definition->metadata.description);
	detail_catalog_->setText(
		tr("%1 · revision %2")
			.arg(
				installed->read_result.catalog.name.isEmpty()
					? installed->read_result.catalog.id
					: installed->read_result.catalog.name,
				QString::number(
					installed->read_result.catalog.revision)));
	detail_dates_->setText(dateRange(definition->metadata));
	detail_attribution_->setText(
		definition->notices.attribution_text.isEmpty()
			? tr("Not specified")
			: definition->notices.attribution_text);
	detail_terms_->setText(
		definition->notices.terms_url.isEmpty()
			? tr("Not specified")
			: definition->notices.terms_url.toDisplayString());

	if (definition->resolved_source)
	{
		auto const& source = *definition->resolved_source;
		detail_status_->setText(tr("Ready"));
		detail_crs_->setText(
			tr("%1 · zoom %2–%3")
				.arg(source.tile_matrix_set.crs)
				.arg(source.min_zoom)
				.arg(source.max_zoom));
		detail_hosts_->setText(
			sourceHosts(source).join(QLatin1Char('\n')));
		setSelectedSource(
			source,
			definition->metadata.name);
		setStatus(
			tr("Review the source details, then add an immutable "
			   "snapshot to this map."),
			QStyle::SP_MessageBoxInformation);
	}
	else
	{
		detail_status_->setText(tr("Not supported"));
		detail_crs_->setText(tr("Unavailable"));
		detail_hosts_->clear();
		clearSelectedSource();
		QStringList reasons =
			definition->unsupported_capabilities;
		auto const source_index = int(
			definition
			- installed->read_result.catalog.sources.data());
		for (auto const& diagnostic
		     : installed->read_result.diagnostics)
		{
			if (diagnostic.source_index == source_index
			    && (diagnostic.kind
			          == imagery::OicDiagnosticKind::UnsupportedSource
			        || diagnostic.kind
			          == imagery::OicDiagnosticKind::SourceError))
				reasons.push_back(diagnostic.message);
		}
		reasons.removeDuplicates();
		setStatus(
			reasons.isEmpty()
				? tr("This catalog source is not supported by this build.")
				: reasons.join(QLatin1Char('\n')),
			QStyle::SP_MessageBoxWarning);
	}
}


void OnlineImageryDialog::showManualPage()
{
	pages_->setCurrentIndex(1);
	source_tree_->clearSelection();
	selected_handle_.reset();
	clearSelectedSource();
	manual_url_->setFocus();
	classifyManual();
}


imagery::ManualTiledSourceSettings
OnlineImageryDialog::manualSettings(
	QString* error) const
{
	imagery::ManualTiledSourceSettings settings;
	settings.scheme = imagery::TileRowScheme(
		manual_scheme_->currentData().toInt());
	settings.min_zoom = manual_min_zoom_->value();
	settings.max_zoom = manual_max_zoom_->value();
	settings.tile_size =
		manual_tile_size_->currentData().toInt();
	settings.referer =
		QUrl(manual_referer_->text().trimmed());
	settings.attribution_text =
		manual_attribution_->text().trimmed();
	settings.attribution_url =
		QUrl(manual_attribution_url_->text().trimmed());
	settings.empty_http_status_codes.clear();
	QSet<int> seen;
	for (auto const& item :
	     manual_empty_statuses_->text().split(
		     QRegularExpression(QStringLiteral("[,\\s]+")),
		     Qt::SkipEmptyParts))
	{
		bool ok = false;
		auto const status = item.toInt(&ok);
		if (!ok || status < 100 || status > 599
		    || seen.contains(status))
		{
			if (error)
			{
				*error = tr(
					"Empty HTTP statuses must be unique numbers "
					"between 100 and 599.");
			}
			return {};
		}
		seen.insert(status);
		settings.empty_http_status_codes.push_back(status);
	}
	return settings;
}


void OnlineImageryDialog::scheduleManualClassification()
{
	if (pages_->currentIndex() != 1)
		pages_->setCurrentIndex(1);
	if (discovery_token_)
	{
		network_.cancel(discovery_token_);
		discovery_token_ = 0;
	}
	++discovery_generation_;
	insecure_discovery_consent_.clear();
	private_discovery_approval_url_.clear();
	classify_timer_->start();
	clearSelectedSource();
	discover_button_->hide();
	approve_private_button_->hide();
}


void OnlineImageryDialog::classifyManual()
{
	QString settings_error;
	auto const settings = manualSettings(&settings_error);
	if (!settings_error.isEmpty())
	{
		manual_result_ = {};
		clearSelectedSource();
		setStatus(
			settings_error,
			QStyle::SP_MessageBoxWarning);
		return;
	}
	manual_result_ = imagery::ManualImagerySource::classify(
		manual_url_->text(),
		settings);
	discover_button_->setVisible(
		manual_result_.outcome
		== imagery::ManualImageryOutcome::NeedsDiscovery);
	approve_private_button_->hide();
	switch (manual_result_.outcome)
	{
	case imagery::ManualImageryOutcome::Direct:
		setSelectedSource(
			*manual_result_.source,
			manual_result_.suggested_name);
		setStatus(
			manual_result_.warnings.isEmpty()
				? tr("Recognized a direct tiled source.")
				: tr("Recognized a tiled source. Its URL contains "
				     "credential-like query parameters that will be "
				     "embedded in the map."),
			manual_result_.warnings.isEmpty()
				? QStyle::SP_DialogApplyButton
				: QStyle::SP_MessageBoxWarning);
		break;
	case imagery::ManualImageryOutcome::NeedsDiscovery:
		clearSelectedSource();
		if (display_name_->text().trimmed().isEmpty())
			display_name_->setText(
				manual_result_.suggested_name);
		setStatus(
			tr("This service must publish a cached, dyadic tile "
			   "scheme. Read its metadata to verify it."),
			QStyle::SP_MessageBoxInformation);
		break;
	case imagery::ManualImageryOutcome::Unsupported:
		clearSelectedSource();
		setStatus(
			manual_result_.detail,
			QStyle::SP_MessageBoxWarning);
		break;
	case imagery::ManualImageryOutcome::Invalid:
		clearSelectedSource();
		setStatus(
			manual_result_.detail,
			manual_url_->text().isEmpty()
				? QStyle::SP_MessageBoxInformation
				: QStyle::SP_MessageBoxWarning);
		break;
	}
}


imagery::TileNetworkRequest
OnlineImageryDialog::arcGisDiscoveryRequest(
	quint64 generation) const
{
	imagery::TileNetworkRequest request;
	request.url = manual_result_.discovery_url;
	request.client_id = network_client_id_;
	request.generation = generation;
	request.priority = imagery::TileRequestPriority::Visible;
	request.payload_kind = imagery::NetworkPayloadKind::JsonDocument;
	request.referer = QUrl(
		manual_referer_->text().trimmed())
		.toString(QUrl::FullyEncoded);
	request.empty_http_status_codes.clear();
	request.max_response_bytes =
		imagery::ArcGisTileService::maximum_metadata_size;
	return request;
}


void OnlineImageryDialog::discoverArcGis()
{
	if (manual_result_.outcome
	    != imagery::ManualImageryOutcome::NeedsDiscovery)
		return;
	if (manual_result_.discovery_url.scheme().toLower()
	      == QLatin1String("http")
	    && insecure_discovery_consent_
	         != manual_result_.discovery_url)
	{
		auto const answer = QMessageBox::warning(
			this,
			tr("Unencrypted service metadata"),
			tr("This service publishes metadata over plain HTTP. "
			   "Continue for this request?"),
			QMessageBox::Yes | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (answer != QMessageBox::Yes)
			return;
		insecure_discovery_consent_ =
			manual_result_.discovery_url;
	}
	if (potentiallyPrivate(manual_result_.discovery_url)
	    && !permissions_->isApproved(
		    manual_result_.discovery_url))
	{
		approve_private_button_->show();
		setStatus(
			tr("This service is on a local or private network. "
			   "It requires an installation-local approval."),
			QStyle::SP_MessageBoxWarning);
		return;
	}
	if (discovery_token_)
		network_.cancel(discovery_token_);
	++discovery_generation_;
	discovery_token_ = network_.submit(
		arcGisDiscoveryRequest(discovery_generation_));
	discover_button_->setEnabled(false);
	approve_private_button_->hide();
	setStatus(
		tr("Reading ArcGIS service metadata…"),
		QStyle::SP_BrowserReload);
}


void OnlineImageryDialog::onNetworkFinished(
	imagery::TileNetworkManager::Token token,
	const imagery::TileNetworkResult& result)
{
	if (!discovery_token_ || token != discovery_token_
	    || result.client_id != network_client_id_
	    || result.generation != discovery_generation_
	    || pages_->currentIndex() != 1
	    || manual_result_.outcome
	         != imagery::ManualImageryOutcome::NeedsDiscovery)
		return;
	discovery_token_ = 0;
	discover_button_->setEnabled(true);
	if (result.outcome
	    != imagery::TileNetworkResult::Outcome::Success)
	{
		clearSelectedSource();
		if (result.private_network_rejected
		    && !result.private_network_rejected_url.isEmpty()
		    && !permissions_->isApproved(
			    result.private_network_rejected_url))
		{
			private_discovery_approval_url_ =
				result.private_network_rejected_url;
			approve_private_button_->show();
		}
		setStatus(
			result.error_string.isEmpty()
				? tr("Could not read the service metadata.")
				: result.error_string,
			QStyle::SP_MessageBoxWarning);
		return;
	}
	private_discovery_approval_url_.clear();

	QString settings_error;
	auto const manual_settings = manualSettings(&settings_error);
	if (!settings_error.isEmpty())
	{
		setStatus(
			settings_error,
			QStyle::SP_MessageBoxWarning);
		return;
	}
	imagery::ArcGisTileServiceSettings settings;
	settings.name = display_name_->text().trimmed();
	settings.referer = manual_settings.referer;
	settings.empty_http_status_codes =
		manual_settings.empty_http_status_codes;
	settings.attribution_text =
		manual_settings.attribution_text;
	settings.attribution_url =
		manual_settings.attribution_url;
	auto const service_url =
		result.final_url.isValid() && !result.final_url.isEmpty()
			? result.final_url
			: manual_result_.service_url;
	auto const parsed = imagery::ArcGisTileService::parse(
		result.body,
		service_url,
		settings);
	if (!parsed.resolved())
	{
		clearSelectedSource();
		setStatus(
			parsed.detail,
			parsed.outcome
			      == imagery::ArcGisTileServiceOutcome::Unsupported
				? QStyle::SP_MessageBoxWarning
				: QStyle::SP_MessageBoxCritical);
		return;
	}
	setSelectedSource(
		*parsed.source,
		parsed.service_title);
	setStatus(
		parsed.likely_secret_parameters.isEmpty()
			? tr("Resolved a cached ArcGIS tile service.")
			: tr("Resolved the service. Its endpoint contains "
			     "credential-like query parameters that will be "
			     "embedded in the map."),
		parsed.likely_secret_parameters.isEmpty()
			? QStyle::SP_DialogApplyButton
			: QStyle::SP_MessageBoxWarning);
}


void OnlineImageryDialog::approvePrivateDiscoveryOrigin()
{
	auto const approval_url =
		private_discovery_approval_url_.isEmpty()
			? manual_result_.discovery_url
			: private_discovery_approval_url_;
	auto const origin = imagery::TileNetworkManager::canonicalOrigin(
		approval_url);
	auto const answer = QMessageBox::warning(
		this,
		tr("Allow local-network imagery"),
		tr("Allow Mapper to contact %1 from this installation?\n\n"
		   "This permission is stored only on this device. Catalogs "
		   "and map files cannot grant it.")
			.arg(origin),
		QMessageBox::Yes | QMessageBox::Cancel,
		QMessageBox::Cancel);
	if (answer != QMessageBox::Yes)
		return;
	if (permissions_->approve(approval_url))
	{
		private_discovery_approval_url_.clear();
		discoverArcGis();
	}
}


void OnlineImageryDialog::setSelectedSource(
	imagery::ResolvedImagerySource source,
	const QString& suggested_name)
{
	selected_source_ = std::move(source);
	if (!suggested_name.trimmed().isEmpty())
		display_name_->setText(suggested_name.trimmed());
	add_button_->setEnabled(true);
}


void OnlineImageryDialog::clearSelectedSource()
{
	selected_source_.reset();
	add_button_->setEnabled(false);
}


void OnlineImageryDialog::updateCatalogSelectionAfterReload()
{
	source_tree_->expandAll();
	if (!selected_handle_)
		return;
	auto const snapshot = repository_.snapshot();
	if (!snapshot)
	{
		clearSelectedSource();
		return;
	}
	if (selected_handle_->source_id.isEmpty())
	{
		auto const source_index =
			source_model_->indexForHandle(*selected_handle_);
		auto const proxy_index =
			source_filter_->mapFromSource(source_index);
		if (proxy_index.isValid()
		    && snapshot->source(*selected_handle_))
		{
			source_tree_->setCurrentIndex(proxy_index);
			showCatalogSource(*selected_handle_);
			return;
		}
		clearSelectedSource();
		setStatus(
			tr("The selected invalid source is no longer available."),
			QStyle::SP_MessageBoxWarning);
		return;
	}
	auto const latest = snapshot->latestHandle(
		selected_handle_->catalog_id,
		selected_handle_->source_id);
	if (!latest)
	{
		clearSelectedSource();
		setStatus(
			tr("The selected source was removed by a catalog update."),
			QStyle::SP_MessageBoxWarning);
		return;
	}
	auto const changed = *latest != *selected_handle_;
	selected_handle_ = *latest;
	auto const source_index =
		source_model_->indexForHandle(*latest);
	auto const proxy_index =
		source_filter_->mapFromSource(source_index);
	if (proxy_index.isValid())
		source_tree_->setCurrentIndex(proxy_index);
	showCatalogSource(*latest);
	if (changed)
	{
		setStatus(
			tr("The catalog changed while this dialog was open. "
			   "Review the updated source before adding it."),
			QStyle::SP_MessageBoxWarning);
	}
}


void OnlineImageryDialog::importCatalog()
{
	auto const path = FileDialog::getOpenFileName(
		this,
		tr("Import imagery catalog"),
		QString {},
		tr("Imagery catalogs (*.oic);;All files (*.*)"));
	if (path.isEmpty())
		return;
	CatalogImportDialog dialog(repository_, this, permissions_);
	dialog.startFile(path);
	dialog.exec();
}


void OnlineImageryDialog::manageCatalogs()
{
	CatalogManagerDialog dialog(repository_, this, permissions_);
	dialog.exec();
}


void OnlineImageryDialog::manageNetworkPermissions()
{
	ImageryNetworkPermissionsDialog dialog(
		*permissions_,
		this);
	dialog.exec();
}


QVector<QUrl> OnlineImageryDialog::selectedRequestUrls() const
{
	QVector<QUrl> urls;
	if (!selected_source_)
		return urls;
	for (auto const& tile : selected_source_->tile_urls)
		urls.push_back(QUrl(sourceUrlProbe(tile.value)));
	return urls;
}


void OnlineImageryDialog::acceptSelection()
{
	if (!selected_source_)
		return;
	auto const urls = selectedRequestUrls();
	bool insecure = false;
	QStringList secret_parameters;
	for (auto const& url : urls)
	{
		insecure = insecure
			|| url.scheme().toLower() == QLatin1String("http");
		secret_parameters.append(
			imagery::ManualImagerySource::
				likelySecretQueryParameters(url));
		if (potentiallyPrivate(url)
		    && !permissions_->isApproved(url))
		{
			auto const answer = QMessageBox::warning(
				this,
				tr("Allow local-network imagery"),
				tr("This source contacts %1 on a local or private "
				   "network. Allow that origin on this device?")
					.arg(
						imagery::TileNetworkManager::
							canonicalOrigin(url)),
				QMessageBox::Yes | QMessageBox::Cancel,
				QMessageBox::Cancel);
			if (answer != QMessageBox::Yes
			    || !permissions_->approve(url))
				return;
		}
	}
	secret_parameters.removeDuplicates();
	QStringList warnings;
	if (insecure)
		warnings.push_back(
			tr("Imagery tiles will be downloaded over unencrypted HTTP."));
	if (!secret_parameters.isEmpty())
	{
		warnings.push_back(
			tr("The complete endpoint, including credential-like "
			   "query parameter(s) %1, will be embedded in this map.")
				.arg(secret_parameters.join(
					QStringLiteral(", "))));
	}
	if (!warnings.isEmpty())
	{
		auto const answer = QMessageBox::warning(
			this,
			tr("Review imagery endpoint"),
			warnings.join(QStringLiteral("\n\n")),
			QMessageBox::Yes | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (answer != QMessageBox::Yes)
			return;
	}
	if (display_name_->text().trimmed().isEmpty())
		display_name_->setText(selected_source_->metadata.name);
	accept();
}


void OnlineImageryDialog::setStatus(
	const QString& text,
	QStyle::StandardPixmap icon)
{
	status_icon_->setPixmap(
		style()->standardIcon(icon).pixmap(24, 24));
	status_text_->setText(text);
}

}  // namespace OpenOrienteering
