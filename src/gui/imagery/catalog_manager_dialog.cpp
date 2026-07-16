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

#include "gui/imagery/catalog_manager_dialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "gui/file_dialog.h"
#include "gui/imagery/catalog_import_dialog.h"

namespace OpenOrienteering {

namespace {

constexpr int catalog_id_role = Qt::UserRole;

QString checkedText(const QDateTime& value)
{
	return value.isValid()
		? QLocale().toString(
			value.toLocalTime(),
			QLocale::ShortFormat)
		: CatalogManagerDialog::tr("Never");
}

QString plainTextToolTip(const QString& value)
{
	auto escaped = value.toHtmlEscaped();
	escaped.replace(
		QLatin1Char('\n'),
		QStringLiteral("<br>"));
	return QStringLiteral("<qt>%1</qt>").arg(escaped);
}

}  // namespace


CatalogManagerDialog::CatalogManagerDialog(
	imagery::ImageryCatalogRepository& repository,
	QWidget* parent,
	imagery::ImageryNetworkPermissions* permissions)
 : QDialog(parent)
 , repository_(repository)
	, permissions_(permissions)
{
	setWindowTitle(tr("Imagery catalogs"));
	resize(780, 440);

	issues_icon_ = new QLabel(this);
	issues_icon_->setPixmap(
		style()->standardIcon(
			QStyle::SP_MessageBoxWarning).pixmap(24, 24));
	issues_icon_->setAccessibleName(
		tr("Catalog storage warning"));
	issues_icon_->hide();
	issues_ = new QLabel(this);
	issues_->setTextFormat(Qt::PlainText);
	issues_->setWordWrap(true);
	issues_->hide();
	auto* issues_layout = new QHBoxLayout();
	issues_layout->addWidget(
		issues_icon_, 0, Qt::AlignTop);
	issues_layout->addWidget(issues_, 1);

	catalogs_ = new QTreeWidget(this);
	catalogs_->setRootIsDecorated(false);
	catalogs_->setSelectionMode(
		QAbstractItemView::SingleSelection);
	catalogs_->setHeaderLabels({
		tr("Catalog"),
		tr("Revision"),
		tr("Usable sources"),
		tr("Last checked"),
		tr("Origin"),
	});
	catalogs_->setAlternatingRowColors(true);
	catalogs_->setSortingEnabled(true);
	catalogs_->sortByColumn(0, Qt::AscendingOrder);

	auto* import_file = new QPushButton(
		tr("Import file…"),
		this);
	auto* import_url = new QPushButton(
		tr("Import URL…"),
		this);
	update_button_ = new QPushButton(
		tr("Check for update"),
		this);
	remove_button_ = new QPushButton(
		tr("Remove"),
		this);
	auto* action_layout = new QHBoxLayout();
	action_layout->addWidget(import_file);
	action_layout->addWidget(import_url);
	action_layout->addStretch();
	action_layout->addWidget(update_button_);
	action_layout->addWidget(remove_button_);

	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Close,
		this);

	auto* layout = new QVBoxLayout(this);
	layout->addLayout(issues_layout);
	layout->addWidget(catalogs_, 1);
	layout->addLayout(action_layout);
	layout->addWidget(buttons);

	connect(
		import_file,
		&QPushButton::clicked,
		this,
		&CatalogManagerDialog::importFile);
	connect(
		import_url,
		&QPushButton::clicked,
		this,
		&CatalogManagerDialog::importUrl);
	connect(
		update_button_,
		&QPushButton::clicked,
		this,
		&CatalogManagerDialog::checkForUpdate);
	connect(
		remove_button_,
		&QPushButton::clicked,
		this,
		&CatalogManagerDialog::removeSelected);
	connect(
		buttons,
		&QDialogButtonBox::rejected,
		this,
		&QDialog::reject);
	connect(
		catalogs_,
		&QTreeWidget::itemSelectionChanged,
		this,
		&CatalogManagerDialog::updateButtons);
	connect(
		&repository_,
		&imagery::ImageryCatalogRepository::snapshotChanged,
		this,
		&CatalogManagerDialog::rebuild);
	connect(
		&repository_,
		&imagery::ImageryCatalogRepository::operationFinished,
		this,
		&CatalogManagerDialog::operationFinished);

	rebuild();
}


void CatalogManagerDialog::rebuild()
{
	auto selected_id = QString {};
	if (auto* item = catalogs_->currentItem())
		selected_id = item->data(0, catalog_id_role).toString();

	catalogs_->clear();
	auto const snapshot = repository_.snapshot();
	if (!snapshot)
		return;
	for (auto const& installed : snapshot->catalogs)
	{
		auto const& catalog = installed.read_result.catalog;
		auto* item = new QTreeWidgetItem(catalogs_);
		item->setText(
			0,
			catalog.name.isEmpty() ? catalog.id : catalog.name);
		item->setText(1, QString::number(catalog.revision));
		item->setText(
			2,
			QString::number(
				installed.read_result.supportedSourceCount()));
		item->setText(3, checkedText(installed.state.checked_at));
		item->setText(
			4,
			QUrl(installed.state.origin)
				.toDisplayString(
					QUrl::RemovePassword
					| QUrl::RemoveQuery));
		item->setData(0, catalog_id_role, catalog.id);
		item->setToolTip(
			0,
			plainTextToolTip(
				tr("ID: %1\nSHA-256: %2")
					.arg(
						catalog.id,
						QString::fromLatin1(
							installed.state.sha256))));
		if (catalog.id == selected_id)
			catalogs_->setCurrentItem(item);
	}
	catalogs_->resizeColumnToContents(0);
	catalogs_->resizeColumnToContents(1);
	catalogs_->resizeColumnToContents(2);
	catalogs_->resizeColumnToContents(3);

	if (snapshot->issues.isEmpty())
	{
		issues_icon_->hide();
		issues_->hide();
	}
	else
	{
		issues_->setText(
			tr("%n catalog store entry could not be loaded. "
			   "Healthy catalogs remain available.",
			   nullptr,
			   snapshot->issues.size()));
		issues_icon_->show();
		issues_->show();
	}
	updateButtons();
}


const imagery::InstalledImageryCatalog*
CatalogManagerDialog::selectedCatalog() const
{
	auto* item = catalogs_->currentItem();
	if (!item)
		return nullptr;
	auto const snapshot = repository_.snapshot();
	return snapshot
		? snapshot->catalog(
			item->data(0, catalog_id_role).toString())
		: nullptr;
}


void CatalogManagerDialog::updateButtons()
{
	auto const* catalog = selectedCatalog();
	auto const scheme = catalog
		? QUrl(
			catalog->state.final_url.isEmpty()
				? catalog->state.origin
				: catalog->state.final_url)
			  .scheme()
			  .toLower()
		: QString {};
	update_button_->setEnabled(
		catalog
		&& (scheme == QLatin1String("https")
		    || scheme == QLatin1String("http"))
		&& !remove_operation_);
	remove_button_->setEnabled(catalog && !remove_operation_);
}


void CatalogManagerDialog::importFile()
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


void CatalogManagerDialog::importUrl()
{
	bool accepted = false;
	auto const value = QInputDialog::getText(
		this,
		tr("Import imagery catalog"),
		tr("Catalog URL:"),
		QLineEdit::Normal,
		QStringLiteral("https://"),
		&accepted);
	if (!accepted || value.trimmed().isEmpty())
		return;
	imagery::ImageryCatalogFetchRequest request;
	request.url = QUrl::fromUserInput(value.trimmed());
	if (request.url.scheme().toLower() == QLatin1String("http"))
	{
		auto const answer = QMessageBox::warning(
			this,
			tr("Unencrypted catalog download"),
			tr("This catalog URL uses plain HTTP. Its contents and "
			   "publisher identity cannot be trusted in transit. Continue?"),
			QMessageBox::Yes | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (answer != QMessageBox::Yes)
			return;
		request.allow_insecure_http = true;
	}
	CatalogImportDialog dialog(repository_, this, permissions_);
	dialog.startFetch(request);
	dialog.exec();
}


void CatalogManagerDialog::checkForUpdate()
{
	auto const* installed = selectedCatalog();
	if (!installed)
		return;
	imagery::ImageryCatalogFetchRequest request;
	request.url = QUrl(
		installed->state.final_url.isEmpty()
			? installed->state.origin
			: installed->state.final_url);
	request.etag = installed->state.etag;
	request.last_modified = installed->state.last_modified;
	request.installed_catalog_id =
		installed->read_result.catalog.id;
	if (request.url.scheme().toLower() == QLatin1String("http"))
	{
		auto const answer = QMessageBox::warning(
			this,
			tr("Unencrypted catalog update"),
			tr("Checking this catalog uses plain HTTP. Continue?"),
			QMessageBox::Yes | QMessageBox::Cancel,
			QMessageBox::Cancel);
		if (answer != QMessageBox::Yes)
			return;
		request.allow_insecure_http = true;
	}
	CatalogImportDialog dialog(repository_, this, permissions_);
	dialog.startFetch(request);
	dialog.exec();
}


void CatalogManagerDialog::removeSelected()
{
	auto const* installed = selectedCatalog();
	if (!installed || remove_operation_)
		return;
	auto const& catalog = installed->read_result.catalog;
	QMessageBox confirmation(
		QMessageBox::Question,
		tr("Remove imagery catalog"),
		tr("Remove “%1” from this installation?\n\n"
		   "Existing maps keep their embedded source snapshots.")
			.arg(catalog.name.isEmpty() ? catalog.id : catalog.name),
		QMessageBox::NoButton,
		this);
	auto* remove_button = confirmation.addButton(
		tr("Remove"),
		QMessageBox::DestructiveRole);
	confirmation.addButton(QMessageBox::Cancel);
	confirmation.exec();
	if (confirmation.clickedButton() != remove_button)
		return;
	remove_operation_ =
		repository_.removeCatalog(catalog.id);
	updateButtons();
}


void CatalogManagerDialog::operationFinished(
	imagery::ImageryCatalogRepository::OperationId id,
	const imagery::ImageryCatalogOperationResult& result)
{
	if (id != remove_operation_)
		return;
	remove_operation_ = 0;
	if (result.kind == imagery::ImageryCatalogOperationKind::Failed)
	{
		QMessageBox::warning(
			this,
			tr("Could not remove catalog"),
			result.error);
	}
	updateButtons();
}

}  // namespace OpenOrienteering
