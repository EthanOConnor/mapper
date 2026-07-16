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

#include "templates/raster_resource_manager.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <exception>
#include <limits>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

#include <QDebug>
#include <QMetaObject>
#include <QPointer>
#include <QThread>
#include <QThreadPool>

namespace OpenOrienteering {

struct RasterResourceManager::CancellationToken::OwnerState
{
	std::uint64_t id = 0;
	std::atomic<std::uint64_t> generation { 1 };
	std::atomic_bool retired { false };
	std::array<int, 2> concurrency_limit { 1, 1 };
	std::array<int, 2> active {};
};

struct RasterResourceManager::Owner::SharedState
 : std::enable_shared_from_this<RasterResourceManager::Owner::SharedState>
{
	struct Job
	{
		std::shared_ptr<OwnerState> owner;
		std::uint64_t generation = 0;
		Lane lane = Lane::BlockingIo;
		Priority priority = Priority::Background;
		QPointer<QObject> receiver;
		Work work;
		std::uint64_t sequence = 0;
	};

	struct LaneState
	{
		explicit LaneState(int thread_limit)
		{
			pool.setMaxThreadCount(std::max(1, thread_limit));
			pool.setExpiryTimeout(30'000);
		}

		QThreadPool pool;
		std::vector<std::shared_ptr<Job>> pending;
		std::uint64_t last_owner = 0;
		int active = 0;
	};

	SharedState(RasterResourceManager* context, Limits limits)
	 : context(context)
	 , blocking(limits.blocking_io_threads)
	 , decode(limits.decode_threads)
	 , max_pending_per_owner(std::max<std::size_t>(
		1, limits.max_pending_per_owner
	 ))
	 , max_pending_per_lane(std::max<std::size_t>(
		max_pending_per_owner, limits.max_pending_per_lane
	 ))
	{}

	LaneState& laneState(Lane lane)
	{
		return lane == Lane::BlockingIo ? blocking : decode;
	}

	const LaneState& laneState(Lane lane) const
	{
		return lane == Lane::BlockingIo ? blocking : decode;
	}

	static std::size_t laneIndex(Lane lane)
	{
		return static_cast<std::size_t>(lane);
	}

	bool jobIsCurrent(const Job& job) const noexcept
	{
		return !job.owner->retired.load(std::memory_order_relaxed)
		       && job.owner->generation.load(std::memory_order_relaxed)
		            == job.generation;
	}

	void purgeInvalidLocked(LaneState& lane)
	{
		std::erase_if(lane.pending, [this](auto const& job) {
			return !jobIsCurrent(*job);
		});
	}

	std::vector<std::shared_ptr<Job>>::iterator chooseNextLocked(
		Lane lane_id,
		LaneState& lane)
	{
		purgeInvalidLocked(lane);
		auto const lane_index = laneIndex(lane_id);
		std::vector<std::uint64_t> owners;
		for (auto const& job : lane.pending)
		{
			if (job->owner->active[lane_index]
			    < job->owner->concurrency_limit[lane_index])
			{
				owners.push_back(job->owner->id);
			}
		}
		if (owners.empty())
			return lane.pending.end();

		std::ranges::sort(owners);
		owners.erase(std::unique(owners.begin(), owners.end()), owners.end());
		auto owner = std::ranges::upper_bound(owners, lane.last_owner);
		if (owner == owners.end())
			owner = owners.begin();

		auto selected = lane.pending.end();
		for (auto it = lane.pending.begin(); it != lane.pending.end(); ++it)
		{
			if ((*it)->owner->id != *owner
			    || (*it)->owner->active[lane_index]
			         >= (*it)->owner->concurrency_limit[lane_index])
			{
				continue;
			}
			if (selected == lane.pending.end()
			    || (*it)->priority < (*selected)->priority
			    || ((*it)->priority == (*selected)->priority
			        && (*it)->sequence < (*selected)->sequence))
			{
				selected = it;
			}
		}
		if (selected != lane.pending.end())
			lane.last_owner = *owner;
		return selected;
	}

	void dispatchLocked(Lane lane_id)
	{
		auto& lane = laneState(lane_id);
		while (!shutting_down
		       && lane.active < lane.pool.maxThreadCount())
		{
			auto selected = chooseNextLocked(lane_id, lane);
			if (selected == lane.pending.end())
				break;

			auto job = std::move(*selected);
			lane.pending.erase(selected);
			++lane.active;
			++job->owner->active[laneIndex(lane_id)];
			auto self = shared_from_this();
			lane.pool.start([self = std::move(self), job = std::move(job)] {
				Completion completion;
				try
				{
					completion = job->work(CancellationToken {
						job->owner, job->generation
					});
				}
				catch (const std::exception& error)
				{
					qWarning() << "Raster worker failed:" << error.what();
				}
				catch (...)
				{
					qWarning() << "Raster worker failed with an unknown exception";
				}
				self->finish(std::move(job), std::move(completion));
			});
		}
	}

	void finish(std::shared_ptr<Job> job, Completion completion)
	{
		bool deliver = false;
		{
			std::lock_guard lock(mutex);
			auto& lane = laneState(job->lane);
			--lane.active;
			--job->owner->active[laneIndex(job->lane)];
			deliver = !shutting_down && completion && jobIsCurrent(*job);
			dispatchLocked(Lane::BlockingIo);
			dispatchLocked(Lane::Decode);
		}

		if (!deliver)
			return;

		auto self = shared_from_this();
		QMetaObject::invokeMethod(
			context,
			[self = std::move(self), job = std::move(job),
			 completion = std::move(completion)]() mutable {
				{
					std::lock_guard lock(self->mutex);
					if (self->shutting_down || !self->jobIsCurrent(*job)
					    || !job->receiver)
					{
						return;
					}
				}
				completion();
			},
			Qt::QueuedConnection
		);
	}

	void invalidate(const std::shared_ptr<OwnerState>& owner, bool retire)
	{
		if (!owner)
			return;
		if (retire)
			owner->retired.store(true, std::memory_order_relaxed);
		owner->generation.fetch_add(1, std::memory_order_relaxed);

		std::lock_guard lock(mutex);
		if (retire)
			owners.erase(owner->id);
		purgeInvalidLocked(blocking);
		purgeInvalidLocked(decode);
		dispatchLocked(Lane::BlockingIo);
		dispatchLocked(Lane::Decode);
	}

	void setConcurrencyLimit(
		const std::shared_ptr<OwnerState>& owner,
		Lane lane,
		int limit)
	{
		if (!owner)
			return;
		std::lock_guard lock(mutex);
		owner->concurrency_limit[laneIndex(lane)] = std::max(1, limit);
		dispatchLocked(lane);
	}

	int concurrencyLimit(
		const std::shared_ptr<OwnerState>& owner,
		Lane lane) const
	{
		if (!owner)
			return 0;
		std::lock_guard lock(mutex);
		return owner->concurrency_limit[laneIndex(lane)];
	}

	RasterResourceManager* context = nullptr;
	mutable std::mutex mutex;
	LaneState blocking;
	LaneState decode;
	std::unordered_map<std::uint64_t, std::weak_ptr<OwnerState>> owners;
	std::uint64_t next_owner_id = 1;
	std::uint64_t next_sequence = 1;
	std::size_t max_pending_per_owner = 128;
	std::size_t max_pending_per_lane = 2048;
	bool shutting_down = false;
};

RasterResourceManager::CancellationToken::CancellationToken(
	std::weak_ptr<OwnerState> owner,
	std::uint64_t generation) noexcept
 : owner_(std::move(owner))
 , generation_(generation)
{}

bool RasterResourceManager::CancellationToken::isCancelled() const noexcept
{
	auto const owner = owner_.lock();
	return !owner
	       || owner->retired.load(std::memory_order_relaxed)
	       || owner->generation.load(std::memory_order_relaxed) != generation_;
}

RasterResourceManager::Owner::Owner(
	std::weak_ptr<SharedState> manager,
	std::shared_ptr<OwnerState> state) noexcept
 : manager_(std::move(manager))
 , state_(std::move(state))
{}

RasterResourceManager::Owner::Owner(Owner&& other) noexcept
 : manager_(std::move(other.manager_))
 , state_(std::move(other.state_))
{}

RasterResourceManager::Owner& RasterResourceManager::Owner::operator=(
	Owner&& other) noexcept
{
	if (this != &other)
	{
		retire();
		manager_ = std::move(other.manager_);
		state_ = std::move(other.state_);
	}
	return *this;
}

RasterResourceManager::Owner::~Owner()
{
	retire();
}

bool RasterResourceManager::Owner::isValid() const noexcept
{
	return state_ && !state_->retired.load(std::memory_order_relaxed);
}

void RasterResourceManager::Owner::invalidate()
{
	if (auto manager = manager_.lock())
		manager->invalidate(state_, false);
}

void RasterResourceManager::Owner::setConcurrencyLimit(Lane lane, int limit)
{
	if (auto manager = manager_.lock())
		manager->setConcurrencyLimit(state_, lane, limit);
}

int RasterResourceManager::Owner::concurrencyLimit(Lane lane) const
{
	if (auto manager = manager_.lock())
		return manager->concurrencyLimit(state_, lane);
	return 0;
}

std::uint64_t RasterResourceManager::Owner::generation() const noexcept
{
	return state_ ? state_->generation.load(std::memory_order_relaxed) : 0;
}

void RasterResourceManager::Owner::retire()
{
	if (!state_)
		return;
	if (auto manager = manager_.lock())
		manager->invalidate(state_, true);
	else
		state_->retired.store(true, std::memory_order_relaxed);
	state_.reset();
	manager_.reset();
}

RasterResourceManager::Limits RasterResourceManager::defaultLimits()
{
	auto const ideal = std::max(1, QThread::idealThreadCount());
#ifdef Q_OS_ANDROID
	return { std::min(2, ideal), std::min(2, ideal) };
#else
	return {
		std::clamp(ideal / 2, 1, 4),
		std::clamp(ideal - 1, 1, 4),
	};
#endif
}

RasterResourceManager& RasterResourceManager::instance()
{
	static RasterResourceManager manager;
	return manager;
}

RasterResourceManager::RasterResourceManager(Limits limits, QObject* parent)
 : QObject(parent)
{
	if (limits.blocking_io_threads <= 0 || limits.decode_threads <= 0)
		limits = defaultLimits();
	state_ = std::make_shared<SharedState>(this, limits);
}

RasterResourceManager::~RasterResourceManager()
{
	auto state = std::move(state_);
	if (!state)
		return;
	{
		std::lock_guard lock(state->mutex);
		state->shutting_down = true;
		for (auto const& [id, weak_owner] : state->owners)
		{
			Q_UNUSED(id)
			if (auto owner = weak_owner.lock())
			{
				owner->retired.store(true, std::memory_order_relaxed);
				owner->generation.fetch_add(1, std::memory_order_relaxed);
			}
		}
		state->blocking.pending.clear();
		state->decode.pending.clear();
	}
	state->blocking.pool.clear();
	state->decode.pool.clear();
	state->blocking.pool.waitForDone();
	state->decode.pool.waitForDone();
}

RasterResourceManager::Owner RasterResourceManager::createOwner(
	int concurrency_limit)
{
	Q_ASSERT(QThread::currentThread() == thread());
	auto owner = std::make_shared<CancellationToken::OwnerState>();
	{
		std::lock_guard lock(state_->mutex);
		if (state_->next_owner_id == std::numeric_limits<std::uint64_t>::max())
			qFatal("Raster resource owner id space exhausted");
		owner->id = state_->next_owner_id++;
		owner->concurrency_limit.fill(std::max(1, concurrency_limit));
		state_->owners[owner->id] = owner;
	}
	return Owner { state_, std::move(owner) };
}

bool RasterResourceManager::submit(
	const Owner& owner,
	Lane lane,
	Priority priority,
	QObject* receiver,
	Work work)
{
	Q_ASSERT(QThread::currentThread() == thread());
	Q_ASSERT(!receiver || receiver->thread() == thread());
	if (owner.manager_.lock() != state_
	    || !owner.isValid() || !receiver || !work)
		return false;

	auto job = std::make_shared<SharedState::Job>();
	job->owner = owner.state_;
	job->generation = owner.generation();
	job->lane = lane;
	job->priority = priority;
	job->receiver = receiver;
	job->work = std::move(work);
	{
		std::lock_guard lock(state_->mutex);
		if (state_->shutting_down || !state_->jobIsCurrent(*job))
			return false;
		auto& lane_state = state_->laneState(lane);
		auto const owner_pending = std::ranges::count_if(
			lane_state.pending,
			[&owner](auto const& pending) {
				return pending->owner == owner.state_;
			}
		);
		if (lane_state.pending.size() >= state_->max_pending_per_lane
		    || std::size_t(owner_pending) >= state_->max_pending_per_owner)
		{
			return false;
		}
		if (state_->next_sequence == std::numeric_limits<std::uint64_t>::max())
			qFatal("Raster resource job sequence space exhausted");
		job->sequence = state_->next_sequence++;
		lane_state.pending.push_back(std::move(job));
		state_->dispatchLocked(lane);
	}
	return true;
}

int RasterResourceManager::threadLimit(Lane lane) const
{
	std::lock_guard lock(state_->mutex);
	return state_->laneState(lane).pool.maxThreadCount();
}

int RasterResourceManager::activeCount(Lane lane) const
{
	std::lock_guard lock(state_->mutex);
	return state_->laneState(lane).active;
}

std::size_t RasterResourceManager::pendingCount(Lane lane) const
{
	std::lock_guard lock(state_->mutex);
	return state_->laneState(lane).pending.size();
}

}  // namespace OpenOrienteering
