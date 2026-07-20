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

#ifndef OPENORIENTEERING_GUI_IMAGERY_NETWORK_PERMISSIONS_DIALOG_H
#define OPENORIENTEERING_GUI_IMAGERY_NETWORK_PERMISSIONS_DIALOG_H

#include <QDialog>

class QLabel;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;

namespace OpenOrienteering {

namespace imagery {
class ImageryNetworkPermissions;
}

/** Reviews installation-local exceptions to the private-network imagery policy. */
class ImageryNetworkPermissionsDialog final : public QDialog
{
Q_OBJECT

public:
	explicit ImageryNetworkPermissionsDialog(
		imagery::ImageryNetworkPermissions& permissions,
		QWidget* parent = nullptr);

private:
	void rebuild();
	void updateButtons();
	void approveSelected();
	void revokeSelected();
	void dismissSelected();
	QTreeWidgetItem* selectedItem() const;

	imagery::ImageryNetworkPermissions& permissions_;
	QTreeWidget* origins_ = nullptr;
	QLabel* empty_state_ = nullptr;
	QPushButton* approve_button_ = nullptr;
	QPushButton* revoke_button_ = nullptr;
	QPushButton* dismiss_button_ = nullptr;
};

}  // namespace OpenOrienteering

#endif
