// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <corosched/core/critical.hpp>

#include <cstdint>
#include <stataider/stataider.hpp>
#include <type_traits>

#include <corosched/core/proactor.hpp>

namespace corosched {
    /// Holds the latest value, so a slow task misses values instead of falling behind.
    /// Use @c ring when every event counts.
    ///
    /// @c T is copied with interrupts masked, so a large one adds to interrupt latency.
    template<class T>
    class slot : public proactor_base {
        static_assert(std::is_trivially_copyable_v<T>, "slot payload must be trivially copyable, it is copied under a critical section");

    public:
        /// What a read gives: the newest value, and how many publishes it stands for.
        struct result_type {
            T data;
            /// Publishes since the previous read. 1 is on time, above 1 means the ones before the last were overwritten,
            /// and 0 means nothing new, so @c data is the previous value again.
            std::uint32_t writes;
        };

        /// Counters for a debugger to read, in the @c stats_ member.
        struct stats_type {
            std::uint32_t writes; ///< values published
            std::uint32_t reads; ///< values taken
            std::uint32_t coalesced; ///< publishes that replaced an unread value
            std::uint32_t worst_writes; ///< most publishes folded into one read
        };

        /// Store @c v, replacing any unread value, and wake the awaiting task.
        void publish(const T &v) noexcept {
            critical_section cs;
            stataider::inc(payload_.writes);
            stataider::inc(stats_.writes);
            if (payload_.writes > 1) stataider::inc(stats_.coalesced);
            payload_.data = v;
            wake();
        }

        /// True if a value is waiting to be read.
        [[nodiscard]] bool ready() const noexcept {
            critical_section cs;
            return payload_.writes > 0;
        }

        /// Read the value without waiting. With nothing new, @c writes is 0 and @c data is the previous value.
        result_type produce() noexcept {
            critical_section cs;
            const auto out = payload_;
            stataider::inc(stats_.reads);
            stataider::max(stats_.worst_writes, out.writes);
            payload_.writes = 0;
            return out;
        }

    private:
        result_type payload_{};
        stats_type stats_{};
    };

    static_assert(proactor<slot<int>>);
}
