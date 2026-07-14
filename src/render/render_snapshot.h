/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#ifndef OPENORIENTEERING_RENDER_SNAPSHOT_H
#define OPENORIENTEERING_RENDER_SNAPSHOT_H

#include <array>
#include <cstddef>
#include <map>
#include <memory>
#include <vector>

#include "core/renderables/renderable.h"
#include "render/render_ir.h"

namespace OpenOrienteering::render {

enum class SpotMethod : std::uint8_t
{
	Undefined,
	Custom,
	Spot,
};

struct SpotComponent
{
	int priority = MapColor::Reserved;
	float factor = 0;
};

struct SnapshotColor
{
	int priority = MapColor::Reserved;
	Color color;
	double opacity = 1;
	SpotMethod spot_method = SpotMethod::Undefined;
	bool knockout = false;
	std::vector<SpotComponent> components;
};

struct SnapshotObject
{
	ObjectId id = 0;
	Rect extent;
	bool helper_symbol = false;
	bool hidden_symbol = false;
	std::map<int, SharedRenderables> colors;
};

inline constexpr std::size_t snapshot_object_block_size = 256;

struct SnapshotObjectBlock
{
	std::array<std::shared_ptr<const SnapshotObject>, snapshot_object_block_size> objects;
};

using SnapshotObjectBlockPtr = std::shared_ptr<const SnapshotObjectBlock>;
using SnapshotObjectIds = std::shared_ptr<const std::vector<ObjectId>>;

struct RenderRequest
{
	Rect bounding_box;
	double scaling = 1;
	RenderConfig::Options options;
	double opacity = 1;
};

/** Immutable document revision from which every backend records IR. */
class MapRenderSnapshot
{
public:
	MapRenderSnapshot(Revision revision,
	                  std::map<int, SnapshotColor> colors,
	                  std::vector<SnapshotObjectBlockPtr> object_blocks,
	                  std::map<int, SnapshotObjectIds> color_objects,
	                  std::size_t object_count);

	Revision revision() const noexcept;
	std::uint64_t identity() const noexcept;
	const std::map<int, SnapshotColor>& colors() const noexcept;
	std::size_t objectCount() const noexcept;
	ObjectId maxObjectId() const noexcept;
	const SnapshotObject* object(ObjectId id) const noexcept;

	std::shared_ptr<const RenderIR> buildIR(const RenderRequest& request) const;
	std::shared_ptr<const RenderIR> buildColorSeparationIR(
		const RenderRequest& request,
		int separation_priority,
		bool use_color
	) const;

private:
	const SnapshotColor* color(int priority) const;
	bool appendObjectColor(RenderIRBuilder& builder,
	                       const SnapshotObject& object,
	                       int color_priority,
	                       Color color,
	                       const RenderRequest& request) const;

	Revision revision_;
	std::uint64_t identity_ = 0;
	std::map<int, SnapshotColor> colors_;
	std::vector<SnapshotObjectBlockPtr> object_blocks_;
	std::map<int, SnapshotObjectIds> color_objects_;
	std::size_t object_count_ = 0;
};

}  // namespace OpenOrienteering::render

#endif
