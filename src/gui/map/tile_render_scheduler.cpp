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


#include "tile_render_scheduler.h"

#include <algorithm>

#include <QPainter>
#include <QMetaObject>

namespace OpenOrienteering {


TileRenderScheduler::TileRenderScheduler(QObject* parent)
    : QObject(parent)
{
	int num_threads = std::max(1, static_cast<int>(std::thread::hardware_concurrency()) - 2);
	workers.reserve(num_threads);
	for (int i = 0; i < num_threads; ++i)
		workers.emplace_back(&TileRenderScheduler::workerLoop, this);
}


TileRenderScheduler::~TileRenderScheduler()
{
	stopping.store(true, std::memory_order_release);
	queue_cv.notify_all();
	for (auto& w : workers)
	{
		if (w.joinable())
			w.join();
	}
}


void TileRenderScheduler::cancelPending()
{
	gen.fetch_add(1, std::memory_order_release);

	// Drain the work queue (no point rendering stale jobs).
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		decltype(work_queue) empty;
		work_queue.swap(empty);
	}

	// Wait for any in-progress renders to complete.  Workers hold
	// a shared lock on render_data_mutex while executing render_func.
	// Taking a unique lock here blocks until all workers finish,
	// guaranteeing no worker is reading map data when we return.
	{
		std::unique_lock lock(render_data_mutex);
	}

	// Drain stale results.
	{
		std::lock_guard<std::mutex> lock(result_mutex);
		results.clear();
	}
}


void TileRenderScheduler::suspend()
{
	suspended = true;
	cancelPending();
}


void TileRenderScheduler::resume()
{
	suspended = false;
}


void TileRenderScheduler::submit(std::vector<TileRenderJob> jobs,
                                  TileRenderSnapshot snapshot)
{
	if (jobs.empty() || suspended)
		return;

	auto shared = std::make_shared<const TileRenderSnapshot>(std::move(snapshot));
	{
		std::lock_guard<std::mutex> lock(queue_mutex);
		for (auto& job : jobs)
			work_queue.push({std::move(job), shared});
	}
	in_flight.fetch_add(static_cast<int>(jobs.size()), std::memory_order_relaxed);
	queue_cv.notify_all();
}


std::vector<TileRenderResult> TileRenderScheduler::collectResults()
{
	std::lock_guard<std::mutex> lock(result_mutex);
	std::vector<TileRenderResult> out;
	int current_gen = gen.load(std::memory_order_relaxed);
	for (auto& r : results)
	{
		if (r.generation == current_gen)
			out.push_back(std::move(r));
	}
	results.clear();
	return out;
}


bool TileRenderScheduler::hasPendingWork() const
{
	return in_flight.load(std::memory_order_relaxed) > 0;
}


void TileRenderScheduler::workerLoop()
{
	while (true)
	{
		std::pair<TileRenderJob, std::shared_ptr<const TileRenderSnapshot>> work;

		{
			std::unique_lock<std::mutex> lock(queue_mutex);
			queue_cv.wait(lock, [this]() {
				return stopping.load(std::memory_order_acquire) || !work_queue.empty();
			});

			if (stopping.load(std::memory_order_acquire) && work_queue.empty())
				return;

			work = std::move(work_queue.front());
			work_queue.pop();
		}

		auto& job = work.first;
		const auto& snapshot = *work.second;

		// Check if this job is still relevant.
		if (job.generation != gen.load(std::memory_order_relaxed))
		{
			in_flight.fetch_sub(1, std::memory_order_relaxed);
			continue;
		}

		// Render the tile.
		// Hold a shared (read) lock on the rendering data for the
		// duration of the render.  This prevents the main thread from
		// mutating objects/renderables/templates while we read them.
		if (snapshot.render_func)
		{
			std::shared_lock lock(render_data_mutex);

			// Re-check generation under lock — the main thread may
			// have cancelled while we waited.
			if (job.generation != gen.load(std::memory_order_relaxed))
			{
				lock.unlock();
				in_flight.fetch_sub(1, std::memory_order_relaxed);
				continue;
			}

			QPainter painter(&job.image);

			// Clear to transparent.
			painter.setCompositionMode(QPainter::CompositionMode_Clear);
			painter.fillRect(0, 0, job.image.width(), job.image.height(), Qt::transparent);
			painter.setCompositionMode(QPainter::CompositionMode_SourceOver);

			if (snapshot.antialiasing)
				painter.setRenderHint(QPainter::Antialiasing);

			// Set up the transform: tile pixel → view space → map space.
			painter.translate(-job.tile_view_rect.x(), -job.tile_view_rect.y());
			painter.setWorldTransform(snapshot.world_transform, true);

			snapshot.render_func(painter, job.tile_map_rect, snapshot);

			painter.end();
		}

		// Check generation again after rendering.
		int current_gen = gen.load(std::memory_order_relaxed);
		if (job.generation == current_gen)
		{
			TileRenderResult result;
			result.key = job.key;
			result.image = std::move(job.image);
			result.generation = job.generation;

			{
				std::lock_guard<std::mutex> lock(result_mutex);
				results.push_back(std::move(result));
			}

			// Signal the main thread that results are available.
			QMetaObject::invokeMethod(this, "resultsReady", Qt::QueuedConnection);
		}

		in_flight.fetch_sub(1, std::memory_order_relaxed);
	}
}


}  // namespace OpenOrienteering
