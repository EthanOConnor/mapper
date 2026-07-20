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

#include "imagery/imagery_catalog_store.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <utility>

#include <QCryptographicHash>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLockFile>
#include <QMap>
#include <QSaveFile>
#include <QSet>
#include <QStandardPaths>
#include <QUrl>
#include <QUuid>

namespace OpenOrienteering::imagery {

namespace {

constexpr auto current_format =
	"org.openorienteering.imagery-catalog-installation";
constexpr auto snapshot_format =
	"org.openorienteering.imagery-catalog-snapshot";
constexpr int store_version = 1;
constexpr qint64 maximum_state_size = 64 * 1024;
constexpr qsizetype maximum_validator_size = 4096;

QString tr(const char* text)
{
	return QCoreApplication::translate(
		"OpenOrienteering::imagery::ImageryCatalogStore",
		text);
}

QByteArray sha256(const QByteArray& bytes)
{
	return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex();
}

bool isSha256(const QByteArray& value)
{
	if (value.size() != 64)
		return false;
	for (auto const character : value)
	{
		if (!((character >= '0' && character <= '9')
		      || (character >= 'a' && character <= 'f')))
			return false;
	}
	return true;
}

QByteArray sanitizedValidator(QByteArray value)
{
	value = value.trimmed();
	if (value.size() > maximum_validator_size)
		return {};
	for (auto const character : value)
	{
		auto const byte = static_cast<unsigned char>(character);
		if (byte < 0x20 || byte == 0x7f)
			return {};
	}
	return value;
}

bool isRemoteOrigin(const QString& value)
{
	auto const scheme = QUrl(value).scheme().toLower();
	return scheme == QLatin1String("http")
	       || scheme == QLatin1String("https");
}

bool fail(QString* error, QString message)
{
	if (error)
		*error = std::move(message);
	return false;
}

bool saveFile(
	const QString& path,
	const QByteArray& bytes,
	QString* error)
{
	QSaveFile file(path);
	if (!file.open(QIODevice::WriteOnly))
		return fail(error, file.errorString());
	if (file.write(bytes) != bytes.size())
		return fail(error, file.errorString());
	if (!file.commit())
		return fail(error, file.errorString());
	return true;
}

std::optional<QByteArray> readSmallFile(
	const QString& path,
	qint64 maximum_size,
	QString* error)
{
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
	{
		fail(error, file.errorString());
		return std::nullopt;
	}
	if (file.size() < 0 || file.size() > maximum_size)
	{
		fail(
			error,
			tr("The catalog store metadata exceeds its safety limit."));
		return std::nullopt;
	}
	return file.readAll();
}

std::optional<QJsonObject> readObject(
	const QString& path,
	QString* error)
{
	auto const bytes = readSmallFile(path, maximum_state_size, error);
	if (!bytes)
		return std::nullopt;
	QJsonParseError parse_error;
	auto const document = QJsonDocument::fromJson(*bytes, &parse_error);
	if (parse_error.error != QJsonParseError::NoError || !document.isObject())
	{
		fail(
			error,
			tr("Invalid catalog store metadata: %1")
				.arg(parse_error.errorString()));
		return std::nullopt;
	}
	return document.object();
}

QDateTime dateTime(
	const QJsonObject& object,
	const QString& name)
{
	return QDateTime::fromString(
		object.value(name).toString(),
		Qt::ISODateWithMs);
}

QByteArray decodedHeader(
	const QJsonObject& object,
	const QString& name)
{
	auto const encoded = object.value(name).toString().toLatin1();
	return sanitizedValidator(
		QByteArray::fromBase64(
			encoded,
			QByteArray::Base64UrlEncoding));
}

struct SnapshotMetadata
{
	QDateTime stored_at;
	QString origin;
	QString final_url;
	QDateTime updated_at;
	QByteArray etag;
	QByteArray last_modified;
};

QJsonObject currentObject(
	const OicCatalogReadResult& catalog,
	const ImageryCatalogState& state)
{
	return {
		{ QStringLiteral("format"), QString::fromLatin1(current_format) },
		{ QStringLiteral("version"), store_version },
		{ QStringLiteral("catalogId"), catalog.catalog.id },
		{ QStringLiteral("catalogRevision"), catalog.catalog.revision },
		{ QStringLiteral("sha256"), QString::fromLatin1(state.sha256) },
		{ QStringLiteral("previousSha256"),
		  QString::fromLatin1(state.previous_sha256) },
		{ QStringLiteral("origin"), state.origin },
		{ QStringLiteral("finalUrl"), state.final_url },
		{ QStringLiteral("installedAt"),
		  state.installed_at.toUTC().toString(Qt::ISODateWithMs) },
		{ QStringLiteral("updatedAt"),
		  state.updated_at.toUTC().toString(Qt::ISODateWithMs) },
		{ QStringLiteral("checkedAt"),
		  state.checked_at.toUTC().toString(Qt::ISODateWithMs) },
		{ QStringLiteral("etagBase64"),
		  QString::fromLatin1(
			  state.etag.toBase64(QByteArray::Base64UrlEncoding
			                      | QByteArray::OmitTrailingEquals)) },
		{ QStringLiteral("lastModifiedBase64"),
		  QString::fromLatin1(
			  state.last_modified.toBase64(
				  QByteArray::Base64UrlEncoding
				  | QByteArray::OmitTrailingEquals)) },
	};
}

QJsonObject snapshotObject(
	const OicCatalogReadResult& catalog,
	const QDateTime& stored_at,
	const ImageryCatalogState& state)
{
	return {
		{ QStringLiteral("format"), QString::fromLatin1(snapshot_format) },
		{ QStringLiteral("version"), store_version },
		{ QStringLiteral("catalogId"), catalog.catalog.id },
		{ QStringLiteral("catalogRevision"), catalog.catalog.revision },
		{ QStringLiteral("sha256"),
		  QString::fromLatin1(catalog.catalog.document_sha256) },
		{ QStringLiteral("storedAt"),
		  stored_at.toUTC().toString(Qt::ISODateWithMs) },
		{ QStringLiteral("origin"), state.origin },
		{ QStringLiteral("finalUrl"), state.final_url },
		{ QStringLiteral("updatedAt"),
		  state.updated_at.toUTC().toString(Qt::ISODateWithMs) },
		{ QStringLiteral("etagBase64"),
		  QString::fromLatin1(
			  state.etag.toBase64(
				  QByteArray::Base64UrlEncoding
				  | QByteArray::OmitTrailingEquals)) },
		{ QStringLiteral("lastModifiedBase64"),
		  QString::fromLatin1(
			  state.last_modified.toBase64(
				  QByteArray::Base64UrlEncoding
				  | QByteArray::OmitTrailingEquals)) },
	};
}

std::optional<SnapshotMetadata> readSnapshotMetadata(
	const QString& snapshot_directory,
	const QByteArray& expected_sha)
{
	auto const object = readObject(
		QDir(snapshot_directory).filePath(
			QStringLiteral("snapshot.json")),
		nullptr);
	if (!object
	    || object->value(QStringLiteral("format")).toString()
	         != QLatin1String(snapshot_format)
	    || object->value(QStringLiteral("version")).toInt(-1)
	         != store_version
	    || object->value(QStringLiteral("sha256"))
	             .toString()
	             .toLatin1()
	         != expected_sha)
	{
		return std::nullopt;
	}
	auto const stored_at =
		dateTime(*object, QStringLiteral("storedAt"));
	if (!stored_at.isValid())
		return std::nullopt;

	SnapshotMetadata metadata;
	metadata.stored_at = stored_at;
	metadata.origin =
		object->value(QStringLiteral("origin")).toString();
	metadata.final_url =
		object->value(QStringLiteral("finalUrl")).toString();
	if (metadata.final_url.isEmpty())
		metadata.final_url = metadata.origin;
	metadata.updated_at =
		dateTime(*object, QStringLiteral("updatedAt"));
	if (!metadata.updated_at.isValid())
		metadata.updated_at = stored_at;
	metadata.etag =
		decodedHeader(*object, QStringLiteral("etagBase64"));
	metadata.last_modified =
		decodedHeader(
			*object,
			QStringLiteral("lastModifiedBase64"));
	return metadata;
}

QString catalogFilename()
{
	return QStringLiteral("catalog.") + OicCatalogReader::fileExtension();
}

bool safeDirectory(const QString& path, QString* error)
{
	QFileInfo info(path);
	if (info.exists() && (info.isSymLink() || !info.isDir()))
	{
		return fail(
			error,
			tr(
				"The catalog store contains an unsafe directory entry: %1")
				.arg(path));
	}
	return true;
}

bool parseCurrentState(
	const QJsonObject& object,
	ImageryCatalogState* state,
	QString* catalog_id,
	int* revision,
	QString* error)
{
	if (object.value(QStringLiteral("format")).toString()
	      != QLatin1String(current_format)
	    || object.value(QStringLiteral("version")).toInt(-1)
	         != store_version)
	{
		return fail(
			error,
			tr("Unsupported catalog store metadata format."));
	}

	*catalog_id = object.value(QStringLiteral("catalogId")).toString();
	*revision = object.value(QStringLiteral("catalogRevision")).toInt();
	state->sha256 =
		object.value(QStringLiteral("sha256")).toString().toLatin1();
	state->previous_sha256 =
		object.value(QStringLiteral("previousSha256")).toString().toLatin1();
	state->origin = object.value(QStringLiteral("origin")).toString();
	state->final_url =
		object.value(QStringLiteral("finalUrl")).toString();
	if (state->final_url.isEmpty())
		state->final_url = state->origin;
	state->installed_at =
		dateTime(object, QStringLiteral("installedAt"));
	state->updated_at =
		dateTime(object, QStringLiteral("updatedAt"));
	state->checked_at =
		dateTime(object, QStringLiteral("checkedAt"));
	state->etag = decodedHeader(object, QStringLiteral("etagBase64"));
	state->last_modified =
		decodedHeader(object, QStringLiteral("lastModifiedBase64"));

	if (catalog_id->isEmpty() || *revision <= 0
	    || !isSha256(state->sha256)
	    || (!state->previous_sha256.isEmpty()
	        && !isSha256(state->previous_sha256))
	    || !state->installed_at.isValid()
	    || !state->updated_at.isValid()
	    || !state->checked_at.isValid())
	{
		return fail(
			error,
			tr("Catalog store metadata is incomplete or invalid."));
	}
	return true;
}

bool parseLegacyState(
	const QJsonObject& object,
	ImageryCatalogState* state)
{
	state->origin = object.value(QStringLiteral("origin")).toString();
	state->final_url = state->origin;
	state->installed_at =
		dateTime(object, QStringLiteral("installedAt"));
	state->updated_at = state->installed_at;
	state->checked_at = state->installed_at;
	state->sha256 =
		object.value(QStringLiteral("sha256")).toString().toLatin1();
	state->etag =
		sanitizedValidator(
			object.value(QStringLiteral("etag")).toString().toLatin1());
	state->last_modified =
		sanitizedValidator(
			object.value(QStringLiteral("lastModified")).toString().toLatin1());
	state->legacy_layout = true;
	return state->installed_at.isValid() && isSha256(state->sha256);
}

bool validateSnapshot(
	const QString& snapshot_directory,
	const QByteArray& expected_sha,
	QString* error)
{
	if (!safeDirectory(snapshot_directory, error))
		return false;
	auto const path =
		QDir(snapshot_directory).filePath(catalogFilename());
	QFile file(path);
	if (!file.open(QIODevice::ReadOnly))
		return fail(error, file.errorString());
	if (file.size() < 0
	    || file.size() > OicCatalogReader::maximum_document_size)
	{
		return fail(
			error,
			tr("Stored catalog exceeds the document safety limit."));
	}
	auto const bytes = file.readAll();
	if (sha256(bytes) != expected_sha)
	{
		return fail(
			error,
			tr(
				"Stored catalog bytes do not match their snapshot identity."));
	}
	return true;
}

bool createSnapshot(
	const QString& catalog_directory,
	const OicCatalogReadResult& catalog,
	const QDateTime& stored_at,
	const ImageryCatalogState& state,
	QString* error)
{
	auto const snapshots_directory =
		QDir(catalog_directory).filePath(QStringLiteral("snapshots"));
	if (!safeDirectory(snapshots_directory, error)
	    || !QDir().mkpath(snapshots_directory))
	{
		return fail(
			error,
			error && !error->isEmpty()
			  ? *error
			  : tr(
				  "Could not create the catalog snapshot directory."));
	}

	auto const sha = catalog.catalog.document_sha256;
	auto const final_path =
		QDir(snapshots_directory).filePath(QString::fromLatin1(sha));
	if (QFileInfo::exists(final_path))
	{
		if (!validateSnapshot(final_path, sha, error))
			return false;

		// Catalog bytes are immutable, but transport provenance may advance
		// when identical bytes are fetched from a new origin or revalidated.
		// Refresh that bounded recovery metadata atomically while retaining the
		// snapshot's original creation time.
		auto snapshot_stored_at = stored_at;
		if (auto const metadata =
		    readSnapshotMetadata(final_path, sha))
			snapshot_stored_at = metadata->stored_at;
		return saveFile(
			QDir(final_path).filePath(
				QStringLiteral("snapshot.json")),
			QJsonDocument(
				snapshotObject(
					catalog,
					snapshot_stored_at,
					state))
				.toJson(QJsonDocument::Compact),
			error);
	}

	auto const staging_path = QDir(snapshots_directory).filePath(
		QStringLiteral(".staging-")
		+ QUuid::createUuid().toString(QUuid::Id128));
	if (!QDir().mkpath(staging_path)
	    || !saveFile(
		    QDir(staging_path).filePath(catalogFilename()),
		    catalog.catalog.original_bytes,
		    error)
		    || !saveFile(
			    QDir(staging_path).filePath(QStringLiteral("snapshot.json")),
			    QJsonDocument(
				    snapshotObject(
					    catalog,
					    stored_at,
					    state))
				    .toJson(QJsonDocument::Compact),
		    error))
	{
		QDir(staging_path).removeRecursively();
		if (error && error->isEmpty())
			*error = tr(
				"Could not write the catalog snapshot.");
		return false;
	}

	if (!QDir().rename(staging_path, final_path))
	{
		QDir(staging_path).removeRecursively();
		if (QFileInfo::exists(final_path))
			return validateSnapshot(final_path, sha, error);
		return fail(
			error,
			tr("Could not activate the catalog snapshot."));
	}
	return true;
}

void pruneSnapshots(
	const QString& catalog_directory,
	const QSet<QByteArray>& retained)
{
	QDir snapshots(
		QDir(catalog_directory).filePath(QStringLiteral("snapshots")));
	if (!snapshots.exists())
		return;
	for (auto const& info : snapshots.entryInfoList(
		     QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden,
		     QDir::Name))
	{
		if (info.isSymLink())
			continue;
		auto const name = info.fileName().toLatin1();
		if ((name.startsWith(".staging-")
		     || (isSha256(name) && !retained.contains(name))))
			QDir(info.absoluteFilePath()).removeRecursively();
	}
}

bool writeCurrent(
	const QString& catalog_directory,
	const OicCatalogReadResult& catalog,
	const ImageryCatalogState& state,
	QString* error)
{
	return saveFile(
		QDir(catalog_directory).filePath(QStringLiteral("current.json")),
		QJsonDocument(currentObject(catalog, state))
			.toJson(QJsonDocument::Compact),
		error);
}

bool lockStore(
	const QString& root,
	std::unique_ptr<QLockFile>* lock,
	QString* error)
{
	if (!QDir().mkpath(root))
	{
		return fail(
			error,
			tr("Could not create the imagery catalog store."));
	}
	QFileInfo root_info(root);
	if (root_info.isSymLink() || !root_info.isDir())
	{
		return fail(
			error,
			tr("The imagery catalog store path is not a safe directory."));
	}
	auto acquired = std::make_unique<QLockFile>(
		QDir(root).filePath(QStringLiteral(".store.lock")));
	acquired->setStaleLockTime(30'000);
	if (!acquired->tryLock(5'000))
	{
		return fail(
			error,
			tr(
				"Another process is updating the imagery catalog store."));
	}
	*lock = std::move(acquired);
	return true;
}

}  // namespace


ImageryCatalogStore::ImageryCatalogStore(QString root)
{
	this->root = root.isEmpty()
		? QDir(
			QStandardPaths::writableLocation(
				QStandardPaths::AppDataLocation))
			  .filePath(QStringLiteral("imagery-catalogs"))
		: std::move(root);
}


QString ImageryCatalogStore::rootPath() const
{
	return root;
}


QString ImageryCatalogStore::directoryKey(
	const QString& catalog_id) const
{
	return QString::fromLatin1(
		sha256(catalog_id.toUtf8()).left(32));
}


InstalledImageryCatalog ImageryCatalogStore::loadDirectory(
	const QString& directory,
	QString* error) const
{
	InstalledImageryCatalog installed;
	installed.directory = directory;
	if (!safeDirectory(directory, error))
		return installed;

	auto const current_path =
		QDir(directory).filePath(QStringLiteral("current.json"));
	if (QFileInfo::exists(current_path))
	{
		auto const object = readObject(current_path, error);
		if (!object)
			return installed;
		QString catalog_id;
		int revision = 0;
		if (!parseCurrentState(
			    *object,
			    &installed.state,
			    &catalog_id,
			    &revision,
			    error))
			return installed;

		auto const expected_key = directoryKey(catalog_id);
		if (QFileInfo(directory).fileName() != expected_key)
		{
			fail(
				error,
				tr("Catalog store directory identity does not match its catalog."));
			return installed;
		}
		auto const snapshot_directory =
			QDir(directory).filePath(
				QStringLiteral("snapshots/")
				+ QString::fromLatin1(installed.state.sha256));
		QString active_error;
		if (validateSnapshot(
			    snapshot_directory,
			    installed.state.sha256,
			    &active_error))
		{
			installed.catalog_path =
				QDir(snapshot_directory).filePath(catalogFilename());
		}
		else if (!installed.state.previous_sha256.isEmpty())
		{
			auto const previous_directory =
				QDir(directory).filePath(
					QStringLiteral("snapshots/")
					+ QString::fromLatin1(
						installed.state.previous_sha256));
			QString previous_error;
			if (!validateSnapshot(
				    previous_directory,
				    installed.state.previous_sha256,
				    &previous_error))
			{
				fail(
					error,
					tr("The active and previous catalog snapshots "
					   "are both unavailable: %1; %2")
						.arg(active_error, previous_error));
				return installed;
			}
			installed.state.sha256 =
				installed.state.previous_sha256;
			installed.state.previous_sha256.clear();
			if (auto const metadata =
			    readSnapshotMetadata(
				    previous_directory,
				    installed.state.sha256);
			    metadata && !metadata->origin.isEmpty())
			{
				installed.state.origin =
					metadata->origin;
				installed.state.final_url =
					metadata->final_url.isEmpty()
						? metadata->origin
						: metadata->final_url;
				installed.state.updated_at =
					metadata->updated_at;
				installed.state.etag =
					metadata->etag;
				installed.state.last_modified =
					metadata->last_modified;
			}
			else
			{
				// Older snapshots did not retain transport provenance. Never
				// apply the damaged active snapshot's validators to different
				// bytes; force an unconditional refresh from the best origin
				// still available in current.json.
				installed.state.final_url =
					installed.state.origin;
				installed.state.etag.clear();
				installed.state.last_modified.clear();
				if (metadata)
					installed.state.updated_at =
						metadata->updated_at;
			}
			installed.state.recovered_previous = true;
			installed.catalog_path =
				QDir(previous_directory).filePath(
					catalogFilename());
			if (error)
			{
				*error = tr(
					"The active catalog snapshot was damaged; "
					"the previous snapshot is being used.");
			}
		}
		else
		{
			fail(error, active_error);
			return installed;
		}
	}
	else
	{
		installed.catalog_path =
			QDir(directory).filePath(catalogFilename());
		auto const state_path =
			QDir(directory).filePath(QStringLiteral("state.json"));
		if (!QFileInfo::exists(installed.catalog_path)
		    || !QFileInfo::exists(state_path))
		{
			fail(
				error,
				tr("Catalog store entry has no active snapshot."));
			return installed;
		}
		auto const object = readObject(state_path, error);
		if (!object
		    || !parseLegacyState(*object, &installed.state))
		{
			fail(
				error,
				tr("Legacy catalog store metadata is invalid."));
			return installed;
		}
	}

	QFile catalog_file(installed.catalog_path);
	if (!catalog_file.open(QIODevice::ReadOnly))
	{
		fail(error, catalog_file.errorString());
		return installed;
	}
	if (catalog_file.size() < 0
	    || catalog_file.size()
	         > OicCatalogReader::maximum_document_size)
	{
		fail(
			error,
			tr("Stored catalog exceeds the document safety limit."));
		return installed;
	}
	auto const bytes = catalog_file.readAll();
	if (sha256(bytes) != installed.state.sha256)
	{
		fail(
			error,
			tr("Stored catalog checksum does not match its metadata."));
		return installed;
	}
	installed.read_result = OicCatalogReader::read(bytes);
	if (!installed.read_result.accepted())
	{
		fail(error, tr("Installed catalog is invalid."));
		return installed;
	}
	if (installed.read_result.catalog.document_sha256
	      != installed.state.sha256
	    || QFileInfo(directory).fileName()
	         != directoryKey(installed.read_result.catalog.id))
	{
		fail(
			error,
			tr("Installed catalog identity does not match its store entry."));
		installed.read_result = {};
		return installed;
	}
	return installed;
}


QVector<InstalledImageryCatalog> ImageryCatalogStore::catalogs(
	QVector<ImageryCatalogStoreIssue>* issues) const
{
	QVector<InstalledImageryCatalog> installed;
	QDir directory(root);
	if (!directory.exists())
		return installed;
	if (!QFileInfo(root).isDir() || QFileInfo(root).isSymLink())
	{
		if (issues)
		{
			issues->push_back({
				root,
				tr("The imagery catalog store path is not a safe directory."),
			});
		}
		return installed;
	}

	for (auto const& info : directory.entryInfoList(
		     QDir::Dirs | QDir::NoDotAndDotDot,
		     QDir::Name))
	{
		if (info.fileName().startsWith(QLatin1Char('.')))
			continue;
		QString load_error;
		auto catalog =
			loadDirectory(info.absoluteFilePath(), &load_error);
		if (!catalog.read_result.accepted())
		{
			if (issues)
			{
				issues->push_back({
					info.absoluteFilePath(),
					load_error.isEmpty()
					  ? tr("Could not load the installed imagery catalog.")
					  : load_error,
				});
			}
			continue;
		}
		if (issues && !load_error.isEmpty())
		{
			issues->push_back({
				info.absoluteFilePath(),
				load_error,
			});
		}
		installed.push_back(std::move(catalog));
	}

	std::stable_sort(
		installed.begin(),
		installed.end(),
		[](auto const& left, auto const& right) {
			auto const by_name = QString::localeAwareCompare(
				left.read_result.catalog.name,
				right.read_result.catalog.name);
			if (by_name != 0)
				return by_name < 0;
			return left.read_result.catalog.id
			     < right.read_result.catalog.id;
		});
	return installed;
}


ImageryCatalogAnalysis ImageryCatalogStore::analyze(
	const OicCatalogReadResult& candidate,
	QVector<ImageryCatalogStoreIssue>* issues) const
{
	ImageryCatalogAnalysis analysis;
	QSet<int> invalid_sources;
	QSet<int> unsupported_sources;
	for (auto const& diagnostic : candidate.diagnostics)
	{
		if (diagnostic.source_index < 0)
			continue;
		if (diagnostic.kind == OicDiagnosticKind::SourceError)
			invalid_sources.insert(diagnostic.source_index);
		else if (diagnostic.kind
		         == OicDiagnosticKind::UnsupportedSource)
			unsupported_sources.insert(diagnostic.source_index);
	}
	analysis.invalid = invalid_sources.size();
	analysis.unsupported = unsupported_sources.size();

	auto const installed = catalogs(issues);
	const InstalledImageryCatalog* previous = nullptr;
	for (auto const& catalog : installed)
	{
		if (catalog.read_result.catalog.id == candidate.catalog.id)
		{
			previous = &catalog;
			break;
		}
	}

	if (previous)
	{
		if (previous->read_result.catalog.revision
		      < candidate.catalog.revision)
			analysis.update_kind =
				ImageryCatalogAnalysis::UpdateKind::HigherRevision;
		else if (previous->read_result.catalog.revision
		         > candidate.catalog.revision)
			analysis.update_kind =
				ImageryCatalogAnalysis::UpdateKind::LowerRevision;
		else if (previous->state.sha256
		         == candidate.catalog.document_sha256)
			analysis.update_kind =
				ImageryCatalogAnalysis::UpdateKind::ExactReimport;
		else
			analysis.update_kind =
				ImageryCatalogAnalysis::UpdateKind::SameRevisionConflict;

		QMap<QString, QByteArray> old_sources;
		QMap<QString, QByteArray> old_operational_sources;
		QMap<QString, QString> old_source_names;
		for (auto const& source
		     : previous->read_result.catalog.sources)
		{
			if (!source.metadata.id.isEmpty())
			{
				old_sources.insert(
					source.metadata.id,
					source.full_fingerprint);
				old_operational_sources.insert(
					source.metadata.id,
					source.operational_fingerprint);
				old_source_names.insert(
					source.metadata.id,
					source.metadata.name);
			}
		}
		for (auto const& source : candidate.catalog.sources)
		{
			if (source.metadata.id.isEmpty())
				continue;
			auto const found =
				old_sources.find(source.metadata.id);
			if (found == old_sources.end())
			{
				++analysis.added;
				analysis.source_changes.push_back({
					ImageryCatalogSourceChangeKind::Added,
					source.metadata.id,
					source.metadata.name,
				});
			}
			else
			{
				if (found.value() != source.full_fingerprint)
				{
					++analysis.changed;
					auto const operational =
						old_operational_sources.value(
							source.metadata.id)
						!= source.operational_fingerprint;
					if (operational)
						++analysis.operational_changed;
					else
						++analysis.metadata_only_changed;
					analysis.source_changes.push_back({
						operational
						  ? ImageryCatalogSourceChangeKind::Operational
						  : ImageryCatalogSourceChangeKind::MetadataOnly,
						source.metadata.id,
						source.metadata.name,
					});
				}
				old_sources.erase(found);
				old_operational_sources.remove(source.metadata.id);
				old_source_names.remove(source.metadata.id);
			}
		}
		analysis.removed = old_sources.size();
		for (auto it = old_sources.cbegin();
		     it != old_sources.cend();
		     ++it)
		{
			analysis.source_changes.push_back({
				ImageryCatalogSourceChangeKind::Removed,
				it.key(),
				old_source_names.value(it.key()),
			});
		}
	}
	else
	{
		for (auto const& source : candidate.catalog.sources)
		{
			if (!source.metadata.id.isEmpty())
			{
				++analysis.added;
				analysis.source_changes.push_back({
					ImageryCatalogSourceChangeKind::Added,
					source.metadata.id,
					source.metadata.name,
				});
			}
		}
	}

	QSet<QByteArray> existing_full;
	QSet<QByteArray> existing_operational;
	for (auto const& catalog : installed)
	{
		if (catalog.read_result.catalog.id == candidate.catalog.id)
			continue;
		for (auto const& existing
		     : catalog.read_result.catalog.sources)
		{
			if (!existing.full_fingerprint.isEmpty())
				existing_full.insert(existing.full_fingerprint);
			if (!existing.operational_fingerprint.isEmpty())
			{
				existing_operational.insert(
					existing.operational_fingerprint);
			}
		}
	}
	for (auto const& source : candidate.catalog.sources)
	{
		if (source.full_fingerprint.isEmpty()
		    || source.operational_fingerprint.isEmpty())
			continue;
		if (existing_full.contains(source.full_fingerprint))
			++analysis.exact_duplicates;
		else if (existing_operational.contains(
			         source.operational_fingerprint))
			++analysis.potential_duplicates;
	}
	return analysis;
}


bool ImageryCatalogStore::install(
	const OicCatalogReadResult& catalog,
	const QString& origin,
	const QByteArray& etag,
	const QByteArray& last_modified,
	QString* error) const
{
	return install(
		catalog,
		ImageryCatalogInstallMetadata {
			origin,
			origin,
			etag,
			last_modified,
		},
		ImageryCatalogInstallOptions {},
		error);
}


bool ImageryCatalogStore::install(
	const OicCatalogReadResult& catalog,
	const ImageryCatalogInstallMetadata& metadata,
	const ImageryCatalogInstallOptions& options,
	QString* error) const
{
	if (!catalog.accepted())
		return fail(error, tr("Catalog is not installable."));
	if (!isSha256(catalog.catalog.document_sha256)
	    || sha256(catalog.catalog.original_bytes)
	         != catalog.catalog.document_sha256)
	{
		return fail(
			error,
			tr("Catalog document identity is invalid."));
	}

	std::unique_ptr<QLockFile> lock;
	if (!lockStore(root, &lock, error))
		return false;

	auto const catalog_directory = QDir(root).filePath(
		directoryKey(catalog.catalog.id));
	if (!safeDirectory(catalog_directory, error)
	    || !QDir().mkpath(catalog_directory))
	{
		return fail(
			error,
			error && !error->isEmpty()
			  ? *error
			  : tr("Could not create the catalog installation directory."));
	}

	QString previous_error;
	auto previous =
		loadDirectory(catalog_directory, &previous_error);
	auto const had_previous = previous.read_result.accepted();
	auto const now = QDateTime::currentDateTimeUtc();
	auto const exact_reimport =
		had_previous
		&& previous.state.sha256
		     == catalog.catalog.document_sha256;
	if (had_previous
	    && previous.read_result.catalog.revision
	         > catalog.catalog.revision
	    && !options.allow_lower_revision)
	{
		return fail(
			error,
			tr("Installing an older catalog revision requires explicit approval."));
	}
	if (had_previous
	    && previous.read_result.catalog.revision
	         == catalog.catalog.revision
	    && previous.state.sha256
	         != catalog.catalog.document_sha256
	    && !options.allow_same_revision_conflict)
	{
		return fail(
			error,
			tr("This catalog revision was republished with different contents."));
	}

	ImageryCatalogState state;
	auto const incoming_final_url = metadata.final_url.isEmpty()
		? metadata.origin
		: metadata.final_url;
	auto const preserve_remote_provenance =
		exact_reimport
		&& isRemoteOrigin(
			previous.state.final_url.isEmpty()
				? previous.state.origin
				: previous.state.final_url)
		&& !isRemoteOrigin(incoming_final_url);
	state.origin = preserve_remote_provenance
		? previous.state.origin
		: metadata.origin;
	state.final_url = preserve_remote_provenance
		? previous.state.final_url
		: incoming_final_url;
	state.installed_at =
		had_previous ? previous.state.installed_at : now;
	state.updated_at =
		exact_reimport ? previous.state.updated_at : now;
	state.checked_at = now;
	state.sha256 = catalog.catalog.document_sha256;
	state.etag = sanitizedValidator(
		preserve_remote_provenance
			? previous.state.etag
			: metadata.etag);
	state.last_modified = sanitizedValidator(
		preserve_remote_provenance
			? previous.state.last_modified
			: metadata.last_modified);
	if (had_previous
	    && previous.state.sha256 != state.sha256)
		state.previous_sha256 = previous.state.sha256;
	else if (had_previous)
		state.previous_sha256 = previous.state.previous_sha256;

	if (had_previous && previous.state.legacy_layout
	    && !createSnapshot(
		    catalog_directory,
		    previous.read_result,
		    previous.state.installed_at,
		    previous.state,
		    error))
		return false;
	if (!createSnapshot(
		    catalog_directory,
		    catalog,
		    now,
		    state,
		    error))
		return false;

	if (!writeCurrent(
		    catalog_directory,
		    catalog,
		    state,
		    error))
		return false;

	QSet<QByteArray> retained { state.sha256 };
	if (!state.previous_sha256.isEmpty())
		retained.insert(state.previous_sha256);
	pruneSnapshots(catalog_directory, retained);
	return true;
}


bool ImageryCatalogStore::markChecked(
	const QString& catalog_id,
	const QString& final_url,
	const QByteArray& etag,
	const QByteArray& last_modified,
	QString* error) const
{
	std::unique_ptr<QLockFile> lock;
	if (!lockStore(root, &lock, error))
		return false;
	auto const catalog_directory =
		QDir(root).filePath(directoryKey(catalog_id));
	QString load_error;
	auto installed =
		loadDirectory(catalog_directory, &load_error);
	if (!installed.read_result.accepted())
	{
		return fail(
			error,
			load_error.isEmpty()
			  ? tr("The imagery catalog is not installed.")
			  : load_error);
	}
	if (installed.read_result.catalog.id != catalog_id)
		return fail(error, tr("Catalog store identity mismatch."));

	if (installed.state.legacy_layout
	    && !createSnapshot(
		    catalog_directory,
		    installed.read_result,
		    installed.state.installed_at,
		    installed.state,
		    error))
		return false;
	installed.state.legacy_layout = false;
	installed.state.checked_at = QDateTime::currentDateTimeUtc();
	if (!final_url.isEmpty())
		installed.state.final_url = final_url;
	installed.state.etag = sanitizedValidator(etag);
	installed.state.last_modified =
		sanitizedValidator(last_modified);
	if (!createSnapshot(
		    catalog_directory,
		    installed.read_result,
		    installed.state.installed_at,
		    installed.state,
		    error))
		return false;
	return writeCurrent(
		catalog_directory,
		installed.read_result,
		installed.state,
		error);
}


bool ImageryCatalogStore::remove(
	const QString& catalog_id,
	QString* error) const
{
	std::unique_ptr<QLockFile> lock;
	if (!lockStore(root, &lock, error))
		return false;
	auto const path =
		QDir(root).filePath(directoryKey(catalog_id));
	QFileInfo info(path);
	if (!info.exists())
		return true;
	if (info.isSymLink() || !info.isDir())
	{
		return fail(
			error,
			tr("Refusing to remove an unsafe catalog store entry."));
	}
	if (!QDir(path).removeRecursively())
	{
		return fail(
			error,
			tr("Could not remove the imagery catalog."));
	}
	return true;
}

}  // namespace OpenOrienteering::imagery
