// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Replacing one tick hook but not the other. Its own program, as the replacement is link-wide.

#include <unity.h>
#include "stataider/stataider.hpp"

// Only the read is replaced, so the default start would report the width of the cycle counter or of steady_clock.
stataider::tick_t stataider_ticks() noexcept { return 0; }

void setUp(void) {}

void tearDown(void) {}

void test_replacing_one_hook_is_refused(void) {
#if defined(STATAIDER_HAS_DEFAULT_TICKS) && defined(__ELF__)
    TEST_ASSERT_FALSE(stataider::start_source());
    TEST_ASSERT_EQUAL_UINT64(0, stataider::elapsed(0, 100)); // timing stays off
#else
    TEST_IGNORE_MESSAGE("the check needs ELF aliases and a default source");
#endif
}

int main() {
    UNITY_BEGIN();
    RUN_TEST(test_replacing_one_hook_is_refused);
    return UNITY_END();
}
