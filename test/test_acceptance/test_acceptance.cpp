// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

/**
 * Acceptance test.
 *
 * Drive a real scheduler with two real coroutine tasks, inject a CAN-heavy
 * burst and a periodic phasing collision, and confirm that EVERY resulting
 * degradation surfaces in the consumer's resume value: `slot`'s
 * `result_type::writes > 1` reports coalesced publishes, and `periodic`'s
 * `result_type::delay > 0` reports a late consume. A degradation that cannot be
 * accounted for would mean observability is not done.
 */
#include <unity.h>
#include "corosched/corosched.hpp"

#include <cstdint>
#include <ratio>

using namespace corosched;
using namespace corosched::literals;

namespace {
    // Observation of what the tasks actually did.
    long g_rx_sum = 0;
    int g_rx_count = 0;
    std::uint32_t g_rx_writes_max = 0;
    int g_ctrl_runs = 0;
    std::uint32_t g_ctrl_delay_max = 0;

    // ── fake periodic_clock ─────────────────────────────────────────────────
    // tick() comes from tick_list: the test calls it to drive one tick as if from the timer interrupt.
    struct fake_clock : corosched::tick_list {
        using time_base = corosched::time_base<std::ratio<1, 1'000'000>>; // 1 µs tick
    };
    static_assert(periodic_clock<fake_clock>);
}

// High-priority CAN consumer (bit 0): drains a last-wins slot.
corosched::task can_consumer(corosched::task_frame<1024> &self, slot<int> &can) {
    (void) self;
    for (;;) {
        const auto frame = co_await can.next();
        g_rx_sum += frame.data;
        ++g_rx_count;
        if (frame.writes > g_rx_writes_max) g_rx_writes_max = frame.writes;
    }
}

// Control loop (bit 1): a no-drift periodic.
corosched::task ctrl_loop(corosched::task_frame<1024> &self, periodic<fake_clock, 10_us> &p) {
    (void) self;
    for (;;) {
        const auto r = co_await p.next();
        ++g_ctrl_runs;
        if (r.delay > g_ctrl_delay_max) g_ctrl_delay_max = r.delay;
    }
}

void test_acceptance_burst_and_phasing(void) {
    fake_clock clk;
    slot<int> can;
    periodic<fake_clock, 10_us> p{clk}; // 10 ticks/period
    corosched::task_frame<1024> can_buf;
    corosched::task_frame<1024> ctrl_buf;

    corosched::scheduler sched{
            can_consumer(can_buf, can),
            ctrl_loop(ctrl_buf, p),
    };
    sched.kick(); // both tasks arm and suspend

    // ── Inject a CAN-heavy burst: 5 frames land before the consumer runs ──
    // The slot is last-wins: 4 frames are coalesced. They must be COUNTED in
    // `result_type::writes`, not silently dropped.
    for (int v = 1; v <= 5; ++v) {
        can.publish(v * 10);
    }

    // ── Inject a phasing collision: the timer fires past one period boundary
    // before the control task gets to consume — `delay` must reflect it.
    for (int i = 0; i < 14; ++i) clk.tick(); // 10 ticks + 4 lateness

    // ── Drain the scheduler ──
    for (int i = 0; i < 8; ++i) {
        sched.step();
    }

    // The consumer received exactly ONE frame, and it is the FRESHEST one
    // (50) — no stale frame, no torn garbage propagated downstream — and the
    // five-into-one coalescing surfaced as writes==5 on the consume side.
    TEST_ASSERT_EQUAL_INT(1, g_rx_count);
    TEST_ASSERT_EQUAL_INT(50, g_rx_sum);
    TEST_ASSERT_EQUAL_UINT32(5u, g_rx_writes_max);

    // The phasing collision surfaced as a non-zero `delay` on the periodic's
    // consume side.
    TEST_ASSERT_EQUAL_INT(1, g_ctrl_runs);
    TEST_ASSERT_EQUAL_UINT32(4u, g_ctrl_delay_max);
}

void setUp(void) {
    g_rx_sum = 0;
    g_rx_count = 0;
    g_rx_writes_max = 0;
    g_ctrl_runs = 0;
    g_ctrl_delay_max = 0;
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_acceptance_burst_and_phasing);
    return UNITY_END();
}
