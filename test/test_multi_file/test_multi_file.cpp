// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// A task defined in one file, run by a scheduler in another (see task.cpp). The
// awaiters and the scheduler must agree on the running task across files.

#include <corosched/corosched.hpp>
#include <unity.h>

#include "shared.hpp"

using namespace corosched;

void test_task_in_another_file_is_woken_and_gets_the_value(void) {
    slot<int> slot;
    task_frame<1024> frame;
    int got = 0;
    scheduler sched{wait_for_value(frame, slot, got)};

    sched.kick();
    TEST_ASSERT_EQUAL_INT(0, got);

    slot.publish(42);
    sched.step();
    TEST_ASSERT_EQUAL_INT(42, got);
}

namespace {
    task wait_on_built_awaiter(task_frame<1024> &, proactor_awaiter<slot<int>> awaiter, int &out) { out = (co_await awaiter).data; }
}

void test_awaiter_built_outside_the_task_wakes_the_task_awaiting_it(void) {
    slot<int> input, other_input;
    task_frame<1024> frame, other_frame;
    int got = 0, other_got = 0;
    // Bit 0 goes to a task that never wakes, so a wrong bit would be noticed.
    scheduler sched{wait_for_value(other_frame, other_input, other_got), wait_on_built_awaiter(frame, input.next(), got)};

    sched.kick();
    input.publish(7);
    sched.step();
    TEST_ASSERT_EQUAL_INT(7, got);
    TEST_ASSERT_EQUAL_INT(0, other_got);
}

void test_two_schedulers_keep_their_tasks_apart(void) {
    slot<int> slot_a, slot_b;
    task_frame<1024> frame_a, frame_b;
    int got_a = 0, got_b = 0;
    scheduler a{wait_for_value(frame_a, slot_a, got_a)};
    scheduler b{wait_for_value(frame_b, slot_b, got_b)};

    a.kick();
    b.kick();
    slot_b.publish(2);
    a.step(); // nothing for this one
    TEST_ASSERT_EQUAL_INT(0, got_b);
    b.step();
    TEST_ASSERT_EQUAL_INT(2, got_b);
    slot_a.publish(1);
    a.step();
    TEST_ASSERT_EQUAL_INT(1, got_a);
}

void setUp(void) {}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_task_in_another_file_is_woken_and_gets_the_value);
    RUN_TEST(test_awaiter_built_outside_the_task_wakes_the_task_awaiting_it);
    RUN_TEST(test_two_schedulers_keep_their_tasks_apart);
    return UNITY_END();
}
