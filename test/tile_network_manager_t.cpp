/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include "tile_network_manager_t.h"

#include <memory>

#include <QtTest>
#include <QHash>
#include <QPointer>
#include <QSignalSpy>
#include <QStringList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUrl>

#include "imagery/tile_network_manager.h"

using OpenOrienteering::imagery::TileNetworkManager;
using OpenOrienteering::imagery::TileNetworkRequest;
using OpenOrienteering::imagery::TileNetworkResult;
using OpenOrienteering::imagery::TileRequestPriority;

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
					handle(socket, path);
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

	void handle(QTcpSocket* socket, const QString& path)
	{
		if (path == QLatin1String("/ok"))
		{
			respond(
				socket, 200, {}, QByteArrayLiteral("tile"),
				QByteArrayLiteral("Content-Type: image/png\r\n"));
		}
		else if (path == QLatin1String("/empty"))
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

	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 3, 2000);
	for (int index = 0; index < spy.size(); ++index)
		QCOMPARE(resultAt(spy, index).outcome, TileNetworkResult::Outcome::Rejected);
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
	TileNetworkManager manager(configFor(directory));
	QSignalSpy spy(&manager, &TileNetworkManager::finished);

	manager.submit(request(server.url(QStringLiteral("/hold/old")), 42, 3));
	QTRY_COMPARE_WITH_TIMEOUT(server.heldCount(), 1, 2000);
	manager.cancelClient(42, 3);
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 1, 2000);
	QCOMPARE(resultAt(spy, 0).outcome, TileNetworkResult::Outcome::Cancelled);
	QCOMPARE(resultAt(spy, 0).client_id, quint64(42));
	QCOMPARE(resultAt(spy, 0).generation, quint64(3));
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
	auto client_two_background = request(
		server.url(QStringLiteral("/order/client-two-background")), 2, 1, 4);
	client_two_background.priority = TileRequestPriority::Background;
	manager.submit(std::move(client_two_background));
	manager.submit(request(server.url(QStringLiteral("/order/rejected")), 3, 1, 5));

	QTRY_VERIFY_WITH_TIMEOUT(spy.size() >= 1, 2000);
	QCOMPARE(resultAt(spy, 0).user_data, quint64(5));
	QCOMPARE(resultAt(spy, 0).outcome, TileNetworkResult::Outcome::Rejected);

	server.releaseOne();
	QTRY_COMPARE_WITH_TIMEOUT(spy.size(), 5, 4000);
	auto const paths = server.paths();
	QCOMPARE(paths.at(0), QStringLiteral("/hold/first"));
	QCOMPARE(paths.at(1), QStringLiteral("/order/client-two-background"));
	QCOMPARE(paths.at(2), QStringLiteral("/order/client-one-coverage"));
	QCOMPARE(paths.at(3), QStringLiteral("/order/client-one-visible"));
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
