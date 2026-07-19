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

#ifndef OPENORIENTEERING_IMAGERY_CATALOG_STORE_H
#define OPENORIENTEERING_IMAGERY_CATALOG_STORE_H

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

#include "imagery/oic_catalog.h"

namespace OpenOrienteering::imagery {

struct ImageryCatalogState
{
	QString origin;
	QString final_url;
	QDateTime installed_at;
	QDateTime updated_at;
	QDateTime checked_at;
	QByteArray sha256;
	QByteArray previous_sha256;
	QByteArray etag;
	QByteArray last_modified;
	bool legacy_layout = false;
	bool recovered_previous = false;

	bool operator==(const ImageryCatalogState&) const = default;
};

struct ImageryCatalogInstallMetadata
{
	QString origin;
	QString final_url;
	QByteArray etag;
	QByteArray last_modified;

	bool operator==(const ImageryCatalogInstallMetadata&) const = default;
};

struct ImageryCatalogInstallOptions
{
	bool allow_lower_revision = false;
	bool allow_same_revision_conflict = false;

	bool operator==(const ImageryCatalogInstallOptions&) const = default;
};

enum class ImageryCatalogSourceChangeKind
{
	Added,
	Removed,
	Operational,
	MetadataOnly,
};

struct ImageryCatalogSourceChange
{
	ImageryCatalogSourceChangeKind kind =
		ImageryCatalogSourceChangeKind::Added;
	QString source_id;
	QString name;

	bool operator==(const ImageryCatalogSourceChange&) const = default;
};

struct InstalledImageryCatalog
{
	OicCatalogReadResult read_result;
	ImageryCatalogState state;
	QString directory;
	QString catalog_path;
};

struct ImageryCatalogStoreIssue
{
	QString path;
	QString message;

	bool operator==(const ImageryCatalogStoreIssue&) const = default;
};

struct ImageryCatalogAnalysis
{
	enum class UpdateKind
	{
		NewCatalog,
		ExactReimport,
		HigherRevision,
		LowerRevision,
		SameRevisionConflict,
	};

	UpdateKind update_kind = UpdateKind::NewCatalog;
	int added = 0;
	int changed = 0;
	int operational_changed = 0;
	int metadata_only_changed = 0;
	int removed = 0;
	int invalid = 0;
	int unsupported = 0;
	int exact_duplicates = 0;
	int potential_duplicates = 0;
	QVector<ImageryCatalogSourceChange> source_changes;

	bool operator==(const ImageryCatalogAnalysis&) const = default;
};

/**
 * Crash-safe local store for imported OIC catalog snapshots.
 *
 * Catalog bytes are immutable and addressed by their document SHA-256. An
 * atomically replaced current.json selects the active snapshot, so a process
 * interruption cannot combine catalog bytes and metadata from two revisions.
 * The immediately previous snapshot is retained for diagnostics and recovery.
 *
 * The reader also accepts the one-directory catalog.oic/state.json layout used
 * by the earlier mapper-coc implementation. The next successful write migrates
 * that installation to the snapshot layout without changing its catalog bytes.
 */
class ImageryCatalogStore
{
public:
	explicit ImageryCatalogStore(QString root = {});

	QString rootPath() const;
	QString directoryKey(const QString& catalog_id) const;

	QVector<InstalledImageryCatalog> catalogs(
		QVector<ImageryCatalogStoreIssue>* issues = nullptr) const;
	ImageryCatalogAnalysis analyze(
		const OicCatalogReadResult& candidate,
		QVector<ImageryCatalogStoreIssue>* issues = nullptr) const;

	bool install(
		const OicCatalogReadResult& catalog,
		const QString& origin,
		const QByteArray& etag = {},
		const QByteArray& last_modified = {},
		QString* error = nullptr) const;
	bool install(
		const OicCatalogReadResult& catalog,
		const ImageryCatalogInstallMetadata& metadata,
		const ImageryCatalogInstallOptions& options,
		QString* error = nullptr) const;
	bool markChecked(
		const QString& catalog_id,
		const QString& final_url,
		const QByteArray& etag,
		const QByteArray& last_modified,
		QString* error = nullptr) const;
	bool remove(const QString& catalog_id, QString* error = nullptr) const;

private:
	InstalledImageryCatalog loadDirectory(
		const QString& directory,
		QString* error) const;

	QString root;
};

}  // namespace OpenOrienteering::imagery

#endif
