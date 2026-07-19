/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "tile_network_manager_t.h"

#include <memory>

#include <QtTest>
#include <QDateTime>
#include <QHash>
#include <QIODevice>
#include <QNetworkCacheMetaData>
#include <QNetworkDiskCache>
#include <QNetworkRequest>
#include <QPointer>
#include <QSignalSpy>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QThread>
#include <QUrl>
#include <QVariant>

#include "imagery/tile_network_manager.h"

using OpenOrienteering::imagery::TileNetworkManager;
using OpenOrienteering::imagery::TileNetworkRequest;
using OpenOrienteering::imagery::TileNetworkResult;
using OpenOrienteering::imagery::TileRequestPriority;
using OpenOrienteering::imagery::NetworkPayloadKind;

namespace {

class MiniHttpServer final : public QObject
{
public:
	explicit MiniHttpServer(QObject* parent = nullptr)
	 : QObject(parent)
	{
		connect(&server_, &QTcpServer::newConnection, this, [this] {
			while (auto* socket = server_.nextPendingConnection())
			{
				connect(socket, &QTcpSocket::readyRead, this, [this, socket] {
					buffers_[socket] += socket->readAll();
					if (!buffers_[socket].contains("\r\n\r\n"))
						return;
					auto const request = buffers_.take(socket);
					auto const first_line = request.left(request.indexOf("\r\n"));
					auto const parts = first_line.split(' ');
					if (parts.size() < 2)
					{
						respond(socket, 400, "Bad Request", {});
						return;
					}
					auto const path = QString::fromUtf8(parts.at(1));
					paths_.push_back(path);
					requests_.push_back(request);
					handle(socket, path, request);
				});
				connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
					buffers_.remove(socket);
					socket->deleteLater();
				});
			}
		});
		QVERIFY(server_.listen(QHostAddress::LocalHost));
	}

	QUrl url(const QString& path) const
	{
		return QUrl(
			QStringLiteral("http://127.0.0.1:%1%2")
				.arg(server_.serverPort())
				.arg(path));
	}

	QStringList paths() const
	{
		return paths_;
	}

	int retryRequests() const
	{
		return retry_requests_;
	}

	QByteArray lastRequest() const
	{
		return requests_.isEmpty() ? QByteArray {} : requests_.last();
	}

	void setRedirectTarget(QUrl target)
	{
		redirect_target_ = std::move(target);
	}

	int heldCount() const
	{
		return int(std::ranges::count_if(
			held_, [](auto const& socket) { return !socket.isNull(); }));
	}

	void releaseOne()
	{
		for (auto& socket : held_)
		{
			if (!socket)
				continue;
			respond(socket, 200, "OK", QByteArrayLiteral("held"));
			socket.clear();
			return;
		}
	}

	void releaseAll()
	{
		while (heldCount() > 0)
			releaseOne();
	}

private:
	static QByteArray reason(int status)
	{
		switch (status)
		{
		case 200: return QByteArrayLiteral("OK");
		case 302: return QByteArrayLiteral("Found");
		case 304: return QByteArrayLiteral("Not Modified");
		case 401: return QByteArrayLiteral("Unauthorized");
		case 404: return QByteArrayLiteral("Not Found");
		case 503: return QByteArrayLiteral("Service Unavailable");
		default: return QByteArrayLiteral("Error");
		}
	}

	void respond(
		QTcpSocket* socket,
		int status,
		QByteArray status_text,
		QByteArray body,
		QByteArray extra_headers = {})
	{
		if (!socket)
			return;
		if (status_text.isEmpty())
			status_text = reason(status);
		QByteArray response = QByteArrayLiteral("HTTP/1.1 ")
		                    + QByteArray::number(status) + ' ' + status_text
		                    + QByteArrayLiteral("\r\nContent-Length: ")
		                    + QByteArray::number(body.size())
		                    + QByteArrayLiteral("\r\nConnection: close\r\n")
		                    + extra_headers
		                    + QByteArrayLiteral("\r\n")
		                    + body;
		socket->write(response);
		socket->disconnectFromHost();
	}

	void handle(
		QTcpSocket* socket,
		const QString& path,
		const QByteArray& request)
	{
		if (path == QLatin1String("/ok"))
		{
			respond(
				socket, 200, {}, QByteArrayLiteral("tile"),
				QByteArrayLiteral("Content-Type: image/png\r\n"));
		}
		else if (path == QLatin1String("/auth"))
		{
			if (request.contains("Authorization: Bearer map-token\r\n"))
				respond(socket, 200, {}, QByteArrayLiteral("authorized"));
			else
				respond(socket, 401, {}, {});
		}
		else if (path.startsWith(QLatin1String("/empty")))
		{
			respond(socket, 404, {}, {});
		}
		else if (path == QLatin1String("/large"))
		{
			respond(socket, 200, {}, QByteArray(256, 'x'));
		}
		else if (path == QLatin1String("/redirect"))
		{
			respond(
				socket, 302, {}, {},
				QByteArrayLiteral("Location: /ok\r\n"));
		}
		else if (path == QLatin1String("/private-redirect"))
		{
			respond(
				socket, 302, {}, {},
				QByteArrayLiteral("Location: ")
				+ redirect_target_.toEncoded(QUrl::FullyEncoded)
				+ QByteArrayLiteral("\r\n"));
		}
		else if (path == QLatin1String("/retry"))
		{
			++retry_requests_;
			if (retry_requests_ == 1)
			{
				respond(
					socket, 503, {}, {},
					QByteArrayLiteral("Retry-After: 0\r\n"));
			}
			else
			{
				respond(socket, 200, {}, QByteArrayLiteral("retry-ok"));
			}
		}
		else if (path == QLatin1String("/cache"))
		{
			respond(
				socket, 200, {}, QByteArrayLiteral("cached"),
				QByteArrayLiteral(
					"Content-Type: image/png\r\n"
					"Cache-Control: public, max-age=3600\r\n"));
		}
		else if (path == QLatin1String("/catalog"))
		{
			if (request.toLower().contains(
			    "if-none-match: \"catalog-one\"\r\n"))
			{
				respond(
					socket, 304, {}, {},
					QByteArrayLiteral(
						"ETag: \"catalog-one\"\r\n"
						"Last-Modified: Wed, 15 Jul 2026 12:00:00 GMT\r\n"));
			}
			else
			{
				respond(
					socket, 200, {}, QByteArrayLiteral("{\"version\":1}"),
					QByteArrayLiteral(
						"Content-Type: application/json\r\n"
						"ETag: \"catalog-one\"\r\n"
						"Last-Modified: Wed, 15 Jul 2026 12:00:00 GMT\r\n"));
			}
		}
		else if (path == QLatin1String("/slow"))
		{
			held_.push_back(socket);
		}
		else if (path.startsWith(QLatin1String("/hold/")))
		{
			held_.push_back(socket);
		}
		else if (path.startsWith(QLatin1String("/order/")))
		{
			respond(socket, 200, {}, path.toUtf8());
		}
		else
		{
			respond(socket, 404, {}, {});
		}
	}

	QTcpServer server_;
	QHash<QTcpSocket*, QByteArray> buffers_;
	QVector<QPointer<QTcpSocket>> held_;
	QStringList paths_;
	QVector<QByteArray> requests_;
	QUrl redirect_target_;
	int retry_requests_ = 0;
};

TileNetworkManager::Config configFor(
	const QTemporaryDir& directory,
	qint64 max_response_bytes = 1024)
{
	TileNetworkManager::Config config;
	config.cache_directory = directory.filePath(QStringLiteral("cache"));
	config.disk_cache_bytes = 1024 * 1024;
	config.max_active_total = 4;
	config.max_active_per_host = 4;
	config.max_active_per_client = 4;
	config.max_pending_total = 32;
	config.max_pending_per_client = 16;
	config.max_response_bytes = max_response_bytes;
	config.max_retries = 0;
	config.retry_base_delay_ms = 5;
	config.retry_max_delay_ms = 50;
	config.transfer_timeout = std::chrono::milliseconds(250);
	config.absolute_timeout = std::chrono::milliseconds(1000);
	config.allow_private_networks = true;
	return config;
}

TileNetworkRequest request(
	QUrl url,
	quint64 client,
	quint64 generation = 1,
	quint64 user_data = 0)
{
	TileNetworkRequest result;
	result.url = std::move(url);
	result.client_id = client;
	result.generation = generation;
	result.user_data = user_data;
	return result;
}

TileNetworkResult resultAt(const QSignalSpy& spy, int index)
{
	return qvariant_cast<TileNetworkResult>(spy.at(index).at(1));
}

}  // namespace

void TileNetworkManagerTest::rejectsUnsafeUrls()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.allow_private_networks = false;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(QUrl(QStringLiteral("file:///tmp/tile.png")), 1));
	manager.submit(request(QUrl(QStringLiteral("http://127.0.0.1/tile.png")), 1));
	manager.submit(request(
		QUrl(QStringLiteral("https://user:secret@example.test/tile.png")), 1));
	for (auto const& value : {
		QStringLiteral("http://0.0.0.0/tile.png"),
		QStringLiteral("http://10.0.0.1/tile.png"),
			QStringLiteral("http://100.64.0.1/tile.png"),
			QStringLiteral("http://192.0.2.1/tile.png"),
			QStringLiteral("http://192.31.196.1/tile.png"),
			QStringLiteral("http://192.52.193.1/tile.png"),
			QStringLiteral("http://192.175.48.1/tile.png"),
			QStringLiteral("http://198.18.0.1/tile.png"),
		QStringLiteral("http://[::]/tile.png"),
			QStringLiteral("http://[fc00::1]/tile.png"),
			QStringLiteral("http://[2001:db8::1]/tile.png"),
			QStringLiteral("http://[2620:4f:8000::1]/tile.png"),
		}) {
		manager.submit(request(QUrl(value), 1));
	}

	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 15, 2000);
	for (int index = 0; index < spy.size(); ++index)
	{
		QCOMPARE(resultAt(spy, index).outcome, TileNetworkResult::Outcome::Rejected);
		if (index > 0 && index != 2)
			QVERIFY(resultAt(spy, index).private_network_rejected);
	}
}

void TileNetworkManagerTest::canonicalizesIpv6OriginsExactly()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.allow_private_networks = false;
	TileNetworkManager manager(config);
	auto const first =
		QUrl(QStringLiteral("http://[fd00::1]:8080/tiles/0/0/0"));
	auto const second =
		QUrl(QStringLiteral("http://[fd00::2]:8080/tiles/0/0/0"));

	QCOMPARE(
		TileNetworkManager::canonicalOrigin(first),
		QStringLiteral("http://[fd00::1]:8080"));
	QCOMPARE(
		TileNetworkManager::canonicalOrigin(second),
		QStringLiteral("http://[fd00::2]:8080"));
	QVERIFY(manager.approvePrivateOrigin(first));
	QVERIFY(manager.isPrivateOriginApproved(first));
	QVERIFY(!manager.isPrivateOriginApproved(second));
	QVERIFY(
		TileNetworkManager::isPublicDestinationAddress(
			QHostAddress(QStringLiteral("8.8.8.8"))));
	QVERIFY(
		TileNetworkManager::isPublicDestinationAddress(
			QHostAddress(QStringLiteral(
				"2606:4700:4700::1111"))));
	QVERIFY(
		TileNetworkManager::isPublicDestinationAddress(
			QHostAddress(QStringLiteral(
				"64:ff9b::808:808"))));
	QVERIFY(
		!TileNetworkManager::isPublicDestinationAddress(
			QHostAddress(QStringLiteral(
				"64:ff9b::a00:1"))));
	QVERIFY(
		!TileNetworkManager::isPublicDestinationAddress(
			QHostAddress(QStringLiteral(
				"::ffff:10.0.0.1"))));
}

void TileNetworkManagerTest::approvesPrivateOriginsExplicitly()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.allow_private_networks = false;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(server.url(QStringLiteral("/ok")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	QCOMPARE(
		resultAt(spy, 0).outcome,
		TileNetworkResult::Outcome::Rejected);

	QVERIFY(manager.approvePrivateOrigin(
		server.url(QStringLiteral("/ignored"))));
	QVERIFY(manager.isPrivateOriginApproved(
		server.url(QStringLiteral("/elsewhere"))));
	auto request_with_private_referer =
		request(server.url(QStringLiteral("/ok")), 1);
	request_with_private_referer.referer =
		QStringLiteral("http://127.0.0.2/private-context");
	manager.submit(std::move(request_with_private_referer));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 2000);
	QCOMPARE(
		resultAt(spy, 1).outcome,
		TileNetworkResult::Outcome::Success);
	QVERIFY(server.lastRequest().contains(
		"Referer: http://127.0.0.2/private-context\r\n"));

	QVERIFY(manager.revokePrivateOrigin(
		server.url(QStringLiteral("/"))));
	QVERIFY(!manager.isPrivateOriginApproved(
		server.url(QStringLiteral("/"))));
	manager.submit(request(server.url(QStringLiteral("/ok")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 3, 2000);
	QCOMPARE(
		resultAt(spy, 2).outcome,
		TileNetworkResult::Outcome::Rejected);
}

void TileNetworkManagerTest::injectsBearerOnlyForExactOriginAndBypassesSharedCache()
{
	MiniHttpServer server;
	MiniHttpServer redirect_target;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	TileNetworkManager manager(configFor(directory));
	QSignalSpy spy(&manager, &TileNetworkManager::finished);
	QVERIFY(manager.setBearerCredential(
		server.url(QStringLiteral("/")),
		QByteArrayLiteral("map-token"),
		QByteArrayLiteral("account-one")));

	manager.submit(request(server.url(QStringLiteral("/auth")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	QCOMPARE(resultAt(spy, 0).outcome, TileNetworkResult::Outcome::Success);
	QVERIFY(server.lastRequest().contains("Authorization: Bearer map-token\r\n"));

	manager.submit(request(server.url(QStringLiteral("/cache")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 2000);
	manager.submit(request(server.url(QStringLiteral("/cache")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 3, 2000);
	QCOMPARE(server.paths().count(QStringLiteral("/cache")), 2);

	server.setRedirectTarget(redirect_target.url(QStringLiteral("/ok")));
	manager.submit(request(server.url(QStringLiteral("/private-redirect")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 4, 3000);
	QCOMPARE(resultAt(spy, 3).outcome, TileNetworkResult::Outcome::Success);
	QVERIFY(!redirect_target.lastRequest().contains("Authorization:"));

	manager.clearBearerCredential(server.url(QStringLiteral("/")));
	manager.submit(request(server.url(QStringLiteral("/auth")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 5, 2000);
	QCOMPARE(resultAt(spy, 4).outcome, TileNetworkResult::Outcome::PermanentError);
}

void TileNetworkManagerTest::credentialChangesCancelRequestsAndPartitionNegativeCache()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.max_active_total = 1;
	TileNetworkManager manager(config);
	QSignalSpy results(&manager, &TileNetworkManager::finished);
	QSignalSpy changes(&manager, &TileNetworkManager::bearerCredentialChanged);
	auto const origin = server.url(QStringLiteral("/"));
	QVERIFY(manager.setBearerCredential(
		origin, QByteArrayLiteral("account-a-token"),
		QByteArrayLiteral("stable-server-account")));
	QCOMPARE(changes.size(), 1);

	manager.submit(request(server.url(QStringLiteral("/empty-credential")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(results.size(), 1, 2000);
	manager.submit(request(server.url(QStringLiteral("/empty-credential")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(results.size(), 2, 2000);
	QCOMPARE(server.paths().count(QStringLiteral("/empty-credential")), 1);
	QVERIFY(resultAt(results, 1).from_cache);

	manager.submit(request(server.url(QStringLiteral("/hold/credential")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);
	QVERIFY(manager.setBearerCredential(
		origin, QByteArrayLiteral("account-b-token"),
		QByteArrayLiteral("stable-server-account")));
	QCOMPARE(changes.size(), 2);
	QTRY_COMPARE_WITH_TIMEOUT(results.size(), 3, 2000);
	QCOMPARE(resultAt(results, 2).outcome, TileNetworkResult::Outcome::Cancelled);
	QVERIFY(resultAt(results, 2).body.isEmpty());

	manager.submit(request(server.url(QStringLiteral("/empty-credential")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(results.size(), 4, 2000);
	QCOMPARE(server.paths().count(QStringLiteral("/empty-credential")), 2);
	server.releaseAll();
}

void TileNetworkManagerTest::handlesRedirectsEmptyTilesAndBodyLimits()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	TileNetworkManager manager(configFor(directory, 32));
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(server.url(QStringLiteral("/redirect")), 1, 1, 10));
	manager.submit(request(server.url(QStringLiteral("/empty")), 1, 1, 20));
	manager.submit(request(server.url(QStringLiteral("/large")), 1, 1, 30));

	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 3, 3000);
	QHash<quint64, TileNetworkResult> by_user_data;
	for (int index = 0; index < spy.size(); ++index)
		by_user_data.insert(resultAt(spy, index).user_data, resultAt(spy, index));
	QCOMPARE(by_user_data[10].outcome, TileNetworkResult::Outcome::Success);
	QCOMPARE(by_user_data[10].body, QByteArray("tile"));
	QCOMPARE(by_user_data[20].outcome, TileNetworkResult::Outcome::EmptyTile);
	QCOMPARE(by_user_data[20].http_status, 404);
	QCOMPARE(by_user_data[30].outcome, TileNetworkResult::Outcome::PermanentError);
	QVERIFY(by_user_data[30].error_string.contains(QStringLiteral("safety")));
}

void TileNetworkManagerTest::reportsExactPrivateRedirectTarget()
{
	MiniHttpServer entry_server;
	MiniHttpServer private_target;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.allow_private_networks = false;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	auto const target_url =
		private_target.url(QStringLiteral("/ok"));
	entry_server.setRedirectTarget(target_url);
	QVERIFY(manager.approvePrivateOrigin(
		entry_server.url(QStringLiteral("/"))));
	manager.submit(request(
		entry_server.url(QStringLiteral("/private-redirect")),
		1));

	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 3000);
	auto const result = resultAt(spy, 0);
	QCOMPARE(result.outcome, TileNetworkResult::Outcome::Rejected);
	QVERIFY(result.private_network_rejected);
	QCOMPARE(result.private_network_rejected_url, target_url);
	QCOMPARE(private_target.paths().size(), 0);
}

void TileNetworkManagerTest::boundsAndExpiresNegativeCache()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.max_negative_cache_entries = 2;
	config.negative_cache_ttl_ms = 10'000;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);
	auto expected_results = 0;
	auto submit_empty = [&](const QString& suffix) {
		manager.submit(request(
			server.url(QStringLiteral("/empty/") + suffix), 1));
		++expected_results;
		QTRY_COMPARE_WITH_TIMEOUT(
			spy.size(), expected_results, 2000);
		QCOMPARE(
			resultAt(spy, expected_results - 1).outcome,
			TileNetworkResult::Outcome::EmptyTile);
	};

	submit_empty(QStringLiteral("a"));
	submit_empty(QStringLiteral("b"));
	submit_empty(QStringLiteral("a"));  // Refresh a as most recently used.
	QCOMPARE(
		server.paths().count(QStringLiteral("/empty/a")), 1);
	submit_empty(QStringLiteral("c"));  // Evicts b, the least recently used.
	submit_empty(QStringLiteral("b"));
	QCOMPARE(
		server.paths().count(QStringLiteral("/empty/b")), 2);

	QTemporaryDir expiry_directory;
	QVERIFY(expiry_directory.isValid());
	auto expiry_config = configFor(expiry_directory);
	expiry_config.negative_cache_ttl_ms = 10;
	TileNetworkManager expiry_manager(expiry_config);
	QSignalSpy expiry_spy(
		&expiry_manager, &TileNetworkManager::finished);
	auto const expiry_url =
		server.url(QStringLiteral("/empty/expiry"));
	expiry_manager.submit(request(expiry_url, 2));
	QTRY_COMPARE_WITH_TIMEOUT(expiry_spy.size(), 1, 2000);
	QTest::qWait(20);
	expiry_manager.submit(request(expiry_url, 2));
	QTRY_COMPARE_WITH_TIMEOUT(expiry_spy.size(), 2, 2000);
	QCOMPARE(
		server.paths().count(QStringLiteral("/empty/expiry")), 2);
}

void TileNetworkManagerTest::negativeCacheRespectsRepresentationPolicy()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	TileNetworkManager manager(configFor(directory));
	QSignalSpy spy(&manager, &TileNetworkManager::finished);
	auto const url = server.url(QStringLiteral("/empty/policy"));

	manager.submit(request(url, 1, 1, 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	QCOMPARE(
		resultAt(spy, 0).outcome,
		TileNetworkResult::Outcome::EmptyTile);

	auto strict = request(url, 1, 1, 2);
	strict.empty_http_status_codes.clear();
	manager.submit(std::move(strict));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 2000);
	QCOMPARE(
		resultAt(spy, 1).outcome,
		TileNetworkResult::Outcome::PermanentError);
	QCOMPARE(
		server.paths().count(QStringLiteral("/empty/policy")),
		2);

	auto first_referer = request(url, 1, 1, 3);
	first_referer.referer =
		QStringLiteral("https://first.example.test/map");
	manager.submit(std::move(first_referer));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 3, 2000);
	QCOMPARE(
		resultAt(spy, 2).outcome,
		TileNetworkResult::Outcome::EmptyTile);

	auto second_referer = request(url, 1, 1, 4);
	second_referer.referer =
		QStringLiteral("https://second.example.test/map");
	manager.submit(std::move(second_referer));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 4, 2000);
	QCOMPARE(
		resultAt(spy, 3).outcome,
		TileNetworkResult::Outcome::EmptyTile);
	QCOMPARE(
		server.paths().count(QStringLiteral("/empty/policy")),
		4);
}

void TileNetworkManagerTest::
	digestsLongNegativeCacheRepresentationsWithoutCollision()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.max_negative_cache_entries = 2;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	auto first_url = server.url(QStringLiteral("/empty/digest"));
	first_url.setQuery(
		QStringLiteral("token=")
		+ QString(7000, QLatin1Char('a'))
		+ QStringLiteral("first"));
	auto second_url = server.url(QStringLiteral("/empty/digest"));
	second_url.setQuery(
		QStringLiteral("token=")
		+ QString(7000, QLatin1Char('a'))
		+ QStringLiteral("second"));
	auto const referer =
		QStringLiteral("https://viewer.example.test/map?context=")
		+ QString(7000, QLatin1Char('b'));

	auto submit = [&](const QUrl& url, quint64 user_data) {
		auto tile_request = request(url, 1, 1, user_data);
		tile_request.referer = referer;
		manager.submit(std::move(tile_request));
		QTRY_COMPARE_WITH_TIMEOUT(
			spy.size(), int(user_data), 3000);
		QCOMPARE(
			resultAt(spy, int(user_data) - 1).outcome,
			TileNetworkResult::Outcome::EmptyTile);
	};

	submit(first_url, 1);
	submit(second_url, 2);
	submit(first_url, 3);
	submit(second_url, 4);
	QCOMPARE(server.paths().size(), 2);
	QVERIFY(resultAt(spy, 2).from_cache);
	QVERIFY(resultAt(spy, 3).from_cache);
}

void TileNetworkManagerTest::fetchesCatalogsConditionally()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	TileNetworkManager manager(configFor(directory, 1024));
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	auto first = request(
		server.url(QStringLiteral("/catalog")),
		1,
		1,
		40);
	first.payload_kind = NetworkPayloadKind::JsonDocument;
	first.empty_http_status_codes.clear();
	first.max_response_bytes = 512;
	manager.submit(first);
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 3000);
	auto const first_result = resultAt(spy, 0);
	QCOMPARE(first_result.outcome, TileNetworkResult::Outcome::Success);
	QCOMPARE(first_result.body, QByteArray("{\"version\":1}"));
	QCOMPARE(first_result.content_type, QStringLiteral("application/json"));
	QCOMPARE(first_result.etag, QByteArray("\"catalog-one\""));
	QCOMPARE(
		first_result.last_modified,
		QByteArray("Wed, 15 Jul 2026 12:00:00 GMT"));
	QCOMPARE(first_result.final_url, server.url(QStringLiteral("/catalog")));
	QVERIFY(server.lastRequest().toLower().contains(
		"accept: application/json, application/*+json;q=0.9, "
		"application/octet-stream;q=0.5\r\n"));

	auto second = request(
		server.url(QStringLiteral("/catalog")),
		1,
		2,
		41);
	second.payload_kind = NetworkPayloadKind::JsonDocument;
	second.empty_http_status_codes.clear();
	second.if_none_match = first_result.etag;
	second.if_modified_since = first_result.last_modified;
	second.max_response_bytes = 512;
	manager.submit(second);
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 3000);
	auto const second_result = resultAt(spy, 1);
	QCOMPARE(
		second_result.outcome,
		TileNetworkResult::Outcome::NotModified);
	QCOMPARE(second_result.http_status, 304);
	QCOMPARE(second_result.etag, QByteArray("\"catalog-one\""));
	QVERIFY(server.lastRequest().toLower().contains(
		"if-none-match: \"catalog-one\"\r\n"));
	QVERIFY(server.lastRequest().toLower().contains(
		"if-modified-since: wed, 15 jul 2026 12:00:00 gmt\r\n"));
}

void TileNetworkManagerTest::retriesTransientFailures()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.max_retries = 1;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(server.url(QStringLiteral("/retry")), 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 3000);
	QCOMPARE(resultAt(spy, 0).outcome, TileNetworkResult::Outcome::Success);
	QCOMPARE(resultAt(spy, 0).body, QByteArray("retry-ok"));
	QCOMPARE(server.retryRequests(), 2);
}

void TileNetworkManagerTest::cancelsClientGenerations()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	// Cancellation is the behavior under test. Avoid racing Qt's internal
	// transfer-timeout abort on a loaded runner as cancelClient() is delivered.
	config.transfer_timeout = std::chrono::seconds(10);
	config.absolute_timeout = std::chrono::seconds(10);
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(server.url(QStringLiteral("/hold/old")), 42, 3));
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);
	manager.cancelClient(42, 3);
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	QCOMPARE(resultAt(spy, 0).outcome, TileNetworkResult::Outcome::Cancelled);
	QCOMPARE(resultAt(spy, 0).client_id, quint64(42));
	QCOMPARE(resultAt(spy, 0).generation, quint64(3));

	manager.submit(request(server.url(QStringLiteral("/ok")), 42, 4));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 2000);
	QCOMPARE(resultAt(spy, 1).outcome, TileNetworkResult::Outcome::Success);
}

void TileNetworkManagerTest::enteringOfflineModeAbortsActiveRequests()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	TileNetworkManager manager(configFor(directory));
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(
		server.url(QStringLiteral("/hold/offline")), 1, 1, 71));
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);
	manager.setOfflineMode(true);

	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	auto const result = resultAt(spy, 0);
	QCOMPARE(result.user_data, quint64(71));
	QCOMPARE(result.outcome, TileNetworkResult::Outcome::OfflineMiss);
	QVERIFY(result.error_string.contains(QStringLiteral("offline")));
	QCOMPARE(
		server.paths().count(QStringLiteral("/hold/offline")),
		1);
	server.releaseAll();
}

void TileNetworkManagerTest::offlineTransitionRejectsQueuedOnlineSuccess()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	TileNetworkManager manager(configFor(directory));
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(
		server.url(QStringLiteral("/hold/offline-delivery")),
		1, 1, 72));
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);
	server.releaseOne();
	// Let the network thread finish while this facade thread deliberately does
	// not deliver its already-queued completion.
	QThread::msleep(100);
	QCOMPARE(spy.size(), 0);
	manager.setOfflineMode(true);

	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	auto const result = resultAt(spy, 0);
	QCOMPARE(result.user_data, quint64(72));
	QCOMPARE(result.outcome, TileNetworkResult::Outcome::OfflineMiss);
	QVERIFY(result.body.isEmpty());
}

void TileNetworkManagerTest::revokingPrivateOriginRejectsActiveAndQueuedRequests()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.allow_private_networks = false;
	config.max_active_total = 1;
	config.max_active_per_host = 1;
	config.max_active_per_client = 1;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	QVERIFY(manager.approvePrivateOrigin(server.url(QStringLiteral("/"))));
	manager.submit(request(
		server.url(QStringLiteral("/hold/active")), 1, 1, 81));
	manager.submit(request(
		server.url(QStringLiteral("/hold/queued")), 1, 1, 82));
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);

	QVERIFY(manager.revokePrivateOrigin(server.url(QStringLiteral("/"))));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 2000);
	QHash<quint64, TileNetworkResult> by_user_data;
	for (int index = 0; index < spy.size(); ++index)
		by_user_data.insert(resultAt(spy, index).user_data, resultAt(spy, index));
	QCOMPARE(by_user_data[81].outcome, TileNetworkResult::Outcome::Rejected);
	QCOMPARE(by_user_data[82].outcome, TileNetworkResult::Outcome::Rejected);
	QVERIFY(by_user_data[81].error_string.contains(QStringLiteral("revoked")));
	QVERIFY(by_user_data[82].error_string.contains(QStringLiteral("revoked")));
	QCOMPARE(
		server.paths().count(QStringLiteral("/hold/queued")),
		0);
	server.releaseAll();
}

void TileNetworkManagerTest::revocationRejectsQueuedPrivateSuccess()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.allow_private_networks = false;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);
	auto const origin = server.url(QStringLiteral("/"));
	QVERIFY(manager.approvePrivateOrigin(origin));

	manager.submit(request(
		server.url(QStringLiteral("/hold/revoked-delivery")),
		1, 1, 83));
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);
	server.releaseOne();
	// Queue a successful worker completion, then revoke before the facade can
	// publish it.
	QThread::msleep(100);
	QCOMPARE(spy.size(), 0);
	QVERIFY(manager.revokePrivateOrigin(origin));

	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	auto const result = resultAt(spy, 0);
	QCOMPARE(result.user_data, quint64(83));
	QCOMPARE(result.outcome, TileNetworkResult::Outcome::Rejected);
	QVERIFY(result.private_network_rejected);
	QVERIFY(result.private_network_permission_revoked);
	QVERIFY(result.body.isEmpty());
}

void TileNetworkManagerTest::enforcesFairnessAndQueueBounds()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.max_active_total = 1;
	config.max_active_per_host = 1;
	config.max_active_per_client = 1;
	config.max_pending_total = 3;
	config.max_pending_per_client = 2;
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(server.url(QStringLiteral("/hold/first")), 1, 1, 1));
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);

	auto client_one_coverage = request(
		server.url(QStringLiteral("/order/client-one-coverage")), 1, 1, 2);
	client_one_coverage.priority = TileRequestPriority::Coverage;
	manager.submit(std::move(client_one_coverage));
	auto client_one_visible = request(
		server.url(QStringLiteral("/order/client-one-visible")), 1, 1, 3);
	client_one_visible.priority = TileRequestPriority::Visible;
	manager.submit(std::move(client_one_visible));
	manager.submit(request(
		server.url(QStringLiteral("/order/client-one-busy")), 1, 1, 6));
	auto client_two_background = request(
		server.url(QStringLiteral("/order/client-two-background")), 2, 1, 4);
	client_two_background.priority = TileRequestPriority::Background;
	manager.submit(std::move(client_two_background));
	manager.submit(request(server.url(QStringLiteral("/order/busy")), 3, 1, 5));

	QTRY_VERIFY_WITH_TIMEOUT(spy.size() >= 2, 2000);
	QHash<quint64, TileNetworkResult> queue_results;
	for (int index = 0; index < spy.size(); ++index)
		queue_results.insert(
			resultAt(spy, index).user_data, resultAt(spy, index));
	QCOMPARE(
		queue_results[5].outcome,
		TileNetworkResult::Outcome::Busy);
	QCOMPARE(
		queue_results[6].outcome,
		TileNetworkResult::Outcome::Busy);

	server.releaseOne();
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 6, 4000);
	auto const paths = server.paths();
	QCOMPARE(paths.at(0), QStringLiteral("/hold/first"));
	QCOMPARE(paths.at(1), QStringLiteral("/order/client-two-background"));
	QCOMPARE(paths.at(2), QStringLiteral("/order/client-one-coverage"));
	QCOMPARE(paths.at(3), QStringLiteral("/order/client-one-visible"));
}

void TileNetworkManagerTest::boundsOutstandingResultDelivery()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.max_active_total = 4;
	config.max_active_per_host = 4;
	config.max_active_per_client = 4;
	config.max_outstanding_results = 4;
	config.max_outstanding_response_bytes =
		2 * config.max_response_bytes;
	// This test controls completion explicitly. Keep the transport deadline well
	// outside the assertion window so a loaded CI runner cannot turn all four
	// held requests into timeout results before the first release is observed.
	config.absolute_timeout = std::chrono::seconds(10);
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	for (quint64 index = 0; index < 4; ++index)
	{
		manager.submit(request(
			server.url(
				QStringLiteral("/hold/delivery-%1").arg(index)),
			1, 1, 90 + index));
	}

	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 2, 2000);
	QTest::qWait(50);
	QCOMPARE(server.heldCount(), 2);
	QCOMPARE(server.paths().size(), 2);

	server.releaseOne();
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 2, 2000);
	QCOMPARE(server.paths().size(), 3);

	server.releaseAll();
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 3, 2000);
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);
	QCOMPARE(server.paths().size(), 4);
	server.releaseAll();
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 4, 2000);
	for (int index = 0; index < spy.size(); ++index)
		QCOMPARE(
			resultAt(spy, index).outcome,
			TileNetworkResult::Outcome::Success);
}

void TileNetworkManagerTest::servesFreshDiskCacheOffline()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	TileNetworkManager manager(configFor(directory));
	QSignalSpy spy(&manager, &TileNetworkManager::finished);
	auto const url = server.url(QStringLiteral("/cache"));

	manager.submit(request(url, 1, 1, 1));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 3000);
	QCOMPARE(resultAt(spy, 0).outcome, TileNetworkResult::Outcome::Success);

	manager.setOfflineMode(true);
	manager.submit(request(url, 1, 2, 2));
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 2, 3000);
	auto const cached = resultAt(spy, 1);
	QCOMPARE(cached.outcome, TileNetworkResult::Outcome::Success);
	QVERIFY(cached.from_cache);
	QCOMPARE(server.paths().count(QStringLiteral("/cache")), 1);
}

void TileNetworkManagerTest::servesHostnameDiskCacheOfflineWithoutDns()
{
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.allow_private_networks = false;
	auto const url = QUrl(QStringLiteral(
		"http://offline-cache.invalid/tiles/1/2/3.png"));
	auto const body = QByteArrayLiteral("cache-only-hostname-tile");

	{
		QNetworkDiskCache cache;
		cache.setCacheDirectory(config.cache_directory);
		cache.setMaximumCacheSize(config.disk_cache_bytes);
		QNetworkCacheMetaData metadata;
		metadata.setUrl(url);
		metadata.setSaveToDisk(true);
		metadata.setExpirationDate(
			QDateTime::currentDateTimeUtc().addSecs(3600));
		metadata.setRawHeaders({
			{ QByteArrayLiteral("Content-Type"),
			  QByteArrayLiteral("image/png") },
			{ QByteArrayLiteral("Content-Length"),
			  QByteArray::number(body.size()) },
			{ QByteArrayLiteral("Cache-Control"),
			  QByteArrayLiteral("public, max-age=3600") },
		});
		QNetworkCacheMetaData::AttributesMap attributes;
		attributes.insert(
			QNetworkRequest::HttpStatusCodeAttribute, QVariant(200));
		metadata.setAttributes(attributes);
		auto* device = cache.prepare(metadata);
		QVERIFY(device);
		QCOMPARE(device->write(body), qint64(body.size()));
		cache.insert(device);
	}

	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);
	manager.submit(request(url, 1));
	// The facade flips the offline flag immediately, while the worker receives
	// this already-queued submission before its offline transition event.
	manager.setOfflineMode(true);

	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	auto const result = resultAt(spy, 0);
	QCOMPARE(result.outcome, TileNetworkResult::Outcome::Success);
	QCOMPARE(result.body, body);
	QVERIFY(result.from_cache);
}

void TileNetworkManagerTest::timesOutWithoutBlockingTheCaller()
{
	MiniHttpServer server;
	QTemporaryDir directory;
	QVERIFY(directory.isValid());
	auto config = configFor(directory);
	config.transfer_timeout = std::chrono::milliseconds(40);
	config.absolute_timeout = std::chrono::milliseconds(80);
	TileNetworkManager manager(config);
	QSignalSpy spy(&manager, &TileNetworkManager::finished);
	QElapsedTimer elapsed;
	elapsed.start();

	manager.submit(request(server.url(QStringLiteral("/slow")), 1));
	QVERIFY(elapsed.elapsed() < 20);
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	QCOMPARE(
		resultAt(spy, 0).outcome,
		TileNetworkResult::Outcome::TransientError);
	QVERIFY(resultAt(spy, 0).error_string.contains(QStringLiteral("timed out")));
	server.releaseAll();
}

QTEST_GUILESS_MAIN(TileNetworkManagerTest)
