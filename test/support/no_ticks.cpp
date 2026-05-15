// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

// Tick hooks for the Cortex-M test builds on cores with no default source, such as the M0, which link the suite without running it. Weak, so
// that the tests defining their own hooks keep them. The cortex_m section of platformio.ini adds this file.

#include <stataider/monotonic.hpp>

#ifndef STATAIDER_HAS_DEFAULT_TICKS

[[gnu::weak]] stataider::tick_t stataider_ticks() noexcept { return 0; }
[[gnu::weak]] unsigned stataider_start_ticks() noexcept { return 0; }

#endif
