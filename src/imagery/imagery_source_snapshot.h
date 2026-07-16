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

#ifndef OPENORIENTEERING_IMAGERY_SOURCE_SNAPSHOT_H
#define OPENORIENTEERING_IMAGERY_SOURCE_SNAPSHOT_H

#include <optional>

#include <QByteArray>
#include <QString>

#include "imagery/imagery_source.h"

namespace OpenOrienteering::imagery {

struct ImagerySourceSnapshot
{
	ResolvedImagerySource source;
	QByteArray canonical_json;
	QByteArray sha256;
};

/**
 * Deterministic JSON persistence for one fully resolved runtime source.
 *
 * The fingerprint is the lowercase SHA-256 digest of canonical_json. Decoding
 * re-encodes accepted input so formatting differences cannot alter identity.
 */
class ImagerySourceSnapshotCodec
{
public:
	static constexpr int version = 1;
	static constexpr qsizetype maximum_size = 1024 * 1024;

	static QString formatIdentifier();
	static std::optional<ImagerySourceSnapshot> encode(
		const ResolvedImagerySource& source,
		QString* error = nullptr
	);
	static std::optional<ImagerySourceSnapshot> decode(
		const QByteArray& json,
		QString* error = nullptr
	);
};

}  // namespace OpenOrienteering::imagery

#endif
