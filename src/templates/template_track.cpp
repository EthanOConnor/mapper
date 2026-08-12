/*
 *    Copyright 2012, 2013 Thomas Schöps
 *    Copyright 2013-2020 Kai Pastor
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


#include "template_track.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

#include <Qt>
#include <QByteArray>
#include <QCommandLinkButton>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFont>
#include <QLatin1Char>
#include <QLatin1String>
#include <QMessageBox>
#include <QPainterPath>
#include <QRect>
#include <QRgb>
#include <QSize>
#include <QStringView>
#include <QXmlStreamReader>
#include <QXmlStreamWriter>

#include "core/georeferencing.h"
#include "core/latlon.h"
#include "core/map.h"
#include "core/map_color.h"
#include "core/map_coord.h"
#include "core/map_part.h"
#include "core/objects/object.h"
#include "core/symbols/line_symbol.h"
#include "core/symbols/point_symbol.h"
#include "gui/georeferencing_dialog.h"
#include "gui/task_dialog.h"
#include "undo/object_undo.h"
#include "util/util.h"
#include "render/qt_render_bridge.h"

class QAbstractButton;


namespace OpenOrienteering {

class Symbol;

namespace {

/// Ground half-widths (m) which accuracy values are grouped into for drawing.
const std::array<double, 9> accuracy_buckets = { 0.05, 0.1, 0.25, 0.5, 1.0, 2.0, 5.0, 10.0, 25.0 };

/// Bounds on the drawn band, so that neither an RTK fix disappears nor a lost
/// fix paints over half the map.
constexpr auto min_band_meters = 0.25;
constexpr auto max_band_meters = 25.0;

/// Opacity is band_ink/width, bounded to stay both visible and unobtrusive.
constexpr auto band_ink = 0.25;
constexpr auto min_band_alpha = 0.04;
constexpr auto max_band_alpha = 0.5;

/// Neutral grey: the band carries no hue of its own, leaving colour to the map.
const QColor band_base_color = QColor(40, 40, 40);

/**
 * Returns the accuracy bucket index for a horizontal accuracy in meters,
 * or -1 if the accuracy is unknown.
 */
int accuracyBucket(float h_accuracy)
{
	if (!(h_accuracy > 0))  // false for NaN
		return -1;
	for (std::size_t i = 0; i < accuracy_buckets.size(); ++i)
	{
		if (double(h_accuracy) <= accuracy_buckets[i])
			return int(i);
	}
	return int(accuracy_buckets.size()) - 1;
}

/// Serialized names of the track display modes.
QString displayModeName(TemplateTrack::TrackDisplayMode mode)
{
	switch (mode)
	{
	case TemplateTrack::TrackDisplayMode::AccuracyBand:
		return QString::fromLatin1("accuracy_band");
	case TemplateTrack::TrackDisplayMode::FixAware:
		return QString::fromLatin1("fix_aware");
	case TemplateTrack::TrackDisplayMode::Classic:
		break;
	}
	return QString::fromLatin1("classic");
}

TemplateTrack::TrackDisplayMode displayModeFromName(QStringView name)
{
	if (name == QLatin1String("accuracy_band"))
		return TemplateTrack::TrackDisplayMode::AccuracyBand;
	if (name == QLatin1String("fix_aware"))
		return TemplateTrack::TrackDisplayMode::FixAware;
	return TemplateTrack::TrackDisplayMode::Classic;
}

/**
 * Returns the centre line dash pattern which represents a GNSS fix mode.
 *
 * Solid means an RTK fixed solution, dashed a float solution, dotted an
 * ordinary or differential 3D fix. Anything less trustworthy is drawn solid,
 * like a track without recorded quality data.
 */
std::vector<double> dashPatternForFix(TrackFixType fix, double width)
{
	switch (fix)
	{
	case TrackFixType::RtkFloat:
		return { 4 * width, 3 * width };
	case TrackFixType::DGPS:
	case TrackFixType::Fix3D:
		return { width, 2 * width };
	case TrackFixType::RtkFixed:
	case TrackFixType::Fix2D:
	case TrackFixType::None:
	case TrackFixType::Unknown:
		break;
	}
	return {};
}

const MapColor& makeTrackColor(Map& map)
{
	auto* track_color = new MapColor(QLatin1String{"Purple"}, 0); 
	track_color->setSpotColorName(QLatin1String{"PURPLE"});
	track_color->setCmyk({0.35f, 0.85f, 0.0, 0.0});
	track_color->setRgbFromCmyk();
	map.addColor(track_color, 0);
	return *track_color;
}

const LineSymbol& makeTrackSymbol(Map& map, const MapColor& color)
{
	auto* track_symbol = new LineSymbol();
	track_symbol->setName(TemplateTrack::tr("Track"));
	track_symbol->setNumberComponent(0, 1);
	track_symbol->setColor(&color);
	track_symbol->setLineWidth(0.1); // mm, almost cosmetic
	track_symbol->setCapStyle(LineSymbol::FlatCap);
	track_symbol->setJoinStyle(LineSymbol::MiterJoin);
	map.addSymbol(track_symbol, map.getNumSymbols());
	return *track_symbol;
}

const LineSymbol& makeRouteSymbol(Map& map, const MapColor& color)
{
	auto* route_symbol = new LineSymbol();
	route_symbol->setName(TemplateTrack::tr("Route"));
	route_symbol->setNumberComponent(0, 2);
	route_symbol->setColor(&color);
	route_symbol->setLineWidth(0.5); // mm
	route_symbol->setCapStyle(LineSymbol::FlatCap);
	route_symbol->setJoinStyle(LineSymbol::MiterJoin);
	map.addSymbol(route_symbol, map.getNumSymbols());
	return *route_symbol;
}

const PointSymbol& makeWaypointSymbol(Map& map, const MapColor& color)
{
	auto* waypoint_symbol = new PointSymbol();
	waypoint_symbol->setName(TemplateTrack::tr("Waypoint"));
	waypoint_symbol->setNumberComponent(0, 3);
	waypoint_symbol->setInnerColor(&color);
	waypoint_symbol->setInnerRadius(500); // (um)
	map.addSymbol(waypoint_symbol, map.getNumSymbols());
	return *waypoint_symbol;
}

PathObject* importPath(Map& map, const Symbol& symbol, MapCoordVector coords)
{
	if (coords.empty())
		return nullptr;
	
	if (coords.size() == 1)
		coords.push_back(coords.front());
	
	auto* path = new PathObject(&symbol, std::move(coords));
	map.addObject(path);
	map.addObjectToSelection(path, false);
	return path;
}

PointObject* importPoint(Map& map, const Symbol& symbol, const MapCoordF& position, const QString& name)
{
	auto* point = new PointObject(&symbol);
	point->setPosition(position);
	if (!name.isEmpty())
		point->setTag(QStringLiteral("name"), name);
	map.addObject(point);
	map.addObjectToSelection(point, false);
	return point;
}

}  // namespace


const std::vector<QByteArray>& TemplateTrack::supportedExtensions()
{
	static std::vector<QByteArray> extensions = { "gpx" };
	return extensions;
}

TemplateTrack::TemplateTrack(const QString& path, Map* map)
 : Template(path, map)
{
	// set default value
	track_crs_spec = Georeferencing::geographic_crs_spec;
	
	const Georeferencing& georef = map->getGeoreferencing();
	connect(&georef, &Georeferencing::projectionChanged, this, &TemplateTrack::updateGeoreferencing);
	connect(&georef, &Georeferencing::transformationChanged, this, &TemplateTrack::updateGeoreferencing);
	connect(&georef, &Georeferencing::stateChanged, this, &TemplateTrack::updateGeoreferencing);
	connect(&georef, &Georeferencing::declinationChanged, this, &TemplateTrack::updateGeoreferencing);
}

TemplateTrack::TemplateTrack(const TemplateTrack& proto)
: Template(proto)
, track(proto.track)
, track_crs_spec(proto.track_crs_spec)
, projected_crs_spec(proto.projected_crs_spec)
{
	if (proto.preserved_georef)
		preserved_georef = std::make_unique<Georeferencing>(*proto.preserved_georef);
	
	const Georeferencing& georef = map->getGeoreferencing();
	connect(&georef, &Georeferencing::projectionChanged, this, &TemplateTrack::updateGeoreferencing);
	connect(&georef, &Georeferencing::transformationChanged, this, &TemplateTrack::updateGeoreferencing);
	connect(&georef, &Georeferencing::stateChanged, this, &TemplateTrack::updateGeoreferencing);
	connect(&georef, &Georeferencing::declinationChanged, this, &TemplateTrack::updateGeoreferencing);
}

TemplateTrack::~TemplateTrack()
{
	if (template_state == Loaded)
		unloadTemplateFile();
}


TemplateTrack* TemplateTrack::duplicate() const
{
	return new TemplateTrack(*this);
}



void TemplateTrack::setTrackDisplayMode(TrackDisplayMode mode)
{
	// The mode belongs to the template configuration in the map file, not to
	// the track file, so it must not mark the template file as modified.
	display_mode = mode;
}


bool TemplateTrack::hasGnssQualityData() const
{
	for (int segment = 0; segment < track.getNumSegments(); ++segment)
	{
		auto const count = track.getSegmentPointCount(segment);
		for (int i = 0; i < count; ++i)
		{
			auto const& point = track.getSegmentPoint(segment, i);
			if (point.hAccuracy > 0 || point.fixType != TrackFixType::Unknown)
				return true;
		}
	}
	return false;
}


void TemplateTrack::saveTypeSpecificTemplateConfiguration(QXmlStreamWriter& xml) const
{
	if (preserved_georef)
	{
		// Preserve explicit georeferencing from OgrTemplate.
		preserved_georef->save(xml);
		return;
	}

	if (display_mode != TrackDisplayMode::Classic)
	{
		xml.writeStartElement(QString::fromLatin1("display"));
		xml.writeAttribute(QString::fromLatin1("mode"), displayModeName(display_mode));
		xml.writeEndElement(/*display*/);
	}

	// Follow map georeferencing XML structure
	xml.writeStartElement(QString::fromLatin1("crs_spec"));
	xml.writeCharacters(track_crs_spec);
	xml.writeEndElement(/*crs_spec*/);
	if (!projected_crs_spec.isEmpty())
	{
		Q_ASSERT(!is_georeferenced);
		xml.writeStartElement(QString::fromLatin1("projected_crs_spec"));
		xml.writeCharacters(projected_crs_spec);
		xml.writeEndElement(/*crs_spec*/);
	}
}


bool TemplateTrack::loadTypeSpecificTemplateConfiguration(QXmlStreamReader& xml)
{
	if (xml.name() == QLatin1String("crs_spec"))
	{
		track_crs_spec = xml.readElementText();
	}
	else if (xml.name() == QLatin1String("display"))
	{
		display_mode = displayModeFromName(xml.attributes().value(QLatin1String("mode")));
		xml.skipCurrentElement();
	}
	else if (xml.name() == QLatin1String("projected_crs_spec"))
	{
		Q_ASSERT(!is_georeferenced);
		projected_crs_spec = xml.readElementText();
	}
	else if (xml.name() == QLatin1String("georeferencing"))
	{
		// Preserve explicit georeferencing from OgrTemplate.
		preserved_georef = std::make_unique<Georeferencing>();
		preserved_georef->load(xml, false);
	}
	else
	{
		xml.skipCurrentElement(); // unsupported
	}
	
	return true;
}


bool TemplateTrack::writeTemplateFile(const QString& path) const
{
	if (!track.saveTo(path))
		return false;
	return true;
}

bool TemplateTrack::loadTemplateFileImpl()
{
	if (preserved_georef)
	{
		setErrorString(tr("This template must be loaded with GDAL/OGR."));
		return false;
	}
	
	if (!track_crs_spec.isEmpty() && track_crs_spec != Georeferencing::geographic_crs_spec)
	{
		setErrorString(tr("This template must be loaded with GDAL/OGR."));
		return false;
	}
	
	if (!track.loadFrom(template_path, false))
		return false;
	
	if (getTemplateState() != Configuring)
	{
		if (!is_georeferenced)
		{
			if (projected_crs_spec.isEmpty())
				projected_crs_spec = calculateLocalGeoreferencing();
			applyProjectedCrsSpec();
		}
		else
		{
			projected_crs_spec.clear();
			track.changeMapGeoreferencing(map->getGeoreferencing());
		}
	}
	
	return true;
}

bool TemplateTrack::postLoadSetup(QWidget* dialog_parent, bool& /*out_center_in_view*/)
{
	is_georeferenced = true;
	
	// If the CRS is geographic, ask if track should be loaded using map georeferencing or ad-hoc georeferencing
	/** \todo Remove historical scope */
	{
		TaskDialog georef_dialog(dialog_parent, tr("Opening track ..."),
			tr("Load the track in georeferenced or non-georeferenced mode?"),
			QDialogButtonBox::Abort);
		QString georef_text = tr("Positions the track according to the map's georeferencing settings.");
		if (map->getGeoreferencing().getState() != Georeferencing::Geospatial)
			georef_text += QLatin1Char(' ') + tr("These are not configured yet, so they will be shown as the next step.");
		QAbstractButton* georef_button = georef_dialog.addCommandButton(tr("Georeferenced"), georef_text);
		QAbstractButton* non_georef_button = georef_dialog.addCommandButton(tr("Non-georeferenced"), tr("Projects the track using an orthographic projection with center at the track's coordinate average. Allows adjustment of the transformation and setting the map georeferencing using the adjusted track position."));
		
		georef_dialog.exec();
		if (georef_dialog.clickedButton() == georef_button)
			is_georeferenced = true;
		else if (georef_dialog.clickedButton() == non_georef_button)
			is_georeferenced = false;
		else // abort
			return false;
	}
	
	// If the track is loaded as georeferenced and the transformation parameters
	// were not set yet, it must be done now
	if (is_georeferenced && map->getGeoreferencing().getState() != Georeferencing::Geospatial)
	{
		// Set default for real world reference point as some average of the track coordinates
		Georeferencing georef(map->getGeoreferencing());
		georef.setGeographicRefPoint(track.calcAveragePosition());
		
		// Show the parameter dialog
		GeoreferencingDialog dialog(dialog_parent, map, &georef);
		dialog.setKeepGeographicRefCoords();
		if (dialog.exec() == QDialog::Rejected || map->getGeoreferencing().getState() != Georeferencing::Geospatial)
			return false;
	}
	
	// If the track is loaded as not georeferenced,
	// the map coords for the track coordinates have to be calculated
	if (!is_georeferenced)
	{
		if (projected_crs_spec.isEmpty())
			projected_crs_spec = calculateLocalGeoreferencing();
		applyProjectedCrsSpec();
	}
	else
	{
		projected_crs_spec.clear();
		track.changeMapGeoreferencing(map->getGeoreferencing());
	}
	
	return true;
}

void TemplateTrack::unloadTemplateFileImpl()
{
	track.clear();
}

std::shared_ptr<const render::RenderIR> TemplateTrack::buildRenderIR(
	bool on_screen,
	double view_scale,
	render::Revision revision) const
{
	render::RenderIRBuilder builder(revision);
	auto coord_scale = 1.0;
	if (!is_georeferenced)
	{
		auto const origin = templateToMap(QPointF(0, 0));
		auto const x_axis = templateToMap(QPointF(1, 0));
		auto const y_axis = templateToMap(QPointF(0, 1));
		builder.pushTransform({
			x_axis.x() - origin.x(), x_axis.y() - origin.y(),
			y_axis.x() - origin.x(), y_axis.y() - origin.y(),
			origin.x(), origin.y(),
		});
		coord_scale = std::hypot(x_axis.x() - origin.x(), x_axis.y() - origin.y());
		if (!(coord_scale > 0))
			coord_scale = 1.0;
	}

	auto const track_width = on_screen ? 1.0 / std::max(view_scale, 1.0e-9) : 0.1;
	auto const track_color = QColor(212, 0, 244);

	// Ground meters per drawing unit. Map coordinates are millimetres at map
	// scale; when the track is not georeferenced, the pushed transform scales
	// them again, and accuracy widths have to be divided by that factor to end
	// up the intended size on the map.
	auto const scale_denominator = map ? double(map->getScaleDenominator()) : 0.0;
	auto const drawing_units_per_meter = scale_denominator > 0
	                                     ? 1000.0 / scale_denominator / coord_scale
	                                     : 0.0;
	auto const quality_mode = display_mode != TrackDisplayMode::Classic
	                          && drawing_units_per_meter > 0
	                          && hasGnssQualityData();

	auto const stroke_run = [this, &builder](int segment, int from, int to,
	                                         double width, const QColor& color,
	                                         std::vector<double> dash_pattern) {
		render::PathBuilder path;
		for (int i = from; i <= to; ++i)
		{
			auto const& point = track.getSegmentPoint(segment, i);
			if (i == from)
				path.moveTo({ point.map_coord.x(), point.map_coord.y() });
			else
				path.lineTo({ point.map_coord.x(), point.map_coord.y() });
		}
		auto stroke = render::StrokeStyle{};
		stroke.width = width;
		stroke.cap = render::LineCap::Round;
		stroke.join = render::LineJoin::Round;
		stroke.dash_pattern = std::move(dash_pattern);
		builder.strokePath(path.finish(), render::fromQColor(color),
		                   std::move(stroke),
		                   render::QualityHint::ForceAntialiasing);
	};

	// Groups consecutive points sharing a key into runs, so that the number of
	// paths follows the number of quality changes rather than the sample rate.
	auto const for_each_run = [this](int segment, auto key, auto apply_run) {
		auto const count = track.getSegmentPointCount(segment);
		if (count < 2)
			return;
		auto run_start = 0;
		auto run_key = key(track.getSegmentPoint(segment, 0));
		for (int i = 1; i < count; ++i)
		{
			auto const point_key = key(track.getSegmentPoint(segment, i));
			if (point_key != run_key)
			{
				apply_run(run_start, i, run_key);
				run_start = i;
				run_key = point_key;
			}
		}
		apply_run(run_start, count - 1, run_key);
	};

	for (int segment = 0; segment < track.getNumSegments(); ++segment)
	{
		if (quality_mode)
		{
			// The band is the recorded confidence region: its ground width is
			// twice the horizontal accuracy. Opacity is inversely proportional
			// to the width so that a segment's total ink stays roughly constant
			// - a sloppy fix reads as a wide faint smear, an exact one as a
			// crisp line, without introducing a second colour to the map.
			for_each_run(segment,
			             [](const TrackPoint& point) { return accuracyBucket(point.hAccuracy); },
			             [&](int from, int to, int bucket) {
				if (bucket < 0)
					return;
				auto const band_meters = std::clamp(2.0 * accuracy_buckets[std::size_t(bucket)],
				                                    min_band_meters, max_band_meters);
				auto const band_width = band_meters * drawing_units_per_meter;
				if (band_width <= track_width)
					return;
				auto band_color = band_base_color;
				band_color.setAlphaF(std::clamp(band_ink / band_meters,
				                                min_band_alpha, max_band_alpha));
				stroke_run(segment, from, to, band_width, band_color, {});
			});
		}

		if (quality_mode && display_mode == TrackDisplayMode::FixAware)
		{
			// The fix mode rides on the centre line's dash pattern rather than
			// on its colour, which orienteering maps have no room left for.
			for_each_run(segment,
			             [](const TrackPoint& point) { return point.fixType; },
			             [&](int from, int to, TrackFixType fix) {
				stroke_run(segment, from, to, track_width, track_color,
				           dashPatternForFix(fix, track_width));
			});
		}
		else
		{
			stroke_run(segment, 0, track.getSegmentPointCount(segment) - 1,
			           track_width, track_color, {});
		}
	}

	QFont waypoint_font;
	waypoint_font.setPixelSize(2);
	for (int waypoint = 0; waypoint < track.getNumWaypoints(); ++waypoint)
	{
		auto const& point = track.getWaypoint(waypoint);
		builder.fillEllipse(
			{ point.map_coord.x() - 0.25, point.map_coord.y() - 0.25, 0.5, 0.5 },
			render::fromQColor(QColor(Qt::red)),
			render::QualityHint::ForceAntialiasing
		);
		auto const& name = track.getWaypointName(waypoint);
		if (!name.isEmpty())
		{
			QPainterPath text;
			text.addText(QPointF(), waypoint_font, name);
			auto const bounds = text.boundingRect();
			text.translate(point.map_coord.x() - bounds.center().x(),
			               point.map_coord.y() - bounds.bottom());
			builder.fillPath(render::fromQPainterPath(text),
			                 render::fromQColor(QColor(Qt::red)));
		}
	}

	if (!is_georeferenced)
		builder.popTransform();
	return builder.finish();
}

QRectF TemplateTrack::getTemplateExtent() const
{
	// Infinite because the extent of the waypoint texts is unknown
	return infiniteRectF();
}

QRectF TemplateTrack::calculateTemplateBoundingBox() const
{
	QRectF bbox;
	
	int size = track.getNumWaypoints();
	for (int i = 0; i < size; ++i)
	{
		const TrackPoint& track_point = track.getWaypoint(i);
		MapCoordF point = track_point.map_coord;
		rectIncludeSafe(bbox, is_georeferenced ? point : templateToMap(point));
	}
	for (int i = 0; i < track.getNumSegments(); ++i)
	{
		size = track.getSegmentPointCount(i);
		for (int k = 0; k < size; ++k)
		{
			const TrackPoint& track_point = track.getSegmentPoint(i, k);
			MapCoordF point = track_point.map_coord;
			rectIncludeSafe(bbox, is_georeferenced ? point : templateToMap(point));
		}
	}
	
	return bbox;
}

int TemplateTrack::getTemplateBoundingBoxPixelBorder() const
{
	// As we don't estimate the extent of the widest waypoint text,
	// return a "very big" number to cover everything
	return 10e8;
}


bool TemplateTrack::hasAlpha() const
{
	return false;
}


bool TemplateTrack::import(QWidget* dialog_parent)
{
	if (!map)
	{
		return false;
	}
	
	if (track.getNumWaypoints() == 0 && track.getNumSegments() == 0)
	{
		QMessageBox::critical(dialog_parent, tr("Error"), tr("The path is empty, there is nothing to import!"));
		return false;
	}
	
	auto* undo_step = new DeleteObjectsUndoStep(map);
	MapPart* part = map->getCurrentPart();
	std::vector< Object* > result;
	// clazy:excludeall=reserve-candidates
	
	map->clearObjectSelection(false);
	
	auto& track_color = makeTrackColor(*map);
	auto& track_symbol = makeTrackSymbol(*map, track_color);
	
	auto const num_waypoints = track.getNumWaypoints();
	if (num_waypoints > 0)
	{
		auto res = QMessageBox::No;
		if (num_waypoints > 1)
		{
			res = QMessageBox::question(
			          dialog_parent,
			          tr("Question"),
			          tr("Should the waypoints be imported as a line going through all points?"),
			          QMessageBox::Yes | QMessageBox::No,
			          QMessageBox::No);
		}
		if (res == QMessageBox::No)
		{
			auto& waypoint_symbol = makeWaypointSymbol(*map, track_color);
			for (int i = 0; i < num_waypoints; i++)
			{
				auto pos = templateToMap(track.getWaypoint(i).map_coord);
				auto& name = track.getWaypointName(i);
				if (auto* waypoint = importPoint(*map, waypoint_symbol, pos, name))
					result.push_back(waypoint);
			}
		}
		else
		{
			MapCoordVector coords;
			coords.reserve(MapCoordVector::size_type(track.getNumWaypoints()));
			for (int i = 0; i < track.getNumWaypoints(); i++)
				coords.push_back(MapCoord(templateToMap(track.getWaypoint(i).map_coord)));
			
			auto& route_symbol = makeRouteSymbol(*map, track_color);
			if (auto* path = importPath(*map, route_symbol, std::move(coords)))
			    result.push_back(path);
		}
	}
	
	int skipped_paths = 0;
	for (int i = 0; i < track.getNumSegments(); i++)
	{
		auto const segment_size = track.getSegmentPointCount(i);
		if (segment_size == 0)
		{
			++skipped_paths;
			continue; // Don't create path without objects.
		}
		
		MapCoordVector coords;
		coords.reserve(MapCoordVector::size_type(segment_size));
		for (int j = 0; j < segment_size; j++)
			coords.push_back(MapCoord(templateToMap(track.getSegmentPoint(i, j).map_coord)));
		
		if (auto* path = importPath(*map, track_symbol, std::move(coords)))
		{
			if (track.getSegmentPoint(i, 0).latlon == track.getSegmentPoint(i, segment_size-1).latlon)
				path->closeAllParts();
			result.push_back(path);
		}
	}
	
	for (const auto* object : result) // keep as separate loop to get the correct (final) indices
		undo_step->addObject(part->findObjectIndex(object));
	
	map->setObjectsDirty();
	map->push(undo_step);
	
	map->emitSelectionChanged();
	map->emitSelectionEdited();		// TODO: is this necessary here?
	
	if (skipped_paths)
	{
		QMessageBox::information(
		  dialog_parent,
		  tr("Import problems"),
		  tr("%n path object(s) could not be imported (reason: missing coordinates).", "", skipped_paths) );
	}
	
	return true;
}

void TemplateTrack::configureForGPSTrack()
{
	is_georeferenced = true;
	
	track_crs_spec = Georeferencing::geographic_crs_spec;
	
	projected_crs_spec.clear();
	track.changeMapGeoreferencing(map->getGeoreferencing());
	
	template_state = Template::Loaded;
}

void TemplateTrack::updateGeoreferencing()
{
	if (is_georeferenced && template_state == Template::Loaded)
	{
		projected_crs_spec.clear();
		track.changeMapGeoreferencing(map->getGeoreferencing());
		map->requestRedraw();
	}
}

QString TemplateTrack::calculateLocalGeoreferencing() const
{
	LatLon proj_center = track.calcAveragePosition();
	return QString::fromLatin1("+proj=ortho +datum=WGS84 +lat_0=%1 +lon_0=%2")
	        .arg(proj_center.latitude(), 0, 'f')
	        .arg(proj_center.longitude(), 0, 'f');
	
}

void TemplateTrack::applyProjectedCrsSpec()
{
	Georeferencing georef;
	georef.setScaleDenominator(int(map->getScaleDenominator()));
	georef.setProjectedCRS(QString{}, projected_crs_spec);
	georef.setProjectedRefPoint({});
	georef.setCombinedScaleFactor(1.0);
	georef.setGrivation(0.0);
	track.changeMapGeoreferencing(georef);
}


}  // namespace OpenOrienteering
