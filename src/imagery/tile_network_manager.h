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
#include <QHash>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QString>
#include <QUrl>
#include <QVector>

class QThread;
class QHostAddress;

namespace OpenOrienteering::imagery {

enum class TileRequestPriority
{
	Coverage,
	Visible,
	Background,
};

enum class NetworkPayloadKind
{
	TileImage,
	JsonDocument,
};

struct TileNetworkRequest
{
	QUrl url;
	quint64 client_id = 0;
	quint64 generation = 0;
	quint64 user_data = 0;
	TileRequestPriority priority = TileRequestPriority::Visible;
	NetworkPayloadKind payload_kind = NetworkPayloadKind::TileImage;
	double distance_priority = 0;
	QString referer;
	QVector<int> empty_http_status_codes = { 204, 404 };
	QByteArray if_none_match;
	QByteArray if_modified_since;
	/** Zero uses Config::max_response_bytes. */
	qint64 max_response_bytes = 0;
	/**
	 * Ephemeral credential injected by TileNetworkManager for an explicitly
	 * trusted origin. Never serialized into an imagery source or map.
	 */
	QByteArray bearer_token;
	QByteArray credential_identity;
	QString credential_origin;
	quint64 credential_generation = 0;
};

struct TileNetworkResult
{
	enum class Outcome
	{
		Success,
		NotModified,
		EmptyTile,
		Cancelled,
		OfflineMiss,
		/** A bounded scheduler queue is full; the request may be retried later. */
		Busy,
		TransientError,
		PermanentError,
		/** The request is invalid or disallowed by network policy. */
		Rejected,
	};

	Outcome outcome = Outcome::PermanentError;
	QByteArray body;
	QString content_type;
	QString error_string;
	QUrl final_url;
	QByteArray etag;
	QByteArray last_modified;
	int http_status = 0;
	bool from_cache = false;
	/** Rejected because a network destination was private/non-global. */
	bool private_network_rejected = false;
	/** Exact URL whose origin failed the private-network policy. */
	QUrl private_network_rejected_url;
	/** Rejection was caused by an explicit user revocation, not discovery. */
	bool private_network_permission_revoked = false;
	quint64 client_id = 0;
	quint64 generation = 0;
	quint64 user_data = 0;
};

/**
 * Application-scoped, bounded HTTP scheduler for online imagery resources.
 *
 * A single QNetworkAccessManager and QNetworkDiskCache live on a dedicated
 * event-loop thread. Public methods are thread-safe. Results are emitted on
 * this object's thread and retain the caller's client/generation/user fields.
 *
 * Requests are owner-fair: clients rotate before priority is considered, and
 * each client's coverage, visible, then background work is ordered by distance.
 * The manager enforces total, per-host, per-client, and pending limits.
 *
 * Tile images and OIC catalogs share one connection pool and disk cache while
 * retaining resource-specific Accept, cache, conditional-request, and body
 * limit behavior. Only HTTP(S) URLs without embedded credentials are accepted.
 * Cookies and HTTP authentication are disabled. Redirects are validated
 * explicitly and HTTPS downgrades are rejected by default. Response bodies and
 * time are bounded before parsing or image decoding. Active requests reserve
 * bounded result-delivery slots and body bytes until the application thread
 * consumes their completion, preventing a blocked UI event loop from growing
 * an unbounded cross-thread body backlog.
 *
 * Unapproved hostnames re-enter DNS preflight for retries and redirects, and
 * successful decisions expire after one second of scheduler waiting.
 * QNetworkAccessManager performs the eventual connection resolution itself,
 * so this substantially narrows but cannot entirely remove the DNS-rebinding
 * interval without bypassing Qt's TLS and HTTP cache stack.
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
		/** Active requests plus results awaiting application-thread delivery. */
		int max_outstanding_results = 16;
		/** Reserved response limits for those outstanding requests/results. */
		qint64 max_outstanding_response_bytes = qint64(48) << 20;
#else
		qint64 disk_cache_bytes = qint64(512) << 20;
		int max_active_total = 12;
		int max_active_per_client = 6;
		/** Active requests plus results awaiting application-thread delivery. */
		int max_outstanding_results = 32;
		/** Reserved response limits for those outstanding requests/results. */
		qint64 max_outstanding_response_bytes = qint64(192) << 20;
#endif
		int max_active_per_host = 6;
		int max_pending_total = 2048;
		int max_pending_per_client = 256;
		/** Bounds for long-lived scheduler/cache bookkeeping. Zero retains none. */
		int max_negative_cache_entries = 4096;
		int max_client_history_entries = 4096;
		int max_host_backoff_entries = 1024;
		int max_destination_cache_entries = 1024;
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
	/** True only for destinations which may be contacted without approval. */
	static bool isPublicDestinationAddress(const QHostAddress& address);

	Token submit(TileNetworkRequest request);
	void cancel(Token token);
	void cancelClient(
		quint64 client_id,
		quint64 through_generation = std::numeric_limits<quint64>::max());

	void setOfflineMode(bool offline);
	bool offlineMode() const noexcept;
	bool approvePrivateOrigin(const QUrl& url);
	bool revokePrivateOrigin(const QUrl& url);
	bool isPrivateOriginApproved(const QUrl& url) const;

	/**
	 * Register an in-memory bearer credential for one exact origin.
	 * Authenticated imagery bypasses the shared HTTP disk cache, preventing
	 * account-specific responses from crossing login boundaries.
	 */
	bool setBearerCredential(const QUrl& origin, QByteArray token, QByteArray identity);
	void clearBearerCredential(const QUrl& origin);
	void clearBearerCredentials();

signals:
	void offlineModeChanged(bool offline);
	void privateOriginApprovalChanged(
		const QString& origin,
		bool approved);
	/** Emitted whenever an origin's authenticated identity changes or is removed. */
	void bearerCredentialChanged(const QString& origin);
	void finished(
		OpenOrienteering::imagery::TileNetworkManager::Token token,
		const OpenOrienteering::imagery::TileNetworkResult& result);

private:
	class Worker;
	struct NetworkModeSnapshot
	{
		bool offline = false;
		quint64 generation = 0;
	};
	NetworkModeSnapshot networkModeSnapshot() const;
	quint64 privateOriginGeneration(const QString& origin) const;

	Config config_;
	QThread* network_thread_ = nullptr;
	Worker* worker_ = nullptr;
	std::atomic<Token> next_token_ { 1 };
	std::atomic_bool offline_ { false };
	std::atomic<quint64> network_mode_generation_ { 1 };
	mutable QMutex network_mode_mutex_;
	mutable QMutex permissions_mutex_;
	struct BearerCredential
	{
		QByteArray token;
		QByteArray identity;
		quint64 generation = 0;
	};
	mutable QMutex credentials_mutex_;
	QHash<QString, BearerCredential> bearer_credentials_;
	quint64 next_credential_generation_ = 1;
	QSet<QString> approved_private_origins_;
	QHash<QString, quint64> private_origin_generations_;
	quint64 next_private_origin_generation_ = 1;
	quint64 credentialGeneration(const QString& origin) const;
};

}  // namespace OpenOrienteering::imagery

Q_DECLARE_METATYPE(OpenOrienteering::imagery::TileNetworkResult)

#endif
