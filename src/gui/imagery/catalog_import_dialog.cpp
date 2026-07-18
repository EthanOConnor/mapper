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

#include "gui/imagery/catalog_import_dialog.h"

#include <algorithm>

#include <QCheckBox>
#include <QDialogButtonBox>
#include <QFormLayout>
#include <QHostAddress>
#include <QLabel>
#include <QMessageBox>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSet>
#include <QStyle>
#include <QUrl>
#include <QVBoxLayout>

#include "imagery/manual_imagery_source.h"
#include "imagery/imagery_network_permissions.h"
#include "imagery/tile_network_manager.h"

namespace OpenOrienteering {

namespace {

QString updateKindText(
	imagery::ImageryCatalogAnalysis::UpdateKind kind)
{
	using Kind = imagery::ImageryCatalogAnalysis::UpdateKind;
	switch (kind)
	{
	case Kind::NewCatalog:
		return CatalogImportDialog::tr("New catalog");
	case Kind::ExactReimport:
		return CatalogImportDialog::tr(
			"Same catalog snapshot");
	case Kind::HigherRevision:
		return CatalogImportDialog::tr(
			"Newer revision");
	case Kind::LowerRevision:
		return CatalogImportDialog::tr(
			"Older revision");
	case Kind::SameRevisionConflict:
		return CatalogImportDialog::tr(
			"Republished revision");
	}
	Q_UNREACHABLE();
}

QString displayUrl(const QString& value)
{
	auto url = QUrl(value);
	if (!url.isValid())
		return value;
	url.setPassword(QString {});
	url.setUserName(QString {});
	auto query = url.query();
	if (!query.isEmpty())
		url.setQuery(QStringLiteral("…"));
	return url.toDisplayString(
		QUrl::RemovePassword | QUrl::DecodeReserved);
}

QStringList requestHosts(
	const imagery::OicCatalogReadResult& catalog)
{
	QSet<QString> hosts;
	for (auto const& source : catalog.catalog.sources)
	{
		for (auto const& tile : source.tile_urls)
		{
			auto text = tile.value;
			text.replace(QStringLiteral("${z}"), QStringLiteral("0"));
			text.replace(QStringLiteral("${x}"), QStringLiteral("0"));
			text.replace(QStringLiteral("${y}"), QStringLiteral("0"));
			text.replace(QStringLiteral("{z}"), QStringLiteral("0"));
			text.replace(QStringLiteral("{x}"), QStringLiteral("0"));
			text.replace(QStringLiteral("{y}"), QStringLiteral("0"));
			auto const url = QUrl(text);
			if (!url.host().isEmpty())
				hosts.insert(url.host().toLower());
		}
	}
	auto list = hosts.values();
	std::sort(list.begin(), list.end());
	return list;
}

QVector<QUrl> requestUrls(
	const imagery::OicCatalogReadResult& catalog)
{
	QVector<QUrl> urls;
	for (auto const& source : catalog.catalog.sources)
	{
		for (auto const& tile : source.tile_urls)
		{
			auto text = tile.value;
			text.replace(QStringLiteral("${z}"), QStringLiteral("0"));
			text.replace(QStringLiteral("${x}"), QStringLiteral("0"));
			text.replace(QStringLiteral("${y}"), QStringLiteral("0"));
			text.replace(QStringLiteral("{z}"), QStringLiteral("0"));
			text.replace(QStringLiteral("{x}"), QStringLiteral("0"));
			text.replace(QStringLiteral("{y}"), QStringLiteral("0"));
			urls.push_back(QUrl(text));
		}
	}
	return urls;
}

}  // namespace


CatalogImportDialog::CatalogImportDialog(
	imagery::ImageryCatalogRepository& repository,
	QWidget* parent,
	imagery::ImageryNetworkPermissions* permissions)
 : QDialog(parent)
 , repository_(repository)
{
	setWindowTitle(tr("Review imagery catalog"));
	resize(620, 560);

	status_icon_ = new QLabel(this);
	status_icon_->setAccessibleName(
		tr("Catalog review status"));
	status_text_ = new QLabel(this);
	status_text_->setTextFormat(Qt::PlainText);
	status_text_->setWordWrap(true);
	auto* status_layout = new QHBoxLayout();
	status_layout->addWidget(status_icon_, 0, Qt::AlignTop);
	status_layout->addWidget(status_text_, 1);

	progress_ = new QProgressBar(this);
	progress_->setRange(0, 0);
	progress_->setTextVisible(false);

	auto* details = new QWidget(this);
	auto* form = new QFormLayout(details);
	form->setFieldGrowthPolicy(
		QFormLayout::AllNonFixedFieldsGrow);
	identity_ = new QLabel(details);
	origin_ = new QLabel(details);
	publisher_ = new QLabel(details);
	hash_ = new QLabel(details);
	changes_ = new QLabel(details);
	sources_ = new QLabel(details);
	hosts_ = new QLabel(details);
	warnings_ = new QLabel(details);
	for (auto* label : {
		     identity_, origin_, publisher_, hash_, changes_,
		     sources_, hosts_, warnings_ })
	{
		label->setTextFormat(Qt::PlainText);
		label->setWordWrap(true);
		label->setTextInteractionFlags(
			Qt::TextSelectableByMouse);
	}
	form->addRow(tr("Catalog:"), identity_);
	form->addRow(tr("Origin:"), origin_);
	form->addRow(tr("Publisher:"), publisher_);
	form->addRow(tr("Document SHA-256:"), hash_);
	form->addRow(tr("Update:"), changes_);
	form->addRow(tr("Sources:"), sources_);
	form->addRow(tr("Request hosts:"), hosts_);
	form->addRow(tr("Review:"), warnings_);

	auto* scroll = new QScrollArea(this);
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidget(details);

	confirmation_ = new QCheckBox(this);
	confirmation_->hide();

	buttons_ = new QDialogButtonBox(
		QDialogButtonBox::Cancel,
		this);
	approve_private_button_ = buttons_->addButton(
		tr("Allow local network and retry"),
		QDialogButtonBox::ActionRole);
	approve_private_button_->hide();
	install_button_ = buttons_->addButton(
		tr("Install"),
		QDialogButtonBox::AcceptRole);
	install_button_->setEnabled(false);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(status_layout);
	layout->addWidget(progress_);
	layout->addWidget(scroll, 1);
	layout->addWidget(confirmation_);
	layout->addWidget(buttons_);

	connect(
		buttons_,
		&QDialogButtonBox::rejected,
		this,
		&QDialog::reject);
	connect(
		install_button_,
		&QPushButton::clicked,
		this,
		&CatalogImportDialog::install);
	connect(
		approve_private_button_,
		&QPushButton::clicked,
		this,
		&CatalogImportDialog::approvePrivateFetch);
	connect(
		confirmation_,
		&QCheckBox::toggled,
		this,
		&CatalogImportDialog::updateInstallButton);
	connect(
		&repository_,
		&imagery::ImageryCatalogRepository::operationFinished,
		this,
		&CatalogImportDialog::operationFinished);
	permissions_ = permissions
		? permissions
		: new imagery::ImageryNetworkPermissions(
			repository_.networkManager(),
			this);
}


CatalogImportDialog::~CatalogImportDialog()
{
	if (operation_id_)
		repository_.cancel(operation_id_);
}


void CatalogImportDialog::startFile(
	const QString& path)
{
	fetch_request_.reset();
	private_network_approval_url_.clear();
	startOperation(
		repository_.readCatalogFile(path),
		tr("Reading and validating the catalog…"));
}


void CatalogImportDialog::startFetch(
	const imagery::ImageryCatalogFetchRequest& request)
{
	fetch_request_ = request;
	private_network_approval_url_.clear();
	startOperation(
		repository_.fetchCatalog(request),
		request.installed_catalog_id.isEmpty()
			? tr("Downloading and validating the catalog…")
			: tr("Checking for a catalog update…"));
}


void CatalogImportDialog::startOperation(
	imagery::ImageryCatalogRepository::OperationId operation_id,
	const QString& activity)
{
	if (operation_id_)
		repository_.cancel(operation_id_);
	operation_id_ = operation_id;
	candidate_.clear();
	approve_private_button_->hide();
	progress_->show();
	showStatus(activity, QStyle::SP_BrowserReload);
	updateInstallButton();
}


void CatalogImportDialog::operationFinished(
	imagery::ImageryCatalogRepository::OperationId operation_id,
	const imagery::ImageryCatalogOperationResult& result)
{
	if (operation_id != operation_id_)
		return;
	operation_id_ = 0;
	progress_->hide();
	switch (result.kind)
	{
	case imagery::ImageryCatalogOperationKind::CandidateReady:
		showCandidate(result.candidate);
		return;

	case imagery::ImageryCatalogOperationKind::NotModified:
		showStatus(
			tr("This catalog is already up to date."),
			QStyle::SP_DialogApplyButton);
		install_button_->setText(tr("Close"));
		install_button_->setEnabled(true);
		disconnect(
			install_button_,
			&QPushButton::clicked,
			this,
			&CatalogImportDialog::install);
		connect(
			install_button_,
			&QPushButton::clicked,
			this,
			&QDialog::accept);
		return;

	case imagery::ImageryCatalogOperationKind::Installed:
		showStatus(
			tr("The imagery catalog was installed."),
			QStyle::SP_DialogApplyButton);
		accept();
		return;

	case imagery::ImageryCatalogOperationKind::Cancelled:
		showStatus(
			tr("Catalog operation cancelled."),
			QStyle::SP_MessageBoxInformation);
		return;

	case imagery::ImageryCatalogOperationKind::Failed:
		if (fetch_request_
		    && !result.private_network_approval_url.isEmpty())
		{
			private_network_approval_url_ =
				result.private_network_approval_url;
			showStatus(
				tr("This catalog is on a local or private "
				   "network. Contacting it requires an explicit, "
				   "installation-local approval."),
				QStyle::SP_MessageBoxWarning);
			approve_private_button_->show();
			updateInstallButton();
			return;
		}
		showStatus(
			result.error.isEmpty()
				? tr("The catalog operation failed.")
				: result.error,
			QStyle::SP_MessageBoxCritical);
		updateInstallButton();
		return;

	case imagery::ImageryCatalogOperationKind::Removed:
		break;
	}
}


void CatalogImportDialog::approvePrivateFetch()
{
	if (!fetch_request_
	    || private_network_approval_url_.isEmpty())
		return;
	auto const origin =
		imagery::TileNetworkManager::canonicalOrigin(
			private_network_approval_url_);
	auto const answer = QMessageBox::warning(
		this,
		tr("Allow local-network catalog"),
		tr("Allow Mapper to contact %1 from this installation?\n\n"
		   "This permission is stored only on this device. Catalogs "
		   "and map files cannot grant it.")
			.arg(origin),
		QMessageBox::Yes | QMessageBox::Cancel,
		QMessageBox::Cancel);
	if (answer != QMessageBox::Yes
	    || !permissions_->approve(private_network_approval_url_))
		return;
	auto const request = *fetch_request_;
	startFetch(request);
}


void CatalogImportDialog::showCandidate(
	imagery::ImageryCatalogCandidatePtr candidate)
{
	candidate_ = std::move(candidate);
	confirmation_required_ = false;
	install_options_ = {};
	if (!candidate_)
	{
		showStatus(
			tr("The catalog could not be read."),
			QStyle::SP_MessageBoxCritical);
		return;
	}

	auto const& read = candidate_->read_result;
	auto const& catalog = read.catalog;
	auto const& analysis = candidate_->analysis;
	identity_->setText(
		tr("%1\nID: %2\nRevision: %3")
			.arg(
				catalog.name.isEmpty() ? catalog.id : catalog.name,
				catalog.id)
			.arg(catalog.revision));
	origin_->setText(displayUrl(candidate_->metadata.origin));
	publisher_->setText(
		catalog.publisher
			? catalog.publisher->name
			: tr("Not specified"));
	hash_->setText(QString::fromLatin1(catalog.document_sha256));
	changes_->setText(
		tr("%1\n%2 added, %3 removed, %4 operational, "
		   "%5 metadata-only")
			.arg(updateKindText(analysis.update_kind))
			.arg(analysis.added)
			.arg(analysis.removed)
			.arg(analysis.operational_changed)
			.arg(analysis.metadata_only_changed));
	sources_->setText(
		tr("%1 total · %2 usable · %3 invalid · %4 unsupported")
			.arg(catalog.sources.size())
			.arg(read.supportedSourceCount())
			.arg(analysis.invalid)
			.arg(analysis.unsupported));
	auto const hosts = requestHosts(read);
	hosts_->setText(
		hosts.isEmpty()
			? tr("None")
			: hosts.join(QLatin1Char('\n')));

	QStringList warnings;
	if (!read.accepted())
	{
		for (auto const& diagnostic : read.diagnostics)
		{
			warnings.push_back(diagnostic.displayText());
			if (warnings.size() >= 8)
				break;
		}
	}
	if (analysis.update_kind
	    == imagery::ImageryCatalogAnalysis::UpdateKind::LowerRevision)
	{
		install_options_.allow_lower_revision = true;
		confirmation_required_ = true;
		warnings.push_back(
			tr("This would replace a newer installed revision."));
	}
	if (analysis.update_kind
	    == imagery::ImageryCatalogAnalysis::UpdateKind::
	       SameRevisionConflict)
	{
		install_options_.allow_same_revision_conflict = true;
		confirmation_required_ = true;
		warnings.push_back(
			tr("The publisher reused a revision number for different contents."));
	}
	if (catalog.original_bytes.size() > 1024 * 1024
	    || catalog.sources.size() > 100)
	{
		confirmation_required_ = true;
		warnings.push_back(
			tr("This is a large catalog; review its publisher and request hosts."));
	}
	if (QUrl(candidate_->metadata.origin).scheme()
	      == QLatin1String("http"))
	{
		confirmation_required_ = true;
		warnings.push_back(
			tr("The catalog was downloaded over unencrypted HTTP."));
	}
	if (analysis.exact_duplicates > 0
	    || analysis.potential_duplicates > 0)
	{
		warnings.push_back(
			tr("%1 exact and %2 operational duplicate source(s) "
			   "also exist in other catalogs.")
				.arg(analysis.exact_duplicates)
				.arg(analysis.potential_duplicates));
	}
	bool insecure_service = false;
	bool local_service = false;
	QStringList secret_parameters;
	for (auto const& url : requestUrls(read))
	{
		insecure_service = insecure_service
			|| url.scheme().toLower() == QLatin1String("http");
		auto const host = url.host().toLower();
			QHostAddress address;
			local_service = local_service
				|| (address.setAddress(host)
				    && !imagery::TileNetworkManager::
				           isPublicDestinationAddress(address))
			|| host == QLatin1String("localhost")
			|| host.endsWith(QLatin1String(".localhost"))
			|| host.endsWith(QLatin1String(".local"))
			|| host.endsWith(QLatin1String(".internal"))
			|| host.endsWith(QLatin1String(".home.arpa"));
		secret_parameters.append(
			imagery::ManualImagerySource::
				likelySecretQueryParameters(url));
	}
	secret_parameters.removeDuplicates();
	if (insecure_service)
	{
		warnings.push_back(
			tr("One or more imagery services use unencrypted HTTP."));
	}
	if (local_service)
	{
		warnings.push_back(
			tr("One or more services target a local or private-network "
			   "origin. Installing this catalog does not grant access."));
	}
	if (!secret_parameters.isEmpty())
	{
		warnings.push_back(
			tr("Service endpoints contain credential-like query "
			   "parameter(s): %1.")
				.arg(secret_parameters.join(QStringLiteral(", "))));
	}
	for (auto const& source : catalog.sources)
	{
		if (source.registration.kind
		    != imagery::OicRegistrationKind::None)
		{
			warnings.push_back(
				tr("One or more sources include surveyed registration corrections."));
			break;
		}
	}
	warnings.removeDuplicates();
	warnings_->setText(
		warnings.isEmpty()
			? tr("No additional warnings.")
			: QStringLiteral("• ")
			  + warnings.join(QStringLiteral("\n• ")));

	if (read.accepted())
	{
		showStatus(
			tr("Review the catalog before installing it. "
			   "No imagery service has been contacted."),
			warnings.isEmpty()
				? QStyle::SP_MessageBoxInformation
				: QStyle::SP_MessageBoxWarning);
	}
	else
	{
		showStatus(
			tr("This file is not an installable OIC catalog."),
			QStyle::SP_MessageBoxCritical);
	}

	confirmation_->setText(
		tr("I reviewed the warnings and want to install this catalog."));
	confirmation_->setChecked(false);
	confirmation_->setVisible(
		confirmation_required_ && read.accepted());
	updateInstallButton();
}


void CatalogImportDialog::showStatus(
	const QString& text,
	QStyle::StandardPixmap icon)
{
	status_icon_->setPixmap(
		style()->standardIcon(icon).pixmap(24, 24));
	status_text_->setText(text);
}


void CatalogImportDialog::updateInstallButton()
{
	auto const accepted =
		candidate_
		&& candidate_->read_result.accepted()
		&& (!confirmation_required_
		    || confirmation_->isChecked());
	install_button_->setEnabled(
		accepted && operation_id_ == 0);
}


void CatalogImportDialog::install()
{
	if (!candidate_ || operation_id_)
		return;
	startOperation(
		repository_.installCandidate(
			candidate_,
			install_options_),
		tr("Installing the catalog snapshot…"));
}

}  // namespace OpenOrienteering
