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

#include "imagery/imagery_catalog_repository.h"

#include <atomic>
#include <utility>

#include <QFile>
#include <QFileInfo>
#include <QApplicationStatic>
#include <QCoreApplication>
#include <QMetaObject>
#include <QPointer>
#include <QStandardPaths>
#include <QThread>

namespace OpenOrienteering::imagery {

struct ImageryCatalogRepository::Operation
{
	OperationId id = 0;
	TileNetworkManager::Token network_token = 0;
	ImageryCatalogFetchRequest fetch_request;
	std::atomic_bool cancelled { false };
};


bool ImagerySourceHandle::isValid() const noexcept
{
	return !catalog_id.isEmpty()
	       && catalog_sha256.size() == 64
	       && ((!source_id.isEmpty() && source_index < 0)
	           || (source_id.isEmpty() && source_index >= 0));
}


const InstalledImageryCatalog*
ImageryCatalogRepositorySnapshot::catalog(
	const QString& catalog_id,
	const QByteArray& sha256) const noexcept
{
	for (auto const& installed : catalogs)
	{
		if (installed.read_result.catalog.id == catalog_id
		    && (sha256.isEmpty()
		        || installed.state.sha256 == sha256))
			return &installed;
	}
	return nullptr;
}


const OicSourceDefinition*
ImageryCatalogRepositorySnapshot::source(
	const ImagerySourceHandle& handle,
	const InstalledImageryCatalog** installed) const noexcept
{
	auto const* found_catalog =
		catalog(handle.catalog_id, handle.catalog_sha256);
	if (!found_catalog)
		return nullptr;
	if (handle.source_id.isEmpty())
	{
		if (handle.source_index < 0
		    || handle.source_index
		         >= found_catalog->read_result.catalog.sources.size())
			return nullptr;
		if (installed)
			*installed = found_catalog;
		return &found_catalog->read_result.catalog.sources.at(
			handle.source_index);
	}
	for (auto const& definition
	     : found_catalog->read_result.catalog.sources)
	{
		if (definition.metadata.id == handle.source_id)
		{
			if (installed)
				*installed = found_catalog;
			return &definition;
		}
	}
	return nullptr;
}


std::optional<ImagerySourceHandle>
ImageryCatalogRepositorySnapshot::latestHandle(
	const QString& catalog_id,
	const QString& source_id) const
{
	if (source_id.isEmpty())
		return std::nullopt;
	auto const* found_catalog = catalog(catalog_id);
	if (!found_catalog)
		return std::nullopt;
	for (auto const& definition
	     : found_catalog->read_result.catalog.sources)
	{
		if (definition.metadata.id == source_id)
		{
			return ImagerySourceHandle {
				catalog_id,
				source_id,
				found_catalog->state.sha256,
				-1,
			};
		}
	}
	return std::nullopt;
}


ImageryCatalogRepository::ImageryCatalogRepository(
	QString store_root,
	TileNetworkManager* network,
	QObject* parent)
 : QObject(parent)
 , store_root_(
	   store_root.isEmpty()
	     ? ImageryCatalogStore {}.rootPath()
	     : std::move(store_root))
 , network_(
	   network ? network : &TileNetworkManager::instance())
 , snapshot_(
	   QSharedPointer<ImageryCatalogRepositorySnapshot>::create())
{
	Q_ASSERT(network_);
	Q_ASSERT(thread() == network_->thread());
	worker_pool_.setMaxThreadCount(1);
	worker_pool_.setExpiryTimeout(30'000);
	qRegisterMetaType<ImageryCatalogOperationResult>();
	connect(
		network_,
		&TileNetworkManager::finished,
		this,
		&ImageryCatalogRepository::onNetworkFinished);
	reload();
}


ImageryCatalogRepository::~ImageryCatalogRepository()
{
	for (auto const& operation : std::as_const(operations_))
	{
		operation->cancelled.store(true);
		if (operation->network_token)
			network_->cancel(operation->network_token);
	}
	operations_.clear();
	network_operations_.clear();
	worker_pool_.clear();
	worker_pool_.waitForDone();
}


Q_APPLICATION_STATIC(
	ImageryCatalogRepository,
	application_catalog_repository)

ImageryCatalogRepository&
ImageryCatalogRepository::instance()
{
	auto* application = QCoreApplication::instance();
	Q_ASSERT(application);
	Q_ASSERT(QThread::currentThread() == application->thread());
	return *application_catalog_repository;
}


QString ImageryCatalogRepository::storeRoot() const
{
	return store_root_;
}


TileNetworkManager&
ImageryCatalogRepository::networkManager() const noexcept
{
	return *network_;
}


ImageryCatalogRepositorySnapshotPtr
ImageryCatalogRepository::snapshot() const
{
	return snapshot_;
}


void ImageryCatalogRepository::reload()
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto const generation = ++reload_generation_;
	auto const root = store_root_;
	QPointer<ImageryCatalogRepository> receiver(this);
	worker_pool_.start([receiver, root, generation] {
		auto next =
			QSharedPointer<ImageryCatalogRepositorySnapshot>::create();
		next->generation = generation;
		ImageryCatalogStore store(root);
		next->catalogs = store.catalogs(&next->issues);
		QMetaObject::invokeMethod(
			receiver,
			[receiver, generation, next = std::move(next)] {
				if (!receiver
				    || generation != receiver->reload_generation_)
					return;
				receiver->snapshot_ = next;
				emit receiver->snapshotChanged(generation);
			},
			Qt::QueuedConnection);
	});
}


ImageryCatalogRepository::OperationId
ImageryCatalogRepository::nextOperationId()
{
	auto const id = next_operation_id_++;
	if (id == 0)
		qFatal("Imagery catalog operation identity space exhausted");
	return id;
}


ImageryCatalogRepository::OperationId
ImageryCatalogRepository::readCatalogFile(
	const QString& path)
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto operation = std::make_shared<Operation>();
	operation->id = nextOperationId();
	operations_.insert(operation->id, operation);

	auto const absolute_path = QFileInfo(path).absoluteFilePath();
	auto const origin =
		QUrl::fromLocalFile(absolute_path).toString();
	QPointer<ImageryCatalogRepository> receiver(this);
	auto const root = store_root_;
	worker_pool_.start(
		[receiver, operation, absolute_path, origin, root] {
			ImageryCatalogOperationResult result;
			if (operation->cancelled.load())
				return;
			QFile file(absolute_path);
			if (!file.open(QIODevice::ReadOnly))
			{
				result.kind = ImageryCatalogOperationKind::Failed;
				result.error = file.errorString();
			}
			else if (file.size() < 0
			         || file.size()
			              > OicCatalogReader::maximum_document_size)
			{
				result.kind = ImageryCatalogOperationKind::Failed;
				result.error = ImageryCatalogRepository::tr(
					"The catalog exceeds the %1 MiB safety limit.")
					.arg(
						OicCatalogReader::maximum_document_size
						/ (1024 * 1024));
			}
			else
			{
				auto candidate =
					QSharedPointer<ImageryCatalogCandidate>::create();
				candidate->metadata.origin = origin;
				candidate->metadata.final_url = origin;
				candidate->read_result =
					OicCatalogReader::read(file.readAll());
				ImageryCatalogStore store(root);
				candidate->analysis =
					store.analyze(candidate->read_result);
				result.kind =
					ImageryCatalogOperationKind::CandidateReady;
				result.candidate = std::move(candidate);
			}
			QMetaObject::invokeMethod(
				receiver,
				[receiver, id = operation->id,
				 result = std::move(result)]() mutable {
					if (receiver)
						receiver->complete(id, std::move(result));
				},
				Qt::QueuedConnection);
		});
	return operation->id;
}


ImageryCatalogRepository::OperationId
ImageryCatalogRepository::fetchCatalog(
	const ImageryCatalogFetchRequest& request)
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto operation = std::make_shared<Operation>();
	operation->id = nextOperationId();
	operation->fetch_request = request;
	operations_.insert(operation->id, operation);

	auto const scheme = request.url.scheme().toLower();
	if (scheme == QLatin1String("http")
	    && !request.allow_insecure_http)
	{
		ImageryCatalogOperationResult result;
		result.kind = ImageryCatalogOperationKind::Failed;
		result.error = tr(
			"Downloading a catalog over plain HTTP requires explicit approval.");
		QMetaObject::invokeMethod(
			this,
			[this, id = operation->id,
			 result = std::move(result)]() mutable {
				complete(id, std::move(result));
			},
			Qt::QueuedConnection);
		return operation->id;
	}

	TileNetworkRequest network_request;
	network_request.url = request.url;
	network_request.client_id =
		TileNetworkManager::nextClientId();
	network_request.generation = operation->id;
	network_request.priority = TileRequestPriority::Visible;
	network_request.payload_kind =
		NetworkPayloadKind::JsonDocument;
	network_request.empty_http_status_codes.clear();
	network_request.if_none_match = request.etag;
	network_request.if_modified_since =
		request.last_modified;
	network_request.max_response_bytes =
		OicCatalogReader::maximum_document_size;
	operation->network_token =
		network_->submit(std::move(network_request));
	network_operations_.insert(
		operation->network_token,
		operation->id);
	return operation->id;
}


void ImageryCatalogRepository::parseCandidate(
	const std::shared_ptr<Operation>& operation,
	QByteArray bytes,
	ImageryCatalogInstallMetadata metadata)
{
	QPointer<ImageryCatalogRepository> receiver(this);
	auto const root = store_root_;
	worker_pool_.start(
		[receiver, operation, root,
		 bytes = std::move(bytes),
		 metadata = std::move(metadata)]() mutable {
			if (operation->cancelled.load())
				return;
			auto candidate =
				QSharedPointer<ImageryCatalogCandidate>::create();
			candidate->metadata = std::move(metadata);
			candidate->read_result =
				OicCatalogReader::read(bytes);
			auto const expected_catalog_id =
				operation->fetch_request.installed_catalog_id;
			if (!expected_catalog_id.isEmpty()
			    && candidate->read_result.catalog.id
			         != expected_catalog_id)
			{
				ImageryCatalogOperationResult result;
				result.kind =
					ImageryCatalogOperationKind::Failed;
				result.error = ImageryCatalogRepository::tr(
					"The downloaded catalog ID does not match the "
					"installed catalog being updated.");
				QMetaObject::invokeMethod(
					receiver,
					[receiver, id = operation->id,
					 result = std::move(result)]() mutable {
						if (receiver)
							receiver->complete(
								id,
								std::move(result));
					},
					Qt::QueuedConnection);
				return;
			}
			ImageryCatalogStore store(root);
			candidate->analysis =
				store.analyze(candidate->read_result);
			ImageryCatalogOperationResult result;
			result.kind =
				ImageryCatalogOperationKind::CandidateReady;
			result.candidate = std::move(candidate);
			QMetaObject::invokeMethod(
				receiver,
				[receiver, id = operation->id,
				 result = std::move(result)]() mutable {
					if (receiver)
						receiver->complete(id, std::move(result));
				},
				Qt::QueuedConnection);
		});
}


ImageryCatalogRepository::OperationId
ImageryCatalogRepository::installCandidate(
	ImageryCatalogCandidatePtr candidate,
	ImageryCatalogInstallOptions options)
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto operation = std::make_shared<Operation>();
	operation->id = nextOperationId();
	operations_.insert(operation->id, operation);

	QPointer<ImageryCatalogRepository> receiver(this);
	auto const root = store_root_;
	worker_pool_.start(
		[receiver, operation, root,
		 candidate = std::move(candidate),
		 options]() mutable {
			ImageryCatalogOperationResult result;
			if (operation->cancelled.load())
				return;
			if (!candidate)
			{
				result.kind = ImageryCatalogOperationKind::Failed;
				result.error = ImageryCatalogRepository::tr(
					"No imagery catalog candidate was provided.");
			}
			else
			{
				QString error;
				ImageryCatalogStore store(root);
				auto const success = store.install(
					candidate->read_result,
					candidate->metadata,
					options,
					&error);
				result.kind = success
					? ImageryCatalogOperationKind::Installed
					: ImageryCatalogOperationKind::Failed;
				result.catalog_id =
					candidate->read_result.catalog.id;
				result.error = std::move(error);
			}
			QMetaObject::invokeMethod(
				receiver,
				[receiver, id = operation->id,
				 result = std::move(result)]() mutable {
					if (receiver)
						receiver->complete(id, std::move(result));
				},
				Qt::QueuedConnection);
		});
	return operation->id;
}


ImageryCatalogRepository::OperationId
ImageryCatalogRepository::removeCatalog(
	const QString& catalog_id)
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto operation = std::make_shared<Operation>();
	operation->id = nextOperationId();
	operations_.insert(operation->id, operation);

	QPointer<ImageryCatalogRepository> receiver(this);
	auto const root = store_root_;
	worker_pool_.start(
		[receiver, operation, root, catalog_id] {
			ImageryCatalogOperationResult result;
			if (operation->cancelled.load())
				return;
			QString error;
			ImageryCatalogStore store(root);
			auto const success =
				store.remove(catalog_id, &error);
			result.kind = success
				? ImageryCatalogOperationKind::Removed
				: ImageryCatalogOperationKind::Failed;
			result.catalog_id = catalog_id;
			result.error = std::move(error);
			QMetaObject::invokeMethod(
				receiver,
				[receiver, id = operation->id,
				 result = std::move(result)]() mutable {
					if (receiver)
						receiver->complete(id, std::move(result));
				},
				Qt::QueuedConnection);
		});
	return operation->id;
}


void ImageryCatalogRepository::cancel(
	OperationId operation_id)
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto operation = operations_.take(operation_id);
	if (!operation)
		return;
	operation->cancelled.store(true);
	if (operation->network_token)
	{
		network_operations_.remove(
			operation->network_token);
		network_->cancel(operation->network_token);
	}
	ImageryCatalogOperationResult result;
	result.kind = ImageryCatalogOperationKind::Cancelled;
	emit operationFinished(operation_id, result);
}


void ImageryCatalogRepository::onNetworkFinished(
	TileNetworkManager::Token token,
	const TileNetworkResult& network_result)
{
	auto const operation_id =
		network_operations_.take(token);
	if (!operation_id)
		return;
	auto operation = operations_.value(operation_id);
	if (!operation || operation->cancelled.load())
		return;
	operation->network_token = 0;

	ImageryCatalogInstallMetadata metadata;
	metadata.origin =
		operation->fetch_request.url.toString();
	metadata.final_url =
		network_result.final_url.isEmpty()
			? metadata.origin
			: network_result.final_url.toString();
	metadata.etag = network_result.etag;
	metadata.last_modified =
		network_result.last_modified;

	if (network_result.outcome
	    == TileNetworkResult::Outcome::Success)
	{
		parseCandidate(
			operation,
			network_result.body,
			std::move(metadata));
		return;
	}
	if (network_result.outcome
	    == TileNetworkResult::Outcome::NotModified)
	{
		if (metadata.etag.isEmpty())
			metadata.etag =
				operation->fetch_request.etag;
		if (metadata.last_modified.isEmpty())
			metadata.last_modified =
				operation->fetch_request.last_modified;
		auto const catalog_id =
			operation->fetch_request.installed_catalog_id;
		if (catalog_id.isEmpty())
		{
			ImageryCatalogOperationResult result;
			result.kind =
				ImageryCatalogOperationKind::NotModified;
			result.metadata = std::move(metadata);
			complete(operation_id, std::move(result));
			return;
		}

		QPointer<ImageryCatalogRepository> receiver(this);
		auto const root = store_root_;
		worker_pool_.start(
			[receiver, operation, root, catalog_id,
			 metadata = std::move(metadata)]() mutable {
				if (operation->cancelled.load())
					return;
				QString error;
				ImageryCatalogStore store(root);
				auto const success = store.markChecked(
					catalog_id,
					metadata.final_url,
					metadata.etag,
					metadata.last_modified,
					&error);
				ImageryCatalogOperationResult result;
				result.kind = success
					? ImageryCatalogOperationKind::NotModified
					: ImageryCatalogOperationKind::Failed;
				result.catalog_id = catalog_id;
				result.metadata = std::move(metadata);
				result.error = std::move(error);
				QMetaObject::invokeMethod(
					receiver,
					[receiver, id = operation->id,
					 result = std::move(result)]() mutable {
						if (receiver)
						{
							receiver->complete(
								id,
								std::move(result));
						}
					},
					Qt::QueuedConnection);
			});
		return;
	}

	ImageryCatalogOperationResult result;
	result.kind = network_result.outcome
		== TileNetworkResult::Outcome::Cancelled
		? ImageryCatalogOperationKind::Cancelled
		: ImageryCatalogOperationKind::Failed;
	result.error = network_result.error_string;
	result.metadata = std::move(metadata);
	auto const approval_url =
		network_result.private_network_rejected_url;
	auto const approval_scheme =
		approval_url.scheme().toLower();
	if (network_result.private_network_rejected
	    && approval_url.isValid()
	    && !approval_url.isRelative()
	    && !approval_url.host().isEmpty()
	    && approval_url.userInfo().isEmpty()
	    && !approval_url.hasFragment()
	    && (approval_scheme == QLatin1String("http")
	        || approval_scheme == QLatin1String("https"))
	    && !network_->isPrivateOriginApproved(
		    approval_url))
	{
		result.private_network_approval_url =
			approval_url;
	}
	complete(operation_id, std::move(result));
}


void ImageryCatalogRepository::complete(
	OperationId operation_id,
	ImageryCatalogOperationResult result)
{
	auto operation = operations_.take(operation_id);
	if (!operation || operation->cancelled.load())
		return;
	if (result.kind == ImageryCatalogOperationKind::Installed
	    || result.kind == ImageryCatalogOperationKind::Removed
	    || result.kind == ImageryCatalogOperationKind::NotModified)
		reload();
	emit operationFinished(operation_id, result);
}

}  // namespace OpenOrienteering::imagery
