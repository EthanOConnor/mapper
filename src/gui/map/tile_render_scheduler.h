/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 *
 *    OpenOrienteering is free software: you can redistribute it and/or modify
 *    it under the terms of the GNU General Public License as published by
 *    the Free Software Foundation, either version 3 of the License, or
 *    (at your option) any later version.
 *
 *    OpenOrienteering is distributed in the hope that it will be useful,
 *    but WITHOUT ANY WARRANTY; without even the implied warranty of
 *    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *    GNU General Public License for more details.
 *
 *    You should have received a copy of the GNU General Public License
 *    along with OpenOrienteering.  If not, see <http://www.gnu.org/licenses/>.
 */


#ifndef OPENORIENTEERING_TILE_RENDER_SCHEDULER_H
#define OPENORIENTEERING_TILE_RENDER_SCHEDULER_H

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <thread>
#include <vector>

#include <QImage>
#include <QObject>
#include <QRectF>
#include <QTransform>

#include "backing_store.h"

namespace OpenOrienteering {


struct TileRenderSnapshot;

/**
 * The function type used by worker threads to render tile content.
 * Must be thread-safe (read-only access to shared data).
 */
using TileRenderFunction = std::function<void(QPainter& painter,
                                              const QRectF& tile_map_rect,
                                              const TileRenderSnapshot& snapshot)>;

/**
 * Snapshot of the view state needed by worker threads to render tiles.
 * Captured on the main thread before dispatching jobs, so workers
 * never read live MapView state.
 */
struct TileRenderSnapshot
{
	QTransform world_transform;
	double zoom_factor;
	bool antialiasing;
	bool overprinting;
	bool grid_visible;
	int generation;
	TileRenderFunction render_func;
};


/**
 * A single tile render job for a worker thread.
 */
struct TileRenderJob
{
	TileKey key;
	QRectF tile_view_rect;    // view-space rect this tile covers
	QRectF tile_map_rect;     // map-space rect this tile covers
	QImage image;             // the QImage to paint into (detached)
	int generation;           // generation counter for cancellation
};


/**
 * A completed tile render result.
 */
struct TileRenderResult
{
	TileKey key;
	QImage image;
	int generation;
};


/**
 * Schedules tile rendering on background threads.
 *
 * The main thread dispatches TileRenderJobs. Worker threads render
 * into QImages (which is thread-safe). Completed tiles are collected
 * by the main thread and installed into the BackingStore.
 *
 * Thread count: max(1, hardware_concurrency - 2).
 *
 * Cancellation: a monotonic generation counter. When the view changes,
 * the generation increments. Workers check the generation before
 * committing results; stale results are discarded.
 */
class TileRenderScheduler : public QObject
{
	Q_OBJECT

public:
	explicit TileRenderScheduler(QObject* parent = nullptr);
	~TileRenderScheduler() override;

	/** Returns the current generation counter. */
	int generation() const { return gen.load(std::memory_order_relaxed); }

	/** Increments the generation counter, effectively cancelling all
	 *  in-flight jobs from the previous generation. */
	void cancelPending();

	/** Submits a batch of tile render jobs.
	 *  The snapshot (including its render_func) is shared by all jobs
	 *  in this batch and kept alive until all workers finish with it. */
	void submit(std::vector<TileRenderJob> jobs,
	            TileRenderSnapshot snapshot);

	/** Collects completed results (called on the main thread).
	 *  Returns results whose generation matches the current generation. */
	std::vector<TileRenderResult> collectResults();

	/** Returns true if there are jobs in flight or results pending. */
	bool hasPendingWork() const;

	/**
	 * Suspends tile rendering.  Cancels all pending work, waits for
	 * in-flight renders to complete, and prevents submit() from
	 * accepting new jobs until resume() is called.
	 *
	 * Use this before major map mutations (file loading, undo) that
	 * happen outside the normal paint → updateSceneTiles path.
	 */
	void suspend();

	/** Resumes tile rendering after suspend(). */
	void resume();

	/**
	 * Returns the mutex that protects rendering data.
	 *
	 * Workers hold a shared (read) lock while executing the render
	 * function.  The main thread must hold a unique (write) lock
	 * before mutating any data visible to workers (objects, renderables,
	 * templates).  This ensures workers never see partially-updated state.
	 *
	 * The unique lock naturally waits for active renders to finish,
	 * so the main thread's mutation phase is always conflict-free.
	 */
	std::shared_mutex& renderDataMutex() { return render_data_mutex; }

signals:
	/** Emitted (via queued connection) when results are available. */
	void resultsReady();

private:
	void workerLoop();

	// Work queue — snapshot is shared across all jobs in a batch.
	std::mutex queue_mutex;
	std::condition_variable queue_cv;
	std::queue<std::pair<TileRenderJob, std::shared_ptr<const TileRenderSnapshot>>> work_queue;

	// Results
	std::mutex result_mutex;
	std::vector<TileRenderResult> results;

	// Generation counter for cancellation
	std::atomic<int> gen{0};

	// Rendering data lock — shared by workers (read), exclusive for main thread (write).
	std::shared_mutex render_data_mutex;

	// Suspend flag — when true, submit() is a no-op.
	bool suspended = false;

	// Worker threads
	std::vector<std::thread> workers;
	std::atomic<bool> stopping{false};
	std::atomic<int> in_flight{0};
};


}  // namespace OpenOrienteering

#endif
