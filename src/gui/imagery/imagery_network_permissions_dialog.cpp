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

#include "gui/imagery/imagery_network_permissions_dialog.h"

#include <QAbstractItemView>
#include <QDialogButtonBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QStyle>
#include <QTreeWidget>
#include <QUrl>
#include <QVBoxLayout>

#include "imagery/imagery_network_permissions.h"

namespace OpenOrienteering {

namespace {

constexpr int origin_role = Qt::UserRole;
constexpr int pending_role = Qt::UserRole + 1;

}  // namespace


ImageryNetworkPermissionsDialog::ImageryNetworkPermissionsDialog(
	imagery::ImageryNetworkPermissions& permissions,
	QWidget* parent)
 : QDialog(parent)
 , permissions_(permissions)
{
	setWindowTitle(tr("Imagery network permissions"));
	resize(660, 390);

	auto* explanation = new QLabel(
		tr("Mapper blocks imagery requests to private and non-global "
		   "destinations by default. Allow only origins you recognize. "
		   "These permissions are stored on this device; catalogs and map "
		   "files cannot grant them."),
		this);
	explanation->setWordWrap(true);
	explanation->setTextFormat(Qt::PlainText);

	origins_ = new QTreeWidget(this);
	origins_->setObjectName(
		QStringLiteral("imagery_network_permission_origins"));
	origins_->setRootIsDecorated(false);
	origins_->setSelectionMode(
		QAbstractItemView::SingleSelection);
	origins_->setAlternatingRowColors(true);
	origins_->setHeaderLabels({ tr("Origin"), tr("Access") });
	origins_->setSortingEnabled(true);
	origins_->sortByColumn(0, Qt::AscendingOrder);

	empty_state_ = new QLabel(this);
	empty_state_->setTextFormat(Qt::PlainText);
	empty_state_->setWordWrap(true);
	empty_state_->setAlignment(Qt::AlignCenter);
	empty_state_->setText(
		tr("No private-network imagery origins need review."));

	approve_button_ = new QPushButton(tr("Allow"), this);
	approve_button_->setObjectName(
		QStringLiteral("approve_imagery_network_origin"));
	revoke_button_ = new QPushButton(tr("Revoke"), this);
	revoke_button_->setObjectName(
		QStringLiteral("revoke_imagery_network_origin"));
	dismiss_button_ = new QPushButton(tr("Dismiss"), this);
	dismiss_button_->setObjectName(
		QStringLiteral("dismiss_imagery_network_origin"));
	auto* action_layout = new QHBoxLayout();
	action_layout->addWidget(approve_button_);
	action_layout->addWidget(revoke_button_);
	action_layout->addWidget(dismiss_button_);
	action_layout->addStretch();

	auto* buttons = new QDialogButtonBox(
		QDialogButtonBox::Close,
		this);
	auto* layout = new QVBoxLayout(this);
	layout->addWidget(explanation);
	layout->addWidget(origins_, 1);
	layout->addWidget(empty_state_);
	layout->addLayout(action_layout);
	layout->addWidget(buttons);

	connect(
		buttons,
		&QDialogButtonBox::rejected,
		this,
		&QDialog::reject);
	connect(
		origins_,
		&QTreeWidget::itemSelectionChanged,
		this,
		&ImageryNetworkPermissionsDialog::updateButtons);
	connect(
		approve_button_,
		&QPushButton::clicked,
		this,
		&ImageryNetworkPermissionsDialog::approveSelected);
	connect(
		revoke_button_,
		&QPushButton::clicked,
		this,
		&ImageryNetworkPermissionsDialog::revokeSelected);
	connect(
		dismiss_button_,
		&QPushButton::clicked,
		this,
		&ImageryNetworkPermissionsDialog::dismissSelected);
	connect(
		&permissions_,
		&imagery::ImageryNetworkPermissions::approvalsChanged,
		this,
		&ImageryNetworkPermissionsDialog::rebuild);
	connect(
		&permissions_,
		&imagery::ImageryNetworkPermissions::pendingOriginsChanged,
		this,
		&ImageryNetworkPermissionsDialog::rebuild);

	rebuild();
}


void ImageryNetworkPermissionsDialog::rebuild()
{
	auto selected_origin = QString {};
	if (auto* item = selectedItem())
		selected_origin = item->data(0, origin_role).toString();

	origins_->clear();
	for (auto const& origin : permissions_.pendingOrigins())
	{
		auto* item = new QTreeWidgetItem(origins_);
		item->setText(0, origin);
		item->setText(1, tr("Approval needed"));
		item->setData(0, origin_role, origin);
		item->setData(0, pending_role, true);
		item->setIcon(
			0,
			style()->standardIcon(QStyle::SP_MessageBoxWarning));
		item->setToolTip(
			0,
			tr("A recent imagery request resolved to a private or "
			   "non-global destination and was blocked."));
		if (origin == selected_origin)
			origins_->setCurrentItem(item);
	}
	for (auto const& origin : permissions_.approvedOrigins())
	{
		auto* item = new QTreeWidgetItem(origins_);
		item->setText(0, origin);
		item->setText(1, tr("Allowed on this device"));
		item->setData(0, origin_role, origin);
		item->setData(0, pending_role, false);
		item->setIcon(
			0,
			style()->standardIcon(QStyle::SP_DialogApplyButton));
		item->setToolTip(
			0,
			tr("Mapper may contact this private-network origin for "
			   "imagery requests."));
		if (origin == selected_origin)
			origins_->setCurrentItem(item);
	}
	origins_->resizeColumnToContents(0);
	origins_->resizeColumnToContents(1);
	auto const empty = origins_->topLevelItemCount() == 0;
	origins_->setVisible(!empty);
	empty_state_->setVisible(empty);
	updateButtons();
}


QTreeWidgetItem*
ImageryNetworkPermissionsDialog::selectedItem() const
{
	return origins_->currentItem();
}


void ImageryNetworkPermissionsDialog::updateButtons()
{
	auto* item = selectedItem();
	auto const pending = item
		&& item->data(0, pending_role).toBool();
	approve_button_->setEnabled(item && pending);
	dismiss_button_->setEnabled(item && pending);
	revoke_button_->setEnabled(item && !pending);
}


void ImageryNetworkPermissionsDialog::approveSelected()
{
	auto* item = selectedItem();
	if (!item || !item->data(0, pending_role).toBool())
		return;
	auto const origin = item->data(0, origin_role).toString();
	auto const answer = QMessageBox::warning(
		this,
		tr("Allow private-network imagery"),
		tr("Allow Mapper to contact %1 for imagery?\n\n"
		   "Only continue if you recognize and trust this origin. "
		   "The permission applies to this installation until revoked.")
			.arg(origin),
		QMessageBox::Yes | QMessageBox::Cancel,
		QMessageBox::Cancel);
	if (answer == QMessageBox::Yes)
		permissions_.approve(QUrl(origin));
}


void ImageryNetworkPermissionsDialog::revokeSelected()
{
	auto* item = selectedItem();
	if (!item || item->data(0, pending_role).toBool())
		return;
	auto const origin = item->data(0, origin_role).toString();
	auto const answer = QMessageBox::question(
		this,
		tr("Revoke imagery permission"),
		tr("Stop allowing imagery requests to %1?\n\n"
		   "Active requests to this origin will be cancelled.")
			.arg(origin),
		QMessageBox::Yes | QMessageBox::Cancel,
		QMessageBox::Yes);
	if (answer == QMessageBox::Yes)
		permissions_.revoke(QUrl(origin));
}


void ImageryNetworkPermissionsDialog::dismissSelected()
{
	auto* item = selectedItem();
	if (!item || !item->data(0, pending_role).toBool())
		return;
	permissions_.dismissPending(
		QUrl(item->data(0, origin_role).toString()));
}

}  // namespace OpenOrienteering
