// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// periodic: tick-driven no-drift multishot proactor.
#include <unity.h>
#include "corosched/core/scheduler.hpp"
#include "corosched/proactors/delay.hpp"
#include "corosched/proactors/periodic.hpp"
#include "corosched/proactors/time.hpp"

#include <cstdint>
#include <ratio>

using namespace corosched;
using namespace corosched::literals;

// ─── fake periodic_clock ────────────────────────────────────────────────────
//
// A test double satisfying `periodic_clock`. Its `tick_list` base holds the
// hooks and provides `tick()`, which the tests call to drive one tick as if
// from the timer interrupt.

namespace {
    struct fake_clock : corosched::tick_list {
        using time_base = corosched::time_base<std::ratio<1, 1'000'000>>; // 1 µs tick
    };

    static_assert(periodic_clock<fake_clock>);
}

// ─── task fixture ───────────────────────────────────────────────────────────

namespace {
    std::uint32_t g_delay = 999;
    int g_runs = 0;
}

template<nanos Period>
corosched::task periodic_task(corosched::task_frame<1024> &f, periodic<fake_clock, Period> &p) {
    (void) f;
    for (;;) {
        auto r = co_await p.next();
        g_delay = r.delay;
        ++g_runs;
    }
}

template<nanos Duration>
corosched::task delay_task(corosched::task_frame<1024> &f, delay<fake_clock, Duration> &d) {
    (void) f;
    co_await d.next();
    ++g_runs;
}

// ─── tests ──────────────────────────────────────────────────────────────────

void test_periodic_fires_after_exactly_period_ticks(void) {
    fake_clock clk;
    periodic<fake_clock, 5_us> p{clk}; // 5 ticks at 1 µs/tick
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{periodic_task<5_us>(buf, p)};

    sched.kick(); // arms, then suspends
    TEST_ASSERT_EQUAL_INT(0, g_runs);

    for (int i = 0; i < 4; ++i) { // 4 ticks — not yet
        clk.tick();
        sched.step();
        TEST_ASSERT_EQUAL_INT(0, g_runs);
    }

    clk.tick(); // 5th tick — fires
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs);
    TEST_ASSERT_EQUAL_UINT32(0u, g_delay); // on time
}

void test_periodic_delay_reports_late_consume(void) {
    // The consumer is slow: extra ticks accumulate after the boundary and
    // `delay` measures the lateness in ticks.
    fake_clock clk;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{periodic_task<5_us>(buf, p)};

    sched.kick();
    for (int i = 0; i < 5; ++i) clk.tick(); // hit the boundary
    for (int i = 0; i < 3; ++i) clk.tick(); // 3 ticks of lateness
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs);
    TEST_ASSERT_EQUAL_UINT32(3u, g_delay);
}

void test_periodic_no_drift_across_late_consume(void) {
    // After a late consume, the next fire stays aligned to the original grid:
    // `period_ticks - 3` more ticks (not a fresh full period) until next fire.
    fake_clock clk;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{periodic_task<5_us>(buf, p)};

    sched.kick();
    for (int i = 0; i < 8; ++i) clk.tick(); // 5 + 3 lateness
    sched.step();
    TEST_ASSERT_EQUAL_UINT32(3u, g_delay);

    // 2 more ticks (5 - 3) should land on the next grid boundary.
    clk.tick();
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs); // not yet
    clk.tick();
    sched.step();
    TEST_ASSERT_EQUAL_INT(2, g_runs); // fires
    TEST_ASSERT_EQUAL_UINT32(0u, g_delay); // back on grid
}

void test_periodic_survives_lateness_of_a_full_period(void) {
    // Regression: lateness of exactly one full period used to leave the
    // countdown parked at 0 after the reload, so the `== 0` crossing never
    // came round again and the task was never woken for the rest of the run.
    fake_clock clk;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{periodic_task<5_us>(buf, p)};

    sched.kick();
    for (int i = 0; i < 10; ++i) clk.tick(); // 5 + 5 lateness
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs);
    TEST_ASSERT_EQUAL_UINT32(5u, g_delay);

    // The proactor must keep firing on the grid: tick 15 is the next boundary.
    for (int i = 0; i < 4; ++i) {
        clk.tick();
        sched.step();
        TEST_ASSERT_EQUAL_INT(1, g_runs);
    }
    clk.tick();
    sched.step();
    TEST_ASSERT_EQUAL_INT(2, g_runs);
    TEST_ASSERT_EQUAL_UINT32(0u, g_delay);
}

void test_periodic_survives_stall_of_many_periods(void) {
    // A long stall (an ISR storm starving the task) crosses many boundaries.
    // They collapse into a single fire, `delay` reports the whole stall, and
    // the proactor stays alive and on-grid afterwards.
    fake_clock clk;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{periodic_task<5_us>(buf, p)};

    sched.kick();
    for (int i = 0; i < 5 + 97; ++i) clk.tick(); // boundary, then 97 ticks starved
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs); // one fire, not 20
    TEST_ASSERT_EQUAL_UINT32(97u, g_delay);

    // 102 ticks in; the next grid boundary is at 105.
    for (int i = 0; i < 2; ++i) {
        clk.tick();
        sched.step();
        TEST_ASSERT_EQUAL_INT(1, g_runs);
    }
    clk.tick();
    sched.step();
    TEST_ASSERT_EQUAL_INT(2, g_runs);
    TEST_ASSERT_EQUAL_UINT32(0u, g_delay);

    // And it keeps running indefinitely.
    for (int i = 0; i < 20; ++i) {
        clk.tick();
        sched.step();
    }
    TEST_ASSERT_EQUAL_INT(6, g_runs); // 4 further boundaries
}

void test_periodic_survives_stall_with_no_consumer(void) {
    // Nobody ever consumes: the countdown must not run away (it used to
    // decrement without bound, overflowing its counter). Once the task does
    // get to run, the proactor is still on its grid.
    fake_clock clk;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{periodic_task<5_us>(buf, p)};

    sched.kick();
    for (int i = 0; i < 100'000; ++i) clk.tick(); // would blow past INT32_MIN eventually
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs);
    TEST_ASSERT_EQUAL_UINT32(100'000u - 5u, g_delay);

    for (int i = 0; i < 5; ++i) {
        clk.tick();
        sched.step();
    }
    TEST_ASSERT_EQUAL_INT(2, g_runs);
    TEST_ASSERT_EQUAL_UINT32(0u, g_delay);
}

void test_periodic_disarm_on_destruction(void) {
    fake_clock clk;
    {
        periodic<fake_clock, 1_us> p{clk};
        TEST_ASSERT_FALSE(clk.empty());
    }
    TEST_ASSERT_TRUE(clk.empty());
}

// ─── delay tests ────────────────────────────────────────────────────────────

void test_delay_fires_after_duration_plus_one_tick(void) {
    // sleep(5_us) on a 1 µs tick clock counts 5 + 1 = 6 ticks to guarantee
    // sleep >= 5 µs irrespective of the phase at arm.
    fake_clock clk;
    delay<fake_clock, 5_us> d{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{delay_task<5_us>(buf, d)};

    sched.kick();
    TEST_ASSERT_EQUAL_INT(0, g_runs);

    for (int i = 0; i < 5; ++i) {
        clk.tick();
        sched.step();
        TEST_ASSERT_EQUAL_INT(0, g_runs);
    }

    clk.tick(); // 6th tick — fires
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs);
}

void test_delay_is_single_shot(void) {
    // Extra ticks after the fire are no-ops; the task is done.
    fake_clock clk;
    delay<fake_clock, 2_us> d{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{delay_task<2_us>(buf, d)};

    sched.kick();
    for (int i = 0; i < 3; ++i) clk.tick(); // 2 + 1 = 3 ticks → fires
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs);

    for (int i = 0; i < 10; ++i) clk.tick(); // no further effect
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_runs);
}

void test_delay_disarm_on_destruction(void) {
    fake_clock clk;
    {
        delay<fake_clock, 1_us> d{clk};
        TEST_ASSERT_FALSE(clk.empty());
    }
    TEST_ASSERT_TRUE(clk.empty());
}

// ─── tick_list ──────────────────────────────────────────────────────────────

namespace {
    // A hook that counts its calls and asks to be called again after `every` ticks, or to leave the list once `left` calls are done.
    struct tick_counter {
        std::uint32_t every = 1;
        int left = -1; // negative: never leave
        int n = 0;
        std::uint32_t on_due() noexcept {
            ++n;
            if (left > 0 && --left == 0) return 0;
            return every;
        }
    };
}

void test_one_clock_drives_periodic_and_delay(void) {
    fake_clock clk;
    periodic<fake_clock, 2_us> p{clk};
    delay<fake_clock, 3_us> d{clk};

    clk.tick();
    clk.tick();
    TEST_ASSERT_TRUE(p.ready());
    TEST_ASSERT_FALSE(d.ready());
    (void) p.produce();

    clk.tick();
    clk.tick(); // 3 + 1 ticks for the delay, a second period for the periodic
    TEST_ASSERT_TRUE(d.ready());
    TEST_ASSERT_TRUE(p.ready());
}

void test_tick_list_attach_and_detach(void) {
    tick_list list;
    tick_counter a, b, c;
    tick_hook ha{&a, member_tick<&tick_counter::on_due>};
    tick_hook hb{&b, member_tick<&tick_counter::on_due>};
    tick_hook hc{&c, member_tick<&tick_counter::on_due>};
    TEST_ASSERT_TRUE(list.empty());

    list.attach(ha, 1);
    list.attach(hb, 1);
    list.attach(hc, 1);
    list.tick();
    TEST_ASSERT_EQUAL_INT(1, a.n);
    TEST_ASSERT_EQUAL_INT(1, b.n);
    TEST_ASSERT_EQUAL_INT(1, c.n);

    list.detach(hb); // from the middle
    list.detach(hb); // on no list now: nothing changes
    list.tick();
    TEST_ASSERT_EQUAL_INT(2, a.n);
    TEST_ASSERT_EQUAL_INT(1, b.n);
    TEST_ASSERT_EQUAL_INT(2, c.n);

    list.detach(hc); // the head
    list.detach(ha);
    TEST_ASSERT_TRUE(list.empty());
    list.tick();
    TEST_ASSERT_EQUAL_INT(2, a.n);
}

void test_tick_list_calls_each_hook_when_due(void) {
    tick_list list;
    tick_counter every2{.every = 2}, every3{.every = 3};
    tick_hook h2{&every2, member_tick<&tick_counter::on_due>};
    tick_hook h3{&every3, member_tick<&tick_counter::on_due>};
    list.attach(h2, 2);
    list.attach(h3, 3);

    for (int i = 0; i < 12; ++i) list.tick();
    TEST_ASSERT_EQUAL_INT(6, every2.n); // ticks 2, 4, ..., 12
    TEST_ASSERT_EQUAL_INT(4, every3.n); // ticks 3, 6, 9, 12
    TEST_ASSERT_EQUAL_UINT32(12u, list.now());
}

void test_tick_list_hook_leaves_by_returning_zero(void) {
    tick_list list;
    tick_counter before, leaver{.left = 2}, after;
    tick_hook h_after{&after, member_tick<&tick_counter::on_due>};
    tick_hook h_leaver{&leaver, member_tick<&tick_counter::on_due>};
    tick_hook h_before{&before, member_tick<&tick_counter::on_due>};

    // The newest hook is first on the list, so the walk goes h_before, h_leaver, h_after.
    list.attach(h_after, 1);
    list.attach(h_leaver, 1);
    list.attach(h_before, 1);

    list.tick();
    list.tick(); // h_leaver returns 0 here and is unlinked mid-walk; h_after must still be called
    TEST_ASSERT_EQUAL_INT(2, before.n);
    TEST_ASSERT_EQUAL_INT(2, leaver.n);
    TEST_ASSERT_EQUAL_INT(2, after.n);
    TEST_ASSERT_FALSE(h_leaver.linked());

    list.tick();
    TEST_ASSERT_EQUAL_INT(3, before.n);
    TEST_ASSERT_EQUAL_INT(2, leaver.n);
    TEST_ASSERT_EQUAL_INT(3, after.n);

    list.detach(h_leaver); // already off the list: nothing changes
    list.attach(h_leaver, 1); // and it can be attached again
    list.tick();
    TEST_ASSERT_EQUAL_INT(3, leaver.n);
}

// ─── compile-time multiplicity (only enabled on demand — these MUST fail) ───
//
// Uncomment either line to verify the static_assert fires:
//
//   periodic<fake_clock, nanos{500}> bad_period{};     // 500 ns not a multiple of 1 µs
//   periodic<fake_clock, nanos{0}>   bad_zero{};       // non-positive
//
// Both paths trigger the assert in `to_ticks`; a clock has no check of
// its own to write. See test_stall.cpp for why a non-positive period has to be
// rejected.

void setUp(void) {
    g_delay = 999;
    g_runs = 0;
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_periodic_fires_after_exactly_period_ticks);
    RUN_TEST(test_periodic_delay_reports_late_consume);
    RUN_TEST(test_periodic_no_drift_across_late_consume);
    RUN_TEST(test_periodic_survives_lateness_of_a_full_period);
    RUN_TEST(test_periodic_survives_stall_of_many_periods);
    RUN_TEST(test_periodic_survives_stall_with_no_consumer);
    RUN_TEST(test_periodic_disarm_on_destruction);
    RUN_TEST(test_delay_fires_after_duration_plus_one_tick);
    RUN_TEST(test_delay_is_single_shot);
    RUN_TEST(test_delay_disarm_on_destruction);
    RUN_TEST(test_one_clock_drives_periodic_and_delay);
    RUN_TEST(test_tick_list_attach_and_detach);
    RUN_TEST(test_tick_list_calls_each_hook_when_due);
    RUN_TEST(test_tick_list_hook_leaves_by_returning_zero);
    return UNITY_END();
}
