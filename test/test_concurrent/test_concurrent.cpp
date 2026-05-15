// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Concurrent stress: a real producer thread bashes a proactor while the
// scheduler drives the consumer on the main thread. The hosted `critical_section`
// is mutex-backed (see core/critical.hpp), so the ISR↔task boundary is modeled
// with real mutual exclusion — exposing the publish-during-arm race that was
// invisible to the single-threaded tests.
//
// The contract we exercise: every `publish()` is eventually reflected in some
// `produce()`: a slot's `writes`, or a ring event plus its `dropped`. A regression in `slot::arm` / `ring::arm`
// (lost wake when a publish races the arm) would manifest as the consumer
// deadlocking with a publish still pending — the assertion at the end would
// fire short of `kPublishes`.

#include <unity.h>
#include "corosched/corosched.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <thread>

using namespace corosched;

namespace {
    std::atomic<std::uint64_t> g_consumed_writes{0};
    std::atomic<std::uint64_t> g_consumed_count{0};
    std::atomic<bool> g_stop{false};
}

// ─── slot: every publish must eventually be reflected in some produce() ─────

corosched::task slot_consumer_loop(corosched::task_frame<2048> &f, slot<int> &s) {
    (void) f;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto r = co_await s.next();
        g_consumed_writes.fetch_add(r.writes, std::memory_order_relaxed);
        g_consumed_count.fetch_add(1, std::memory_order_relaxed);
    }
}

void test_slot_concurrent_no_lost_wake(void) {
    constexpr std::uint64_t kPublishes = 200'000;

    slot<int> slot;
    corosched::task_frame<2048> buf;
    corosched::scheduler sched{slot_consumer_loop(buf, slot)};

    sched.kick();

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kPublishes; ++i) {
            slot.publish(static_cast<int>(i));
        }
        producer_done.store(true, std::memory_order_release);
    });

    // Drive the scheduler until the producer is done AND the slot is empty
    // for a sustained streak — long enough that a missed wake would not be
    // masked by a follow-up publish.
    int idle_streak = 0;
    while (idle_streak < 2000) {
        sched.step();
        const bool done = producer_done.load(std::memory_order_acquire);
        const bool empty = !slot.ready();
        idle_streak = (done && empty) ? idle_streak + 1 : 0;
    }
    producer.join();

    // If `produce()` reset writes after a stray publish that never re-armed,
    // the residual would show here.
    g_stop.store(true, std::memory_order_relaxed);
    sched.step();
    sched.step();

    TEST_ASSERT_EQUAL_UINT64(kPublishes, g_consumed_writes.load());
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_consumed_count.load());
    TEST_ASSERT_FALSE(slot.ready());
}

// ─── ring: every push must eventually be popped (or coalesced under overflow) ─

corosched::task ring_consumer_loop(corosched::task_frame<2048> &f, ring<int, 16> &r) {
    (void) f;
    while (!g_stop.load(std::memory_order_relaxed)) {
        const auto v = co_await r.next();
        if (v.has_value()) {
            g_consumed_writes.fetch_add(1u + v->dropped, std::memory_order_relaxed);
            g_consumed_count.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void test_ring_concurrent_no_lost_wake(void) {
    constexpr std::uint64_t kPublishes = 200'000;

    ring<int, 16> ring;
    corosched::task_frame<2048> buf;
    corosched::scheduler sched{ring_consumer_loop(buf, ring)};

    sched.kick();

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (std::uint64_t i = 0; i < kPublishes; ++i) {
            ring.publish(static_cast<int>(i));
        }
        producer_done.store(true, std::memory_order_release);
    });

    int idle_streak = 0;
    while (idle_streak < 2000) {
        sched.step();
        const bool done = producer_done.load(std::memory_order_acquire);
        const bool empty = !ring.ready();
        idle_streak = (done && empty) ? idle_streak + 1 : 0;
    }
    producer.join();

    g_stop.store(true, std::memory_order_relaxed);
    sched.step();
    sched.step();

    // The ring counts dropped publishes on the newest queued event, so each
    // event plus its `dropped`, summed over all consumes, equals the publish count.
    TEST_ASSERT_EQUAL_UINT64(kPublishes, g_consumed_writes.load());
    TEST_ASSERT_GREATER_THAN_UINT32(0u, g_consumed_count.load());
    TEST_ASSERT_FALSE(ring.ready());
}

// ─── targeted: tight publish/arm interleave to maximize the race window ─────

void test_slot_tight_interleave(void) {
    // Repeat the publish/consume handshake at maximum rate over many short
    // bursts. The producer briefly idles between bursts so the consumer is
    // forced to re-arm — that is precisely the window where a missing
    // catch-up would deadlock.
    constexpr std::uint64_t kBursts = 1'000;
    constexpr std::uint64_t kPerBurst = 200;
    constexpr std::uint64_t kPublishes = kBursts * kPerBurst;

    slot<int> slot;
    corosched::task_frame<2048> buf;
    corosched::scheduler sched{slot_consumer_loop(buf, slot)};

    sched.kick();

    std::atomic<bool> producer_done{false};
    std::thread producer([&] {
        for (std::uint64_t b = 0; b < kBursts; ++b) {
            for (std::uint64_t i = 0; i < kPerBurst; ++i) {
                slot.publish(static_cast<int>(i));
            }
            // Brief gap: invite the consumer to drain and re-arm before the
            // next burst, hitting the await_ready → arm race window head-on.
            std::this_thread::yield();
        }
        producer_done.store(true, std::memory_order_release);
    });

    int idle_streak = 0;
    while (idle_streak < 2000) {
        sched.step();
        const bool done = producer_done.load(std::memory_order_acquire);
        const bool empty = !slot.ready();
        idle_streak = (done && empty) ? idle_streak + 1 : 0;
    }
    producer.join();
    g_stop.store(true, std::memory_order_relaxed);
    sched.step();
    sched.step();

    TEST_ASSERT_EQUAL_UINT64(kPublishes, g_consumed_writes.load());
}

// ─── missed-event path: a single publish must be observed ───────────────────
//
// The stress tests above keep publishing, so a lost wake is eventually masked
// by the next publish. These tests fire exactly one event per trial and bound
// the scheduler step budget — a lost wake fails as "consumer did not run",
// not as a hang.

void test_slot_arm_catches_up_on_pending_publish(void) {
    // Directly exercise slot::arm() catch-up: a publish whose wake()
    // targeted the default waker (no-op) must still result in the runqueue
    // bit getting set when arm() runs and observes pending writes.
    slot<int> slot;
    runqueue_word word{};
    const waker w{&word, 1u << 5};

    slot.publish(42); // wake() → default waker → no-op
    TEST_ASSERT_EQUAL_UINT32(0u, word.load());

    slot.arm(w); // catch-up must fire wake()
    TEST_ASSERT_EQUAL_UINT32(1u << 5, word.load());
}

void test_ring_arm_catches_up_on_pending_publish(void) {
    ring<int, 8> ring;
    runqueue_word word{};
    const waker w{&word, 1u << 7};

    ring.publish(99);
    TEST_ASSERT_EQUAL_UINT32(0u, word.load());

    ring.arm(w);
    TEST_ASSERT_EQUAL_UINT32(1u << 7, word.load());
}

// Step the scheduler until `pred()` is true or the deadline elapses. Yields
// each step so a producer thread is not starved by a tight step() loop.
template<class Sched, class Pred>
bool step_until(Sched &sched, Pred pred, std::chrono::milliseconds deadline) noexcept {
    const auto end = std::chrono::steady_clock::now() + deadline;
    while (std::chrono::steady_clock::now() < end) {
        if (pred()) return true;
        sched.step();
        std::this_thread::yield();
    }
    return pred();
}

void test_slot_single_event_concurrent_no_deadlock(void) {
    // Each trial: consumer arms on the main thread, a producer thread fires
    // exactly one publish. Scheduler must resume the consumer within a
    // generous wall-clock deadline. Repeated to hit the await_ready→arm race.
    constexpr int kTrials = 2'000;
    constexpr auto kDeadline = std::chrono::milliseconds(50);

    for (int trial = 0; trial < kTrials; ++trial) {
        g_consumed_writes.store(0);
        g_consumed_count.store(0);
        g_stop.store(false);

        slot<int> slot;
        corosched::task_frame<2048> buf;
        corosched::scheduler sched{slot_consumer_loop(buf, slot)};
        sched.kick(); // arms and suspends

        std::atomic<bool> go{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire)) { /* spin */
            }
            slot.publish(trial);
        });
        go.store(true, std::memory_order_release);

        const bool ok = step_until(sched, [] { return g_consumed_count.load() >= 1; }, kDeadline);
        producer.join();

        if (!ok) {
            char msg[128];
            std::snprintf(msg, sizeof(msg),
                          "trial %d: single publish not observed within %lldms "
                          "(consumed_count=%llu, slot.ready=%d)",
                          trial, (long long) kDeadline.count(), (unsigned long long) g_consumed_count.load(), slot.ready() ? 1 : 0);
            TEST_FAIL_MESSAGE(msg);
        }
        g_stop.store(true, std::memory_order_relaxed);
    }
}

void test_ring_single_event_concurrent_no_deadlock(void) {
    constexpr int kTrials = 2'000;
    constexpr auto kDeadline = std::chrono::milliseconds(50);

    for (int trial = 0; trial < kTrials; ++trial) {
        g_consumed_writes.store(0);
        g_consumed_count.store(0);
        g_stop.store(false);

        ring<int, 16> ring;
        corosched::task_frame<2048> buf;
        corosched::scheduler sched{ring_consumer_loop(buf, ring)};
        sched.kick();

        std::atomic<bool> go{false};
        std::thread producer([&] {
            while (!go.load(std::memory_order_acquire)) { /* spin */
            }
            ring.publish(trial);
        });
        go.store(true, std::memory_order_release);

        const bool ok = step_until(sched, [] { return g_consumed_count.load() >= 1; }, kDeadline);
        producer.join();

        if (!ok) {
            char msg[128];
            std::snprintf(msg, sizeof(msg), "trial %d: single ring publish not observed within %lldms", trial, (long long) kDeadline.count());
            TEST_FAIL_MESSAGE(msg);
        }
        g_stop.store(true, std::memory_order_relaxed);
    }
}

// Several publishers are allowed, so none of their publishes may go missing from `writes`, even when a ring
// overflows. Under TSan this also shows that nothing in `publish` runs unprotected.
namespace {
    constexpr int kEachPublisher = 20'000;

    template<class Publish, class Drain>
    std::uint64_t writes_from_two_publishers(Publish publish, Drain drain) {
        std::atomic<int> running{2};
        const auto publisher = [&] {
            for (int i = 0; i < kEachPublisher; ++i) publish(i);
            running.fetch_sub(1, std::memory_order_release);
        };
        std::thread a{publisher}, b{publisher};

        std::uint64_t writes = 0;
        while (running.load(std::memory_order_acquire) != 0) writes += drain();
        a.join();
        b.join();
        for (std::uint64_t got = drain(); got != 0; got = drain()) writes += got;
        return writes;
    }
}

void test_slot_two_publishers_lose_no_count(void) {
    slot<int> s;
    const auto writes = writes_from_two_publishers([&s](const int v) { s.publish(v); }, [&s]() -> std::uint64_t { return s.produce().writes; });
    TEST_ASSERT_EQUAL_UINT64(2 * kEachPublisher, writes);
}

void test_ring_two_publishers_lose_no_count(void) {
    ring<int, 8> r;
    const auto writes = writes_from_two_publishers([&r](const int v) { r.publish(v); },
                                                   [&r]() -> std::uint64_t {
                                                       const auto event = r.produce();
                                                       return event ? 1u + event->dropped : 0u;
                                                   });
    TEST_ASSERT_EQUAL_UINT64(2 * kEachPublisher, writes);
}

void setUp(void) {
    g_consumed_writes.store(0);
    g_consumed_count.store(0);
    g_stop.store(false);
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_slot_concurrent_no_lost_wake);
    RUN_TEST(test_ring_concurrent_no_lost_wake);
    RUN_TEST(test_slot_tight_interleave);
    RUN_TEST(test_slot_arm_catches_up_on_pending_publish);
    RUN_TEST(test_ring_arm_catches_up_on_pending_publish);
    RUN_TEST(test_slot_single_event_concurrent_no_deadlock);
    RUN_TEST(test_ring_single_event_concurrent_no_deadlock);
    RUN_TEST(test_slot_two_publishers_lose_no_count);
    RUN_TEST(test_ring_two_publishers_lose_no_count);
    return UNITY_END();
}
