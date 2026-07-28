/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_SKETCH_LAYER_FEATURE_H
#define OPENORIENTEERING_SKETCH_LAYER_FEATURE_H

#include <QObject>

class QAction;

namespace OpenOrienteering {

class MapEditorController;

/** Owns the map editor action which activates the seamless sketch layer. */
class SketchLayerFeature final : public QObject
{
	Q_OBJECT

public:
	explicit SketchLayerFeature(MapEditorController& controller);
	~SketchLayerFeature() override;

	void setEnabled(bool enabled);
	QAction* sketchAction() const noexcept;

private:
	void sketchClicked(bool checked);
	void startSketching();
	void finishSketching();

	MapEditorController& controller;
	QAction* sketch_action = nullptr;

	Q_DISABLE_COPY(SketchLayerFeature)
};

}  // namespace OpenOrienteering

#endif
