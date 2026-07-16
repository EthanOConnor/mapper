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

#ifndef OPENORIENTEERING_IMAGERY_CATALOG_REPOSITORY_H
#define OPENORIENTEERING_IMAGERY_CATALOG_REPOSITORY_H

#include <memory>

#include <QHash>
#include <QObject>
#include <QSharedPointer>
#include <QString>
#include <QThreadPool>
#include <QUrl>
#include <QVector>

#include "imagery/imagery_catalog_store.h"
#include "imagery/tile_network_manager.h"

namespace OpenOrienteering::imagery {

struct ImagerySourceHandle
{
	QString catalog_id;
	QString source_id;
	QByteArray catalog_sha256;
	int source_index = -1;

	bool isValid() const noexcept;
	bool operator==(const ImagerySourceHandle&) const = default;
};

inline size_t qHash(
	const ImagerySourceHandle& handle,
	size_t seed = 0) noexcept
{
	seed = qHash(handle.catalog_id, seed);
	seed = qHash(handle.source_id, seed);
	seed = qHash(handle.catalog_sha256, seed);
	return ::qHash(handle.source_index, seed);
}

struct ImageryCatalogRepositorySnapshot
{
	quint64 generation = 0;
	QVector<InstalledImageryCatalog> catalogs;
	QVector<ImageryCatalogStoreIssue> issues;

	const InstalledImageryCatalog* catalog(
		const QString& catalog_id,
		const QByteArray& sha256 = {}) const noexcept;
	const OicSourceDefinition* source(
		const ImagerySourceHandle& handle,
		const InstalledImageryCatalog** installed = nullptr) const noexcept;
	std::optional<ImagerySourceHandle> latestHandle(
		const QString& catalog_id,
		const QString& source_id) const;
};

using ImageryCatalogRepositorySnapshotPtr =
	QSharedPointer<const ImageryCatalogRepositorySnapshot>;

struct ImageryCatalogCandidate
{
	OicCatalogReadResult read_result;
	ImageryCatalogAnalysis analysis;
	ImageryCatalogInstallMetadata metadata;
};

using ImageryCatalogCandidatePtr =
	QSharedPointer<const ImageryCatalogCandidate>;

struct ImageryCatalogFetchRequest
{
	QUrl url;
	QByteArray etag;
	QByteArray last_modified;
	QString installed_catalog_id;
	bool allow_insecure_http = false;
};

enum class ImageryCatalogOperationKind
{
	CandidateReady,
	NotModified,
	Installed,
	Removed,
	Cancelled,
	Failed,
};

struct ImageryCatalogOperationResult
{
	ImageryCatalogOperationKind kind =
		ImageryCatalogOperationKind::Failed;
	ImageryCatalogCandidatePtr candidate;
	ImageryCatalogInstallMetadata metadata;
	QString catalog_id;
	QString error;
	QUrl private_network_approval_url;
};

/**
 * Application-facing asynchronous catalog index and operation coordinator.
 *
 * Store scans, JSON parsing, update analysis, and filesystem mutations run on
 * one bounded worker. Network transfers share TileNetworkManager's connection
 * pool and security policy. The published snapshot is immutable and selections
 * use catalog/source/document identities. Invalid rows without a trustworthy
 * source ID use their index only within an immutable catalog snapshot.
 */
class ImageryCatalogRepository final : public QObject
{
Q_OBJECT

public:
	using OperationId = quint64;

	explicit ImageryCatalogRepository(
		QString store_root = {},
		TileNetworkManager* network = nullptr,
		QObject* parent = nullptr);
	~ImageryCatalogRepository() override;

	ImageryCatalogRepository(
		const ImageryCatalogRepository&) = delete;
	ImageryCatalogRepository& operator=(
		const ImageryCatalogRepository&) = delete;

	static ImageryCatalogRepository& instance();

	QString storeRoot() const;
	TileNetworkManager& networkManager() const noexcept;
	ImageryCatalogRepositorySnapshotPtr snapshot() const;

	void reload();
	OperationId readCatalogFile(const QString& path);
	OperationId fetchCatalog(
		const ImageryCatalogFetchRequest& request);
	OperationId installCandidate(
		ImageryCatalogCandidatePtr candidate,
		ImageryCatalogInstallOptions options = {});
	OperationId removeCatalog(const QString& catalog_id);
	void cancel(OperationId operation_id);

signals:
	void snapshotChanged(quint64 generation);
	void operationFinished(
		OpenOrienteering::imagery::ImageryCatalogRepository::OperationId id,
		const OpenOrienteering::imagery::ImageryCatalogOperationResult& result);

private:
	struct Operation;

	OperationId nextOperationId();
	void parseCandidate(
		const std::shared_ptr<Operation>& operation,
		QByteArray bytes,
		ImageryCatalogInstallMetadata metadata);
	void complete(
		OperationId operation_id,
		ImageryCatalogOperationResult result);
	void onNetworkFinished(
		TileNetworkManager::Token token,
		const TileNetworkResult& result);

	QString store_root_;
	TileNetworkManager* network_ = nullptr;
	quint64 next_operation_id_ = 1;
	quint64 reload_generation_ = 0;
	ImageryCatalogRepositorySnapshotPtr snapshot_;
	QThreadPool worker_pool_;
	QHash<OperationId, std::shared_ptr<Operation>> operations_;
	QHash<TileNetworkManager::Token, OperationId> network_operations_;
};

}  // namespace OpenOrienteering::imagery

Q_DECLARE_METATYPE(
	OpenOrienteering::imagery::ImageryCatalogOperationResult)
Q_DECLARE_METATYPE(OpenOrienteering::imagery::ImagerySourceHandle)

#endif
