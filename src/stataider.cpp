// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#include <stataider/monotonic.hpp>

#include <cstdint>

#ifndef STATAIDER_CORTEX_M
    #include <chrono>
#endif

namespace stataider::detail {
    constinit tick_t mask = 0;
}

#ifdef STATAIDER_HAS_DEFAULT_TICKS

// The default hooks.
// They have C names so that the weak hooks below can be aliases of them, and start_source can tell from their addresses whether a hook was replaced.
extern "C" {
    #ifdef STATAIDER_HAS_CYCLE_COUNTER

namespace {
    volatile std::uint32_t &reg(const std::uintptr_t address) noexcept { return *reinterpret_cast<volatile std::uint32_t *>(address); }

    constexpr std::uintptr_t demcr = 0xE000'EDFC;
    constexpr std::uintptr_t dwt_ctrl = 0xE000'1000;
    constexpr std::uintptr_t dwt_count = 0xE000'1004;
    constexpr std::uintptr_t dwt_lock = 0xE000'1FB0;

    constexpr std::uint32_t trcena = 1u << 24;
    constexpr std::uint32_t nocyccnt = 1u << 25;
    constexpr std::uint32_t cyccntena = 1u << 0;
    constexpr std::uint32_t unlock = 0xC5AC'CE55;
}

// The DWT cycle counter: 32 bits, which wraps after about 24 s at 180 MHz. A single timed scope longer than one wrap is undercounted.
static stataider::tick_t stataider_default_ticks() noexcept { return reg(dwt_count); }

static unsigned stataider_default_start_ticks() noexcept {
    reg(demcr) = reg(demcr) | trcena;
    if ((reg(dwt_ctrl) & nocyccnt) != 0) {
        return 0; // this chip left the counter out
    }
    reg(dwt_ctrl) = reg(dwt_ctrl) | cyccntena;
    if ((reg(dwt_ctrl) & cyccntena) == 0) {
        // On a Cortex-M7 the DWT can be locked.
        reg(dwt_lock) = unlock;
        reg(dwt_ctrl) = reg(dwt_ctrl) | cyccntena;
    }
    return (reg(dwt_ctrl) & cyccntena) != 0 ? 32 : 0;
}

    #else

// Nanoseconds since the steady-clock epoch.
static stataider::tick_t stataider_default_ticks() noexcept {
    using clock = std::chrono::steady_clock;
    static_assert(clock::is_steady, "monotonic tick source requires a non-decreasing clock");
    // steady_clock's period is implementation-defined, so convert to nanoseconds.
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch());
    return static_cast<stataider::tick_t>(ns.count());
}

static unsigned stataider_default_start_ticks() noexcept { return 64; }

    #endif
}

    #ifdef __ELF__

[[gnu::weak, gnu::alias("stataider_default_ticks")]] stataider::tick_t stataider_ticks() noexcept;
[[gnu::weak, gnu::alias("stataider_default_start_ticks")]] unsigned stataider_start_ticks() noexcept;

    #else

// Mach-O has no aliases, so a hook replaced alone goes undetected on a macOS host.
[[gnu::weak]] stataider::tick_t stataider_ticks() noexcept { return stataider_default_ticks(); }
[[gnu::weak]] unsigned stataider_start_ticks() noexcept { return stataider_default_start_ticks(); }

    #endif

#endif

namespace stataider {
    namespace {
        // Hides where a function pointer came from, so that comparing it with another is done on the addresses the linker chose: the compiler
        // knows which function each alias names here, and could otherwise fold the comparison.
        template<class F>
        F *opaque(F *f) noexcept {
            __asm("" : "+r"(f));
            return f;
        }
    }

    bool start_source() noexcept {
#if defined(STATAIDER_HAS_DEFAULT_TICKS) && defined(__ELF__)
        const bool own_ticks = opaque(&stataider_ticks) != &stataider_default_ticks;
        const bool own_start = opaque(&stataider_start_ticks) != &stataider_default_start_ticks;
        if (own_ticks != own_start) {
            detail::mask = 0;
            return false;
        }
#endif
        const unsigned bits = stataider_start_ticks();
        detail::mask = bits >= 64 ? ~tick_t{0} : (tick_t{1} << bits) - 1;
        return true;
    }
}
