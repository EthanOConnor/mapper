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

#include "imagery/tile_network_manager.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include <QAuthenticator>
#include <QAbstractSocket>
#include <QApplicationStatic>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QHostAddress>
#include <QHostInfo>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkCookie>
#include <QNetworkCookieJar>
#include <QNetworkDiskCache>
#include <QNetworkProxy>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>
#include <QVariant>

namespace OpenOrienteering::imagery {

namespace {

class RejectingCookieJar final : public QNetworkCookieJar
{
public:
	using QNetworkCookieJar::QNetworkCookieJar;

	QList<QNetworkCookie> cookiesForUrl(const QUrl&) const override
	{
		return {};
	}

	bool setCookiesFromUrl(const QList<QNetworkCookie>&, const QUrl&) override
	{
		return false;
	}
};

QByteArray defaultUserAgent()
{
	auto name = QCoreApplication::applicationName();
	if (name.isEmpty())
		name = QStringLiteral("OpenOrienteering-Mapper");
	auto version = QCoreApplication::applicationVersion();
	if (version.isEmpty())
		version = QStringLiteral("development");
	return (name + QLatin1Char('/') + version
	        + QLatin1String(" (+https://www.openorienteering.org/)")).toUtf8();
}

bool isTransientNetworkError(QNetworkReply::NetworkError error)
{
	switch (error)
	{
	case QNetworkReply::ConnectionRefusedError:
	case QNetworkReply::RemoteHostClosedError:
	case QNetworkReply::HostNotFoundError:
	case QNetworkReply::TimeoutError:
	case QNetworkReply::TemporaryNetworkFailureError:
	case QNetworkReply::NetworkSessionFailedError:
	case QNetworkReply::ProxyConnectionClosedError:
	case QNetworkReply::ProxyNotFoundError:
	case QNetworkReply::ProxyTimeoutError:
	case QNetworkReply::ServiceUnavailableError:
	case QNetworkReply::UnknownNetworkError:
	case QNetworkReply::UnknownProxyError:
		return true;
	default:
		return false;
	}
}

bool isTransientHttpStatus(int status)
{
	switch (status)
	{
	case 408:
	case 425:
	case 429:
	case 500:
	case 502:
	case 503:
	case 504:
		return true;
	default:
		return false;
	}
}

QString hostKey(const QUrl& url)
{
	auto const default_port = url.scheme() == QLatin1String("https") ? 443 : 80;
	QHostAddress address;
	auto const host = address.setAddress(url.host())
	                ? address.toString().toLower()
	                : QString::fromLatin1(
	                      QUrl::toAce(url.host()).toLower());
	QUrl origin;
	origin.setScheme(url.scheme().toLower());
	origin.setHost(host);
	origin.setPort(url.port(default_port));
	return origin.toString(QUrl::FullyEncoded);
}

bool isPublicDestination(const QHostAddress& candidate)
{
	auto address = candidate;
	bool has_ipv4 = false;
	auto const ipv4 = address.toIPv4Address(&has_ipv4);
	if (has_ipv4)
		address = QHostAddress(ipv4);

	auto const in_subnet = [&address](
		const QHostAddress& prefix,
		int length) {
		return address.isInSubnet(prefix, length);
	};
	if (address.protocol()
	    == QAbstractSocket::IPv4Protocol)
	{
		static const std::array non_public {
			std::pair { QHostAddress(QStringLiteral("0.0.0.0")), 8 },
			std::pair { QHostAddress(QStringLiteral("10.0.0.0")), 8 },
			std::pair { QHostAddress(QStringLiteral("100.64.0.0")), 10 },
			std::pair { QHostAddress(QStringLiteral("127.0.0.0")), 8 },
			std::pair { QHostAddress(QStringLiteral("169.254.0.0")), 16 },
			std::pair { QHostAddress(QStringLiteral("172.16.0.0")), 12 },
			std::pair { QHostAddress(QStringLiteral("192.0.0.0")), 24 },
			std::pair { QHostAddress(QStringLiteral("192.0.2.0")), 24 },
			std::pair { QHostAddress(QStringLiteral("192.31.196.0")), 24 },
			std::pair { QHostAddress(QStringLiteral("192.52.193.0")), 24 },
			std::pair { QHostAddress(QStringLiteral("192.88.99.0")), 24 },
			std::pair { QHostAddress(QStringLiteral("192.168.0.0")), 16 },
			std::pair { QHostAddress(QStringLiteral("192.175.48.0")), 24 },
			std::pair { QHostAddress(QStringLiteral("198.18.0.0")), 15 },
			std::pair { QHostAddress(QStringLiteral("198.51.100.0")), 24 },
			std::pair { QHostAddress(QStringLiteral("203.0.113.0")), 24 },
			std::pair { QHostAddress(QStringLiteral("224.0.0.0")), 4 },
			std::pair { QHostAddress(QStringLiteral("240.0.0.0")), 4 },
		};
		if (std::ranges::any_of(
			    non_public,
			    [&in_subnet](auto const& subnet) {
				    return in_subnet(
					    subnet.first, subnet.second);
			    }))
			return false;
	}
	else if (address.protocol()
	         == QAbstractSocket::IPv6Protocol)
	{
		// The well-known NAT64 prefix is globally reachable, but its embedded
		// IPv4 destination must independently pass this same policy.
		static const QHostAddress nat64(
			QStringLiteral("64:ff9b::"));
		if (address.isInSubnet(nat64, 96))
		{
			auto const bytes = address.toIPv6Address();
			auto const embedded =
				(quint32(bytes[12]) << 24)
				| (quint32(bytes[13]) << 16)
				| (quint32(bytes[14]) << 8)
				| quint32(bytes[15]);
			return isPublicDestination(
				QHostAddress(embedded));
		}

		static const QHostAddress global_unicast(
			QStringLiteral("2000::"));
		if (!address.isInSubnet(global_unicast, 3))
			return false;
		static const std::array non_public {
			std::pair { QHostAddress(QStringLiteral("2001::")), 23 },
			std::pair { QHostAddress(QStringLiteral("2001:db8::")), 32 },
			std::pair { QHostAddress(QStringLiteral("2002::")), 16 },
			std::pair { QHostAddress(QStringLiteral("2620:4f:8000::")), 48 },
			std::pair { QHostAddress(QStringLiteral("3fff::")), 20 },
		};
		if (std::ranges::any_of(
			    non_public,
			    [&in_subnet](auto const& subnet) {
				    return in_subnet(
					    subnet.first, subnet.second);
			    }))
			return false;
	}
	else
	{
		return false;
	}

	// QHostAddress::isGlobal() intentionally includes RFC 1918, IPv6 ULA,
	// and deprecated site-local ranges. Online imagery treats all of those as
	// permission-gated destinations, while isGlobal() rejects other reserved
	// and documentation-only ranges.
	return address.isGlobal()
	       && !address.isNull()
	       && !address.isLoopback()
	       && !address.isLinkLocal()
	       && !address.isMulticast()
	       && !address.isBroadcast()
	       && !address.isPrivateUse()
	       && !address.isSiteLocal();
}

bool isLocalHostname(QString host)
{
	host = host.toLower();
	return host == QLatin1String("localhost")
	       || host == QLatin1String("localhost.localdomain")
	       || host.endsWith(QLatin1String(".localhost"))
	       || host.endsWith(QLatin1String(".local"))
	       || host.endsWith(QLatin1String(".internal"))
	       || host.endsWith(QLatin1String(".home.arpa"));
}

QString validateHttpUrl(
	const QUrl& url,
	const TileNetworkManager::Config& config,
	QUrl* private_network_rejected_url = nullptr,
	bool enforce_private_network_policy = true)
{
	if (private_network_rejected_url)
		private_network_rejected_url->clear();
	if (!url.isValid() || url.isRelative())
		return TileNetworkManager::tr("The imagery URL is invalid.");
	auto const scheme = url.scheme().toLower();
	if (scheme != QLatin1String("http") && scheme != QLatin1String("https"))
		return TileNetworkManager::tr("Only HTTP and HTTPS imagery URLs are allowed.");
	if (url.host().isEmpty())
		return TileNetworkManager::tr("The imagery URL has no host.");
	if (!url.userInfo().isEmpty())
		return TileNetworkManager::tr("Credentials embedded in imagery URLs are not allowed.");
	if (url.hasFragment())
		return TileNetworkManager::tr("Imagery URLs must not contain fragments.");
	auto const encoded = url.toEncoded();
	if (encoded.contains('\r') || encoded.contains('\n') || encoded.contains('\0'))
		return TileNetworkManager::tr("The imagery URL contains unsafe control characters.");
	if (encoded.size() > 16 * 1024)
		return TileNetworkManager::tr("The imagery URL is too long.");
	auto const port = url.port();
	if (port == 0 || port > 65535)
		return TileNetworkManager::tr("The imagery URL has an invalid port.");
	if (!enforce_private_network_policy)
		return {};

	if (config.allow_private_networks
	    || config.approved_private_origins.contains(hostKey(url)))
		return {};

	QHostAddress address;
	if (address.setAddress(url.host()))
	{
		if (!isPublicDestination(address))
		{
			if (private_network_rejected_url)
				*private_network_rejected_url = url;
			return TileNetworkManager::tr(
				"Private, local, and link-local imagery hosts require explicit permission.");
		}
	}
	else if (isLocalHostname(url.host()))
	{
		if (private_network_rejected_url)
			*private_network_rejected_url = url;
		return TileNetworkManager::tr(
			"Private, local, and link-local imagery hosts require explicit permission.");
	}
	return {};
}

QByteArray negativeCacheKey(const TileNetworkRequest& request)
{
	QCryptographicHash digest(QCryptographicHash::Sha256);
	QByteArray representation;
	representation.append(static_cast<char>(
		request.payload_kind));
	representation.append('\n');
	representation.append(
		request.url.toEncoded(QUrl::FullyEncoded));
	representation.append('\n');
	representation.append(request.referer.toUtf8());
	representation.append('\n');
	representation.append(request.credential_identity);
	representation.append('\n');
	auto statuses = request.empty_http_status_codes;
	std::sort(statuses.begin(), statuses.end());
	for (auto const status : std::as_const(statuses))
	{
		representation.append(QByteArray::number(status));
		representation.append(',');
	}
	digest.addData(representation);
	return digest.result();
}

QString validateRequest(
	const TileNetworkRequest& request,
	const TileNetworkManager::Config& config,
	QUrl* private_network_rejected_url = nullptr)
{
	if (private_network_rejected_url)
		private_network_rejected_url->clear();
	if (request.client_id == 0)
		return TileNetworkManager::tr("The imagery request has no client identity.");
	if (!std::isfinite(request.distance_priority))
		return TileNetworkManager::tr("The imagery request priority is invalid.");
	if (auto const error = validateHttpUrl(
		    request.url, config, private_network_rejected_url);
	    !error.isEmpty())
	{
		return error;
	}
	if (!request.referer.isEmpty())
	{
		auto const referer = QUrl(request.referer);
		if (auto const error = validateHttpUrl(
			    referer, config, nullptr, false);
		    !error.isEmpty())
		{
			return TileNetworkManager::tr("The imagery Referer is invalid: %1").arg(error);
		}
	}
	if (request.empty_http_status_codes.size() > 32)
		return TileNetworkManager::tr("Too many empty-tile HTTP status codes.");
	QSet<int> statuses;
	for (auto const status : request.empty_http_status_codes)
	{
		if (status < 100 || status > 599 || statuses.contains(status))
			return TileNetworkManager::tr("The empty-tile HTTP status list is invalid.");
		statuses.insert(status);
	}
	auto const valid_header = [](const QByteArray& value) {
		return value.size() <= 8192
		       && !value.contains('\r')
		       && !value.contains('\n')
		       && !value.contains('\0');
	};
	if (!valid_header(request.if_none_match)
	    || !valid_header(request.if_modified_since)
	    || !valid_header(request.bearer_token)
	    || !valid_header(request.credential_identity))
	{
		return TileNetworkManager::tr(
			"The imagery conditional request headers are invalid.");
	}
	if ((!request.bearer_token.isEmpty() || !request.credential_identity.isEmpty())
	    && (request.bearer_token.isEmpty() || request.credential_identity.isEmpty()
	        || request.credential_origin.isEmpty()
	        || request.bearer_token.size() > 4096
	        || request.credential_identity.size() > 128))
	{
		return TileNetworkManager::tr("The imagery bearer credential is invalid.");
	}
	if (request.max_response_bytes < 0
	    || request.max_response_bytes > config.max_response_bytes)
	{
		return TileNetworkManager::tr(
			"The imagery response limit is invalid.");
	}
	return {};
}

bool isBetterEntry(const TileNetworkRequest& lhs, quint64 lhs_sequence,
	               const TileNetworkRequest& rhs, quint64 rhs_sequence)
{
	if (lhs.priority != rhs.priority)
		return lhs.priority < rhs.priority;
	if (lhs.distance_priority != rhs.distance_priority)
		return lhs.distance_priority < rhs.distance_priority;
	return lhs_sequence < rhs_sequence;
}

}  // namespace

class TileNetworkManager::Worker final : public QObject
{
public:
	Worker(Config config,
	       QPointer<TileNetworkManager> facade,
	       std::atomic_bool* offline,
	       std::atomic<quint64>* network_mode_generation)
		 : config_(std::move(config))
		 , facade_(std::move(facade))
		 , offline_(offline)
		 , network_mode_generation_(network_mode_generation)
	{}

	void initialize()
	{
		Q_ASSERT(QThread::currentThread() == thread());
		clock_.start();
		wake_timer_ = new QTimer(this);
		wake_timer_->setSingleShot(true);
		connect(wake_timer_, &QTimer::timeout, this, [this] { dispatch(); });
		auto* state_prune_timer = new QTimer(this);
		state_prune_timer->setInterval(std::chrono::minutes(1));
		connect(state_prune_timer, &QTimer::timeout, this, [this] {
			pruneNegativeCache();
			pruneClientHistory();
			pruneHostBackoff();
			pruneDestinationCache();
		});
		state_prune_timer->start();

		network_ = new QNetworkAccessManager(this);
		network_->setCookieJar(new RejectingCookieJar(network_));
		network_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
		network_->setTransferTimeout(config_.transfer_timeout);
		connect(
			network_, &QNetworkAccessManager::authenticationRequired,
			this, [this](QNetworkReply* reply, QAuthenticator*) {
				auto const found = active_replies_.constFind(reply);
				if (found == active_replies_.cend())
					return;
				(*found)->authentication_rejected = true;
				reply->abort();
			});
		connect(
			network_, &QNetworkAccessManager::proxyAuthenticationRequired,
			this, [](const QNetworkProxy&, QAuthenticator*) {
				// Catalogs and sources never supply proxy credentials.
			});

		auto* cache = new QNetworkDiskCache(network_);
		cache->setCacheDirectory(config_.cache_directory);
		cache->setMaximumCacheSize(config_.disk_cache_bytes);
		network_->setCache(cache);
	}

	void shutdown()
	{
		Q_ASSERT(QThread::currentThread() == thread());
		shutting_down_ = true;
		if (wake_timer_)
			wake_timer_->stop();
		auto const active_replies = active_replies_.keys();
		for (auto* reply : active_replies)
		{
			disconnect(reply, nullptr, this, nullptr);
			reply->abort();
		}
		for (auto const lookup_id : std::as_const(destination_lookups_))
			QHostInfo::abortHostLookup(lookup_id);
		destination_lookups_.clear();
		destination_waiters_.clear();
		queue_.clear();
		entries_.clear();
		active_replies_.clear();
		active_hosts_.clear();
		active_clients_.clear();
		client_entry_counts_.clear();
		client_last_service_.clear();
		host_not_before_.clear();
		negative_cache_.clear();
		destination_cache_.clear();
		active_total_ = 0;
		outstanding_results_ = 0;
		outstanding_response_bytes_ = 0;
		if (network_)
		{
			delete network_;
			network_ = nullptr;
		}
	}

	void submit(Token token, TileNetworkRequest request)
	{
		Q_ASSERT(QThread::currentThread() == thread());
		if (shutting_down_)
			return;

		QUrl private_network_rejected_url;
		if (auto const error = validateRequest(
			    request, config_, &private_network_rejected_url);
		    !error.isEmpty())
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Rejected;
			result.private_network_rejected =
				!private_network_rejected_url.isEmpty();
			result.private_network_rejected_url =
				private_network_rejected_url;
			result.error_string = error;
			deliver(
				token,
				request,
				std::move(result),
				false,
				0,
				DeliveryGuard {});
			return;
		}

		auto url = request.url;
		auto const negative_key = negativeCacheKey(request);
		auto negative = negative_cache_.find(negative_key);
		if (request.payload_kind == NetworkPayloadKind::TileImage
		    && negative != negative_cache_.end())
		{
			if (negative->expires > now())
			{
				negative->last_access = nextStateAccess();
				TileNetworkResult result;
				result.outcome = TileNetworkResult::Outcome::EmptyTile;
				result.from_cache = true;
				deliver(
					token,
					request,
					std::move(result),
					false,
					0,
					DeliveryGuard {});
				return;
			}
			negative_cache_.erase(negative);
		}

		auto const pending_total = std::max<qsizetype>(
			0, entries_.size() - active_total_);
		auto const pending_for_client = std::ranges::count_if(
			entries_, [&request](auto const& entry) {
				return !entry->reply
				       && entry->request.client_id == request.client_id;
			});
		if (pending_total >= config_.max_pending_total
		    || pending_for_client >= config_.max_pending_per_client)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Busy;
			result.error_string = TileNetworkManager::tr(
				"The imagery request queue is full.");
			deliver(
				token,
				request,
				std::move(result),
				false,
				0,
				DeliveryGuard {});
			return;
		}

		auto entry = std::make_shared<Entry>();
		entry->token = token;
		entry->request = std::move(request);
		entry->current_url = std::move(url);
		entry->sequence = next_sequence_++;
		entries_.insert(token, entry);
		++client_entry_counts_[entry->request.client_id];
		queueAfterDestinationCheck(entry);
	}

	void cancel(Token token)
	{
		Q_ASSERT(QThread::currentThread() == thread());
		auto const found = entries_.constFind(token);
		if (found == entries_.cend())
			return;
		auto const entry = *found;
		entry->cancelled = true;
		if (entry->reply)
		{
			entry->reply->abort();
			return;
		}
		eraseQueued(entry);
		TileNetworkResult result;
		result.outcome = TileNetworkResult::Outcome::Cancelled;
		finish(entry, std::move(result));
	}

	void cancelClient(quint64 client_id, quint64 through_generation)
	{
		Q_ASSERT(QThread::currentThread() == thread());
		QVector<Token> tokens;
		for (auto it = entries_.cbegin(); it != entries_.cend(); ++it)
		{
			auto const& request = (*it)->request;
			if (request.client_id == client_id
			    && request.generation <= through_generation)
			{
				tokens.push_back(it.key());
			}
		}
		for (auto const token : tokens)
			cancel(token);
	}

	void bearerCredentialChanged(const QString& origin)
	{
		Q_ASSERT(QThread::currentThread() == thread());
		QVector<Token> tokens;
		for (auto it = entries_.cbegin(); it != entries_.cend(); ++it)
		{
			if ((*it)->request.credential_origin == origin)
				tokens.push_back(it.key());
		}
		for (auto const token : tokens)
			cancel(token);
	}

	void setOfflineMode(bool offline)
	{
		Q_ASSERT(QThread::currentThread() == thread());
		if (!offline)
		{
			dispatch();
			return;
		}

		QVector<std::shared_ptr<Entry>> active;
		active.reserve(active_replies_.size());
		for (auto const& entry : std::as_const(active_replies_))
			active.push_back(entry);
		for (auto const& entry : std::as_const(active))
		{
				if (!entries_.contains(entry->token) || !entry->reply
				    || entry->cache_only_request)
					continue;
			entry->offline_abort = true;
			entry->body.clear();
		}
		for (auto const& entry : std::as_const(active))
		{
			if (entry->reply && !entry->cache_only_request)
				entry->reply->abort();
		}

		QVector<std::shared_ptr<Entry>> destination_waiters;
		for (auto const& waiters : std::as_const(destination_waiters_))
			destination_waiters.append(waiters);
		for (auto const lookup_id : std::as_const(destination_lookups_))
			QHostInfo::abortHostLookup(lookup_id);
		destination_lookups_.clear();
		destination_waiters_.clear();
		for (auto const& entry : std::as_const(destination_waiters))
		{
			if (!entries_.contains(entry->token))
				continue;
			entry->validated_origin.clear();
			entry->destination_valid_until = 0;
			enqueue(entry);
		}
	}

	void setPrivateOriginApproved(
		QString origin,
		bool approved)
	{
		Q_ASSERT(QThread::currentThread() == thread());
		if (approved)
			config_.approved_private_origins.insert(origin);
		else
			config_.approved_private_origins.remove(origin);
		destination_cache_.remove(origin);
		if (approved || config_.allow_private_networks)
			return;

		QVector<std::shared_ptr<Entry>> affected;
		affected.reserve(entries_.size());
		for (auto const& entry : std::as_const(entries_))
		{
			if (hostKey(entry->request.url) == origin
			    || hostKey(entry->current_url) == origin)
			{
				affected.push_back(entry);
			}
		}

		for (auto const& entry : std::as_const(affected))
		{
			if (!entries_.contains(entry->token))
				continue;
			entry->permission_revoked = true;
			entry->permission_revoked_url =
				hostKey(entry->current_url) == origin
					? entry->current_url
					: entry->request.url;
			entry->body.clear();
		}
		for (auto const& entry : std::as_const(affected))
		{
			if (!entries_.contains(entry->token))
				continue;
			if (entry->reply)
			{
				entry->reply->abort();
				continue;
			}
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Rejected;
			result.private_network_rejected = true;
			result.private_network_rejected_url =
				entry->permission_revoked_url;
			result.private_network_permission_revoked = true;
			result.error_string = TileNetworkManager::tr(
				"Permission for the private imagery origin was revoked.");
			finish(entry, std::move(result));
		}

		pruneDestinationWaiters();
		dispatch();
	}

private:
	struct Entry
	{
		Token token = 0;
		TileNetworkRequest request;
		QUrl current_url;
		quint64 sequence = 0;
		int redirects = 0;
		int retries = 0;
		qint64 not_before = 0;
		QPointer<QNetworkReply> reply;
		QByteArray body;
		QString active_host;
		bool cancelled = false;
		bool too_large = false;
		bool absolute_timeout = false;
		bool authentication_rejected = false;
		bool received_metadata = false;
		bool offline_abort = false;
			bool permission_revoked = false;
			QUrl permission_revoked_url;
		bool cache_only_request = false;
		bool result_slot_reserved = false;
		qint64 reserved_response_bytes = 0;
		quint64 network_mode_generation = 0;
		QString private_permission_origin;
		QUrl private_permission_url;
		quint64 private_permission_generation = 0;
		QString validated_origin;
		qint64 destination_valid_until = 0;
	};

	struct DeliveryGuard
	{
		quint64 network_mode_generation = 0;
		bool cache_only_request = false;
		QString private_permission_origin;
		QUrl private_permission_url;
		quint64 private_permission_generation = 0;
		QString credential_origin;
		quint64 credential_generation = 0;
	};

	struct DestinationDecision
	{
		bool allowed = false;
		bool transient_failure = false;
		QString error;
		qint64 expires = 0;
		quint64 last_access = 0;
	};

	struct NegativeCacheEntry
	{
		qint64 expires = 0;
		quint64 last_access = 0;
	};

	qint64 now() const
	{
		return clock_.elapsed();
	}

	qint64 responseLimit(const Entry& entry) const
	{
		return entry.request.max_response_bytes > 0
		         ? entry.request.max_response_bytes
		         : config_.max_response_bytes;
	}

	quint64 privateOriginGeneration(const QString& origin) const
	{
		auto const facade = facade_;
		return facade ? facade->privateOriginGeneration(origin) : 0;
	}

	quint64 credentialGeneration(const QString& origin) const
	{
		auto const facade = facade_;
		return facade ? facade->credentialGeneration(origin) : 0;
	}

	TileNetworkManager::NetworkModeSnapshot networkModeSnapshot() const
	{
		auto const facade = facade_;
		if (facade)
			return facade->networkModeSnapshot();
		return {
			offline_->load(),
			network_mode_generation_->load(),
		};
	}

	bool privatePermissionChanged(const Entry& entry) const
	{
		return entry.private_permission_generation != 0
		       && privateOriginGeneration(entry.private_permission_origin)
		            != entry.private_permission_generation;
	}

	bool networkModeChanged(const Entry& entry) const
	{
		return !entry.cache_only_request
		       && entry.network_mode_generation != 0
		       && entry.network_mode_generation
		            != networkModeSnapshot().generation;
	}

	DeliveryGuard deliveryGuard(const Entry& entry) const
	{
		return {
			entry.network_mode_generation,
			entry.cache_only_request,
			entry.private_permission_origin,
			entry.private_permission_url,
			entry.private_permission_generation,
			entry.request.credential_origin,
			entry.request.credential_generation,
		};
	}

	quint64 nextStateAccess()
	{
		if (next_state_access_ == std::numeric_limits<quint64>::max())
		{
			for (auto& entry : negative_cache_)
				entry.last_access = 0;
			for (auto& entry : destination_cache_)
				entry.last_access = 0;
			next_state_access_ = 1;
		}
		return next_state_access_++;
	}

	void pruneNegativeCache()
	{
		auto const current = now();
		negative_cache_.removeIf([current](
			QHash<QByteArray, NegativeCacheEntry>::iterator it) {
			return it->expires <= current;
		});
		while (negative_cache_.size() > config_.max_negative_cache_entries)
		{
			auto victim = negative_cache_.end();
			for (auto it = negative_cache_.begin();
			     it != negative_cache_.end(); ++it)
			{
				if (victim == negative_cache_.end()
				    || it->last_access < victim->last_access
				    || (it->last_access == victim->last_access
				        && it.key() < victim.key()))
				{
					victim = it;
				}
			}
			if (victim == negative_cache_.end())
				break;
			negative_cache_.erase(victim);
		}
	}

	void pruneClientHistory()
	{
		while (client_last_service_.size()
		       > config_.max_client_history_entries)
		{
			auto victim = client_last_service_.end();
			for (auto it = client_last_service_.begin();
			     it != client_last_service_.end(); ++it)
			{
				if (victim == client_last_service_.end()
				    || it.value() < victim.value()
				    || (it.value() == victim.value()
				        && it.key() < victim.key()))
				{
					victim = it;
				}
			}
			if (victim == client_last_service_.end())
				break;
			client_last_service_.erase(victim);
		}
	}

	void pruneHostBackoff()
	{
		auto const current = now();
		host_not_before_.removeIf([current](
			QHash<QString, qint64>::iterator it) {
			return it.value() <= current;
		});
		while (host_not_before_.size() > config_.max_host_backoff_entries)
		{
			auto victim = host_not_before_.end();
			for (auto it = host_not_before_.begin();
			     it != host_not_before_.end(); ++it)
			{
				if (victim == host_not_before_.end()
				    || it.value() < victim.value()
				    || (it.value() == victim.value()
				        && it.key() < victim.key()))
				{
					victim = it;
				}
			}
			if (victim == host_not_before_.end())
				break;
			host_not_before_.erase(victim);
		}
	}

	void pruneDestinationCache()
	{
		auto const current = now();
		destination_cache_.removeIf([current](
			QHash<QString, DestinationDecision>::iterator it) {
			return it->expires <= current;
		});
		while (destination_cache_.size()
		       > config_.max_destination_cache_entries)
		{
			auto victim = destination_cache_.end();
			for (auto it = destination_cache_.begin();
			     it != destination_cache_.end(); ++it)
			{
				if (victim == destination_cache_.end()
				    || it->last_access < victim->last_access
				    || (it->last_access == victim->last_access
				        && it.key() < victim.key()))
				{
					victim = it;
				}
			}
			if (victim == destination_cache_.end())
				break;
			destination_cache_.erase(victim);
		}
	}

	void pruneDestinationWaiters()
	{
		for (auto it = destination_waiters_.begin();
		     it != destination_waiters_.end();)
		{
			it.value().removeIf([this](auto const& entry) {
				return !entries_.contains(entry->token);
			});
			if (it.value().isEmpty())
			{
				if (auto const lookup = destination_lookups_.take(it.key());
				    lookup != 0)
				{
					QHostInfo::abortHostLookup(lookup);
				}
				it = destination_waiters_.erase(it);
			}
			else
			{
				++it;
			}
		}
	}

	bool destinationNeedsPreflight(
		const Entry& entry,
		bool offline) const
	{
		// AlwaysCache never opens a connection, so offline cache reads must not
		// depend on DNS being available. URL syntax/private-literal policy was
		// still enforced by validateRequest().
		if (offline)
			return false;
		auto const origin = hostKey(entry.current_url);
		if (config_.allow_private_networks
		    || config_.approved_private_origins.contains(origin))
		{
			return false;
		}
		QHostAddress literal;
		return !literal.setAddress(entry.current_url.host());
	}

	bool destinationNeedsPreflight(const Entry& entry) const
	{
		return destinationNeedsPreflight(entry, offline_->load());
	}

	bool hasResultCapacity(const Entry& entry) const
	{
		if (entry.result_slot_reserved)
			return true;
		auto const response_bytes = responseLimit(entry);
		return outstanding_results_ < config_.max_outstanding_results
		       && response_bytes
		            <= config_.max_outstanding_response_bytes
		                 - outstanding_response_bytes_;
	}

	void reserveResultCapacity(const std::shared_ptr<Entry>& entry)
	{
		if (entry->result_slot_reserved)
			return;
		Q_ASSERT(hasResultCapacity(*entry));
		entry->result_slot_reserved = true;
		entry->reserved_response_bytes = responseLimit(*entry);
		++outstanding_results_;
		outstanding_response_bytes_ += entry->reserved_response_bytes;
	}

	void releaseResultCapacity(const std::shared_ptr<Entry>& entry)
	{
		if (!entry->result_slot_reserved)
			return;
		entry->result_slot_reserved = false;
		--outstanding_results_;
		outstanding_response_bytes_ -= entry->reserved_response_bytes;
		entry->reserved_response_bytes = 0;
		Q_ASSERT(outstanding_results_ >= 0);
		Q_ASSERT(outstanding_response_bytes_ >= 0);
	}

	void acknowledgeResult(qint64 reserved_response_bytes)
	{
		Q_ASSERT(QThread::currentThread() == thread());
		--outstanding_results_;
		outstanding_response_bytes_ -= reserved_response_bytes;
		Q_ASSERT(outstanding_results_ >= 0);
		Q_ASSERT(outstanding_response_bytes_ >= 0);
		dispatch();
	}

	void enqueue(const std::shared_ptr<Entry>& entry)
	{
		if (!entries_.contains(entry->token) || entry->cancelled)
			return;
		entry->not_before = std::max(entry->not_before, now());
		queue_.push_back(entry);
		dispatch();
	}

	void destinationFailure(
		const std::shared_ptr<Entry>& entry,
		const DestinationDecision& decision)
	{
		if (!entries_.contains(entry->token))
			return;
		TileNetworkResult result;
		result.outcome = decision.transient_failure
		               ? TileNetworkResult::Outcome::TransientError
		               : TileNetworkResult::Outcome::Rejected;
		result.private_network_rejected =
			!decision.transient_failure;
		if (result.private_network_rejected)
			result.private_network_rejected_url = entry->current_url;
		result.error_string = decision.error;
		finish(entry, std::move(result));
	}

	void queueAfterDestinationCheck(const std::shared_ptr<Entry>& entry)
	{
		auto const origin = hostKey(entry->current_url);
		if (offline_->load())
		{
			// This is cache-only admission, not a reusable network decision. If
			// online mode resumes before dispatch, start() will preflight again.
			entry->validated_origin.clear();
			entry->destination_valid_until = 0;
			enqueue(entry);
			return;
		}
		if (!destinationNeedsPreflight(*entry))
		{
			entry->validated_origin = origin;
			entry->destination_valid_until =
				std::numeric_limits<qint64>::max();
			enqueue(entry);
			return;
		}

		auto cached = destination_cache_.find(origin);
		if (cached != destination_cache_.end() && cached->expires > now())
		{
			cached->last_access = nextStateAccess();
			destinationFailure(entry, *cached);
			return;
		}
		if (cached != destination_cache_.end())
			destination_cache_.erase(cached);

		auto& waiters = destination_waiters_[origin];
		waiters.push_back(entry);
		if (destination_lookups_.contains(origin))
			return;

		auto const lookup_id = std::make_shared<int>(0);
		*lookup_id = QHostInfo::lookupHost(
			entry->current_url.host(), this,
			[this, origin, lookup_id](QHostInfo info) {
				auto const current_lookup =
					destination_lookups_.constFind(origin);
				if (current_lookup == destination_lookups_.cend()
				    || *current_lookup != *lookup_id)
					return;
				destination_lookups_.remove(origin);
				auto waiters = destination_waiters_.take(origin);
				DestinationDecision decision;
				if (info.error() != QHostInfo::NoError || info.addresses().isEmpty())
				{
					decision.transient_failure = true;
					decision.error = TileNetworkManager::tr(
						"The imagery host could not be resolved.");
					decision.expires = now() + 10'000;
				}
				else
				{
						decision.allowed = std::ranges::all_of(
							info.addresses(),
							[](auto const& address) {
								return isPublicDestination(address);
							});
					if (!decision.allowed)
					{
						decision.error = TileNetworkManager::tr(
							"The imagery host resolved to a private or non-global address.");
					}
					decision.expires = now() + 5 * 60 * 1000;
				}
				auto const origin_is_approved =
					config_.allow_private_networks
					|| config_.approved_private_origins.contains(origin);
				if (!decision.allowed && !origin_is_approved)
				{
					decision.last_access = nextStateAccess();
					destination_cache_.insert(origin, decision);
					pruneDestinationCache();
				}
				for (auto const& waiter : std::as_const(waiters))
				{
					if (decision.allowed || origin_is_approved)
					{
						waiter->validated_origin = origin;
						// Keep the DNS decision close to QNAM's own resolution.
						// Long scheduler waits force another preflight.
						waiter->destination_valid_until = now() + 1000;
						enqueue(waiter);
					}
					else
						destinationFailure(waiter, decision);
				}
				dispatch();
			});
		destination_lookups_.insert(origin, *lookup_id);
	}

	bool eligible(const std::shared_ptr<Entry>& entry, qint64 current) const
	{
		if (entry->cancelled || entry->permission_revoked
		    || entry->not_before > current)
			return false;
		if (!hasResultCapacity(*entry))
			return false;
		if (active_clients_.value(entry->request.client_id)
		    >= config_.max_active_per_client)
		{
			return false;
		}
		auto const host = hostKey(entry->current_url);
		if (active_hosts_.value(host) >= config_.max_active_per_host)
			return false;
		return host_not_before_.value(host) <= current;
	}

	std::optional<int> chooseNext() const
	{
		auto const current = now();
		QHash<quint64, int> best_for_client;
		for (int index = 0; index < queue_.size(); ++index)
		{
			auto const& entry = queue_.at(index);
			if (!eligible(entry, current))
				continue;
			auto const client = entry->request.client_id;
			auto const found = best_for_client.constFind(client);
			if (found == best_for_client.cend())
			{
				best_for_client.insert(client, index);
				continue;
			}
			auto const& current_best = queue_.at(*found);
			if (isBetterEntry(
				    entry->request, entry->sequence,
				    current_best->request, current_best->sequence))
			{
				best_for_client[client] = index;
			}
		}
		if (best_for_client.isEmpty())
			return std::nullopt;

		std::optional<int> selected;
		quint64 selected_service = 0;
		quint64 selected_sequence = 0;
		for (auto const index : std::as_const(best_for_client))
		{
			auto const& entry = queue_.at(index);
			auto const service = client_last_service_.value(entry->request.client_id);
			if (!selected || service < selected_service
			    || (service == selected_service
			        && entry->sequence < selected_sequence))
			{
				selected = index;
				selected_service = service;
				selected_sequence = entry->sequence;
			}
		}
		return selected;
	}

	void dispatch()
	{
		Q_ASSERT(QThread::currentThread() == thread());
		if (shutting_down_ || !network_)
			return;
		if (wake_timer_)
			wake_timer_->stop();

		while (active_total_ < config_.max_active_total)
		{
			auto const selected = chooseNext();
			if (!selected)
				break;
			auto entry = queue_.takeAt(*selected);
			client_last_service_[entry->request.client_id] = next_service_++;
			pruneClientHistory();
			start(entry);
		}
		scheduleWake();
	}

	void scheduleWake()
	{
		if (!wake_timer_ || queue_.isEmpty()
		    || active_total_ >= config_.max_active_total)
			return;
		auto const current = now();
		auto earliest = std::numeric_limits<qint64>::max();
		for (auto const& entry : std::as_const(queue_))
		{
			auto const ready = std::max(
				entry->not_before,
				host_not_before_.value(hostKey(entry->current_url)));
			if (ready > current)
				earliest = std::min(earliest, ready);
		}
		if (earliest == std::numeric_limits<qint64>::max())
			return;
		auto const delay = int(std::clamp<qint64>(
			earliest - current, 1, 60'000));
		wake_timer_->start(delay);
	}

	void start(const std::shared_ptr<Entry>& entry)
	{
		auto const origin = hostKey(entry->current_url);
		auto const network_mode = networkModeSnapshot();
		auto const needs_preflight =
			destinationNeedsPreflight(*entry, network_mode.offline);
		if (needs_preflight
		    && (entry->validated_origin != origin
		        || entry->destination_valid_until <= now()))
		{
			queueAfterDestinationCheck(entry);
			return;
		}
		if (needs_preflight)
			entry->destination_valid_until = 0;

		auto const cache_only = network_mode.offline;
		entry->cache_only_request = cache_only;
		entry->network_mode_generation =
			network_mode.generation;
		entry->private_permission_origin.clear();
		entry->private_permission_url.clear();
		entry->private_permission_generation = 0;
		if (!config_.allow_private_networks
		    && config_.approved_private_origins.contains(origin))
		{
			auto const permission_generation =
				privateOriginGeneration(origin);
			if (permission_generation == 0)
			{
				TileNetworkResult result;
				result.outcome = TileNetworkResult::Outcome::Rejected;
				result.private_network_rejected = true;
				result.private_network_rejected_url = entry->current_url;
				result.private_network_permission_revoked = true;
				result.error_string = TileNetworkManager::tr(
					"Permission for the private imagery origin was revoked.");
				finish(entry, std::move(result));
				return;
			}
			entry->private_permission_origin = origin;
			entry->private_permission_url = entry->current_url;
			entry->private_permission_generation =
				permission_generation;
		}
		reserveResultCapacity(entry);

		auto const credentialed = !entry->request.bearer_token.isEmpty();
		if (cache_only && (!entry->request.referer.isEmpty() || credentialed))
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::OfflineMiss;
			result.error_string = TileNetworkManager::tr(
				"Credential- or Referer-dependent imagery is not stored in the offline HTTP cache.");
			finish(entry, std::move(result));
			return;
		}

		entry->body.clear();
		entry->too_large = false;
		entry->absolute_timeout = false;
		entry->authentication_rejected = false;
		entry->received_metadata = false;
		entry->active_host = hostKey(entry->current_url);
		++active_total_;
		++active_hosts_[entry->active_host];
		++active_clients_[entry->request.client_id];

		QNetworkRequest request(entry->current_url);
		request.setHeader(
			QNetworkRequest::UserAgentHeader,
			QString::fromUtf8(config_.user_agent));
		if (!entry->request.referer.isEmpty())
		{
			request.setRawHeader(
				QByteArrayLiteral("Referer"),
				entry->request.referer.toUtf8());
		}
		if (credentialed
		    && TileNetworkManager::canonicalOrigin(entry->current_url)
		         == entry->request.credential_origin)
		{
			request.setRawHeader(
				QByteArrayLiteral("Authorization"),
				QByteArrayLiteral("Bearer ") + entry->request.bearer_token);
		}
		if (entry->request.payload_kind
		    == NetworkPayloadKind::JsonDocument)
		{
			request.setRawHeader(
				QByteArrayLiteral("Accept"),
				QByteArrayLiteral(
					"application/json, application/*+json;q=0.9, "
					"application/octet-stream;q=0.5"));
			if (entry->redirects == 0)
			{
				if (!entry->request.if_none_match.isEmpty())
				{
					request.setRawHeader(
						QByteArrayLiteral("If-None-Match"),
						entry->request.if_none_match);
				}
				if (!entry->request.if_modified_since.isEmpty())
				{
					request.setRawHeader(
						QByteArrayLiteral("If-Modified-Since"),
						entry->request.if_modified_since);
				}
			}
		}
		else
		{
			request.setRawHeader(
				QByteArrayLiteral("Accept"),
				QByteArrayLiteral("image/*"));
		}
		request.setPriority(
			entry->request.priority == TileRequestPriority::Coverage
				? QNetworkRequest::HighPriority
				: entry->request.priority == TileRequestPriority::Background
					? QNetworkRequest::LowPriority
					: QNetworkRequest::NormalPriority);
		request.setAttribute(
			QNetworkRequest::RedirectPolicyAttribute,
			QNetworkRequest::ManualRedirectPolicy);
		request.setAttribute(
			QNetworkRequest::CookieLoadControlAttribute,
			QNetworkRequest::Manual);
		request.setAttribute(
			QNetworkRequest::CookieSaveControlAttribute,
			QNetworkRequest::Manual);
		request.setAttribute(
			QNetworkRequest::AuthenticationReuseAttribute,
			QNetworkRequest::Manual);
		request.setAttribute(QNetworkRequest::UseCredentialsAttribute, false);
		auto const referer_dependent = !entry->request.referer.isEmpty();
		auto const private_representation = referer_dependent || credentialed;
		request.setAttribute(
			QNetworkRequest::CacheLoadControlAttribute,
			private_representation
				? QNetworkRequest::AlwaysNetwork
					: cache_only
					? QNetworkRequest::AlwaysCache
					: entry->request.payload_kind
					    == NetworkPayloadKind::JsonDocument
						? QNetworkRequest::AlwaysNetwork
						: QNetworkRequest::PreferNetwork);
		request.setAttribute(
			QNetworkRequest::CacheSaveControlAttribute,
			!private_representation
			&& entry->request.payload_kind
			     == NetworkPayloadKind::TileImage);
		request.setMaximumRedirectsAllowed(config_.max_redirects);
		request.setTransferTimeout(config_.transfer_timeout);
		auto const response_limit = responseLimit(*entry);
		request.setDecompressedSafetyCheckThreshold(response_limit);

		auto* reply = network_->get(request);
		entry->reply = reply;
		active_replies_.insert(reply, entry);
		connect(reply, &QNetworkReply::metaDataChanged, this, [this, entry] {
			if (!entry->reply)
				return;
			entry->received_metadata = true;
			auto const length = entry->reply->header(
				QNetworkRequest::ContentLengthHeader).toLongLong();
			auto const response_limit = responseLimit(*entry);
			if (length > response_limit)
			{
				entry->too_large = true;
				entry->reply->abort();
			}
		});
		connect(reply, &QIODevice::readyRead, this, [this, entry] {
			if (!entry->reply || entry->too_large || entry->cancelled
			    || entry->offline_abort || entry->permission_revoked)
				return;
			auto chunk = entry->reply->readAll();
			auto const response_limit = responseLimit(*entry);
			if (chunk.size() > response_limit - entry->body.size())
			{
				entry->too_large = true;
				entry->body.clear();
				entry->reply->abort();
				return;
			}
			entry->body += chunk;
		});
		connect(reply, &QNetworkReply::finished, this, [this, entry] {
			replyFinished(entry);
		});
		QTimer::singleShot(
			config_.first_byte_timeout,
			reply,
			[entry] {
				if (entry->reply && entry->reply->isRunning()
				    && !entry->received_metadata)
				{
					entry->absolute_timeout = true;
					entry->reply->abort();
				}
			});
		QTimer::singleShot(
			config_.absolute_timeout,
			reply,
			[entry] {
				if (entry->reply && entry->reply->isRunning())
				{
					entry->absolute_timeout = true;
					entry->reply->abort();
				}
			});
	}

	void releaseActive(const std::shared_ptr<Entry>& entry)
	{
		auto* reply = entry->reply.data();
		if (reply)
			active_replies_.remove(reply);
		entry->reply = nullptr;
		--active_total_;
		if (--active_hosts_[entry->active_host] <= 0)
			active_hosts_.remove(entry->active_host);
		if (--active_clients_[entry->request.client_id] <= 0)
			active_clients_.remove(entry->request.client_id);
		entry->active_host.clear();
		if (reply)
			reply->deleteLater();
	}

	void replyFinished(const std::shared_ptr<Entry>& entry)
	{
		if (!entry->reply)
			return;
		auto* reply = entry->reply.data();
		if (!entry->too_large && !entry->cancelled
		    && !entry->offline_abort && !entry->permission_revoked
		    && reply->isReadable())
		{
			auto tail = reply->readAll();
			auto const response_limit = responseLimit(*entry);
			if (tail.size() > response_limit - entry->body.size())
			{
				entry->too_large = true;
				entry->body.clear();
			}
			else
			{
				entry->body += tail;
			}
		}

		auto const network_error = reply->error();
		auto const network_error_string = reply->errorString();
		auto const status = reply->attribute(
			QNetworkRequest::HttpStatusCodeAttribute).toInt();
		auto const redirect = reply->attribute(
			QNetworkRequest::RedirectionTargetAttribute).toUrl();
		auto const content_type = reply->header(
			QNetworkRequest::ContentTypeHeader).toString();
		auto const from_cache = reply->attribute(
			QNetworkRequest::SourceIsFromCacheAttribute).toBool();
		auto const retry_after = reply->rawHeader(QByteArrayLiteral("Retry-After"));
		auto const etag = reply->rawHeader(QByteArrayLiteral("ETag"));
		auto const last_modified =
			reply->rawHeader(QByteArrayLiteral("Last-Modified"));
		auto const final_url = reply->url();
		releaseActive(entry);

		auto const add_response_metadata =
			[&](TileNetworkResult& result) {
				result.final_url = final_url;
				result.etag = etag;
				result.last_modified = last_modified;
			};

		if (entry->cancelled)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Cancelled;
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}
		if (entry->permission_revoked)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Rejected;
			result.private_network_rejected = true;
			result.private_network_rejected_url =
				entry->permission_revoked_url;
			result.private_network_permission_revoked = true;
			result.error_string = TileNetworkManager::tr(
				"Permission for the private imagery origin was revoked.");
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}
		if (privatePermissionChanged(*entry))
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Rejected;
			result.private_network_rejected = true;
			result.private_network_rejected_url =
				entry->private_permission_url;
			result.private_network_permission_revoked = true;
			result.error_string = TileNetworkManager::tr(
				"Permission for the private imagery origin changed while the request was active.");
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}
		if (networkModeChanged(*entry))
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::OfflineMiss;
			result.error_string = TileNetworkManager::tr(
				"The imagery request was stopped because offline mode was enabled.");
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}
		if (entry->offline_abort)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::OfflineMiss;
			result.error_string = TileNetworkManager::tr(
				"The imagery request was stopped because offline mode was enabled.");
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}
		if (entry->too_large)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::PermanentError;
			result.error_string = TileNetworkManager::tr(
				"The imagery response exceeded the %1 MB safety limit.")
				.arg((entry->request.max_response_bytes > 0
				      ? entry->request.max_response_bytes
				      : config_.max_response_bytes)
				     / (1024 * 1024));
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}
		if (entry->authentication_rejected)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::PermanentError;
			result.error_string = TileNetworkManager::tr(
				"Imagery sources requiring HTTP authentication are not supported.");
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}

		if (!redirect.isEmpty() && status >= 300 && status < 400)
		{
			auto const next = entry->current_url.resolved(redirect)
			                 .adjusted(QUrl::RemoveFragment);
			QUrl private_network_rejected_url;
			auto error = validateHttpUrl(
				next, config_, &private_network_rejected_url);
			if (error.isEmpty()
			    && entry->current_url.scheme() == QLatin1String("https")
			    && next.scheme() == QLatin1String("http")
			    && !config_.allow_https_downgrade)
			{
				error = TileNetworkManager::tr(
					"An imagery redirect attempted to downgrade HTTPS to HTTP.");
			}
			if (error.isEmpty() && entry->redirects >= config_.max_redirects)
			{
				error = TileNetworkManager::tr(
					"The imagery server redirected too many times.");
			}
			if (!error.isEmpty())
			{
				TileNetworkResult result;
				result.outcome = private_network_rejected_url.isEmpty()
					? TileNetworkResult::Outcome::PermanentError
					: TileNetworkResult::Outcome::Rejected;
				result.private_network_rejected =
					!private_network_rejected_url.isEmpty();
				result.private_network_rejected_url =
					private_network_rejected_url;
				result.http_status = status;
				result.error_string = error;
				add_response_metadata(result);
				finish(entry, std::move(result));
				dispatch();
				return;
			}
			++entry->redirects;
			entry->current_url = next;
			entry->not_before = now();
			entry->body.clear();
			releaseResultCapacity(entry);
			queueAfterDestinationCheck(entry);
			dispatch();
			return;
		}

		if (entry->request.payload_kind
		      == NetworkPayloadKind::JsonDocument
		    && status == 304)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::NotModified;
			result.http_status = status;
			result.content_type = content_type;
			result.from_cache = from_cache;
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}

		if (entry->request.empty_http_status_codes.contains(status))
		{
			negative_cache_.insert(
				negativeCacheKey(entry->request),
				{ now() + config_.negative_cache_ttl_ms,
				  nextStateAccess() });
			pruneNegativeCache();
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::EmptyTile;
			result.http_status = status;
			result.content_type = content_type;
			result.from_cache = from_cache;
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}

		if (network_error == QNetworkReply::NoError && status >= 200 && status < 300)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Success;
			result.body = std::move(entry->body);
			result.http_status = status;
			result.content_type = content_type;
			result.from_cache = from_cache;
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}

		if (offline_->load()
		    && (network_error == QNetworkReply::ContentNotFoundError
		        || network_error == QNetworkReply::ProtocolUnknownError))
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::OfflineMiss;
			result.http_status = status;
			result.error_string = TileNetworkManager::tr(
				"The imagery tile is not available in the offline cache.");
			add_response_metadata(result);
			finish(entry, std::move(result));
			dispatch();
			return;
		}

		auto const transient = entry->absolute_timeout
		                    || isTransientNetworkError(network_error)
		                    || isTransientHttpStatus(status);
		if (transient && entry->retries < config_.max_retries)
		{
			auto delay = retryDelay(entry, retry_after);
			if (status == 429 || status == 503)
			{
				auto const host = hostKey(entry->current_url);
				host_not_before_[host] = std::max(
					host_not_before_.value(host), now() + delay);
				pruneHostBackoff();
			}
			++entry->retries;
			entry->not_before = now() + delay;
			entry->body.clear();
			releaseResultCapacity(entry);
			queueAfterDestinationCheck(entry);
			dispatch();
			return;
		}

		TileNetworkResult result;
		result.outcome = transient
		               ? TileNetworkResult::Outcome::TransientError
		               : TileNetworkResult::Outcome::PermanentError;
		result.http_status = status;
		result.content_type = content_type;
		result.from_cache = from_cache;
		add_response_metadata(result);
		result.error_string = entry->absolute_timeout
		                    ? TileNetworkManager::tr("The imagery request timed out.")
		                    : network_error_string;
		if (result.error_string.isEmpty())
		{
			result.error_string = TileNetworkManager::tr(
				"The imagery server returned HTTP status %1.").arg(status);
		}
		finish(entry, std::move(result));
		dispatch();
	}

	int retryDelay(const std::shared_ptr<Entry>& entry, const QByteArray& header) const
	{
		bool seconds_ok = false;
		auto const seconds = header.trimmed().toInt(&seconds_ok);
		qint64 delay = 0;
		if (seconds_ok && seconds >= 0)
		{
			delay = qint64(seconds) * 1000;
		}
		else if (!header.isEmpty())
		{
			auto const date = QDateTime::fromString(
				QString::fromLatin1(header), Qt::RFC2822Date);
			if (date.isValid())
				delay = QDateTime::currentDateTimeUtc().msecsTo(date.toUTC());
		}
		if (delay <= 0)
		{
			delay = qint64(config_.retry_base_delay_ms)
			        << std::min(entry->retries, 20);
			auto const jitter_percent = int(
				(entry->token * 1103515245u + quint64(entry->retries) * 12345u) % 21u) - 10;
			delay += delay * jitter_percent / 100;
		}
		return int(std::clamp<qint64>(
			delay, 1, std::max(1, config_.retry_max_delay_ms)));
	}

	void eraseQueued(const std::shared_ptr<Entry>& entry)
	{
		queue_.erase(
			std::remove_if(
				queue_.begin(), queue_.end(),
				[&entry](auto const& queued) { return queued == entry; }),
			queue_.end());
	}

	void finish(const std::shared_ptr<Entry>& entry, TileNetworkResult result)
	{
		eraseQueued(entry);
		entries_.remove(entry->token);
		pruneDestinationWaiters();
		auto client_count = client_entry_counts_.find(
			entry->request.client_id);
		if (client_count != client_entry_counts_.end()
		    && --(*client_count) <= 0)
		{
			client_entry_counts_.erase(client_count);
			client_last_service_.remove(entry->request.client_id);
		}
		auto const reserved = entry->result_slot_reserved;
		auto const reserved_response_bytes = entry->reserved_response_bytes;
		auto const guard = deliveryGuard(*entry);
		entry->result_slot_reserved = false;
		entry->reserved_response_bytes = 0;
		deliver(
			entry->token,
			entry->request,
			std::move(result),
			reserved,
			reserved_response_bytes,
			guard);
	}

	void deliver(
		Token token,
		const TileNetworkRequest& request,
		TileNetworkResult result,
		bool reserved,
		qint64 reserved_response_bytes,
		DeliveryGuard guard)
	{
		if (shutting_down_)
			return;
		result.client_id = request.client_id;
		result.generation = request.generation;
		result.user_data = request.user_data;
		auto facade = facade_;
		if (!facade)
		{
			if (reserved)
				acknowledgeResult(reserved_response_bytes);
			return;
		}
		QPointer<Worker> worker(this);
		QMetaObject::invokeMethod(
			facade,
			[facade, worker, token, result = std::move(result),
			 reserved, reserved_response_bytes,
			 guard = std::move(guard)]() mutable {
				if (facade)
				{
					if (result.outcome
					      != TileNetworkResult::Outcome::Cancelled
					    && result.outcome
					      != TileNetworkResult::Outcome::Rejected)
					{
						auto const credential_changed =
							guard.credential_generation != 0
							&& facade->credentialGeneration(
								guard.credential_origin)
							     != guard.credential_generation;
						auto const private_permission_changed =
							guard.private_permission_generation != 0
							&& facade->privateOriginGeneration(
								guard.private_permission_origin)
							     != guard.private_permission_generation;
						if (credential_changed)
						{
							result.body.clear();
							result.outcome =
								TileNetworkResult::Outcome::Cancelled;
							result.error_string = TileNetworkManager::tr(
								"The imagery account changed before the response was delivered.");
						}
						else if (private_permission_changed)
						{
							result.body.clear();
							result.outcome =
								TileNetworkResult::Outcome::Rejected;
							result.private_network_rejected = true;
							result.private_network_rejected_url =
								guard.private_permission_url;
							result.private_network_permission_revoked = true;
							result.error_string = TileNetworkManager::tr(
								"Permission for the private imagery origin changed before the response was delivered.");
						}
						else if (!guard.cache_only_request
						         && guard.network_mode_generation != 0
						         && facade->networkModeSnapshot().generation
						              != guard.network_mode_generation)
						{
							result.body.clear();
							result.outcome =
								TileNetworkResult::Outcome::OfflineMiss;
							result.error_string = TileNetworkManager::tr(
								"The imagery request was stopped because offline mode was enabled.");
						}
					}
					emit facade->finished(token, result);
				}
				if (reserved && worker)
				{
					QMetaObject::invokeMethod(
						worker,
						[worker, reserved_response_bytes] {
							if (worker)
								worker->acknowledgeResult(
									reserved_response_bytes);
						},
						Qt::QueuedConnection);
				}
			},
			Qt::QueuedConnection);
	}

	Config config_;
	QPointer<TileNetworkManager> facade_;
	std::atomic_bool* offline_ = nullptr;
	std::atomic<quint64>* network_mode_generation_ = nullptr;
	QElapsedTimer clock_;
	QNetworkAccessManager* network_ = nullptr;
	QTimer* wake_timer_ = nullptr;
	bool shutting_down_ = false;
	quint64 next_sequence_ = 1;
	quint64 next_service_ = 1;
	int active_total_ = 0;
	int outstanding_results_ = 0;
	qint64 outstanding_response_bytes_ = 0;
	QHash<Token, std::shared_ptr<Entry>> entries_;
	QVector<std::shared_ptr<Entry>> queue_;
	QHash<QNetworkReply*, std::shared_ptr<Entry>> active_replies_;
	QHash<QString, int> active_hosts_;
	QHash<quint64, int> active_clients_;
	QHash<quint64, int> client_entry_counts_;
	QHash<quint64, quint64> client_last_service_;
	QHash<QString, qint64> host_not_before_;
	QHash<QByteArray, NegativeCacheEntry> negative_cache_;
	QHash<QString, DestinationDecision> destination_cache_;
	QHash<QString, int> destination_lookups_;
	QHash<QString, QVector<std::shared_ptr<Entry>>> destination_waiters_;
	quint64 next_state_access_ = 1;
};

TileNetworkManager::TileNetworkManager(QObject* parent)
 : TileNetworkManager(Config {}, parent)
{}

TileNetworkManager::TileNetworkManager(Config config, QObject* parent)
 : QObject(parent)
 , config_(std::move(config))
{
	if (config_.cache_directory.isEmpty())
	{
		config_.cache_directory = QDir(
			QStandardPaths::writableLocation(QStandardPaths::CacheLocation))
			.filePath(QStringLiteral("online-imagery"));
	}
	if (config_.user_agent.isEmpty())
		config_.user_agent = defaultUserAgent();
	config_.max_active_total = std::max(1, config_.max_active_total);
	config_.max_active_per_host = std::max(1, config_.max_active_per_host);
	config_.max_active_per_client = std::max(1, config_.max_active_per_client);
	config_.max_pending_total = std::max(1, config_.max_pending_total);
	config_.max_pending_per_client = std::max(1, config_.max_pending_per_client);
	config_.max_negative_cache_entries =
		std::max(0, config_.max_negative_cache_entries);
	config_.max_client_history_entries =
		std::max(0, config_.max_client_history_entries);
	config_.max_host_backoff_entries =
		std::max(0, config_.max_host_backoff_entries);
	config_.max_destination_cache_entries =
		std::max(0, config_.max_destination_cache_entries);
	config_.max_redirects = std::max(0, config_.max_redirects);
	config_.max_retries = std::max(0, config_.max_retries);
	config_.negative_cache_ttl_ms =
		std::max(0, config_.negative_cache_ttl_ms);
	config_.max_response_bytes = std::max<qint64>(1, config_.max_response_bytes);
	config_.max_outstanding_results =
		std::max(1, config_.max_outstanding_results);
	config_.max_outstanding_response_bytes = std::max(
		config_.max_response_bytes,
		config_.max_outstanding_response_bytes);
	config_.disk_cache_bytes = std::max<qint64>(0, config_.disk_cache_bytes);
	approved_private_origins_ = config_.approved_private_origins;
	for (auto const& origin : std::as_const(approved_private_origins_))
	{
		auto const generation = next_private_origin_generation_++;
		if (generation == 0)
			qFatal("Imagery private-origin generation space exhausted");
		private_origin_generations_.insert(origin, generation);
	}

	qRegisterMetaType<TileNetworkResult>();
	network_thread_ = new QThread(this);
	network_thread_->setObjectName(QStringLiteral("Mapper imagery network"));
	worker_ = new Worker(
		config_, this, &offline_, &network_mode_generation_);
	worker_->moveToThread(network_thread_);
	network_thread_->start();
	QMetaObject::invokeMethod(
		worker_, [worker = worker_] { worker->initialize(); },
		Qt::BlockingQueuedConnection);
}

TileNetworkManager::~TileNetworkManager()
{
	if (!network_thread_ || !worker_)
		return;
	QMetaObject::invokeMethod(
		worker_, [worker = worker_] { worker->shutdown(); },
		Qt::BlockingQueuedConnection);
	QMetaObject::invokeMethod(worker_, &QObject::deleteLater, Qt::QueuedConnection);
	network_thread_->quit();
	network_thread_->wait();
	worker_ = nullptr;
}

Q_APPLICATION_STATIC(TileNetworkManager, application_tile_network_manager)

TileNetworkManager& TileNetworkManager::instance()
{
	auto* application = QCoreApplication::instance();
	Q_ASSERT(application);
	Q_ASSERT(QThread::currentThread() == application->thread());
	return *application_tile_network_manager;
}

quint64 TileNetworkManager::nextClientId()
{
	static std::atomic<quint64> next { 1 };
	auto const id = next.fetch_add(1);
	if (id == 0)
		qFatal("Imagery network client identity space exhausted");
	return id;
}

QString TileNetworkManager::canonicalOrigin(const QUrl& url)
{
	return hostKey(url);
}

bool TileNetworkManager::isPublicDestinationAddress(
	const QHostAddress& address)
{
	return isPublicDestination(address);
}

TileNetworkManager::Token TileNetworkManager::submit(TileNetworkRequest request)
{
	// Credentials are manager-owned. Ignore anything a catalog/runtime caller
	// attempted to place in these transport-only fields.
	request.bearer_token.clear();
	request.credential_identity.clear();
	request.credential_origin.clear();
	request.credential_generation = 0;
	{
		QMutexLocker lock(&credentials_mutex_);
		auto const origin = canonicalOrigin(request.url);
		auto const credential = bearer_credentials_.constFind(origin);
		if (credential != bearer_credentials_.cend())
		{
			request.bearer_token = credential->token;
			request.credential_identity = credential->identity;
			request.credential_origin = origin;
			request.credential_generation = credential->generation;
		}
	}
	auto const token = next_token_.fetch_add(1);
	if (token == 0)
		qFatal("Imagery network token space exhausted");
	QMetaObject::invokeMethod(
		worker_,
		[worker = worker_, token, request = std::move(request)]() mutable {
			worker->submit(token, std::move(request));
		},
		Qt::QueuedConnection);
	return token;
}

bool TileNetworkManager::setBearerCredential(const QUrl& origin, QByteArray token, QByteArray identity)
{
	auto const canonical = canonicalOrigin(origin);
	if (canonical.isEmpty() || token.isEmpty() || token.size() > 4096
	    || identity.isEmpty() || identity.size() > 128
	    || token.contains('\r') || token.contains('\n') || token.contains('\0')
	    || identity.contains('\r') || identity.contains('\n') || identity.contains('\0'))
	{
		return false;
	}
	QByteArray credential_identity = identity;
	credential_identity.append('\0');
	credential_identity.append(token);
	identity = QCryptographicHash::hash(
		credential_identity, QCryptographicHash::Sha256).toHex();
	{
		QMutexLocker lock(&credentials_mutex_);
		auto const existing = bearer_credentials_.constFind(canonical);
		if (existing != bearer_credentials_.cend()
		    && existing->token == token && existing->identity == identity)
			return true;
		auto const generation = next_credential_generation_++;
		if (generation == 0)
			qFatal("Imagery credential generation space exhausted");
		bearer_credentials_.insert(
			canonical,
			{ std::move(token), std::move(identity), generation });
		QMetaObject::invokeMethod(
			worker_,
			[worker = worker_, canonical] {
				worker->bearerCredentialChanged(canonical);
			},
			Qt::QueuedConnection);
	}
	emit bearerCredentialChanged(canonical);
	return true;
}

void TileNetworkManager::clearBearerCredential(const QUrl& origin)
{
	auto const canonical = canonicalOrigin(origin);
	{
		QMutexLocker lock(&credentials_mutex_);
		if (!bearer_credentials_.remove(canonical))
			return;
		QMetaObject::invokeMethod(
			worker_,
			[worker = worker_, canonical] {
				worker->bearerCredentialChanged(canonical);
			},
			Qt::QueuedConnection);
	}
	emit bearerCredentialChanged(canonical);
}

void TileNetworkManager::clearBearerCredentials()
{
	QStringList origins;
	{
		QMutexLocker lock(&credentials_mutex_);
		origins = bearer_credentials_.keys();
		bearer_credentials_.clear();
		for (auto const& origin : std::as_const(origins))
		{
			QMetaObject::invokeMethod(
				worker_,
				[worker = worker_, origin] {
					worker->bearerCredentialChanged(origin);
				},
				Qt::QueuedConnection);
		}
	}
	for (auto const& origin : std::as_const(origins))
		emit bearerCredentialChanged(origin);
}

void TileNetworkManager::cancel(Token token)
{
	QMetaObject::invokeMethod(
		worker_,
		[worker = worker_, token] { worker->cancel(token); },
		Qt::QueuedConnection);
}

void TileNetworkManager::cancelClient(
	quint64 client_id, quint64 through_generation)
{
	QMetaObject::invokeMethod(
		worker_,
		[worker = worker_, client_id, through_generation] {
			worker->cancelClient(client_id, through_generation);
		},
		Qt::QueuedConnection);
}

void TileNetworkManager::setOfflineMode(bool offline)
{
	{
		QMutexLocker lock(&network_mode_mutex_);
		if (offline_.load() == offline)
			return;
		auto const advance_generation = [this] {
			if (network_mode_generation_.fetch_add(1)
			    == std::numeric_limits<quint64>::max())
			{
				qFatal("Imagery network-mode generation space exhausted");
			}
		};
		// A worker which observes the transition without taking this mutex must
		// see either the old online mode or a cache-only state. The generation
		// makes every already-active online request stale.
		if (offline)
		{
			offline_.store(true);
			advance_generation();
		}
		else
		{
			advance_generation();
			offline_.store(false);
		}
		// Queue under the same mutex so concurrent callers cannot reorder the
		// worker's transition callbacks after publishing facade state.
		QMetaObject::invokeMethod(
			worker_,
			[worker = worker_, offline] {
				worker->setOfflineMode(offline);
			},
			Qt::QueuedConnection);
	}
	if (QThread::currentThread() == thread())
	{
		emit offlineModeChanged(offline);
	}
	else
	{
		QPointer<TileNetworkManager> self(this);
		QMetaObject::invokeMethod(
			this,
			[self, offline] {
				if (self)
					emit self->offlineModeChanged(offline);
			},
			Qt::QueuedConnection);
	}
}

bool TileNetworkManager::offlineMode() const noexcept
{
	return offline_.load();
}

TileNetworkManager::NetworkModeSnapshot
TileNetworkManager::networkModeSnapshot() const
{
	QMutexLocker lock(&network_mode_mutex_);
	return {
		offline_.load(),
		network_mode_generation_.load(),
	};
}

bool TileNetworkManager::approvePrivateOrigin(
	const QUrl& url)
{
	auto const scheme = url.scheme().toLower();
	if (!url.isValid() || url.isRelative() || url.host().isEmpty()
	    || !url.userInfo().isEmpty()
	    || (scheme != QLatin1String("http")
	        && scheme != QLatin1String("https")))
		return false;
	auto const origin = canonicalOrigin(url);
	{
		QMutexLocker lock(&permissions_mutex_);
		if (approved_private_origins_.contains(origin))
			return true;
		approved_private_origins_.insert(origin);
		auto const generation = next_private_origin_generation_++;
		if (generation == 0)
			qFatal("Imagery private-origin generation space exhausted");
		private_origin_generations_.insert(origin, generation);
		// Preserve mutation order when approvals are changed concurrently.
		QMetaObject::invokeMethod(
			worker_,
			[worker = worker_, origin] {
				worker->setPrivateOriginApproved(origin, true);
			},
			Qt::QueuedConnection);
	}
	QPointer<TileNetworkManager> self(this);
	QMetaObject::invokeMethod(
		this,
		[self, origin] {
			if (self)
				emit self->privateOriginApprovalChanged(origin, true);
		},
		Qt::QueuedConnection);
	return true;
}

bool TileNetworkManager::revokePrivateOrigin(
	const QUrl& url)
{
	auto const origin = canonicalOrigin(url);
	{
		QMutexLocker lock(&permissions_mutex_);
		if (!approved_private_origins_.remove(origin))
			return false;
		private_origin_generations_.remove(origin);
		// Preserve mutation order when approvals are changed concurrently.
		QMetaObject::invokeMethod(
			worker_,
			[worker = worker_, origin] {
				worker->setPrivateOriginApproved(origin, false);
			},
			Qt::QueuedConnection);
	}
	QPointer<TileNetworkManager> self(this);
	QMetaObject::invokeMethod(
		this,
		[self, origin] {
			if (self)
				emit self->privateOriginApprovalChanged(origin, false);
		},
		Qt::QueuedConnection);
	return true;
}

bool TileNetworkManager::isPrivateOriginApproved(
	const QUrl& url) const
{
	QMutexLocker lock(&permissions_mutex_);
	return approved_private_origins_.contains(
		canonicalOrigin(url));
}

quint64 TileNetworkManager::privateOriginGeneration(
	const QString& origin) const
{
	QMutexLocker lock(&permissions_mutex_);
	return private_origin_generations_.value(origin);
}

quint64 TileNetworkManager::credentialGeneration(const QString& origin) const
{
	QMutexLocker lock(&credentials_mutex_);
	auto const credential = bearer_credentials_.constFind(origin);
	return credential == bearer_credentials_.cend()
	       ? 0
	       : credential->generation;
}

}  // namespace OpenOrienteering::imagery
