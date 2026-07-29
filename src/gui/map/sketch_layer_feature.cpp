/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "gui/map/sketch_layer_feature.h"

#include <QAction>
#include <QCryptographicHash>
#include <QDialog>
#include <QDialogButtonBox>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QVBoxLayout>
#include <QUuid>

#include "settings.h"
#include "collaboration/map_hub_read_only_document.h"
#include "core/document_path.h"
#include "core/map.h"
#include "core/map_part.h"
#include "core/sketch_layer.h"
#include "core/sketch_layer_sidecar.h"
#include "gui/main_window.h"
#include "gui/map/map_editor.h"
#include "tools/pan_tool.h"
#include "tools/sketch_tool.h"
#include "undo/undo_manager.h"

namespace OpenOrienteering {

namespace {

QString serverIdentityKey(const QString& server)
{
	return QString::fromLatin1(
	    QCryptographicHash::hash(
	        server.toUtf8(), QCryptographicHash::Sha256).toHex());
}

}  // namespace

SketchLayerFeature::SketchLayerFeature(MapEditorController& controller)
 : controller(controller)
{
	sketch_action = controller.newCheckAction(
	    "sketch", tr("Sketch"), nullptr, nullptr, "pencil",
	    tr("Draw seamless field notes on a single map-owned sketch layer."));
	connect(sketch_action, &QAction::triggered,
	        this, &SketchLayerFeature::sketchClicked);
	connect(controller.getMap(), &Map::editCommitted, this,
	        [this] { persistReadOnlySidecar(); });
}

SketchLayerFeature::~SketchLayerFeature() = default;

void SketchLayerFeature::setEnabled(bool enabled)
{
	sketch_action->setEnabled(enabled);
}

QAction* SketchLayerFeature::sketchAction() const noexcept
{
	return sketch_action;
}

void SketchLayerFeature::sketchClicked(bool checked)
{
	if (checked)
		startSketching();
	else
		finishSketching();
}

void SketchLayerFeature::startSketching()
{
	resolveUserIdentity();
	loadReadOnlySidecar();
	if (!chooseLayerForToday())
	{
		sketch_action->setChecked(false);
		return;
	}
	auto* layer = SketchLayer::findById(
	    *controller.getMap(), selected_layer_id);
	if (!layer || layer->sketchOwnerId() != current_user_id)
	{
		sketch_action->setChecked(false);
		return;
	}
	auto* tool = qobject_cast<SketchTool*>(controller.getTool());
	if (!tool)
	{
		tool = new SketchTool(&controller, sketch_action);
		controller.setTool(tool);
	}
	tool->setLayer(layer, historyFor(layer));
	tool->setChooseLayerCallback([this] {
		layer_choice_date = {};
		if (!chooseLayerForToday())
			return;
		if (auto* active =
		        qobject_cast<SketchTool*>(controller.getTool()))
		{
			auto* selected = SketchLayer::findById(
			    *controller.getMap(), selected_layer_id);
			active->setLayer(selected, historyFor(selected));
		}
	});
	sketch_action->setChecked(true);
}

UndoManager* SketchLayerFeature::historyFor(MapPart* layer)
{
	if (!layer)
		return nullptr;
	const auto id = layer->persistentId();
	auto found = layer_histories.find(id);
	if (found != layer_histories.end())
		return found->second.get();
	auto history = std::make_unique<UndoManager>(controller.getMap());
	connect(history.get(), &UndoManager::editCommitted,
	        controller.getMap(), &Map::editCommitted);
	auto* result = history.get();
	layer_histories.emplace(id, std::move(history));
	return result;
}

void SketchLayerFeature::finishSketching()
{
	if (qobject_cast<SketchTool*>(controller.getTool()))
		controller.setTool(
		    new PanTool(&controller, controller.pan_act));
}

void SketchLayerFeature::resolveUserIdentity()
{
	if (!current_user_id.isEmpty())
		return;
	QSettings settings;
	const auto server = Settings::getInstance()
	                        .getSetting(Settings::MapHub_ServerUrl)
	                        .toString();
	const auto account_key =
	    QStringLiteral("FieldSketches/MapHub/%1/")
	        .arg(serverIdentityKey(server));
	current_user_id =
	    settings.value(account_key + QStringLiteral("person_id")).toString();
	current_user_name =
	    settings.value(account_key + QStringLiteral("display_name")).toString();
	if (!current_user_id.isEmpty())
		return;

	const auto device_key =
	    QStringLiteral("FieldSketches/device_user_id");
	current_user_id = settings.value(device_key).toString();
	if (QUuid(current_user_id).isNull())
	{
		current_user_id =
		    QUuid::createUuid().toString(QUuid::WithoutBraces);
		settings.setValue(device_key, current_user_id);
	}
	current_user_name = tr("Me");
}

void SketchLayerFeature::loadReadOnlySidecar()
{
	if (sidecar_loaded)
		return;
	sidecar_loaded = true;
	if (!controller.isReadOnly() || !controller.getWindow())
		return;

	QString error;
	const auto document = MapHubReadOnlyDocument::loadForMap(
	    controller.getWindow()->currentPath(), &error);
	if (!document.isValid())
		return;
	sidecar_storage_key =
	    QStringLiteral("map-hub:%1:%2")
	        .arg(document.server_url, document.project_id);
	if (!SketchLayerSidecar::load(
	        *controller.getMap(), sidecar_storage_key, &error))
	{
		QMessageBox::warning(
		    controller.getWindow(), tr("Could not load field sketches"),
		    error);
		return;
	}
	controller.getMap()->undoManager().setClean();
	controller.getMap()->setHasUnsavedChanges(false);
	controller.getWindow()->setHasUnsavedChanges(false);
}

QString SketchLayerFeature::uniqueDefaultLayerName() const
{
	const auto base = SketchLayer::defaultLayerName(QDate::currentDate());
	QString name = base;
	int suffix = 2;
	auto exists = [this](const QString& candidate) {
		for (const auto* layer :
		     SketchLayer::all(*controller.getMap()))
			if (layer->getName() == candidate)
				return true;
		return false;
	};
	while (exists(name))
		name = tr("%1 (%2)").arg(base).arg(suffix++);
	return name;
}

bool SketchLayerFeature::chooseLayerForToday()
{
	const auto today = QDate::currentDate();
	if (layer_choice_date == today)
	{
		auto* selected = SketchLayer::findById(
		    *controller.getMap(), selected_layer_id);
		if (selected && selected->sketchOwnerId() == current_user_id)
			return true;
	}

	for (auto* layer : SketchLayer::all(*controller.getMap()))
	{
		if (!layer->hasSketchLayerMetadata())
			layer->setSketchLayerMetadata(
			    current_user_id, current_user_name, today);
	}

	QDialog dialog(controller.getWindow());
	dialog.setWindowTitle(tr("Choose a sketch layer"));
	auto* layout = new QVBoxLayout(&dialog);
	auto* introduction = new QLabel(
	    tr("Sketches stay separate from the map. You can draw only on "
	       "layers created by your account."),
	    &dialog);
	introduction->setWordWrap(true);
	layout->addWidget(introduction);
	auto* list = new QListWidget(&dialog);
	list->setSelectionMode(QAbstractItemView::SingleSelection);
	list->setMinimumHeight(180);
	for (const auto* layer : SketchLayer::all(*controller.getMap()))
	{
		const auto mine = layer->sketchOwnerId() == current_user_id;
		auto text = layer->getName();
		if (!mine)
			text += tr("\nCreated by %1")
			            .arg(layer->sketchOwnerName().isEmpty()
			                 ? tr("another mapper")
			                 : layer->sketchOwnerName());
		auto* item = new QListWidgetItem(text, list);
		item->setData(Qt::UserRole, layer->persistentId());
		if (!mine)
			item->setFlags(item->flags() & ~Qt::ItemIsSelectable);
		else if (layer->persistentId() == selected_layer_id)
			list->setCurrentItem(item);
	}
	if (list->count() == 0)
	{
		auto* empty = new QListWidgetItem(
		    tr("No sketch layers yet"), list);
		empty->setFlags(Qt::NoItemFlags);
	}
	layout->addWidget(list);

	auto* buttons = new QDialogButtonBox(
	    QDialogButtonBox::Ok | QDialogButtonBox::Cancel, &dialog);
	auto* create = buttons->addButton(
	    tr("New layer"), QDialogButtonBox::ActionRole);
	buttons->button(QDialogButtonBox::Ok)->setEnabled(
	    list->currentItem()
	    && list->currentItem()->flags().testFlag(Qt::ItemIsSelectable));
	connect(list, &QListWidget::currentItemChanged, &dialog,
	        [buttons](QListWidgetItem* item) {
		        buttons->button(QDialogButtonBox::Ok)->setEnabled(
		            item
		            && item->flags().testFlag(Qt::ItemIsSelectable));
	        });
	connect(buttons, &QDialogButtonBox::accepted,
	        &dialog, &QDialog::accept);
	connect(buttons, &QDialogButtonBox::rejected,
	        &dialog, &QDialog::reject);
	QString created_layer_id;
	connect(create, &QPushButton::clicked, &dialog, [&] {
		bool accepted = false;
		auto name = QInputDialog::getText(
		    &dialog, tr("New sketch layer"), tr("Layer name:"),
		    QLineEdit::Normal, uniqueDefaultLayerName(), &accepted);
		name = name.trimmed();
		if (!accepted || name.isEmpty())
			return;
		auto* layer = SketchLayer::create(
		    *controller.getMap(), name, current_user_id,
		    current_user_name, today);
		created_layer_id = layer->persistentId();
		persistReadOnlySidecar();
		dialog.accept();
	});

	layout->addWidget(buttons);
	if (dialog.exec() != QDialog::Accepted)
		return false;
	if (!created_layer_id.isEmpty())
		selected_layer_id = created_layer_id;
	else
	{
		const auto* item = list->currentItem();
		if (!item
		    || !item->flags().testFlag(Qt::ItemIsSelectable))
			return false;
		selected_layer_id =
		    item->data(Qt::UserRole).toString();
	}
	layer_choice_date = today;
	return true;
}

void SketchLayerFeature::persistReadOnlySidecar()
{
	if (sidecar_storage_key.isEmpty())
		return;
	QString error;
	if (!SketchLayerSidecar::save(
	        *controller.getMap(), sidecar_storage_key, &error))
	{
		if (controller.getWindow())
			controller.getWindow()->showStatusBarMessage(
			    tr("Could not save field sketches: %1").arg(error),
			    12000);
		return;
	}
	controller.getMap()->undoManager().setClean();
	controller.getMap()->setHasUnsavedChanges(false);
	if (controller.getWindow())
		controller.getWindow()->setHasUnsavedChanges(false);
}

}  // namespace OpenOrienteering
