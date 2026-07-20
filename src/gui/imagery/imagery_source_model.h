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

#ifndef OPENORIENTEERING_GUI_IMAGERY_SOURCE_MODEL_H
#define OPENORIENTEERING_GUI_IMAGERY_SOURCE_MODEL_H

#include <memory>
#include <optional>
#include <vector>

#include <QAbstractItemModel>

#include "imagery/imagery_catalog_repository.h"

namespace OpenOrienteering {

class ImagerySourceModel final : public QAbstractItemModel
{
Q_OBJECT

public:
	enum Role
	{
		NodeTypeRole = Qt::UserRole + 1,
		SourceHandleRole,
		SupportedRole,
		SearchTextRole,
		StatusTextRole,
	};

	enum class NodeType
	{
		Catalog,
		Source,
	};

	explicit ImagerySourceModel(
		imagery::ImageryCatalogRepository& repository,
		QObject* parent = nullptr);
	~ImagerySourceModel() override;

	QModelIndex index(
		int row,
		int column,
		const QModelIndex& parent = {}) const override;
	QModelIndex parent(
		const QModelIndex& child) const override;
	int rowCount(
		const QModelIndex& parent = {}) const override;
	int columnCount(
		const QModelIndex& parent = {}) const override;
	QVariant data(
		const QModelIndex& index,
		int role = Qt::DisplayRole) const override;
	Qt::ItemFlags flags(const QModelIndex& index) const override;

	std::optional<imagery::ImagerySourceHandle> sourceHandle(
		const QModelIndex& index) const;
	QModelIndex indexForHandle(
		const imagery::ImagerySourceHandle& handle) const;

private:
	struct Node;

	void rebuild();
	Node* node(const QModelIndex& index) const;

	imagery::ImageryCatalogRepository& repository_;
	imagery::ImageryCatalogRepositorySnapshotPtr snapshot_;
	std::vector<std::unique_ptr<Node>> roots_;
};

}  // namespace OpenOrienteering

#endif
