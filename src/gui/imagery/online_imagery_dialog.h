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

#ifndef OPENORIENTEERING_GUI_ONLINE_IMAGERY_DIALOG_H
#define OPENORIENTEERING_GUI_ONLINE_IMAGERY_DIALOG_H

#include <optional>

#include <QDialog>
#include <QStyle>
#include <QUrl>

#include "imagery/arcgis_tile_service.h"
#include "imagery/imagery_catalog_repository.h"
#include "imagery/manual_imagery_source.h"

class QComboBox;
class QDialogButtonBox;
class QGroupBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSortFilterProxyModel;
class QSpinBox;
class QStackedWidget;
class QTimer;
class QTreeView;
class OnlineImageryDialogTest;

namespace OpenOrienteering {

namespace imagery {
class ImageryNetworkPermissions;
}

class ImagerySourceModel;

class OnlineImageryDialog final : public QDialog
{
Q_OBJECT

public:
	explicit OnlineImageryDialog(
		imagery::ImageryCatalogRepository& repository,
		imagery::TileNetworkManager& network,
		QWidget* parent = nullptr,
		imagery::ImageryNetworkPermissions* permissions = nullptr);
	~OnlineImageryDialog() override;

	const imagery::ResolvedImagerySource& selectedSource() const;
	QString displayName() const;

private:
	friend class ::OnlineImageryDialogTest;

	void selectCatalogIndex(const QModelIndex& proxy_index);
	void cancelManualDiscovery();
	void showCatalogSource(
		const imagery::ImagerySourceHandle& handle);
	void showManualPage();
	void classifyManual();
	void scheduleManualClassification();
	imagery::TileNetworkRequest arcGisDiscoveryRequest(
		quint64 generation) const;
	void discoverArcGis();
	void onNetworkFinished(
		imagery::TileNetworkManager::Token token,
		const imagery::TileNetworkResult& result);
	void approvePrivateDiscoveryOrigin();
	void updateCatalogSelectionAfterReload();
	void importCatalog();
	void manageCatalogs();
	void manageNetworkPermissions();
	void acceptSelection();
	void setStatus(
		const QString& text,
		QStyle::StandardPixmap icon);
	void setSelectedSource(
		imagery::ResolvedImagerySource source,
		const QString& suggested_name);
	void clearSelectedSource();
	imagery::ManualTiledSourceSettings manualSettings(
		QString* error) const;
	QVector<QUrl> selectedRequestUrls() const;

	imagery::ImageryCatalogRepository& repository_;
	imagery::TileNetworkManager& network_;
	imagery::ImageryNetworkPermissions* permissions_ = nullptr;
	quint64 network_client_id_ = 0;
	imagery::TileNetworkManager::Token discovery_token_ = 0;
	quint64 discovery_generation_ = 0;
	QUrl insecure_discovery_consent_;
	QUrl private_discovery_approval_url_;
	imagery::ManualImageryDiscoveryResult manual_result_;
	std::optional<imagery::ImagerySourceHandle> selected_handle_;
	std::optional<imagery::ResolvedImagerySource> selected_source_;

	ImagerySourceModel* source_model_ = nullptr;
	QSortFilterProxyModel* source_filter_ = nullptr;
	QLineEdit* search_ = nullptr;
	QTreeView* source_tree_ = nullptr;
	QStackedWidget* pages_ = nullptr;

	QLabel* detail_title_ = nullptr;
	QLabel* detail_description_ = nullptr;
	QLabel* detail_status_ = nullptr;
	QLabel* detail_catalog_ = nullptr;
	QLabel* detail_dates_ = nullptr;
	QLabel* detail_crs_ = nullptr;
	QLabel* detail_hosts_ = nullptr;
	QLabel* detail_attribution_ = nullptr;
	QLabel* detail_terms_ = nullptr;

	QLineEdit* manual_url_ = nullptr;
	QComboBox* manual_scheme_ = nullptr;
	QSpinBox* manual_min_zoom_ = nullptr;
	QSpinBox* manual_max_zoom_ = nullptr;
	QComboBox* manual_tile_size_ = nullptr;
	QLineEdit* manual_referer_ = nullptr;
	QLineEdit* manual_empty_statuses_ = nullptr;
	QLineEdit* manual_attribution_ = nullptr;
	QLineEdit* manual_attribution_url_ = nullptr;
	QPushButton* discover_button_ = nullptr;
	QPushButton* approve_private_button_ = nullptr;
	QTimer* classify_timer_ = nullptr;

	QLineEdit* display_name_ = nullptr;
	QLabel* status_icon_ = nullptr;
	QLabel* status_text_ = nullptr;
	QDialogButtonBox* buttons_ = nullptr;
	QPushButton* add_button_ = nullptr;
};

}  // namespace OpenOrienteering

#endif
