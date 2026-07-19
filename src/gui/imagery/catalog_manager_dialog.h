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

#ifndef OPENORIENTEERING_GUI_CATALOG_MANAGER_DIALOG_H
#define OPENORIENTEERING_GUI_CATALOG_MANAGER_DIALOG_H

#include <QDialog>

#include "imagery/imagery_catalog_repository.h"

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace OpenOrienteering {

namespace imagery {
class ImageryNetworkPermissions;
}

class CatalogManagerDialog final : public QDialog
{
Q_OBJECT

public:
	explicit CatalogManagerDialog(
		imagery::ImageryCatalogRepository& repository,
		QWidget* parent = nullptr,
		imagery::ImageryNetworkPermissions* permissions = nullptr);

private:
	void rebuild();
	const imagery::InstalledImageryCatalog* selectedCatalog() const;
	void updateButtons();
	void importFile();
	void importUrl();
	void checkForUpdate();
	void removeSelected();
	void operationFinished(
		imagery::ImageryCatalogRepository::OperationId id,
		const imagery::ImageryCatalogOperationResult& result);

	imagery::ImageryCatalogRepository& repository_;
	imagery::ImageryNetworkPermissions* permissions_ = nullptr;
	imagery::ImageryCatalogRepository::OperationId remove_operation_ = 0;
	QTreeWidget* catalogs_ = nullptr;
	QLabel* issues_icon_ = nullptr;
	QLabel* issues_ = nullptr;
	QPushButton* update_button_ = nullptr;
	QPushButton* remove_button_ = nullptr;
};

}  // namespace OpenOrienteering

#endif
