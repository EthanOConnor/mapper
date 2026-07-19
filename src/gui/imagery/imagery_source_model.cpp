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

#include "gui/imagery/imagery_source_model.h"

#include <algorithm>

#include <QCoreApplication>
#include <QIcon>
#include <QPalette>
#include <QStringList>
#include <QUrl>

#include "gui/action_icon.h"

namespace OpenOrienteering {

namespace {

QString displayOrigin(QString value)
{
	auto url = QUrl(value);
	if (!url.isValid() || url.scheme().isEmpty())
		return value;
	url.setUserName(QString {});
	url.setPassword(QString {});
	url.setQuery(QString {});
	url.setFragment(QString {});
	return url.toDisplayString(QUrl::RemovePassword);
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

struct ImagerySourceModel::Node
{
	NodeType type = NodeType::Catalog;
	Node* parent = nullptr;
	int row = 0;
	QString name;
	QString tooltip;
	QString search_text;
	QString status;
	bool supported = true;
	std::optional<imagery::ImagerySourceHandle> handle;
	std::vector<std::unique_ptr<Node>> children;
};


ImagerySourceModel::ImagerySourceModel(
	imagery::ImageryCatalogRepository& repository,
	QObject* parent)
 : QAbstractItemModel(parent)
 , repository_(repository)
{
	connect(
		&repository_,
		&imagery::ImageryCatalogRepository::snapshotChanged,
		this,
		[this] { rebuild(); });
	rebuild();
}


ImagerySourceModel::~ImagerySourceModel() = default;


void ImagerySourceModel::rebuild()
{
	beginResetModel();
	snapshot_ = repository_.snapshot();
	roots_.clear();
	if (snapshot_)
	{
		roots_.reserve(snapshot_->catalogs.size());
		for (auto const& installed : snapshot_->catalogs)
		{
			auto catalog = std::make_unique<Node>();
			catalog->type = NodeType::Catalog;
			catalog->row = int(roots_.size());
			catalog->name =
				installed.read_result.catalog.name.isEmpty()
					? installed.read_result.catalog.id
					: installed.read_result.catalog.name;
			catalog->status = tr("Revision %1 · %n source(s)", nullptr,
			                     installed.read_result.catalog.sources.size())
				.arg(installed.read_result.catalog.revision);
			catalog->tooltip = tr("%1\nInstalled from %2")
				.arg(
					installed.read_result.catalog.id,
					displayOrigin(installed.state.origin));
			catalog->search_text =
				catalog->name + QLatin1Char(' ')
				+ installed.read_result.catalog.id + QLatin1Char(' ')
				+ installed.read_result.catalog.description;

			for (qsizetype source_index = 0;
			     source_index
			       < installed.read_result.catalog.sources.size();
			     ++source_index)
			{
				auto const& definition =
					installed.read_result.catalog.sources.at(
						source_index);
				auto source = std::make_unique<Node>();
				source->type = NodeType::Source;
				source->parent = catalog.get();
				source->row = int(catalog->children.size());
				source->name = definition.metadata.name.isEmpty()
					? definition.metadata.id
					: definition.metadata.name;
				if (source->name.isEmpty())
				{
					source->name = tr("Invalid source %1")
						.arg(source_index + 1);
				}
				source->supported =
					definition.valid && definition.supported
					&& definition.resolved_source.has_value();
				auto const use_index_identity =
					!definition.valid
					|| definition.metadata.id.isEmpty();
				source->handle = imagery::ImagerySourceHandle {
					installed.read_result.catalog.id,
					use_index_identity
						? QString {}
						: definition.metadata.id,
					installed.state.sha256,
					use_index_identity
						? int(source_index)
						: -1,
				};
				source->search_text =
					source->name + QLatin1Char(' ')
					+ definition.metadata.id + QLatin1Char(' ')
					+ definition.metadata.description + QLatin1Char(' ')
					+ imagery::categoryName(
						definition.metadata.category);
				if (source->supported)
				{
					auto const& resolved =
						*definition.resolved_source;
					source->status = tr("%1 · zoom %2–%3")
						.arg(
							resolved.tile_matrix_set.crs)
						.arg(resolved.min_zoom)
						.arg(resolved.max_zoom);
					source->tooltip =
						definition.metadata.description;
				}
				else
				{
					source->status = tr("Not supported");
					QStringList reasons =
						definition.unsupported_capabilities;
					for (auto const& diagnostic
					     : installed.read_result.diagnostics)
					{
						if (diagnostic.source_index
						      == source->row
						    && (diagnostic.kind
						          == imagery::OicDiagnosticKind::
							         UnsupportedSource
						        || diagnostic.kind
						          == imagery::OicDiagnosticKind::
							         SourceError))
						{
							reasons.push_back(
								diagnostic.message);
						}
					}
					reasons.removeDuplicates();
					source->tooltip = reasons.isEmpty()
						? tr("This source cannot be used by this build.")
						: reasons.join(QLatin1Char('\n'));
				}
				catalog->children.push_back(std::move(source));
			}
			std::stable_sort(
				catalog->children.begin(),
				catalog->children.end(),
				[](auto const& left, auto const& right) {
					return QString::localeAwareCompare(
						left->name,
						right->name) < 0;
				});
			for (int row = 0;
			     row < int(catalog->children.size());
			     ++row)
				catalog->children[row]->row = row;
			roots_.push_back(std::move(catalog));
		}
	}
	endResetModel();
}


ImagerySourceModel::Node* ImagerySourceModel::node(
	const QModelIndex& index) const
{
	return index.isValid()
		? static_cast<Node*>(index.internalPointer())
		: nullptr;
}


QModelIndex ImagerySourceModel::index(
	int row,
	int column,
	const QModelIndex& parent) const
{
	if (column != 0 || row < 0)
		return {};
	auto* parent_node = node(parent);
	if (!parent_node)
	{
		if (row >= int(roots_.size()))
			return {};
		return createIndex(row, column, roots_[row].get());
	}
	if (parent_node->type != NodeType::Catalog
	    || row >= int(parent_node->children.size()))
		return {};
	return createIndex(
		row,
		column,
		parent_node->children[row].get());
}


QModelIndex ImagerySourceModel::parent(
	const QModelIndex& child) const
{
	auto* child_node = node(child);
	if (!child_node || !child_node->parent)
		return {};
	auto* parent_node = child_node->parent;
	return createIndex(
		parent_node->row,
		0,
		parent_node);
}


int ImagerySourceModel::rowCount(
	const QModelIndex& parent) const
{
	auto* parent_node = node(parent);
	if (!parent_node)
		return int(roots_.size());
	return parent_node->type == NodeType::Catalog
		? int(parent_node->children.size())
		: 0;
}


int ImagerySourceModel::columnCount(
	const QModelIndex&) const
{
	return 1;
}


QVariant ImagerySourceModel::data(
	const QModelIndex& index,
	int role) const
{
	auto* item = node(index);
	if (!item)
		return {};
	switch (role)
	{
	case Qt::DisplayRole:
		return item->name;
	case Qt::ToolTipRole:
		return plainTextToolTip(item->tooltip);
	case Qt::DecorationRole:
		if (item->type == NodeType::Catalog)
			return ActionIcon::fromName(u"folder");
		return item->supported
			? ActionIcon::fromName(u"image")
			: ActionIcon::fromName(u"warning");
	case NodeTypeRole:
		return int(item->type);
	case SourceHandleRole:
		return item->handle
			? QVariant::fromValue(*item->handle)
			: QVariant {};
	case SupportedRole:
		return item->supported;
	case SearchTextRole:
		return item->search_text;
	case StatusTextRole:
		return item->status;
	case Qt::ForegroundRole:
		if (!item->supported
		    && item->type == NodeType::Source)
			return QPalette().brush(
				QPalette::Disabled,
				QPalette::Text);
		break;
	default:
		break;
	}
	return {};
}


Qt::ItemFlags ImagerySourceModel::flags(
	const QModelIndex& index) const
{
	auto* item = node(index);
	if (!item)
		return Qt::NoItemFlags;
	return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}


std::optional<imagery::ImagerySourceHandle>
ImagerySourceModel::sourceHandle(
	const QModelIndex& index) const
{
	auto* item = node(index);
	return item ? item->handle : std::nullopt;
}


QModelIndex ImagerySourceModel::indexForHandle(
	const imagery::ImagerySourceHandle& handle) const
{
	for (auto const& catalog : roots_)
	{
		for (auto const& source : catalog->children)
		{
			if (source->handle && *source->handle == handle)
				return createIndex(
					source->row,
					0,
					source.get());
		}
	}
	return {};
}

}  // namespace OpenOrienteering
