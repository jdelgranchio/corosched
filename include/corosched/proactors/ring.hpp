// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <corosched/core/critical.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stataider/stataider.hpp>
#include <type_traits>

#include <corosched/core/proactor.hpp>

namespace corosched {
    /// Queues up to @c N events in order, so a task sees every one. Use it for signals where each occurrence counts, such as button presses; use
    /// @c slot for state, where a queue only fills with stale values.
    ///
    /// Any interrupt or task may publish, and events queue in the order they were published. One task awaits; a second awaiting task traps with
    /// @c fault::second_consumer. @c T is copied with interrupts masked, so a large one adds to interrupt latency. When the ring is full the new
    /// event is dropped and counted on the newest queued one, in its @c event_type::dropped.
    template<class T, std::size_t N>
    class ring : public proactor_base {
        static_assert(N >= 2, "ring needs at least 2 slots");
        // The indices run free and are reduced with a mask, as a division is a library call on a core with no hardware divider (Cortex-M0).
        static_assert((N & (N - 1u)) == 0, "ring size must be a power of two");
        static_assert(std::is_trivially_copyable_v<T>, "ring payload must be trivially copyable");

    public:
        /// One event taken from the ring.
        struct event_type {
            T data;
            /// Publishes dropped right after this event because the ring was full. Their data is lost; 0 means none were.
            std::uint32_t dropped;
        };

        using result_type = std::optional<event_type>;

        /// Counters for a debugger to read, in the @c stats_ member.
        struct stats_type {
            std::uint32_t writes; ///< events queued
            std::uint32_t reads; ///< events taken
            std::uint32_t overflows; ///< publishes dropped because the ring was full
            std::uint32_t worst_occupancy; ///< most events queued at once
        };

        /// Queue @c v and wake the awaiting task. If the ring is full, @c v is dropped and the @c dropped of the newest queued event goes up by one.
        void publish(const T &v) noexcept {
            critical_section cs;
            if (head_ - tail_ < N) {
                buf_[head_ & mask] = {v, 0};
                ++head_;
                stataider::inc(stats_.writes);
                stataider::max(stats_.worst_occupancy, static_cast<std::uint32_t>(head_ - tail_));
            } else {
                // Full: the newest queued event is the one just before head_.
                stataider::inc(buf_[(head_ - 1u) & mask].dropped);
                stataider::inc(stats_.overflows);
            }
            wake();
        }

        /// True if at least one event is queued.
        [[nodiscard]] bool ready() const noexcept {
            critical_section cs;
            return head_ != tail_;
        }

        /// Take the oldest event, or @c std::nullopt if the ring is empty.
        result_type produce() noexcept {
            critical_section cs;
            if (head_ == tail_) return std::nullopt;
            const result_type out = buf_[tail_ & mask];
            ++tail_;
            stataider::inc(stats_.reads);
            return out;
        }

    private:
        static constexpr std::size_t mask = N - 1u;

        event_type buf_[N]{};
        std::size_t head_{0}; // events pushed so far; head_ & mask is the next write
        std::size_t tail_{0}; // events popped so far; tail_ & mask is the next read
        stats_type stats_{};
    };

    static_assert(proactor<ring<int, 2>>);
}
