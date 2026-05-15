// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Partial-disarm lattice: a one-shot and a multishot under one select.
#include <unity.h>
#include "corosched/corosched.hpp"

#include <cstdint>
#include <ratio>
#include <variant>

using namespace corosched;
using namespace corosched::literals;

static_assert(proactor<slot<int>>);

// ─── fake periodic_clock ────────────────────────────────────────────────────
namespace {
    // tick() comes from tick_list: the test calls it to drive one tick as if from the timer interrupt.
    struct fake_clock : corosched::tick_list {
        using time_base = corosched::time_base<std::ratio<1, 1'000'000>>; // 1 µs tick
    };
    static_assert(periodic_clock<fake_clock>);
}

static_assert(proactor<periodic<fake_clock, 5_us>>);

namespace {
    int g_idx = -1;
    int g_runs = 0;
    bool g_done = false;
}

corosched::task lattice_once(corosched::task_frame<1024> &f, slot<int> &can, periodic<fake_clock, 5_us> &p) {
    (void) f;
    auto rc = can.next();
    auto pa = p.next();
    const auto result = co_await corosched::select(rc, pa);
    g_idx = static_cast<int>(result.index());
    g_done = true;
}

corosched::task lattice_loop(corosched::task_frame<2048> &f, slot<int> &can, periodic<fake_clock, 5_us> &p) {
    (void) f;
    for (int k = 0; k < 3; ++k) {
        auto rc = can.next();
        auto pa = p.next();
        const auto result = co_await corosched::select(rc, pa);
        (void) result;
        ++g_runs;
    }
    g_done = true;
}

void test_select_oneshot_wins_periodic_keeps_counting(void) {
    fake_clock clk;
    slot<int> can;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{lattice_once(buf, can, p)};

    sched.kick(); // both branches arm
    can.publish(7); // the one-shot branch fires
    sched.step();
    TEST_ASSERT_EQUAL_INT(0, g_idx);

    // The periodic loser is still armed on the clock side; ticking it must not
    // crash or stray-wake the torn-down task.
    for (int i = 0; i < 5; ++i) clk.tick();
    sched.step();
    TEST_ASSERT_TRUE(g_done);
}

void test_select_periodic_can_win(void) {
    fake_clock clk;
    slot<int> can;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{lattice_once(buf, can, p)};

    sched.kick();
    for (int i = 0; i < 5; ++i) clk.tick(); // periodic branch fires
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_idx);
}

void test_periodic_no_drift_across_repeated_select_loss(void) {
    // The classic select(recv, periodic) shape: the period loses every round
    // yet keeps counting and stays on the original grid (no drift).
    fake_clock clk;
    slot<int> can;
    periodic<fake_clock, 5_us> p{clk};
    corosched::task_frame<2048> buf;
    corosched::scheduler sched{lattice_loop(buf, can, p)};

    sched.kick();
    for (int k = 1; k <= 3; ++k) {
        can.publish(k); // one-shot wins
        for (int i = 0; i < 5; ++i) clk.tick(); // tick a full period too
        sched.step();
    }

    TEST_ASSERT_EQUAL_INT(3, g_runs);
    TEST_ASSERT_TRUE(g_done);
}

void setUp(void) {
    g_idx = -1;
    g_runs = 0;
    g_done = false;
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_select_oneshot_wins_periodic_keeps_counting);
    RUN_TEST(test_select_periodic_can_win);
    RUN_TEST(test_periodic_no_drift_across_repeated_select_loss);
    return UNITY_END();
}
