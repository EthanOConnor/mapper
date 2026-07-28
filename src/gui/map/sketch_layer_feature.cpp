/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "gui/map/sketch_layer_feature.h"

#include <QAction>

#include "core/map.h"
#include "core/sketch_layer.h"
#include "gui/map/map_editor.h"
#include "tools/sketch_tool.h"

namespace OpenOrienteering {

SketchLayerFeature::SketchLayerFeature(MapEditorController& controller)
 : controller(controller)
{
	sketch_action = controller.newCheckAction(
	    "sketch", tr("Sketch"), nullptr, nullptr, "pencil",
	    tr("Draw seamless field notes on a single map-owned sketch layer."));
	connect(sketch_action, &QAction::triggered,
	        this, &SketchLayerFeature::sketchClicked);
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
	if (controller.isReadOnly())
	{
		sketch_action->setChecked(false);
		return;
	}
	auto* layer = SketchLayer::ensure(*controller.getMap());
	auto* tool = qobject_cast<SketchTool*>(controller.getTool());
	if (!tool)
	{
		tool = new SketchTool(&controller, sketch_action);
		controller.setTool(tool);
	}
	tool->setLayer(layer);
	sketch_action->setChecked(true);
}

void SketchLayerFeature::finishSketching()
{
	if (auto* tool = qobject_cast<SketchTool*>(controller.getTool()))
		tool->deactivate();
}

}  // namespace OpenOrienteering
