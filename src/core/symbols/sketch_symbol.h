/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_SKETCH_SYMBOL_H
#define OPENORIENTEERING_SKETCH_SYMBOL_H

#include <QColor>

#include "core/symbols/line_symbol.h"

namespace OpenOrienteering {

/**
 * A self-colored, screen-only line symbol for the seamless sketch layer.
 *
 * Sketch colors deliberately do not enter the map color table. This keeps a
 * style fully represented by a symbol.put operation in connected editing and
 * avoids turning every field annotation color into a printable separation.
 */
class SketchSymbol final : public LineSymbol
{
public:
	SketchSymbol();
	~SketchSymbol() override;

	const QColor& strokeColor() const noexcept;
	void setStrokeColor(const QColor& color);

	void createRenderables(
	        const Object* object,
	        const VirtualCoordVector& coords,
	        ObjectRenderables& output,
	        RenderableOptions options) const override;
	void createRenderables(
	        const PathObject* object,
	        const PathPartVector& path_parts,
	        ObjectRenderables& output,
	        RenderableOptions options) const override;

protected:
	explicit SketchSymbol(const SketchSymbol& proto);
	SketchSymbol* duplicate() const override;

	void saveRootAttributes(QXmlStreamWriter& xml) const override;
	void saveImpl(QXmlStreamWriter& xml, const Map& map) const override;
	bool loadImpl(QXmlStreamReader& xml, const Map& map,
	              SymbolDictionary& symbol_dict, int version) override;
	bool equalsImpl(const Symbol* other,
	                Qt::CaseSensitivity case_sensitivity) const override;

private:
	QColor stroke_color = QColor(Qt::red);
};

}  // namespace OpenOrienteering

#endif
