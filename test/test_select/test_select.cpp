// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// select over proactor branches: winner pick + no-carry teardown.
#include <unity.h>
#include "corosched/corosched.hpp"

#include <cstdint>
#include <variant>

using namespace corosched;

namespace {
    int g_idx = -1;
    int g_val = -1;
    bool g_done = false;
}

corosched::task selector(corosched::task_frame<1024> &f, slot<int> &a, slot<int> &b) {
    (void) f;
    auto ra = a.next();
    auto rb = b.next();
    const auto result = co_await corosched::select(ra, rb);
    g_idx = static_cast<int>(result.index());
    g_val = (result.index() == 0) ? std::get<0>(result).data : std::get<1>(result).data;
    g_done = true;
}

void test_select_first_branch_wins(void) {
    slot<int> a;
    slot<int> b;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{selector(buf, a, b)};

    sched.kick(); // both branches arm, suspend
    TEST_ASSERT_FALSE(g_done);
    a.publish(10); // branch 0 fires
    sched.step();

    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(0, g_idx);
    TEST_ASSERT_EQUAL_INT(10, g_val);
}

void test_select_second_branch_wins(void) {
    slot<int> a;
    slot<int> b;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{selector(buf, a, b)};

    sched.kick();
    b.publish(20); // only branch 1 fires
    sched.step();

    TEST_ASSERT_EQUAL_INT(1, g_idx);
    TEST_ASSERT_EQUAL_INT(20, g_val);
}

void test_select_lowest_index_when_both_fire(void) {
    slot<int> a;
    slot<int> b;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{selector(buf, a, b)};

    sched.kick();
    b.publish(2);
    a.publish(1); // both fired before resume
    sched.step();

    // Lowest-index ready branch wins on resume (select.hpp polls in order).
    TEST_ASSERT_EQUAL_INT(0, g_idx);
    TEST_ASSERT_EQUAL_INT(1, g_val);
}

void test_select_short_circuits_on_ready_branch(void) {
    slot<int> a;
    slot<int> b;
    b.publish(7); // branch 1 already has a value
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{selector(buf, a, b)};

    sched.kick(); // synchronous — no suspend
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(1, g_idx);
    TEST_ASSERT_EQUAL_INT(7, g_val);
}

void test_select_no_carry_after_teardown(void) {
    // A losing branch is disarmed during teardown — a later publish to it
    // targets a default waker and is a safe no-op (no crash, no stray bit).
    slot<int> a;
    slot<int> b;
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{selector(buf, a, b)};

    sched.kick();
    b.publish(20); // branch 1 wins
    sched.step();
    TEST_ASSERT_EQUAL_INT(1, g_idx);

    a.publish(99); // loser — post-teardown activity
    sched.step(); // must not resume a done task
    TEST_ASSERT_TRUE(g_done);
}

void setUp(void) {
    g_idx = -1;
    g_val = -1;
    g_done = false;
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_select_first_branch_wins);
    RUN_TEST(test_select_second_branch_wins);
    RUN_TEST(test_select_lowest_index_when_both_fire);
    RUN_TEST(test_select_short_circuits_on_ready_branch);
    RUN_TEST(test_select_no_carry_after_teardown);
    return UNITY_END();
}
