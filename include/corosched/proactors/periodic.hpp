// SPDX-License-Identifier: MPL-2.0
/* This Source Code Form is subject to the terms of the Mozilla Public License, v. 2.0.
 * If a copy of the MPL was not distributed with this file, You can obtain one at https://mozilla.org/MPL/2.0/. */

#pragma once

#include <corosched/core/critical.hpp>
#include <corosched/core/proactor.hpp>

#include <corosched/proactors/time.hpp>

#include <stataider/stataider.hpp>

namespace corosched {
    /// Fires every @c Period, counted in ticks of @c Clock, with no drift. @c co_await on @c next() gives a @c result_type saying how late it is.
    ///
    /// It is attached to @c clock from construction to destruction, so the clock must outlive the @c periodic, and the @c periodic cannot be
    /// copied or moved. @c Period must be a positive exact multiple of the clock's tick, checked at compile time. One interrupt context ticks the
    /// clock and one task awaits; a second awaiting task traps with @c fault::second_consumer.
    ///
    /// A fire that has not been taken when the next period ends is not queued: the two merge into one,
    /// and @c result_type::delay counts the ticks it has been waiting.
    /// The @c stats_ member (for a debugger) holds the sum, worst and number of the delays reported.
    template<periodic_clock Clock, nanos Period>
    class periodic : public proactor_base {
        static constexpr tick_t period_ticks = to_ticks<typename Clock::time_base, Period>();

    public:
        struct result_type {
            /// Ticks between the period boundary and the moment the fire was taken, 0 if on time. Counted modulo 2^32 ticks.
            tick_t delay;
        };

        explicit periodic(Clock &clock) noexcept : hook_(clock, this, member_tick<&periodic::on_due>, period_ticks) {}

        /// True if a fire is waiting to be taken.
        [[nodiscard]] bool ready() const noexcept {
            critical_section cs;
            return pending_;
        }

        /// Take the waiting fire and report how late it is. Adds the delay to @c stats_.
        result_type produce() noexcept {
            critical_section cs;
            const tick_t delay = pending_ ? hook_.now() - fired_at_ : 0;
            pending_ = false;
            stats_.absorb(delay);
            return {delay};
        }

    private:
        // Called by the clock from interrupt context at every period boundary. The clock reloads the countdown with the period it returns, on the
        // tick it came due, so the grid does not drift however late the task is. A fire still waiting absorbs the boundary and keeps its time.
        tick_t on_due() noexcept {
            if (!pending_) {
                pending_ = true;
                fired_at_ = hook_.now();
            }
            wake();
            return period_ticks;
        }

        tick_t fired_at_{0}; // clock tick of the waiting fire
        bool pending_{false};
        stataider::counter_ticks_max stats_{};
        clock_hook<Clock> hook_; // last: see clock_hook
    };
}
