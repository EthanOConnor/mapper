/*
 *    Copyright 2011-2013 Thomas Schöps
 *    Copyright 2012-2017 Kai Pastor
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


#include "new_map_dialog.h"
#include "gui/action_icon.h"

#include <utility>

#include <Qt>
#include <QtGlobal>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QFlags>
#include <QFormLayout>
#include <QIcon>
#include <QIntValidator>
#include <QLabel>
#include <QLatin1Char>
#include <QLatin1String>
#include <QList>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpacerItem>
#include <QStringList>
#include <QVariant>
#include <QVBoxLayout>

#include "fileformats/file_format.h"
#include "fileformats/file_format_registry.h"
#include "gui/file_dialog.h"
#include "gui/util_gui.h"
#include "util/util.h"

// IWYU pragma: no_forward_declare QLabel
// IWYU pragma: no_forward_declare QVBoxLayout


namespace OpenOrienteering {

NewMapDialog::NewMapDialog(QWidget* parent)
 : QDialog(parent, Qt::WindowSystemMenuHint | Qt::WindowTitleHint)
 , requirement_label(new QLabel(this))
{
	this->setWhatsThis(Util::makeWhatThis("new_map.html"));
	setWindowTitle(tr("Create new map"));
	
	auto form_layout = new QFormLayout();
	
	form_layout->addRow(new QLabel(tr("Choose the scale and symbol set for the new map.")));
	requirement_label->setWordWrap(true);
	requirement_label->hide();
	form_layout->addRow(requirement_label);
	
	form_layout->addItem(Util::SpacerItem::create(this));
	
	scale_combo = new QComboBox();
	scale_combo->setEditable(true);
	scale_combo->setValidator(new QIntValidator(1, 9999999, scale_combo));
	form_layout->addRow(tr("Scale:  1 : "), scale_combo); /// \todo Fix form layout dependency
	
	auto layout = new QVBoxLayout();
	layout->addLayout(form_layout);
	
	layout->addWidget(new QLabel(tr("Symbol sets:")));
	
	symbol_set_list = new QListWidget();
	layout->addWidget(symbol_set_list, 1);
	
	symbol_set_matching = new QCheckBox(tr("Only show symbol sets matching the selected scale"));
	layout->addWidget(symbol_set_matching);
	
	layout->addItem(Util::SpacerItem::create(this));
	
	auto button_box = new QDialogButtonBox(QDialogButtonBox::Cancel | QDialogButtonBox::Ok);
	create_button = button_box->button(QDialogButtonBox::Ok);
	create_button->setIcon(ActionIcon::fromName(u"arrow-right"));
	create_button->setText(tr("Create"));
	layout->addWidget(button_box);
	
	setLayout(layout);
	
	loadSymbolSetMap();
	for (auto& item : symbol_set_map)
	{
		if (item.first.toInt() != 0)
			scale_combo->addItem(item.first);
	}
	
	QSettings settings;
	settings.beginGroup(QString::fromLatin1("NewMapDialog"));
	const auto default_scale = settings.value(QString::fromLatin1("DefaultScale"), QVariant(10000)).toString();
	const auto matching = settings.value(QString::fromLatin1("OnlyMatchingSymbolSets"), QVariant(true)).toBool();
	settings.endGroup();
	
	scale_combo->setEditText(default_scale);
	symbol_set_matching->setChecked(matching);
	connect(scale_combo, &QComboBox::editTextChanged, this, &NewMapDialog::updateSymbolSetList);
	connect(symbol_set_list, &QListWidget::itemDoubleClicked, this, &NewMapDialog::symbolSetDoubleClicked);
	connect(symbol_set_matching, &QCheckBox::checkStateChanged, this, &NewMapDialog::updateSymbolSetList);
	connect(button_box, &QDialogButtonBox::rejected, this, &QDialog::reject);
	connect(button_box, &QDialogButtonBox::accepted, this, &NewMapDialog::createClicked);
	updateSymbolSetList();
}


NewMapDialog::~NewMapDialog() = default;



unsigned int NewMapDialog::getSelectedScale() const
{
	return scale_combo->currentText().toUInt();
}

QString NewMapDialog::getSelectedSymbolSetPath() const
{
	QListWidgetItem* item = symbol_set_list->currentItem();
	if (! item || ! item->data(Qt::UserRole).isValid())
	{
		// FIXME: add proper error handling for release builds, or remove.
		Q_ASSERT(false);
		return QString{};
	}

	return item->data(Qt::UserRole).toString();
}

void NewMapDialog::setInitialScale(unsigned int scale, bool locked)
{
	if (scale == 0)
		return;
	scale_combo->setEditText(QString::number(scale));
	updateSymbolSetList();
	scale_combo->setEnabled(!locked);
}

void NewMapDialog::setRequiredSymbolStandard(const QString& standard)
{
	required_symbol_standard = standard.trimmed();
	requirement_label->setVisible(!required_symbol_standard.isEmpty());
	requirement_label->setText(
	  tr("Map Hub requires symbol standard %1. Compatible installed symbol "
	     "sets are selectable below; a custom file must be confirmed.")
	    .arg(required_symbol_standard));
	updateSymbolSetList();
}

void NewMapDialog::accept()
{
	QSettings settings;
	settings.beginGroup(QString::fromLatin1("NewMapDialog"));
	settings.setValue(QString::fromLatin1("DefaultScale"), getSelectedScale());
	settings.setValue(QString::fromLatin1("OnlyMatchingSymbolSets"), symbol_set_matching->isChecked());
	settings.endGroup();
	
	QDialog::accept();
}

void NewMapDialog::updateSymbolSetList()
{
	QString scale = scale_combo->currentText();
	if (scale.toInt() == 0)
	{
		create_button->setEnabled(false);
		symbol_set_list->setEnabled(false);
		return;
	}
	
	create_button->setEnabled(true);
	symbol_set_list->setEnabled(true);
	symbol_set_list->clear();
		
	auto item = new QListWidgetItem(tr("Empty symbol set"));
	item->setData(Qt::UserRole, QVariant(QString{}));
	item->setIcon(ActionIcon::fromName(u"new"));
	symbol_set_list->addItem(item);
	if (!required_symbol_standard.isEmpty())
		item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
	
	const auto control = ActionIcon::fromName(u"control");
	auto it = symbol_set_map.find(scale);
	if (it != symbol_set_map.end())
	{
		for (auto&& symbol_set : it->second)
		{
			item = new QListWidgetItem(symbol_set.completeBaseName());
			item->setData(Qt::UserRole, symbol_set.canonicalFilePath());
			item->setIcon(control);
			if (!required_symbol_standard.isEmpty()
			    && !symbol_set.completeBaseName().contains(required_symbol_standard, Qt::CaseInsensitive))
				item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
			symbol_set_list->addItem(item);
		}
	}
	
	if (! symbol_set_matching->isChecked())
	{
		for (it = symbol_set_map.begin(); it != symbol_set_map.end(); ++it )
		{
			if (it->first == scale) 
				continue;
			
			bool is_scale = (it->first.toInt() > 0);
			QString remark = QLatin1String(" (") + QLatin1String(is_scale ? ("1 : ") : "") + it->first + QLatin1Char(')');
			
			for (auto&& symbol_set : it->second)
			{
				item = new QListWidgetItem(symbol_set.completeBaseName() + remark);
				item->setData(Qt::UserRole, symbol_set.canonicalFilePath());
				item->setIcon(control);
				if (!required_symbol_standard.isEmpty()
				    && !symbol_set.completeBaseName().contains(required_symbol_standard, Qt::CaseInsensitive))
					item->setFlags(item->flags() & ~Qt::ItemIsEnabled);
				symbol_set_list->addItem(item);
			}
		}
	}
	
	load_from_file = new QListWidgetItem(tr("Load symbol set from a file..."));
	load_from_file->setData(Qt::UserRole, QVariant::fromValue<void*>(nullptr));
	load_from_file->setIcon(ActionIcon::fromName(u"open"));
	symbol_set_list->addItem(load_from_file);
	
	// Select the first enabled installed symbol set, falling back to the custom
	// file action when Map Hub requires a standard which is not installed.
	for (int row = 1; row < symbol_set_list->count(); ++row)
	{
		auto* candidate = symbol_set_list->item(row);
		if (candidate->flags().testFlag(Qt::ItemIsEnabled))
		{
			symbol_set_list->setCurrentItem(candidate);
			break;
		}
	}
}

void NewMapDialog::symbolSetDoubleClicked(QListWidgetItem* item)
{
	symbol_set_list->setCurrentItem(item);
	if (item == load_from_file)
		showFileDialog();
	else
		accept();
}

void NewMapDialog::createClicked()
{
	QListWidgetItem* item = symbol_set_list->currentItem();
	if (item == load_from_file)
		showFileDialog();
	else
		accept();
}

void NewMapDialog::showFileDialog()
{
	// Get the saved directory to start in, defaulting to the user's home directory.
	QString open_directory = QSettings().value(QString::fromLatin1("openFileDirectory"), QDir::homePath()).toString();
	
	// Build the list of supported file filters based on the file format registry
	QString filters, extensions;
	for (auto format : FileFormats.formats())
	{
		if (format->supportsFileOpen())
		{
			if (filters.isEmpty())
			{
				filters    = format->filter();
				extensions = QLatin1String("*.") + format->fileExtensions().join(QString::fromLatin1(" *."));
			}
			else
			{
				filters    = filters    + QLatin1String(";;")  + format->filter();
				extensions = extensions + QLatin1String(" *.") + format->fileExtensions().join(QString::fromLatin1(" *."));
			}
		}
	}
	filters = 
	  tr("All symbol set files") + QLatin1String(" (") + extensions + QLatin1String(");;") +
	  filters         + QLatin1String(";;") +
	  tr("All files") + QLatin1String(" (*.*)");

	QString path = FileDialog::getOpenFileName(this, tr("Load symbol set from a file..."), open_directory, filters);
	path = QFileInfo(path).canonicalFilePath();
	if (path.isEmpty())
		return;
	if (!required_symbol_standard.isEmpty()
	    && !QFileInfo(path).completeBaseName().contains(required_symbol_standard, Qt::CaseInsensitive)
	    && QMessageBox::question(
	         this, tr("Confirm symbol standard"),
	         tr("Map Hub requires %1, but the selected file name does not "
	            "identify that standard:\n%2\n\nUse this symbol set only if you have "
	            "verified that it implements %1.")
	           .arg(required_symbol_standard, path),
	         QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
		return;
	
	load_from_file->setData(Qt::UserRole, path);
	accept();
}

void NewMapDialog::loadSymbolSetMap()
{
	loadSymbolSetDir(QDir(QDir::homePath() + QLatin1String("/my symbol sets")));
	
	const auto locations = QDir::searchPaths(QLatin1String("data"));
	for (const auto& symbol_set_dir : locations)
	{
		loadSymbolSetDir(QDir(symbol_set_dir + QLatin1String("/symbol sets")));
	}
}

void NewMapDialog::loadSymbolSetDir(const QDir& symbol_set_dir)
{
	QStringList subdirs = symbol_set_dir.entryList(QDir::Dirs | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::NoSort);
	for (auto&& dir_name : subdirs)
	{
		//int scale = dir_name.toInt();
		//if (scale == 0)
		//{
		//	qDebug() << dir_name + ": not a valid map scale denominator, using it as group name.";
		//}
		
		QDir subdir(symbol_set_dir);
		if (!subdir.cd(dir_name))
		{
			qDebug("%s: cannot access this directory.", qPrintable(dir_name));
			continue;
		}
		
		QStringList symbol_set_filters;
		for (auto format : FileFormats.formats())
		{
			if (format->supportsFileOpen())
			{
				for (const auto& extension : format->fileExtensions())
					symbol_set_filters.append(QStringLiteral("*.") + extension);
			}
		}
		subdir.setNameFilters(symbol_set_filters);
		
		QFileInfoList symbol_set_files = subdir.entryInfoList(QDir::Files | QDir::Hidden | QDir::NoSymLinks | QDir::NoDotAndDotDot, QDir::Name);
		auto item = symbol_set_map.emplace(dir_name, QFileInfoList{}).first;
		item->second.append(symbol_set_files);
	}
}


}  // namespace OpenOrienteering
