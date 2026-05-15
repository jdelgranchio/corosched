// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <cstdint>

#if defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')
    /// Defined when building for a Cortex-M.
    #define STATAIDER_CORTEX_M 1
    #if !defined(__ARM_ARCH_6M__) && !defined(__ARM_ARCH_8M_BASE__)
        /// Defined on architectures whose DWT can have a cycle counter. It is optional even there, so the default source still asks the chip.
        #define STATAIDER_HAS_CYCLE_COUNTER 1
    #endif
#endif

#if defined(STATAIDER_HAS_CYCLE_COUNTER) || !defined(STATAIDER_CORTEX_M)
    /// Defined where stataider has a default tick source:
    /// the DWT cycle counter on a core that can have one, and @c std::chrono::steady_clock on a host.
    /// Elsewhere, such as on a Cortex-M0, the application defines both hooks below, or the link fails.
    #define STATAIDER_HAS_DEFAULT_TICKS 1
#endif

namespace stataider {
    /// A tick count. It is 64 bits wide, which a 32-bit core reads in two halves, so a live read of a 64-bit total can be torn.
    using tick_t = std::uint64_t;
}

/// Read the tick source that timed scopes, and the scheduler's task segments, are measured against.
///
/// This and @c stataider_start_ticks are hooks: where @c STATAIDER_HAS_DEFAULT_TICKS is defined the library's definitions are weak, and an
/// application replaces them by defining its own, always both together. On an ELF target, replacing only one makes @c stataider::start_source
/// return false, since the width would belong to another source. Called directly, not through a pointer, so a read is a plain function call.
///
/// For example, with a 16-bit timer:
/// @code
/// stataider::tick_t stataider_ticks() noexcept { return TIM3->CNT; }
/// unsigned stataider_start_ticks() noexcept { start_tim3(); return 16; }
/// @endcode
stataider::tick_t stataider_ticks() noexcept;

/// Start the tick source and return how many bits it counts before wrapping. 64 means it never wraps; 0 turns timing off, so every elapsed
/// time reads 0. The default returns 32 once it has started the DWT cycle counter, 0 if this chip left the counter out, and 64 on a host,
/// where it reads nanoseconds. Called by @c stataider::start_source.
unsigned stataider_start_ticks() noexcept;

/// Define both hooks with a source that reads 0 and has a width of 0, which turns timing off. Use it at namespace scope in one file of the
/// program, on a core with no default source that has no timer to spare, or anywhere to drop the default:
/// @code
/// STATAIDER_ZERO_TICKS();
/// @endcode
#define STATAIDER_ZERO_TICKS()                                                                                                                       \
    stataider::tick_t stataider_ticks() noexcept { return 0; }                                                                                       \
    unsigned stataider_start_ticks() noexcept { return 0; }                                                                                          \
    static_assert(true, "absorbs the semicolon after the macro")

namespace stataider {
    // The width of the source as a mask, set by start_source and defined in stataider.cpp. Visible here only so that elapsed can be inlined.
    namespace detail {
        extern tick_t mask;
    }

    /// The current tick of the source.
    [[nodiscard]] inline tick_t monotonic_ticks() noexcept { return stataider_ticks(); }

    /// Ticks from @c start to @c now, both read from the source. Taken modulo the source's width, so one wrap in between is handled.
    /// Reads 0 until @c start_source has run.
    [[nodiscard]] inline tick_t elapsed(const tick_t start, const tick_t now) noexcept { return (now - start) & detail::mask; }

    /// Start the source with @c stataider_start_ticks and take its width. The scheduler calls this in @c kick(), before any task runs.
    /// Calling it again starts the source again and takes its width again. Not synchronized with readers, so call it before anything is timed.
    ///
    /// Returns false, leaving timing off, if only one of @c stataider_ticks and @c stataider_start_ticks was replaced. That is only detected on
    /// ELF targets, as the check compares each hook with the default it is an alias of.
    [[nodiscard]] bool start_source() noexcept;
}
