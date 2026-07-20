/*
 *    Copyright 2020 Kai Pastor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "template_table_model.h"
#include "gui/action_icon.h"

#include <Qt>
#include <QtGlobal>
#include <QApplication>
#include <QBrush>
#include <QColor>
#include <QCoreApplication>
#include <QFlags>
#include <QIcon>
#include <QLatin1String>
#include <QLocale>
#include <QModelIndex>
#include <QPalette>
#include <QStringList>
#include <QStyle>

#include "core/map.h"
#include "core/map_view.h"
#include "templates/template.h"


namespace {

inline
Qt::CheckState toCheckState(const QVariant& value)
{
	return value.toBool() ? Qt::Checked : Qt::Unchecked;
}

QColor paletteColor(OpenOrienteering::MapView const& view, QPalette::ColorRole role) {
	auto const group = view.areAllTemplatesHidden() ? QPalette::Disabled : QPalette::Active;
	return QPalette().color(group, role);
}

/**
 * Provides unique combinations of column index and item data role.
 * 
 * This utility function allows more compact expressions (switch statements)
 * in the functions related item data.
 */
constexpr int combined(int column, Qt::ItemDataRole role)
{
	return int(role) * 256 + column + 256;
}

QString resourceStatusText(const OpenOrienteering::TemplateResourceStatus& status)
{
	QStringList parts;
	auto const countText = [](qsizetype count) {
		return QLocale().toString(static_cast<qlonglong>(count));
	};
	if (status.ready_resources > 0)
		parts.push_back(
			QCoreApplication::translate(
				"OpenOrienteering::TemplateTableModel", "%1 ready")
				.arg(countText(status.ready_resources)));
	if (status.loading_resources > 0)
		parts.push_back(
			QCoreApplication::translate(
				"OpenOrienteering::TemplateTableModel", "%1 loading")
				.arg(countText(status.loading_resources)));
	if (status.offline_resources > 0)
		parts.push_back(
			QCoreApplication::translate(
				"OpenOrienteering::TemplateTableModel",
				"%1 waiting for network")
				.arg(countText(status.offline_resources)));
	if (status.transient_failures > 0)
		parts.push_back(
			QCoreApplication::translate(
				"OpenOrienteering::TemplateTableModel",
				"%1 retrying after an error")
				.arg(countText(status.transient_failures)));
	if (status.permanent_failures > 0)
		parts.push_back(
			QCoreApplication::translate(
				"OpenOrienteering::TemplateTableModel",
				"%1 unavailable")
				.arg(countText(status.permanent_failures)));

	QString text;
	if (!parts.isEmpty())
	{
		auto const separator = QCoreApplication::translate(
			"OpenOrienteering::TemplateTableModel", ", ");
		text = QCoreApplication::translate(
			"OpenOrienteering::TemplateTableModel", "Resources: %1")
			.arg(parts.join(separator));
	}
	if (!status.message.isEmpty())
	{
		if (!text.isEmpty())
			text += QLatin1Char('\n');
		text += status.message;
	}
	return text;
}

QIcon resourceStatusIcon(
	const OpenOrienteering::TemplateResourceStatus& status)
{
	using Condition =
		OpenOrienteering::TemplateResourceStatus::Condition;

	if (!status.isReported())
		return {};

	auto* application =
		qobject_cast<QApplication*>(QCoreApplication::instance());
	if (!application)
		return {};

	auto* style = application->style();
	auto standard_pixmap = QStyle::SP_MessageBoxInformation;
	QStyle::StandardPixmap fallback = QStyle::SP_MessageBoxInformation;
	switch (status.dominantCondition())
	{
	case Condition::Ready:
		standard_pixmap = QStyle::SP_DialogApplyButton;
		break;
	case Condition::Loading:
		standard_pixmap = QStyle::SP_BrowserReload;
		break;
	case Condition::Offline:
		standard_pixmap = QStyle::SP_DriveNetIcon;
		break;
	case Condition::TransientFailure:
		standard_pixmap = QStyle::SP_MessageBoxWarning;
		break;
	case Condition::PermanentFailure:
		standard_pixmap = QStyle::SP_MessageBoxCritical;
		fallback = QStyle::SP_MessageBoxCritical;
		break;
	}

	auto icon = style->standardIcon(standard_pixmap);
	if (icon.isNull())
		icon = style->standardIcon(fallback);
	if (icon.isNull())
	{
			icon =
				status.dominantCondition() == Condition::PermanentFailure
					? OpenOrienteering::ActionIcon::fromName(u"close")
					: OpenOrienteering::ActionIcon::fromName(u"info");
	}
	return icon;
}

}


namespace OpenOrienteering {

TemplateTableModel::~TemplateTableModel() = default;

TemplateTableModel::TemplateTableModel(Map& map, MapView& view, QObject* parent)
: QAbstractTableModel(parent)
, map(map)
, view(view)
{
	connect(&map, &Map::firstFrontTemplateAboutToBeChanged, this, &TemplateTableModel::onFirstFrontTemplateAboutToBeChanged);
	connect(&map, &Map::firstFrontTemplateChanged, this, &TemplateTableModel::onFirstFrontTemplateChanged);
	connect(&map, &Map::templateAboutToBeAdded, this, &TemplateTableModel::onTemplateAboutToBeAdded);
	connect(&map, &Map::templateAdded, this, &TemplateTableModel::onTemplateAdded);
	connect(&map, &Map::templateChanged, this, &TemplateTableModel::onTemplateChanged);
	connect(&map, &Map::templateAboutToBeMoved, this, &TemplateTableModel::onTemplateAboutToBeMoved);
	connect(&map, &Map::templateMoved, this, &TemplateTableModel::onTemplateMoved);
	connect(&map, &Map::templateAboutToBeDeleted, this, &TemplateTableModel::onTemplateAboutToBeDeleted);
	connect(&map, &Map::templateDeleted, this, &TemplateTableModel::onTemplateDeleted);
	
	for (auto i = 0, last = map.getNumTemplates(); i < last; ++i)
	{
		auto* temp = map.getTemplate(i);
		connect(temp, &Template::templateStateChanged, this, &TemplateTableModel::onTemplateStateChanged);
		connect(temp, &Template::resourceStatusChanged, this, &TemplateTableModel::onTemplateResourceStatusChanged);
	}
}


void TemplateTableModel::setCheckBoxDecorator(QVariant decorator)
{
	checkbox_decorator = decorator;
	emit dataChanged(this->index(0, visibilityColumn()), this->index(rowCount() - 1, visibilityColumn()));
}


void TemplateTableModel::setTouchMode(bool enabled)
{
	touch_mode = enabled;
	emit dataChanged(this->index(0, 0), this->index(rowCount() - 1, columnCount() - 1));
}


int TemplateTableModel::rowCount(const QModelIndex& parent) const
{
	return parent.isValid() ? 0 : map.getNumTemplates() + 1;
}

int TemplateTableModel::columnCount(const QModelIndex& /*parent*/) const
{
	return 4;
}


int TemplateTableModel::posFromRow(int row) const
{
	auto pos = map.getNumTemplates() - row;
	if (pos == map.getFirstFrontTemplate())
		pos = -1; // the map row
	else if (pos > map.getFirstFrontTemplate())
		--pos; // before the map row 
	
	return pos;
}

int TemplateTableModel::rowFromPos(int pos) const
{
	Q_ASSERT(pos >= 0);
	
	auto row = map.getNumTemplates() - pos;
	if (pos >= map.getFirstFrontTemplate())
		--row;
	
	return row;
}

int TemplateTableModel::insertionRowFromPos(int pos, bool in_background) const
{
	Q_ASSERT(pos >= 0);
	
	auto row = map.getNumTemplates() - pos;
	if (in_background)
		++row;
	
	return row;
}

int TemplateTableModel::mapRow(int first_front_template) const
{
	return map.getNumTemplates() - first_front_template;
}


QVariant TemplateTableModel::headerData(int section, Qt::Orientation orientation, int role) const
{
	if (orientation == Qt::Horizontal)
	{
		switch (section)
		{
		case visibilityColumn():
			if (role == Qt::ToolTipRole)
				return QCoreApplication::translate("OpenOrienteering::TemplateListWidget", "Show");
			if (role == Qt::DecorationRole)
				return checkBoxDecorator();
			break;
		case opacityColumn():
			if (role == Qt::DisplayRole)
				return QCoreApplication::translate("OpenOrienteering::TemplateListWidget", "Opacity");
			break;
		case groupColumn():
			if (role == Qt::DisplayRole)
				return QCoreApplication::translate("OpenOrienteering::TemplateListWidget", "Group");
			break;
		case nameColumn():
			if (role == Qt::DisplayRole)
				return QCoreApplication::translate("OpenOrienteering::TemplateListWidget", "Filename");
			break;
		}
	}
	return {};
}

Qt::ItemFlags TemplateTableModel::flags(const QModelIndex &index) const
{
	auto pos = posFromRow(index.row());
	auto* temp = pos >= 0 ? map.getTemplate(pos) : nullptr;
	auto visibility = temp ? view.getTemplateVisibility(temp) : view.getMapVisibility();
	switch (index.column())
	{
	case visibilityColumn():
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsUserCheckable;
	case opacityColumn():
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable | ((visibility.visible && !touchMode()) ? Qt::ItemIsEditable : Qt::NoItemFlags);
	default:
		return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
	}
}

QVariant TemplateTableModel::data(const QModelIndex &index, int role) const
{
	auto const pos = posFromRow(index.row());
	return pos < 0 ? mapData(index, role) : templateData(map.getTemplate(pos), index, role);
}

bool TemplateTableModel::setData(const QModelIndex& index, const QVariant& value, int role)
{
	auto const pos = posFromRow(index.row());
	return pos < 0 ? setMapData(index, value, role) : setTemplateData(map.getTemplate(pos), index, value, role);
}


QVariant TemplateTableModel::mapData(const QModelIndex &index, int role) const
{
	if (role == Qt::BackgroundRole)
	{
#ifdef Q_OS_ANDROID
		auto background_color = paletteColor(view, QPalette::Window);
		auto r = (128 + 5 * background_color.red()) / 6;
		auto g = (128 + 5 * background_color.green()) / 6;
		auto b = (128 + 5 * background_color.blue()) / 6;
		return QBrush(QColor(r, g, b));
#else
		return QBrush(paletteColor(view, QPalette::AlternateBase));
#endif
	}
	
	auto visibility = view.getMapVisibility();
	if (role == Qt::ForegroundRole)
	{
		if (visibility.visible)
			return QBrush(paletteColor(view, QPalette::WindowText));
		return QBrush(QPalette().color(QPalette::Disabled, QPalette::WindowText));
	}
	
	switch (combined(index.column(), Qt::ItemDataRole(role)))
	{
	case combined(visibilityColumn(), Qt::CheckStateRole):
		return toCheckState(visibility.visible);
		
	case combined(opacityColumn(), Qt::DisplayRole):
	case combined(opacityColumn(), Qt::EditRole):
		return visibility.opacity;
		
	case combined(opacityColumn(), Qt::DecorationRole):
		if (visibility.visible)
			return QColor::fromCmykF(0, 0, 0, visibility.opacity);
		return QColor{Qt::transparent};
		
	case combined(visibilityColumn(), Qt::DisplayRole):
		if (!touchMode())
			break;
		Q_FALLTHROUGH();
	case combined(nameColumn(), Qt::DisplayRole):
		return QCoreApplication::translate("OpenOrienteering::TemplateListWidget", "- Map -");
	}
	return {};
}

QVariant TemplateTableModel::templateData(Template* temp, const QModelIndex &index, int role) const
{
	if (role == ResourceStatusRole)
		return QVariant::fromValue(temp->resourceStatus());
	if (role == ResourceStatusTextRole)
		return resourceStatusText(temp->resourceStatus());

	if (role == Qt::BackgroundRole)
	{
#ifdef Q_OS_ANDROID
		return QBrush(paletteColor(view, QPalette::Window));
#else
		return QBrush(paletteColor(view, QPalette::Base));
#endif
	}
	
	auto visibility = view.getTemplateVisibility(temp);
	if (role == Qt::ForegroundRole)
	{
		auto text_color = QColor::fromRgb(255, 51, 51);
		if (temp->getTemplateState() == Template::Invalid)
		{
			if (visibility.visible)
				text_color = text_color.darker();
		}
		else if (visibility.visible)
			text_color = paletteColor(view, QPalette::WindowText);
		else
			text_color = QPalette().color(QPalette::Disabled, QPalette::WindowText);
		return QBrush(text_color);
	}
	
	switch (combined(index.column(), Qt::ItemDataRole(role)))
	{
	case combined(visibilityColumn(), Qt::CheckStateRole):
		if (visibility.visible && temp->getTemplateState() != Template::Loaded)
			return Qt::PartiallyChecked;
		return toCheckState(visibility.visible);
		
	case combined(opacityColumn(), Qt::DisplayRole):
	case combined(opacityColumn(), Qt::EditRole):
		return visibility.opacity;
		
	case combined(opacityColumn(), Qt::DecorationRole):
		if (temp->getTemplateState() == Template::Invalid)
				return QIcon::fromTheme(QLatin1String("image-missing"), ActionIcon::fromName(u"close"));
		if (visibility.visible)
			return QColor::fromCmykF(0, 0, 0, visibility.opacity);
		return QColor{Qt::transparent};

	case combined(nameColumn(), Qt::DecorationRole):
		return resourceStatusIcon(temp->resourceStatus());

	case combined(visibilityColumn(), Qt::DecorationRole):
		if (touchMode())
			return resourceStatusIcon(temp->resourceStatus());
		break;
		
	case combined(visibilityColumn(), Qt::DisplayRole):
		if (!touchMode())
			break;
		Q_FALLTHROUGH();
	case combined(nameColumn(), Qt::DisplayRole):
		return temp->getTemplateFilename();
		
	case combined(visibilityColumn(), Qt::ToolTipRole):
		if (!touchMode())
			break;
		Q_FALLTHROUGH();
	case combined(nameColumn(), Qt::ToolTipRole):
	{
		auto tooltip = temp->getTemplatePath();
		auto const resource_status_text =
			resourceStatusText(temp->resourceStatus());
		if (!resource_status_text.isEmpty())
		{
			if (!tooltip.isEmpty())
				tooltip += QLatin1Char('\n');
			tooltip += resource_status_text;
		}
		return tooltip;
	}
	}

	auto const is_display_column =
		index.column() == nameColumn()
		|| (touchMode() && index.column() == visibilityColumn());
	if (!is_display_column)
		return {};
	if (role != Qt::AccessibleDescriptionRole
	    && role != Qt::StatusTipRole
	    && role != Qt::AccessibleTextRole)
	{
		return {};
	}

	auto const resource_status_text =
		resourceStatusText(temp->resourceStatus());
	if (role == Qt::AccessibleDescriptionRole
	    || role == Qt::StatusTipRole)
	{
		return resource_status_text;
	}
	if (role == Qt::AccessibleTextRole)
	{
		if (resource_status_text.isEmpty())
			return temp->getTemplateFilename();
		return QCoreApplication::translate(
			"OpenOrienteering::TemplateTableModel", "%1, %2")
			.arg(temp->getTemplateFilename(), resource_status_text);
	}
	return {};
}


bool TemplateTableModel::setMapData(const QModelIndex& index, const QVariant& value, int role)
{
	switch (index.column())
	{
	case visibilityColumn():
		if (role == Qt::CheckStateRole)
		{
			auto vis = view.getMapVisibility();
			vis.visible = value.toInt() == Qt::Checked;
			view.setMapVisibility(vis);
			// Visibility impacts enabled state of the other fields in the row.
			emit dataChanged(this->index(index.row(), 0), this->index(index.row(), columnCount() - 1));
			return true;
		}
		break;
		
	case opacityColumn():
		if (role == Qt::EditRole)
		{
			auto vis = view.getMapVisibility();
			vis.opacity = qBound(0.0f, value.toFloat(), 1.0f);
			view.setMapVisibility(vis);
			emit dataChanged(index, index);
			return true;
		}
	}
	return false;
}

bool TemplateTableModel::setTemplateData(Template* temp, const QModelIndex& index, const QVariant& value, int role)
{
	switch (index.column())
	{
	case visibilityColumn():
		if (role == Qt::CheckStateRole)
		{
			auto vis = view.getTemplateVisibility(temp);
			vis.visible = value.toInt() == Qt::Checked;
			view.setTemplateVisibility(temp, vis);
			// Visibility impacts enabled state of the other fields in the row.
			emit dataChanged(this->index(index.row(), 0), this->index(index.row(), columnCount() - 1));
			return true;
		}
		break;
	case opacityColumn():
		if (role == Qt::EditRole)
		{
			auto vis = view.getTemplateVisibility(temp);
			vis.opacity = qBound(0.0f, value.toFloat(), 1.0f);
			view.setTemplateVisibility(temp, vis);
			emit dataChanged(index, index);
			return true;
		}
		break;
	}
	return false;
}


void TemplateTableModel::onFirstFrontTemplateAboutToBeChanged(int old_pos, int new_pos)
{
	// In this table, pos actually identifies the map row.
	auto old_row = mapRow(old_pos);
	auto new_row = mapRow(new_pos);
	if (new_row > old_row)
		++new_row;
	beginMoveRows({}, old_row, old_row, {}, new_row);
}

void TemplateTableModel::onFirstFrontTemplateChanged(int old_pos, int new_pos)
{
	// In this table, pos actually identifies the map row.
	auto old_row = mapRow(old_pos);
	auto new_row = mapRow(new_pos);
	if (new_row > old_row)
		++new_row;
	moveRows({}, old_row, 1, {}, new_row);
	endMoveRows();
}

void TemplateTableModel::onTemplateAboutToBeAdded(int pos, Template* /*temp*/, bool in_background)
{
	auto const row = insertionRowFromPos(pos, in_background);
	beginInsertRows({}, row, row);
}

void TemplateTableModel::onTemplateAdded(int /*pos*/, Template* temp)
{
	endInsertRows();
	connect(temp, &Template::templateStateChanged, this, &TemplateTableModel::onTemplateStateChanged);
	connect(temp, &Template::resourceStatusChanged, this, &TemplateTableModel::onTemplateResourceStatusChanged);
}

void TemplateTableModel::onTemplateChanged(int pos, Template* temp)
{
	connect(temp, &Template::templateStateChanged, this, &TemplateTableModel::onTemplateStateChanged);
	connect(temp, &Template::resourceStatusChanged, this, &TemplateTableModel::onTemplateResourceStatusChanged);
	auto const row = rowFromPos(pos);
	emit dataChanged(index(row, 0), index(row, 3));
}

void TemplateTableModel::onTemplateAboutToBeMoved(int old_pos, int new_pos)
{
	auto old_row = rowFromPos(old_pos);
	auto new_row = rowFromPos(new_pos);
	if (new_row > old_row)
		++new_row;
	beginMoveRows({}, old_row, old_row, {}, new_row);
}

void TemplateTableModel::onTemplateMoved()
{
	endMoveRows();
}

void TemplateTableModel::onTemplateAboutToBeDeleted(int pos, Template* temp)
{
	disconnect(temp);
	beginRemoveRows({}, rowFromPos(pos), rowFromPos(pos));
}

void TemplateTableModel::onTemplateDeleted()
{
	endRemoveRows();
}

void TemplateTableModel::onTemplateStateChanged()
{
	auto pos = this->map.findTemplateIndex(qobject_cast<Template*>(sender()));
	if (pos >= 0)
	{
		auto const row = rowFromPos(pos);
		emit dataChanged(index(row, 0), index(row, 3));
	}
}

void TemplateTableModel::onTemplateResourceStatusChanged()
{
	auto pos = map.findTemplateIndex(qobject_cast<Template*>(sender()));
	if (pos < 0)
		return;

	auto const row = rowFromPos(pos);
	emit dataChanged(
		index(row, 0),
		index(row, columnCount() - 1),
		{
			Qt::DecorationRole,
			Qt::ToolTipRole,
			Qt::StatusTipRole,
			Qt::AccessibleTextRole,
			Qt::AccessibleDescriptionRole,
			ResourceStatusRole,
			ResourceStatusTextRole,
		});
}


}  // namespace OpenOrienteering
