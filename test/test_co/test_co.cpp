// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Nested `co<T>` coroutines: explicit `co_frame<N>` storage, symmetric
// transfer, and value passthrough. A `co_frame` is single-use: reusing it
// traps with `fault::frame_reused`. Loop callers declare a fresh frame per
// iteration. Coverage:
//
//   - co<int>     — non-void return value propagates back to the caller
//   - co<void>    — final_suspend transfers control without a value
//   - chained co<T> — deep symmetric-transfer chains stay flat
//   - co<T> reused inside a loop via per-iteration frames
//   - co<T> awaiting a proactor — suspension nests cleanly through the chain

#include <unity.h>
#include "corosched/corosched.hpp"

#include <cstdint>

using namespace corosched;

namespace {
    int g_int_result = -1;
    int g_void_marker = 0;
    int g_chain_result = -1;
    int g_loop_sum = 0;
    int g_proactor_val = -1;
    bool g_done = false;
}

// ── 1. co<int> returning a value ────────────────────────────────────────────
static co<int> compute_42(co_frame<128> &f) noexcept {
    (void) f;
    co_return 42;
}

corosched::task task_int(corosched::task_frame<1024> &self) {
    (void) self;
    co_frame<128> f;
    g_int_result = co_await compute_42(f);
    g_done = true;
}

void test_co_int_return_value(void) {
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{task_int(buf)};
    sched.kick();
    sched.step();
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(42, g_int_result);
}

// ── 2. co<void> — no value, just sequencing ─────────────────────────────────
static co<void> touch_marker(co_frame<128> &f) noexcept {
    (void) f;
    g_void_marker = 7;
    co_return;
}

corosched::task task_void(corosched::task_frame<1024> &self) {
    (void) self;
    co_frame<128> f;
    co_await touch_marker(f);
    g_done = true;
}

void test_co_void_executes(void) {
    corosched::task_frame<1024> buf;
    corosched::scheduler sched{task_void(buf)};
    sched.kick();
    sched.step();
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(7, g_void_marker);
}

// ── 3. Chained co<int>: deep symmetric-transfer chain ───────────────────────
static co<int> add1(co_frame<128> &f, int x) noexcept {
    (void) f;
    co_return x + 1;
}
static co<int> add5(co_frame<2048> &f, int x) noexcept {
    (void) f;
    co_frame<128> f1, f2, f3, f4, f5;
    const int a = co_await add1(f1, x);
    const int b = co_await add1(f2, a);
    const int c = co_await add1(f3, b);
    const int d = co_await add1(f4, c);
    const int e = co_await add1(f5, d);
    co_return e;
}

corosched::task task_chain(corosched::task_frame<4096> &self) {
    (void) self;
    co_frame<2048> outer;
    g_chain_result = co_await add5(outer, 10);
    g_done = true;
}

void test_co_chained_symmetric_transfer(void) {
    corosched::task_frame<4096> buf;
    corosched::scheduler sched{task_chain(buf)};
    sched.kick();
    sched.step();
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(15, g_chain_result);
}

// ── 4. co<T> reused inside a loop — fresh frame per iteration ───────────────
static co<int> identity(co_frame<128> &f, int x) noexcept {
    (void) f;
    co_return x;
}

corosched::task task_loop(corosched::task_frame<2048> &self) {
    (void) self;
    for (int i = 1; i <= 5; ++i) {
        co_frame<128> f;
        g_loop_sum += co_await identity(f, i);
    }
    g_done = true;
}

void test_co_loop_reentry(void) {
    corosched::task_frame<2048> buf;
    corosched::scheduler sched{task_loop(buf)};
    sched.kick();
    sched.step();
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(1 + 2 + 3 + 4 + 5, g_loop_sum);
}

// ── 5. co<T> awaiting a proactor: suspension nests through the chain ─────────
//      The leaf awaiter records the nested coroutine's handle into
//      the task's state; on wake the scheduler resumes that handle
//      directly, not the root. When nested completes, final_awaiter restores
//      the root handle so subsequent suspensions resume the root.
static co<int> read_slot(co_frame<128> &f, slot<int> &s) noexcept {
    (void) f;
    const auto r = co_await s.next();
    co_return r.data;
}

corosched::task task_proactor(corosched::task_frame<2048> &self, slot<int> &s) {
    (void) self;
    co_frame<128> f;
    g_proactor_val = co_await read_slot(f, s);
    g_done = true;
}

void test_co_synchronous_proactor_read(void) {
    slot<int> s;
    s.publish(123); // value already pending — co_await takes fast path
    corosched::task_frame<2048> buf;
    corosched::scheduler sched{task_proactor(buf, s)};

    sched.kick(); // task runs straight through — nested completes
    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(123, g_proactor_val);
}

void test_co_suspends_through_nested(void) {
    slot<int> s;
    corosched::task_frame<2048> buf;
    corosched::scheduler sched{task_proactor(buf, s)};

    sched.kick(); // task suspends inside nested co<int>
    TEST_ASSERT_FALSE(g_done);
    s.publish(456); // wakes the awaiter at the bottom of the chain
    sched.step(); // resume re-enters nested, cascades back up

    TEST_ASSERT_TRUE(g_done);
    TEST_ASSERT_EQUAL_INT(456, g_proactor_val);
}

void setUp(void) {
    g_int_result = -1;
    g_void_marker = 0;
    g_chain_result = -1;
    g_loop_sum = 0;
    g_proactor_val = -1;
    g_done = false;
}

void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_co_int_return_value);
    RUN_TEST(test_co_void_executes);
    RUN_TEST(test_co_chained_symmetric_transfer);
    RUN_TEST(test_co_loop_reentry);
    RUN_TEST(test_co_synchronous_proactor_read);
    RUN_TEST(test_co_suspends_through_nested);
    return UNITY_END();
}
