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

#ifndef OPENORIENTEERING_GUI_CATALOG_IMPORT_DIALOG_H
#define OPENORIENTEERING_GUI_CATALOG_IMPORT_DIALOG_H

#include <optional>

#include <QDialog>
#include <QStyle>

#include "imagery/imagery_catalog_repository.h"

class QCheckBox;
class QDialogButtonBox;
class QLabel;
class QProgressBar;
class QPushButton;

namespace OpenOrienteering {

namespace imagery {
class ImageryNetworkPermissions;
}

class CatalogImportDialog final : public QDialog
{
Q_OBJECT

public:
	explicit CatalogImportDialog(
		imagery::ImageryCatalogRepository& repository,
		QWidget* parent = nullptr,
		imagery::ImageryNetworkPermissions* permissions = nullptr);
	~CatalogImportDialog() override;

	void startFile(const QString& path);
	void startFetch(
		const imagery::ImageryCatalogFetchRequest& request);

private:
	void startOperation(
		imagery::ImageryCatalogRepository::OperationId operation_id,
		const QString& activity);
	void operationFinished(
		imagery::ImageryCatalogRepository::OperationId operation_id,
		const imagery::ImageryCatalogOperationResult& result);
	void showCandidate(
		imagery::ImageryCatalogCandidatePtr candidate);
	void showStatus(
		const QString& text,
		QStyle::StandardPixmap icon);
	void approvePrivateFetch();
	void install();
	void updateInstallButton();

	imagery::ImageryCatalogRepository& repository_;
	imagery::ImageryNetworkPermissions* permissions_ = nullptr;
	imagery::ImageryCatalogRepository::OperationId operation_id_ = 0;
	std::optional<imagery::ImageryCatalogFetchRequest> fetch_request_;
	QUrl private_network_approval_url_;
	imagery::ImageryCatalogCandidatePtr candidate_;
	imagery::ImageryCatalogInstallOptions install_options_;
	bool confirmation_required_ = false;

	QLabel* status_icon_ = nullptr;
	QLabel* status_text_ = nullptr;
	QLabel* identity_ = nullptr;
	QLabel* origin_ = nullptr;
	QLabel* publisher_ = nullptr;
	QLabel* hash_ = nullptr;
	QLabel* changes_ = nullptr;
	QLabel* sources_ = nullptr;
	QLabel* hosts_ = nullptr;
	QLabel* warnings_ = nullptr;
	QProgressBar* progress_ = nullptr;
	QCheckBox* confirmation_ = nullptr;
	QDialogButtonBox* buttons_ = nullptr;
	QPushButton* approve_private_button_ = nullptr;
	QPushButton* install_button_ = nullptr;
};

}  // namespace OpenOrienteering

#endif
