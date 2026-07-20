/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "imagery_catalog_repository_t.h"

#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#include "imagery/imagery_catalog_repository.h"

using namespace OpenOrienteering;

namespace {

QByteArray catalogBytes(
	int revision = 1,
	const QString& catalog_id =
		QStringLiteral("org.example.repository"))
{
	return QJsonDocument(QJsonObject {
		{ QStringLiteral("format"),
		  QStringLiteral("org.openorienteering.imagery-catalog") },
		{ QStringLiteral("version"), 1 },
		{ QStringLiteral("id"),
		  catalog_id },
		{ QStringLiteral("revision"), revision },
		{ QStringLiteral("name"),
		  QStringLiteral("Repository catalog") },
		{ QStringLiteral("sources"), QJsonArray {
			QJsonObject {
				{ QStringLiteral("id"),
				  QStringLiteral("aerial") },
				{ QStringLiteral("name"),
				  QStringLiteral("Example aerial") },
				{ QStringLiteral("type"),
				  QStringLiteral("raster-tiles") },
				{ QStringLiteral("tiles"), QJsonArray {
					QStringLiteral(
						"https://tiles.example.test/"
						"aerial/{z}/{x}/{y}.png"),
				} },
				{ QStringLiteral("scheme"),
				  QStringLiteral("xyz") },
				{ QStringLiteral("tileMatrixSetURI"),
				  QStringLiteral(
					  "http://www.opengis.net/def/tilematrixset/"
					  "OGC/1.0/WebMercatorQuad") },
			},
		} },
	}).toJson(QJsonDocument::Compact);
}

imagery::TileNetworkManager::Config configFor(
	const QTemporaryDir& directory,
	bool allow_private_networks = true)
{
	imagery::TileNetworkManager::Config config;
	config.cache_directory =
		directory.filePath(QStringLiteral("cache"));
	config.disk_cache_bytes = 1024 * 1024;
	config.max_active_total = 2;
	config.max_active_per_host = 2;
	config.max_active_per_client = 2;
	config.max_pending_total = 16;
	config.max_pending_per_client = 8;
	config.max_response_bytes =
		imagery::OicCatalogReader::maximum_document_size;
	config.max_retries = 0;
	config.transfer_timeout =
		std::chrono::milliseconds(500);
	config.absolute_timeout =
		std::chrono::milliseconds(2000);
	config.allow_private_networks = allow_private_networks;
	return config;
}

imagery::ImageryCatalogOperationResult operationResult(
	const QSignalSpy& spy,
	int index)
{
	return qvariant_cast<
		imagery::ImageryCatalogOperationResult>(
		spy.at(index).at(1));
}

class CatalogServer final : public QObject
{
public:
	explicit CatalogServer(QObject* parent = nullptr)
	 : QObject(parent)
	{
		connect(&server_, &QTcpServer::newConnection, this, [this] {
			while (auto* socket =
			       server_.nextPendingConnection())
			{
				connect(
					socket,
					&QTcpSocket::readyRead,
					this,
					[this, socket] {
						buffers_[socket] += socket->readAll();
						if (!buffers_[socket].contains(
							    "\r\n\r\n"))
							return;
						auto const request =
							buffers_.take(socket);
						++requests_;
						if (request.startsWith(
							    "GET /hold "))
						{
							held_ = socket;
							return;
						}
							if (request.startsWith(
								    "GET /redirect "))
						{
							respond(
								socket,
								302,
								{},
								QByteArrayLiteral("Location: ")
								+ redirect_target_.toEncoded(
									QUrl::FullyEncoded)
								+ QByteArrayLiteral("\r\n"));
								return;
							}
							if (request.startsWith(
								    "GET /no-validators "))
							{
								respond(
									socket,
									200,
									catalogBytes(),
									QByteArrayLiteral(
										"Content-Type: "
										"application/json\r\n"));
								return;
							}
							if (request.toLower().contains(
							    "if-none-match: "
							    "\"repository-one\"\r\n"))
						{
							respond(
								socket,
								304,
								{},
								QByteArrayLiteral(
									"ETag: "
									"\"repository-one\"\r\n"
									"Last-Modified: "
									"Thu, 16 Jul 2026 "
									"12:00:00 GMT\r\n"));
						}
						else
						{
							auto const body = request.startsWith(
								"GET /mismatch ")
								? catalogBytes(
									1,
									QStringLiteral(
										"org.example.different"))
								: catalogBytes();
							respond(
								socket,
								200,
								body,
								QByteArrayLiteral(
									"Content-Type: "
									"application/json\r\n"
									"ETag: "
									"\"repository-one\"\r\n"
									"Last-Modified: "
									"Thu, 16 Jul 2026 "
									"12:00:00 GMT\r\n"));
						}
					});
				connect(
					socket,
					&QTcpSocket::disconnected,
					this,
					[this, socket] {
						buffers_.remove(socket);
						socket->deleteLater();
					});
			}
		});
		QVERIFY(server_.listen(QHostAddress::LocalHost));
	}

	QUrl url(const QString& path = QStringLiteral("/catalog")) const
	{
		return QUrl(
			QStringLiteral("http://127.0.0.1:%1%2")
				.arg(server_.serverPort())
				.arg(path));
	}

	int requestCount() const noexcept
	{
		return requests_;
	}

	void setRedirectTarget(QUrl target)
	{
		redirect_target_ = std::move(target);
	}

	bool hasHeldRequest() const noexcept
	{
		return !held_.isNull();
	}

	void releaseHeld()
	{
		if (held_)
			respond(held_, 200, catalogBytes(), {});
		held_.clear();
	}

private:
	void respond(
		QTcpSocket* socket,
		int status,
		const QByteArray& body,
		const QByteArray& headers)
	{
		auto const reason = status == 304
			? QByteArrayLiteral("Not Modified")
			: status == 302
				? QByteArrayLiteral("Found")
				: QByteArrayLiteral("OK");
		socket->write(
			QByteArrayLiteral("HTTP/1.1 ")
			+ QByteArray::number(status)
			+ ' ' + reason
			+ QByteArrayLiteral("\r\nContent-Length: ")
			+ QByteArray::number(body.size())
			+ QByteArrayLiteral(
				"\r\nConnection: close\r\n")
			+ headers
			+ QByteArrayLiteral("\r\n")
			+ body);
		socket->disconnectFromHost();
	}

	QTcpServer server_;
	QHash<QTcpSocket*, QByteArray> buffers_;
	QPointer<QTcpSocket> held_;
	QUrl redirect_target_;
	int requests_ = 0;
};

}  // namespace


void ImageryCatalogRepositoryTest::
	importsPublishesAndRemovesStableSources()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	QSignalSpy operations(
		&repository,
		&imagery::ImageryCatalogRepository::operationFinished);
	QSignalSpy snapshots(
		&repository,
		&imagery::ImageryCatalogRepository::snapshotChanged);
	repository.reload();
	QTRY_VERIFY_WITH_TIMEOUT(!snapshots.isEmpty(), 3000);

	auto const path =
		directory.filePath(QStringLiteral("catalog.oic"));
	QFile file(path);
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write(catalogBytes()), catalogBytes().size());
	file.close();

	auto const read_id = repository.readCatalogFile(path);
	QTRY_VERIFY_WITH_TIMEOUT(!operations.isEmpty(), 3000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		read_id);
	auto const candidate_result =
		operationResult(operations, operations.size() - 1);
	QCOMPARE(
		candidate_result.kind,
		imagery::ImageryCatalogOperationKind::CandidateReady);
	QVERIFY(candidate_result.candidate);
	QVERIFY(candidate_result.candidate->read_result.accepted());
	QCOMPARE(candidate_result.candidate->analysis.added, 1);

	auto const install_id = repository.installCandidate(
		candidate_result.candidate);
	QTRY_VERIFY_WITH_TIMEOUT(operations.size() >= 2, 3000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		install_id);
	QCOMPARE(
		operationResult(operations, operations.size() - 1).kind,
		imagery::ImageryCatalogOperationKind::Installed);
	QTRY_COMPARE_WITH_TIMEOUT(
		repository.snapshot()->catalogs.size(),
		1,
		3000);

	auto const handle =
		repository.snapshot()->latestHandle(
			QStringLiteral("org.example.repository"),
			QStringLiteral("aerial"));
	QVERIFY(handle);
	QVERIFY(handle->isValid());
	auto const* definition =
		repository.snapshot()->source(*handle);
	QVERIFY(definition);
	QVERIFY(definition->resolved_source.has_value());

	auto const remove_id = repository.removeCatalog(
		QStringLiteral("org.example.repository"));
	QTRY_VERIFY_WITH_TIMEOUT(operations.size() >= 3, 3000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		remove_id);
	QCOMPARE(
		operationResult(operations, operations.size() - 1).kind,
		imagery::ImageryCatalogOperationKind::Removed);
	QTRY_COMPARE_WITH_TIMEOUT(
		repository.snapshot()->catalogs.size(),
		0,
		3000);
	QVERIFY(!repository.snapshot()->source(*handle));
}


void ImageryCatalogRepositoryTest::
	fetchesConditionallyAndMarksChecks()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	CatalogServer server;
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	QSignalSpy operations(
		&repository,
		&imagery::ImageryCatalogRepository::operationFinished);

	imagery::ImageryCatalogFetchRequest request;
	request.url = server.url();
	request.allow_insecure_http = true;
	auto const fetch_id = repository.fetchCatalog(request);
	QTRY_VERIFY_WITH_TIMEOUT(!operations.isEmpty(), 4000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		fetch_id);
	auto candidate_result =
		operationResult(operations, operations.size() - 1);
	QCOMPARE(
		candidate_result.kind,
		imagery::ImageryCatalogOperationKind::CandidateReady);
	QCOMPARE(
		candidate_result.candidate->metadata.etag,
		QByteArray("\"repository-one\""));

	auto const install_id = repository.installCandidate(
		candidate_result.candidate);
	QTRY_VERIFY_WITH_TIMEOUT(operations.size() >= 2, 3000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		install_id);
	QTRY_COMPARE_WITH_TIMEOUT(
		repository.snapshot()->catalogs.size(),
		1,
		3000);
	auto const checked_before =
		repository.snapshot()
			->catalogs.first().state.checked_at;

	request.etag =
		candidate_result.candidate->metadata.etag;
	request.last_modified =
		candidate_result.candidate->metadata.last_modified;
	request.installed_catalog_id =
		QStringLiteral("org.example.repository");
	auto const update_id = repository.fetchCatalog(request);
	QTRY_VERIFY_WITH_TIMEOUT(operations.size() >= 3, 4000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		update_id);
	auto const update_result =
		operationResult(operations, operations.size() - 1);
	QCOMPARE(
		update_result.kind,
		imagery::ImageryCatalogOperationKind::NotModified);
	QCOMPARE(update_result.catalog_id, request.installed_catalog_id);
	QTRY_VERIFY_WITH_TIMEOUT(
		repository.snapshot()
			->catalogs.first().state.checked_at
			>= checked_before,
		3000);
	QCOMPARE(server.requestCount(), 2);

	request.url = server.url(
		QStringLiteral("/no-validators"));
	auto const unvalidated_id =
		repository.fetchCatalog(request);
	QTRY_VERIFY_WITH_TIMEOUT(
		operations.size() >= 4, 4000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		unvalidated_id);
	auto const unvalidated_result =
		operationResult(
			operations,
			operations.size() - 1);
	QCOMPARE(
		unvalidated_result.kind,
		imagery::ImageryCatalogOperationKind::
			CandidateReady);
	QVERIFY(
		unvalidated_result.candidate
			->metadata.etag.isEmpty());
	QVERIFY(
		unvalidated_result.candidate
			->metadata.last_modified.isEmpty());
	repository.installCandidate(
		unvalidated_result.candidate);
	QTRY_VERIFY_WITH_TIMEOUT(
		operations.size() >= 5, 3000);
	QTRY_VERIFY_WITH_TIMEOUT(
		repository.snapshot()
			->catalogs.first().state.etag.isEmpty(),
		3000);
	QVERIFY(
		repository.snapshot()
			->catalogs.first()
			.state.last_modified.isEmpty());
}


void ImageryCatalogRepositoryTest::
	rejectsMismatchedCatalogUpdates()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	CatalogServer server;
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	QSignalSpy operations(
		&repository,
		&imagery::ImageryCatalogRepository::operationFinished);

	imagery::ImageryCatalogFetchRequest request;
	request.url = server.url(QStringLiteral("/mismatch"));
	request.installed_catalog_id =
		QStringLiteral("org.example.repository");
	request.allow_insecure_http = true;
	auto const update_id = repository.fetchCatalog(request);
	QTRY_VERIFY_WITH_TIMEOUT(!operations.isEmpty(), 4000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		update_id);
	auto const result =
		operationResult(operations, operations.size() - 1);
	QCOMPARE(
		result.kind,
		imagery::ImageryCatalogOperationKind::Failed);
	QVERIFY(result.error.contains(QStringLiteral("ID")));
	QVERIFY(!result.candidate);
}


void ImageryCatalogRepositoryTest::
	offersPrivateNetworkApprovalAndRetries()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	CatalogServer server;
	imagery::TileNetworkManager network(
		configFor(directory, false));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	QSignalSpy operations(
		&repository,
		&imagery::ImageryCatalogRepository::operationFinished);

	imagery::ImageryCatalogFetchRequest request;
	request.url = server.url();
	request.allow_insecure_http = true;
	auto const rejected_id = repository.fetchCatalog(request);
	QTRY_VERIFY_WITH_TIMEOUT(!operations.isEmpty(), 3000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		rejected_id);
	auto result =
		operationResult(operations, operations.size() - 1);
	QCOMPARE(
		result.kind,
		imagery::ImageryCatalogOperationKind::Failed);
	QCOMPARE(
		result.private_network_approval_url,
		request.url);
	QCOMPARE(server.requestCount(), 0);

	QVERIFY(network.approvePrivateOrigin(request.url));
	auto const retry_id = repository.fetchCatalog(request);
	QTRY_VERIFY_WITH_TIMEOUT(operations.size() >= 2, 4000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		retry_id);
	result = operationResult(operations, operations.size() - 1);
	QCOMPARE(
		result.kind,
		imagery::ImageryCatalogOperationKind::CandidateReady);
	QVERIFY(result.candidate);
	QVERIFY(result.candidate->read_result.accepted());
	QCOMPARE(server.requestCount(), 1);
}


void ImageryCatalogRepositoryTest::
	offersApprovalForExactPrivateRedirectTarget()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	CatalogServer entry_server;
	CatalogServer private_target;
	auto const target_url = private_target.url();
	entry_server.setRedirectTarget(target_url);
	imagery::TileNetworkManager network(
		configFor(directory, false));
	QVERIFY(network.approvePrivateOrigin(entry_server.url()));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	QSignalSpy operations(
		&repository,
		&imagery::ImageryCatalogRepository::operationFinished);

	imagery::ImageryCatalogFetchRequest request;
	request.url = entry_server.url(QStringLiteral("/redirect"));
	request.allow_insecure_http = true;
	repository.fetchCatalog(request);

	QTRY_VERIFY_WITH_TIMEOUT(!operations.isEmpty(), 3000);
	auto const result =
		operationResult(operations, operations.size() - 1);
	QCOMPARE(
		result.kind,
		imagery::ImageryCatalogOperationKind::Failed);
	QCOMPARE(result.private_network_approval_url, target_url);
	QCOMPARE(private_target.requestCount(), 0);
}


void ImageryCatalogRepositoryTest::
	requiresHttpConsentAndCancels()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	CatalogServer server;
	imagery::TileNetworkManager network(configFor(directory));
	imagery::ImageryCatalogRepository repository(
		directory.filePath(QStringLiteral("catalogs")),
		&network);
	QSignalSpy operations(
		&repository,
		&imagery::ImageryCatalogRepository::operationFinished);

	imagery::ImageryCatalogFetchRequest request;
	request.url = server.url();
	auto const rejected_id =
		repository.fetchCatalog(request);
	QTRY_COMPARE_WITH_TIMEOUT(operations.size(), 1, 2000);
	QCOMPARE(
		operations.first().at(0).toULongLong(),
		rejected_id);
	QCOMPARE(
		operationResult(operations, 0).kind,
		imagery::ImageryCatalogOperationKind::Failed);
	QCOMPARE(server.requestCount(), 0);

	request.url = server.url(QStringLiteral("/hold"));
	request.allow_insecure_http = true;
	auto const cancelled_id =
		repository.fetchCatalog(request);
	QTRY_VERIFY_WITH_TIMEOUT(server.hasHeldRequest(), 2000);
	repository.cancel(cancelled_id);
	QTRY_COMPARE_WITH_TIMEOUT(operations.size(), 2, 2000);
	QCOMPARE(
		operations.last().at(0).toULongLong(),
		cancelled_id);
	QCOMPARE(
		operationResult(operations, 1).kind,
		imagery::ImageryCatalogOperationKind::Cancelled);
	server.releaseHeld();
	QTest::qWait(50);
	QCOMPARE(operations.size(), 2);
}


QTEST_GUILESS_MAIN(ImageryCatalogRepositoryTest)
