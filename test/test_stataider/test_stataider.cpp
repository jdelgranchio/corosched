// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Elapsed ticks across a counter wrap, and what each aggregate keeps.

#include <unity.h>
#include "stataider/stataider.hpp"

#include <cstdint>
#include <initializer_list>

namespace {
    stataider::tick_t g_now = 0;
    unsigned g_bits = 64;
}

// Both hooks replaced, as an application does.
stataider::tick_t stataider_ticks() noexcept { return g_now; }
unsigned stataider_start_ticks() noexcept { return g_bits; }

namespace {
    void start_with_width(const unsigned bits) {
        g_bits = bits;
        TEST_ASSERT_TRUE(stataider::start_source());
    }
}

void setUp(void) { g_now = 0; }

void tearDown(void) {}

void test_a_scope_across_a_wrap_counts_the_ticks_in_it(void) {
    start_with_width(8);
    stataider::counter_ticks_max stats;
    g_now = 250;
    {
        auto sample = stataider::record(stats);
        g_now = 4; // 255 -> 0 in between
    }
    TEST_ASSERT_EQUAL_UINT64(10, stats.total);
    TEST_ASSERT_EQUAL_UINT64(10, stats.worst);
}

void test_a_full_width_source_is_unmasked(void) {
    start_with_width(64);
    stataider::ticks stats;
    g_now = 1;
    {
        auto sample = stataider::record(stats);
        g_now = stataider::tick_t{1} << 40;
    }
    TEST_ASSERT_EQUAL_UINT64((stataider::tick_t{1} << 40) - 1, stats.total);
}

void test_the_start_hook_gives_the_width(void) {
    start_with_width(32);
    TEST_ASSERT_EQUAL_UINT64(1, stataider::elapsed(0xFFFF'FFFF, 0));
    start_with_width(16); // starting again takes the new width
    TEST_ASSERT_EQUAL_UINT64(1, stataider::elapsed(0xFFFF, 0));
}

void test_a_zero_width_turns_timing_off(void) {
    start_with_width(0);
    stataider::counter_ticks_max stats;
    {
        auto sample = stataider::record(stats);
        g_now = 1000;
    }
    TEST_ASSERT_EQUAL_UINT64(0, stats.total);
    TEST_ASSERT_EQUAL_UINT32(1, stats.count);
}

void test_each_aggregate_keeps_what_it_names(void) {
    start_with_width(64);
    stataider::ticks t;
    stataider::ticks_max tm;
    stataider::counter_ticks ct;
    stataider::counter_ticks_max ctm;
    for (const stataider::tick_t cost: {stataider::tick_t{3}, stataider::tick_t{5}}) {
        g_now = 0;
        auto a = stataider::record(t);
        auto b = stataider::record(tm);
        auto c = stataider::record(ct);
        auto d = stataider::record(ctm);
        g_now = cost;
    }
    TEST_ASSERT_EQUAL_UINT64(8, t.total);
    TEST_ASSERT_EQUAL_UINT64(8, tm.total);
    TEST_ASSERT_EQUAL_UINT64(5, tm.worst);
    TEST_ASSERT_EQUAL_UINT64(8, ct.total);
    TEST_ASSERT_EQUAL_UINT32(2, ct.count);
    TEST_ASSERT_EQUAL_UINT64(8, ctm.total);
    TEST_ASSERT_EQUAL_UINT64(5, ctm.worst);
    TEST_ASSERT_EQUAL_UINT32(2, ctm.count);
}

void test_counters_saturate(void) {
    std::uint8_t counter = 255;
    stataider::inc(counter);
    TEST_ASSERT_EQUAL_UINT8(255, counter);
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_a_scope_across_a_wrap_counts_the_ticks_in_it);
    RUN_TEST(test_a_full_width_source_is_unmasked);
    RUN_TEST(test_the_start_hook_gives_the_width);
    RUN_TEST(test_a_zero_width_turns_timing_off);
    RUN_TEST(test_each_aggregate_keeps_what_it_names);
    RUN_TEST(test_counters_saturate);
    return UNITY_END();
}
