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
#include <cmath>
#include <memory>
#include <optional>
#include <utility>

#include <QAuthenticator>
#include <QApplicationStatic>
#include <QCoreApplication>
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
	return url.scheme().toLower() + QLatin1String("://")
	       + QString::fromLatin1(QUrl::toAce(url.host()).toLower())
	       + QLatin1Char(':') + QString::number(url.port(default_port));
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
	const TileNetworkManager::Config& config)
{
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
	auto const port = url.port();
	if (port == 0 || port > 65535)
		return TileNetworkManager::tr("The imagery URL has an invalid port.");

	if (config.allow_private_networks
	    || config.approved_private_origins.contains(hostKey(url)))
		return {};

	QHostAddress address;
	if (address.setAddress(url.host()))
	{
		if (address.isNull() || address.isLoopback() || address.isLinkLocal()
		    || address.isMulticast() || address.isPrivateUse())
		{
			return TileNetworkManager::tr(
				"Private, local, and link-local imagery hosts require explicit permission.");
		}
	}
	else if (isLocalHostname(url.host()))
	{
		return TileNetworkManager::tr(
			"Private, local, and link-local imagery hosts require explicit permission.");
	}
	return {};
}

QString validateRequest(
	const TileNetworkRequest& request,
	const TileNetworkManager::Config& config)
{
	if (request.client_id == 0)
		return TileNetworkManager::tr("The imagery request has no client identity.");
	if (!std::isfinite(request.distance_priority))
		return TileNetworkManager::tr("The imagery request priority is invalid.");
	if (auto const error = validateHttpUrl(request.url, config);
	    !error.isEmpty())
	{
		return error;
	}
	if (!request.referer.isEmpty())
	{
		auto const referer = QUrl(request.referer);
		if (auto const error = validateHttpUrl(referer, config);
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
	       std::atomic_bool* offline)
	 : config_(std::move(config))
	 , facade_(std::move(facade))
	 , offline_(offline)
	{}

	void initialize()
	{
		Q_ASSERT(QThread::currentThread() == thread());
		clock_.start();
		wake_timer_ = new QTimer(this);
		wake_timer_->setSingleShot(true);
		connect(wake_timer_, &QTimer::timeout, this, [this] { dispatch(); });

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
		active_total_ = 0;
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

		if (auto const error = validateRequest(request, config_);
		    !error.isEmpty())
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Rejected;
			result.error_string = error;
			deliver(token, request, std::move(result));
			return;
		}

		auto url = request.url;
		auto const negative = negative_cache_.constFind(url);
		if (negative != negative_cache_.cend())
		{
			if (*negative > now())
			{
				TileNetworkResult result;
				result.outcome = TileNetworkResult::Outcome::EmptyTile;
				result.from_cache = true;
				deliver(token, request, std::move(result));
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
			result.outcome = TileNetworkResult::Outcome::Rejected;
			result.error_string = TileNetworkManager::tr(
				"The imagery request queue is full.");
			deliver(token, request, std::move(result));
			return;
		}

		auto entry = std::make_shared<Entry>();
		entry->token = token;
		entry->request = std::move(request);
		entry->current_url = std::move(url);
		entry->sequence = next_sequence_++;
		entries_.insert(token, entry);
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
	};

	struct DestinationDecision
	{
		bool allowed = false;
		bool transient_failure = false;
		QString error;
		qint64 expires = 0;
	};

	qint64 now() const
	{
		return clock_.elapsed();
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
		result.error_string = decision.error;
		finish(entry, std::move(result));
	}

	void queueAfterDestinationCheck(const std::shared_ptr<Entry>& entry)
	{
		auto const origin = hostKey(entry->current_url);
		if (config_.allow_private_networks
		    || config_.approved_private_origins.contains(origin))
		{
			enqueue(entry);
			return;
		}

		QHostAddress literal;
		if (literal.setAddress(entry->current_url.host()))
		{
			// validateHttpUrl() already rejected non-global literals.
			enqueue(entry);
			return;
		}

		auto const cached = destination_cache_.constFind(origin);
		if (cached != destination_cache_.cend() && cached->expires > now())
		{
			if (cached->allowed)
				enqueue(entry);
			else
				destinationFailure(entry, *cached);
			return;
		}
		if (cached != destination_cache_.cend())
			destination_cache_.erase(cached);

		auto& waiters = destination_waiters_[origin];
		waiters.push_back(entry);
		if (destination_lookups_.contains(origin))
			return;

		auto const lookup_id = QHostInfo::lookupHost(
			entry->current_url.host(), this,
			[this, origin](QHostInfo info) {
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
						[](auto const& address) { return address.isGlobal(); });
					if (!decision.allowed)
					{
						decision.error = TileNetworkManager::tr(
							"The imagery host resolved to a private or non-global address.");
					}
					decision.expires = now() + 5 * 60 * 1000;
				}
				destination_cache_.insert(origin, decision);
				for (auto const& waiter : std::as_const(waiters))
				{
					if (decision.allowed)
						enqueue(waiter);
					else
						destinationFailure(waiter, decision);
				}
				dispatch();
			});
		destination_lookups_.insert(origin, lookup_id);
	}

	bool eligible(const std::shared_ptr<Entry>& entry, qint64 current) const
	{
		if (entry->cancelled || entry->not_before > current)
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
		if (offline_->load() && !entry->request.referer.isEmpty())
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::OfflineMiss;
			result.error_string = TileNetworkManager::tr(
				"Referer-dependent imagery is not stored in the offline HTTP cache.");
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
		request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("image/*"));
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
		request.setAttribute(
			QNetworkRequest::CacheLoadControlAttribute,
			referer_dependent
				? QNetworkRequest::AlwaysNetwork
				: offline_->load()
					? QNetworkRequest::AlwaysCache
					: QNetworkRequest::PreferNetwork);
		request.setAttribute(
			QNetworkRequest::CacheSaveControlAttribute,
			!referer_dependent);
		request.setMaximumRedirectsAllowed(config_.max_redirects);
		request.setTransferTimeout(config_.transfer_timeout);
		request.setDecompressedSafetyCheckThreshold(config_.max_response_bytes);

		auto* reply = network_->get(request);
		entry->reply = reply;
		active_replies_.insert(reply, entry);
		connect(reply, &QNetworkReply::metaDataChanged, this, [this, entry] {
			if (!entry->reply)
				return;
			entry->received_metadata = true;
			auto const length = entry->reply->header(
				QNetworkRequest::ContentLengthHeader).toLongLong();
			if (length > config_.max_response_bytes)
			{
				entry->too_large = true;
				entry->reply->abort();
			}
		});
		connect(reply, &QIODevice::readyRead, this, [this, entry] {
			if (!entry->reply || entry->too_large)
				return;
			auto chunk = entry->reply->readAll();
			if (chunk.size() > config_.max_response_bytes - entry->body.size())
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
		if (!entry->too_large)
		{
			auto tail = reply->readAll();
			if (tail.size() > config_.max_response_bytes - entry->body.size())
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
		releaseActive(entry);

		if (entry->cancelled)
		{
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::Cancelled;
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
				.arg(config_.max_response_bytes / (1024 * 1024));
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
			finish(entry, std::move(result));
			dispatch();
			return;
		}

		if (!redirect.isEmpty() && status >= 300 && status < 400)
		{
			auto const next = entry->current_url.resolved(redirect)
			                 .adjusted(QUrl::RemoveFragment);
			auto error = validateHttpUrl(next, config_);
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
				result.outcome = TileNetworkResult::Outcome::PermanentError;
				result.http_status = status;
				result.error_string = error;
				finish(entry, std::move(result));
				dispatch();
				return;
			}
			++entry->redirects;
			entry->current_url = next;
			entry->not_before = now();
			queueAfterDestinationCheck(entry);
			return;
		}

		if (entry->request.empty_http_status_codes.contains(status))
		{
			negative_cache_.insert(
				entry->request.url.adjusted(QUrl::RemoveFragment),
				now() + config_.negative_cache_ttl_ms);
			TileNetworkResult result;
			result.outcome = TileNetworkResult::Outcome::EmptyTile;
			result.http_status = status;
			result.content_type = content_type;
			result.from_cache = from_cache;
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
			}
			++entry->retries;
			entry->not_before = now() + delay;
			enqueue(entry);
			return;
		}

		TileNetworkResult result;
		result.outcome = transient
		               ? TileNetworkResult::Outcome::TransientError
		               : TileNetworkResult::Outcome::PermanentError;
		result.http_status = status;
		result.content_type = content_type;
		result.from_cache = from_cache;
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
		deliver(entry->token, entry->request, std::move(result));
	}

	void deliver(Token token, const TileNetworkRequest& request, TileNetworkResult result)
	{
		if (shutting_down_)
			return;
		result.client_id = request.client_id;
		result.generation = request.generation;
		result.user_data = request.user_data;
		auto facade = facade_;
		if (!facade)
			return;
		QMetaObject::invokeMethod(
			facade,
			[facade, token, result = std::move(result)] {
				if (facade)
					emit facade->finished(token, result);
			},
			Qt::QueuedConnection);
	}

	Config config_;
	QPointer<TileNetworkManager> facade_;
	std::atomic_bool* offline_ = nullptr;
	QElapsedTimer clock_;
	QNetworkAccessManager* network_ = nullptr;
	QTimer* wake_timer_ = nullptr;
	bool shutting_down_ = false;
	quint64 next_sequence_ = 1;
	quint64 next_service_ = 1;
	int active_total_ = 0;
	QHash<Token, std::shared_ptr<Entry>> entries_;
	QVector<std::shared_ptr<Entry>> queue_;
	QHash<QNetworkReply*, std::shared_ptr<Entry>> active_replies_;
	QHash<QString, int> active_hosts_;
	QHash<quint64, int> active_clients_;
	QHash<quint64, quint64> client_last_service_;
	QHash<QString, qint64> host_not_before_;
	QHash<QUrl, qint64> negative_cache_;
	QHash<QString, DestinationDecision> destination_cache_;
	QHash<QString, int> destination_lookups_;
	QHash<QString, QVector<std::shared_ptr<Entry>>> destination_waiters_;
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
	config_.max_redirects = std::max(0, config_.max_redirects);
	config_.max_retries = std::max(0, config_.max_retries);
	config_.max_response_bytes = std::max<qint64>(1, config_.max_response_bytes);
	config_.disk_cache_bytes = std::max<qint64>(0, config_.disk_cache_bytes);

	qRegisterMetaType<TileNetworkResult>();
	network_thread_ = new QThread(this);
	network_thread_->setObjectName(QStringLiteral("Mapper imagery network"));
	worker_ = new Worker(config_, this, &offline_);
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

TileNetworkManager::Token TileNetworkManager::submit(TileNetworkRequest request)
{
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
	offline_.store(offline);
}

bool TileNetworkManager::offlineMode() const noexcept
{
	return offline_.load();
}

}  // namespace OpenOrienteering::imagery
