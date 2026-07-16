/*
 *    Copyright 2026 Ethan O'Connor
 *
 *    This file is part of OpenOrienteering.
 */

#include <algorithm>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include <vector>

#include <QtTest>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QObject>
#include <QRegularExpression>
#include <QSemaphore>
#include <QThread>

#include "templates/raster_resource_manager.h"

using namespace OpenOrienteering;

namespace {

using Lane = RasterResourceManager::Lane;
using Priority = RasterResourceManager::Priority;

RasterResourceManager::Work gatedWork(
	QSemaphore& started,
	QSemaphore& gate,
	std::function<void()> completion = {})
{
	return [&started, &gate, completion = std::move(completion)](
		const RasterResourceManager::CancellationToken&) mutable {
		started.release();
		gate.acquire();
		return std::move(completion);
	};
}

class WorkDrain
{
public:
	WorkDrain(
		RasterResourceManager& manager,
		std::initializer_list<RasterResourceManager::Owner*> owners,
		std::initializer_list<QSemaphore*> gates)
	 : manager_(manager)
	 , owners_(owners)
	 , gates_(gates)
	{}

	~WorkDrain()
	{
		for (auto* owner : owners_)
			if (owner)
				owner->invalidate();
		for (auto* gate : gates_)
			if (gate)
				gate->release(4096);

		QElapsedTimer timer;
		timer.start();
		while (timer.elapsed() < 5000
		       && (manager_.activeCount(Lane::BlockingIo) != 0
		           || manager_.activeCount(Lane::Decode) != 0
		           || manager_.pendingCount(Lane::BlockingIo) != 0
		           || manager_.pendingCount(Lane::Decode) != 0))
		{
			QCoreApplication::processEvents();
			QThread::msleep(1);
		}
	}

private:
	RasterResourceManager& manager_;
	std::vector<RasterResourceManager::Owner*> owners_;
	std::vector<QSemaphore*> gates_;
};

}  // namespace

class RasterResourceManagerTest : public QObject
{
Q_OBJECT

private slots:
	void laneAndOwnerLimitsAreIndependent()
	{
		RasterResourceManager manager({ 2, 1 });
		auto owner = manager.createOwner(1);
		QSemaphore io_started;
		QSemaphore io_gate;
		QSemaphore decode_started;
		QSemaphore decode_gate;
		auto completed = 0;
		WorkDrain drain(manager, { &owner }, { &io_gate, &decode_gate });

		QVERIFY(manager.submit(
			owner, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(io_started, io_gate, [&] { ++completed; })
		));
		QVERIFY(manager.submit(
			owner, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(io_started, io_gate, [&] { ++completed; })
		));
		QVERIFY(manager.submit(
			owner, Lane::Decode, Priority::Visible, this,
			gatedWork(decode_started, decode_gate, [&] { ++completed; })
		));

		QVERIFY(io_started.tryAcquire(1, 1000));
		QVERIFY(!io_started.tryAcquire(1, 100));
		QVERIFY(decode_started.tryAcquire(1, 1000));
		QCOMPARE(manager.activeCount(Lane::BlockingIo), 1);
		QCOMPARE(manager.activeCount(Lane::Decode), 1);

		io_gate.release();
		QTRY_COMPARE_WITH_TIMEOUT(completed, 1, 1000);
		QVERIFY(io_started.tryAcquire(1, 1000));
		decode_gate.release();
		QTRY_COMPARE_WITH_TIMEOUT(completed, 2, 1000);
		io_gate.release();
		QTRY_COMPARE_WITH_TIMEOUT(completed, 3, 1000);
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::BlockingIo), 0, 1000);
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::Decode), 0, 1000);
	}

	void blockingIoDoesNotStarveDecode()
	{
		RasterResourceManager manager({ 1, 1 });
		auto io_owner = manager.createOwner();
		auto decode_owner = manager.createOwner();
		QSemaphore io_started;
		QSemaphore io_gate;
		QSemaphore decode_started;
		QSemaphore decode_gate;
		WorkDrain drain(
			manager, { &io_owner, &decode_owner }, { &io_gate, &decode_gate }
		);

		QVERIFY(manager.submit(
			io_owner, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(io_started, io_gate)
		));
		QVERIFY(io_started.tryAcquire(1, 1000));
		QVERIFY(manager.submit(
			decode_owner, Lane::Decode, Priority::Visible, this,
			gatedWork(decode_started, decode_gate)
		));
		QVERIFY(decode_started.tryAcquire(1, 1000));

		io_gate.release();
		decode_gate.release();
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::BlockingIo), 0, 1000);
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::Decode), 0, 1000);
	}

	void invalidationCancelsOnlyItsOwner()
	{
		RasterResourceManager manager({ 1, 1 });
		auto first = manager.createOwner();
		auto second = manager.createOwner();
		QSemaphore first_started;
		QSemaphore second_started;
		QSemaphore second_gate;
		auto first_delivered = false;
		auto second_delivered = false;
		WorkDrain drain(
			manager, { &first, &second }, { &second_gate }
		);

		QVERIFY(manager.submit(
			first, Lane::BlockingIo, Priority::Visible, this,
			[&](const RasterResourceManager::CancellationToken& token) {
				first_started.release();
				while (!token.isCancelled())
					QThread::msleep(1);
				return RasterResourceManager::Completion {
					[&] { first_delivered = true; }
				};
			}
		));
		QVERIFY(manager.submit(
			second, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(second_started, second_gate, [&] { second_delivered = true; })
		));
		QVERIFY(first_started.tryAcquire(1, 1000));

		first.invalidate();
		QVERIFY(second_started.tryAcquire(1, 1000));
		second_gate.release();
		QTRY_VERIFY_WITH_TIMEOUT(second_delivered, 1000);
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::BlockingIo), 0, 1000);
		QVERIFY(!first_delivered);
	}

	void staleAndDestroyedReceiverCompletionsAreSuppressed()
	{
		RasterResourceManager manager({ 2, 1 });
		auto stale_owner = manager.createOwner();
		auto receiver_owner = manager.createOwner();
		QSemaphore stale_started;
		QSemaphore stale_gate;
		QSemaphore receiver_started;
		QSemaphore receiver_gate;
		auto stale_delivered = false;
		auto receiver_delivered = false;
		auto* receiver = new QObject;
		WorkDrain drain(
			manager, { &stale_owner, &receiver_owner },
			{ &stale_gate, &receiver_gate }
		);

		QVERIFY(manager.submit(
			stale_owner, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(stale_started, stale_gate, [&] { stale_delivered = true; })
		));
		QVERIFY(manager.submit(
			receiver_owner, Lane::BlockingIo, Priority::Visible, receiver,
			gatedWork(
				receiver_started, receiver_gate,
				[&] { receiver_delivered = true; }
			)
		));
		QVERIFY(stale_started.tryAcquire(1, 1000));
		QVERIFY(receiver_started.tryAcquire(1, 1000));

		stale_owner.invalidate();
		delete receiver;
		stale_gate.release();
		receiver_gate.release();
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::BlockingIo), 0, 1000);
		QCoreApplication::processEvents();
		QVERIFY(!stale_delivered);
		QVERIFY(!receiver_delivered);
	}

	void activeWorkMayOutliveOwnerAndReceiver()
	{
		RasterResourceManager manager({ 1, 1 });
		QSemaphore started;
		QSemaphore gate;
		std::atomic_bool work_finished = false;
		auto delivered = false;
		WorkDrain drain(manager, {}, { &gate });

		{
			auto owner = manager.createOwner();
			auto receiver = std::make_unique<QObject>();
			QVERIFY(manager.submit(
				owner, Lane::BlockingIo, Priority::Visible, receiver.get(),
				[&](const RasterResourceManager::CancellationToken&) {
					started.release();
					gate.acquire();
					work_finished.store(true, std::memory_order_relaxed);
					return RasterResourceManager::Completion {
						[&] { delivered = true; }
					};
				}
			));
			QVERIFY(started.tryAcquire(1, 1000));
		}

		gate.release();
		QTRY_VERIFY_WITH_TIMEOUT(
			work_finished.load(std::memory_order_relaxed), 1000
		);
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::BlockingIo), 0, 1000);
		QCoreApplication::processEvents();
		QVERIFY(!delivered);
	}

	void priorityAndOwnerOrderingAreDeterministic()
	{
		RasterResourceManager manager({ 1, 1 });
		auto first = manager.createOwner();
		auto second = manager.createOwner();
		QSemaphore started;
		QSemaphore gate;
		std::mutex order_mutex;
		std::vector<int> order;
		WorkDrain drain(manager, { &first, &second }, { &gate });

		auto submit = [&](RasterResourceManager::Owner& owner,
		                  Priority priority,
		                  int marker) {
			return manager.submit(
				owner, Lane::BlockingIo, priority, this,
				[&, marker](const RasterResourceManager::CancellationToken&) {
					{
						std::lock_guard lock(order_mutex);
						order.push_back(marker);
					}
					started.release();
					gate.acquire();
					return RasterResourceManager::Completion {};
				}
			);
		};

		QVERIFY(submit(first, Priority::Background, 10));
		QVERIFY(started.tryAcquire(1, 1000));
		QVERIFY(submit(first, Priority::Visible, 11));
		QVERIFY(submit(first, Priority::Visible, 12));
		QVERIFY(submit(second, Priority::Visible, 21));
		QVERIFY(submit(second, Priority::Visible, 22));
		QVERIFY(submit(second, Priority::Coverage, 20));

		for (int index = 0; index < 5; ++index)
		{
			gate.release();
			QVERIFY(started.tryAcquire(1, 1000));
		}
		gate.release();
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::BlockingIo), 0, 1000);

		std::lock_guard lock(order_mutex);
		QCOMPARE(order.size(), std::size_t(6));
		QCOMPARE(order[0], 10);
		QCOMPARE(order[1], 20);
		QCOMPARE(order[2], 11);
		QCOMPARE(order[3], 21);
		QCOMPARE(order[4], 12);
		QCOMPARE(order[5], 22);
	}

	void laneCapAndOwnerParallelismAreBounded()
	{
		RasterResourceManager manager({ 2, 1 });
		auto owner = manager.createOwner(2);
		QSemaphore started;
		QSemaphore gate;
		WorkDrain drain(manager, { &owner }, { &gate });

		for (int index = 0; index < 3; ++index)
		{
			QVERIFY(manager.submit(
				owner, Lane::BlockingIo, Priority::Visible, this,
				gatedWork(started, gate)
			));
		}
		QVERIFY(started.tryAcquire(2, 1000));
		QVERIFY(!started.tryAcquire(1, 100));
		QCOMPARE(manager.activeCount(Lane::BlockingIo), 2);
		gate.release(2);
		QVERIFY(started.tryAcquire(1, 1000));
		gate.release();
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::BlockingIo), 0, 1000);
	}

	void queueBoundsRejectExcessWork()
	{
		RasterResourceManager::Limits limits;
		limits.blocking_io_threads = 1;
		limits.decode_threads = 1;
		limits.max_pending_per_owner = 2;
		limits.max_pending_per_lane = 3;
		RasterResourceManager manager(limits);
		auto owner = manager.createOwner(1);
		QSemaphore started;
		QSemaphore gate;
		WorkDrain drain(manager, { &owner }, { &gate });

		QVERIFY(manager.submit(
			owner, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(started, gate)
		));
		QVERIFY(started.tryAcquire(1, 1000));
		QVERIFY(manager.submit(
			owner, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(started, gate)
		));
		QVERIFY(manager.submit(
			owner, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(started, gate)
		));
		QVERIFY(!manager.submit(
			owner, Lane::BlockingIo, Priority::Visible, this,
			gatedWork(started, gate)
		));
		QCOMPARE(manager.pendingCount(Lane::BlockingIo), std::size_t(2));
	}

	void exceptionReleasesSlotAndCompletionUsesManagerThread()
	{
		RasterResourceManager manager({ 1, 1 });
		auto owner = manager.createOwner();
		WorkDrain drain(manager, { &owner }, {});
		std::atomic_bool work_off_manager_thread = false;
		auto completion_on_manager_thread = false;
		auto delivered = false;

		QTest::ignoreMessage(
			QtWarningMsg,
			QRegularExpression(QStringLiteral("Raster worker failed:.*boom"))
		);
		QVERIFY(manager.submit(
			owner, Lane::BlockingIo, Priority::Visible, this,
			[&](const RasterResourceManager::CancellationToken&) {
				work_off_manager_thread.store(
					QThread::currentThread() != manager.thread(),
					std::memory_order_relaxed
				);
				throw std::runtime_error("boom");
				return RasterResourceManager::Completion {};
			}
		));
		QVERIFY(manager.submit(
			owner, Lane::BlockingIo, Priority::Visible, this,
			[&](const RasterResourceManager::CancellationToken&) {
				return RasterResourceManager::Completion {
					[&] {
						completion_on_manager_thread =
							QThread::currentThread() == manager.thread();
						delivered = true;
					}
				};
			}
		));

		QTRY_VERIFY_WITH_TIMEOUT(delivered, 1000);
		QVERIFY(work_off_manager_thread.load(std::memory_order_relaxed));
		QVERIFY(completion_on_manager_thread);
		QTRY_COMPARE_WITH_TIMEOUT(manager.activeCount(Lane::BlockingIo), 0, 1000);
	}
};

QTEST_MAIN(RasterResourceManagerTest)
#include "raster_resource_manager_t.moc"
