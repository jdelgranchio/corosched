// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Core primitives — waker readiness bits and the critical_section nesting.
#include <unity.h>
#include "corosched/core/critical.hpp"
#include "corosched/core/scheduler.hpp"

#include <atomic>
#include <bit>
#include <cstdint>
#ifndef COROSCHED_CORTEX_M
    #include <thread>
#endif

using namespace corosched;

// ─── waker: readiness bit ───────────────────────────────────────────────────

void test_waker_wake_sets_bit(void) {
    runqueue_word word{};
    const waker w{&word, 1u << 3};

    TEST_ASSERT_EQUAL_UINT32(0u, word.load());
    w.wake();
    TEST_ASSERT_EQUAL_UINT32(1u << 3, word.load());
}

void test_waker_multiple_bits_independent(void) {
    runqueue_word word{};

    waker{&word, 1u << 0}.wake();
    waker{&word, 1u << 31}.wake();
    TEST_ASSERT_EQUAL_UINT32((1u | (1u << 31)), word.load());
}

void test_waker_mask_wakes_several_tasks_at_once(void) {
    runqueue_word word{};

    waker{&word, (1u << 2) | (1u << 6)}.wake();
    TEST_ASSERT_EQUAL_UINT32((1u << 2) | (1u << 6), word.load());
}

void test_waker_null_is_a_safe_noop(void) {
    // A proactor may fire with no consumer armed: its `armed_` is a default
    // waker, and wake() on it must be a harmless no-op, not a crash.
    const waker w{};
    TEST_ASSERT_NULL(w.word);
    w.wake(); // must not dereference null
}

void test_waker_equality_matches_slot(void) {
    runqueue_word word_a{};
    runqueue_word word_b{};

    TEST_ASSERT_TRUE((waker{&word_a, 1u << 4} == waker{&word_a, 1u << 4}));
    TEST_ASSERT_FALSE((waker{&word_a, 1u << 4} == waker{&word_a, 1u << 5}));
    TEST_ASSERT_FALSE((waker{&word_a, 1u << 4} == waker{&word_b, 1u << 4}));
}

// ─── runqueue readiness reduction (single word) ─────────────────────────────

void test_runqueue_picks_lowest_bit(void) {
    runqueue_word word{};

    waker{&word, 1u << 6}.wake(); // task 6 ready
    waker{&word, 1u << 2}.wake(); // task 2 ready

    const std::uint32_t eff = word.load();
    TEST_ASSERT_EQUAL_UINT32((1u << 2) | (1u << 6), eff);
    // Lowest set bit == highest priority task.
    TEST_ASSERT_EQUAL_INT(2, std::countr_zero(eff));
}

// ─── critical_section: ISR↔task masking primitive ───────────────────────────

// On a host the section is a recursive lock: nesting must not deadlock, and leaving the outer one must release it for another thread.
void test_critical_section_nesting(void) {
    {
        critical_section cs;
        {
            critical_section nested;
        }
    }

#ifndef COROSCHED_CORTEX_M
    bool entered = false;
    std::thread other{[&entered] {
        critical_section cs;
        entered = true;
    }};
    other.join();
    TEST_ASSERT_TRUE(entered);
#endif
}

void setUp(void) {}
void tearDown(void) {}

int main(void) {
    UNITY_BEGIN();

    RUN_TEST(test_waker_wake_sets_bit);
    RUN_TEST(test_waker_multiple_bits_independent);
    RUN_TEST(test_waker_mask_wakes_several_tasks_at_once);
    RUN_TEST(test_waker_null_is_a_safe_noop);
    RUN_TEST(test_waker_equality_matches_slot);
    RUN_TEST(test_runqueue_picks_lowest_bit);
    RUN_TEST(test_critical_section_nesting);

    return UNITY_END();
}
