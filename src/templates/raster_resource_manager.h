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

#ifndef OPENORIENTEERING_RASTER_RESOURCE_MANAGER_H
#define OPENORIENTEERING_RASTER_RESOURCE_MANAGER_H

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

#include <QObject>

namespace OpenOrienteering {

/**
 * Application-wide execution policy for asynchronous raster work.
 *
 * Sources continue to own demand, typed caches, and retry policy. The manager
 * owns only bounded execution, fair ordering, cancellation generations, and
 * receiver-safe delivery back to the application thread.
 */
class RasterResourceManager final : public QObject
{
public:
	enum class Lane : std::uint8_t
	{
		BlockingIo,
		Decode,
	};

	enum class Priority : std::uint8_t
	{
		Coverage,
		Visible,
		Background,
	};

	struct Limits
	{
		int blocking_io_threads = 0;
		int decode_threads = 0;
		std::size_t max_pending_per_owner = 128;
		std::size_t max_pending_per_lane = 2048;
	};

	class CancellationToken
	{
	public:
		bool isCancelled() const noexcept;

	private:
		friend class RasterResourceManager;
		struct OwnerState;

		CancellationToken(std::weak_ptr<OwnerState> owner,
		                  std::uint64_t generation) noexcept;

		std::weak_ptr<OwnerState> owner_;
		std::uint64_t generation_ = 0;
	};

	class Owner
	{
	public:
		Owner() = default;
		Owner(Owner&& other) noexcept;
		Owner& operator=(Owner&& other) noexcept;
		~Owner();

		Owner(const Owner&) = delete;
		Owner& operator=(const Owner&) = delete;

		bool isValid() const noexcept;
		void invalidate();
		void setConcurrencyLimit(Lane lane, int limit);
		int concurrencyLimit(Lane lane) const;
		std::uint64_t generation() const noexcept;

	private:
		friend class RasterResourceManager;
		using OwnerState = CancellationToken::OwnerState;
		struct SharedState;

		Owner(std::weak_ptr<SharedState> manager,
		      std::shared_ptr<OwnerState> state) noexcept;
		void retire();

		std::weak_ptr<SharedState> manager_;
		std::shared_ptr<OwnerState> state_;
	};

	using Completion = std::function<void()>;
	using Work = std::function<Completion(const CancellationToken&)>;

	static Limits defaultLimits();
	static RasterResourceManager& instance();

	explicit RasterResourceManager(
		Limits limits = defaultLimits(),
		QObject* parent = nullptr
	);
	~RasterResourceManager() override;

	RasterResourceManager(const RasterResourceManager&) = delete;
	RasterResourceManager& operator=(const RasterResourceManager&) = delete;

	Owner createOwner(int concurrency_limit = 1);

	/**
	 * Queues work for a source owner.
	 *
	 * The work runs on the selected lane. Its returned completion runs on this
	 * manager's thread only when the owner generation is still current and the
	 * receiver still exists. The receiver must share the manager's thread.
	 *
	 * Work may outlive both owner invalidation and receiver destruction. It
	 * must capture only thread-safe values or shared immutable backend state;
	 * it must never dereference the owner or receiver. Only the returned
	 * completion may access receiver-owned state. Captured values must also be
	 * safe to destroy on a worker thread when work is canceled or stale.
	 */
	bool submit(const Owner& owner,
	            Lane lane,
	            Priority priority,
	            QObject* receiver,
	            Work work);

	int threadLimit(Lane lane) const;
	int activeCount(Lane lane) const;
	std::size_t pendingCount(Lane lane) const;

private:
	using SharedState = Owner::SharedState;
	std::shared_ptr<SharedState> state_;
};

}  // namespace OpenOrienteering

#endif
