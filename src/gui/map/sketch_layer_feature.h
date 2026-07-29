/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_SKETCH_LAYER_FEATURE_H
#define OPENORIENTEERING_SKETCH_LAYER_FEATURE_H

#include <QObject>
#include <QDate>
#include <QString>

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
	bool chooseLayerForToday();
	void loadReadOnlySidecar();
	void persistReadOnlySidecar();
	void resolveUserIdentity();
	QString uniqueDefaultLayerName() const;

	MapEditorController& controller;
	QAction* sketch_action = nullptr;
	QString current_user_id;
	QString current_user_name;
	QString selected_layer_id;
	QString sidecar_storage_key;
	QDate layer_choice_date;
	bool sidecar_loaded = false;

	Q_DISABLE_COPY(SketchLayerFeature)
};

}  // namespace OpenOrienteering

#endif
