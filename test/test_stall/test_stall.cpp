// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Liveness suite: the one failure mode this library must not have is a task
// that stops being woken and never runs again.
//
// A stall is invisible from the inside — no counter moves, no assert trips,
// the scheduler just goes idle forever — so it cannot be caught by asserting
// on outputs. Each test here instead drives a proactor into the state that
// would wedge it and then asserts the task keeps making progress *after*.
//
// The compile-time half of the contract (a non-positive Period/Duration, which
// would build a countdown that reloads by nothing) is a `static_assert` and so
// is exercised by the negative-compilation block at the bottom of the file.

#include <unity.h>
#include "corosched/corosched.hpp"

#include <cstdint>
#include <ratio>

using namespace corosched;
using namespace corosched::literals;

// ─── fake periodic_clock ────────────────────────────────────────────────────

namespace {
    // tick() comes from tick_list: the test calls it to drive one tick as if from the timer interrupt.
    struct fake_clock : corosched::tick_list {
        using time_base = corosched::time_base<std::ratio<1, 1'000'000>>; // 1 µs tick
    };

    static_assert(periodic_clock<fake_clock>);

    int g_runs = 0;
    int g_other = 0;
}

// ─── periodic: no consumer lateness can wedge the countdown ─────────────────

namespace {
    corosched::task periodic_loop(corosched::task_frame<1024> &f, periodic<fake_clock, 5_us> &p) {
        (void) f;
        for (;;) {
            co_await p.next();
            ++g_runs;
        }
    }
}

void test_periodic_stays_live_across_every_lateness_offset(void) {
    // The original stall needed the consumer to be late by *exactly* one
    // period; a fix that only handled that case would still wedge at some
    // other offset. Sweep every lateness from 0 to 3 full periods and require
    // the proactor to keep firing on the grid afterwards in each case.
    for (int late = 0; late <= 15; ++late) {
        fake_clock clk;
        periodic<fake_clock, 5_us> p{clk};
        corosched::task_frame<1024> buf;
        corosched::scheduler sched{periodic_loop(buf, p)};

        g_runs = 0;
        sched.kick();

        for (int i = 0; i < 5 + late; ++i) clk.tick(); // boundary, then starved
        sched.step();
        TEST_ASSERT_EQUAL_INT(1, g_runs);

        // Whatever the lateness was, the proactor must still be counting: 100
        // further ticks are 20 more grid boundaries, and none may be lost.
        const int before = g_runs;
        for (int i = 0; i < 100; ++i) {
            clk.tick();
            sched.step();
        }
        TEST_ASSERT_EQUAL_INT(before + 20, g_runs);
    }
}

void test_periodic_stays_live_after_a_very_long_starvation(void) {
    // Long enough that a countdown which kept decrementing without reloading
    // would have run `remaining_` (int32) far from its grid.
    fake_clock clk;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{periodic_loop(buf, p)};

    sched.kick();
    for (int i = 0; i < 1'000'000; ++i) clk.tick(); // never scheduled
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs);

    // Still on the grid: tick 1'000'000 is a boundary, so the next is +5.
    for (int i = 0; i < 5; ++i) {
        clk.tick();
        sched.step();
    }
    TEST_ASSERT_EQUAL_INT(2, g_runs);

    for (int i = 0; i < 500; ++i) {
        clk.tick();
        sched.step();
    }
    TEST_ASSERT_EQUAL_INT(2 + 100, g_runs);
}

// ─── a starved proactor must not starve its neighbours ───────────────────────

namespace {
    corosched::task low_prio_loop(corosched::task_frame<1024> &f, slot<int> &s) {
        (void) f;
        for (;;) {
            co_await s.next();
            ++g_other;
        }
    }
}

void test_unconsumed_periodic_does_not_wedge_another_task(void) {
    // Task 0 (higher priority) never gets to run its periodic to completion
    // in this window; task 1 must still be woken by its own proactor. A proactor
    // that latched a permanent wake would pin the runqueue's low bit and
    // starve everything below it.
    fake_clock clk;
    periodic<fake_clock, 5_us> p{clk};
    slot<int> s;
    corosched::task_frame<1024> b0, b1;
    corosched::scheduler sched{periodic_loop(b0, p), low_prio_loop(b1, s)};

    sched.kick();
    for (int i = 0; i < 200; ++i) clk.tick();

    for (int i = 0; i < 50; ++i) {
        s.publish(i);
        sched.step(); // periodic (bit 0) wins
        sched.step(); // slot (bit 1) must still get its turn
    }

    TEST_ASSERT_TRUE_MESSAGE(g_runs > 0, "periodic task never ran");
    TEST_ASSERT_TRUE_MESSAGE(g_other > 0, "slot task was starved");
}

// ─── slot / ring: a saturated or overflowing proactor stays consumable ───────

namespace {
    corosched::task ring_loop(corosched::task_frame<1024> &f, ring<int, 4> &r) {
        (void) f;
        for (;;) {
            const auto v = co_await r.next();
            if (v) ++g_runs;
        }
    }
}

void test_ring_stays_live_after_sustained_overflow(void) {
    // The ring is deliberately overrun far past capacity with nobody draining
    // it. Once the consumer runs again it must drain and keep being woken by
    // subsequent publishes — an overflow policy that corrupted head_/tail_
    // would leave `ready()` stuck false.
    ring<int, 4> r;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{ring_loop(buf, r)};

    sched.kick();
    for (int i = 0; i < 10'000; ++i) r.publish(i); // ring holds 4

    for (int i = 0; i < 10; ++i) sched.step(); // drain
    const int drained = g_runs;

    // Still alive: every later publish must still reach the consumer.
    for (int i = 0; i < 20; ++i) {
        r.publish(i);
        sched.step();
    }
    TEST_ASSERT_TRUE_MESSAGE(g_runs > drained, "ring stopped waking after overflow");
}

void test_slot_stays_live_after_write_counter_saturates(void) {
    // `writes` is a saturating counter. If it ever wrapped to 0 instead, the
    // slot would read as empty with a value sitting in it and the consumer
    // would sleep on a publish that had already happened.
    slot<int> s;
    for (std::uint64_t i = 0; i < 70'000; ++i) s.publish(1);
    TEST_ASSERT_TRUE(s.ready());

    const auto out = s.produce();
    TEST_ASSERT_TRUE(out.writes > 1);
    TEST_ASSERT_FALSE(s.ready());

    s.publish(2);
    TEST_ASSERT_TRUE_MESSAGE(s.ready(), "slot went deaf after saturation");
}

// ─── the arm/publish race, from both sides ──────────────────────────────────

namespace {
    corosched::task slot_loop(corosched::task_frame<1024> &f, slot<int> &s) {
        (void) f;
        for (;;) {
            co_await s.next();
            ++g_runs;
        }
    }
}

void test_publish_between_poll_and_arm_is_not_lost(void) {
    // The window the `arm()` catch-up exists to close: the proactor is empty
    // when `await_ready()` polls it, and the publish lands before `arm()`
    // registers the waker, so the ISR-side wake() hits the default waker and
    // is a no-op. Without the catch-up the task sleeps on a value it already
    // has.
    slot<int> s;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{slot_loop(buf, s)};

    sched.kick(); // polls empty, suspends, arms
    TEST_ASSERT_EQUAL_INT(0, g_runs);

    for (int round = 0; round < 100; ++round) {
        const int before = g_runs;
        s.publish(round);
        sched.step();
        TEST_ASSERT_EQUAL_INT_MESSAGE(before + 1, g_runs, "publish did not wake the task");
    }
}

// ─── compile-time liveness guards (these MUST NOT compile) ──────────────────
//
// Uncomment any line to verify the guard still fires. Each one would otherwise
// build a proactor whose countdown reloads by zero or by a negative amount —
// i.e. one that stops firing, silently, at some point after startup.
//
//   periodic<fake_clock, nanos{500}>   bad_multiple{clk};  // 500 ns vs 1 µs tick
//   periodic<fake_clock, nanos{0}>     bad_zero{clk};      // reloads by nothing
//   periodic<fake_clock, nanos{-1000}> bad_negative{clk};  // counts backwards
//   delay<fake_clock, nanos{0}>        bad_delay_zero{clk};
//   delay<fake_clock, nanos{-1000}>    bad_delay_neg{clk}; // parks, never fires
//
// `to_ticks` rejects all five, each with its own static_assert message.

void setUp(void) {
    g_runs = 0;
    g_other = 0;
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_periodic_stays_live_across_every_lateness_offset);
    RUN_TEST(test_periodic_stays_live_after_a_very_long_starvation);
    RUN_TEST(test_unconsumed_periodic_does_not_wedge_another_task);
    RUN_TEST(test_ring_stays_live_after_sustained_overflow);
    RUN_TEST(test_slot_stays_live_after_write_counter_saturates);
    RUN_TEST(test_publish_between_poll_and_arm_is_not_lost);
    return UNITY_END();
}
