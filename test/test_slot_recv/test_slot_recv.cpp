// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// slot / ring: publish-side surface + the generic
// `proactor_awaiter` consumed via `co_await x.next()`.
#include <unity.h>
#include "corosched/corosched.hpp"

#include <atomic>
#include <cstdint>

using namespace corosched;

namespace {
    std::uint32_t g_writes = 0;
    int g_data = -1;
    bool g_done = false;

    std::uint32_t g_drain_dropped[2] = {0, 0};
    int g_drain_data[2] = {-1, -1};
}

// ─── slot (last-wins) ──────────────────────────────────────────────

void test_slot_publish_produce(void) {
    slot<int> slot;
    TEST_ASSERT_FALSE(slot.ready());
    slot.publish(42);
    TEST_ASSERT_TRUE(slot.ready());
    const auto r = slot.produce();
    TEST_ASSERT_EQUAL_UINT32(1u, r.writes);
    TEST_ASSERT_EQUAL_INT(42, r.data);
    TEST_ASSERT_FALSE(slot.ready()); // consumed
}

void test_slot_is_last_wins_and_counts_overwrites(void) {
    // Three publishes coalesce: the freshest value wins, and `writes` reports
    // the count so a slow consumer can tell it raced.
    slot<int> slot;
    slot.publish(1);
    slot.publish(2);
    slot.publish(3);
    const auto r = slot.produce();
    TEST_ASSERT_EQUAL_INT(3, r.data);
    TEST_ASSERT_EQUAL_UINT32(3u, r.writes);
}

void test_slot_result_bool_is_on_time(void) {
    // A single write between consumes is the on-time case: `result_type::bool == true`.
    slot<int> slot;
    slot.publish(9);
    const auto r = slot.produce();
    TEST_ASSERT_EQUAL_UINT32(1u, r.writes);
}

// ─── ring (event ring) ──────────────────────────────────────────────

void test_ring_fifo_order(void) {
    ring<int, 4> ring;
    TEST_ASSERT_FALSE(ring.ready());
    ring.publish(10);
    ring.publish(20);
    TEST_ASSERT_TRUE(ring.ready());
    const auto a = ring.produce();
    const auto b = ring.produce();
    TEST_ASSERT_TRUE(a.has_value());
    TEST_ASSERT_TRUE(b.has_value());
    TEST_ASSERT_EQUAL_INT(10, a->data);
    TEST_ASSERT_EQUAL_INT(20, b->data);
    TEST_ASSERT_FALSE(ring.ready());
}

void test_ring_overflow_counts_on_newest(void) {
    // Capacity = N. Publishes past full are dropped and counted on the newest
    // queued event's `dropped` — the loss is counted, not silent.
    ring<int, 4> ring;
    ring.publish(1);
    ring.publish(2);
    ring.publish(3);
    ring.publish(4); // ring full
    ring.publish(5); // dropped, counted on 4
    ring.publish(6); // dropped again

    const auto a = ring.produce();
    const auto b = ring.produce();
    const auto c = ring.produce();
    const auto d = ring.produce();
    TEST_ASSERT_EQUAL_INT(1, a->data);
    TEST_ASSERT_EQUAL_INT(2, b->data);
    TEST_ASSERT_EQUAL_INT(3, c->data);
    TEST_ASSERT_EQUAL_INT(4, d->data);
    TEST_ASSERT_EQUAL_UINT32(0u, c->dropped);
    TEST_ASSERT_EQUAL_UINT32(2u, d->dropped); // 5 and 6
    TEST_ASSERT_FALSE(ring.ready());
}

void test_ring_keeps_order_across_many_wraps(void) {
    // The indices run free, so this walks the mask through every position.
    ring<int, 4> ring;
    for (int i = 0; i < 1000; ++i) {
        ring.publish(i);
        ring.publish(i + 1);
        TEST_ASSERT_EQUAL_INT(i, ring.produce()->data);
        TEST_ASSERT_EQUAL_INT(i + 1, ring.produce()->data);
        TEST_ASSERT_FALSE(ring.ready());
    }
}

void test_ring_produce_empty_is_nullopt(void) {
    ring<int, 4> ring;
    TEST_ASSERT_FALSE(ring.produce().has_value());
}

// ─── proactor_awaiter, end-to-end through a real scheduler ──────────────────

corosched::task slot_consumer(corosched::task_frame<1024> &f, slot<int> &s) {
    (void) f;
    const auto r = co_await s.next();
    g_writes = r.writes;
    g_data = r.data;
    g_done = true;
}

corosched::task ring_drainer(corosched::task_frame<1024> &f, ring<int, 8> &r) {
    (void) f;
    for (int i = 0; i < 2; ++i) {
        const auto v = co_await r.next();
        g_drain_dropped[i] = v->dropped;
        g_drain_data[i] = v->data;
    }
    g_done = true;
}

void test_recv_suspend_path(void) {
    slot<int> slot;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{slot_consumer(buf, slot)};

    sched.kick(); // task arms its waker and suspends
    TEST_ASSERT_FALSE(g_done);
    slot.publish(77); // producer fires
    sched.step(); // scheduler resumes the consumer
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(77, g_data);
    TEST_ASSERT_EQUAL_UINT32(1u, g_writes);
}

void test_recv_fast_path(void) {
    slot<int> slot;
    slot.publish(5); // value already available
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{slot_consumer(buf, slot)};

    sched.kick(); // synchronous: no suspend at all
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(5, g_data);
}

void test_recv_drains_ring(void) {
    ring<int, 8> ring;
    ring.publish(100);
    ring.publish(200);
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{ring_drainer(buf, ring)};

    sched.kick(); // both events buffered → fast-path drain
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(100, g_drain_data[0]);
    TEST_ASSERT_EQUAL_INT(200, g_drain_data[1]);
    TEST_ASSERT_EQUAL_UINT32(0u, g_drain_dropped[0]);
    TEST_ASSERT_EQUAL_UINT32(0u, g_drain_dropped[1]);
}

void test_recv_disarms_after_resume(void) {
    // After await_resume the proactor is disarmed: a later publish targets a
    // default waker and must be a safe no-op (no crash, no stray wake).
    slot<int> slot;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{slot_consumer(buf, slot)};

    sched.kick();
    slot.publish(1);
    sched.step(); // consumer resumes and disarms
    TEST_ASSERT_TRUE(g_done);

    slot.publish(2); // post-disarm producer activity
    TEST_ASSERT_TRUE(slot.ready());
    const auto r = slot.produce();
    TEST_ASSERT_EQUAL_INT(2, r.data);
}

// ─── disarm() with mismatched waker is a no-op ──────────────────────────────
//
// Proactors compare the waker passed to `disarm()` against the one currently
// armed. A mismatch must leave the proactor's `armed_` untouched — covers the
// false branch of the `armed_ == w` check.

void test_slot_disarm_mismatched_waker_is_noop(void) {
    slot<int> slot;
    corosched::runqueue_word wA{}, wB{};
    const corosched::waker a{&wA, 1u << 0};
    const corosched::waker b{&wB, 1u << 0};

    slot.arm(a);
    slot.disarm(b); // mismatch: no-op
    slot.publish(7); // must still wake `a`
    TEST_ASSERT_EQUAL_UINT32(1u, wA.load());
    TEST_ASSERT_EQUAL_UINT32(0u, wB.load());
}

void test_ring_disarm_mismatched_waker_is_noop(void) {
    ring<int, 4> ring;
    corosched::runqueue_word wA{}, wB{};
    const corosched::waker a{&wA, 1u << 1};
    const corosched::waker b{&wB, 1u << 1};

    ring.arm(a);
    ring.disarm(b); // mismatch: no-op
    ring.publish(99);
    TEST_ASSERT_EQUAL_UINT32(2u, wA.load()); // wake() sets bit 1
    TEST_ASSERT_EQUAL_UINT32(0u, wB.load());
}

// ─── arm() with a pending value wakes immediately ───────────────────────────
//
// If `ready()` is true at arm time, the proactor must call `w.wake()`
// synchronously so the bit reflects the buffered event — covers the true
// branch of the `if (ready())` inside `arm()`.

void test_ring_arm_pending_wakes_immediately(void) {
    ring<int, 4> ring;
    ring.publish(42); // value already buffered

    corosched::runqueue_word rq{};
    const corosched::waker w{&rq, 1u << 3};
    ring.arm(w);
    TEST_ASSERT_EQUAL_UINT32(1u << 3, rq.load());
}

void setUp(void) {
    g_writes = 0;
    g_data = -1;
    g_done = false;
    g_drain_dropped[0] = g_drain_dropped[1] = 0;
    g_drain_data[0] = g_drain_data[1] = -1;
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_slot_publish_produce);
    RUN_TEST(test_slot_is_last_wins_and_counts_overwrites);
    RUN_TEST(test_slot_result_bool_is_on_time);
    RUN_TEST(test_ring_fifo_order);
    RUN_TEST(test_ring_overflow_counts_on_newest);
    RUN_TEST(test_ring_keeps_order_across_many_wraps);
    RUN_TEST(test_ring_produce_empty_is_nullopt);
    RUN_TEST(test_recv_suspend_path);
    RUN_TEST(test_recv_fast_path);
    RUN_TEST(test_recv_drains_ring);
    RUN_TEST(test_recv_disarms_after_resume);
    RUN_TEST(test_slot_disarm_mismatched_waker_is_noop);
    RUN_TEST(test_ring_disarm_mismatched_waker_is_noop);
    RUN_TEST(test_ring_arm_pending_wakes_immediately);
    return UNITY_END();
}
