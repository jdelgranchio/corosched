// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#if defined(__ARM_ARCH_PROFILE) && (__ARM_ARCH_PROFILE == 'M')
    /// Defined when building for a Cortex-M.
    #define COROSCHED_CORTEX_M 1
#endif

#ifdef COROSCHED_CORTEX_M
    #include <cstdint>
#else
    #include <mutex>
#endif

namespace corosched {
#ifdef COROSCHED_CORTEX_M

    /// Masks interrupts from construction to destruction.
    /// On exit it restores the PRIMASK it found, so it composes with manual changes and can be nested.
    class critical_section {
        std::uint32_t saved_primask_;

        [[nodiscard]] static std::uint32_t get_primask() noexcept {
            std::uint32_t primask;
            __asm volatile("mrs %0, primask" : "=r"(primask)::"memory");
            return primask;
        }

        /// Write PRIMASK.
        static void set_primask(const std::uint32_t primask) noexcept { __asm volatile("msr primask, %0" ::"r"(primask) : "memory"); }

        /// Mask interrupts.
        static void disable_irq() noexcept { __asm volatile("cpsid i" ::: "memory"); }

    public:
        critical_section() noexcept : saved_primask_(get_primask()) { disable_irq(); }
        ~critical_section() noexcept { set_primask(saved_primask_); }
        critical_section(const critical_section &) = delete;
        critical_section &operator=(const critical_section &) = delete;
    };

#else

    /// Takes a recursive lock from construction to destruction, standing in for masked interrupts.
    class critical_section {
        inline static std::recursive_mutex m;

    public:
        critical_section() noexcept { m.lock(); }
        ~critical_section() noexcept { m.unlock(); }
        critical_section(const critical_section &) = delete;
        critical_section &operator=(const critical_section &) = delete;
    };

#endif

}
