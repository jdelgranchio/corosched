// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// The scheduler's diagnostics, as tasks read them through `stats()`.

#include <unity.h>
#include "corosched/corosched.hpp"

#include <cstdint>

using namespace corosched;

namespace {
    stataider::tick_t g_now = 0;
}

// Both tick hooks replaced, so the segments are timed in ticks the tasks control. kick() starts the source.
stataider::tick_t stataider_ticks() noexcept { return g_now; }
unsigned stataider_start_ticks() noexcept { return 64; }

namespace {
    corosched::task busy(corosched::task_frame<1024> &f, int &left) {
        (void) f;
        while (left-- > 0) {
            g_now += 7; // this segment's cost
            co_await yield();
        }
    }
}

void setUp(void) { g_now = 0; }

void tearDown(void) {}

void test_stats_count_each_task_segments_and_idle(void) {
    int a_left = 3, b_left = 1;
    corosched::task_frame<1024> a_buf, b_buf;
    corosched::scheduler sched{busy(a_buf, a_left), busy(b_buf, b_left)};

    sched.kick();
    for (int i = 0; i < 16; ++i) sched.step();

    const auto &stats = sched.stats();
    // kick() plus one resume per yield, the last one running off the end
    TEST_ASSERT_EQUAL_UINT32(4, stats.tasks[0].count);
    TEST_ASSERT_EQUAL_UINT32(2, stats.tasks[1].count);
    TEST_ASSERT_EQUAL_UINT64(3 * 7, stats.tasks[0].total);
    TEST_ASSERT_EQUAL_UINT64(7, stats.tasks[0].worst);
    TEST_ASSERT_EQUAL_UINT64(7, stats.tasks[1].total);
    TEST_ASSERT_TRUE(stats.idle > 0);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_stats_count_each_task_segments_and_idle);
    return UNITY_END();
}
