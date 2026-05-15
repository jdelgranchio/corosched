// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <atomic>

#include <corosched/core/critical.hpp>

#if defined(__ARM_ARCH_6M__)
    /// Defined on ARMv6-M (Cortex-M0, M0+, M1), which has no exclusive load/store,
    /// so a 32-bit @c std::atomic read-modify-write is not lock-free and GCC turns it into a libatomic call.
    /// The runqueue word then masks interrupts around its read-modify-write instead.
    #define COROSCHED_RUNQUEUE_MASKED 1
#endif

namespace corosched {
#ifdef COROSCHED_RUNQUEUE_MASKED

    // The part of the std::atomic<std::uint32_t> interface that the scheduler uses, with each read-modify-write inside a critical_section.
    // The memory orders are accepted and ignored, as the critical section is a compiler barrier and that is enough ordering on one core.
    class runqueue_word {
        volatile std::uint32_t value_{};

    public:
        [[nodiscard]] std::uint32_t load(std::memory_order = std::memory_order_seq_cst) const noexcept { return value_; }

        operator std::uint32_t() const noexcept { return load(); }

        std::uint32_t fetch_or(const std::uint32_t mask, std::memory_order = std::memory_order_seq_cst) noexcept {
            critical_section _;
            const std::uint32_t old = value_;
            value_ = old | mask;
            return old;
        }

        std::uint32_t fetch_and(const std::uint32_t mask, std::memory_order = std::memory_order_seq_cst) noexcept {
            critical_section _;
            const std::uint32_t old = value_;
            value_ = old & mask;
            return old;
        }
    };

#else

    using runqueue_word = std::atomic<std::uint32_t>;
    static_assert(runqueue_word::is_always_lock_free, "runqueue word is not lock_free, define COROSCHED_RUNQUEUE_MASKED");

#endif
}
