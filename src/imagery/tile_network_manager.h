/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 */

#ifndef OPENORIENTEERING_TILE_NETWORK_MANAGER_H
#define OPENORIENTEERING_TILE_NETWORK_MANAGER_H

#include <atomic>
#include <chrono>
#include <limits>

#include <QByteArray>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

class QThread;

namespace OpenOrienteering::imagery {

enum class TileRequestPriority
{
	Coverage,
	Visible,
	Background,
};

struct TileNetworkRequest
{
	QUrl url;
	quint64 client_id = 0;
	quint64 generation = 0;
	quint64 user_data = 0;
	TileRequestPriority priority = TileRequestPriority::Visible;
	double distance_priority = 0;
	QString referer;
	QVector<int> empty_http_status_codes = { 204, 404 };
};

struct TileNetworkResult
{
	enum class Outcome
	{
		Success,
		EmptyTile,
		Cancelled,
		OfflineMiss,
		TransientError,
		PermanentError,
		Rejected,
	};

	Outcome outcome = Outcome::PermanentError;
	QByteArray body;
	QString content_type;
	QString error_string;
	int http_status = 0;
	bool from_cache = false;
	quint64 client_id = 0;
	quint64 generation = 0;
	quint64 user_data = 0;
};

/**
 * Application-scoped, bounded HTTP scheduler for tiled imagery.
 *
 * A single QNetworkAccessManager and QNetworkDiskCache live on a dedicated
 * event-loop thread. Public methods are thread-safe. Results are emitted on
 * this object's thread and retain the caller's client/generation/user fields.
 *
 * Requests are owner-fair: clients rotate before priority is considered, and
 * each client's coverage, visible, then background work is ordered by distance.
 * The manager enforces total, per-host, per-client, and pending limits.
 *
 * Only HTTP(S) URLs without embedded credentials are accepted. Cookies and
 * HTTP authentication are disabled. Redirects are validated explicitly and
 * HTTPS downgrades are rejected by default. Response bodies and time are
 * bounded before image decoding.
 */
class TileNetworkManager final : public QObject
{
Q_OBJECT

public:
	using Token = quint64;

	struct Config
	{
		QString cache_directory;
#ifdef Q_OS_ANDROID
		qint64 disk_cache_bytes = qint64(128) << 20;
		int max_active_total = 6;
		int max_active_per_client = 4;
#else
		qint64 disk_cache_bytes = qint64(512) << 20;
		int max_active_total = 12;
		int max_active_per_client = 6;
#endif
		int max_active_per_host = 6;
		int max_pending_total = 2048;
		int max_pending_per_client = 256;
		int max_redirects = 5;
		int max_retries = 2;
		int retry_base_delay_ms = 400;
		int retry_max_delay_ms = 15'000;
		int negative_cache_ttl_ms = 5 * 60 * 1000;
		qint64 max_response_bytes = 20 * 1024 * 1024;
		std::chrono::milliseconds first_byte_timeout = std::chrono::seconds(10);
		std::chrono::milliseconds transfer_timeout = std::chrono::seconds(20);
		std::chrono::milliseconds absolute_timeout = std::chrono::seconds(45);
		QByteArray user_agent;
		QSet<QString> approved_private_origins;
		/** Intended for deterministic tests and explicitly trusted deployments. */
		bool allow_private_networks = false;
		bool allow_https_downgrade = false;
	};

	explicit TileNetworkManager(QObject* parent = nullptr);
	explicit TileNetworkManager(Config config, QObject* parent = nullptr);
	~TileNetworkManager() override;

	TileNetworkManager(const TileNetworkManager&) = delete;
	TileNetworkManager& operator=(const TileNetworkManager&) = delete;

	static TileNetworkManager& instance();
	static quint64 nextClientId();
	static QString canonicalOrigin(const QUrl& url);

	Token submit(TileNetworkRequest request);
	void cancel(Token token);
	void cancelClient(
		quint64 client_id,
		quint64 through_generation = std::numeric_limits<quint64>::max());

	void setOfflineMode(bool offline);
	bool offlineMode() const noexcept;

signals:
	void finished(
		OpenOrienteering::imagery::TileNetworkManager::Token token,
		const OpenOrienteering::imagery::TileNetworkResult& result);

private:
	class Worker;

	Config config_;
	QThread* network_thread_ = nullptr;
	Worker* worker_ = nullptr;
	std::atomic<Token> next_token_ { 1 };
	std::atomic_bool offline_ { false };
};

}  // namespace OpenOrienteering::imagery

Q_DECLARE_METATYPE(OpenOrienteering::imagery::TileNetworkResult)

#endif
